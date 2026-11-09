#!/usr/bin/env python3
"""Continuously render bounded Living Azeroth status in a terminal."""

from __future__ import annotations

import json
import subprocess
import sys
import time
from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]
CONTROLLER = ROOT / "tools" / "gate3_server_controller.py"
REFRESH_SECONDS = 5


def fetch_status() -> tuple[dict[str, object] | None, str | None]:
    try:
        process = subprocess.run(
            [sys.executable, str(CONTROLLER), "status"],
            cwd=ROOT,
            capture_output=True,
            text=True,
            errors="replace",
            timeout=12,
            check=False,
        )
    except subprocess.TimeoutExpired:
        return None, "Controller is busy with another operation"
    if process.returncode:
        return None, process.stderr.strip() or process.stdout.strip() or "status failed"
    try:
        return json.loads(process.stdout), None
    except json.JSONDecodeError:
        return None, "Controller returned an unreadable status document"


def yes_no(value: object) -> str:
    return "YES" if value else "NO"


def render(status: dict[str, object] | None, error: str | None) -> None:
    print("\033[2J\033[H", end="")
    print("LIVING AZEROTH · LIVE STATUS")
    print(time.strftime("%Y-%m-%d %H:%M:%S"))
    print("=" * 72)
    if error or status is None:
        print(f"\nWAITING: {error or 'status unavailable'}")
        print("\nThis observer never changes server state. Ctrl+C closes only this tab.")
        return
    containers = status.get("containers", {})
    managed = bool(status.get("managed_runtime"))
    running_count = 0
    if isinstance(containers, dict):
        running_count = sum(
            bool(value.get("running"))
            for value in containers.values()
            if isinstance(value, dict)
        )
    overall = "RUNNING" if managed and running_count == 3 else "STOPPED"
    if managed and running_count != 3:
        overall = "ATTENTION"
    print(f"\nServer:          {overall}")
    print(
        "Player access:   "
        + ("TAILSCALE ONLY" if status.get("player_access_enabled") else "CLOSED")
    )
    print(f"Managed state:   {yes_no(managed)}")
    print(f"Started UTC:     {status.get('runtime_started_utc') or '-'}")
    print("\nSERVICES")
    if isinstance(containers, dict):
        for name, value in containers.items():
            if not isinstance(value, dict):
                continue
            short = str(name).replace("living-azeroth-gate3-", "")
            print(
                f"  {short:<14} {str(value.get('status', 'unknown')).upper():<10} "
                f"restarts={value.get('restart_count', 0)} host-ports="
                f"{value.get('host_port_bindings', {})}"
            )
    metrics = status.get("database_metrics")
    print("\nDATABASE")
    if isinstance(metrics, dict):
        print(
            f"  accounts={metrics.get('accounts')}  characters={metrics.get('characters')}  "
            f"online={metrics.get('online_characters')}  auctions={metrics.get('auctions')}"
        )
    else:
        print("  offline")
    tailscale = status.get("tailscale", {})
    print("\nTAILSCALE")
    if isinstance(tailscale, dict):
        print(
            f"  backend={tailscale.get('backend_state')}  "
            f"node={tailscale.get('hostname')}  "
            f"expected-address={yes_no(tailscale.get('expected_address_present'))}"
        )
        print(
            f"  serve-tcp={tailscale.get('serve_tcp_ports', [])}  "
            f"funnel={yes_no(tailscale.get('funnel_enabled'))}"
        )
    listeners = status.get("host_protected_listeners", [])
    serve_ports = tailscale.get("serve_tcp_ports", []) if isinstance(tailscale, dict) else []
    if status.get("player_access_enabled") and listeners == serve_ports == [3724, 8085]:
        listener_text = "3724/8085 on Tailscale only (expected)"
    elif listeners:
        listener_text = f"ATTENTION: {listeners}"
    else:
        listener_text = "none"
    print(f"\nProtected host listeners: {listener_text}")
    print("\nRefresh: 5 seconds. Ctrl+C closes only this observer tab.")


def main() -> int:
    if sys.version_info < (3, 12):
        print("Python 3.12 or newer is required.", file=sys.stderr)
        return 2
    try:
        while True:
            render(*fetch_status())
            time.sleep(REFRESH_SECONDS)
    except KeyboardInterrupt:
        return 0


if __name__ == "__main__":
    raise SystemExit(main())
