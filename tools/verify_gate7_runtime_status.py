#!/usr/bin/env python3
"""Verify a bounded Gate 7 controller status snapshot."""

from __future__ import annotations

import argparse
import json
import subprocess
import sys
from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]
CONTROLLER = ROOT / "tools" / "gate3_server_controller.py"
CONTAINER_NAMES = {
    "living-azeroth-gate3-worldserver",
    "living-azeroth-gate3-authserver",
    "living-azeroth-gate3-database",
}


def fetch_status() -> dict[str, object]:
    process = subprocess.run(
        [sys.executable, str(CONTROLLER), "status"],
        cwd=ROOT,
        capture_output=True,
        text=True,
        errors="replace",
        timeout=30,
        check=False,
    )
    if process.returncode:
        raise RuntimeError(process.stderr.strip() or process.stdout.strip())
    return json.loads(process.stdout)


def verify_running(status: dict[str, object], player_access: bool) -> dict[str, object]:
    containers = status.get("containers")
    if not isinstance(containers, dict) or set(containers) != CONTAINER_NAMES:
        raise RuntimeError("Unexpected Living Azeroth container inventory")
    for name, value in containers.items():
        if not isinstance(value, dict):
            raise RuntimeError(f"Invalid container state for {name}")
        if (
            not value.get("exists")
            or not value.get("running")
            or value.get("restart_count") != 0
            or value.get("host_port_bindings") != {}
        ):
            raise RuntimeError(f"Container is not healthy and private: {name}")
    metrics = status.get("database_metrics")
    if not isinstance(metrics, dict) or metrics.get("online_characters") != 0:
        raise RuntimeError("Gate 7 requires zero online characters")
    tailscale = status.get("tailscale")
    if not isinstance(tailscale, dict) or tailscale.get("funnel_enabled"):
        raise RuntimeError("Tailscale state is missing or Funnel is enabled")
    expected_ports = [3724, 8085] if player_access else []
    if (
        status.get("result") != "PASS"
        or not status.get("managed_runtime")
        or status.get("player_access_enabled") is not player_access
        or status.get("published_ports") != []
        or status.get("host_protected_listeners") != expected_ports
        or tailscale.get("serve_tcp_ports") != expected_ports
    ):
        raise RuntimeError("Runtime or transport does not match the requested Gate 7 mode")
    if not player_access and not tailscale.get("serve_empty"):
        raise RuntimeError("Server-only status has a non-empty Tailscale Serve configuration")
    return {
        "result": "PASS",
        "mode": "player-access" if player_access else "server-only",
        "containers_running": 3,
        "container_restarts": 0,
        "docker_host_bindings": [],
        "online_characters": 0,
        "tailscale_tcp_ports": expected_ports,
        "funnel_enabled": False,
    }


def verify_stopped(status: dict[str, object]) -> dict[str, object]:
    containers = status.get("containers")
    tailscale = status.get("tailscale")
    if not isinstance(containers, dict) or not isinstance(tailscale, dict):
        raise RuntimeError("Stopped status is incomplete")
    if (
        status.get("managed_runtime")
        or status.get("player_access_enabled")
        or status.get("host_protected_listeners")
        or status.get("published_ports")
        or tailscale.get("serve_tcp_ports")
        or tailscale.get("funnel_enabled")
        or any(
            isinstance(value, dict) and value.get("exists")
            for value in containers.values()
        )
    ):
        raise RuntimeError("Runtime did not stop cleanly")
    return {
        "result": "PASS",
        "mode": "stopped",
        "containers_present": 0,
        "protected_listeners": [],
        "tailscale_tcp_ports": [],
        "funnel_enabled": False,
    }


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("mode", choices=("server-only", "player-access", "stopped"))
    parser.add_argument("--output", type=Path)
    args = parser.parse_args()
    status = fetch_status()
    result = (
        verify_stopped(status)
        if args.mode == "stopped"
        else verify_running(status, args.mode == "player-access")
    )
    rendered = json.dumps(result, indent=2, sort_keys=True) + "\n"
    if args.output:
        args.output.write_text(rendered, encoding="utf-8")
        args.output.chmod(0o600)
    print(rendered, end="")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
