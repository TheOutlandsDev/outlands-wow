#!/usr/bin/env python3
"""Emit bounded Living Azeroth metrics for node-exporter's textfile collector."""

from __future__ import annotations

import argparse
import datetime as dt
import fcntl
import io
import json
import os
import tempfile
import time
from pathlib import Path

import gate3_server_controller as controller


DEFAULT_OUTPUT = Path(
    "/var/lib/node_exporter/textfile_collector/living-azeroth.prom"
)
DEFAULT_BACKUP_ROOT = Path("/var/backups/outlands-wow")
DEFAULT_DEVELOPMENT_OFFLINE_MARKER = controller.DEVELOPMENT_OFFLINE_MARKER
EXPECTED_CONTAINERS = {
    controller.DATABASE_CONTAINER: "database",
    controller.AUTH_CONTAINER: "authserver",
    controller.WORLD_CONTAINER: "worldserver",
}


def quote_label(value: object) -> str:
    return (
        str(value)
        .replace("\\", "\\\\")
        .replace("\n", "\\n")
        .replace('"', '\\"')
    )


def metric(name: str, value: int | float, labels: dict[str, object] | None = None) -> str:
    suffix = ""
    if labels:
        rendered = ",".join(
            f'{key}="{quote_label(label)}"' for key, label in sorted(labels.items())
        )
        suffix = "{" + rendered + "}"
    return f"{name}{suffix} {value}"


def backup_snapshot(root: Path) -> dict[str, int | float]:
    successful: list[tuple[float, int]] = []
    if root.is_dir():
        for manifest_path in root.glob("*-logical/manifest.json"):
            try:
                document = json.loads(manifest_path.read_text(encoding="utf-8"))
                if document.get("result") != "PASS":
                    continue
                created = dt.datetime.fromisoformat(str(document["created_utc"]))
                successful.append((created.timestamp(), int(document["bytes"])))
            except (OSError, ValueError, KeyError, json.JSONDecodeError):
                continue
    latest = max(successful, default=(0.0, 0))
    return {
        "successful_count": len(successful),
        "latest_created_timestamp_seconds": latest[0],
        "latest_bytes": latest[1],
    }


def security_boundary_ok(status: dict[str, object]) -> bool:
    containers = status.get("containers")
    tailscale = status.get("tailscale")
    if not isinstance(containers, dict) or not isinstance(tailscale, dict):
        return False
    if any(
        not isinstance(value, dict) or value.get("host_port_bindings") != {}
        for value in containers.values()
        if isinstance(value, dict) and value.get("exists")
    ):
        return False
    if tailscale.get("funnel_enabled"):
        return False
    profile = str(status.get("ai_profile", "disabled"))
    ollama = status.get("ollama")
    if controller.profile_uses_ollama(profile):
        if (
            not status.get("ai_network")
            or not status.get("ai_network_internal")
            or not isinstance(ollama, dict)
            or not ollama.get("healthy")
            or ollama.get("host_binding") != "127.0.0.1:11434"
            or ollama.get("tailscale_published") is not False
        ):
            return False
    elif status.get("ai_network"):
        return False
    serve_ports = tailscale.get("serve_tcp_ports")
    listeners = status.get("host_protected_listeners")
    if status.get("player_access_enabled"):
        return serve_ports == [3724, 8085] and listeners == [3724, 8085]
    return serve_ports == [] and listeners == []


def runtime_healthy(status: dict[str, object]) -> bool:
    containers = status.get("containers")
    if not status.get("managed_runtime") or not isinstance(containers, dict):
        return False
    expected = [containers.get(name) for name in EXPECTED_CONTAINERS]
    return all(
        isinstance(value, dict)
        and value.get("running")
        and value.get("host_port_bindings") == {}
        for value in expected
    ) and security_boundary_ok(status)


