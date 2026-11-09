#!/usr/bin/env python3
"""Provision and validate the isolated persistent Linux server-only runtime."""

from __future__ import annotations

import argparse
import datetime as dt
import fcntl
import hashlib
import json
import os
import re
import secrets
import shlex
import shutil
import socket
import stat
import subprocess
import sys
import time
from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]
PRIVATE_ROOT = ROOT / "run" / "private" / "gate3"
GATE4_PRIVATE_ROOT = ROOT / "run" / "private" / "gate4"
GATE5_PRIVATE_ROOT = ROOT / "run" / "private" / "gate5"
GATE6_PRIVATE_ROOT = ROOT / "run" / "private" / "gate6"
GATE6_RUNTIME_STATE = GATE6_PRIVATE_ROOT / "runtime-state.json"
DEVELOPMENT_OFFLINE_MARKER = GATE6_PRIVATE_ROOT / "development-offline.json"
DEFAULT_GATE6_BACKUP_ROOT = Path("/var/backups/outlands-wow")
DEFAULT_DATA_SOURCE = Path(
    "/opt/outlands-wow/import/client-data/ac-data-v19-enUS"
)
DEFAULT_MANIFEST_GLOB = "gate2-la25-export-*/manifest.json"
MYSQL_IMAGE = "mysql:8.4"
RUNTIME_IMAGE = "living-azeroth-linux:gate3"
OPERATIONAL_RUNTIME_IMAGE = "living-azeroth-linux:gate6"
GATE8_RUNTIME_IMAGE = "living-azeroth-linux:gate8-profile-candidate"
GATE8_RUNTIME_IMAGE_ID = (
    "sha256:0000000000000000000000000000000000000000000000000000000000000000"
)
GATE8_RUNTIME_SOURCE_REVISION = "6e9bfbdb66b906a56602f0870f32be27ae69e34e"
GATE8_LOCAL_RUNTIME_IMAGE = "living-azeroth-linux:gate8-local-candidate"
GATE8_LOCAL_RUNTIME_IMAGE_ID = (
    "sha256:0000000000000000000000000000000000000000000000000000000000000000"
)
GATE8_LOCAL_RUNTIME_SOURCE_REVISION = "d6afce3f0eebf344efc67257ec507a7449700878"
GATE8_LOCAL_RUNTIME_AUTHSERVER_SHA256 = "0000000000000000000000000000000000000000000000000000000000000000"
GATE8_LOCAL_RUNTIME_WORLDSERVER_SHA256 = "0000000000000000000000000000000000000000000000000000000000000000"
GATE8_LOCAL_ACCEPTED_RUN = "example-accepted-run"
GATE9_RUNTIME_IMAGE = "living-azeroth-linux:gate9-ollama-candidate"
GATE9_RUNTIME_IMAGE_ID = (
    "sha256:0000000000000000000000000000000000000000000000000000000000000000"
)
GATE9_RUNTIME_SOURCE_REVISION = "3957e49d8a02851c7651e6367993ce8248ff0733"
GATE9_RUNTIME_AUTHSERVER_SHA256 = "0000000000000000000000000000000000000000000000000000000000000000"
GATE9_RUNTIME_WORLDSERVER_SHA256 = "0000000000000000000000000000000000000000000000000000000000000000"
GATE9_ACCEPTED_RUN = "example-accepted-run"
GATE9_OLLAMA_MODEL = "qwen2.5:7b-instruct"
GATE9_OLLAMA_BLOB_SHA256 = "0000000000000000000000000000000000000000000000000000000000000000"
GATE10_RUNTIME_IMAGE = "living-azeroth-linux:gate10-ollama-routed-candidate"
GATE10_RUNTIME_IMAGE_ID = (
    "sha256:0000000000000000000000000000000000000000000000000000000000000000"
)
GATE10_RUNTIME_SOURCE_REVISION = "d0e51b0d2f3f89530f39a0f16865f53fb1087a70"
GATE10_RUNTIME_AUTHSERVER_SHA256 = "0000000000000000000000000000000000000000000000000000000000000000"
GATE10_RUNTIME_WORLDSERVER_SHA256 = "0000000000000000000000000000000000000000000000000000000000000000"
GATE10_ACCEPTED_RUN = "example-accepted-run"
DATABASE_VOLUME = "living-azeroth-gate3-mysql"
DATA_VOLUME = "living-azeroth-gate3-data-v19"
NETWORK = "living-azeroth-gate3-internal"
AI_NETWORK = "living-azeroth-gate9-ai-internal"
OLLAMA_CONTAINER = "ollama-ollama-1"
DATABASE_CONTAINER = "living-azeroth-gate3-database"
AUTH_CONTAINER = "living-azeroth-gate3-authserver"
WORLD_CONTAINER = "living-azeroth-gate3-worldserver"
CONTAINERS = (WORLD_CONTAINER, AUTH_CONTAINER, DATABASE_CONTAINER)
RESOURCE_POLICY_VERSION = "living-azeroth-v1"
DATABASE_RESOURCE_BUDGET = {
    "cpus": "2",
    "nano_cpus": 2_000_000_000,
    "memory": "4g",
    "memory_bytes": 4 * 1024**3,
    "pids_limit": 512,
}
AUTH_RESOURCE_BUDGET = {
    "cpus": "0.5",
    "nano_cpus": 500_000_000,
    "memory": "512m",
    "memory_bytes": 512 * 1024**2,
    "pids_limit": 1024,
}
WORLD_DISABLED_RESOURCE_BUDGET = {
    "cpus": "2",
    "nano_cpus": 2_000_000_000,
    "memory": "6g",
    "memory_bytes": 6 * 1024**3,
    "pids_limit": 1024,
}
WORLD_ACTIVE_RESOURCE_BUDGET = {
    "cpus": "4",
    "nano_cpus": 4_000_000_000,
    "memory": "8g",
    "memory_bytes": 8 * 1024**3,
    "pids_limit": 1024,
}
GATE4_CHANGE_ID = "linux-server-only-soak-backup-gate4"
GATE5_CHANGE_ID = "tailscale-client-smoke-gate5"
GATE6_CHANGE_ID = "private-operational-deployment-gate6"
GATE7_CHANGE_ID = "private-production-hardening-gate7"
GATE8_CHANGE_ID = "ai-restoration-gate8"
GATE9_CHANGE_ID = "ai-restoration-gate9-ollama-dialogue"
GATE10_CHANGE_ID = "ai-restoration-gate10-deterministic-routing"
OPERATIONAL_AI_PROFILES = (
    "disabled",
    "bots-small",
    "bots-dialogue-small",
    "bots-local-dialogue-small",
    "bots-ollama-dialogue-small",
    "bots-ollama-routed-small",
)
GATE6_ACCEPTED_BACKUP_DIRECTORY = "example-logical-backup"
TAILSCALE_HOSTNAME = "example-wow-host"
TAILSCALE_ADDRESS = "100.64.0.10"
SCHEMAS = (
    "la_ahfinal11_auth",
    "la_ahfinal11_characters",
    "la_ahfinal11_world",
    "la_ahfinal11_playerbots",
)
STRUCTURAL_METRICS = (
    "schema_count",
    "accounts",
    "characters",
    "world_tables",
    "playerbots_tables",
)
PROTECTED_HOST_PORTS = (3306, 3724, 8085)
FATAL_PATTERNS = (
    "segmentation fault",
    "assertion failed",
    "terminate called",
    "access violation",
    "deadlock found",
    "could not connect to mysql",
    "unable to connect to database",
)


class GateError(RuntimeError):
    """Raised when a Gate 3 safety or acceptance check fails."""


def utc_now() -> str:
    return dt.datetime.now(dt.UTC).isoformat()


