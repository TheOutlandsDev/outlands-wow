#!/usr/bin/env python3
"""Observe a running Tailscale-only Gate 6 deployment without lifecycle authority."""

from __future__ import annotations

import argparse
import datetime as dt
import json
import os
import subprocess
import sys
import time
from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]
CONTROLLER = ROOT / "tools" / "gate3_server_controller.py"
CONTAINERS = (
    "living-azeroth-gate3-worldserver",
    "living-azeroth-gate3-authserver",
    "living-azeroth-gate3-database",
)


class ObservationError(RuntimeError):
    """Raised when the read-only soak observes an unsafe or unhealthy state."""


def utc_now() -> str:
    return dt.datetime.now(dt.UTC).isoformat()


def fetch_status() -> dict[str, object]:
    process = subprocess.run(
        [sys.executable, str(CONTROLLER), "status"],
        cwd=ROOT,
        capture_output=True,
        text=True,
        errors="replace",
        timeout=20,
        check=False,
    )
    if process.returncode:
        raise ObservationError(process.stderr.strip() or "controller status failed")
    try:
        value = json.loads(process.stdout)
    except json.JSONDecodeError as exc:
        raise ObservationError("controller status was not JSON") from exc
    if not isinstance(value, dict):
        raise ObservationError("controller status was not an object")
    return value


def validate_status(
    status: dict[str, object],
    elapsed: int,
    *,
    player_access: bool = True,
    expected_ai_profile: str | None = None,
) -> dict[str, object]:
    containers = status.get("containers")
    tailscale = status.get("tailscale")
    metrics = status.get("database_metrics")
    if not isinstance(containers, dict) or not isinstance(tailscale, dict):
        raise ObservationError("status is missing container or Tailscale state")
    if not status.get("managed_runtime"):
        raise ObservationError("Gate 6 is not running as a managed runtime")
    if bool(status.get("player_access_enabled")) != player_access:
        raise ObservationError("Gate 6 player-access mode differs from the requested soak")
    if status.get("resource_limits_ok") is not True:
        raise ObservationError("one or more container resource limits drifted")
    if expected_ai_profile is not None and status.get("ai_profile") != expected_ai_profile:
        raise ObservationError("the active AI profile differs from the requested soak")
    for name in CONTAINERS:
        value = containers.get(name)
        if (
            not isinstance(value, dict)
            or not value.get("running")
            or value.get("host_port_bindings")
            or value.get("restart_count") != 0
            or value.get("resource_limits_ok") is not True
        ):
            raise ObservationError(f"container is unhealthy or host-published: {name}")
    expected_ports = [3724, 8085] if player_access else []
    if (
        tailscale.get("backend_state") != "Running"
        or not tailscale.get("expected_address_present")
        or tailscale.get("serve_tcp_ports") != expected_ports
        or tailscale.get("funnel_enabled")
    ):
        raise ObservationError("Tailscale transport is not in its exact safe state")
    if status.get("host_protected_listeners") != expected_ports:
        raise ObservationError("host protected listeners differ from the requested soak")
    if not player_access and tailscale.get("serve_empty") is not True:
        raise ObservationError("server-only soak requires empty Tailscale Serve state")
    ollama = status.get("ollama")
    if expected_ai_profile and "ollama" in expected_ai_profile:
        if (
            not isinstance(ollama, dict)
            or ollama.get("required") is not True
            or ollama.get("healthy") is not True
        ):
            raise ObservationError("private Ollama boundary is not healthy")
    if not status.get("database_volume") or not status.get("data_volume"):
        raise ObservationError("a persistent Living Azeroth volume is missing")
    if not isinstance(metrics, dict):
        raise ObservationError("database metrics are unavailable")
    online = int(metrics.get("online_characters", -1))
    if online < 0:
        raise ObservationError("online character metric is invalid")
    return {
        "elapsed_seconds": elapsed,
        "observed_utc": utc_now(),
        "running_containers": 3,
        "container_restarts": 0,
        "online_characters": online,
        "ai_profile": status.get("ai_profile"),
        "resource_limits_ok": True,
        "tailscale_tcp_ports": expected_ports,
        "funnel_enabled": False,
        "docker_host_port_bindings": {},
    }


