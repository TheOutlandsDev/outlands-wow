#!/usr/bin/env python3
"""Validate a staged logical WoW database dump in a disposable MySQL container."""

from __future__ import annotations

import argparse
import datetime as dt
import hashlib
import json
import os
import re
import secrets
import shlex
import subprocess
import tempfile
import time
from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]
DEFAULT_DUMP = Path(
    "/opt/outlands-wow/import/run/gates/"
    "20260808-110506Z-la17-paced-demand-production-promotion/restore-test-remapped.sql"
)
DEFAULT_SHA256 = "16e7bd0f2b02f4cf247044f48a3e88a0f5f964980319770753bbbf7a82d10b47"
IMAGE = "mysql:8.4"
HISTORICAL_SCHEMAS = {
    "auth": "la17_restore_auth_20260808",
    "characters": "la17_restore_characters_20260808",
    "world": "la17_restore_world_20260808",
    "playerbots": "la17_restore_playerbots_20260808",
}
HISTORICAL_EXPECTED = {
    "schema_count": 4,
    "accounts": 123,
    "characters": 1009,
    "online_characters": 0,
    "auctions": 11009,
    "world_tables": 320,
    "playerbots_tables": 30,
}
REQUIRED_LA17_TABLES = (
    "la_profile_snapshot",
    "la_guild_schema",
    "la_profile_commerce_schema",
    "la_outlands_item_provenance",
)


class GateError(RuntimeError):
    """Raised when a Gate 2 acceptance check fails."""


def utc_now() -> str:
    return dt.datetime.now(dt.UTC).isoformat()


