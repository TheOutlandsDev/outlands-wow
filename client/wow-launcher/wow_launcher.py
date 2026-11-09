#!/usr/bin/env python3
"""Player-safe Living Azeroth Windows launcher for the Gate 5 smoke test."""

from __future__ import annotations

import argparse
import dataclasses
import datetime as dt
import ipaddress
import json
import os
import platform
import shutil
import socket
import subprocess
import sys
from pathlib import Path
from typing import Any


APP_NAME = "Living Azeroth Launcher"
APP_VERSION = "0.1.0-gate5"
BASE_DIR = Path(__file__).resolve().parent
MANIFEST_PATH = BASE_DIR / "servers.json"
BACKUP_SUFFIX = ".living-azeroth-backup"
REALMLIST_TEXT = "set realmlist {address}\r\n"


class LauncherError(RuntimeError):
    """A player-safe, user-facing error."""


@dataclasses.dataclass(frozen=True)
class Server:
    server_id: str
    name: str
    game: str
    build: int
    address: str
    auth_port: int
    world_port: int
    access: str
    requires_tailscale: bool


@dataclasses.dataclass(frozen=True)
class NetworkCheck:
    tailscale_binary: Path
    local_tailscale_ips: tuple[str, ...]
    peer_found: bool
    peer_online: bool
    tsmp_ok: bool
    auth_tcp_ok: bool
    world_tcp_ok: bool

    @property
    def ready(self) -> bool:
        return all(
            (
                self.peer_found,
                self.peer_online,
                self.tsmp_ok,
                self.auth_tcp_ok,
                self.world_tcp_ok,
            )
        )


def load_server(path: Path = MANIFEST_PATH) -> Server:
    try:
        payload = json.loads(path.read_text(encoding="utf-8"))
    except (OSError, json.JSONDecodeError) as exc:
        raise LauncherError(f"Could not read servers.json: {exc}") from exc
    if payload.get("schema_version") != 1:
        raise LauncherError("Unsupported servers.json schema version.")
    entries = payload.get("servers")
    if not isinstance(entries, list) or len(entries) != 1:
        raise LauncherError("Gate 5 requires exactly one server entry.")
    raw = entries[0]
    try:
        server = Server(
            server_id=str(raw["id"]),
            name=str(raw["name"]),
            game=str(raw["game"]),
            build=int(raw["build"]),
            address=str(raw["address"]),
            auth_port=int(raw["auth_port"]),
            world_port=int(raw["world_port"]),
            access=str(raw["access"]),
            requires_tailscale=bool(raw["requires_tailscale"]),
        )
    except (KeyError, TypeError, ValueError) as exc:
        raise LauncherError(f"Invalid server entry: {exc}") from exc
    try:
        parsed = ipaddress.ip_address(server.address)
    except ValueError as exc:
        raise LauncherError("The server address is not IPv4.") from exc
    if (
        not isinstance(parsed, ipaddress.IPv4Address)
        or parsed not in ipaddress.ip_network("100.64.0.0/10")
        or server.access != "tailscale-only"
    ):
        raise LauncherError("The Gate 5 server must use a Tailscale IPv4 address.")
    if server.build != 12340 or not server.requires_tailscale:
        raise LauncherError("The launcher requires WoW build 12340 and Tailscale.")
    if not all(1 <= port <= 65535 for port in (server.auth_port, server.world_port)):
        raise LauncherError("The server manifest contains an invalid TCP port.")
    return server


def run_command(command: list[str], timeout: float = 12.0) -> subprocess.CompletedProcess[str]:
    try:
        return subprocess.run(
            command,
            capture_output=True,
            text=True,
            errors="replace",
            timeout=timeout,
            check=False,
            creationflags=(
                subprocess.CREATE_NO_WINDOW if platform.system() == "Windows" else 0
            ),
        )
    except (OSError, subprocess.TimeoutExpired) as exc:
        raise LauncherError(f"Could not run {Path(command[0]).name}: {exc}") from exc


def find_tailscale() -> Path | None:
    candidates: list[Path] = []
    located = shutil.which("tailscale") or shutil.which("tailscale.exe")
    if located:
        candidates.append(Path(located))
    if platform.system() == "Windows":
        for variable in ("ProgramFiles", "ProgramFiles(x86)"):
            if root := os.environ.get(variable):
                candidates.append(Path(root) / "Tailscale" / "tailscale.exe")
    return next((path for path in candidates if path.is_file()), None)


def flatten_peers(payload: dict[str, Any]) -> list[dict[str, Any]]:
    peers = payload.get("Peer", {})
    if isinstance(peers, dict):
        return [peer for peer in peers.values() if isinstance(peer, dict)]
    if isinstance(peers, list):
        return [peer for peer in peers if isinstance(peer, dict)]
    return []


