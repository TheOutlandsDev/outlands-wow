#!/usr/bin/env python3
"""Minimal systemd supervisor that delegates every mutation to the controller."""

from __future__ import annotations

import argparse
import json
import signal
import subprocess
import sys
import threading
from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]
CONTROLLER = ROOT / "tools" / "gate3_server_controller.py"
DEVELOPMENT_OFFLINE_MARKER = (
    ROOT / "run" / "private" / "gate6" / "development-offline.json"
)
STOP_REQUESTED = threading.Event()
MAX_CONSECUTIVE_BUSY_POLLS = 5


def request_stop(_signum: int, _frame: object) -> None:
    STOP_REQUESTED.set()


def run_controller(*arguments: str, timeout: int) -> subprocess.CompletedProcess[str]:
    return subprocess.run(
        [sys.executable, str(CONTROLLER), *arguments],
        cwd=ROOT,
        capture_output=True,
        text=True,
        errors="replace",
        timeout=timeout,
        check=False,
    )


def emit(process: subprocess.CompletedProcess[str]) -> None:
    if process.stdout:
        print(process.stdout, end="", flush=True)
    if process.stderr:
        print(process.stderr, end="", file=sys.stderr, flush=True)


def classify_status(process: subprocess.CompletedProcess[str]) -> str:
    if process.returncode:
        return "unhealthy"
    try:
        status = json.loads(process.stdout)
    except json.JSONDecodeError:
        return "unhealthy"
    containers = status.get("containers")
    if not isinstance(containers, dict) or len(containers) != 3:
        return "unhealthy"
    if status.get("result") != "PASS" or status.get("published_ports"):
        return "unhealthy"
    if status.get("resource_limits_ok") is not True:
        return "unhealthy"
    if not status.get("managed_runtime"):
        if all(
            isinstance(value, dict) and not value.get("exists")
            for value in containers.values()
        ):
            return "stopped"
        return "unhealthy"
    if not all(
        isinstance(value, dict)
        and value.get("running")
        and value.get("host_port_bindings") == {}
        for value in containers.values()
    ):
        return "unhealthy"

    tailscale = status.get("tailscale")
    if not isinstance(tailscale, dict) or tailscale.get("funnel_enabled"):
        return "unhealthy"
    listeners = status.get("host_protected_listeners")
    serve_ports = tailscale.get("serve_tcp_ports")
    if status.get("player_access_enabled"):
        if listeners != [3724, 8085] or serve_ports != [3724, 8085]:
            return "unhealthy"
    elif listeners or serve_ports or not tailscale.get("serve_empty"):
        return "unhealthy"
    return "healthy"


def managed_runtime_present(
    process: subprocess.CompletedProcess[str] | None,
) -> bool:
    if process is None or process.returncode:
        return False
    try:
        status = json.loads(process.stdout)
    except json.JSONDecodeError:
        return False
    return status.get("result") == "PASS" and bool(status.get("managed_runtime"))


def development_offline_requested() -> bool:
    return DEVELOPMENT_OFFLINE_MARKER.is_file()


def poll_controller_status(
    timeout: int = 30,
) -> tuple[str, subprocess.CompletedProcess[str] | None]:
    """Classify status while tolerating a bounded controller-owned operation.

    The controller serializes every action, including read-only status, behind
    one lifecycle lock. A verified backup can therefore make a status command
    wait longer than the normal health-check timeout without making the
    runtime unhealthy.
    """
    try:
        process = run_controller("status", timeout=timeout)
    except subprocess.TimeoutExpired:
        return "busy", None
    return classify_status(process), process


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--check-interval", type=int, default=60)
    args = parser.parse_args()
    if not 15 <= args.check_interval <= 300:
        parser.error("--check-interval must be between 15 and 300")
    if sys.version_info < (3, 12):
        print("Python 3.12 or newer is required.", file=sys.stderr)
        return 2

    signal.signal(signal.SIGTERM, request_stop)
    signal.signal(signal.SIGINT, request_stop)
    initial_classification, initial_status = poll_controller_status()
    if development_offline_requested():
        if managed_runtime_present(initial_status):
            print(
                "Development-offline marker found; stopping the managed runtime.",
                flush=True,
            )
            stopped = run_controller("stop", "--development-offline", timeout=360)
            emit(stopped)
            if stopped.returncode:
                return stopped.returncode
        elif initial_classification != "stopped":
            if initial_status is not None:
                emit(initial_status)
            print(
                "Development-offline marker found with an unsafe runtime state.",
                file=sys.stderr,
                flush=True,
            )
            return 1
        print(
            "Living Azeroth is intentionally offline for development.", flush=True
        )
    elif initial_classification == "healthy":
        print("Adopting the already healthy controller-owned runtime.", flush=True)
    else:
        if managed_runtime_present(initial_status):
            print(
                "Reconciling stale controller-owned runtime state before startup.",
                flush=True,
            )
            recovered = run_controller("stop", timeout=360)
            emit(recovered)
            if recovered.returncode:
                return recovered.returncode
        print("Starting Living Azeroth through the controller in server-only mode.", flush=True)
        started = run_controller("start", "--server-only", timeout=720)
        emit(started)
        if started.returncode:
            return started.returncode

    exit_code = 0
    consecutive_busy_polls = 0
    try:
        while not STOP_REQUESTED.wait(args.check_interval):
            classification, status = poll_controller_status()
            if classification == "busy":
                if STOP_REQUESTED.is_set():
                    break
                consecutive_busy_polls += 1
                if consecutive_busy_polls < MAX_CONSECUTIVE_BUSY_POLLS:
                    print(
                        "Controller status is waiting behind another serialized "
                        "operation; deferring this health poll.",
                        flush=True,
                    )
                    continue
                print(
                    "Controller status remained busy for "
                    f"{MAX_CONSECUTIVE_BUSY_POLLS} consecutive polls.",
                    file=sys.stderr,
                    flush=True,
                )
                classification = "unhealthy"
            else:
                consecutive_busy_polls = 0
            if development_offline_requested():
                if managed_runtime_present(status):
                    print(
                        "Development-offline marker is active; stopping the managed runtime.",
                        flush=True,
                    )
                    stopped = run_controller(
                        "stop", "--development-offline", timeout=360
                    )
                    emit(stopped)
                    if stopped.returncode:
                        exit_code = stopped.returncode
                        STOP_REQUESTED.set()
                    continue
                if classification == "stopped":
                    continue
                if status is not None:
                    emit(status)
                print(
                    "Development-offline state became unsafe.",
                    file=sys.stderr,
                    flush=True,
                )
                exit_code = 1
                STOP_REQUESTED.set()
                continue
            if classification == "healthy":
                continue
            if classification == "stopped":
                print(
                    "The runtime is cleanly stopped; the supervisor remains available.",
                    flush=True,
                )
                continue
            if status is not None:
                emit(status)
            print(
                "Server-only health changed; requesting a fail-closed controller stop.",
                file=sys.stderr,
                flush=True,
            )
            exit_code = 1
            STOP_REQUESTED.set()
    finally:
        print("Stopping Living Azeroth through the controller.", flush=True)
        stopped = run_controller("stop", timeout=360)
        emit(stopped)
        if stopped.returncode:
            exit_code = stopped.returncode
    return exit_code


if __name__ == "__main__":
    raise SystemExit(main())