def sha256_file(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as source:
        for block in iter(lambda: source.read(4 * 1024 * 1024), b""):
            digest.update(block)
    return digest.hexdigest()


def staged_schemas(path: Path) -> set[str]:
    pattern = re.compile(br"^-- Current Database: `([^`]+)`")
    found: set[str] = set()
    with path.open("rb") as source:
        for line in source:
            match = pattern.match(line)
            if match:
                found.add(match.group(1).decode("ascii"))
    return found


def has_completion_marker(path: Path) -> bool:
    with path.open("rb") as source:
        source.seek(max(0, path.stat().st_size - 128 * 1024))
        return b"-- Dump completed on" in source.read()


def log_command(log, command: list[str]) -> None:
    log.write("$ " + shlex.join(command) + "\n")
    log.flush()


def run_capture(
    command: list[str], log, *, check: bool = True
) -> subprocess.CompletedProcess[str]:
    log_command(log, command)
    process = subprocess.run(
        command,
        cwd=ROOT,
        stdout=subprocess.PIPE,
        stderr=subprocess.STDOUT,
        text=True,
        errors="replace",
        check=False,
    )
    log.write(process.stdout)
    log.flush()
    if check and process.returncode:
        raise subprocess.CalledProcessError(process.returncode, command, process.stdout)
    return process


def run_from_file(command: list[str], input_path: Path, log) -> None:
    log_command(log, command + [f"< {input_path}"])
    with input_path.open("rb") as source:
        process = subprocess.run(
            command,
            cwd=ROOT,
            stdin=source,
            stdout=subprocess.PIPE,
            stderr=subprocess.STDOUT,
            check=False,
        )
    output = process.stdout.decode("utf-8", errors="replace")
    log.write(output)
    log.flush()
    if process.returncode:
        raise subprocess.CalledProcessError(process.returncode, command, output)


def run_to_file(command: list[str], output_path: Path, log) -> None:
    log_command(log, command + [f"> {output_path}"])
    with output_path.open("wb") as destination:
        process = subprocess.run(
            command,
            cwd=ROOT,
            stdout=destination,
            stderr=subprocess.PIPE,
            check=False,
        )
    stderr = process.stderr.decode("utf-8", errors="replace")
    log.write(stderr)
    log.flush()
    if process.returncode:
        raise subprocess.CalledProcessError(process.returncode, command, stderr)


def secret_shell(tool: str, arguments: list[str]) -> list[str]:
    shell = (
        'MYSQL_PWD="$(cat /run/secrets/db_root_password)" '
        f"exec {tool} \"$@\""
    )
    return ["sh", "-c", shell, "gate2", *arguments]


def docker_exec(
    container: str, tool: str, arguments: list[str], *, interactive: bool = False
) -> list[str]:
    command = ["docker", "exec"]
    if interactive:
        command.append("--interactive")
    return [*command, container, *secret_shell(tool, arguments)]


def wait_ready(container: str, log, timeout: int = 120) -> None:
    deadline = time.monotonic() + timeout
    command = docker_exec(container, "mysqladmin", ["-uroot", "ping", "--silent"])
    while time.monotonic() < deadline:
        if run_capture(command, log, check=False).returncode == 0:
            return
        time.sleep(2)
    raise GateError(f"MySQL did not become ready within {timeout} seconds")


def mysql_query(container: str, sql: str, log) -> str:
    command = docker_exec(
        container,
        "mysql",
        ["-uroot", "--batch", "--raw", "--skip-column-names", "--execute", sql],
    )
    return run_capture(command, log).stdout.strip()


def collect_metrics(container: str, schemas: dict[str, str], log) -> dict[str, int]:
    queries = {
        "schema_count": (
            "SELECT COUNT(*) FROM information_schema.schemata "
            "WHERE schema_name IN ("
            + ",".join(f"'{schema}'" for schema in schemas.values())
            + ")"
        ),
        "accounts": f"SELECT COUNT(*) FROM `{schemas['auth']}`.`account`",
        "characters": f"SELECT COUNT(*) FROM `{schemas['characters']}`.`characters`",
        "online_characters": (
            f"SELECT COUNT(*) FROM `{schemas['characters']}`.`characters` WHERE `online` <> 0"
        ),
        "auctions": f"SELECT COUNT(*) FROM `{schemas['characters']}`.`auctionhouse`",
        "world_tables": (
            "SELECT COUNT(*) FROM information_schema.tables "
            f"WHERE table_schema = '{schemas['world']}'"
        ),
        "playerbots_tables": (
            "SELECT COUNT(*) FROM information_schema.tables "
            f"WHERE table_schema = '{schemas['playerbots']}'"
        ),
    }
    metrics: dict[str, int] = {}
    for name, query in queries.items():
        value = mysql_query(container, query, log)
        try:
            metrics[name] = int(value)
        except ValueError as exc:
            raise GateError(f"Non-integer metric {name}: {value!r}") from exc
    return metrics


def collect_engines(
    container: str, schemas: dict[str, str], log
) -> dict[str, int]:
    schema_values = ",".join(f"'{schema}'" for schema in schemas.values())
    rows = mysql_query(
        container,
        "SELECT COALESCE(engine,'NULL'),COUNT(*) FROM information_schema.tables "
        f"WHERE table_schema IN ({schema_values}) AND table_type='BASE TABLE' "
        "GROUP BY engine ORDER BY engine",
        log,
    )
    engines: dict[str, int] = {}
    for row in rows.splitlines():
        try:
            name, count = row.split("\t", 1)
            engines[name] = int(count)
        except ValueError as exc:
            raise GateError(f"Invalid storage-engine row: {row!r}") from exc
    unsupported = set(engines) - {"InnoDB", "MyISAM"}
    if unsupported:
        raise GateError(f"Unsupported storage engines for round trip: {engines}")
    return engines


def validate_metrics(metrics: dict[str, int], expected_metrics: dict[str, int]) -> None:
    differences = {
        name: {"expected": expected, "actual": metrics.get(name)}
        for name, expected in expected_metrics.items()
        if metrics.get(name) != expected
    }
    if differences:
        raise GateError(f"Database metric mismatch: {json.dumps(differences, sort_keys=True)}")


def validate_required_tables(container: str, characters_schema: str, log) -> list[str]:
    values = ",".join(f"'{table}'" for table in REQUIRED_LA17_TABLES)
    sql = (
        "SELECT table_name FROM information_schema.tables "
        f"WHERE table_schema = '{characters_schema}' "
        f"AND table_name IN ({values}) ORDER BY table_name"
    )
    present = [line for line in mysql_query(container, sql, log).splitlines() if line]
    missing = sorted(set(REQUIRED_LA17_TABLES) - set(present))
    if missing:
        raise GateError(f"Required Living Azeroth tables missing: {', '.join(missing)}")
    return present


def stop_cleanly(container: str, log) -> int:
    run_capture(["docker", "stop", "--timeout", "30", container], log)
    value = run_capture(
        ["docker", "inspect", "--format", "{{.State.ExitCode}}", container], log
    ).stdout.strip()
    try:
        exit_code = int(value)
    except ValueError as exc:
        raise GateError(f"Invalid container exit code: {value!r}") from exc
    if exit_code != 0:
        raise GateError(f"MySQL did not stop cleanly: exit code {exit_code}")
    return exit_code


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--dump", type=Path)
    parser.add_argument("--manifest", type=Path)
    parser.add_argument("--expected-sha256")
    parser.add_argument("--image", default=IMAGE)
    parser.add_argument("--evidence-root", type=Path)
    args = parser.parse_args()

    manifest: dict[str, object] | None = None
    release: str | None = None
    if args.manifest:
        manifest_path = args.manifest.resolve()
        if not manifest_path.is_file():
            parser.error("--manifest must name an existing exporter manifest")
        manifest = json.loads(manifest_path.read_text(encoding="utf-8-sig"))
        if not isinstance(manifest, dict):
            parser.error("--manifest must contain a JSON object")
        if manifest.get("result") != "PASS":
            parser.error("--manifest must record result PASS")
        if manifest.get("change_id") != "database-migration-gate2-export":
            parser.error("--manifest must come from a Gate 2 exporter or recovery")
        manifest_schemas = manifest.get("schemas")
        if not isinstance(manifest_schemas, list) or len(manifest_schemas) != 4:
            parser.error("--manifest must list four schemas in component order")
        if any(not isinstance(value, str) for value in manifest_schemas):
            parser.error("--manifest schema names must be strings")
        schemas = dict(zip(("auth", "characters", "world", "playerbots"), manifest_schemas))
        manifest_metrics = manifest.get("metrics")
        if not isinstance(manifest_metrics, dict):
            parser.error("--manifest must contain aggregate metrics")
        try:
            expected_metrics = {
                name: int(manifest_metrics[name]) for name in HISTORICAL_EXPECTED
            }
        except (KeyError, TypeError, ValueError) as exc:
            parser.error(f"--manifest contains invalid aggregate metrics: {exc}")
        if expected_metrics["schema_count"] != 4:
            parser.error("--manifest must report exactly four schemas")
        if expected_metrics["online_characters"] != 0:
            parser.error("--manifest must report zero online characters")
        dump_name = manifest.get("dump_file")
        if args.dump:
            dump = args.dump.resolve()
        elif isinstance(dump_name, str) and dump_name:
            dump = (manifest_path.parent / dump_name).resolve()
        else:
            parser.error("--manifest must contain dump_file when --dump is omitted")
        expected_sha256 = args.expected_sha256 or str(manifest.get("sha256", ""))
        release = str(manifest.get("release", ""))
        input_class = "manifest-backed-logical-export"
    else:
        dump = (args.dump or DEFAULT_DUMP).resolve()
        schemas = HISTORICAL_SCHEMAS
        expected_metrics = HISTORICAL_EXPECTED
        expected_sha256 = args.expected_sha256 or DEFAULT_SHA256
        input_class = "historical-la17-logical-restore-artifact"

    if any(not re.fullmatch(r"[A-Za-z0-9_]+", value) for value in schemas.values()):
        parser.error("schema names must contain only letters, digits, and underscores")
    if not dump.is_file() or dump.suffix.lower() != ".sql":
        parser.error("--dump must name an existing logical .sql file")
    if "/data/mysql/" in f"{dump}/":
        parser.error("raw MySQL data directories are forbidden; provide a logical .sql dump")

    stamp = dt.datetime.now(dt.UTC).strftime("%Y%m%d-%H%M%SZ")
    evidence = args.evidence_root or ROOT / "run" / "gates" / f"{stamp}-database-migration-gate2"
    evidence.mkdir(parents=True, exist_ok=False)
    log_path = evidence / "database-test.log"
    status_path = evidence / "status.json"
    container = f"wow-gate2-{stamp.lower()}"
    volume = f"wow-gate2-{stamp.lower()}-mysql"
    created_container = False
    created_volume = False
    secret_value = ""
    status: dict[str, object] = {
        "change_id": "database-migration-gate2",
        "started_utc": utc_now(),
        "result": "IN_PROGRESS",
        "gate_eligible": False,
        "input_class": input_class,
        "release": release,
        "dump_bytes": dump.stat().st_size,
        "dump_contents_retained": False,
        "image": args.image,
        "network_mode": "none",
        "published_ports": [],
        "secret_transport": "ephemeral root-only file mounted read-only",
    }

    try:
        with log_path.open("w", encoding="utf-8") as log, tempfile.TemporaryDirectory(
            prefix="wow-gate2-secrets-"
        ) as secret_dir_name, tempfile.TemporaryDirectory(prefix="wow-gate2-roundtrip-") as temp_name:
            secret_dir = Path(secret_dir_name)
            os.chmod(secret_dir, 0o700)
            secret_file = secret_dir / "db_root_password"
            secret_value = secrets.token_urlsafe(36)
            secret_file.write_text(secret_value + "\n", encoding="utf-8")
            os.chmod(secret_file, 0o600)
            roundtrip = Path(temp_name) / "roundtrip.sql"

            source_hash = sha256_file(dump)
            status["dump_sha256"] = source_hash
            if expected_sha256 and source_hash.lower() != expected_sha256.lower():
                raise GateError("Logical dump SHA-256 does not match the declared input")
            if not has_completion_marker(dump):
                raise GateError("Logical dump completion marker is missing")
            discovered = staged_schemas(dump)
            if discovered != set(schemas.values()):
                raise GateError(
                    "Unexpected logical-dump schemas: " + json.dumps(sorted(discovered))
                )
            status["schemas"] = schemas

            revision = run_capture(["git", "rev-parse", "HEAD"], log).stdout.strip()
            status["source_commit"] = revision
            run_capture(["docker", "version"], log)
            image_info = run_capture(
                [
                    "docker",
                    "image",
                    "inspect",
                    args.image,
                    "--format",
                    "{{json .RepoDigests}}|{{.Architecture}}/{{.Os}}",
                ],
                log,
            ).stdout.strip()
            status["image_identity"] = image_info

            run_capture(["docker", "volume", "create", volume], log)
            created_volume = True
            run_capture(
                [
                    "docker",
                    "run",
                    "--detach",
                    "--name",
                    container,
                    "--network",
                    "none",
                    "--mount",
                    f"type=volume,source={volume},target=/var/lib/mysql",
                    "--mount",
                    f"type=bind,source={secret_file},target=/run/secrets/db_root_password,readonly",
                    "--env",
                    "MYSQL_ROOT_PASSWORD_FILE=/run/secrets/db_root_password",
                    args.image,
                ],
                log,
            )
            created_container = True
            wait_ready(container, log)
            status["mysql_version"] = mysql_query(container, "SELECT VERSION()", log)
            network = run_capture(
                [
                    "docker",
                    "inspect",
                    "--format",
                    "{{.HostConfig.NetworkMode}}|{{json .HostConfig.PortBindings}}",
                    container,
                ],
                log,
            ).stdout.strip()
            status["observed_network_and_ports"] = network
            if network not in {"none|null", "none|{}"}:
                raise GateError(f"Unexpected container network or port bindings: {network}")

            import_command = docker_exec(
                container,
                "mysql",
                ["-uroot", "--binary-mode=1", "--default-character-set=utf8mb4"],
                interactive=True,
            )
            run_from_file(import_command, dump, log)
            initial = collect_metrics(container, schemas, log)
            validate_metrics(initial, expected_metrics)
            required_tables = validate_required_tables(container, schemas["characters"], log)
            status["initial_metrics"] = initial
            status["required_custom_tables"] = required_tables
            status["storage_engines"] = collect_engines(container, schemas, log)

            status["first_stop_exit_code"] = stop_cleanly(container, log)
            run_capture(["docker", "start", container], log)
            wait_ready(container, log)
            restarted = collect_metrics(container, schemas, log)
            validate_metrics(restarted, expected_metrics)
            if restarted != initial:
                raise GateError("Metrics changed across clean MySQL restart")
            status["restart_metrics"] = restarted

            export_command = docker_exec(
                container,
                "mysqldump",
                [
                    "-uroot",
                    "--lock-all-tables",
                    "--quick",
                    "--routines",
                    "--events",
                    "--triggers",
                    "--hex-blob",
                    "--no-tablespaces",
                    "--set-gtid-purged=OFF",
                    "--default-character-set=utf8mb4",
                    "--databases",
                    *schemas.values(),
                ],
            )
            run_to_file(export_command, roundtrip, log)
            if not has_completion_marker(roundtrip):
                raise GateError("Round-trip export completion marker is missing")
            status["roundtrip_dump_consistency"] = (
                "global read lock on isolated networkless validation server"
            )
            status["roundtrip_bytes"] = roundtrip.stat().st_size
            status["roundtrip_sha256"] = sha256_file(roundtrip)
            status["roundtrip_dump_retained"] = False

            drop_sql = ";".join(f"DROP DATABASE `{schema}`" for schema in schemas.values())
            mysql_query(container, drop_sql, log)
            run_from_file(import_command, roundtrip, log)
            restored = collect_metrics(container, schemas, log)
            validate_metrics(restored, expected_metrics)
            if restored != initial:
                raise GateError("Metrics changed after logical export/drop/restore rollback test")
            validate_required_tables(container, schemas["characters"], log)
            restored_engines = collect_engines(container, schemas, log)
            if restored_engines != status["storage_engines"]:
                raise GateError("Storage engines changed after logical rollback test")
            status["roundtrip_metrics"] = restored
            status["roundtrip_storage_engines"] = restored_engines

            status["final_stop_exit_code"] = stop_cleanly(container, log)
            mysql_logs = run_capture(["docker", "logs", container], log).stdout
            if secret_value in mysql_logs:
                raise GateError("Generated secret appeared in container logs")
            (evidence / "mysql.log").write_text(mysql_logs, encoding="utf-8")
            status["result"] = "PASS"
            status["gate_eligible"] = bool(manifest and release.lower().startswith("la25"))
            if not status["gate_eligible"]:
                status["blocker"] = "A fresh terminal-zero LA25 logical export has not been staged"
            return_code = 0
    except (GateError, OSError, subprocess.CalledProcessError) as exc:
        status["result"] = "FAIL"
        status["error"] = str(exc)
        return_code = 1
    finally:
        cleanup_log = evidence / "cleanup.log"
        container_removed = not created_container
        volume_removed = not created_volume
        with cleanup_log.open("w", encoding="utf-8") as log:
            if created_container:
                run_capture(
                    ["docker", "stop", "--timeout", "30", container], log, check=False
                )
                run_capture(["docker", "rm", "--force", container], log, check=False)
                container_removed = run_capture(
                    ["docker", "inspect", container], log, check=False
                ).returncode != 0
            if created_volume:
                run_capture(["docker", "volume", "rm", "--force", volume], log, check=False)
                volume_removed = run_capture(
                    ["docker", "volume", "inspect", volume], log, check=False
                ).returncode != 0
        status["cleanup"] = {
            "container_removed": container_removed,
            "volume_removed": volume_removed,
            "secret_removed": True,
            "roundtrip_dump_removed": True,
        }
        status["finished_utc"] = utc_now()
        status_path.write_text(json.dumps(status, indent=2, sort_keys=True) + "\n", encoding="utf-8")

    print(json.dumps(status, indent=2, sort_keys=True))
    print(f"Evidence: {evidence}")
    return return_code


if __name__ == "__main__":
    raise SystemExit(main())