def tcp_probe(address: str, port: int, timeout: float = 4.0) -> bool:
    try:
        with socket.create_connection((address, port), timeout=timeout):
            return True
    except OSError:
        return False


def check_network(server: Server) -> NetworkCheck:
    binary = find_tailscale()
    if binary is None:
        raise LauncherError("Tailscale CLI was not found. Install or start Tailscale first.")
    status = run_command([str(binary), "status", "--json"])
    try:
        payload = json.loads(status.stdout)
    except json.JSONDecodeError as exc:
        raise LauncherError("Tailscale status did not return valid JSON.") from exc
    if status.returncode != 0 or payload.get("BackendState") != "Running":
        raise LauncherError("Tailscale is not in the Running state.")
    self_value = payload.get("Self", {})
    local_ips = tuple(
        str(value)
        for value in self_value.get("TailscaleIPs", [])
        if isinstance(value, str)
    ) if isinstance(self_value, dict) else ()
    peer = next(
        (
            item
            for item in flatten_peers(payload)
            if server.address in item.get("TailscaleIPs", [])
        ),
        None,
    )
    peer_found = peer is not None
    peer_online = bool(peer and peer.get("Online"))
    ping = run_command(
        [str(binary), "ping", "--tsmp", "--c", "1", server.address], timeout=15
    )
    return NetworkCheck(
        tailscale_binary=binary,
        local_tailscale_ips=local_ips,
        peer_found=peer_found,
        peer_online=peer_online,
        tsmp_ok=ping.returncode == 0,
        auth_tcp_ok=tcp_probe(server.address, server.auth_port),
        world_tcp_ok=tcp_probe(server.address, server.world_port),
    )


def settings_directory() -> Path:
    root = Path(os.environ.get("LOCALAPPDATA", Path.home() / "AppData" / "Local"))
    return root / "LivingAzerothLauncher"


def load_saved_wow_directory() -> Path | None:
    try:
        value = json.loads(
            (settings_directory() / "settings.json").read_text(encoding="utf-8")
        ).get("wow_directory")
    except (OSError, json.JSONDecodeError, AttributeError):
        return None
    return Path(value) if isinstance(value, str) and value else None


def save_wow_directory(path: Path) -> None:
    directory = settings_directory()
    directory.mkdir(parents=True, exist_ok=True)
    temporary = directory / "settings.json.tmp"
    temporary.write_text(
        json.dumps({"wow_directory": str(path)}, indent=2) + "\n", encoding="utf-8"
    )
    temporary.replace(directory / "settings.json")


def find_wow_directory(explicit: Path | None = None) -> Path | None:
    candidates: list[Path] = []
    if explicit:
        candidates.append(explicit)
    if configured := os.environ.get("WOW_335A_DIR"):
        candidates.append(Path(configured))
    if saved := load_saved_wow_directory():
        candidates.append(saved)
    candidates.extend((BASE_DIR, BASE_DIR.parent, BASE_DIR / "World of Warcraft"))
    for variable in ("ProgramFiles", "ProgramFiles(x86)"):
        if root := os.environ.get(variable):
            for name in ("World of Warcraft 3.3.5a", "World of Warcraft"):
                candidates.append(Path(root) / name)
    for candidate in candidates:
        candidate = candidate.expanduser()
        if candidate.is_file() and candidate.name.lower() == "wow.exe":
            candidate = candidate.parent
        if (candidate / "Wow.exe").is_file():
            return candidate.resolve()
    return None


def find_realmlist(wow_directory: Path) -> Path:
    data = wow_directory / "Data"
    preferred = [data / locale / "realmlist.wtf" for locale in ("enUS", "enGB")]
    matches = [path for path in preferred if path.is_file()]
    matches.extend(
        path for path in sorted(data.glob("*/realmlist.wtf")) if path not in matches
    )
    if len(matches) != 1:
        raise LauncherError(
            f"Expected exactly one Data/<locale>/realmlist.wtf; found {len(matches)}."
        )
    resolved = matches[0].resolve()
    try:
        resolved.relative_to(wow_directory.resolve())
    except ValueError as exc:
        raise LauncherError("The realmlist resolves outside the selected WoW folder.") from exc
    return resolved


def backup_path(realmlist: Path) -> Path:
    return realmlist.with_name(realmlist.name + BACKUP_SUFFIX)


