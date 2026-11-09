#!/usr/bin/env python3
"""Compile and run the LA25 standalone module tests on native Linux."""

from __future__ import annotations

import argparse
import datetime as dt
import hashlib
import json
import os
import shlex
import subprocess
from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]
MODULE = ROOT / "source" / "modules" / "mod-ollama-chat"

TESTS = (
    ("chat_policy_test", ("tests/chat_policy_test.cpp",), False),
    ("continuity_store_test", ("tests/continuity_store_test.cpp",), False),
    ("social_coordinator_test", ("tests/social_coordinator_test.cpp",), False),
    (
        "query_priority_test",
        ("tests/query_priority_test.cpp", "src/mod-ollama-chat_querymanager.cpp"),
        True,
    ),
)


def execute(command: list[str], log) -> subprocess.CompletedProcess[str]:
    process = subprocess.run(
        command,
        cwd=ROOT,
        stdout=subprocess.PIPE,
        stderr=subprocess.STDOUT,
        text=True,
        errors="replace",
        check=False,
    )
    log.write("$ " + shlex.join(command) + "\n")
    log.write(process.stdout)
    log.flush()
    if process.returncode:
        raise subprocess.CalledProcessError(process.returncode, command, process.stdout)
    return process


def sha256(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as handle:
        for block in iter(lambda: handle.read(1024 * 1024), b""):
            digest.update(block)
    return digest.hexdigest().upper()


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--compiler", default=os.environ.get("CXX", "g++"))
    parser.add_argument("--evidence-root", type=Path)
    args = parser.parse_args()

    stamp = dt.datetime.now(dt.timezone.utc).strftime("%Y%m%d-%H%M%SZ")
    evidence = args.evidence_root or ROOT / "run" / "gates" / f"{stamp}-linux-gate1-offline"
    binaries = evidence / "bin"
    binaries.mkdir(parents=True, exist_ok=False)
    log_path = evidence / "offline-tests.log"
    status_path = evidence / "status.json"
    status: dict[str, object] = {
        "change_id": "linux-native-compile-gate1-offline",
        "started_utc": dt.datetime.now(dt.timezone.utc).isoformat(),
        "result": "IN_PROGRESS",
        "tests": [],
    }

    return_code = 1
    try:
        with log_path.open("w", encoding="utf-8") as log:
            status["source_commit"] = execute(["git", "rev-parse", "HEAD"], log).stdout.strip()
            status["compiler"] = execute([args.compiler, "--version"], log).stdout.splitlines()[0]
            results: list[dict[str, object]] = []
            for name, relative_sources, threaded in TESTS:
                executable = binaries / name
                command = [
                    args.compiler,
                    "-std=c++20",
                    "-Wall",
                    "-Wextra",
                    "-Wpedantic",
                    f"-I{MODULE / 'tests' / 'stubs'}",
                    f"-I{MODULE / 'src'}",
                ]
                if threaded:
                    command.append("-pthread")
                command.extend(str(MODULE / source) for source in relative_sources)
                command.extend(["-o", str(executable)])
                execute(command, log)
                result = execute([str(executable)], log)
                if "PASS" not in result.stdout:
                    raise RuntimeError(f"{name} returned successfully without a PASS marker")
                results.append(
                    {
                        "name": name,
                        "result": "PASS",
                        "sha256": sha256(executable),
                        "output": result.stdout.strip(),
                    }
                )
            status["tests"] = results
        status["result"] = "PASS"
        return_code = 0
    except (OSError, RuntimeError, subprocess.CalledProcessError) as exc:
        status["result"] = "FAIL"
        status["error"] = str(exc)
    finally:
        status["finished_utc"] = dt.datetime.now(dt.timezone.utc).isoformat()
        status_path.write_text(json.dumps(status, indent=2) + "\n", encoding="utf-8")

    print(json.dumps(status, indent=2))
    print(f"Evidence: {evidence}")
    return return_code


if __name__ == "__main__":
    raise SystemExit(main())
