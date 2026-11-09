#!/usr/bin/env python3
"""Rebuild public WoW SQL in networkless MySQL with synthetic rows and restore."""

from __future__ import annotations

import argparse
import datetime as dt
import hashlib
import json
import secrets
import subprocess
import tempfile
import time
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
MODULES = ROOT / "source/modules"
SCHEMAS = {"db_auth": "gate2_auth", "db_characters": "gate2_characters", "db_world": "gate2_world"}
IMAGE = "mysql:8.4"


def now() -> str:
    return dt.datetime.now(dt.timezone.utc).isoformat()


def sha256(path: Path) -> str:
    h = hashlib.sha256()
    with path.open("rb") as file:
        for block in iter(lambda: file.read(4 * 1024 * 1024), b""):
            h.update(block)
    return h.hexdigest()


def run(args: list[str], *, check: bool = True) -> str:
    proc = subprocess.run(args, stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True, errors="replace")
    if check and proc.returncode:
        raise RuntimeError(f"{args[0]} {args[1:3]} failed ({proc.returncode}): {proc.stdout[-2500:]}")
    return proc.stdout.strip()


def db_command(container: str, tool: str, *args: str, interactive: bool = False) -> list[str]:
    prefix = ["docker", "exec"]
    if interactive:
        prefix.append("-i")
    return [*prefix, container, "sh", "-c", 'MYSQL_PWD="$(cat /run/secrets/db_root_password)" exec "$@"', "gate2", tool, "-uroot", *args]


def query(container: str, statement: str) -> str:
    return run(db_command(container, "mysql", "--batch", "--raw", "--skip-column-names", "--execute", statement))


def import_files(container: str, schema: str, paths: list[Path], log: Path) -> None:
    schema_args = [schema] if schema else []
    cmd = db_command(container, "mysql", "--binary-mode=1", "--default-character-set=utf8mb4", *schema_args, interactive=True)
    with log.open("a", encoding="utf-8") as output:
        output.write(f"import {schema}: {len(paths)} public SQL files\n")
        output.flush()
        proc = subprocess.Popen(cmd, stdin=subprocess.PIPE, stdout=output, stderr=subprocess.STDOUT)
        assert proc.stdin is not None
        try:
            for path in paths:
                with path.open("rb") as source:
                    for block in iter(lambda: source.read(1024 * 1024), b""):
                        proc.stdin.write(block)
                proc.stdin.write(b"\n")
            proc.stdin.close()
            code = proc.wait()
        except BaseException:
            proc.stdin.close()
            proc.wait()
            raise
    if code:
        raise RuntimeError(f"SQL import failed for {schema} ({code}); see import.log")


def dump(container: str, destination: Path) -> None:
    cmd = db_command(container, "mysqldump", "--single-transaction", "--quick", "--routines", "--events", "--triggers", "--hex-blob", "--no-tablespaces", "--set-gtid-purged=OFF", "--databases", *SCHEMAS.values())
    with destination.open("wb") as out:
        proc = subprocess.run(cmd, stdout=out, stderr=subprocess.PIPE)
    if proc.returncode:
        raise RuntimeError("Logical export failed: " + proc.stderr.decode("utf-8", errors="replace")[-1500:])
    with destination.open("rb") as file:
        file.seek(max(0, destination.stat().st_size - 131072))
        if b"-- Dump completed on" not in file.read():
            raise RuntimeError("Logical export completion marker missing")