def sha256_file(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as source:
        for block in iter(lambda: source.read(4 * 1024 * 1024), b""):
            digest.update(block)
    return digest.hexdigest()


def log_command(log, command: list[str]) -> None:
    log.write("$ " + shlex.join(command) + "\n")
    log.flush()


def run_capture(
    command: list[str], log, *, check: bool = True, record_output: bool = True
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
    if record_output:
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


def run_to_file(command: list[str], output_path: Path, log) -> str:
    log_command(log, command + [f"> {output_path.name}"])
    with output_path.open("xb") as destination:
        os.chmod(output_path, 0o600)
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
    return stderr


def has_dump_completion_marker(path: Path) -> bool:
    with path.open("rb") as dump:
        dump.seek(max(0, path.stat().st_size - 8192))
        tail = dump.read().decode("utf-8", errors="replace")
    return "-- Dump completed on " in tail


def write_private_json(path: Path, value: object) -> None:
    temporary = path.with_suffix(path.suffix + ".tmp")
    temporary.write_text(
        json.dumps(value, indent=2, sort_keys=True) + "\n", encoding="utf-8"
    )
    os.chmod(temporary, 0o600)
    temporary.replace(path)


def ensure_secret(path: Path) -> None:
    if path.exists():
        value = path.read_text(encoding="utf-8").strip()
        allowed = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789-_"
        if not value or any(character not in allowed for character in value):
            raise GateError(f"Invalid existing secret format: {path.name}")
    else:
        path.write_text(secrets.token_urlsafe(36) + "\n", encoding="utf-8")
    os.chmod(path, 0o600)


def docker_object_exists(kind: str, name: str, log) -> bool:
    command = ["docker", kind, "inspect", name]
    return run_capture(command, log, check=False, record_output=False).returncode == 0


def host_selected_listeners(selected_ports: tuple[int, ...]) -> list[int]:
    listeners: set[int] = set()
    for table in (Path("/proc/net/tcp"), Path("/proc/net/tcp6")):
        try:
            lines = table.read_text(encoding="ascii").splitlines()[1:]
        except OSError:
            continue
        for line in lines:
            fields = line.split()
            if len(fields) < 4 or fields[3] != "0A":
                continue
            port = int(fields[1].split(":", 1)[1], 16)
            if port in selected_ports:
                listeners.add(port)
    return sorted(listeners)


def host_protected_listeners() -> list[int]:
    return host_selected_listeners(PROTECTED_HOST_PORTS)


def load_manifest(path: Path) -> tuple[dict[str, object], Path]:
    manifest_path = path.resolve()
    if not manifest_path.is_file():
        raise GateError(f"Gate 2 manifest is missing: {manifest_path}")
    manifest = json.loads(manifest_path.read_text(encoding="utf-8-sig"))
    if not isinstance(manifest, dict):
        raise GateError("Gate 2 manifest must contain a JSON object")
    if manifest.get("result") != "PASS" or manifest.get("change_id") != "database-migration-gate2-export":
        raise GateError("Gate 2 manifest is not a passing logical export")
    if not str(manifest.get("release", "")).lower().startswith("la25"):
        raise GateError("Gate 2 manifest is not an LA25 release")
    if manifest.get("schemas") != list(SCHEMAS):
        raise GateError("Gate 2 manifest does not contain the four expected schemas")
    dump_name = manifest.get("dump_file")
    if not isinstance(dump_name, str) or not dump_name:
        raise GateError("Gate 2 manifest has no dump filename")
    dump = (manifest_path.parent / dump_name).resolve()
    if not dump.is_file() or dump.suffix.lower() != ".sql":
        raise GateError("Gate 2 logical dump is missing")
    if sha256_file(dump) != manifest.get("sha256"):
        raise GateError("Gate 2 logical dump hash does not match its manifest")
    return manifest, dump


def discover_manifest() -> Path:
    matches = sorted((ROOT / "run" / "private").glob(DEFAULT_MANIFEST_GLOB))
    passing: list[Path] = []
    for match in matches:
        try:
            value = json.loads(match.read_text(encoding="utf-8-sig"))
        except (OSError, json.JSONDecodeError):
            continue
        if isinstance(value, dict) and value.get("result") == "PASS":
            passing.append(match)
    if len(passing) != 1:
        raise GateError(
            "Expected exactly one passing Gate 2 private manifest; pass --manifest explicitly"
        )
    return passing[0]


def source_inventory(path: Path) -> dict[str, int]:
    file_count = 0
    apparent_bytes = 0
    for directory, _, files in os.walk(path):
        for name in files:
            info = (Path(directory) / name).stat()
            file_count += 1
            apparent_bytes += info.st_size
    if not file_count:
        raise GateError(f"Data source has no files: {path}")
    return {"file_count": file_count, "apparent_bytes": apparent_bytes}


def secret_shell(tool: str, arguments: list[str]) -> list[str]:
    shell = 'MYSQL_PWD="$(cat /run/secrets/db_root_password)" exec ' + tool + ' "$@"'
    return ["sh", "-c", shell, "gate3", *arguments]


def docker_root_exec(container: str, tool: str, arguments: list[str]) -> list[str]:
    return ["docker", "exec", container, *secret_shell(tool, arguments)]


def wait_mysql_container(container: str, log, timeout: int = 180) -> None:
    deadline = time.monotonic() + timeout
    command = docker_root_exec(container, "mysqladmin", ["-uroot", "ping", "--silent"])
    while time.monotonic() < deadline:
        if run_capture(command, log, check=False, record_output=False).returncode == 0:
            return
        if not docker_object_exists("container", container, log):
            raise GateError("Database container disappeared during startup")
        running = run_capture(
            ["docker", "inspect", "--format", "{{.State.Running}}", container],
            log,
            check=False,
            record_output=False,
        ).stdout.strip()
        if running == "false":
            raise GateError("Database container exited before becoming ready")
        time.sleep(2)
    raise GateError("Database did not become ready before timeout")


def wait_database(log, timeout: int = 180) -> None:
    wait_mysql_container(DATABASE_CONTAINER, log, timeout)


def mysql_query(sql: str, log, container: str = DATABASE_CONTAINER) -> str:
    return run_capture(
        docker_root_exec(
            container,
            "mysql",
            ["-uroot", "--batch", "--raw", "--skip-column-names", "--execute", sql],
        ),
        log,
    ).stdout.strip()


def collect_metrics(log, container: str = DATABASE_CONTAINER) -> dict[str, int]:
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
        value = mysql_query(query, log, container)
        try:
            metrics[name] = int(value)
        except ValueError as exc:
            raise GateError(f"Invalid database metric {name}: {value!r}") from exc
    return metrics


def normalize_offline_character_state(
    before_count: int, log, container: str = DATABASE_CONTAINER
) -> dict[str, int | str]:
    if before_count < 0:
        raise GateError("Invalid pre-normalization online-character count")
    if before_count == 0:
        return {
            "column": f"{SCHEMAS[1]}.characters.online",
            "before": 0,
            "rows_changed": 0,
            "after": 0,
        }
    changed_text = mysql_query(
        f"START TRANSACTION; UPDATE `{SCHEMAS[1]}`.`characters` "
        "SET `online` = 0 WHERE `online` <> 0; SELECT ROW_COUNT(); COMMIT",
        log,
        container,
    )
    try:
        rows_changed = int(changed_text)
    except ValueError as exc:
        raise GateError(
            f"Invalid offline-state normalization count: {changed_text!r}"
        ) from exc
    after_count = int(
        mysql_query(
            f"SELECT COUNT(*) FROM `{SCHEMAS[1]}`.`characters` "
            "WHERE `online` <> 0",
            log,
            container,
        )
    )
    if rows_changed != before_count or after_count != 0:
        raise GateError(
            "Offline-state normalization did not change exactly the expected rows"
        )
    return {
        "column": f"{SCHEMAS[1]}.characters.online",
        "before": before_count,
        "rows_changed": rows_changed,
        "after": after_count,
    }


def collect_storage_engines(log, container: str = DATABASE_CONTAINER) -> dict[str, int]:
    schemas = ",".join(f"'{schema}'" for schema in SCHEMAS)
    output = mysql_query(
        "SELECT COALESCE(engine, 'NULL'), COUNT(*) "
        "FROM information_schema.tables "
        f"WHERE table_schema IN ({schemas}) GROUP BY engine ORDER BY engine",
        log,
        container,
    )
    result: dict[str, int] = {}
    for line in output.splitlines():
        try:
            engine, count = line.split("\t", 1)
            result[engine] = int(count)
        except ValueError as exc:
            raise GateError(f"Invalid storage-engine metric: {line!r}") from exc
    if not result:
        raise GateError("No storage-engine metrics were returned")
    return result


def validate_structural_metrics(
    metrics: dict[str, int], expected: dict[str, int], *, require_offline: bool
) -> None:
    differences = {
        name: {"expected": expected.get(name), "actual": metrics.get(name)}
        for name in STRUCTURAL_METRICS
        if metrics.get(name) != expected.get(name)
    }
    if differences:
        raise GateError(f"Structural database metrics changed: {differences}")
    if require_offline and metrics.get("online_characters") != 0:
        raise GateError("Characters remain marked online after server shutdown")


def create_network(log, change_id: str = "linux-server-only-runtime-gate3") -> None:
    if docker_object_exists("network", NETWORK, log):
        raise GateError(f"Refusing to adopt existing Docker network: {NETWORK}")
    run_capture(
        [
            "docker",
            "network",
            "create",
            "--internal",
            "--label",
            f"io.outlandswow.gate={change_id}",
            NETWORK,
        ],
        log,
    )
    internal = run_capture(
        ["docker", "network", "inspect", "--format", "{{.Internal}}", NETWORK], log
    ).stdout.strip()
    if internal != "true":
        raise GateError("Gate 3 Docker network is not internal")


def remove_network(log) -> bool:
    if not docker_object_exists("network", NETWORK, log):
        return True
    run_capture(["docker", "network", "rm", NETWORK], log, check=False)
    return not docker_object_exists("network", NETWORK, log)


def profile_uses_ollama(ai_profile: str) -> bool:
    return ai_profile in {
        "bots-ollama-dialogue-small",
        "bots-ollama-routed-small",
    }


def verify_private_ollama(log, *, require_ai_network: bool = False) -> dict[str, object]:
    if not docker_object_exists("container", OLLAMA_CONTAINER, log):
        raise GateError("The protected Ollama container is missing")
    details = json.loads(
        run_capture(["docker", "inspect", OLLAMA_CONTAINER], log, record_output=False).stdout
    )[0]
    if not details.get("State", {}).get("Running"):
        raise GateError("The protected Ollama container is not running")
    bindings = details.get("HostConfig", {}).get("PortBindings", {}).get("11434/tcp", [])
    if not bindings or any(
        item.get("HostIp") not in {"127.0.0.1", "::1"} for item in bindings
    ):
        raise GateError("Ollama TCP 11434 is not restricted to host loopback")
    networks = details.get("NetworkSettings", {}).get("Networks", {})
    if require_ai_network and AI_NETWORK not in networks:
        raise GateError("Ollama is absent from the private Living Azeroth AI network")
    modelfile = run_capture(
        ["docker", "exec", OLLAMA_CONTAINER, "ollama", "show", GATE9_OLLAMA_MODEL, "--modelfile"],
        log,
        record_output=False,
    ).stdout
    match = re.search(r"^FROM\s+\S*sha256-([0-9a-f]{64})\s*$", modelfile, re.MULTILINE)
    if not match or match.group(1) != GATE9_OLLAMA_BLOB_SHA256:
        raise GateError("The local Ollama model differs from the accepted Gate 9 artifact")
    return {
        "running": True,
        "model": GATE9_OLLAMA_MODEL,
        "model_blob_sha256": GATE9_OLLAMA_BLOB_SHA256,
        "host_binding": "127.0.0.1:11434",
        "ai_network_attached": AI_NETWORK in networks,
        "tailscale_published": False,
    }


def create_ollama_network(log) -> dict[str, object]:
    if docker_object_exists("network", AI_NETWORK, log):
        raise GateError(f"Refusing to adopt existing Docker network: {AI_NETWORK}")
    verify_private_ollama(log)
    run_capture(
        [
            "docker",
            "network",
            "create",
            "--internal",
            "--label",
            f"io.outlandswow.gate={GATE9_CHANGE_ID}",
            AI_NETWORK,
        ],
        log,
    )
    internal = run_capture(
        ["docker", "network", "inspect", "--format", "{{.Internal}}", AI_NETWORK],
        log,
    ).stdout.strip()
    if internal != "true":
        raise GateError("Living Azeroth AI network is not internal")
    run_capture(
        ["docker", "network", "connect", "--alias", "ollama", AI_NETWORK, OLLAMA_CONTAINER],
        log,
    )
    return verify_private_ollama(log, require_ai_network=True)


def remove_ollama_network(log) -> bool:
    if not docker_object_exists("network", AI_NETWORK, log):
        return True
    details = json.loads(
        run_capture(["docker", "network", "inspect", AI_NETWORK], log, record_output=False).stdout
    )[0]
    labels = details.get("Labels", {}) or {}
    if labels.get("io.outlandswow.gate") != GATE9_CHANGE_ID:
        raise GateError("Refusing to remove an unowned Living Azeroth AI network")
    if docker_object_exists("container", OLLAMA_CONTAINER, log):
        ollama = json.loads(
            run_capture(["docker", "inspect", OLLAMA_CONTAINER], log, record_output=False).stdout
        )[0]
        networks = ollama.get("NetworkSettings", {}).get("Networks", {})
        if AI_NETWORK in networks:
            run_capture(
                ["docker", "network", "disconnect", "--force", AI_NETWORK, OLLAMA_CONTAINER],
                log,
                check=False,
            )
    run_capture(["docker", "network", "rm", AI_NETWORK], log, check=False)
    return not docker_object_exists("network", AI_NETWORK, log)


def resource_budget_for(
    role: str, ai_profile: str = "disabled"
) -> dict[str, int | str]:
    if role == "database":
        return DATABASE_RESOURCE_BUDGET
    if role == "authserver":
        return AUTH_RESOURCE_BUDGET
    if role == "worldserver":
        return (
            WORLD_DISABLED_RESOURCE_BUDGET
            if ai_profile == "disabled"
            else WORLD_ACTIVE_RESOURCE_BUDGET
        )
    raise GateError(f"Unknown resource-budget role: {role}")


def resource_limit_arguments(
    budget: dict[str, int | str]
) -> list[str]:
    return [
        "--cpus",
        str(budget["cpus"]),
        "--memory",
        str(budget["memory"]),
        "--memory-swap",
        str(budget["memory"]),
        "--pids-limit",
        str(budget["pids_limit"]),
        "--label",
        f"com.server.resource-policy={RESOURCE_POLICY_VERSION}",
    ]


def resource_limit_snapshot(details: dict[str, object]) -> dict[str, int]:
    host_config = details.get("HostConfig", {})
    if not isinstance(host_config, dict):
        host_config = {}
    return {
        "nano_cpus": int(host_config.get("NanoCpus") or 0),
        "memory_bytes": int(host_config.get("Memory") or 0),
        "memory_swap_bytes": int(host_config.get("MemorySwap") or 0),
        "pids_limit": int(host_config.get("PidsLimit") or 0),
    }


def expected_resource_limit_snapshot(
    budget: dict[str, int | str]
) -> dict[str, int]:
    memory_bytes = int(budget["memory_bytes"])
    return {
        "nano_cpus": int(budget["nano_cpus"]),
        "memory_bytes": memory_bytes,
        "memory_swap_bytes": memory_bytes,
        "pids_limit": int(budget["pids_limit"]),
    }


def start_database(
    root_secret: Path,
    app_secret: Path,
    log,
    change_id: str = "linux-server-only-runtime-gate3",
) -> dict[str, object]:
    if docker_object_exists("container", DATABASE_CONTAINER, log):
        raise GateError(f"Refusing to adopt existing container: {DATABASE_CONTAINER}")
    budget = resource_budget_for("database")
    command = [
            "docker",
            "run",
            "--detach",
            "--name",
            DATABASE_CONTAINER,
            "--network",
            NETWORK,
            "--network-alias",
            "database",
        ]
    command.extend(resource_limit_arguments(budget))
    command.extend(
        [
            "--mount",
            f"type=volume,source={DATABASE_VOLUME},target=/var/lib/mysql",
            "--mount",
            f"type=bind,source={root_secret},target=/run/secrets/db_root_password,readonly",
            "--mount",
            f"type=bind,source={app_secret},target=/run/secrets/acore_password,readonly",
            "--env",
            "MYSQL_ROOT_PASSWORD_FILE=/run/secrets/db_root_password",
            "--label",
            f"io.outlandswow.gate={change_id}",
            "--label",
            "com.server.managed=true",
            "--label",
            "com.server.service=living-azeroth",
            "--label",
            "com.server.role=database",
            MYSQL_IMAGE,
        ]
    )
    run_capture(command, log)
    wait_database(log)
    return assert_no_host_bindings(
        DATABASE_CONTAINER, log, expected_resource_budget=budget
    )


def create_application_user(log) -> None:
    shell = (
        'set -eu; ROOT_PASSWORD="$(cat /run/secrets/db_root_password)"; '
        'APP_PASSWORD="$(cat /run/secrets/acore_password)"; '
        'MYSQL_PWD="$ROOT_PASSWORD" mysql -uroot --execute="'
        "CREATE USER IF NOT EXISTS 'acore'@'%' IDENTIFIED BY '$APP_PASSWORD'; "
        "ALTER USER 'acore'@'%' IDENTIFIED BY '$APP_PASSWORD'; "
        + " ".join(
            f"GRANT ALL PRIVILEGES ON {schema}.* TO 'acore'@'%';"
            for schema in SCHEMAS
        )
        + ' FLUSH PRIVILEGES;"'
    )
    run_capture(
        ["docker", "exec", DATABASE_CONTAINER, "sh", "-c", shell], log
    )


def assert_no_host_bindings(
    container: str,
    log,
    *,
    expected_networks: tuple[str, ...] = (NETWORK,),
    expected_resource_budget: dict[str, int | str] | None = None,
) -> dict[str, object]:
    details = json.loads(
        run_capture(["docker", "inspect", container], log, record_output=False).stdout
    )[0]
    bindings = details["HostConfig"].get("PortBindings")
    if bindings not in (None, {}):
        raise GateError(f"Container unexpectedly publishes host ports: {container}")
    networks = sorted(details["NetworkSettings"].get("Networks", {}).keys())
    if networks != sorted(expected_networks):
        raise GateError(f"Container is attached to unexpected networks: {container}")
    result: dict[str, object] = {
        "host_port_bindings": {},
        "networks": networks,
        "read_only_root": bool(details["HostConfig"].get("ReadonlyRootfs")),
        "user": details["Config"].get("User", ""),
    }
    if expected_resource_budget is not None:
        actual = resource_limit_snapshot(details)
        expected = expected_resource_limit_snapshot(expected_resource_budget)
        if actual != expected:
            raise GateError(
                f"Container resource limits differ for {container}: "
                f"expected={expected}, actual={actual}"
            )
        labels = details.get("Config", {}).get("Labels", {}) or {}
        if labels.get("com.server.resource-policy") != RESOURCE_POLICY_VERSION:
            raise GateError(f"Container resource policy label differs for {container}")
        result["resource_policy"] = RESOURCE_POLICY_VERSION
        result["resource_limits"] = actual
    return result


def start_server(
    role: str,
    app_secret: Path,
    log,
    *,
    image: str = RUNTIME_IMAGE,
    change_id: str = "linux-server-only-runtime-gate3",
    ai_profile: str = "disabled",
) -> str:
    if role == "authserver":
        container = AUTH_CONTAINER
    elif role == "worldserver":
        container = WORLD_CONTAINER
    else:
        raise GateError(f"Unknown server role: {role}")
    if docker_object_exists("container", container, log):
        raise GateError(f"Refusing to adopt existing container: {container}")
    command = [
        "docker",
        "run",
        "--detach",
        "--name",
        container,
        "--network",
        NETWORK,
        "--user",
        "1000:1000",
        "--read-only",
        "--tmpfs",
        "/run/living-azeroth:rw,nosuid,nodev,noexec,uid=1000,gid=1000,mode=0700",
        "--tmpfs",
        "/tmp:rw,nosuid,nodev,noexec,uid=1000,gid=1000,mode=0700",
        "--cap-drop",
        "ALL",
        "--security-opt",
        "no-new-privileges",
        "--stop-timeout",
        "120",
        "--mount",
        f"type=bind,source={app_secret},target=/run/secrets/acore_password,readonly",
    ]
    budget = resource_budget_for(role, ai_profile)
    command.extend(resource_limit_arguments(budget))
    if role == "worldserver":
        if ai_profile not in OPERATIONAL_AI_PROFILES:
            raise GateError(f"Unsupported operational AI profile: {ai_profile}")
        if ai_profile != "disabled":
            command.extend(["--env", f"LIVING_AZEROTH_AI_PROFILE={ai_profile}"])
        if profile_uses_ollama(ai_profile):
            command.extend(["--network", AI_NETWORK])
        command.extend(
            [
                "--tmpfs",
                "/azerothcore/dist/etc/modules:rw,nosuid,nodev,noexec,uid=1000,gid=1000,mode=0700",
                "--mount",
                f"type=volume,source={DATA_VOLUME},target=/azerothcore/data,readonly",
            ]
        )
    command.extend(
        [
            "--label",
            f"io.outlandswow.gate={change_id}",
            "--label",
            "com.server.managed=true",
            "--label",
            "com.server.service=living-azeroth",
            "--label",
            f"com.server.role={role}",
            image,
            role,
        ]
    )
    run_capture(command, log)
    return container


def container_network_address(container: str, log) -> str:
    details = json.loads(
        run_capture(["docker", "inspect", container], log, record_output=False).stdout
    )[0]
    network = details["NetworkSettings"].get("Networks", {}).get(NETWORK)
    if not isinstance(network, dict) or not network.get("IPAddress"):
        raise GateError(f"Container has no address on the Gate 5 network: {container}")
    return str(network["IPAddress"])


def container_listener_count(container: str, port: int, log) -> int:
    port_hex = f"{port:04X}"
    command = [
        "docker",
        "exec",
        container,
        "awk",
        "-v",
        f"port={port_hex}",
        '$4 == "0A" {split($2,a,":"); if(a[2] == port) count += 1} END {print count + 0}',
        "/proc/net/tcp",
        "/proc/net/tcp6",
    ]
    value = run_capture(command, log, check=False, record_output=False).stdout.strip()
    try:
        return int(value)
    except ValueError:
        return 0


def wait_server(container: str, port: int, marker: str, log, timeout: int) -> None:
    deadline = time.monotonic() + timeout
    while time.monotonic() < deadline:
        running = run_capture(
            ["docker", "inspect", "--format", "{{.State.Running}}", container],
            log,
            check=False,
            record_output=False,
        ).stdout.strip()
        if running != "true":
            raise GateError(f"{container} exited before readiness")
        output = run_capture(
            ["docker", "logs", "--tail", "500", container],
            log,
            check=False,
            record_output=False,
        ).stdout
        marker_in_file = (
            run_capture(
                [
                    "docker",
                    "exec",
                    container,
                    "grep",
                    "--fixed-strings",
                    "--quiet",
                    "--",
                    marker,
                    "/run/living-azeroth/logs/Server.log",
                ],
                log,
                check=False,
                record_output=False,
            ).returncode
            == 0
        )
        if (marker in output or marker_in_file) and container_listener_count(
            container, port, log
        ) == 1:
            return
        time.sleep(3)
    raise GateError(f"{container} did not become ready before timeout")


def capture_container_log(container: str, destination: Path, secrets_to_scan: list[str], log) -> str:
    output = run_capture(["docker", "logs", container], log, check=False).stdout
    for value in secrets_to_scan:
        if value and value in output:
            raise GateError(f"A generated secret appeared in {container} logs")
    destination.write_text(output, encoding="utf-8")
    return output


def stop_container(container: str, timeout: int, log) -> int:
    if not docker_object_exists("container", container, log):
        return 0
    run_capture(
        ["docker", "stop", "--timeout", str(timeout), container], log, check=False
    )
    value = run_capture(
        ["docker", "inspect", "--format", "{{.State.ExitCode}}", container],
        log,
        check=False,
    ).stdout.strip()
    try:
        return int(value)
    except ValueError as exc:
        raise GateError(f"Could not read exit code for {container}") from exc


def remove_container(container: str, log) -> bool:
    if not docker_object_exists("container", container, log):
        return True
    run_capture(["docker", "rm", "--force", container], log, check=False)
    return not docker_object_exists("container", container, log)


def build_runtime_image(log) -> dict[str, str]:
    revision = run_capture(["git", "rev-parse", "HEAD"], log).stdout.strip()
    run_capture(
        [
            "docker",
            "build",
            "--pull=false",
            "--network",
            "none",
            "--file",
            "deploy/Dockerfile.gate3",
            "--build-arg",
            f"SOURCE_REVISION={revision}",
            "--tag",
            RUNTIME_IMAGE,
            "deploy",
        ],
        log,
    )
    identity = run_capture(
        [
            "docker",
            "image",
            "inspect",
            RUNTIME_IMAGE,
            "--format",
            "{{.Id}}|{{.Architecture}}/{{.Os}}",
        ],
        log,
    ).stdout.strip()
    binaries = run_capture(
        [
            "docker",
            "run",
            "--rm",
            "--network",
            "none",
            "--entrypoint",
            "sha256sum",
            RUNTIME_IMAGE,
            "/azerothcore/dist/bin/authserver",
            "/azerothcore/dist/bin/worldserver",
        ],
        log,
    ).stdout.strip().splitlines()
    return {
        "identity": identity,
        "authserver_sha256": binaries[0].split()[0],
        "worldserver_sha256": binaries[1].split()[0],
        "source_commit": revision,
    }


def copy_data_volume(source: Path, expected: dict[str, int], log) -> dict[str, object]:
    mount_check = run_capture(
        [
            "docker",
            "run",
            "--rm",
            "--network",
            "none",
            "--mount",
            f"type=bind,source={source},target=/source,readonly",
            MYSQL_IMAGE,
            "bash",
            "-lc",
            "awk '$2 == \"/source\" {n=split($4,a,\",\"); mode=\"\"; "
            "for(i=1;i<=n;i++) if(a[i]==\"ro\" || a[i]==\"rw\") mode=a[i]; "
            "print mode, $3}' /proc/mounts",
        ],
        log,
    ).stdout.strip().split()
    if len(mount_check) != 2 or mount_check[0] != "ro":
        raise GateError("Client-data source bind is not read-only")
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
            f"type=volume,source={DATA_VOLUME},target=/target",
            MYSQL_IMAGE,
            "bash",
            "-lc",
            "set -euo pipefail; cp -a /source/. /target/; "
            "chown -R 1000:1000 /target; chmod -R a=rX,u+w /target",
        ],
        log,
    )
    output = run_capture(
        [
            "docker",
            "run",
            "--rm",
            "--network",
            "none",
            "--mount",
            f"type=volume,source={DATA_VOLUME},target=/target,readonly",
            MYSQL_IMAGE,
            "bash",
            "-lc",
            "find /target -type f -printf '%s\\n' | "
            "awk '{bytes += $1; files += 1} END {print files \"\\t\" bytes}'",
        ],
        log,
    ).stdout.strip()
    try:
        files_text, bytes_text = output.split("\t", 1)
        copied = {"file_count": int(files_text), "apparent_bytes": int(bytes_text)}
    except ValueError as exc:
        raise GateError(f"Could not parse copied data inventory: {output!r}") from exc
    if copied != expected:
        raise GateError(f"Local data-volume inventory mismatch: {copied} != {expected}")
    return {
        "source_bind": "ro",
        "source_filesystem": mount_check[1],
        "source": expected,
        "copy": copied,
    }


