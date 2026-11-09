#!/usr/bin/env python3
"""Follow one exact Living Azeroth container without lifecycle authority."""

from __future__ import annotations

import argparse
import subprocess
import time


CONTAINERS = {
    "database": "living-azeroth-gate3-database",
    "auth": "living-azeroth-gate3-authserver",
    "world": "living-azeroth-gate3-worldserver",
}


def running(container: str) -> bool:
    result = subprocess.run(
        ["docker", "inspect", "--format", "{{.State.Running}}", container],
        capture_output=True,
        text=True,
        check=False,
    )
    return result.returncode == 0 and result.stdout.strip() == "true"


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("role", choices=tuple(CONTAINERS))
    args = parser.parse_args()
    container = CONTAINERS[args.role]
    print(f"LIVING AZEROTH · {args.role.upper()} LOG")
    print(f"Observer for exact container: {container}")
    print("This tab cannot start, stop, or restart anything.\n", flush=True)
    waiting_printed = False
    try:
        while True:
            if not running(container):
                if not waiting_printed:
                    print("Waiting for the controller to start this container...", flush=True)
                    waiting_printed = True
                time.sleep(2)
                continue
            waiting_printed = False
            print("Container is running; following logs.\n", flush=True)
            subprocess.run(
                ["docker", "logs", "--follow", "--tail", "200", container],
                check=False,
            )
            print("\nContainer stopped; waiting for the next controlled start...", flush=True)
            time.sleep(2)
    except KeyboardInterrupt:
        return 0


if __name__ == "__main__":
    raise SystemExit(main())