def metrics(container: str) -> dict[str, int]:
    statements = {
        "auth_tables": "SELECT COUNT(*) FROM information_schema.tables WHERE table_schema='gate2_auth'",
        "characters_tables": "SELECT COUNT(*) FROM information_schema.tables WHERE table_schema='gate2_characters'",
        "world_tables": "SELECT COUNT(*) FROM information_schema.tables WHERE table_schema='gate2_world'",
        "custom_tables": "SELECT COUNT(*) FROM information_schema.tables WHERE table_schema='gate2_characters' AND table_name LIKE 'la\\_%'",
        "accounts": "SELECT COUNT(*) FROM gate2_auth.account",
        "characters": "SELECT COUNT(*) FROM gate2_characters.characters",
        "online_characters": "SELECT COUNT(*) FROM gate2_characters.characters WHERE online<>0",
    }
    return {name: int(query(container, sql)) for name, sql in statements.items()}


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--core-source", type=Path, required=True, help="Pinned AzerothCore checkout containing data/sql/base")
    parser.add_argument("--evidence-dir", type=Path, required=True, help="New owner-private directory outside this repository")
    args = parser.parse_args()
    base = args.core_source.resolve() / "data/sql/base"
    if not base.is_dir():
        parser.error("--core-source lacks data/sql/base")
    stamp = dt.datetime.now(dt.timezone.utc).strftime("%Y%m%d-%H%M%SZ")
    evidence = args.evidence_dir.resolve()
    if evidence == ROOT or ROOT in evidence.parents:
        parser.error("--evidence-dir must be outside the public repository")
    evidence.mkdir(mode=0o700)
    status: dict[str, object] = {"started_utc": now(), "result": "IN_PROGRESS", "input_class": "public-base-sql-plus-public-custom-migrations-and-synthetic-rows", "network_mode": "none", "published_ports": [], "image": IMAGE}
    name = f"wow-gate2-synthetic-{stamp.lower()}"
    volume = name + "-mysql"
    status["container"] = name
    status["volume"] = volume
    started = False
    volume_created = False
    secret_text = ""
    try:
        base_files = {key: sorted((base / key).glob("*.sql")) for key in SCHEMAS}
        if [len(base_files[k]) for k in SCHEMAS] != [22, 108, 309]:
            raise RuntimeError("Pinned core base SQL file counts differ from tested source")
        custom_files = sorted(MODULES.glob("mod-living-azeroth-*/data/sql/db-characters/*.sql"), key=lambda p: p.name)
        if len(custom_files) != 8:
            raise RuntimeError("Expected eight public custom migrations")
        files = [*sum(base_files.values(), []), *custom_files]
        status["input_file_count"] = len(files)
        status["custom_migrations"] = [{"name": p.name, "sha256": sha256(p)} for p in custom_files]
        status["base_sql_manifest_sha256"] = hashlib.sha256("\n".join(f"{p.relative_to(base)} {sha256(p)}" for p in files if p not in custom_files).encode()).hexdigest()
        status["core_commit"] = run(["git", "-C", str(args.core_source.resolve()), "rev-parse", "HEAD"])
        status["image_id"] = run(["docker", "image", "inspect", IMAGE, "--format", "{{.Id}}"])
        with tempfile.TemporaryDirectory(prefix="wow-gate2-secret-") as temp:
            secret = Path(temp) / "db_root_password"
            secret_text = secrets.token_urlsafe(36)
            secret.write_text(secret_text + "\n", encoding="ascii")
            secret.chmod(0o600)
            run(["docker", "volume", "create", volume])
            volume_created = True
            run(["docker", "run", "--detach", "--name", name, "--network", "none", "--restart", "no", "--mount", f"type=volume,source={volume},target=/var/lib/mysql", "--mount", f"type=bind,source={secret},target=/run/secrets/db_root_password,readonly", "--env", "MYSQL_ROOT_PASSWORD_FILE=/run/secrets/db_root_password", IMAGE])
            started = True
            status["observed_network_ports"] = run(["docker", "inspect", "--format", "{{.HostConfig.NetworkMode}}|{{json .HostConfig.PortBindings}}", name])
            if status["observed_network_ports"] not in ("none|null", "none|{}"):
                raise RuntimeError("Container network/port isolation differs from requested")
            ready = False
            for _ in range(90):
                if run(db_command(name, "mysql", "--batch", "--skip-column-names", "--execute", "SELECT 1"), check=False) == "1":
                    ready = True
                    break
                time.sleep(2)
            if not ready:
                raise RuntimeError("MySQL did not become ready within 180 seconds")
            status["mysql_version"] = query(name, "SELECT VERSION()")
            for schema in SCHEMAS.values():
                query(name, f"CREATE DATABASE `{schema}` CHARACTER SET utf8mb4 COLLATE utf8mb4_unicode_ci")
            for key, schema in SCHEMAS.items():
                import_files(name, schema, base_files[key], evidence / "import.log")
            status["base_metrics"] = metrics(name)
            for path in custom_files:
                import_files(name, SCHEMAS["db_characters"], [path], evidence / "import.log")
            status["migrated_metrics"] = metrics(name)
            query(name, "INSERT INTO gate2_auth.account(username,salt,verifier,email,reg_mail) VALUES ('GATE2_SYNTH',REPEAT(UNHEX('00'),32),REPEAT(UNHEX('00'),32),'synthetic.invalid','synthetic.invalid')")
            query(name, "INSERT INTO gate2_characters.characters(guid,account,name,race,`class`,gender,level,taximask,innTriggerId) VALUES (1,1,'GateTwo',1,1,0,1,'',0)")
            status["synthetic_metrics"] = metrics(name)
            expected = status["synthetic_metrics"]
            if not (expected["auth_tables"] >= 22 and expected["characters_tables"] >= 108 and expected["world_tables"] >= 300 and expected["custom_tables"] >= 4 and expected["accounts"] == 1 and expected["characters"] == 1 and expected["online_characters"] == 0):
                raise RuntimeError("Synthetic/base/custom table checks failed")
            roundtrip = evidence / "roundtrip.sql"
            dump(name, roundtrip)
            status["roundtrip_bytes"] = roundtrip.stat().st_size
            status["roundtrip_sha256"] = sha256(roundtrip)
            for schema in SCHEMAS.values():
                query(name, f"DROP DATABASE `{schema}`")
            import_files(name, "", [roundtrip], evidence / "import.log")
            status["restored_metrics"] = metrics(name)
            if status["restored_metrics"] != expected:
                raise RuntimeError("Logical restore metrics differ from pre-export values")
            status["result"] = "PASS"
            roundtrip.unlink()
            status["roundtrip_dump_retained"] = False
    except BaseException as exc:
        status["result"] = "FAIL"
        status["error"] = str(exc)
    finally:
        if started:
            status["stop_output"] = run(["docker", "stop", "--timeout", "30", name], check=False)
            logs = run(["docker", "logs", name], check=False)
            if secret_text and secret_text in logs:
                status["result"] = "FAIL"
                status["error"] = "Generated secret appeared in container logs"
                logs = logs.replace(secret_text, "[REDACTED]")
            (evidence / "mysql.log").write_text(logs + "\n", encoding="utf-8")
        if status["result"] == "PASS":
            run(["docker", "rm", name])
            run(["docker", "volume", "rm", volume])
            status["container_removed"] = True
            status["volume_removed"] = True
        else:
            status["container_preserved_for_failure"] = started
            status["volume_preserved_for_failure"] = volume_created
        status["finished_utc"] = now()
        (evidence / "status.json").write_text(json.dumps(status, indent=2, sort_keys=True) + "\n", encoding="utf-8")
        print(json.dumps(status, indent=2, sort_keys=True))
        print(f"Evidence: {evidence}")
    return 0 if status["result"] == "PASS" else 1


if __name__ == "__main__":
    raise SystemExit(main())