def provision(
    manifest: dict[str, object],
    dump: Path,
    data_source: Path,
    root_secret: Path,
    app_secret: Path,
    state_path: Path,
    log,
) -> dict[str, object]:
    if state_path.is_file():
        state = json.loads(state_path.read_text(encoding="utf-8"))
        if state.get("result") != "PROVISIONED":
            raise GateError("Existing Gate 3 state is not a provisioned baseline")
        if state.get("dump_sha256") != manifest.get("sha256"):
            raise GateError("Persistent volume baseline does not match the Gate 2 dump")
        if not docker_object_exists("volume", DATABASE_VOLUME, log) or not docker_object_exists(
            "volume", DATA_VOLUME, log
        ):
            raise GateError("Gate 3 state exists but a persistent volume is missing")
        ensure_secret(root_secret)
        ensure_secret(app_secret)
        return {"result": "ALREADY_PROVISIONED", "state": state}

    if docker_object_exists("volume", DATABASE_VOLUME, log) or docker_object_exists(
        "volume", DATA_VOLUME, log
    ):
        raise GateError("Refusing to adopt unlabeled Gate 3 persistent volumes")
    if not data_source.is_dir():
        raise GateError(f"Client-data source is missing: {data_source}")
    for required in ("dbc", "maps", "vmaps", "mmaps", "Cameras"):
        if not (data_source / required).is_dir():
            raise GateError(f"Client-data source is missing {required}")

    data_inventory = source_inventory(data_source)
    ensure_secret(root_secret)
    ensure_secret(app_secret)
    created_volumes: list[str] = []
    network_created = False
    try:
        for volume, purpose in (
            (DATABASE_VOLUME, "mysql"),
            (DATA_VOLUME, "client-data-v19"),
        ):
            run_capture(
                [
                    "docker",
                    "volume",
                    "create",
                    "--label",
                    "io.outlandswow.gate=linux-server-only-runtime-gate3",
                    "--label",
                    f"io.outlandswow.purpose={purpose}",
                    volume,
                ],
                log,
            )
            created_volumes.append(volume)
        data_result = copy_data_volume(data_source, data_inventory, log)
        create_network(log)
        network_created = True
        start_database(root_secret, app_secret, log)
        import_command = [
            "docker",
            "exec",
            "--interactive",
            DATABASE_CONTAINER,
            *secret_shell(
                "mysql", ["-uroot", "--binary-mode=1", "--default-character-set=utf8mb4"]
            ),
        ]
        run_from_file(import_command, dump, log)
        create_application_user(log)
        metrics = collect_metrics(log)
        expected_metrics = {
            name: int(value)
            for name, value in dict(manifest.get("metrics", {})).items()
        }
        validate_structural_metrics(metrics, expected_metrics, require_offline=True)
        if metrics.get("auctions") != expected_metrics.get("auctions"):
            raise GateError("Auction count changed during initial logical provisioning")
        database_stop_code = stop_container(DATABASE_CONTAINER, 60, log)
        if database_stop_code != 0:
            raise GateError(f"Initial database stop returned {database_stop_code}")
        if not remove_container(DATABASE_CONTAINER, log):
            raise GateError("Initial database container cleanup failed")
        if not remove_network(log):
            raise GateError("Initial Gate 3 network cleanup failed")
        network_created = False
        state = {
            "result": "PROVISIONED",
            "created_utc": utc_now(),
            "database_volume": DATABASE_VOLUME,
            "data_volume": DATA_VOLUME,
            "data_inventory": data_result,
            "dump_bytes": int(manifest["bytes"]),
            "dump_sha256": manifest["sha256"],
            "metrics": metrics,
            "schemas": list(SCHEMAS),
            "secrets_recorded": False,
        }
        write_private_json(state_path, state)
        return {"result": "PROVISIONED", "state": state}
    except Exception:
        for container in CONTAINERS:
            stop_container(container, 30, log)
            remove_container(container, log)
        if network_created:
            remove_network(log)
        for volume in reversed(created_volumes):
            run_capture(["docker", "volume", "rm", "--force", volume], log, check=False)
        state_path.unlink(missing_ok=True)
        raise


def require_gate3_baseline(
    manifest: dict[str, object],
    root_secret: Path,
    app_secret: Path,
    state_path: Path,
    log,
) -> dict[str, object]:
    evidence_path = (
        ROOT
        / "migration"
        / "gates"
        / "evidence"
        / "0003-server-only-runtime"
        / "server-only-runtime-status.json"
    )
    if not evidence_path.is_file():
        raise GateError("Curated Gate 3 PASS evidence is missing")
    gate3_evidence = json.loads(evidence_path.read_text(encoding="utf-8"))
    if gate3_evidence.get("result") != "PASS" or not gate3_evidence.get(
        "gate_eligible"
    ):
        raise GateError("Curated Gate 3 evidence is not gate-eligible")
    if not state_path.is_file():
        raise GateError("Gate 3 persistent-volume state is missing")
    state = json.loads(state_path.read_text(encoding="utf-8"))
    if state.get("result") != "PROVISIONED":
        raise GateError("Gate 3 persistent-volume state is not provisioned")
    if state.get("dump_sha256") != manifest.get("sha256"):
        raise GateError("Gate 3 persistent database does not match the Gate 2 dump")
    for volume in (DATABASE_VOLUME, DATA_VOLUME):
        if not docker_object_exists("volume", volume, log):
            raise GateError(f"Required Gate 3 persistent volume is missing: {volume}")
    ensure_secret(root_secret)
    ensure_secret(app_secret)
    return {
        "result": "PASS",
        "gate3_commit": "14f9e55",
        "database_volume": DATABASE_VOLUME,
        "data_volume": DATA_VOLUME,
        "dump_sha256": state["dump_sha256"],
        "created_utc": state["created_utc"],
        "secrets_recorded": False,
    }


def require_gate4_baseline(log) -> dict[str, object]:
    evidence_path = (
        ROOT
        / "migration"
        / "gates"
        / "evidence"
        / "0004-server-only-soak-backup"
        / "soak-backup-status.json"
    )
    if not evidence_path.is_file():
        raise GateError("Curated Gate 4 PASS evidence is missing")
    evidence = json.loads(evidence_path.read_text(encoding="utf-8"))
    if evidence.get("result") != "PASS" or not evidence.get("gate_eligible"):
        raise GateError("Curated Gate 4 evidence is not gate-eligible")
    backup = evidence.get("backup", {})
    if not isinstance(backup, dict) or not backup.get("retained"):
        raise GateError("Gate 4 evidence does not identify a retained backup")
    relative = backup.get("relative_directory")
    if not isinstance(relative, str) or not relative.startswith("run/private/gate4/"):
        raise GateError("Gate 4 backup path is outside protected local storage")
    dump = ROOT / relative / "living-azeroth-gate4.sql"
    manifest = ROOT / relative / "manifest.json"
    if not dump.is_file() or not manifest.is_file():
        raise GateError("Retained Gate 4 backup or manifest is missing")
    expected_size = int(backup.get("bytes", -1))
    expected_hash = str(backup.get("sha256", ""))
    if dump.stat().st_size != expected_size or sha256_file(dump) != expected_hash:
        raise GateError("Retained Gate 4 backup size or hash changed")
    private_manifest = json.loads(manifest.read_text(encoding="utf-8"))
    if (
        private_manifest.get("result") != "PASS"
        or private_manifest.get("sha256") != expected_hash
        or int(private_manifest.get("bytes", -1)) != expected_size
    ):
        raise GateError("Retained Gate 4 private manifest disagrees with curated evidence")
    return {
        "result": "PASS",
        "backup_bytes": expected_size,
        "backup_sha256": expected_hash,
        "relative_directory": relative,
        "contents_recorded_in_git": False,
    }


def require_gate5_baseline() -> dict[str, object]:
    evidence_path = (
        ROOT
        / "migration"
        / "gates"
        / "evidence"
        / "0005-tailscale-client-smoke"
        / "client-smoke-status.json"
    )
    if not evidence_path.is_file():
        raise GateError("Curated Gate 5 PASS evidence is missing")
    evidence = json.loads(evidence_path.read_text(encoding="utf-8"))
    if (
        evidence.get("result") != "PASS"
        or not evidence.get("gate_eligible")
        or evidence.get("change_id") != GATE5_CHANGE_ID
    ):
        raise GateError("Curated Gate 5 evidence is not gate-eligible")
    player = evidence.get("player_observation")
    client = evidence.get("client_attestation")
    cleanup = evidence.get("cleanup")
    security = evidence.get("security")
    runtime = evidence.get("runtime_image")
    if not all(isinstance(value, dict) for value in (player, client, cleanup, security, runtime)):
        raise GateError("Curated Gate 5 evidence is incomplete")
    assert isinstance(player, dict)
    assert isinstance(client, dict)
    assert isinstance(cleanup, dict)
    assert isinstance(security, dict)
    assert isinstance(runtime, dict)
    if (
        player.get("result") != "PASS"
        or player.get("maximum_online_characters") != 1
        or not player.get("logout_observed")
        or not client.get("realmlist_restored")
        or client.get("client_exit_code") != 0
        or not cleanup.get("runtime_containers_removed")
        or not cleanup.get("serve_empty")
        or not cleanup.get("funnel_empty")
        or security.get("tailscale_ipv4") != TAILSCALE_ADDRESS
    ):
        raise GateError("Curated Gate 5 acceptance evidence is incomplete")
    required_runtime_fields = (
        "identity",
        "source_commit",
        "authserver_sha256",
        "worldserver_sha256",
    )
    if any(not runtime.get(field) for field in required_runtime_fields):
        raise GateError("Curated Gate 5 runtime identity is incomplete")
    return {
        "result": "PASS",
        "accepted_run": evidence.get("accepted_run"),
        "tailscale_ipv4": TAILSCALE_ADDRESS,
        "runtime_image": {field: runtime[field] for field in required_runtime_fields},
        "player_identity_recorded": False,
        "credentials_recorded": False,
    }


def inspect_runtime_image(image: str, log) -> dict[str, str]:
    details = json.loads(
        run_capture(["docker", "image", "inspect", image], log, record_output=False).stdout
    )[0]
    image_id = str(details.get("Id", ""))
    architecture = str(details.get("Architecture", ""))
    operating_system = str(details.get("Os", ""))
    labels = details.get("Config", {}).get("Labels", {}) or {}
    binaries = run_capture(
        [
            "docker",
            "run",
            "--rm",
            "--network",
            "none",
            "--entrypoint",
            "sha256sum",
            image,
            "/azerothcore/dist/bin/authserver",
            "/azerothcore/dist/bin/worldserver",
        ],
        log,
    ).stdout.strip().splitlines()
    if len(binaries) != 2:
        raise GateError(f"Could not read both server hashes from {image}")
    return {
        "identity": f"{image_id}|{architecture}/{operating_system}",
        "authserver_sha256": binaries[0].split()[0],
        "worldserver_sha256": binaries[1].split()[0],
        "source_commit": str(labels.get("org.opencontainers.image.revision", "")),
    }


def verify_operational_runtime_image(
    gate5_baseline: dict[str, object], log
) -> dict[str, object]:
    actual = inspect_runtime_image(OPERATIONAL_RUNTIME_IMAGE, log)
    expected = dict(gate5_baseline["runtime_image"])
    for field in ("authserver_sha256", "worldserver_sha256"):
        if actual[field] != expected[field]:
            raise GateError(
                f"Gate 6 {field} differs from the accepted Gate 5 runtime"
            )
    if not actual["source_commit"]:
        raise GateError("Gate 6 runtime image has no source revision label")
    return {
        **actual,
        "binary_hashes_match_gate5": True,
        "gate5_image_identity": str(expected["identity"]),
    }


def verify_promoted_runtime_image(
    gate6_baseline: dict[str, object], log
) -> dict[str, str]:
    actual = inspect_runtime_image(OPERATIONAL_RUNTIME_IMAGE, log)
    expected = gate6_baseline.get("runtime_image")
    if not isinstance(expected, dict):
        raise GateError("Gate 6 evidence has no runtime image identity")
    required = (
        "identity",
        "source_commit",
        "authserver_sha256",
        "worldserver_sha256",
    )
    differences = {
        field: {"expected": expected.get(field), "actual": actual.get(field)}
        for field in required
        if expected.get(field) != actual.get(field)
    }
    if differences:
        raise GateError(f"Operational runtime image differs from Gate 6: {differences}")
    return actual


def verify_gate8_runtime_image(
    gate6_baseline: dict[str, object], ai_profile: str, log
) -> dict[str, object]:
    """Pin each AI profile to the exact image that passed its disposable soak."""
    if ai_profile == "bots-ollama-routed-small":
        image = GATE10_RUNTIME_IMAGE
        expected_id = GATE10_RUNTIME_IMAGE_ID
        expected_revision = GATE10_RUNTIME_SOURCE_REVISION
        expected_hashes = {
            "authserver_sha256": GATE10_RUNTIME_AUTHSERVER_SHA256,
            "worldserver_sha256": GATE10_RUNTIME_WORLDSERVER_SHA256,
        }
        accepted_run = GATE10_ACCEPTED_RUN
    elif ai_profile == "bots-ollama-dialogue-small":
        image = GATE9_RUNTIME_IMAGE
        expected_id = GATE9_RUNTIME_IMAGE_ID
        expected_revision = GATE9_RUNTIME_SOURCE_REVISION
        expected_hashes = {
            "authserver_sha256": GATE9_RUNTIME_AUTHSERVER_SHA256,
            "worldserver_sha256": GATE9_RUNTIME_WORLDSERVER_SHA256,
        }
        accepted_run = GATE9_ACCEPTED_RUN
    elif ai_profile == "bots-local-dialogue-small":
        image = GATE8_LOCAL_RUNTIME_IMAGE
        expected_id = GATE8_LOCAL_RUNTIME_IMAGE_ID
        expected_revision = GATE8_LOCAL_RUNTIME_SOURCE_REVISION
        expected_hashes = {
            "authserver_sha256": GATE8_LOCAL_RUNTIME_AUTHSERVER_SHA256,
            "worldserver_sha256": GATE8_LOCAL_RUNTIME_WORLDSERVER_SHA256,
        }
        accepted_run = GATE8_LOCAL_ACCEPTED_RUN
    elif ai_profile in {"bots-small", "bots-dialogue-small"}:
        image = GATE8_RUNTIME_IMAGE
        expected_id = GATE8_RUNTIME_IMAGE_ID
        expected_revision = GATE8_RUNTIME_SOURCE_REVISION
        expected_hashes = {
            "authserver_sha256": gate6_baseline.get("runtime_image", {}).get("authserver_sha256"),
            "worldserver_sha256": gate6_baseline.get("runtime_image", {}).get("worldserver_sha256"),
        }
        accepted_run = None
    else:
        raise GateError(f"Unsupported AI runtime profile: {ai_profile}")
    actual = inspect_runtime_image(image, log)
    expected = gate6_baseline.get("runtime_image")
    if not isinstance(expected, dict):
        raise GateError("Gate 6 evidence has no runtime image identity")
    if actual["identity"] != f"{expected_id}|amd64/linux":
        raise GateError("AI runtime image identity differs from the accepted candidate")
    if actual["source_commit"] != expected_revision:
        raise GateError("AI runtime image source revision differs from the accepted candidate")
    for field in ("authserver_sha256", "worldserver_sha256"):
        if actual[field] != expected_hashes[field]:
            raise GateError(f"AI runtime {field} differs from the accepted candidate")
    return {
        **actual,
        "image": image,
        "candidate_identity_verified": True,
        "binary_hashes_match_accepted_candidate": True,
        "binary_hashes_match_gate6": all(
            actual[field] == expected.get(field)
            for field in ("authserver_sha256", "worldserver_sha256")
        ),
        "accepted_run": accepted_run,
    }


