#!/usr/bin/env python3
"""Recover a logical LA25 export from a stopped, read-only Windows MySQL datadir."""

from __future__ import annotations

import argparse
import datetime as dt
import hashlib
import json
import os
import shlex
import subprocess
import sys
import tempfile
import time
from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]
DEFAULT_SOURCE = Path("/opt/outlands-wow/import/data/mysql")
DEFAULT_CREDENTIALS = Path(
    "/opt/outlands-wow/import/secrets/mysql-credentials.json"
)
IMAGE = "mysql:8.4"
RELEASE = "la25-social-conversation-v8"
SCHEMAS = (
    "la_ahfinal11_auth",
    "la_ahfinal11_characters",
    "la_ahfinal11_world",
    "la_ahfinal11_playerbots",
)
REQUIRED_TABLES = (
    "la_profile_snapshot",
    "la_guild_schema",
    "la_profile_commerce_schema",
    "la_outlands_item_provenance",
    "mod_ollama_chat_history",
    "mod_ollama_chat_personality",
    "mod_ollama_chat_personality_templates",
    "mod_ollama_chat_bot_player_sentiments",
)


class RecoveryError(RuntimeError):
    """Raised when an offline recovery precondition or validation fails."""


def utc_now() -> str:
    return dt.datetime.now(dt.UTC).isoformat()


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


