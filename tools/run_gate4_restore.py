#!/usr/bin/env python3
"""Restore a Gate 3 synthetic dump into fresh MySQL and restart auth/world."""

from __future__ import annotations

import argparse
import datetime as dt
import importlib.util
import json
import os
import secrets
import subprocess
import time
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]

spec = importlib.util.spec_from_file_location("gate3_synthetic", ROOT / "tools/run_gate3_synthetic.py")
assert spec and spec.loader
gate3 = importlib.util.module_from_spec(spec)
spec.loader.exec_module(gate3)


def restore_dump(container: str, source_dump: Path, log: Path) -> None:
    command = gate3.gate2.db_command(container, "mysql", "--binary-mode=1", "--default-character-set=utf8mb4", interactive=True)
    with source_dump.open("rb") as source, log.open("wb") as output:
        process = subprocess.run(command, stdin=source, stdout=output, stderr=subprocess.STDOUT)
    if process.returncode:
        raise RuntimeError(f"Synthetic logical restore failed ({process.returncode}); see private restore.log")


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--core-source", type=Path, required=True, help="Pinned reconstructed AzerothCore source checkout")
    parser.add_argument("--runtime-image", required=True, help="Same local runtime image used in the Gate 3 source run")
    parser.add_argument("--client-assets-volume", required=True, help="Same owner-provided client asset volume used in Gate 3")
    parser.add_argument("--test-subnet", required=True, help="Unused private IPv4 subnet for the temporary internal network")
    parser.add_argument("--source-status", type=Path, required=True, help="Owner-private Gate 3 PASS status.json")
    parser.add_argument("--source-dump", type=Path, required=True, help="Owner-private synthetic-handoff.sql named by the source status")
    parser.add_argument("--evidence-dir", type=Path, required=True, help="New owner-private directory outside the kit")
    parser.add_argument("--allow-stopped-asset-ref", action="append", default=[], help="Exact stopped prior synthetic world container allowed to reference the asset volume")
    args = parser.parse_args()
    os.umask(0o077)
    core = args.core_source.resolve()
    if not (core / "data/sql/base").is_dir():
        parser.error("--core-source lacks data/sql/base")
    source_status_path = args.source_status.resolve()
    source_dump_path = args.source_dump.resolve()
    if ROOT in source_status_path.parents or ROOT in source_dump_path.parents:
        parser.error("--source-status and --source-dump must be outside the public repository")
    evidence = args.evidence_dir.resolve()
    if evidence == ROOT or ROOT in evidence.parents:
        parser.error("--evidence-dir must be outside the public repository")
    stamp = dt.datetime.now(dt.timezone.utc).strftime("%Y%m%d-%H%M%SZ")
    evidence.mkdir(mode=0o700, parents=True, exist_ok=False)
    prefix = f"wow-gate4-restore-{stamp.lower()}"
    network, volume = prefix + "-net", prefix + "-mysql"
    db, auth, world = prefix + "-db", prefix + "-auth", prefix + "-world"
    created: list[str] = []
    root_password = app_password = ""
    status: dict[str, object] = {
        "started_utc": gate3.now(), "result": "IN_PROGRESS", "test": "synthetic-logical-restore-and-server-restart",
        "source_status_sha256": gate3.gate2.sha256(source_status_path),
        "new_network": network, "new_subnet": args.test_subnet, "network_internal": True,
        "new_volume": volume, "containers": [db, auth, world], "published_ports": [],
        "asset_volume": args.client_assets_volume, "asset_volume_mode": "readonly",
    }
    try:
        source_status = json.loads(source_status_path.read_text(encoding="utf-8"))
        if source_status.get("result") != "PASS" or source_status.get("handoff", {}).get("synthetic_only") is not True:
            raise RuntimeError("Source Gate 3 status is not a synthetic PASS")
        expected_hash = source_status["handoff"]["sha256"]
        expected_size = source_status["handoff"]["bytes"]
        if source_dump_path.name != source_status["handoff"]["file"]:
            raise RuntimeError("Source dump filename differs from Gate 3 status")
        if source_dump_path.stat().st_size != expected_size or gate3.gate2.sha256(source_dump_path) != expected_hash:
            raise RuntimeError("Source synthetic dump size or SHA-256 mismatch")
        status["verified_dump_bytes"] = expected_size
        status["verified_dump_sha256"] = expected_hash
        status["mysql_image_id"] = gate3.docker("image", "inspect", gate3.MYSQL_IMAGE, "--format", "{{.Id}}")
        status["runtime_image_id"] = gate3.docker("image", "inspect", args.runtime_image, "--format", "{{.Id}}")
        if status["mysql_image_id"] != source_status["mysql_image_id"] or status["runtime_image_id"] != source_status["runtime_image_id"]:
            raise RuntimeError("Pinned local image IDs differ from successful Gate 3 run")
        status["existing_network_count"] = gate3.check_subnet(args.test_subnet)
        refs = set(gate3.docker("ps", "-a", "--filter", f"volume={args.client_assets_volume}", "--format", "{{.Names}}").splitlines())
        allowed = set(args.allow_stopped_asset_ref)
        if refs - allowed:
            raise RuntimeError("Client asset volume has an unexpected container reference")
        for name in refs:
            state = gate3.docker("inspect", "--format", "{{.State.Running}}|{{index .Config.Labels \"io.outlandswow.gate\"}}", name)
            if state != "false|synthetic-smoke":
                raise RuntimeError("Prior synthetic asset-volume reference is not stopped and labeled")
        status["preserved_prior_asset_volume_refs"] = sorted(refs)

        root_password = secrets.token_hex(24)
        app_password = secrets.token_hex(24)
        secret_dir = evidence / "secrets"
        secret_dir.mkdir(mode=0o700)
        root_file, app_file = secret_dir / "db_root_password", secret_dir / "db_app_password"
        root_file.write_text(root_password + "\n", encoding="utf-8")
        app_file.write_text(app_password + "\n", encoding="utf-8")
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
        gate3.configure(modules / "playerbots.conf", modules / "playerbots.conf", {
            "PlayerbotsDatabaseInfo": f'"database;3306;gate4;{app_password};acore_playerbots"',
            "Playerbots.Updates.EnableDatabases": "0",
        })
        connection = lambda name: f'"database;3306;gate4;{app_password};{name}"'
        gate3.configure(core / "src/server/apps/authserver/authserver.conf.dist", conf / "authserver.conf", {
            "LoginDatabaseInfo": connection("acore_auth"),
            "RealmServerPort": "3724", "LogsDir": '"/smoke/logs"', "Updates.EnableDatabases": "0",
        })
        gate3.configure(core / "src/server/apps/worldserver/worldserver.conf.dist", conf / "worldserver.conf", {
            "LoginDatabaseInfo": connection("acore_auth"), "WorldDatabaseInfo": connection("acore_world"),
            "CharacterDatabaseInfo": connection("acore_characters"),
            "WorldServerPort": "8085", "DataDir": '"/azerothcore/data"', "LogsDir": '"/smoke/logs"',
            "Updates.EnableDatabases": "0", "Console.Enable": "0",
        })
        status["ai_profile"] = "disabled"
        status["prepared_config_count"] = len(list(modules.glob("*.conf"))) + 2

        gate3.docker("network", "create", "--internal", "--subnet", args.test_subnet,
                     "--label", "io.outlandswow.gate=synthetic-restore", network)
        created.append("network")
        if gate3.docker("network", "inspect", network, "--format", "{{.Internal}}") != "true":
            raise RuntimeError("New Docker network is not internal")
        gate3.docker("volume", "create", "--label", "io.outlandswow.gate=synthetic-restore", volume)
        created.append("volume")
        gate3.docker("run", "-d", "--name", db, "--network", network, "--network-alias", "database",
                     "--restart", "no", "--cpus", "2", "--memory", "4g", "--label", "io.outlandswow.gate=synthetic-restore",
                     "--mount", f"type=volume,source={volume},target=/var/lib/mysql",
                     "--mount", f"type=bind,source={root_file},target=/run/secrets/db_root_password,readonly",
                     "--env", "MYSQL_ROOT_PASSWORD_FILE=/run/secrets/db_root_password", gate3.MYSQL_IMAGE)
        created.append("db")
        gate3.wait_mysql(db)
        restore_dump(db, source_dump_path, evidence / "restore.log")
        status["restored_metrics"] = gate3.metrics(db)
        expected_metrics = source_status["synthetic_metrics"]
        if status["restored_metrics"] != expected_metrics:
            raise RuntimeError("Restored database metrics differ from Gate 3 synthetic baseline")
        if gate3.gate2.query(db, "SELECT username FROM acore_auth.account WHERE id=1") != "GATE3_SYNTH":
            raise RuntimeError("Synthetic account identity did not restore")
        if gate3.gate2.query(db, "SELECT name FROM acore_characters.characters WHERE guid=1") != "GateThree":
            raise RuntimeError("Synthetic character identity did not restore")
        grants = [f"GRANT ALL PRIVILEGES ON `{schema}`.* TO 'gate4'@'%';" for schema in gate3.SCHEMAS]
        gate3.sql_input(db, "CREATE USER 'gate4'@'%' IDENTIFIED BY '" + app_password + "';\n" + "\n".join(grants) + "\n")

        common_mounts = ["--mount", f"type=bind,source={modules},target=/azerothcore/dist/etc/modules,readonly",
                         "--mount", f"type=bind,source={logs},target=/smoke/logs"]
        gate3.docker("run", "-d", "--name", auth, "--network", network, "--restart", "no", "--cpus", "0.5", "--memory", "512m",
                     "--label", "io.outlandswow.gate=synthetic-restore", *common_mounts,
                     "--mount", f"type=bind,source={conf/'authserver.conf'},target=/smoke/authserver.conf,readonly",
                     args.runtime_image, "/azerothcore/dist/bin/authserver", "--config", "/smoke/authserver.conf")
        created.append("auth")
        gate3.wait_server(auth, "Added realm", 180)
        gate3.docker("run", "-d", "--name", world, "--network", network, "--restart", "no", "--cpus", "4", "--memory", "8g",
                     "--label", "io.outlandswow.gate=synthetic-restore", *common_mounts,
                     "--mount", f"type=bind,source={conf/'worldserver.conf'},target=/smoke/worldserver.conf,readonly",
                     "--mount", f"type=volume,source={args.client_assets_volume},target=/azerothcore/data,readonly",
                     args.runtime_image, "/azerothcore/dist/bin/worldserver", "--config", "/smoke/worldserver.conf")
        created.append("world")
        gate3.wait_server(world, "WORLD: World Initialized In", 300)
        time.sleep(20)
        for name in (db, auth, world):
            if gate3.docker("inspect", "--format", "{{.State.Running}}", name) != "true":
                raise RuntimeError(f"{name} exited during restore stability interval")
            bindings = gate3.docker("inspect", "--format", "{{json .HostConfig.PortBindings}}", name)
            if bindings not in ("{}", "null"):
                raise RuntimeError(f"{name} unexpectedly published a host port")
        status["running_metrics"] = gate3.metrics(db)
        if status["running_metrics"] != expected_metrics:
            raise RuntimeError("Restored database metrics changed during server startup")
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
                gate3.docker("stop", "--timeout", "60", name, check=False)
                if gate3.capture_logs(name, evidence, [root_password, app_password]):
                    status["result"] = "FAIL"
                    status["error"] = "Generated secret appeared in container logs"
                status.setdefault("stopped_container_states", {})[kind] = gate3.docker(
                    "inspect", "--format", "{{.State.Running}}|{{.State.ExitCode}}|{{.State.OOMKilled}}", name, check=False)
        if status["result"] == "PASS":
            try:
                for name in (world, auth, db):
                    gate3.docker("rm", name)
                gate3.docker("volume", "rm", volume)
                gate3.docker("network", "rm", network)
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
        status["finished_utc"] = gate3.now()
        (evidence / "status.json").write_text(json.dumps(status, indent=2, sort_keys=True) + "\n", encoding="utf-8")
        print(json.dumps(status, indent=2, sort_keys=True))
        print(f"Evidence: {evidence}")
    return 0 if status["result"] == "PASS" else 1


if __name__ == "__main__":
    raise SystemExit(main())