def build_operational_runtime_image(
    gate5_baseline: dict[str, object], log
) -> dict[str, object]:
    print("GATE6_BUILD: building and verifying the operational runtime image", flush=True)
    revision = run_capture(["git", "rev-parse", "HEAD"], log).stdout.strip()
    run_capture(
        [
            "docker",
            "build",
            "--pull=false",
            "--network",
            "none",
            "--file",
            "deploy/Dockerfile.gate6",
            "--build-arg",
            f"SOURCE_REVISION={revision}",
            "--tag",
            OPERATIONAL_RUNTIME_IMAGE,
            "deploy",
        ],
        log,
    )
    return verify_operational_runtime_image(gate5_baseline, log)


def require_gate6_backup_root(path: Path) -> Path:
    resolved = path.resolve()
    if not resolved.is_dir() or path.is_symlink():
        raise GateError(f"Gate 6 backup directory is missing or unsafe: {path}")
    metadata = resolved.stat()
    if metadata.st_uid != os.getuid():
        raise GateError("Gate 6 backup directory is not owned by the local operator")
    if stat.S_IMODE(metadata.st_mode) != 0o700:
        raise GateError("Gate 6 backup directory must have mode 0700")
    if not os.access(resolved, os.R_OK | os.W_OK | os.X_OK):
        raise GateError("Gate 6 backup directory is not accessible to the local operator")
    return resolved


def load_runtime_state() -> dict[str, object] | None:
    try:
        value = json.loads(GATE6_RUNTIME_STATE.read_text(encoding="utf-8"))
    except FileNotFoundError:
        return None
    except (OSError, json.JSONDecodeError) as exc:
        raise GateError(f"Gate 6 runtime state is unreadable: {exc}") from exc
    if not isinstance(value, dict) or value.get("schema_version") != 1:
        raise GateError("Gate 6 runtime state has an unsupported format")
    return value


def tailscale_json(command: list[str], log) -> dict[str, object]:
    process = run_capture(command, log, record_output=False)
    try:
        value = json.loads(process.stdout)
    except json.JSONDecodeError as exc:
        raise GateError(f"{shlex.join(command)} did not return JSON") from exc
    if not isinstance(value, dict):
        raise GateError(f"{shlex.join(command)} did not return a JSON object")
    return value


def tailscale_funnel_enabled(config: dict[str, object]) -> bool:
    """Funnel status returns the shared Serve config; only AllowFunnel enables it."""
    return bool(config.get("AllowFunnel"))


def require_empty_tailscale_transport(log) -> dict[str, object]:
    status = tailscale_json(["tailscale", "status", "--json"], log)
    self_value = status.get("Self")
    if status.get("BackendState") != "Running" or not isinstance(self_value, dict):
        raise GateError("Host Tailscale backend is not Running")
    hostname = self_value.get("HostName")
    addresses = self_value.get("TailscaleIPs", [])
    if hostname != TAILSCALE_HOSTNAME or TAILSCALE_ADDRESS not in addresses:
        raise GateError(
            "Host Tailscale identity does not match the approved Gate 5 endpoint"
        )
    serve = tailscale_json(["tailscale", "serve", "status", "--json"], log)
    funnel = tailscale_json(["tailscale", "funnel", "status", "--json"], log)
    if serve:
        raise GateError("Refusing to replace a pre-existing Tailscale Serve configuration")
    if tailscale_funnel_enabled(funnel):
        raise GateError("Tailscale Funnel must be empty before Gate 5")
    return {
        "backend_state": "Running",
        "hostname": hostname,
        "addresses": addresses,
        "serve_preexisting": False,
        "funnel_preexisting": False,
    }


def configure_gate5_tailscale(
    auth_backend: str, world_backend: str, log
) -> dict[str, object]:
    run_capture(
        [
            "tailscale",
            "serve",
            "--bg",
            "--tcp",
            "3724",
            f"tcp://{auth_backend}",
        ],
        log,
    )
    run_capture(
        [
            "tailscale",
            "serve",
            "--bg",
            "--tcp",
            "8085",
            f"tcp://{world_backend}",
        ],
        log,
    )
    serve = tailscale_json(["tailscale", "serve", "status", "--json"], log)
    encoded = json.dumps(serve, sort_keys=True)
    required = (
        "3724",
        auth_backend,
        "8085",
        world_backend,
    )
    if any(value not in encoded for value in required):
        raise GateError("Tailscale Serve does not contain both exact Gate 5 forwards")
    funnel = tailscale_json(["tailscale", "funnel", "status", "--json"], log)
    if tailscale_funnel_enabled(funnel):
        raise GateError("Tailscale Funnel became enabled during Gate 5")
    return {
        "tailnet_tcp_ports": [3724, 8085],
        "internal_container_backends": [auth_backend, world_backend],
        "funnel_enabled": False,
    }


def reset_gate5_tailscale(log) -> dict[str, object]:
    process = run_capture(["tailscale", "serve", "reset"], log, check=False)
    serve = tailscale_json(["tailscale", "serve", "status", "--json"], log)
    funnel = tailscale_json(["tailscale", "funnel", "status", "--json"], log)
    if process.returncode != 0 or serve or tailscale_funnel_enabled(funnel):
        raise GateError("Gate 5 Tailscale Serve cleanup did not reach an empty state")
    return {"serve_empty": True, "funnel_empty": True}


def read_realm_route(log) -> dict[str, object]:
    columns = set(
        mysql_query(
            "SELECT column_name FROM information_schema.columns "
            "WHERE table_schema='la_ahfinal11_auth' AND table_name='realmlist'",
            log,
        ).splitlines()
    )
    if not {"id", "address", "port"}.issubset(columns):
        raise GateError("Realm table does not have the required routing columns")
    rows = mysql_query(
        "SELECT id,address,port FROM la_ahfinal11_auth.realmlist ORDER BY id", log
    ).splitlines()
    if len(rows) != 1:
        raise GateError(f"Gate 5 requires exactly one realm row; found {len(rows)}")
    fields = rows[0].split("\t")
    if len(fields) != 3:
        raise GateError("Could not parse the realm routing row")
    try:
        return {"id": int(fields[0]), "address": fields[1], "port": int(fields[2])}
    except ValueError as exc:
        raise GateError("Realm routing row contains invalid numeric fields") from exc


def mysql_literal(value: str) -> str:
    return "'" + value.replace("\\", "\\\\").replace("'", "''") + "'"


def write_realm_route(route: dict[str, object], log) -> None:
    realm_id = int(route["id"])
    address = mysql_literal(str(route["address"]))
    port = int(route["port"])
    mysql_query(
        "UPDATE la_ahfinal11_auth.realmlist "
        f"SET address={address},port={port} WHERE id={realm_id}; "
        "SELECT ROW_COUNT()",
        log,
    )
    actual = read_realm_route(log)
    if actual != {"id": realm_id, "address": str(route["address"]), "port": port}:
        raise GateError(f"Realm route verification failed: {actual}")


def assert_log_health(output: str, role: str) -> list[str]:
    lowered = output.lower()
    hits = [pattern for pattern in FATAL_PATTERNS if pattern in lowered]
    if hits:
        raise GateError(f"{role} log contains fatal patterns: {hits}")
    return hits


def run_boot(
    number: int,
    expected_metrics: dict[str, int],
    root_secret: Path,
    app_secret: Path,
    evidence: Path,
    stable_seconds: int,
    boot_timeout: int,
    secret_values: list[str],
    log,
) -> dict[str, object]:
    started = utc_now()
    start_database(root_secret, app_secret, log)
    before = collect_metrics(log)
    validate_structural_metrics(before, expected_metrics, require_offline=True)

    start_server("authserver", app_secret, log)
    wait_server(AUTH_CONTAINER, 3724, "Added realm", log, min(boot_timeout, 180))
    auth_security = assert_no_host_bindings(AUTH_CONTAINER, log)

    start_server("worldserver", app_secret, log)
    wait_server(
        WORLD_CONTAINER, 8085, "WORLD: World Initialized In", log, boot_timeout
    )
    world_security = assert_no_host_bindings(WORLD_CONTAINER, log)
    if host_protected_listeners():
        raise GateError("A protected WoW/MySQL port became visible on the host")

    time.sleep(stable_seconds)
    if container_listener_count(AUTH_CONTAINER, 3724, log) != 1:
        raise GateError("Authserver did not retain exactly one internal listener")
    if container_listener_count(WORLD_CONTAINER, 8085, log) != 1:
        raise GateError("Worldserver did not retain exactly one internal listener")

    auth_log = capture_container_log(
        AUTH_CONTAINER, evidence / f"boot-{number}-authserver.log", secret_values, log
    )
    world_log = capture_container_log(
        WORLD_CONTAINER, evidence / f"boot-{number}-worldserver.log", secret_values, log
    )
    assert_log_health(auth_log, "authserver")
    assert_log_health(world_log, "worldserver")

    world_exit = stop_container(WORLD_CONTAINER, 120, log)
    auth_exit = stop_container(AUTH_CONTAINER, 60, log)
    auth_log = capture_container_log(
        AUTH_CONTAINER, evidence / f"boot-{number}-authserver.log", secret_values, log
    )
    world_log = capture_container_log(
        WORLD_CONTAINER, evidence / f"boot-{number}-worldserver.log", secret_values, log
    )
    assert_log_health(auth_log, "authserver")
    assert_log_health(world_log, "worldserver")
    if world_exit != 0 or auth_exit != 0:
        raise GateError(
            f"Server stop was not graceful: world={world_exit}, auth={auth_exit}"
        )
    if not remove_container(WORLD_CONTAINER, log) or not remove_container(
        AUTH_CONTAINER, log
    ):
        raise GateError("Server container cleanup failed")

    after = collect_metrics(log)
    validate_structural_metrics(after, expected_metrics, require_offline=True)
    database_exit = stop_container(DATABASE_CONTAINER, 60, log)
    if database_exit != 0:
        raise GateError(f"Database stop was not graceful: {database_exit}")
    if not remove_container(DATABASE_CONTAINER, log):
        raise GateError("Database container cleanup failed")
    if host_protected_listeners():
        raise GateError("A protected port remained visible after server shutdown")

    return {
        "boot": number,
        "started_utc": started,
        "finished_utc": utc_now(),
        "stable_seconds": stable_seconds,
        "database_metrics_before": before,
        "database_metrics_after": after,
        "auction_delta": after["auctions"] - before["auctions"],
        "auth": {
            "internal_listener": "3724/tcp",
            "readiness_marker": "Added realm",
            "exit_code": auth_exit,
            "security": auth_security,
            "fatal_log_hits": [],
        },
        "world": {
            "internal_listener": "8085/tcp",
            "readiness_marker": "WORLD: World Initialized In",
            "exit_code": world_exit,
            "security": world_security,
            "fatal_log_hits": [],
        },
        "database_exit_code": database_exit,
        "host_protected_listeners": [],
    }


def cleanup_runtime(log) -> dict[str, object]:
    exits: dict[str, int] = {}
    removed: dict[str, bool] = {}
    for container, timeout in (
        (WORLD_CONTAINER, 120),
        (AUTH_CONTAINER, 60),
        (DATABASE_CONTAINER, 60),
    ):
        exits[container] = stop_container(container, timeout, log)
        removed[container] = remove_container(container, log)
    ai_network_removed = remove_ollama_network(log)
    network_removed = remove_network(log)
    return {
        "exit_codes": exits,
        "containers_removed": removed,
        "network_removed": network_removed,
        "ai_network_removed": ai_network_removed,
        "persistent_database_volume_retained": docker_object_exists(
            "volume", DATABASE_VOLUME, log
        ),
        "persistent_data_volume_retained": docker_object_exists("volume", DATA_VOLUME, log),
        "host_protected_listeners": host_protected_listeners(),
    }


def gate_test(
    manifest: dict[str, object],
    root_secret: Path,
    app_secret: Path,
    evidence: Path,
    stable_seconds: int,
    boot_timeout: int,
    log,
) -> dict[str, object]:
    if host_protected_listeners():
        raise GateError("Protected MySQL/WoW host ports must be closed before Gate 3")
    expected_metrics = {
        name: int(value) for name, value in dict(manifest.get("metrics", {})).items()
    }
    secret_values = [
        root_secret.read_text(encoding="utf-8").strip(),
        app_secret.read_text(encoding="utf-8").strip(),
    ]
    create_network(log)
    boots = []
    try:
        for number in (1, 2):
            boots.append(
                run_boot(
                    number,
                    expected_metrics,
                    root_secret,
                    app_secret,
                    evidence,
                    stable_seconds,
                    boot_timeout,
                    secret_values,
                    log,
                )
            )
        if not remove_network(log):
            raise GateError("Gate 3 internal network remained after two-boot test")
        return {"result": "PASS", "boots": boots}
    except Exception:
        for container in CONTAINERS:
            if docker_object_exists("container", container, log):
                try:
                    capture_container_log(
                        container,
                        evidence / f"failure-{container}.log",
                        secret_values,
                        log,
                    )
                except GateError:
                    pass
        raise


def container_is_running(container: str, log) -> bool:
    if not docker_object_exists("container", container, log):
        return False
    return (
        run_capture(
            ["docker", "inspect", "--format", "{{.State.Running}}", container],
            log,
            check=False,
            record_output=False,
        ).stdout.strip()
        == "true"
    )


def gate4_health_checkpoint(
    elapsed_seconds: int, expected_metrics: dict[str, int], log
) -> dict[str, object]:
    for container in CONTAINERS:
        if not container_is_running(container, log):
            raise GateError(f"{container} stopped during the Gate 4 soak")
    auth_listener = container_listener_count(AUTH_CONTAINER, 3724, log)
    world_listener = container_listener_count(WORLD_CONTAINER, 8085, log)
    if auth_listener != 1 or world_listener != 1:
        raise GateError(
            "Gate 4 internal listener check failed: "
            f"auth={auth_listener}, world={world_listener}"
        )
    listeners = host_protected_listeners()
    if listeners:
        raise GateError(f"Protected host ports opened during Gate 4: {listeners}")
    metrics = collect_metrics(log)
    validate_structural_metrics(metrics, expected_metrics, require_offline=True)
    return {
        "elapsed_seconds": elapsed_seconds,
        "auth_internal_listeners": auth_listener,
        "world_internal_listeners": world_listener,
        "host_protected_listeners": [],
        "database_metrics": metrics,
    }


def create_gate4_backup(
    backup_root: Path,
    expected_metrics: dict[str, int],
    secret_values: list[str],
    log,
) -> dict[str, object]:
    stamp = dt.datetime.now(dt.UTC).strftime("%Y%m%d-%H%M%SZ")
    backup_dir = backup_root / f"{stamp}-live-logical"
    backup_dir.mkdir(parents=True, exist_ok=False, mode=0o700)
    os.chmod(backup_dir, 0o700)
    partial = backup_dir / "living-azeroth-gate4.sql.partial"
    dump = backup_dir / "living-azeroth-gate4.sql"
    manifest_path = backup_dir / "manifest.json"

    before = collect_metrics(log)
    validate_structural_metrics(before, expected_metrics, require_offline=True)
    engines = collect_storage_engines(log)
    command = docker_root_exec(
        DATABASE_CONTAINER,
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
            *SCHEMAS,
        ],
    )
    stderr = run_to_file(command, partial, log)
    if any(value and value in stderr for value in secret_values):
        raise GateError("A generated secret appeared in mysqldump diagnostics")
    if not has_dump_completion_marker(partial):
        raise GateError("Gate 4 logical backup completion marker is missing")
    partial.replace(dump)
    os.chmod(dump, 0o600)

    after = collect_metrics(log)
    validate_structural_metrics(after, before, require_offline=False)
    backup_sha256 = sha256_file(dump)
    manifest = {
        "result": "PASS",
        "change_id": GATE4_CHANGE_ID,
        "created_utc": utc_now(),
        "dump_file": dump.name,
        "bytes": dump.stat().st_size,
        "sha256": backup_sha256,
        "schemas": list(SCHEMAS),
        "metrics": before,
        "storage_engines": engines,
        "consistency": (
            "mysqldump --lock-all-tables global read lock while isolated "
            "servers were live"
        ),
        "credentials_recorded": False,
        "player_access_enabled": False,
    }
    write_private_json(manifest_path, manifest)
    return {
        "result": "PASS",
        "relative_directory": str(backup_dir.relative_to(ROOT)),
        "dump_file": dump.name,
        "bytes": manifest["bytes"],
        "sha256": backup_sha256,
        "metrics": before,
        "storage_engines": engines,
        "consistency": manifest["consistency"],
        "file_mode": "0600",
        "manifest_mode": "0600",
        "contents_recorded_in_git": False,
    }


