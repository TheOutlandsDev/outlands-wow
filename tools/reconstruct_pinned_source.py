#!/usr/bin/env python3
"""Reconstruct the public kit from four already fetched, pinned Git checkouts.

No network access or existing output is used. A failed run leaves its new output
directory in place for inspection; choose another destination for a retry.
"""

from __future__ import annotations

import argparse
from dataclasses import dataclass
from pathlib import Path
import shutil
import subprocess
import sys
import tarfile


KIT = Path(__file__).resolve().parents[1]


@dataclass(frozen=True)
class Component:
    option: str
    pin: str
    patch: str
    destination: str


COMPONENTS = (
    Component("core", "ceeb3116ebedf4b35f12b75e5481b6ddd0de7a89", "upstream-to-import-core.patch", "."),
    Component("playerbots", "3fa1c1e49f8f1324b72461e576bce7c89b0a6521", "upstream-to-import-mod-playerbots.patch", "modules/mod-playerbots"),
    Component("auction", "a680cc1c98290713e9b3d3289544af78e5186dc1", "upstream-to-import-mod-ah-bot.patch", "modules/mod-ah-bot"),
    Component("ollama", "8ba5e791f0a84ee04636f0b19b62d3c4aff3dce1", "upstream-to-import-mod-ollama-chat.patch", "modules/mod-ollama-chat"),
)


def git(directory: Path, *arguments: str) -> str:
    result = subprocess.run(
        ["git", "-C", str(directory), *arguments],
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
        text=True,
        errors="replace",
    )
    if result.returncode:
        raise RuntimeError(f"git {arguments[0]} failed in {directory}: {result.stderr[-1200:]}")
    return result.stdout.strip()


def check_checkout(source: Path, component: Component) -> Path:
    source = source.resolve(strict=True)
    if not source.is_dir():
        raise ValueError(f"Not a directory: {source}")
    top = Path(git(source, "rev-parse", "--show-toplevel")).resolve()
    if top != source:
        raise ValueError(f"Expected the checkout root for {component.option}: {source}")
    head = git(source, "rev-parse", "HEAD")
    if head != component.pin:
        raise ValueError(f"{component.option} HEAD is {head}; expected {component.pin}")
    if git(source, "cat-file", "-t", component.pin) != "commit":
        raise ValueError(f"{component.option} pin is not a commit")
    return source


def check_kit() -> None:
    for component in COMPONENTS:
        patch = KIT / "patches" / component.patch
        if not patch.is_file() or patch.is_symlink():
            raise ValueError(f"Missing regular patch: {patch}")
    local_patch = KIT / "patches/third-party-local-changes.patch"
    if not local_patch.is_file() or local_patch.is_symlink():
        raise ValueError(f"Missing regular patch: {local_patch}")
    for module in ("economy", "guilds", "war"):
        directory = KIT / "source/modules" / f"mod-living-azeroth-{module}"
        if not directory.is_dir() or directory.is_symlink():
            raise ValueError(f"Missing module: {directory}")
        if any(path.is_symlink() for path in directory.rglob("*")):
            raise ValueError(f"Module contains a symlink: {directory}")


def export_commit(source: Path, pin: str, destination: Path, *, core: bool = False) -> None:
    destination.mkdir(parents=True, exist_ok=False)
    process = subprocess.Popen(
        ["git", "-C", str(source), "archive", "--format=tar", pin],
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
    )
    assert process.stdout is not None
    try:
        with tarfile.open(fileobj=process.stdout, mode="r|") as archive:
            for member in archive:
                if member.issym():
                    # The pinned core has one upstream development-tool link.
                    # It is outside the game build and absent from the public
                    # reconstruction delta. Reject every other link.
                    if core and member.name == ".claude/skills/generate-pr-description" and member.linkname == "../../.agents/skills/generate-pr-description":
                        continue
                    raise ValueError(f"Unexpected archive symlink: {member.name}")
                # The data filter rejects paths and links that leave destination.
                archive.extract(member, path=destination, filter="data")
        error = process.stderr.read() if process.stderr else b""
        if process.wait():
            raise RuntimeError(f"git archive failed: {error.decode(errors='replace')[-1200:]}")
    except BaseException:
        process.kill()
        process.wait()
        raise
    finally:
        process.stdout.close()
        if process.stderr:
            process.stderr.close()


def apply_patch(destination: Path, patch: Path, *, strip: int = 1) -> None:
    for options in (("--check",), ()):
        result = subprocess.run(
            ["git", "apply", f"-p{strip}", *options, str(patch)],
            cwd=destination,
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
            text=True,
            errors="replace",
        )
        if result.returncode:
            raise RuntimeError(f"Patch failed in {destination}: {patch.name}: {result.stderr[-1200:]}")


def reconstruct(inputs: dict[str, Path], output: Path) -> None:
    check_kit()
    sources = {component.option: check_checkout(inputs[component.option], component) for component in COMPONENTS}
    output = output.expanduser().resolve()
    if output.exists():
        raise FileExistsError(f"Output already exists: {output}")
    if not output.parent.is_dir():
        raise ValueError(f"Create the output parent first: {output.parent}")
    for root in (KIT, *sources.values()):
        if output == root or root in output.parents:
            raise ValueError(f"Output must be outside the kit and input checkouts: {output}")

    output.mkdir(mode=0o700)
    try:
        # The core is exported into the reserved output itself.
        for component in COMPONENTS:
            destination = output if component.option == "core" else output / component.destination
            if component.option == "core":
                # export_commit requires an absent destination. Export to a new
                # child, then move entries into the already reserved output.
                exported = output / ".core-export"
                export_commit(sources[component.option], component.pin, exported, core=True)
                for entry in exported.iterdir():
                    entry.rename(output / entry.name)
                exported.rmdir()
            else:
                export_commit(sources[component.option], component.pin, destination)
            apply_patch(destination, KIT / "patches" / component.patch)
            print(f"Reconstructed {component.option} at {component.pin}", flush=True)

        for module in ("economy", "guilds", "war"):
            name = f"mod-living-azeroth-{module}"
            shutil.copytree(KIT / "source/modules" / name, output / "modules" / name)
        apply_patch(output, KIT / "patches/third-party-local-changes.patch", strip=2)
        print(f"Reconstruction complete: {output}")
    except BaseException:
        print(f"Reconstruction stopped; inspect the preserved output: {output}", file=sys.stderr)
        raise


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    for component in COMPONENTS:
        parser.add_argument(f"--{component.option}-source", type=Path, required=True, help="local checkout at the documented pin")
    parser.add_argument("--output-dir", type=Path, required=True, help="new, nonexistent destination; parent must exist")
    args = parser.parse_args()
    reconstruct({component.option: getattr(args, f"{component.option}_source") for component in COMPONENTS}, args.output_dir)


if __name__ == "__main__":
    main()