def render_metrics(
    status: dict[str, object],
    backups: dict[str, int | float],
    now: float,
    development_offline: bool,
) -> str:
    lines = [
        "# HELP living_azeroth_metrics_collection_success Whether the latest bounded collection succeeded.",
        "# TYPE living_azeroth_metrics_collection_success gauge",
        metric("living_azeroth_metrics_collection_success", 1),
        "# HELP living_azeroth_metrics_last_scrape_timestamp_seconds Unix timestamp of the latest bounded collection.",
        "# TYPE living_azeroth_metrics_last_scrape_timestamp_seconds gauge",
        metric("living_azeroth_metrics_last_scrape_timestamp_seconds", int(now)),
        "# HELP living_azeroth_managed_runtime Whether controller-owned runtime state is present.",
        "# TYPE living_azeroth_managed_runtime gauge",
        metric("living_azeroth_managed_runtime", int(bool(status.get("managed_runtime")))),
        "# HELP living_azeroth_development_offline Whether the persistent development-offline marker is active.",
        "# TYPE living_azeroth_development_offline gauge",
        metric("living_azeroth_development_offline", int(development_offline)),
        "# HELP living_azeroth_desired_online Whether runtime health and current backups are operationally required.",
        "# TYPE living_azeroth_desired_online gauge",
        metric("living_azeroth_desired_online", int(not development_offline)),
        "# HELP living_azeroth_backup_required Whether runtime changes make the thirty-hour backup objective active.",
        "# TYPE living_azeroth_backup_required gauge",
        metric("living_azeroth_backup_required", int(not development_offline)),
        "# HELP living_azeroth_runtime_healthy Whether all controller-owned components and exposure checks pass.",
        "# TYPE living_azeroth_runtime_healthy gauge",
        metric("living_azeroth_runtime_healthy", int(runtime_healthy(status))),
        "# HELP living_azeroth_security_boundary_ok Whether Docker, MySQL, Tailscale, and Funnel exposure is bounded.",
        "# TYPE living_azeroth_security_boundary_ok gauge",
        metric("living_azeroth_security_boundary_ok", int(security_boundary_ok(status))),
        "# HELP living_azeroth_player_access_enabled Whether explicit Tailscale-only player access is enabled.",
        "# TYPE living_azeroth_player_access_enabled gauge",
        metric(
            "living_azeroth_player_access_enabled",
            int(bool(status.get("player_access_enabled"))),
        ),
    ]

    containers = status.get("containers")
    if not isinstance(containers, dict):
        containers = {}
    lines.extend(
        (
            "# HELP living_azeroth_container_running Whether a required runtime container is running.",
            "# TYPE living_azeroth_container_running gauge",
            "# HELP living_azeroth_container_restart_count Docker restart count for a required runtime container.",
            "# TYPE living_azeroth_container_restart_count gauge",
        )
    )
    for name, role in EXPECTED_CONTAINERS.items():
        value = containers.get(name)
        detail = value if isinstance(value, dict) else {}
        labels = {"role": role}
        lines.append(
            metric("living_azeroth_container_running", int(bool(detail.get("running"))), labels)
        )
        lines.append(
            metric(
                "living_azeroth_container_restart_count",
                int(detail.get("restart_count", 0)),
                labels,
            )
        )

    profile = str(status.get("ai_profile", "disabled"))
    ollama = status.get("ollama")
    ollama_detail = ollama if isinstance(ollama, dict) else {}
    ollama_required = controller.profile_uses_ollama(profile)
    lines.extend(
        (
            "# HELP living_azeroth_ai_profile_info Active bounded AI profile (one labeled series).",
            "# TYPE living_azeroth_ai_profile_info gauge",
            metric("living_azeroth_ai_profile_info", 1, {"profile": profile}),
            "# HELP living_azeroth_playerbot_profile_enabled Whether a bounded playerbot profile is active.",
            "# TYPE living_azeroth_playerbot_profile_enabled gauge",
            metric("living_azeroth_playerbot_profile_enabled", int(profile != "disabled")),
            "# HELP living_azeroth_ollama_required Whether the active bounded profile requires local Ollama.",
            "# TYPE living_azeroth_ollama_required gauge",
            metric("living_azeroth_ollama_required", int(ollama_required)),
            "# HELP living_azeroth_ollama_backend_healthy Whether the private model, loopback binding, and internal bridge pass.",
            "# TYPE living_azeroth_ollama_backend_healthy gauge",
            metric(
                "living_azeroth_ollama_backend_healthy",
                int(not ollama_required or bool(ollama_detail.get("healthy"))),
            ),
        )
    )

    database_metrics = status.get("database_metrics")
    if not isinstance(database_metrics, dict):
        database_metrics = {}
    lines.extend(
        (
            "# HELP living_azeroth_database_metric Bounded aggregate database counts; no player identity is exported.",
            "# TYPE living_azeroth_database_metric gauge",
        )
    )
    for name in (
        "schema_count",
        "accounts",
        "characters",
        "online_characters",
        "world_tables",
        "playerbots_tables",
        "auctions",
    ):
        if name in database_metrics:
            lines.append(
                metric(
                    "living_azeroth_database_metric",
                    int(database_metrics[name]),
                    {"metric": name},
                )
            )

    lines.extend(
        (
            "# HELP living_azeroth_backup_successful_count Number of successful retained logical-backup manifests.",
            "# TYPE living_azeroth_backup_successful_count gauge",
            metric(
                "living_azeroth_backup_successful_count",
                int(backups["successful_count"]),
            ),
            "# HELP living_azeroth_backup_latest_created_timestamp_seconds Creation time of the latest successful logical backup.",
            "# TYPE living_azeroth_backup_latest_created_timestamp_seconds gauge",
            metric(
                "living_azeroth_backup_latest_created_timestamp_seconds",
                int(backups["latest_created_timestamp_seconds"]),
            ),
            "# HELP living_azeroth_backup_latest_bytes Size of the latest successful logical backup.",
            "# TYPE living_azeroth_backup_latest_bytes gauge",
            metric("living_azeroth_backup_latest_bytes", int(backups["latest_bytes"])),
        )
    )
    return "\n".join(lines) + "\n"