def create_gate6_backup(
    backup_root: Path,
    expected_metrics: dict[str, int],
    secret_values: list[str],
    player_access_enabled: bool,
    log,
) -> dict[str, object]:
    backup_root = require_gate6_backup_root(backup_root)
    stamp = dt.datetime.now(dt.UTC).strftime("%Y%m%d-%H%M%SZ")
    backup_dir = backup_root / f"{stamp}-logical"
    backup_dir.mkdir(mode=0o700)
    os.chmod(backup_dir, 0o700)
    partial = backup_dir / "living-azeroth.sql.partial"
    dump = backup_dir / "living-azeroth.sql"
    manifest_path = backup_dir / "manifest.json"

    before = collect_metrics(log)
    validate_structural_metrics(before, expected_metrics, require_offline=False)
    engines = collect_storage_engines(log)
    command = docker_root_exec(
        DATABASE_CONTAINER,
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
            *SCHEMAS,
        ],
    )
    stderr = run_to_file(command, partial, log)
    if any(value and value in stderr for value in secret_values):
        raise GateError("A generated secret appeared in Gate 6 backup diagnostics")
    if not has_dump_completion_marker(partial):
        raise GateError("Gate 6 logical backup completion marker is missing")
    partial.replace(dump)
    os.chmod(dump, 0o600)

    after = collect_metrics(log)
    if after != before:
        raise GateError(f"Database metrics changed across live backup: {before} != {after}")
    backup_sha256 = sha256_file(dump)
    manifest = {
        "schema_version": 1,
        "result": "PASS",
        "change_id": GATE6_CHANGE_ID,
        "created_utc": utc_now(),
        "dump_file": dump.name,
        "bytes": dump.stat().st_size,
        "sha256": backup_sha256,
        "schemas": list(SCHEMAS),
        "metrics": before,
        "storage_engines": engines,
        "consistency": (
            "mysqldump --lock-all-tables global read lock while the private "
            "runtime was live"
        ),
        "player_access_enabled_during_backup": player_access_enabled,
        "credentials_recorded": False,
        "player_identity_recorded": False,
    }
    write_private_json(manifest_path, manifest)
    return {
        "result": "PASS",
        "directory": str(backup_dir),
        "dump_file": dump.name,
        "bytes": manifest["bytes"],
        "sha256": backup_sha256,
        "metrics": before,
        "storage_engines": engines,
        "consistency": manifest["consistency"],
        "player_access_enabled_during_backup": player_access_enabled,
        "file_mode": "0600",
        "manifest_mode": "0600",
        "contents_recorded_in_git": False,
    }


def validate_operational_backup_directory(
    backup_root: Path, backup_dir: Path
) -> dict[str, object]:
    """Validate one complete operational backup before retention considers it."""
    backup_root = backup_root.resolve()
    if backup_dir.is_symlink() or backup_dir.parent.resolve() != backup_root:
        raise GateError(f"Unsafe operational backup entry: {backup_dir.name}")
    try:
        dt.datetime.strptime(backup_dir.name, "%Y%m%d-%H%M%SZ-logical")
    except ValueError as exc:
        raise GateError(f"Malformed operational backup directory: {backup_dir.name}") from exc
    directory_stat = backup_dir.stat()
    if not stat.S_ISDIR(directory_stat.st_mode):
        raise GateError(f"Operational backup entry is not a directory: {backup_dir.name}")
    if (
        directory_stat.st_uid != os.getuid()
        or stat.S_IMODE(directory_stat.st_mode) != 0o700
    ):
        raise GateError(
            f"Operational backup directory ownership or mode changed: {backup_dir.name}"
        )

    manifest_path = backup_dir / "manifest.json"
    if manifest_path.is_symlink() or not manifest_path.is_file():
        raise GateError(f"Operational backup manifest is missing or unsafe: {backup_dir.name}")
    manifest_stat = manifest_path.stat()
    if manifest_stat.st_uid != os.getuid() or stat.S_IMODE(manifest_stat.st_mode) != 0o600:
        raise GateError(f"Operational backup manifest ownership or mode changed: {backup_dir.name}")
    try:
        manifest = json.loads(manifest_path.read_text(encoding="utf-8"))
    except (OSError, json.JSONDecodeError) as exc:
        raise GateError(f"Operational backup manifest is invalid: {backup_dir.name}") from exc
    if (
        manifest.get("result") != "PASS"
        or manifest.get("change_id") != GATE6_CHANGE_ID
        or manifest.get("dump_file") != "living-azeroth.sql"
        or manifest.get("credentials_recorded") is not False
        or manifest.get("player_identity_recorded") is not False
    ):
        raise GateError(f"Operational backup manifest is not eligible: {backup_dir.name}")

    dump_path = backup_dir / "living-azeroth.sql"
    if dump_path.is_symlink() or not dump_path.is_file():
        raise GateError(f"Operational backup dump is missing or unsafe: {backup_dir.name}")
    dump_stat = dump_path.stat()
    if dump_stat.st_uid != os.getuid() or stat.S_IMODE(dump_stat.st_mode) != 0o600:
        raise GateError(f"Operational backup dump ownership or mode changed: {backup_dir.name}")
    try:
        expected_bytes = int(manifest.get("bytes", -1))
    except (TypeError, ValueError) as exc:
        raise GateError(f"Operational backup byte count is invalid: {backup_dir.name}") from exc
    expected_sha256 = str(manifest.get("sha256", ""))
    if expected_bytes != dump_stat.st_size or len(expected_sha256) != 64:
        raise GateError(f"Operational backup size or hash is invalid: {backup_dir.name}")
    if sha256_file(dump_path) != expected_sha256:
        raise GateError(f"Operational backup hash changed: {backup_dir.name}")
    return {
        "directory_name": backup_dir.name,
        "bytes": expected_bytes,
        "sha256": expected_sha256,
    }


def apply_operational_backup_retention(
    backup_root: Path, retain_count: int
) -> dict[str, object]:
    """Keep recent validated backups while permanently pinning Gate 6's baseline."""
    backup_root = require_gate6_backup_root(backup_root)
    gate6 = require_gate6_baseline()
    accepted = gate6.get("backup")
    if not isinstance(accepted, dict):
        raise GateError("Gate 6 accepted backup metadata is missing")
    pinned_sha256 = str(accepted.get("sha256", ""))
    if len(pinned_sha256) != 64:
        raise GateError("Gate 6 accepted backup hash is invalid")

    validated = []
    for child in sorted(backup_root.iterdir()):
        if child.name.startswith("."):
            raise GateError(f"Unexpected hidden entry in backup root: {child.name}")
        validated.append(validate_operational_backup_directory(backup_root, child))
    validated.sort(key=lambda item: str(item["directory_name"]), reverse=True)

    pinned = [
        item
        for item in validated
        if item["directory_name"] == GATE6_ACCEPTED_BACKUP_DIRECTORY
    ]
    if len(pinned) != 1 or pinned[0]["sha256"] != pinned_sha256:
        raise GateError("The accepted Gate 6 backup is missing or changed")
    ordinary = [
        item
        for item in validated
        if item["directory_name"] != GATE6_ACCEPTED_BACKUP_DIRECTORY
    ]
    removals = ordinary[retain_count:]
    removed: list[str] = []
    for item in removals:
        target = backup_root / str(item["directory_name"])
        validate_operational_backup_directory(backup_root, target)
        shutil.rmtree(target)
        removed.append(target.name)
    return {
        "result": "PASS",
        "retain_count": retain_count,
        "validated_backup_count": len(validated),
        "pinned_gate6_backup": True,
        "removed_count": len(removed),
        "removed_directory_names": removed,
    }


def validate_gate4_restore(
    backup: dict[str, object],
    root_secret: Path,
    evidence: Path,
    secret_values: list[str],
    log,
    *,
    change_id: str = GATE4_CHANGE_ID,
    name_prefix: str = "gate4",
) -> dict[str, object]:
    suffix = dt.datetime.now(dt.UTC).strftime("%Y%m%d%H%M%S").lower()
    container = f"living-azeroth-{name_prefix}-restore-{suffix}"
    volume = f"living-azeroth-{name_prefix}-restore-{suffix}-mysql"
    if "directory" in backup:
        backup_directory = Path(str(backup["directory"]))
    else:
        backup_directory = ROOT / str(backup["relative_directory"])
    dump = backup_directory / str(backup["dump_file"])
    if sha256_file(dump) != backup["sha256"]:
        raise GateError("Gate 4 backup hash changed before restore validation")

    volume_created = False
    container_created = False
    passed = False
    try:
        run_capture(
            [
                "docker",
                "volume",
                "create",
                "--label",
                f"io.outlandswow.gate={change_id}",
                "--label",
                "io.outlandswow.purpose=disposable-restore-validation",
                volume,
            ],
            log,
        )
        volume_created = True
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
                f"type=bind,source={root_secret},target=/run/secrets/db_root_password,readonly",
                "--env",
                "MYSQL_ROOT_PASSWORD_FILE=/run/secrets/db_root_password",
                "--label",
                f"io.outlandswow.gate={change_id}",
                MYSQL_IMAGE,
                "--skip-networking=ON",
            ],
            log,
        )
        container_created = True
        wait_mysql_container(container, log)
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
            raise GateError(f"Restore validator unexpectedly has network access: {network}")

        import_command = [
            "docker",
            "exec",
            "--interactive",
            container,
            *secret_shell(
                "mysql", ["-uroot", "--binary-mode=1", "--default-character-set=utf8mb4"]
            ),
        ]
        run_from_file(import_command, dump, log)
        restored = collect_metrics(log, container)
        restored_engines = collect_storage_engines(log, container)
        if restored != backup["metrics"]:
            raise GateError(
                f"Restored backup metrics differ: {restored} != {backup['metrics']}"
            )
        if restored_engines != backup["storage_engines"]:
            raise GateError("Restored backup storage engines differ")

        first_stop = stop_container(container, 60, log)
        if first_stop != 0:
            raise GateError(f"Restore validator first stop returned {first_stop}")
        run_capture(["docker", "start", container], log)
        wait_mysql_container(container, log)
        restarted = collect_metrics(log, container)
        restarted_engines = collect_storage_engines(log, container)
        if restarted != restored or restarted_engines != restored_engines:
            raise GateError("Restored database metrics changed across MySQL restart")
        final_stop = stop_container(container, 60, log)
        if final_stop != 0:
            raise GateError(f"Restore validator final stop returned {final_stop}")

        mysql_logs = run_capture(["docker", "logs", container], log).stdout
        if any(value and value in mysql_logs for value in secret_values):
            raise GateError("A generated secret appeared in restore MySQL logs")
        (evidence / "restore-mysql.log").write_text(mysql_logs, encoding="utf-8")
        if not remove_container(container, log):
            raise GateError("Restore validator container cleanup failed")
        container_created = False
        run_capture(["docker", "volume", "rm", "--force", volume], log)
        if docker_object_exists("volume", volume, log):
            raise GateError("Restore validator volume cleanup failed")
        volume_created = False
        passed = True
        return {
            "result": "PASS",
            "network_mode": "none",
            "published_ports": [],
            "metrics": restored,
            "restart_metrics": restarted,
            "storage_engines": restored_engines,
            "first_stop_exit_code": first_stop,
            "final_stop_exit_code": final_stop,
            "container_removed": True,
            "volume_removed": True,
        }
    finally:
        if container_created:
            stop_container(container, 60, log)
            remove_container(container, log)
        if volume_created and passed:
            run_capture(["docker", "volume", "rm", "--force", volume], log, check=False)


def assert_gate4_module_controls(world_log: str) -> None:
    required = (
        "Playerbots Module is disabled in playerbots.conf",
        "AHBot: disabled by configuration",
        "[Ollama Chat] Config loaded: Enabled = false",
        "[Ollama Chat Social] configured enabled=false",
        "[Ollama Chat Telemetry] disabled_by_config",
    )
    missing = [marker for marker in required if marker not in world_log]
    if missing:
        raise GateError(f"Gate 4 disabled-module evidence is missing: {missing}")


def assert_operational_ai_controls(world_log: str, ai_profile: str) -> None:
    if ai_profile == "disabled":
        assert_gate4_module_controls(world_log)
        return
    if ai_profile not in {
        "bots-small",
        "bots-dialogue-small",
        "bots-local-dialogue-small",
        "bots-ollama-dialogue-small",
        "bots-ollama-routed-small",
    }:
        raise GateError(f"Unsupported operational AI profile: {ai_profile}")
    required = [
        (
            f"AI_PROFILE: name={ai_profile} playerbot_limit=10 "
            "autonomous_world_systems=off"
        ),
        "mod-playerbots initialized",
        "AHBot: disabled by configuration",
        "[Ollama Chat Social] configured enabled=false",
        "[Ollama Chat Telemetry] disabled_by_config",
    ]
    if profile_uses_ollama(ai_profile):
        required.extend(
            (
                "Living Azeroth Chat v2 config: enabled=false",
                "[Ollama Chat] Config loaded: Enabled = true",
                f"Model = {GATE9_OLLAMA_MODEL}",
                "Scheduler started: workers=1, max_queue=8, per_player=1, per_bot=1",
            )
        )
        if ai_profile == "bots-ollama-routed-small":
            required.append(
                "[Ollama Chat] Gate10 routing: deterministic=true "
                "bounded_context=true reply_word_cap=15"
            )
    else:
        required.append("[Ollama Chat] Config loaded: Enabled = false")
    if ai_profile in {"bots-dialogue-small", "bots-local-dialogue-small"}:
        required.append(
            "Living Azeroth Chat v2 config: enabled=true world=false "
            "addressed=100 unaddressed=0 bot=0"
        )
    missing = [marker for marker in required if marker not in world_log]
    if missing:
        raise GateError(f"Gate 8 bounded-AI evidence is missing: {missing}")


