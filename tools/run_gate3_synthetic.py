#!/usr/bin/env python3
"""Run an isolated synthetic auth/world startup using a pinned reconstructed core."""

from __future__ import annotations

import argparse
import datetime as dt
import hashlib
import importlib.util
import ipaddress
import json
import os
import re
import secrets
import subprocess
import time
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
GATE2_PATH = ROOT / "tools/run_gate2_synthetic.py"
MYSQL_IMAGE = "mysql:8.4"
CORE_PIN = "ceeb3116ebedf4b35f12b75e5481b6ddd0de7a89"
SCHEMAS = ("acore_auth", "acore_characters", "acore_world", "acore_playerbots")

spec = importlib.util.spec_from_file_location("gate2_synthetic", GATE2_PATH)
assert spec and spec.loader
gate2 = importlib.util.module_from_spec(spec)
spec.loader.exec_module(gate2)


def now() -> str:
    return dt.datetime.now(dt.timezone.utc).isoformat()


def docker(*args: str, check: bool = True) -> str:
    return gate2.run(["docker", *args], check=check)


def sql_input(container: str, sql: str) -> None:
    command = gate2.db_command(container, "mysql", "--binary-mode=1", interactive=True)
    proc = subprocess.run(command, input=sql.encode("utf-8"), stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
    if proc.returncode:
        raise RuntimeError("MySQL statement failed: " + proc.stdout.decode("utf-8", errors="replace")[-900:])


def configure(source: Path, target: Path, changes: dict[str, str]) -> None:
    lines = source.read_text(encoding="utf-8").splitlines()
    counts = {key: 0 for key in changes}
    result: list[str] = []
    for line in lines:
        replaced = False
        for key, value in changes.items():
            if re.match(r"^\s*" + re.escape(key) + r"\s*=", line):
                result.append(f"{key} = {value}")
                counts[key] += 1
                replaced = True
                break
        if not replaced:
            result.append(line)
    if any(count != 1 for count in counts.values()):
        raise RuntimeError(f"Configuration keys not unique: {counts}")
    target.write_text("\n".join(result) + "\n", encoding="utf-8")
    target.chmod(0o600)


def metrics(db: str) -> dict[str, int]:
    statements = {
        "auth_tables": "SELECT COUNT(*) FROM information_schema.tables WHERE table_schema='acore_auth'",
        "characters_tables": "SELECT COUNT(*) FROM information_schema.tables WHERE table_schema='acore_characters'",
        "world_tables": "SELECT COUNT(*) FROM information_schema.tables WHERE table_schema='acore_world'",
        "playerbots_tables": "SELECT COUNT(*) FROM information_schema.tables WHERE table_schema='acore_playerbots'",
        "accounts": "SELECT COUNT(*) FROM acore_auth.account",
        "characters": "SELECT COUNT(*) FROM acore_characters.characters",
        "online_characters": "SELECT COUNT(*) FROM acore_characters.characters WHERE online<>0",
    }
    return {key: int(gate2.query(db, query)) for key, query in statements.items()}


def missing_core_updates(core: Path, base: Path, schema_dir: str) -> list[Path]:
    recorded = (base / schema_dir / "updates.sql").read_text(encoding="utf-8", errors="replace")
    known = set(re.findall(r"^\('([^']+\.sql)'", recorded, re.MULTILINE))
    return [path for path in sorted((core / "data/sql/updates" / schema_dir).glob("*.sql")) if path.name not in known]


def check_subnet(subnet: str) -> int:
    chosen = ipaddress.ip_network(subnet)
    if chosen.version != 4 or chosen.num_addresses < 8 or chosen.num_addresses > 256 or not chosen.is_private:
        raise RuntimeError("Choose an unused private IPv4 subnet between /24 and /29")
    ids = docker("network", "ls", "-q").splitlines()
    networks = json.loads(docker("network", "inspect", *ids))
    used = []
    for network in networks:
        for config in (network.get("IPAM") or {}).get("Config") or []:
            if config.get("Subnet"):
                used.append(ipaddress.ip_network(config["Subnet"], strict=False))
    routes = json.loads(gate2.run(["ip", "-j", "route", "show", "table", "all"]))
    for route in routes:
        destination = route.get("dst")
        if destination and destination != "default":
            try:
                used.append(ipaddress.ip_network(destination, strict=False))
            except ValueError:
                pass
    overlaps = sum(chosen.overlaps(network) for network in used)
    if overlaps:
        raise RuntimeError(f"Requested subnet overlaps {overlaps} Docker or host route networks")
    return len(networks)


def wait_mysql(name: str) -> None:
    for _ in range(90):
        logs = docker("logs", name, check=False)
        if "MySQL init process done. Ready for start up." in logs:
            command = gate2.db_command(name, "mysql", "--batch", "--skip-column-names", "--execute", "SELECT 1")
            if gate2.run(command, check=False) == "1":
                return
        time.sleep(2)
    raise RuntimeError("MySQL did not become ready")


def wait_server(name: str, marker: str, timeout: int) -> str:
    deadline = time.monotonic() + timeout
    while time.monotonic() < deadline:
        state = docker("inspect", "--format", "{{.State.Running}}|{{.State.ExitCode}}", name)
        logs = docker("logs", name, check=False)
        if marker in logs:
            if not state.startswith("true|"):
                raise RuntimeError(f"{name} emitted readiness marker but exited: {state}")
            return logs
        if not state.startswith("true|"):
            raise RuntimeError(f"{name} exited before readiness: {state}")
        time.sleep(3)
    raise RuntimeError(f"{name} did not reach readiness within {timeout}s")


def capture_logs(name: str, evidence: Path, secrets_to_redact: list[str]) -> bool:
    logs = docker("logs", name, check=False)
    leaked = any(secret in logs for secret in secrets_to_redact if secret)
    for secret in secrets_to_redact:
        if secret:
            logs = logs.replace(secret, "[REDACTED]")
    (evidence / f"{name}.log").write_text(logs + "\n", encoding="utf-8")
    return leaked


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--core-source", type=Path, required=True, help="Pinned reconstructed AzerothCore source checkout")
    parser.add_argument("--runtime-image", required=True, help="Locally built image from this source, with authserver and worldserver")
    parser.add_argument("--client-assets-volume", required=True, help="Owner-provided Docker volume containing dbc/maps/vmaps/mmaps/Cameras")
    parser.add_argument("--test-subnet", required=True, help="Unused private IPv4 subnet for the temporary internal network")
    parser.add_argument("--evidence-dir", type=Path, required=True, help="New owner-private directory outside the kit")
    parser.add_argument("--allow-stopped-asset-ref", action="append", default=[], help="Exact stopped prior synthetic world container allowed to reference the asset volume")
    args = parser.parse_args()
    os.umask(0o077)
    core = args.core_source.resolve()
    if not (core / "data/sql/base").is_dir():
        parser.error("--core-source lacks data/sql/base")
    evidence = args.evidence_dir.resolve()
    if evidence == ROOT or ROOT in evidence.parents:
        parser.error("--evidence-dir must be outside this public repository")
    stamp = dt.datetime.now(dt.timezone.utc).strftime("%Y%m%d-%H%M%SZ")
    evidence.mkdir(mode=0o700, parents=True, exist_ok=False)
    prefix = f"wow-gate3-synthetic-{stamp.lower()}"
    network, volume = prefix + "-net", prefix + "-mysql"
    db, auth, world = prefix + "-db", prefix + "-auth", prefix + "-world"
    created: list[str] = []
    status: dict[str, object] = {
        "started_utc": now(), "result": "IN_PROGRESS", "input_class": "public-sql-synthetic-records-readonly-client-assets",
        "asset_volume": args.client_assets_volume, "asset_volume_mode": "readonly", "network_internal": True,
        "published_ports": [], "new_network": network, "new_subnet": args.test_subnet, "new_volume": volume, "containers": [db, auth, world],
    }
    root_password = app_password = ""
    try:
        status["core_commit"] = gate2.run(["git", "-C", str(core), "rev-parse", "HEAD"])
        if status["core_commit"] != CORE_PIN:
            raise RuntimeError("Core source commit differs from UPSTREAM-PINS.md")
        status["mysql_image_id"] = docker("image", "inspect", MYSQL_IMAGE, "--format", "{{.Id}}")
        status["runtime_image_id"] = docker("image", "inspect", args.runtime_image, "--format", "{{.Id}}")
        status["existing_network_count"] = check_subnet(args.test_subnet)
        refs = set(docker("ps", "-a", "--filter", f"volume={args.client_assets_volume}", "--format", "{{.Names}}").splitlines())
        allowed_prior_refs = set(args.allow_stopped_asset_ref)
        if refs - allowed_prior_refs:
            raise RuntimeError("Client asset volume has an unexpected container reference")
        for prior in refs:
            state = docker("inspect", "--format", "{{.State.Running}}|{{index .Config.Labels \"io.outlandswow.gate\"}}", prior)
            if state != "false|synthetic-smoke":
                raise RuntimeError("Prior synthetic asset-volume reference is not stopped and labeled")
        status["preserved_prior_asset_volume_refs"] = sorted(refs)
        base = core / "data/sql/base"
        base_files = {"acore_auth": sorted((base / "db_auth").glob("*.sql")), "acore_characters": sorted((base / "db_characters").glob("*.sql")), "acore_world": sorted((base / "db_world").glob("*.sql"))}
        if [len(base_files[name]) for name in base_files] != [22, 108, 309]:
            raise RuntimeError("Core base SQL count differs from tested pin")
        core_updates = {
            "acore_auth": missing_core_updates(core, base, "db_auth"),
            "acore_characters": missing_core_updates(core, base, "db_characters"),
            "acore_world": missing_core_updates(core, base, "db_world"),
        }
        if [len(core_updates[name]) for name in core_updates] != [1, 1, 92]:
            raise RuntimeError("Core post-base update count differs from inspected pin")
        status["post_base_update_counts"] = {key: len(value) for key, value in core_updates.items()}
        playerbots = core / "modules/mod-playerbots/data/sql"
        bot_files = {
            "acore_characters": sorted((playerbots / "characters/base").glob("*.sql")),
            "acore_world": sorted((playerbots / "world/base").glob("*.sql")),
            "acore_playerbots": sorted((playerbots / "playerbots/base").glob("*.sql")),
        }
        if [len(bot_files[name]) for name in bot_files] != [3, 3, 29]:
            raise RuntimeError("Playerbots base SQL count differs from inspected pin")
        custom_files = sorted((ROOT / "source/modules").glob("mod-living-azeroth-*/data/sql/db-characters/*.sql"), key=lambda p: p.name)
        if len(custom_files) != 8:
            raise RuntimeError("Expected eight public custom migrations")
        ollama_files = sorted((core / "modules/mod-ollama-chat/data/sql/characters/base").glob("*.sql"))
        if len(ollama_files) != 6:
            raise RuntimeError("Expected six pinned mod-ollama-chat character migrations")
        all_sql = [*sum(base_files.values(), []), *sum(core_updates.values(), []), *sum(bot_files.values(), []), *custom_files, *ollama_files]
        status["sql_file_count"] = len(all_sql)
        status["sql_manifest_sha256"] = hashlib.sha256("\n".join(f"{p.name} {gate2.sha256(p)}" for p in all_sql).encode()).hexdigest()

        root_password = secrets.token_hex(24)
        app_password = secrets.token_hex(24)
        secret_dir = evidence / "secrets"
        secret_dir.mkdir(mode=0o700)
        root_file, app_file = secret_dir / "db_root_password", secret_dir / "db_app_password"
        root_file.write_text(root_password + "\n")
        app_file.write_text(app_password + "\n")
        root_file.chmod(0o600)
        app_file.chmod(0o600)
        conf = evidence / "conf"
        modules = conf / "modules"
        modules.mkdir(parents=True, mode=0o700)
        logs = evidence / "server-logs"
        logs.mkdir(mode=0o700)
        for path in sorted((core / "modules").glob("*/conf/*.conf.dist")):
            target = modules / path.name.removesuffix(".dist")
            target.write_bytes(path.read_bytes())
            target.chmod(0o600)
        renderer = ROOT / "deploy/gate8-ai-profile.sh"
        rendered = subprocess.run(["bash", str(renderer), str(modules), "disabled", "0"], capture_output=True, text=True)
        if rendered.returncode:
            raise RuntimeError("Disabled AI profile rendering failed: " + rendered.stderr[-700:])
        configure(modules / "playerbots.conf", modules / "playerbots.conf", {
            "PlayerbotsDatabaseInfo": f'"database;3306;gate3;{app_password};acore_playerbots"',
            "Playerbots.Updates.EnableDatabases": "0",
        })
        connection = lambda name: f'"database;3306;gate3;{app_password};{name}"'
        configure(core / "src/server/apps/authserver/authserver.conf.dist", conf / "authserver.conf", {
            "LoginDatabaseInfo": connection("acore_auth"),
            "RealmServerPort": "3724", "LogsDir": '"/smoke/logs"', "Updates.EnableDatabases": "0",
        })
        configure(core / "src/server/apps/worldserver/worldserver.conf.dist", conf / "worldserver.conf", {
            "LoginDatabaseInfo": connection("acore_auth"), "WorldDatabaseInfo": connection("acore_world"),
            "CharacterDatabaseInfo": connection("acore_characters"),
            "WorldServerPort": "8085", "DataDir": '"/azerothcore/data"', "LogsDir": '"/smoke/logs"',
            "Updates.EnableDatabases": "0", "Console.Enable": "0",
        })
        status["ai_profile"] = "disabled"
        status["prepared_config_count"] = len(list(modules.glob("*.conf"))) + 2

        docker("network", "create", "--internal", "--subnet", args.test_subnet, "--label", "io.outlandswow.gate=synthetic-smoke", network)
        created.append("network")
        if docker("network", "inspect", network, "--format", "{{.Internal}}") != "true":
            raise RuntimeError("New Docker network is not internal")
        docker("volume", "create", "--label", "io.outlandswow.gate=synthetic-smoke", volume)
        created.append("volume")
        docker("run", "-d", "--name", db, "--network", network, "--network-alias", "database",
               "--restart", "no", "--cpus", "2", "--memory", "4g", "--label", "io.outlandswow.gate=synthetic-smoke",
               "--mount", f"type=volume,source={volume},target=/var/lib/mysql",
               "--mount", f"type=bind,source={root_file},target=/run/secrets/db_root_password,readonly",
               "--env", "MYSQL_ROOT_PASSWORD_FILE=/run/secrets/db_root_password", MYSQL_IMAGE)
        created.append("db")
        wait_mysql(db)
        status["mysql_version"] = gate2.query(db, "SELECT VERSION()")
        for schema in SCHEMAS:
            gate2.query(db, f"CREATE DATABASE `{schema}` CHARACTER SET utf8mb4 COLLATE utf8mb4_unicode_ci")
        for schema, files in base_files.items():
            gate2.import_files(db, schema, files, evidence / "import.log")
        for schema, files in core_updates.items():
            for path in files:
                gate2.import_files(db, schema, [path], evidence / "import.log")
                digest = hashlib.sha1(path.read_bytes()).hexdigest().upper()
                gate2.query(db, f"INSERT INTO `{schema}`.`updates` (`name`,`hash`,`state`,`speed`) VALUES ('{path.name}','{digest}','RELEASED',0)")
        for schema, files in bot_files.items():
            gate2.import_files(db, schema, files, evidence / "import.log")
        for path in custom_files:
            gate2.import_files(db, "acore_characters", [path], evidence / "import.log")
        for path in ollama_files:
            gate2.import_files(db, "acore_characters", [path], evidence / "import.log")
        status["imported_metrics"] = metrics(db)
        grants = [f"GRANT ALL PRIVILEGES ON `{schema}`.* TO 'gate3'@'%';" for schema in SCHEMAS]
        sql_input(db, "CREATE USER 'gate3'@'%' IDENTIFIED BY '" + app_password + "';\n" + "\n".join(grants) + "\n")
        gate2.query(db, "INSERT INTO acore_auth.account(username,salt,verifier,email,reg_mail) VALUES ('GATE3_SYNTH',REPEAT(UNHEX('00'),32),REPEAT(UNHEX('00'),32),'synthetic.invalid','synthetic.invalid')")
        gate2.query(db, "INSERT INTO acore_characters.characters(guid,account,name,race,`class`,gender,level,taximask,innTriggerId) VALUES (1,1,'GateThree',1,1,0,1,'',0)")
        before = metrics(db)
        if not (before["auth_tables"] >= 22 and before["characters_tables"] >= 132 and before["world_tables"] >= 309 and before["playerbots_tables"] >= 29 and before["accounts"] == 1 and before["characters"] == 1 and before["online_characters"] == 0):
            raise RuntimeError("Synthetic database baseline metrics are invalid")
        status["synthetic_metrics"] = before
        dump = evidence / "synthetic-handoff.sql"
        command = gate2.db_command(db, "mysqldump", "--single-transaction", "--quick", "--routines", "--events", "--triggers", "--hex-blob", "--no-tablespaces", "--set-gtid-purged=OFF", "--databases", *SCHEMAS)
        with dump.open("wb") as output:
            export = subprocess.run(command, stdout=output, stderr=subprocess.PIPE)
        if export.returncode:
            raise RuntimeError("Synthetic logical export failed: " + export.stderr.decode(errors="replace")[-900:])
        with dump.open("rb") as source:
            source.seek(max(0, dump.stat().st_size - 131072))
            if b"-- Dump completed on" not in source.read():
                raise RuntimeError("Synthetic logical export completion marker missing")
        status["handoff"] = {"file": dump.name, "bytes": dump.stat().st_size, "sha256": gate2.sha256(dump), "private": True, "synthetic_only": True}

        common_mounts = ["--mount", f"type=bind,source={modules},target=/azerothcore/dist/etc/modules,readonly",
                         "--mount", f"type=bind,source={logs},target=/smoke/logs"]
        docker("run", "-d", "--name", auth, "--network", network, "--restart", "no", "--cpus", "0.5", "--memory", "512m",
               "--label", "io.outlandswow.gate=synthetic-smoke", *common_mounts,
               "--mount", f"type=bind,source={conf/'authserver.conf'},target=/smoke/authserver.conf,readonly",
               args.runtime_image, "/azerothcore/dist/bin/authserver", "--config", "/smoke/authserver.conf")
        created.append("auth")
        wait_server(auth, "Added realm", 180)
        docker("run", "-d", "--name", world, "--network", network, "--restart", "no", "--cpus", "4", "--memory", "8g",
               "--label", "io.outlandswow.gate=synthetic-smoke", *common_mounts,
               "--mount", f"type=bind,source={conf/'worldserver.conf'},target=/smoke/worldserver.conf,readonly",
               "--mount", f"type=volume,source={args.client_assets_volume},target=/azerothcore/data,readonly",
               args.runtime_image, "/azerothcore/dist/bin/worldserver", "--config", "/smoke/worldserver.conf")
        created.append("world")
        wait_server(world, "WORLD: World Initialized In", 300)
        time.sleep(20)
        for name in (db, auth, world):
            if not docker("inspect", "--format", "{{.State.Running}}", name) == "true":
                raise RuntimeError(f"{name} exited during stability interval")
            bindings = docker("inspect", "--format", "{{json .HostConfig.PortBindings}}", name)
            if bindings not in ("{}", "null"):
                raise RuntimeError(f"{name} unexpectedly published a host port")
        after = metrics(db)
        status["running_metrics"] = after
        if after["accounts"] != 1 or after["characters"] != 1 or after["online_characters"] != 0:
            raise RuntimeError("Synthetic account/character state changed during smoke test")
        status["auth_ready_marker"] = "Added realm"
        status["world_ready_marker"] = "WORLD: World Initialized In"
        status["result"] = "PASS"
    except BaseException as exc:
        status["result"] = "FAIL"
        message = str(exc)
        for secret in (root_password, app_password):
            if secret:
                message = message.replace(secret, "[REDACTED]")
        status["error"] = message
    finally:
        for kind, name in (("world", world), ("auth", auth), ("db", db)):
            if kind in created:
                docker("stop", "--timeout", "60", name, check=False)
                if capture_logs(name, evidence, [root_password, app_password]):
                    status["result"] = "FAIL"
                    status["error"] = "Generated secret appeared in container logs"
        if status["result"] == "PASS":
            try:
                for name in (world, auth, db):
                    docker("rm", name)
                docker("volume", "rm", volume)
                docker("network", "rm", network)
                status["new_resources_removed"] = True
                for path in (evidence / "secrets").glob("*"):
                    path.unlink()
                (evidence / "secrets").rmdir()
                for path in (evidence / "conf/modules").glob("*"):
                    path.unlink()
                (evidence / "conf/modules").rmdir()
                for path in (evidence / "conf").glob("*"):
                    path.unlink()
                (evidence / "conf").rmdir()
                status["generated_secrets_removed"] = True
            except (OSError, RuntimeError) as exc:
                status["result"] = "FAIL"
                status["cleanup_error"] = str(exc)
        else:
            status["new_resources_preserved_stopped"] = created
        status["finished_utc"] = now()
        (evidence / "status.json").write_text(json.dumps(status, indent=2, sort_keys=True) + "\n", encoding="utf-8")
        print(json.dumps(status, indent=2, sort_keys=True))
        print(f"Evidence: {evidence}")
    return 0 if status["result"] == "PASS" else 1


if __name__ == "__main__":
    raise SystemExit(main())