def sha256_file(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as source:
        for block in iter(lambda: source.read(4 * 1024 * 1024), b""):
            digest.update(block)
    return digest.hexdigest()


def has_completion_marker(path: Path) -> bool:
    with path.open("rb") as source:
        source.seek(max(0, path.stat().st_size - 128 * 1024))
        return b"-- Dump completed on" in source.read()


def source_inventory(path: Path) -> dict[str, object]:
    file_count = 0
    apparent_bytes = 0
    latest_ns = 0
    for directory, _, files in os.walk(path):
        for name in files:
            item = Path(directory) / name
            info = item.stat()
            file_count += 1
            apparent_bytes += info.st_size
            latest_ns = max(latest_ns, info.st_mtime_ns)
    if not file_count:
        raise RecoveryError(f"Offline source contains no regular files: {path}")
    latest = dt.datetime.fromtimestamp(latest_ns / 1_000_000_000, dt.UTC).isoformat()
    return {
        "file_count": file_count,
        "apparent_bytes": apparent_bytes,
        "latest_mtime_utc": latest,
    }


def secret_shell(tool: str, arguments: list[str]) -> list[str]:
    shell = (
        'MYSQL_PWD="$(cat /run/secrets/db_root_password)" '
        f"exec {tool} \"$@\""
    )
    return ["sh", "-c", shell, "gate2-recovery", *arguments]


def docker_exec(container: str, tool: str, arguments: list[str]) -> list[str]:
    return ["docker", "exec", container, *secret_shell(tool, arguments)]


def wait_ready(container: str, log, timeout: int = 180) -> None:
    deadline = time.monotonic() + timeout
    ping = docker_exec(
        container,
        "mysqladmin",
        ["-uroot", "--socket=/var/run/mysqld/mysqld.sock", "ping", "--silent"],
    )
    while time.monotonic() < deadline:
        if run_capture(ping, log, check=False).returncode == 0:
            return
        state = run_capture(
            ["docker", "inspect", "--format", "{{.State.Running}}", container],
            log,
            check=False,
        ).stdout.strip()
        if state == "false":
            raise RecoveryError("Recovery MySQL container exited before becoming ready")
        time.sleep(3)
    raise RecoveryError(f"Recovery MySQL did not become ready within {timeout} seconds")


def mysql_query(container: str, sql: str, log) -> str:
    command = docker_exec(
        container,
        "mysql",
        [
            "-uroot",
            "--socket=/var/run/mysqld/mysqld.sock",
            "--batch",
            "--raw",
            "--skip-column-names",
            "--execute",
            sql,
        ],
    )
    return run_capture(command, log).stdout.strip()


def collect_metrics(container: str, log) -> dict[str, int]:
    auth, characters, world, playerbots = SCHEMAS
    queries = {
        "schema_count": (
            "SELECT COUNT(*) FROM information_schema.schemata WHERE schema_name IN ("
            + ",".join(f"'{schema}'" for schema in SCHEMAS)
            + ")"
        ),
        "accounts": f"SELECT COUNT(*) FROM `{auth}`.`account`",
        "characters": f"SELECT COUNT(*) FROM `{characters}`.`characters`",
        "online_characters": (
            f"SELECT COUNT(*) FROM `{characters}`.`characters` WHERE `online` <> 0"
        ),
        "auctions": f"SELECT COUNT(*) FROM `{characters}`.`auctionhouse`",
        "world_tables": (
            "SELECT COUNT(*) FROM information_schema.tables "
            f"WHERE table_schema = '{world}'"
        ),
        "playerbots_tables": (
            "SELECT COUNT(*) FROM information_schema.tables "
            f"WHERE table_schema = '{playerbots}'"
        ),
    }
    metrics: dict[str, int] = {}
    for name, query in queries.items():
        value = mysql_query(container, query, log)
        try:
            metrics[name] = int(value)
        except ValueError as exc:
            raise RecoveryError(f"Invalid aggregate metric {name}: {value!r}") from exc
    if metrics["schema_count"] != 4:
        raise RecoveryError("The four active Living Azeroth schemas are not all present")
    return metrics


def normalize_offline_state(
    container: str, before_count: int, log
) -> dict[str, int | str]:
    if before_count < 0:
        raise RecoveryError("Invalid pre-normalization online-character count")
    if before_count == 0:
        return {
            "column": f"{SCHEMAS[1]}.characters.online",
            "before": 0,
            "rows_changed": 0,
            "after": 0,
        }
    changed_text = mysql_query(
        container,
        f"START TRANSACTION; UPDATE `{SCHEMAS[1]}`.`characters` "
        "SET `online` = 0 WHERE `online` <> 0; SELECT ROW_COUNT(); COMMIT",
        log,
    )
    try:
        rows_changed = int(changed_text)
    except ValueError as exc:
        raise RecoveryError(
            f"Invalid offline-state normalization count: {changed_text!r}"
        ) from exc
    after_count = int(
        mysql_query(
            container,
            f"SELECT COUNT(*) FROM `{SCHEMAS[1]}`.`characters` WHERE `online` <> 0",
            log,
        )
    )
    if rows_changed != before_count or after_count != 0:
        raise RecoveryError(
            "Offline-state normalization did not change exactly the expected rows"
        )
    return {
        "column": f"{SCHEMAS[1]}.characters.online",
        "before": before_count,
        "rows_changed": rows_changed,
        "after": after_count,
    }


def collect_engines(container: str, log) -> dict[str, int]:
    schemas = ",".join(f"'{schema}'" for schema in SCHEMAS)
    rows = mysql_query(
        container,
        "SELECT COALESCE(engine,'NULL'),COUNT(*) FROM information_schema.tables "
        f"WHERE table_schema IN ({schemas}) AND table_type='BASE TABLE' "
        "GROUP BY engine ORDER BY engine",
        log,
    )
    engines: dict[str, int] = {}
    for row in rows.splitlines():
        name, count = row.split("\t", 1)
        engines[name] = int(count)
    unsupported = set(engines) - {"InnoDB", "MyISAM"}
    if unsupported:
        raise RecoveryError(f"Unsupported source storage engines: {engines}")
    return engines


def validate_tables(container: str, log) -> list[str]:
    values = ",".join(f"'{table}'" for table in REQUIRED_TABLES)
    rows = mysql_query(
        container,
        "SELECT table_name FROM information_schema.tables "
        f"WHERE table_schema = '{SCHEMAS[1]}' AND table_name IN ({values}) "
        "ORDER BY table_name",
        log,
    )
    present = [row for row in rows.splitlines() if row]
    missing = sorted(set(REQUIRED_TABLES) - set(present))
    if missing:
        raise RecoveryError("Required LA25 tables are missing: " + ", ".join(missing))
    return present


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--source", type=Path, default=DEFAULT_SOURCE)
    parser.add_argument("--credentials", type=Path, default=DEFAULT_CREDENTIALS)
    parser.add_argument("--image", default=IMAGE)
    parser.add_argument("--evidence-root", type=Path)
    parser.add_argument("--output-root", type=Path)
    args = parser.parse_args()

    source = args.source.resolve()
    credentials_path = args.credentials.resolve()
    if not source.is_dir() or not (source / "ibdata1").is_file():
        parser.error("--source must be a complete offline MySQL datadir")
    if not credentials_path.is_file():
        parser.error("--credentials must name the staged local credential JSON")

    stamp = dt.datetime.now(dt.UTC).strftime("%Y%m%d-%H%M%SZ")
    evidence = args.evidence_root or ROOT / "run" / "gates" / f"{stamp}-offline-datadir-recovery-gate2"
    output = args.output_root or ROOT / "run" / "private" / f"gate2-la25-export-{stamp}"
    evidence.mkdir(parents=True, exist_ok=False)
    output.mkdir(parents=True, exist_ok=False)
    os.chmod(output, 0o700)
    dump_path = output / "la25-four-databases.sql"
    manifest_path = output / "manifest.json"
    log_path = evidence / "recovery.log"
    status_path = evidence / "status.json"
    container = f"wow-gate2-recovery-{stamp.lower()}"
    volume = f"wow-gate2-recovery-{stamp.lower()}-mysql"
    created_container = False
    created_volume = False
    secret_value = ""
    secret_file_path: Path | None = None
    source_ibdata_mtime: int | None = None
    status: dict[str, object] = {
        "change_id": "offline-datadir-recovery-gate2",
        "started_utc": utc_now(),
        "result": "IN_PROGRESS",
        "source_read_only": False,
        "source_modified": False,
        "image": args.image,
        "network_mode": "none plus skip-networking",
        "published_ports": [],
        "physical_working_copy_retained": False,
        "logical_dump_committed": False,
    }

    try:
        with log_path.open("w", encoding="utf-8") as log, tempfile.TemporaryDirectory(
            prefix="wow-gate2-recovery-secrets-"
        ) as secret_dir_name:
            # The Docker-group host namespace can expose different CIFS VFS
            # options than the workspace sandbox. Verify the actual bind used by
            # the copy process, which is explicitly mounted read-only.
            mount_check = run_capture(
                [
                    "docker",
                    "run",
                    "--rm",
                    "--network",
                    "none",
                    "--mount",
                    f"type=bind,source={source},target=/source,readonly",
                    args.image,
                    "bash",
                    "-lc",
                    "awk '$2 == \"/source\" {n=split($4,a,\",\"); mode=\"\"; "
                    "for(i=1;i<=n;i++) if(a[i]==\"ro\" || a[i]==\"rw\") mode=a[i]; "
                    "print mode, $3}' /proc/mounts",
                ],
                log,
            ).stdout.strip()
            mount_fields = mount_check.split()
            if len(mount_fields) != 2:
                raise RecoveryError(
                    f"Could not parse recovery source mount state: {mount_check!r}"
                )
            mount_options = mount_fields[0].split(",")
            if "ro" not in mount_options:
                raise RecoveryError("Recovery source bind must be read-only")
            status["source_read_only"] = True
            status["source_mount"] = {
                "filesystem": mount_fields[1],
                "vfs_options": mount_options,
            }

            upgrade_history = json.loads(
                (source / "mysql_upgrade_history").read_text(encoding="utf-8")
            )
            history = upgrade_history.get("upgrade_history", [])
            if not history or history[-1].get("version") != "8.4.9":
                raise RecoveryError("Expected the staged MySQL 8.4.9 provenance marker")
            status["physical_source_version"] = history[-1]["version"]
            status["physical_source"] = source_inventory(source)
            source_ibdata_mtime = (source / "ibdata1").stat().st_mtime_ns

            binlog_index = source / "binlog.index"
            binlog_index_bytes = binlog_index.read_bytes()
            binlog_entries = [
                line.rstrip(b"\r")
                for line in binlog_index_bytes.split(b"\n")
                if line.rstrip(b"\r")
            ]
            if not binlog_entries or any(
                not entry.startswith(b".\\") for entry in binlog_entries
            ):
                raise RecoveryError("Expected Windows-style relative binlog.index entries")
            binlog_names = [entry[2:].decode("ascii") for entry in binlog_entries]
            if any(
                Path(name).name != name or not (source / name).is_file()
                for name in binlog_names
            ):
                raise RecoveryError("A binlog.index entry is unsafe or missing")
            normalized_binlog_index = binlog_index_bytes.replace(b".\\", b"./")
            status["binlog_index_translation"] = {
                "entry_count": len(binlog_entries),
                "source_sha256": hashlib.sha256(binlog_index_bytes).hexdigest(),
                "expected_normalized_sha256": hashlib.sha256(
                    normalized_binlog_index
                ).hexdigest(),
                "scope": "disposable physical copy only",
            }

            credential_data = json.loads(credentials_path.read_text(encoding="utf-8-sig"))
            secret_value = str(credential_data.get("root_password", ""))
            if not secret_value:
                raise RecoveryError("Staged credential JSON has no root_password")
            secret_dir = Path(secret_dir_name)
            os.chmod(secret_dir, 0o700)
            secret_file = secret_dir / "db_root_password"
            secret_file.write_text(secret_value + "\n", encoding="utf-8")
            os.chmod(secret_file, 0o600)
            secret_file_path = secret_file

            status["source_commit"] = run_capture(
                ["git", "rev-parse", "HEAD"], log
            ).stdout.strip()
            status["image_identity"] = run_capture(
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
            run_capture(["docker", "volume", "create", volume], log)
            created_volume = True

            run_capture(
                [
                    "docker",
                    "run",
                    "--rm",
                    "--network",
                    "none",
                    "--user",
                    "0:0",
                    "--mount",
                    f"type=bind,source={source},target=/source,readonly",
                    "--mount",
                    f"type=volume,source={volume},target=/target",
                    args.image,
                    "bash",
                    "-lc",
                    "set -euo pipefail; cp -a /source/. /target/; chown -R mysql:mysql /target",
                ],
                log,
            )

            copied_inventory_output = run_capture(
                [
                    "docker",
                    "run",
                    "--rm",
                    "--network",
                    "none",
                    "--mount",
                    f"type=volume,source={volume},target=/target,readonly",
                    args.image,
                    "bash",
                    "-lc",
                    "find /target -type f -printf '%s\\n' | "
                    "awk '{bytes += $1; files += 1} END {print files \"\\t\" bytes}'",
                ],
                log,
            ).stdout.strip()
            try:
                copied_files_text, copied_bytes_text = copied_inventory_output.split(
                    "\t", 1
                )
                copied_inventory = {
                    "file_count": int(copied_files_text),
                    "apparent_bytes": int(copied_bytes_text),
                }
            except (ValueError, TypeError) as exc:
                raise RecoveryError(
                    f"Could not parse disposable-copy inventory: {copied_inventory_output!r}"
                ) from exc
            status["physical_working_copy"] = copied_inventory
            source_inventory_values = status["physical_source"]
            if not isinstance(source_inventory_values, dict) or any(
                copied_inventory[key] != source_inventory_values[key]
                for key in ("file_count", "apparent_bytes")
            ):
                raise RecoveryError(
                    "Disposable physical copy does not match source file count and size"
                )

            normalized_hash_output = run_capture(
                [
                    "docker",
                    "run",
                    "--rm",
                    "--network",
                    "none",
                    "--user",
                    "0:0",
                    "--mount",
                    f"type=volume,source={volume},target=/target",
                    args.image,
                    "bash",
                    "-lc",
                    "sed -i 's#^[.]\\\\#./#' /target/binlog.index; "
                    "sha256sum /target/binlog.index",
                ],
                log,
            ).stdout.strip().split()[0]
            expected_normalized_hash = status["binlog_index_translation"]
            if not isinstance(expected_normalized_hash, dict) or (
                normalized_hash_output
                != expected_normalized_hash["expected_normalized_sha256"]
            ):
                raise RecoveryError("Disposable binlog.index normalization hash mismatch")
            expected_normalized_hash["observed_normalized_sha256"] = (
                normalized_hash_output
            )

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
                    args.image,
                    "--skip-networking=ON",
                    "--lower-case-table-names=1",
                ],
                log,
            )
            created_container = True
            wait_ready(container, log)

            status["mysql_runtime_version"] = mysql_query(
                container, "SELECT VERSION()", log
            )
            lower_case = mysql_query(container, "SELECT @@lower_case_table_names", log)
            if lower_case != "1":
                raise RecoveryError(f"Unexpected lower_case_table_names value: {lower_case}")
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
            if network not in {"none|null", "none|{}"}:
                raise RecoveryError(f"Unexpected recovery network state: {network}")
            status["observed_network_and_ports"] = network
            source_metrics = collect_metrics(container, log)
            status["physical_source_metrics"] = source_metrics
            status["offline_state_normalization"] = normalize_offline_state(
                container, source_metrics["online_characters"], log
            )
            status["metrics"] = collect_metrics(container, log)
            if status["metrics"]["online_characters"] != 0:
                raise RecoveryError("Normalized logical-export state is not offline")
            status["storage_engines"] = collect_engines(container, log)
            status["required_la25_tables"] = validate_tables(container, log)

            export = docker_exec(
                container,
                "mysqldump",
                [
                    "-uroot",
                    "--socket=/var/run/mysqld/mysqld.sock",
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
                    *SCHEMAS,
                ],
            )
            run_to_file(export, dump_path, log)
            os.chmod(dump_path, 0o600)
            if not has_completion_marker(dump_path):
                raise RecoveryError("Fresh logical export has no completion marker")
            status["dump_consistency"] = (
                "global read lock on isolated networkless recovery server"
            )

            run_capture(["docker", "stop", "--timeout", "60", container], log)
            stop_code = run_capture(
                ["docker", "inspect", "--format", "{{.State.ExitCode}}", container],
                log,
            ).stdout.strip()
            if stop_code != "0":
                raise RecoveryError(f"Recovery MySQL stop returned {stop_code}")
            status["recovery_stop_exit_code"] = 0

            manifest = {
                "result": "PASS",
                "change_id": "database-migration-gate2-export",
                "release": RELEASE,
                "created_utc": utc_now(),
                "dump_file": dump_path.name,
                "bytes": dump_path.stat().st_size,
                "sha256": sha256_file(dump_path),
                "schemas": list(SCHEMAS),
                "metrics": status["metrics"],
                "storage_engines": status["storage_engines"],
                "credentials_recorded": False,
                "source_kind": "offline-read-only-mysql-8.4.9-datadir",
            }
            manifest_path.write_text(
                json.dumps(manifest, indent=2, sort_keys=True) + "\n", encoding="utf-8"
            )
            os.chmod(manifest_path, 0o600)
            status["logical_export"] = {
                "relative_directory": str(output.relative_to(ROOT)),
                "bytes": manifest["bytes"],
                "sha256": manifest["sha256"],
                "mode": "0600",
            }

            validation_dir = evidence / "logical-validation"
            validation = run_capture(
                [
                    sys.executable,
                    str(ROOT / "tools" / "run_gate2_database_test.py"),
                    "--manifest",
                    str(manifest_path),
                    "--image",
                    args.image,
                    "--evidence-root",
                    str(validation_dir),
                ],
                log,
            )
            validation_status = json.loads(
                (validation_dir / "status.json").read_text(encoding="utf-8")
            )
            if validation_status.get("result") != "PASS" or not validation_status.get(
                "gate_eligible"
            ):
                raise RecoveryError("Fresh LA25 logical validator did not become gate-eligible")
            status["logical_validation"] = {
                "result": validation_status["result"],
                "gate_eligible": validation_status["gate_eligible"],
                "roundtrip_sha256": validation_status["roundtrip_sha256"],
                "metrics": validation_status["roundtrip_metrics"],
                "runner_output_sha256": hashlib.sha256(
                    validation.stdout.encode("utf-8")
                ).hexdigest(),
            }

            if (source / "ibdata1").stat().st_mtime_ns != source_ibdata_mtime:
                raise RecoveryError("Read-only source metadata changed during recovery")
            status["source_modified"] = False
            status["result"] = "PASS"
            return_code = 0
    except (RecoveryError, OSError, subprocess.CalledProcessError, json.JSONDecodeError) as exc:
        status["result"] = "FAIL"
        status["error"] = str(exc)
        return_code = 1
    finally:
        cleanup_path = evidence / "cleanup.log"
        mysql_log_path = evidence / "recovery-mysql.log"
        container_removed = not created_container
        volume_removed = not created_volume
        with cleanup_path.open("w", encoding="utf-8") as log:
            if created_container:
                mysql_logs = run_capture(
                    ["docker", "logs", container], log, check=False
                ).stdout
                mysql_log_path.write_text(mysql_logs, encoding="utf-8")
                run_capture(
                    ["docker", "stop", "--timeout", "60", container], log, check=False
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
        secret_removed = secret_file_path is None or not secret_file_path.exists()
        if status.get("result") != "PASS":
            dump_path.unlink(missing_ok=True)
            manifest_path.unlink(missing_ok=True)
        if source_ibdata_mtime is not None:
            try:
                status["source_modified"] = (
                    (source / "ibdata1").stat().st_mtime_ns != source_ibdata_mtime
                )
            except OSError as exc:
                status["source_modified_check_error"] = str(exc)
        status["cleanup"] = {
            "container_removed": container_removed,
            "physical_volume_removed": volume_removed,
            "secret_removed": secret_removed,
        }
        status["physical_working_copy_retained"] = not volume_removed
        if not container_removed or not volume_removed or not secret_removed:
            status["result"] = "FAIL"
            status["error"] = "Recovery cleanup verification failed"
            return_code = 1
        if status.get("source_modified"):
            status["result"] = "FAIL"
            status["error"] = "Read-only source metadata changed during recovery"
            return_code = 1
        status["finished_utc"] = utc_now()
        status_path.write_text(
            json.dumps(status, indent=2, sort_keys=True) + "\n", encoding="utf-8"
        )

    print(json.dumps(status, indent=2, sort_keys=True))
    print(f"Evidence: {evidence}")
    return return_code


if __name__ == "__main__":
    raise SystemExit(main())