def gate4_test(
    manifest: dict[str, object],
    root_secret: Path,
    app_secret: Path,
    evidence: Path,
    soak_seconds: int,
    check_interval: int,
    boot_timeout: int,
    log,
) -> dict[str, object]:
    if host_protected_listeners():
        raise GateError("Protected MySQL/WoW host ports must be closed before Gate 4")
    expected_metrics = {
        name: int(value) for name, value in dict(manifest.get("metrics", {})).items()
    }
    secret_values = [
        root_secret.read_text(encoding="utf-8").strip(),
        app_secret.read_text(encoding="utf-8").strip(),
    ]
    GATE4_PRIVATE_ROOT.mkdir(parents=True, exist_ok=True)
    os.chmod(GATE4_PRIVATE_ROOT, 0o700)
    backup_root = GATE4_PRIVATE_ROOT / "backups"
    backup_root.mkdir(parents=True, exist_ok=True)
    os.chmod(backup_root, 0o700)

    create_network(log)
    checkpoints: list[dict[str, object]] = []
    backup: dict[str, object] | None = None
    started_monotonic = 0.0
    observed_soak_seconds = 0
    try:
        start_database(root_secret, app_secret, log)
        baseline = collect_metrics(log)
        validate_structural_metrics(baseline, expected_metrics, require_offline=True)
        start_server("authserver", app_secret, log)
        wait_server(AUTH_CONTAINER, 3724, "Added realm", log, min(boot_timeout, 180))
        auth_security = assert_no_host_bindings(AUTH_CONTAINER, log)
        start_server("worldserver", app_secret, log)
        wait_server(
            WORLD_CONTAINER, 8085, "WORLD: World Initialized In", log, boot_timeout
        )
        world_security = assert_no_host_bindings(WORLD_CONTAINER, log)
        started_monotonic = time.monotonic()

        backup_after = min(60, max(check_interval, soak_seconds // 5))
        while True:
            elapsed = int(time.monotonic() - started_monotonic)
            checkpoints.append(gate4_health_checkpoint(elapsed, expected_metrics, log))
            if backup is None and elapsed >= backup_after:
                backup = create_gate4_backup(
                    backup_root, expected_metrics, secret_values, log
                )
            elapsed = int(time.monotonic() - started_monotonic)
            if elapsed >= soak_seconds:
                break
            time.sleep(min(check_interval, soak_seconds - elapsed))
        observed_soak_seconds = int(time.monotonic() - started_monotonic)
        if backup is None:
            backup = create_gate4_backup(
                backup_root, expected_metrics, secret_values, log
            )

        auth_log = capture_container_log(
            AUTH_CONTAINER, evidence / "soak-authserver.log", secret_values, log
        )
        world_log = capture_container_log(
            WORLD_CONTAINER, evidence / "soak-worldserver.log", secret_values, log
        )
        assert_log_health(auth_log, "authserver")
        assert_log_health(world_log, "worldserver")
        assert_gate4_module_controls(world_log)

        world_exit = stop_container(WORLD_CONTAINER, 120, log)
        auth_exit = stop_container(AUTH_CONTAINER, 60, log)
        auth_log = capture_container_log(
            AUTH_CONTAINER, evidence / "soak-authserver.log", secret_values, log
        )
        world_log = capture_container_log(
            WORLD_CONTAINER, evidence / "soak-worldserver.log", secret_values, log
        )
        assert_log_health(auth_log, "authserver")
        assert_log_health(world_log, "worldserver")
        assert_gate4_module_controls(world_log)
        if world_exit != 0 or auth_exit != 0:
            raise GateError(
                f"Gate 4 server stop was not graceful: world={world_exit}, auth={auth_exit}"
            )
        if not remove_container(WORLD_CONTAINER, log) or not remove_container(
            AUTH_CONTAINER, log
        ):
            raise GateError("Gate 4 server container cleanup failed")

        final_metrics = collect_metrics(log)
        if final_metrics != backup["metrics"]:
            raise GateError(
                f"Source metrics changed after Gate 4 soak: {final_metrics} != {backup['metrics']}"
            )
        database_exit = stop_container(DATABASE_CONTAINER, 60, log)
        if database_exit != 0:
            raise GateError(f"Gate 4 database stop returned {database_exit}")
        if not remove_container(DATABASE_CONTAINER, log):
            raise GateError("Gate 4 database container cleanup failed")
        if not remove_network(log):
            raise GateError("Gate 4 internal network cleanup failed")

        restore = validate_gate4_restore(
            backup, root_secret, evidence, secret_values, log
        )
        if host_protected_listeners():
            raise GateError("A protected host listener remained after Gate 4")
        return {
            "result": "PASS",
            "soak_seconds": observed_soak_seconds,
            "declared_soak_seconds": soak_seconds,
            "check_interval_seconds": check_interval,
            "checkpoints": checkpoints,
            "baseline_metrics": baseline,
            "final_metrics": final_metrics,
            "auth": {
                "readiness_marker": "Added realm",
                "exit_code": auth_exit,
                "security": auth_security,
            },
            "world": {
                "readiness_marker": "WORLD: World Initialized In",
                "exit_code": world_exit,
                "security": world_security,
                "disabled_module_controls_verified": True,
            },
            "database_exit_code": database_exit,
            "backup": backup,
            "restore_validation": restore,
            "host_protected_listeners": [],
        }
    except Exception:
        for container in CONTAINERS:
            if docker_object_exists("container", container, log):
                try:
                    capture_container_log(
                        container,
                        evidence / f"failure-{container}.log",
                        secret_values,
                        log,
                    )
                except GateError:
                    pass
        raise


def probe_tcp(address: str, port: int, timeout: float = 3.0) -> bool:
    try:
        with socket.create_connection((address, port), timeout=timeout):
            return True
    except OSError:
        return False


def wait_tcp_probe(address: str, port: int, timeout: int = 15) -> dict[str, object]:
    deadline = time.monotonic() + timeout
    attempts = 0
    while time.monotonic() < deadline:
        attempts += 1
        if probe_tcp(address, port):
            return {"address": address, "port": port, "attempts": attempts, "ok": True}
        time.sleep(1)
    return {"address": address, "port": port, "attempts": attempts, "ok": False}


def observe_one_player(
    log, connect_timeout: int, hold_seconds: int, logout_timeout: int
) -> dict[str, object]:
    started = time.monotonic()
    observations: list[dict[str, object]] = []
    connected_at: float | None = None
    while time.monotonic() - started <= connect_timeout:
        count = collect_metrics(log)["online_characters"]
        elapsed = int(time.monotonic() - started)
        observations.append({"elapsed_seconds": elapsed, "online_characters": count})
        if count > 1:
            raise GateError("More than one online character was observed during Gate 5")
        if count == 1:
            connected_at = time.monotonic()
            break
        time.sleep(2)
    if connected_at is None:
        raise GateError("No character entered the world before the Gate 5 timeout")

    print(
        f"GATE5_PLAYER_CONNECTED: hold exactly one player for {hold_seconds} seconds",
        flush=True,
    )
    while time.monotonic() - connected_at < hold_seconds:
        count = collect_metrics(log)["online_characters"]
        elapsed = int(time.monotonic() - started)
        observations.append({"elapsed_seconds": elapsed, "online_characters": count})
        if count != 1:
            raise GateError("The one-player observation was interrupted or exceeded")
        time.sleep(2)

    print("GATE5_LOGOUT_NOW: log out and close the WoW client", flush=True)
    logout_started = time.monotonic()
    while time.monotonic() - logout_started <= logout_timeout:
        count = collect_metrics(log)["online_characters"]
        elapsed = int(time.monotonic() - started)
        observations.append({"elapsed_seconds": elapsed, "online_characters": count})
        if count > 1:
            raise GateError("More than one online character was observed during logout")
        if count == 0:
            return {
                "result": "PASS",
                "hold_seconds": hold_seconds,
                "logout_observed": True,
                "maximum_online_characters": 1,
                "observations": observations,
                "player_identity_recorded": False,
            }
        time.sleep(2)
    raise GateError("The player did not return offline before the logout timeout")


def gate5_test(
    manifest: dict[str, object],
    root_secret: Path,
    app_secret: Path,
    evidence: Path,
    boot_timeout: int,
    connect_timeout: int,
    hold_seconds: int,
    logout_timeout: int,
    preflight_only: bool,
    log,
) -> dict[str, object]:
    if host_protected_listeners():
        raise GateError("Protected MySQL/WoW host ports must be closed before Gate 5")
    tailscale_preflight = require_empty_tailscale_transport(log)
    expected_metrics = {
        name: int(value) for name, value in dict(manifest.get("metrics", {})).items()
    }
    secret_values = [
        root_secret.read_text(encoding="utf-8").strip(),
        app_secret.read_text(encoding="utf-8").strip(),
    ]
    GATE5_PRIVATE_ROOT.mkdir(parents=True, exist_ok=True)
    os.chmod(GATE5_PRIVATE_ROOT, 0o700)
    prior_route_path = GATE5_PRIVATE_ROOT / f"{evidence.name}-prior-realm-route.json"
    prior_route: dict[str, object] | None = None
    route_changed = False
    route_changed_during_run = False
    serve_claimed = False
    passed = False
    serve_cleanup: dict[str, object] | None = None

    create_network(log)
    try:
        start_database(root_secret, app_secret, log)
        baseline = collect_metrics(log)
        validate_structural_metrics(baseline, expected_metrics, require_offline=True)

        prior_route = read_realm_route(log)
        write_private_json(prior_route_path, prior_route)
        desired_route = {
            "id": prior_route["id"],
            "address": TAILSCALE_ADDRESS,
            "port": 8085,
        }
        if prior_route != desired_route:
            write_realm_route(desired_route, log)
            route_changed = True
            route_changed_during_run = True

        start_server("authserver", app_secret, log)
        wait_server(AUTH_CONTAINER, 3724, "Added realm", log, min(boot_timeout, 180))
        auth_security = assert_no_host_bindings(AUTH_CONTAINER, log)
        start_server("worldserver", app_secret, log)
        wait_server(
            WORLD_CONTAINER, 8085, "WORLD: World Initialized In", log, boot_timeout
        )
        world_security = assert_no_host_bindings(WORLD_CONTAINER, log)
        if host_protected_listeners():
            raise GateError("A protected host port was directly published during Gate 5")
        auth_address = container_network_address(AUTH_CONTAINER, log)
        world_address = container_network_address(WORLD_CONTAINER, log)
        auth_backend = f"{auth_address}:3724"
        world_backend = f"{world_address}:8085"
        auth_backend_probe = wait_tcp_probe(auth_address, 3724)
        world_backend_probe = wait_tcp_probe(world_address, 8085)
        failed_backends = [
            result["port"]
            for result in (auth_backend_probe, world_backend_probe)
            if not result["ok"]
        ]
        if failed_backends:
            raise GateError(
                f"Gate 5 internal TCP probes failed for container ports {failed_backends}"
            )

        # Mark ownership before the first mutation so partial setup is reset.
        serve_claimed = True
        tailscale_transport = configure_gate5_tailscale(
            auth_backend, world_backend, log
        )
        print(
            "GATE5_READY: run the Windows launcher for 100.64.0.10; "
            "only one player may connect",
            flush=True,
        )

        if preflight_only:
            player_observation: dict[str, object] = {
                "result": "NOT_RUN",
                "reason": "preflight-only",
                "online_characters": collect_metrics(log)["online_characters"],
            }
            if player_observation["online_characters"] != 0:
                raise GateError("A player connected during the Gate 5 preflight")
        else:
            player_observation = observe_one_player(
                log, connect_timeout, hold_seconds, logout_timeout
            )

        auth_log = capture_container_log(
            AUTH_CONTAINER, evidence / "gate5-authserver.log", secret_values, log
        )
        world_log = capture_container_log(
            WORLD_CONTAINER, evidence / "gate5-worldserver.log", secret_values, log
        )
        assert_log_health(auth_log, "authserver")
        assert_log_health(world_log, "worldserver")
        assert_gate4_module_controls(world_log)

        serve_cleanup = reset_gate5_tailscale(log)
        serve_claimed = False
        world_exit = stop_container(WORLD_CONTAINER, 120, log)
        auth_exit = stop_container(AUTH_CONTAINER, 60, log)
        if world_exit != 0 or auth_exit != 0:
            raise GateError(
                f"Gate 5 server stop was not graceful: world={world_exit}, auth={auth_exit}"
            )
        if not remove_container(WORLD_CONTAINER, log) or not remove_container(
            AUTH_CONTAINER, log
        ):
            raise GateError("Gate 5 server container cleanup failed")
        if preflight_only and route_changed and prior_route is not None:
            write_realm_route(prior_route, log)
            route_changed = False
        final_metrics = collect_metrics(log)
        validate_structural_metrics(final_metrics, expected_metrics, require_offline=True)
        database_exit = stop_container(DATABASE_CONTAINER, 60, log)
        if database_exit != 0:
            raise GateError(f"Gate 5 database stop returned {database_exit}")
        if not remove_container(DATABASE_CONTAINER, log):
            raise GateError("Gate 5 database container cleanup failed")
        if not remove_network(log):
            raise GateError("Gate 5 internal network cleanup failed")
        if host_protected_listeners():
            raise GateError("A protected host listener remained after Gate 5")
        passed = True
        return {
            "result": "PRECHECK_PASS" if preflight_only else "PASS",
            "preflight_only": preflight_only,
            "tailscale_preflight": tailscale_preflight,
            "tailscale_transport": tailscale_transport,
            "tailscale_cleanup": serve_cleanup,
            "realm_route": {
                "address": TAILSCALE_ADDRESS,
                "port": 8085,
                "prior_route_protected_outside_git": True,
                "changed_during_run": route_changed_during_run,
                "retained": not preflight_only,
            },
            "internal_container_backends": [auth_backend, world_backend],
            "internal_backend_probes": [auth_backend_probe, world_backend_probe],
            "tailnet_self_probe": {
                "result": "NOT_APPLICABLE",
                "reason": "host cannot hairpin through its own Tailscale Serve endpoint",
                "client_probe_required": True,
            },
            "auth": {"exit_code": auth_exit, "security": auth_security},
            "world": {
                "exit_code": world_exit,
                "security": world_security,
                "disabled_module_controls_verified": True,
            },
            "database_exit_code": database_exit,
            "baseline_metrics": baseline,
            "final_metrics": final_metrics,
            "player_observation": player_observation,
            "central_acl_policy_inspected": False,
            "host_packet_filter_policy_inspected": False,
            "funnel_enabled": False,
            "player_identity_recorded": False,
        }
    except Exception:
        for container in (AUTH_CONTAINER, WORLD_CONTAINER):
            if docker_object_exists("container", container, log):
                try:
                    capture_container_log(
                        container,
                        evidence / f"failure-{container}.log",
                        secret_values,
                        log,
                    )
                except GateError:
                    pass
        raise
    finally:
        cleanup_failure: Exception | None = None
        if serve_claimed:
            try:
                reset_gate5_tailscale(log)
            except (GateError, OSError, subprocess.CalledProcessError) as exc:
                cleanup_failure = exc
        if not passed and route_changed and prior_route is not None and container_is_running(
            DATABASE_CONTAINER, log
        ):
            try:
                write_realm_route(prior_route, log)
            except (GateError, OSError, subprocess.CalledProcessError) as exc:
                cleanup_failure = cleanup_failure or exc
        if cleanup_failure is not None:
            raise cleanup_failure


def require_gate6_baseline() -> dict[str, object]:
    evidence_path = (
        ROOT
        / "migration"
        / "gates"
        / "evidence"
        / "0006-private-operations"
        / "operational-status.json"
    )
    if not evidence_path.is_file():
        raise GateError("Curated Gate 6 PASS evidence is missing")
    evidence = json.loads(evidence_path.read_text(encoding="utf-8"))
    runtime = evidence.get("runtime_image")
    backup = evidence.get("backup")
    if (
        evidence.get("result") != "PASS"
        or not evidence.get("gate_eligible")
        or evidence.get("change_id") != GATE6_CHANGE_ID
        or not isinstance(runtime, dict)
        or not isinstance(backup, dict)
        or backup.get("restore_validation_result") != "PASS"
    ):
        raise GateError("Curated Gate 6 evidence is not gate-eligible")
    return evidence


def expected_metrics_from_manifest(manifest: dict[str, object]) -> dict[str, int]:
    metrics = manifest.get("metrics")
    if not isinstance(metrics, dict):
        raise GateError("Gate 2 manifest metrics are missing")
    try:
        return {name: int(value) for name, value in metrics.items()}
    except (TypeError, ValueError) as exc:
        raise GateError("Gate 2 manifest metrics are invalid") from exc


def tailscale_transport_snapshot(log) -> dict[str, object]:
    status = tailscale_json(["tailscale", "status", "--json"], log)
    self_value = status.get("Self")
    if not isinstance(self_value, dict):
        self_value = {}
    serve = tailscale_json(["tailscale", "serve", "status", "--json"], log)
    funnel = tailscale_json(["tailscale", "funnel", "status", "--json"], log)
    tcp = serve.get("TCP", {})
    ports: list[int] = []
    if isinstance(tcp, dict):
        for value in tcp:
            try:
                ports.append(int(value))
            except (TypeError, ValueError):
                continue
    addresses = self_value.get("TailscaleIPs", [])
    if not isinstance(addresses, list):
        addresses = []
    return {
        "backend_state": status.get("BackendState"),
        "hostname": self_value.get("HostName"),
        "expected_address_present": TAILSCALE_ADDRESS in addresses,
        "serve_tcp_ports": sorted(ports),
        "serve_empty": not bool(serve),
        "funnel_enabled": tailscale_funnel_enabled(funnel),
    }


def start_gate6_stack(
    manifest: dict[str, object],
    root_secret: Path,
    app_secret: Path,
    boot_timeout: int,
    log,
    *,
    image: str = OPERATIONAL_RUNTIME_IMAGE,
    change_id: str = GATE6_CHANGE_ID,
    ai_profile: str = "disabled",
    normalize_stale_online: bool = False,
) -> dict[str, object]:
    expected_metrics = expected_metrics_from_manifest(manifest)
    create_network(log, change_id)
    ollama_boundary = (
        create_ollama_network(log) if profile_uses_ollama(ai_profile) else None
    )
    database_security = start_database(root_secret, app_secret, log, change_id)
    baseline = collect_metrics(log)
    offline_state_normalization = {
        "column": f"{SCHEMAS[1]}.characters.online",
        "before": int(baseline.get("online_characters", 0)),
        "rows_changed": 0,
        "after": int(baseline.get("online_characters", 0)),
    }
    if baseline.get("online_characters") and normalize_stale_online:
        offline_state_normalization = normalize_offline_character_state(
            int(baseline["online_characters"]), log
        )
        baseline = collect_metrics(log)
    validate_structural_metrics(baseline, expected_metrics, require_offline=True)
    route = read_realm_route(log)
    if route.get("address") != TAILSCALE_ADDRESS or route.get("port") != 8085:
        raise GateError("The retained realm route no longer matches the accepted Gate 5 route")

    start_server(
        "authserver",
        app_secret,
        log,
        image=image,
        change_id=change_id,
    )
    wait_server(AUTH_CONTAINER, 3724, "Added realm", log, min(boot_timeout, 180))
    auth_security = assert_no_host_bindings(
        AUTH_CONTAINER,
        log,
        expected_resource_budget=resource_budget_for("authserver"),
    )
    start_server(
        "worldserver",
        app_secret,
        log,
        image=image,
        change_id=change_id,
        ai_profile=ai_profile,
    )
    wait_server(WORLD_CONTAINER, 8085, "WORLD: World Initialized In", log, boot_timeout)
    world_security = assert_no_host_bindings(
        WORLD_CONTAINER,
        log,
        expected_networks=(NETWORK, AI_NETWORK)
        if profile_uses_ollama(ai_profile)
        else (NETWORK,),
        expected_resource_budget=resource_budget_for("worldserver", ai_profile),
    )
    if host_protected_listeners():
        raise GateError("A protected host port became visible during Gate 6 startup")

    auth_address = container_network_address(AUTH_CONTAINER, log)
    world_address = container_network_address(WORLD_CONTAINER, log)
    probes = [
        wait_tcp_probe(auth_address, 3724),
        wait_tcp_probe(world_address, 8085),
    ]
    if not all(result["ok"] for result in probes):
        raise GateError("Gate 6 internal auth/world TCP readiness failed")
    auth_log = run_capture(
        ["docker", "logs", "--tail", "1000", AUTH_CONTAINER],
        log,
        check=False,
        record_output=False,
    ).stdout
    world_log = run_capture(
        ["docker", "logs", "--tail", "1000", WORLD_CONTAINER],
        log,
        check=False,
        record_output=False,
    ).stdout
    assert_log_health(auth_log, "authserver")
    assert_log_health(world_log, "worldserver")
    assert_operational_ai_controls(world_log, ai_profile)
    return {
        "result": "PASS",
        "ai_profile": ai_profile,
        "offline_state_normalization": offline_state_normalization,
        "database_metrics": baseline,
        "database": {"security": database_security},
        "realm_route": {"address": TAILSCALE_ADDRESS, "port": 8085},
        "auth": {"readiness_marker": "Added realm", "security": auth_security},
        "world": {
            "readiness_marker": "WORLD: World Initialized In",
            "security": world_security,
            "disabled_module_controls_verified": True,
        },
        "internal_backend_probes": probes,
        "ollama_boundary": ollama_boundary,
        "host_protected_listeners": [],
    }


def stop_gate6_stack(
    expected_metrics: dict[str, int] | None,
    evidence: Path,
    label: str,
    secret_values: list[str],
    log,
    *,
    ai_profile: str = "disabled",
    allow_unclean_shutdown: bool = False,
    expected_change_id: str | None = None,
) -> dict[str, object]:
    evidence.mkdir(parents=True, exist_ok=True)
    initially_running = {
        container: container_is_running(container, log) for container in CONTAINERS
    }
    unclean_containers = [
        container
        for container in CONTAINERS
        if docker_object_exists("container", container, log)
        and not initially_running[container]
    ]
    if unclean_containers and allow_unclean_shutdown:
        if expected_change_id is None:
            raise GateError("Unclean runtime recovery requires an expected change id")
        for container in unclean_containers:
            details = json.loads(
                run_capture(
                    ["docker", "inspect", container], log, record_output=False
                ).stdout
            )[0]
            labels = details.get("Config", {}).get("Labels", {}) or {}
            if labels.get("io.outlandswow.gate") != expected_change_id:
                raise GateError(
                    f"Refusing unclean recovery of unowned container: {container}"
                )
    world_exit = stop_container(WORLD_CONTAINER, 120, log)
    auth_exit = stop_container(AUTH_CONTAINER, 60, log)
    for container, role in ((AUTH_CONTAINER, "authserver"), (WORLD_CONTAINER, "worldserver")):
        if docker_object_exists("container", container, log):
            output = capture_container_log(
                container, evidence / f"{label}-{role}.log", secret_values, log
            )
            assert_log_health(output, role)
            if role == "worldserver":
                assert_operational_ai_controls(output, ai_profile)
    permitted_world_exit = (
        allow_unclean_shutdown and not initially_running[WORLD_CONTAINER]
    )
    permitted_auth_exit = (
        allow_unclean_shutdown and not initially_running[AUTH_CONTAINER]
    )
    if (world_exit != 0 and not permitted_world_exit) or (
        auth_exit != 0 and not permitted_auth_exit
    ):
        raise GateError(
            f"Gate 6 server stop was not graceful: world={world_exit}, auth={auth_exit}"
        )
    remove_container(WORLD_CONTAINER, log)
    remove_container(AUTH_CONTAINER, log)
    ai_network_removed = (
        remove_ollama_network(log)
        if profile_uses_ollama(ai_profile)
        else not docker_object_exists("network", AI_NETWORK, log)
    )

    final_metrics: dict[str, int] | None = None
    if container_is_running(DATABASE_CONTAINER, log):
        final_metrics = collect_metrics(log)
        if expected_metrics is not None:
            validate_structural_metrics(
                final_metrics, expected_metrics, require_offline=True
            )
    database_exit = stop_container(DATABASE_CONTAINER, 60, log)
    permitted_database_exit = (
        allow_unclean_shutdown and not initially_running[DATABASE_CONTAINER]
    )
    if database_exit != 0 and not permitted_database_exit:
        raise GateError(f"Gate 6 database stop returned {database_exit}")
    remove_container(DATABASE_CONTAINER, log)
    network_removed = remove_network(log)
    containers_removed = {
        container: not docker_object_exists("container", container, log)
        for container in CONTAINERS
    }
    if not all(containers_removed.values()) or not network_removed or not ai_network_removed:
        raise GateError("Gate 6 container or network cleanup failed")
    listeners = host_protected_listeners()
    if listeners:
        raise GateError(f"Protected host listeners remained after Gate 6 stop: {listeners}")
    return {
        "result": "PASS",
        "unclean_shutdown_recovered": bool(unclean_containers),
        "unclean_containers": unclean_containers,
        "exit_codes": {
            "worldserver": world_exit,
            "authserver": auth_exit,
            "database": database_exit,
        },
        "final_metrics": final_metrics,
        "containers_removed": containers_removed,
        "network_removed": network_removed,
        "ai_network_removed": ai_network_removed,
        "host_protected_listeners": [],
    }


def gate6_test(
    manifest: dict[str, object],
    root_secret: Path,
    app_secret: Path,
    evidence: Path,
    backup_root: Path,
    stable_seconds: int,
    boot_timeout: int,
    log,
) -> dict[str, object]:
    if host_protected_listeners():
        raise GateError("Protected MySQL/WoW host ports must be closed before Gate 6")
    backup_root = require_gate6_backup_root(backup_root)
    tailscale_before = require_empty_tailscale_transport(log)
    expected_metrics = expected_metrics_from_manifest(manifest)
    secret_values = [
        root_secret.read_text(encoding="utf-8").strip(),
        app_secret.read_text(encoding="utf-8").strip(),
    ]

    print("GATE6_BOOT_1: starting database, auth, and world without player access", flush=True)
    first_start = start_gate6_stack(
        manifest, root_secret, app_secret, boot_timeout, log
    )
    time.sleep(stable_seconds)
    print("GATE6_BACKUP: creating the durable logical backup", flush=True)
    backup = create_gate6_backup(
        backup_root, expected_metrics, secret_values, False, log
    )
    print("GATE6_RESTORE: validating the backup in a networkless disposable database", flush=True)
    restore = validate_gate4_restore(
        backup,
        root_secret,
        evidence,
        secret_values,
        log,
        change_id=GATE6_CHANGE_ID,
        name_prefix="gate6",
    )
    print("GATE6_STOP_1: gracefully stopping the first boot", flush=True)
    first_stop = stop_gate6_stack(
        expected_metrics, evidence, "first-boot", secret_values, log
    )

    print("GATE6_BOOT_2: proving restart recovery from the retained volumes", flush=True)
    second_start = start_gate6_stack(
        manifest, root_secret, app_secret, boot_timeout, log
    )
    time.sleep(stable_seconds)
    print("GATE6_STOP_2: gracefully stopping the recovery boot", flush=True)
    second_stop = stop_gate6_stack(
        expected_metrics, evidence, "second-boot", secret_values, log
    )
    tailscale_after = tailscale_transport_snapshot(log)
    if not tailscale_after["serve_empty"] or tailscale_after["funnel_enabled"]:
        raise GateError("Tailscale transport changed during the server-only Gate 6 test")
    print("GATE6_PASS: server-only operations, backup, restore, and restart passed", flush=True)
    return {
        "result": "PASS",
        "stable_seconds_per_boot": stable_seconds,
        "first_start": first_start,
        "first_stop": first_stop,
        "backup": backup,
        "restore_validation": restore,
        "second_start": second_start,
        "second_stop": second_stop,
        "tailscale_before": tailscale_before,
        "tailscale_after": tailscale_after,
        "player_access_enabled": False,
        "player_identity_recorded": False,
        "credentials_recorded": False,
    }


def operational_start(
    manifest: dict[str, object],
    root_secret: Path,
    app_secret: Path,
    boot_timeout: int,
    player_access: bool,
    ai_profile: str,
    log,
) -> dict[str, object]:
    if ai_profile not in OPERATIONAL_AI_PROFILES:
        raise GateError(f"Unsupported operational AI profile: {ai_profile}")
    if load_runtime_state() is not None:
        raise GateError("Living Azeroth already has an operational runtime state")
    existing = [
        name
        for name in CONTAINERS
        if docker_object_exists("container", name, log)
    ]
    if (
        existing
        or docker_object_exists("network", NETWORK, log)
        or docker_object_exists("network", AI_NETWORK, log)
    ):
        raise GateError(f"Refusing to adopt unexpected Living Azeroth runtime: {existing}")
    if host_protected_listeners():
        raise GateError("Protected host ports must be closed before operational start")
    require_empty_tailscale_transport(log)
    gate6_baseline = require_gate6_baseline()
    if ai_profile == "disabled":
        image = OPERATIONAL_RUNTIME_IMAGE
        change_id = GATE6_CHANGE_ID
        runtime_image = verify_promoted_runtime_image(gate6_baseline, log)
    else:
        change_id = (
            GATE10_CHANGE_ID
            if ai_profile == "bots-ollama-routed-small"
            else GATE9_CHANGE_ID
            if profile_uses_ollama(ai_profile)
            else GATE8_CHANGE_ID
        )
        runtime_image = verify_gate8_runtime_image(gate6_baseline, ai_profile, log)
        image = str(runtime_image["image"])
        if profile_uses_ollama(ai_profile):
            verify_private_ollama(log)
    expected_metrics = expected_metrics_from_manifest(manifest)
    serve_claimed = False
    try:
        stack = start_gate6_stack(
            manifest,
            root_secret,
            app_secret,
            boot_timeout,
            log,
            image=image,
            change_id=change_id,
            ai_profile=ai_profile,
            normalize_stale_online=not player_access,
        )
        transport: dict[str, object] = {
            "enabled": False,
            "tailnet_tcp_ports": [],
            "funnel_enabled": False,
        }
        if player_access:
            auth_address = container_network_address(AUTH_CONTAINER, log)
            world_address = container_network_address(WORLD_CONTAINER, log)
            serve_claimed = True
            transport = {
                "enabled": True,
                **configure_gate5_tailscale(
                    f"{auth_address}:3724", f"{world_address}:8085", log
                ),
            }
        state = {
            "schema_version": 1,
            "change_id": change_id,
            "started_utc": utc_now(),
            "player_access_enabled": player_access,
            "tailscale_address": TAILSCALE_ADDRESS if player_access else None,
            "runtime_image": runtime_image,
            "ai_profile": ai_profile,
            "expected_metrics": expected_metrics,
            "containers": list(CONTAINERS),
            "network": NETWORK,
            "ai_network": AI_NETWORK if profile_uses_ollama(ai_profile) else None,
        }
        write_private_json(GATE6_RUNTIME_STATE, state)
        DEVELOPMENT_OFFLINE_MARKER.unlink(missing_ok=True)
        return {
            "result": "PASS",
            "stack": stack,
            "transport": transport,
            "runtime_state_written": True,
            "player_access_enabled": player_access,
            "ai_profile": ai_profile,
        }
    except Exception:
        if serve_claimed:
            reset_gate5_tailscale(log)
        cleanup_runtime(log)
        raise


def operational_stop(evidence: Path, log) -> dict[str, object]:
    state = load_runtime_state()
    existing = any(docker_object_exists("container", name, log) for name in CONTAINERS)
    existing = existing or docker_object_exists("network", NETWORK, log)
    existing = existing or docker_object_exists("network", AI_NETWORK, log)
    if state is None:
        if existing:
            raise GateError(
                "Living Azeroth containers exist without Gate 6 runtime state; refusing adoption"
            )
        return {
            "result": "PASS",
            "already_stopped": True,
            "player_access_closed": True,
        }
    if state.get("change_id") not in {
        GATE6_CHANGE_ID,
        GATE8_CHANGE_ID,
        GATE9_CHANGE_ID,
        GATE10_CHANGE_ID,
    }:
        raise GateError("Runtime state does not identify this lifecycle controller")
    ai_profile = str(state.get("ai_profile", "disabled"))
    if ai_profile not in OPERATIONAL_AI_PROFILES:
        raise GateError(f"Runtime state has unsupported AI profile: {ai_profile}")
    player_access = bool(state.get("player_access_enabled"))
    transport_cleanup: dict[str, object] = {
        "not_claimed": True,
        "serve_empty": True,
        "funnel_empty": True,
    }
    if player_access:
        transport_cleanup = reset_gate5_tailscale(log)
    expected_raw = state.get("expected_metrics")
    expected = (
        {name: int(value) for name, value in expected_raw.items()}
        if isinstance(expected_raw, dict)
        else None
    )
    secret_values = []
    for path in (
        PRIVATE_ROOT / "mysql-root-password",
        PRIVATE_ROOT / "acore-password",
    ):
        if path.is_file():
            secret_values.append(path.read_text(encoding="utf-8").strip())
    stopped = stop_gate6_stack(
        expected,
        evidence,
        "operational",
        secret_values,
        log,
        ai_profile=ai_profile,
        allow_unclean_shutdown=True,
        expected_change_id=str(state["change_id"]),
    )
    GATE6_RUNTIME_STATE.unlink(missing_ok=True)
    return {
        "result": "PASS",
        "already_stopped": False,
        "player_access_closed": True,
        "ai_profile": ai_profile,
        "transport_cleanup": transport_cleanup,
        "stack": stopped,
    }


def operational_backup(
    backup_root: Path,
    evidence: Path,
    root_secret: Path,
    retention_count: int | None,
    log,
) -> dict[str, object]:
    state = load_runtime_state()
    if state is None or not all(container_is_running(name, log) for name in CONTAINERS):
        raise GateError("A complete running Gate 6 stack is required for backup")
    expected_raw = state.get("expected_metrics")
    if not isinstance(expected_raw, dict):
        raise GateError("Gate 6 runtime state has no expected database metrics")
    expected = {name: int(value) for name, value in expected_raw.items()}
    secret_values = []
    for path in (root_secret, PRIVATE_ROOT / "acore-password"):
        if path.is_file():
            secret_values.append(path.read_text(encoding="utf-8").strip())
    backup = create_gate6_backup(
        backup_root,
        expected,
        secret_values,
        bool(state.get("player_access_enabled")),
        log,
    )
    restore = validate_gate4_restore(
        backup,
        root_secret,
        evidence,
        secret_values,
        log,
        change_id=GATE6_CHANGE_ID,
        name_prefix="gate6",
    )
    retention = None
    if retention_count is not None:
        retention = apply_operational_backup_retention(backup_root, retention_count)
    return {
        "result": "PASS",
        "backup": backup,
        "restore_validation": restore,
        "retention": retention,
    }


def status_snapshot(log) -> dict[str, object]:
    runtime_state = load_runtime_state()
    ai_profile = (
        str(runtime_state.get("ai_profile", "disabled"))
        if runtime_state
        else "disabled"
    )
    roles = {
        DATABASE_CONTAINER: "database",
        AUTH_CONTAINER: "authserver",
        WORLD_CONTAINER: "worldserver",
    }
    containers: dict[str, object] = {}
    for name in CONTAINERS:
        if not docker_object_exists("container", name, log):
            containers[name] = {"exists": False, "running": False, "status": "absent"}
            continue
        details = json.loads(
            run_capture(["docker", "inspect", name], log, record_output=False).stdout
        )[0]
        state = details.get("State", {})
        expected_resource_limits = expected_resource_limit_snapshot(
            resource_budget_for(roles[name], ai_profile)
        )
        actual_resource_limits = resource_limit_snapshot(details)
        resource_policy = (
            details.get("Config", {}).get("Labels", {}) or {}
        ).get("com.server.resource-policy")
        containers[name] = {
            "exists": True,
            "running": bool(state.get("Running")),
            "status": state.get("Status"),
            "restart_count": int(details.get("RestartCount", 0)),
            "host_port_bindings": details.get("HostConfig", {}).get("PortBindings") or {},
            "resource_policy": resource_policy,
            "resource_limits": actual_resource_limits,
            "expected_resource_limits": expected_resource_limits,
            "resource_limits_ok": (
                actual_resource_limits == expected_resource_limits
                and resource_policy == RESOURCE_POLICY_VERSION
            ),
        }
    transport = tailscale_transport_snapshot(log)
    metrics = None
    if container_is_running(DATABASE_CONTAINER, log):
        metrics = collect_metrics(log)
    ai_network_present = docker_object_exists("network", AI_NETWORK, log)
    ai_network_internal = False
    if ai_network_present:
        ai_network_internal = run_capture(
            ["docker", "network", "inspect", "--format", "{{.Internal}}", AI_NETWORK],
            log,
            check=False,
            record_output=False,
        ).stdout.strip() == "true"
    ollama_status: dict[str, object] = {
        "required": profile_uses_ollama(ai_profile),
        "healthy": False,
    }
    if profile_uses_ollama(ai_profile):
        try:
            ollama_status.update(verify_private_ollama(log, require_ai_network=True))
            ollama_status["healthy"] = True
        except (GateError, OSError, subprocess.CalledProcessError, json.JSONDecodeError):
            ollama_status["reason"] = "private_boundary_check_failed"
    return {
        "database_volume": docker_object_exists("volume", DATABASE_VOLUME, log),
        "data_volume": docker_object_exists("volume", DATA_VOLUME, log),
        "network": docker_object_exists("network", NETWORK, log),
        "ai_network": ai_network_present,
        "ai_network_internal": ai_network_internal,
        "ollama": ollama_status,
        "containers": containers,
        "resource_policy": RESOURCE_POLICY_VERSION,
        "resource_limits_ok": all(
            bool(value.get("resource_limits_ok", True))
            for value in containers.values()
            if isinstance(value, dict)
        ),
        "database_metrics": metrics,
        "tailscale": transport,
        "host_protected_listeners": host_protected_listeners(),
        "player_access_enabled": bool(
            runtime_state and runtime_state.get("player_access_enabled")
        ),
        "tailscale_used": bool(transport.get("serve_tcp_ports")),
        "managed_runtime": runtime_state is not None,
        "development_offline": DEVELOPMENT_OFFLINE_MARKER.is_file(),
        "ai_profile": ai_profile,
        "runtime_started_utc": (
            runtime_state.get("started_utc") if runtime_state else None
        ),
    }


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument(
        "action",
        choices=(
            "gate",
            "gate4",
            "gate5",
            "gate6",
            "start",
            "restart",
            "backup",
            "status",
            "stop",
        ),
        nargs="?",
        default="gate",
    )
    parser.add_argument("--manifest", type=Path)
    parser.add_argument("--data-source", type=Path, default=DEFAULT_DATA_SOURCE)
    parser.add_argument("--stable-seconds", type=int, default=30)
    parser.add_argument("--soak-seconds", type=int, default=300)
    parser.add_argument("--check-interval", type=int, default=30)
    parser.add_argument("--boot-timeout", type=int, default=360)
    parser.add_argument("--connect-timeout", type=int, default=900)
    parser.add_argument("--hold-seconds", type=int, default=30)
    parser.add_argument("--logout-timeout", type=int, default=300)
    parser.add_argument("--preflight-only", action="store_true")
    parser.add_argument("--evidence-root", type=Path)
    parser.add_argument("--backup-root", type=Path, default=DEFAULT_GATE6_BACKUP_ROOT)
    parser.add_argument("--retention-count", type=int)
    parser.add_argument("--development-offline", action="store_true")
    access = parser.add_mutually_exclusive_group()
    access.add_argument("--player-access", action="store_true")
    access.add_argument("--server-only", action="store_true")
    parser.add_argument("--ai-profile", choices=OPERATIONAL_AI_PROFILES)
    args = parser.parse_args()
    if not 10 <= args.stable_seconds <= 300:
        parser.error("--stable-seconds must be between 10 and 300")
    if not 120 <= args.boot_timeout <= 600:
        parser.error("--boot-timeout must be between 120 and 600")
    if not 120 <= args.soak_seconds <= 3600:
        parser.error("--soak-seconds must be between 120 and 3600")
    if not 10 <= args.check_interval <= 60:
        parser.error("--check-interval must be between 10 and 60")
    if not 60 <= args.connect_timeout <= 1800:
        parser.error("--connect-timeout must be between 60 and 1800")
    if not 20 <= args.hold_seconds <= 300:
        parser.error("--hold-seconds must be between 20 and 300")
    if not 60 <= args.logout_timeout <= 600:
        parser.error("--logout-timeout must be between 60 and 600")
    if args.preflight_only and args.action != "gate5":
        parser.error("--preflight-only is valid only with gate5")
    if (args.player_access or args.server_only) and args.action not in {
        "start",
        "restart",
    }:
        parser.error("--player-access/--server-only are valid only with start or restart")
    if args.ai_profile is not None and args.action not in {"start", "restart"}:
        parser.error("--ai-profile is valid only with start or restart")
    if args.retention_count is not None:
        if args.action != "backup":
            parser.error("--retention-count is valid only with backup")
        if not 2 <= args.retention_count <= 90:
            parser.error("--retention-count must be between 2 and 90")
    if args.development_offline and args.action != "stop":
        parser.error("--development-offline is valid only with stop")

    PRIVATE_ROOT.mkdir(parents=True, exist_ok=True)
    os.chmod(PRIVATE_ROOT, 0o700)
    GATE6_PRIVATE_ROOT.mkdir(parents=True, exist_ok=True)
    os.chmod(GATE6_PRIVATE_ROOT, 0o700)
    root_secret = PRIVATE_ROOT / "mysql-root-password"
    app_secret = PRIVATE_ROOT / "acore-password"
    state_path = PRIVATE_ROOT / "state.json"
    lock_path = PRIVATE_ROOT / "operation.lock"

    stamp = dt.datetime.now(dt.UTC).strftime("%Y%m%d-%H%M%SZ")
    gate_action = args.action in {"gate", "gate4", "gate5", "gate6"}
    if gate_action:
        suffixes = {
            "gate": "server-only-runtime-gate3",
            "gate4": "server-only-soak-backup-gate4",
            "gate5": "tailscale-client-smoke-gate5",
            "gate6": "private-operational-deployment-gate6",
        }
        suffix = suffixes[args.action]
        evidence = args.evidence_root or ROOT / "run" / "gates" / f"{stamp}-{suffix}"
        evidence.mkdir(parents=True, exist_ok=False)
        log_path = evidence / f"{args.action}.log"
        status_path = evidence / "status.json"
    else:
        if args.action == "status":
            evidence = GATE6_PRIVATE_ROOT
        else:
            evidence = GATE6_PRIVATE_ROOT / "operations" / f"{stamp}-{args.action}"
            evidence.mkdir(parents=True, exist_ok=False)
            os.chmod(evidence, 0o700)
        log_path = GATE6_PRIVATE_ROOT / "controller.log"
        status_path = GATE6_PRIVATE_ROOT / "latest-status.json"

    status: dict[str, object] = {
        "change_id": {
            "gate": "linux-server-only-runtime-gate3",
            "gate4": GATE4_CHANGE_ID,
            "gate5": GATE5_CHANGE_ID,
            "gate6": GATE6_CHANGE_ID,
        }.get(args.action, GATE6_CHANGE_ID),
        "action": args.action,
        "started_utc": utc_now(),
        "result": "IN_PROGRESS",
        "published_ports": [],
        "tailscale_used": False,
        "player_access_enabled": bool(args.player_access),
    }
    return_code = 1
    with lock_path.open("a+", encoding="utf-8") as lock:
        fcntl.flock(lock, fcntl.LOCK_EX)
        log_mode = "w" if gate_action else "a"
        with log_path.open(log_mode, encoding="utf-8") as log:
            os.chmod(log_path, 0o600)
            if not gate_action:
                log.write(
                    f"\n=== {status['started_utc']} action={args.action} "
                    f"change_id={GATE6_CHANGE_ID} ===\n"
                )
                log.flush()
            try:
                if args.action == "status":
                    status.update(status_snapshot(log))
                    status["result"] = "PASS"
                    return_code = 0
                elif args.action == "stop":
                    status["cleanup"] = operational_stop(evidence, log)
                    if args.development_offline:
                        write_private_json(
                            DEVELOPMENT_OFFLINE_MARKER,
                            {
                                "schema_version": 1,
                                "requested_utc": utc_now(),
                                "reason": "development",
                                "player_access_enabled": False,
                            },
                        )
                    status["development_offline"] = (
                        DEVELOPMENT_OFFLINE_MARKER.is_file()
                    )
                    status["player_access_enabled"] = False
                    status["result"] = "PASS"
                    return_code = 0
                elif args.action in {"start", "restart"}:
                    manifest_path = (args.manifest or discover_manifest()).resolve()
                    manifest, dump = load_manifest(manifest_path)
                    status["gate2_input"] = {
                        "bytes": dump.stat().st_size,
                        "sha256": manifest["sha256"],
                        "release": manifest["release"],
                    }
                    status["gate3_baseline"] = require_gate3_baseline(
                        manifest, root_secret, app_secret, state_path, log
                    )
                    status["gate4_baseline"] = require_gate4_baseline(log)
                    status["gate5_baseline"] = require_gate5_baseline()
                    desired_player_access = bool(args.player_access)
                    desired_ai_profile = args.ai_profile or "disabled"
                    if args.action == "restart":
                        current_state = load_runtime_state()
                        if not args.player_access and not args.server_only:
                            desired_player_access = bool(
                                current_state
                                and current_state.get("player_access_enabled")
                            )
                        if args.ai_profile is None:
                            desired_ai_profile = str(
                                current_state.get("ai_profile", "disabled")
                                if current_state
                                else "disabled"
                            )
                        status["stop"] = operational_stop(evidence, log)
                    status["change_id"] = (
                        GATE10_CHANGE_ID
                        if desired_ai_profile == "bots-ollama-routed-small"
                        else GATE9_CHANGE_ID
                        if profile_uses_ollama(desired_ai_profile)
                        else GATE8_CHANGE_ID
                        if desired_ai_profile != "disabled"
                        else GATE6_CHANGE_ID
                    )
                    status["start"] = operational_start(
                        manifest,
                        root_secret,
                        app_secret,
                        args.boot_timeout,
                        desired_player_access,
                        desired_ai_profile,
                        log,
                    )
                    status["ai_profile"] = desired_ai_profile
                    status["player_access_enabled"] = desired_player_access
                    status["tailscale_used"] = desired_player_access
                    if desired_player_access:
                        status["published_ports"] = [
                            {
                                "transport": "tailscale-serve",
                                "address": TAILSCALE_ADDRESS,
                                "port": port,
                            }
                            for port in (3724, 8085)
                        ]
                    status["result"] = "PASS"
                    return_code = 0
                elif args.action == "backup":
                    status["backup_operation"] = operational_backup(
                        args.backup_root.resolve(),
                        evidence,
                        root_secret,
                        args.retention_count,
                        log,
                    )
                    current_state = load_runtime_state()
                    status["player_access_enabled"] = bool(
                        current_state
                        and current_state.get("player_access_enabled")
                    )
                    status["result"] = "PASS"
                    return_code = 0
                elif args.action == "gate":
                    manifest_path = (args.manifest or discover_manifest()).resolve()
                    manifest, dump = load_manifest(manifest_path)
                    status["gate2_input"] = {
                        "bytes": dump.stat().st_size,
                        "sha256": manifest["sha256"],
                        "release": manifest["release"],
                    }
                    status["runtime_image"] = build_runtime_image(log)
                    status["provision"] = provision(
                        manifest,
                        dump,
                        args.data_source.resolve(),
                        root_secret,
                        app_secret,
                        state_path,
                        log,
                    )
                    status["server_test"] = gate_test(
                        manifest,
                        root_secret,
                        app_secret,
                        evidence,
                        args.stable_seconds,
                        args.boot_timeout,
                        log,
                    )
                    status["result"] = "PASS"
                    status["gate_eligible"] = True
                    return_code = 0
                elif args.action == "gate4":
                    manifest_path = (args.manifest or discover_manifest()).resolve()
                    manifest, dump = load_manifest(manifest_path)
                    status["gate2_input"] = {
                        "bytes": dump.stat().st_size,
                        "sha256": manifest["sha256"],
                        "release": manifest["release"],
                    }
                    status["gate3_baseline"] = require_gate3_baseline(
                        manifest, root_secret, app_secret, state_path, log
                    )
                    status["runtime_image"] = build_runtime_image(log)
                    status["soak_backup_test"] = gate4_test(
                        manifest,
                        root_secret,
                        app_secret,
                        evidence,
                        args.soak_seconds,
                        args.check_interval,
                        args.boot_timeout,
                        log,
                    )
                    status["result"] = "PASS"
                    status["gate_eligible"] = True
                    return_code = 0
                elif args.action == "gate5":
                    manifest_path = (args.manifest or discover_manifest()).resolve()
                    manifest, dump = load_manifest(manifest_path)
                    status["gate2_input"] = {
                        "bytes": dump.stat().st_size,
                        "sha256": manifest["sha256"],
                        "release": manifest["release"],
                    }
                    status["gate3_baseline"] = require_gate3_baseline(
                        manifest, root_secret, app_secret, state_path, log
                    )
                    status["gate4_baseline"] = require_gate4_baseline(log)
                    status["runtime_image"] = build_runtime_image(log)
                    status["client_smoke_test"] = gate5_test(
                        manifest,
                        root_secret,
                        app_secret,
                        evidence,
                        args.boot_timeout,
                        args.connect_timeout,
                        args.hold_seconds,
                        args.logout_timeout,
                        args.preflight_only,
                        log,
                    )
                    status["result"] = "PASS"
                    status["gate_eligible"] = not args.preflight_only
                    status["published_ports"] = [
                        {
                            "transport": "tailscale-serve",
                            "address": TAILSCALE_ADDRESS,
                            "port": 3724,
                        },
                        {
                            "transport": "tailscale-serve",
                            "address": TAILSCALE_ADDRESS,
                            "port": 8085,
                        },
                    ]
                    status["tailscale_used"] = True
                    status["player_access_enabled"] = not args.preflight_only
                    return_code = 0
                else:
                    manifest_path = (args.manifest or discover_manifest()).resolve()
                    manifest, dump = load_manifest(manifest_path)
                    status["gate2_input"] = {
                        "bytes": dump.stat().st_size,
                        "sha256": manifest["sha256"],
                        "release": manifest["release"],
                    }
                    status["gate3_baseline"] = require_gate3_baseline(
                        manifest, root_secret, app_secret, state_path, log
                    )
                    status["gate4_baseline"] = require_gate4_baseline(log)
                    status["gate5_baseline"] = require_gate5_baseline()
                    status["runtime_image"] = build_operational_runtime_image(
                        status["gate5_baseline"], log
                    )
                    status["operational_test"] = gate6_test(
                        manifest,
                        root_secret,
                        app_secret,
                        evidence,
                        args.backup_root.resolve(),
                        args.stable_seconds,
                        args.boot_timeout,
                        log,
                    )
                    status["result"] = "PASS"
                    status["gate_eligible"] = True
                    return_code = 0
            except (
                GateError,
                OSError,
                subprocess.CalledProcessError,
                json.JSONDecodeError,
            ) as exc:
                status["result"] = "FAIL"
                status["gate_eligible"] = False
                status["error"] = str(exc)
            finally:
                if gate_action:
                    status["cleanup"] = cleanup_runtime(log)
                    cleanup = status["cleanup"]
                    persistent_expected = state_path.is_file()
                    volume_state_ok = (
                        cleanup["persistent_database_volume_retained"]
                        and cleanup["persistent_data_volume_retained"]
                        if persistent_expected
                        else not cleanup["persistent_database_volume_retained"]
                        and not cleanup["persistent_data_volume_retained"]
                    )
                    cleanup_ok = (
                        all(cleanup["containers_removed"].values())
                        and cleanup["network_removed"]
                        and cleanup["ai_network_removed"]
                        and volume_state_ok
                        and not cleanup["host_protected_listeners"]
                    )
                    if not cleanup_ok:
                        status["result"] = "FAIL"
                        status["gate_eligible"] = False
                        status.setdefault(
                            "error",
                            f"{args.action} cleanup or persistent-volume verification failed",
                        )
                        status["cleanup_error"] = (
                            "Terminal cleanup did not reach the expected provision state"
                        )
                        return_code = 1
                status["finished_utc"] = utc_now()
                if status_path is not None:
                    write_private_json(status_path, status)

    print(json.dumps(status, indent=2, sort_keys=True))
    if gate_action:
        print(f"Evidence: {evidence}")
    return return_code


if __name__ == "__main__":
    raise SystemExit(main())
