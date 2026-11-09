#!/usr/bin/env python3
"""Fixed-action local operator console for Living Azeroth."""

from __future__ import annotations

import subprocess
import sys
from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]
CONTROLLER = ROOT / "tools" / "gate3_server_controller.py"


def run_action(*arguments: str) -> int:
    command = [sys.executable, str(CONTROLLER), *arguments]
    print("\n" + "=" * 72)
    print(f"Living Azeroth controller: {' '.join(arguments)}")
    print("=" * 72, flush=True)
    return subprocess.run(command, cwd=ROOT, check=False).returncode


def confirm(prompt: str, phrase: str) -> bool:
    print(f"\n{prompt}")
    return input(f"Type {phrase!r} to continue: ").strip() == phrase


def show_menu() -> None:
    print(
        """
LIVING AZEROTH · PRIVATE OPERATIONS

  1  Status
  2  Start or switch — Tailscale player access
  3  Start or switch — server only (no player access)
  4  Restart — preserve current access mode
  5  Validated live backup + networkless restore test
  6  Stop gracefully and close player access
  7  Quit this console (does not stop the server)

The other terminal tabs are observers. This console is the only place in this
window that can request lifecycle changes.
""".strip()
    )


def main() -> int:
    if sys.version_info < (3, 12):
        print("Python 3.12 or newer is required.", file=sys.stderr)
        return 2
    while True:
        show_menu()
        try:
            choice = input("\nChoose 1-7: ").strip()
        except (EOFError, KeyboardInterrupt):
            print("\nConsole closed; server state was not changed.")
            return 0
        if choice == "1":
            run_action("status")
        elif choice == "2":
            if confirm(
                "This opens TCP 3724 and 8085 only through the approved Tailscale node.",
                "OPEN TAILSCALE",
            ):
                run_action("restart", "--player-access")
            else:
                print("Start cancelled.")
        elif choice == "3":
            run_action("restart", "--server-only")
        elif choice == "4":
            run_action("restart")
        elif choice == "5":
            run_action("backup")
        elif choice == "6":
            if confirm(
                "This closes Living Azeroth player access and gracefully stops all three services.",
                "STOP LIVING AZEROTH",
            ):
                run_action("stop")
            else:
                print("Stop cancelled.")
        elif choice == "7":
            print("Console closed; server state was not changed.")
            return 0
        else:
            print("Choose one of the displayed fixed actions.")
        input("\nPress Enter to return to the menu...")


if __name__ == "__main__":
    raise SystemExit(main())