def install_temporary_realmlist(realmlist: Path, address: str) -> Path:
    backup = backup_path(realmlist)
    if backup.exists():
        raise LauncherError(
            "A realmlist recovery backup already exists. Run --restore-realmlist first."
        )
    original = realmlist.read_bytes()
    with backup.open("xb") as output:
        output.write(original)
    temporary = realmlist.with_name(realmlist.name + ".living-azeroth-tmp")
    try:
        temporary.write_text(REALMLIST_TEXT.format(address=address), encoding="ascii")
        temporary.replace(realmlist)
    except Exception:
        realmlist.write_bytes(original)
        temporary.unlink(missing_ok=True)
        backup.unlink(missing_ok=True)
        raise
    return backup


def restore_realmlist(realmlist: Path) -> bool:
    backup = backup_path(realmlist)
    if not backup.is_file():
        return False
    temporary = realmlist.with_name(realmlist.name + ".living-azeroth-restore")
    temporary.write_bytes(backup.read_bytes())
    temporary.replace(realmlist)
    backup.unlink()
    return True


def write_result(check: NetworkCheck, *, launched: bool, restored: bool, exit_code: int | None) -> None:
    directory = settings_directory()
    directory.mkdir(parents=True, exist_ok=True)
    payload = {
        "schema_version": 1,
        "launcher_version": APP_VERSION,
        "finished_utc": dt.datetime.now(dt.UTC).isoformat(),
        "local_tailscale_ips": list(check.local_tailscale_ips),
        "peer_found": check.peer_found,
        "peer_online": check.peer_online,
        "tsmp_ok": check.tsmp_ok,
        "auth_tcp_ok": check.auth_tcp_ok,
        "world_tcp_ok": check.world_tcp_ok,
        "client_launched": launched,
        "client_exit_code": exit_code,
        "realmlist_restored": restored,
        "credentials_recorded": False,
        "player_identity_recorded": False,
    }
    temporary = directory / "client-smoke-result.json.tmp"
    temporary.write_text(json.dumps(payload, indent=2) + "\n", encoding="utf-8")
    temporary.replace(directory / "client-smoke-result.json")


def print_check(check: NetworkCheck) -> None:
    values = (
        ("Tailscale peer found", check.peer_found),
        ("Tailscale peer online", check.peer_online),
        ("TSMP ping", check.tsmp_ok),
        ("Authentication TCP 3724", check.auth_tcp_ok),
        ("World TCP 8085", check.world_tcp_ok),
    )
    for label, passed in values:
        print(f"[{'PASS' if passed else 'FAIL'}] {label}")
    if check.local_tailscale_ips:
        print("This client Tailscale IP: " + ", ".join(check.local_tailscale_ips))


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--check", action="store_true", help="check transport without launching WoW")
    parser.add_argument("--wow-dir", type=Path, help="folder containing Wow.exe")
    parser.add_argument(
        "--restore-realmlist",
        action="store_true",
        help="restore an interrupted launcher's realmlist backup",
    )
    args = parser.parse_args()
    if sys.version_info < (3, 12):
        raise LauncherError("Python 3.12 or newer is required.")
    server = load_server()
    wow_directory = find_wow_directory(args.wow_dir)
    if args.restore_realmlist:
        if wow_directory is None:
            raise LauncherError("Could not find Wow.exe; supply --wow-dir.")
        restored = restore_realmlist(find_realmlist(wow_directory))
        print("Realmlist restored." if restored else "No recovery backup was present.")
        return 0

    check = check_network(server)
    print_check(check)
    write_result(check, launched=False, restored=False, exit_code=None)
    if not check.ready:
        raise LauncherError("Gate 5 transport checks did not all pass.")
    if args.check:
        print("Gate 5 client transport is ready.")
        return 0
    if wow_directory is None:
        if sys.stdin.isatty():
            entered = input("Folder containing Wow.exe: ").strip().strip('"')
            wow_directory = find_wow_directory(Path(entered)) if entered else None
        if wow_directory is None:
            raise LauncherError(
                "Could not find Wow.exe; rerun with --wow-dir C:\\path\\to\\WoW."
            )
    realmlist = find_realmlist(wow_directory)
    save_wow_directory(wow_directory)
    install_temporary_realmlist(realmlist, server.address)
    restored = False
    exit_code: int | None = None
    try:
        print("Launching WoW. Enter one character, remain in world for at least 30 seconds,")
        print("then log out and close WoW. The original realmlist will be restored.")
        process = subprocess.Popen([str(wow_directory / "Wow.exe")], cwd=wow_directory)
        exit_code = process.wait()
    finally:
        restored = restore_realmlist(realmlist)
        write_result(check, launched=True, restored=restored, exit_code=exit_code)
    if not restored:
        raise LauncherError("WoW exited, but the original realmlist was not restored.")
    return exit_code or 0


if __name__ == "__main__":
    try:
        raise SystemExit(main())
    except LauncherError as exc:
        print(f"ERROR: {exc}", file=sys.stderr)
        raise SystemExit(1)
