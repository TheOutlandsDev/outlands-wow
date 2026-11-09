#!/usr/bin/env python3
"""Run the isolated native-Linux compile gate and preserve its evidence."""

from __future__ import annotations

import argparse
import datetime as dt
import json
import os
import subprocess
import sys
from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]
IMAGE = "living-azeroth-linux:gate1"


def run(command: list[str], log, *, check: bool = True) -> subprocess.CompletedProcess[str]:
    process = subprocess.run(
        command,
        cwd=ROOT,
        stdout=subprocess.PIPE,
        stderr=subprocess.STDOUT,
        text=True,
        errors="replace",
        check=False,
    )
    log.write("$ " + " ".join(command) + "\n")
    log.write(process.stdout)
    log.flush()
    if check and process.returncode:
        raise subprocess.CalledProcessError(process.returncode, command, process.stdout)
    return process


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--jobs", type=int, default=min(16, os.cpu_count() or 1))
    parser.add_argument("--evidence-root", type=Path)
    args = parser.parse_args()
    if not 1 <= args.jobs <= 32:
        parser.error("--jobs must be between 1 and 32")

    stamp = dt.datetime.now(dt.timezone.utc).strftime("%Y%m%d-%H%M%SZ")
    evidence = args.evidence_root or ROOT / "run" / "gates" / f"{stamp}-linux-native-compile-gate1"
    evidence.mkdir(parents=True, exist_ok=False)
    log_path = evidence / "build.log"
    status_path = evidence / "status.json"

    status: dict[str, object] = {
        "change_id": "linux-native-compile-gate1",
        "started_utc": dt.datetime.now(dt.timezone.utc).isoformat(),
        "result": "IN_PROGRESS",
        "image": IMAGE,
        "jobs": args.jobs,
    }

    try:
        with log_path.open("w", encoding="utf-8") as log:
            revision = run(["git", "rev-parse", "HEAD"], log).stdout.strip()
            status["source_commit"] = revision
            run(["docker", "version"], log)
            run(
                [
                    "docker",
                    "build",
                    "--progress=plain",
                    "--file",
                    "deploy/Dockerfile.gate1",
                    "--target",
                    "gate1",
                    "--build-arg",
                    f"BUILD_JOBS={args.jobs}",
                    "--build-arg",
                    f"SOURCE_REVISION={revision}",
                    "--tag",
                    IMAGE,
                    "source",
                ],
                log,
            )
            inspect = run(
                [
                    "docker",
                    "run",
                    "--rm",
                    "--network=none",
                    IMAGE,
                    "bash",
                    "-lc",
                    "file /azerothcore/dist/bin/{authserver,worldserver,dbimport}; "
                    "sha256sum /azerothcore/dist/bin/{authserver,worldserver,dbimport}",
                ],
                log,
            )
            (evidence / "binary-evidence.txt").write_text(inspect.stdout, encoding="utf-8")
        status["result"] = "PASS"
        return_code = 0
    except (OSError, subprocess.CalledProcessError) as exc:
        status["result"] = "FAIL"
        status["error"] = str(exc)
        return_code = 1
    finally:
        status["finished_utc"] = dt.datetime.now(dt.timezone.utc).isoformat()
        status_path.write_text(json.dumps(status, indent=2) + "\n", encoding="utf-8")

    print(json.dumps(status, indent=2))
    print(f"Evidence: {evidence}")
    return return_code


if __name__ == "__main__":
    raise SystemExit(main())