def write_private_json(path: Path, value: object) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    os.chmod(path.parent, 0o700)
    temporary = path.with_suffix(path.suffix + ".tmp")
    temporary.write_text(
        json.dumps(value, indent=2, sort_keys=True) + "\n", encoding="utf-8"
    )
    os.chmod(temporary, 0o600)
    temporary.replace(path)


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--duration", type=int, default=600)
    parser.add_argument("--interval", type=int, default=30)
    parser.add_argument("--server-only", action="store_true")
    parser.add_argument("--expected-ai-profile")
    args = parser.parse_args()
    if not 120 <= args.duration <= 3600:
        parser.error("--duration must be between 120 and 3600 seconds")
    if not 10 <= args.interval <= 60:
        parser.error("--interval must be between 10 and 60 seconds")

    stamp = dt.datetime.now(dt.UTC).strftime("%Y%m%d-%H%M%SZ")
    output = (
        ROOT
        / "run"
        / "private"
        / "gate6"
        / "soaks"
        / (
            f"{stamp}-server-only-ai-soak.json"
            if args.server_only
            else f"{stamp}-tailscale-access-soak.json"
        )
    )
    started_utc = utc_now()
    started = time.monotonic()
    checkpoints: list[dict[str, object]] = []
    print(
        f"Observing exact Gate 6 {'server-only' if args.server_only else 'private'} "
        f"state for {args.duration} seconds; "
        "this process has no lifecycle authority.",
        flush=True,
    )
    try:
        while True:
            elapsed = int(time.monotonic() - started)
            checkpoint = validate_status(
                fetch_status(),
                elapsed,
                player_access=not args.server_only,
                expected_ai_profile=args.expected_ai_profile,
            )
            checkpoints.append(checkpoint)
            print(
                f"[{checkpoint['observed_utc']}] elapsed={elapsed}s "
                f"containers=3 online={checkpoint['online_characters']} "
                f"tailscale={checkpoint['tailscale_tcp_ports']} "
                "funnel=off host-bindings=empty limits=exact restarts=0",
                flush=True,
            )
            if elapsed >= args.duration:
                break
            time.sleep(min(args.interval, args.duration - elapsed))
    except (ObservationError, OSError, subprocess.SubprocessError) as exc:
        result = {
            "schema_version": 1,
            "result": "FAIL",
            "started_utc": started_utc,
            "finished_utc": utc_now(),
            "declared_duration_seconds": args.duration,
            "observed_duration_seconds": int(time.monotonic() - started),
            "checkpoints": checkpoints,
            "error": str(exc),
            "lifecycle_authority": False,
        }
        write_private_json(output, result)
        print(f"SOAK FAILED: {exc}\nEvidence: {output}", file=sys.stderr)
        return 1

    result = {
        "schema_version": 1,
        "result": "PASS",
        "started_utc": started_utc,
        "finished_utc": utc_now(),
        "declared_duration_seconds": args.duration,
        "observed_duration_seconds": int(time.monotonic() - started),
        "checkpoint_count": len(checkpoints),
        "maximum_online_characters": max(
            int(value["online_characters"]) for value in checkpoints
        ),
        "maximum_container_restarts": 0,
        "player_access_enabled": not args.server_only,
        "expected_ai_profile": args.expected_ai_profile,
        "resource_limits_verified": True,
        "checkpoints": checkpoints,
        "player_identity_recorded": False,
        "credentials_recorded": False,
        "lifecycle_authority": False,
    }
    write_private_json(output, result)
    print(f"SOAK PASSED\nEvidence: {output}", flush=True)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