def atomic_write(path: Path, contents: str) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    descriptor, temporary = tempfile.mkstemp(prefix=f".{path.name}.", dir=path.parent)
    try:
        with os.fdopen(descriptor, "w", encoding="utf-8") as stream:
            stream.write(contents)
            stream.flush()
            os.fsync(stream.fileno())
        os.chmod(temporary, 0o644)
        os.replace(temporary, path)
    finally:
        if os.path.exists(temporary):
            os.unlink(temporary)


def sanitized_summary(
    status: dict[str, object],
    backups: dict[str, int | float],
    development_offline: bool,
) -> dict[str, object]:
    containers = status.get("containers")
    running = 0
    if isinstance(containers, dict):
        running = sum(
            1
            for value in containers.values()
            if isinstance(value, dict) and value.get("running")
        )
    database_metrics = status.get("database_metrics")
    online = (
        int(database_metrics.get("online_characters", 0))
        if isinstance(database_metrics, dict)
        else 0
    )
    return {
        "event": "living_azeroth_observability_snapshot",
        "result": "PASS",
        "development_offline": development_offline,
        "runtime_healthy": runtime_healthy(status),
        "security_boundary_ok": security_boundary_ok(status),
        "containers_running": running,
        "player_access_enabled": bool(status.get("player_access_enabled")),
        "ai_profile": str(status.get("ai_profile", "disabled")),
        "ollama_required": controller.profile_uses_ollama(
            str(status.get("ai_profile", "disabled"))
        ),
        "ollama_healthy": bool(
            isinstance(status.get("ollama"), dict)
            and status["ollama"].get("healthy")
        ),
        "online_characters": online,
        "successful_backup_count": int(backups["successful_count"]),
    }


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--output", type=Path, default=DEFAULT_OUTPUT)
    parser.add_argument("--backup-root", type=Path, default=DEFAULT_BACKUP_ROOT)
    parser.add_argument(
        "--development-offline-marker",
        type=Path,
        default=DEFAULT_DEVELOPMENT_OFFLINE_MARKER,
    )
    args = parser.parse_args()

    lock_path = controller.PRIVATE_ROOT / "operation.lock"
    lock_path.parent.mkdir(parents=True, exist_ok=True)
    with lock_path.open("a+", encoding="utf-8") as lock:
        try:
            fcntl.flock(lock, fcntl.LOCK_SH | fcntl.LOCK_NB)
        except BlockingIOError:
            print(
                json.dumps(
                    {
                        "event": "living_azeroth_observability_deferred",
                        "reason": "controller_busy",
                    },
                    sort_keys=True,
                ),
                flush=True,
            )
            return 0
        status = controller.status_snapshot(io.StringIO())
        backups = backup_snapshot(args.backup_root)
        development_offline = args.development_offline_marker.is_file()
        atomic_write(
            args.output,
            render_metrics(status, backups, time.time(), development_offline),
        )
        print(
            json.dumps(
                sanitized_summary(status, backups, development_offline), sort_keys=True
            ),
            flush=True,
        )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
