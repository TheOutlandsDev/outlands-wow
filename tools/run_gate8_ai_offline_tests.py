#!/usr/bin/env python3
"""Compile and run every standalone Living Azeroth AI policy test offline."""

from __future__ import annotations

import argparse
import datetime as dt
import hashlib
import json
import os
import shlex
import shutil
import subprocess
from dataclasses import dataclass
from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]
CHAT = ROOT / "source" / "modules" / "mod-ollama-chat"
ECONOMY = ROOT / "source" / "modules" / "mod-living-azeroth-economy"
GUILDS = ROOT / "source" / "modules" / "mod-living-azeroth-guilds"
PLAYERBOTS = ROOT / "source" / "modules" / "mod-playerbots"
PROFILE_RENDERER = ROOT / "deploy" / "gate8-ai-profile.sh"


@dataclass(frozen=True)
class TestSpec:
    name: str
    group: str
    sources: tuple[Path, ...]
    arguments: tuple[str, ...] = ()
    definitions: tuple[str, ...] = ()
    libraries: tuple[str, ...] = ()


def now() -> dt.datetime:
    return dt.datetime.now(dt.timezone.utc)


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
    return digest.hexdigest()


def write_json(path: Path, value: object) -> None:
    path.write_text(json.dumps(value, indent=2) + "\n", encoding="utf-8")


def prepare_rag_fixtures(root: Path) -> tuple[Path, Path, Path]:
    invalid = root / "invalid"
    valid = root / "valid"
    tampered = root / "tampered"
    invalid.mkdir(parents=True)
    valid.mkdir(parents=True)

    records = [
        {
            "id": "creature-hogger",
            "title": "Hogger",
            "content": "Hogger patrols near the road in southwestern Elwynn Forest.",
            "keywords": ["Hogger", "creature", "Elwynn"],
            "tags": ["creature", "quest"],
            "client_build": 12340,
            "provenance_id": "acore-world-db-12340",
        },
        {
            "id": "city-stormwind",
            "title": "Stormwind City",
            "content": "Stormwind is the Alliance capital in Elwynn Forest.",
            "keywords": ["Stormwind", "Alliance"],
            "tags": ["city"],
            "client_build": 12340,
            "provenance_id": "acore-world-db-12340",
        },
        {
            "id": "profession-first-aid",
            "title": "First Aid",
            "content": "First Aid lets adventurers create bandages from cloth.",
            "keywords": ["First Aid", "bandage"],
            "tags": ["profession"],
            "client_build": 12340,
            "provenance_id": "acore-world-db-12340",
        },
    ]
    corpus = valid / "corpus.json"
    write_json(corpus, records)
    manifest = {
        "schema_version": 1,
        "client_build": 12340,
        "expansion": 2,
        "locale": "enUS",
        "test_only": False,
        "corpus_version": "offline-rag-valid-test",
        "total_records": len(records),
        "provenance": [
            {
                "id": "acore-world-db-12340",
                "source_kind": "azerothcore_world_db",
                "source_locator": "offline deterministic fixture",
                "license": "project-test-fixture",
                "approved": True,
            }
        ],
        "files": [
            {
                "file": corpus.name,
                "sha256": sha256(corpus),
                "provenance_id": "acore-world-db-12340",
                "record_count": len(records),
            }
        ],
    }
    write_json(valid / "manifest.json", manifest)
    shutil.copytree(valid, tampered)
    with (tampered / "corpus.json").open("a", encoding="utf-8") as handle:
        handle.write(" ")
    return invalid, valid, tampered


def read_settings(path: Path) -> dict[str, str]:
    settings: dict[str, str] = {}
    for raw_line in path.read_text(encoding="utf-8").splitlines():
        line = raw_line.strip()
        if not line or line.startswith("#") or "=" not in line:
            continue
        key, value = line.split("=", 1)
        settings[key.strip()] = value.strip()
    return settings


def run_profile_tests(root: Path, log) -> list[dict[str, object]]:
    templates = (
        ROOT / "source/modules/mod-playerbots/conf/playerbots.conf.dist",
        ROOT / "source/modules/mod-ollama-chat/conf/mod_ollama_chat.conf.dist",
        ROOT / "source/modules/mod-ah-bot/conf/mod_ahbot.conf.dist",
        ROOT / "source/modules/mod-living-azeroth-war/conf/living_azeroth_war.conf.dist",
        ROOT / "source/modules/mod-living-azeroth-war/conf/living_azeroth_population.conf.dist",
        ROOT / "source/modules/mod-living-azeroth-war/conf/living_azeroth_director.conf.dist",
        ROOT / "source/modules/mod-living-azeroth-guilds/conf/living_azeroth_guilds.conf.dist",
        ROOT / "source/modules/mod-living-azeroth-economy/conf/living_azeroth_economy.conf.dist",
    )
    expected_profile = {
        "disabled": ("0", "0", "0", "0", "0"),
        "bots-small": ("1", "1", "0", "0", "0"),
        "bots-dialogue-small": ("1", "1", "1", "0", "0"),
        "bots-local-dialogue-small": ("1", "1", "1", "0", "1"),
        "conversation-small": ("1", "1", "0", "1", "0"),
        "bots-ollama-dialogue-small": ("1", "1", "0", "1", "1"),
        "bots-ollama-routed-small": ("1", "1", "0", "1", "1"),
    }
    results: list[dict[str, object]] = []
    for profile, expected in expected_profile.items():
        directory = root / profile
        directory.mkdir(parents=True)
        for template in templates:
            shutil.copy2(template, directory / template.name.removesuffix(".dist"))
        output = execute(["bash", str(PROFILE_RENDERER), str(directory), profile], log)
        playerbots = read_settings(directory / "playerbots.conf")
        ollama = read_settings(directory / "mod_ollama_chat.conf")
        actual = (
            playerbots["AiPlayerbot.Enabled"],
            playerbots["AiPlayerbot.RandomBotAutologin"],
            playerbots["AiPlayerbot.LivingAzerothChat.Enable"],
            ollama["OllamaChat.Enable"],
            playerbots["AiPlayerbot.PlayerLocalPopulation.Enable"],
        )
        if actual != expected:
            raise RuntimeError(f"profile {profile} rendered {actual}, expected {expected}")
        if playerbots["AiPlayerbot.MinRandomBots"] != "10" or playerbots[
            "AiPlayerbot.MaxRandomBots"
        ] != "10":
            raise RuntimeError(f"profile {profile} does not enforce the 10-bot limit")
        if playerbots["AiPlayerbot.AddClassAccountPoolSize"] != "0":
            raise RuntimeError(f"profile {profile} permits add-class account provisioning")
        if playerbots["AiPlayerbot.RandomBotTalk"] != "0" or playerbots[
            "AiPlayerbot.EnableBroadcasts"
        ] != "0":
            raise RuntimeError(f"profile {profile} permits native ambient chatter")
        if playerbots["AiPlayerbot.AdaptiveLevelCohort.Enable"] != "0":
            raise RuntimeError(f"profile {profile} permits level cohort mutation")
        if profile in {
            "bots-local-dialogue-small",
            "bots-ollama-dialogue-small",
            "bots-ollama-routed-small",
        }:
            local_expected = {
                "AiPlayerbot.PlayerLocalPopulation.MinimumPerPlayer": "2",
                "AiPlayerbot.PlayerLocalPopulation.TargetPerPlayer": "4",
                "AiPlayerbot.PlayerLocalPopulation.MaximumPerPlayer": "4",
                "AiPlayerbot.PlayerLocalPopulation.MaximumLevelDelta": "8",
                "AiPlayerbot.PlayerLocalPopulation.MaxRelocationsPerCycle": "2",
                "AiPlayerbot.PlayerLocalPopulation.MinimumArrivalDistance": "160",
                "AiPlayerbot.PlayerLocalPopulation.MaximumArrivalDistance": "350",
            }
            for key, value in local_expected.items():
                if playerbots[key] != value:
                    raise RuntimeError(
                        f"local population profile rendered {key}={playerbots[key]}"
                    )
        always_off = {
            "mod_ahbot.conf": ("AuctionHouseBot.EnableSeller", "AuctionHouseBot.EnableBuyer"),
            "living_azeroth_war.conf": ("War.Enable", "War.AutoStart"),
            "living_azeroth_population.conf": ("Population.Enable", "Population.AutoStart"),
            "living_azeroth_director.conf": ("Director.Enable",),
            "living_azeroth_guilds.conf": ("LivingAzerothGuild.Enable",),
            "living_azeroth_economy.conf": ("LivingAzeroth.Economy.Enable",),
        }
        for name, keys in always_off.items():
            rendered = read_settings(directory / name)
            for key in keys:
                if rendered[key] != "0":
                    raise RuntimeError(f"profile {profile} enabled deferred control {key}")
        if profile == "conversation-small":
            if ollama["OllamaChat.Url"] != "http://ollama:11434/api/generate":
                raise RuntimeError("conversation profile has the wrong private endpoint")
            if ollama["OllamaChat.Model"] != "qwen2.5:0.5b":
                raise RuntimeError("conversation profile has the wrong reviewed model")
            if ollama["OllamaChat.BackendProbe.Enable"] != "1":
                raise RuntimeError("conversation profile does not require a backend probe")
        if profile in {"bots-ollama-dialogue-small", "bots-ollama-routed-small"}:
            expected_ollama = {
                "OllamaChat.DebugEnabled": "0",
                "OllamaChat.DebugShowFullPrompt": "0",
                "OllamaChat.Telemetry.Enable": "0",
                "OllamaChat.Url": "http://ollama:11434/api/generate",
                "OllamaChat.Model": "qwen2.5:7b-instruct",
                "OllamaChat.WorkerCount": "1",
                "OllamaChat.MaxQueuedQueries": "8",
                "OllamaChat.MaxPendingPerPlayer": "1",
                "OllamaChat.MaxPendingPerBot": "1",
                "OllamaChat.EnableChatHistory": "0",
                "OllamaChat.ConversationHistorySaveInterval": "0",
                "OllamaChat.ConversationContinuity.Enable": "0",
                "OllamaChat.EnableRPPersonalities": "0",
                "OllamaChat.BackendProbe.Enable": "0",
                "OllamaChat.PlayerReplyChance.Say": "0",
                "OllamaChat.PlayerReplyChance.Channel": "0",
                "OllamaChat.PlayerReplyChance.Party": "0",
                "OllamaChat.PlayerReplyChance.Guild": "0",
                "OllamaChat.DisableForCustomChannels": "1",
                "OllamaChat.DisableForGuild": "1",
                "OllamaChat.DisableForParty": "1",
                "OllamaChat.EnableChatBotSnapshotTemplate": "0",
            }
            for key, value in expected_ollama.items():
                if ollama[key] != value:
                    raise RuntimeError(f"Ollama dialogue profile rendered {key}={ollama[key]}")
            expected_routing = "1" if profile == "bots-ollama-routed-small" else "0"
            for key in (
                "OllamaChat.DeterministicIntentRouting.Enable",
                "OllamaChat.BoundedContext.Enable",
            ):
                if ollama[key] != expected_routing:
                    raise RuntimeError(
                        f"Ollama dialogue profile rendered {key}={ollama[key]}"
                    )
            if profile == "bots-ollama-routed-small":
                if "{bounded_context}" not in ollama["OllamaChat.ChatPromptTemplate"]:
                    raise RuntimeError("routed profile does not include bounded context")
                if "{reply_goal}" not in ollama["OllamaChat.ChatPromptTemplate"]:
                    raise RuntimeError("routed profile does not include the reply goal")
            probe_directory = root / f"{profile}-probe"
            probe_directory.mkdir(parents=True)
            for template in templates:
                shutil.copy2(template, probe_directory / template.name.removesuffix(".dist"))
            execute(
                ["bash", str(PROFILE_RENDERER), str(probe_directory), profile, "1"], log
            )
            probe_ollama = read_settings(probe_directory / "mod_ollama_chat.conf")
            if probe_ollama["OllamaChat.BackendProbe.Enable"] != "1":
                raise RuntimeError("Ollama candidate backend probe was not enabled explicitly")
        results.append(
            {
                "name": f"ai_profile_{profile}",
                "group": "configuration",
                "result": "PASS",
                "output": output.stdout.strip(),
            }
        )

    rejected = subprocess.run(
        ["bash", str(PROFILE_RENDERER), str(root / "disabled"), "unsupported"],
        cwd=ROOT,
        stdout=subprocess.PIPE,
        stderr=subprocess.STDOUT,
        text=True,
        check=False,
    )
    if rejected.returncode == 0:
        raise RuntimeError("profile renderer accepted an unsupported profile")
    results.append(
        {
            "name": "ai_profile_unknown_rejected",
            "group": "configuration",
            "result": "PASS",
            "output": "PASS unsupported profile rejected",
        }
    )
    return results


def run_population_runtime_contract_test() -> dict[str, object]:
    source = (PLAYERBOTS / "src/Bot/RandomPlayerbotMgr.cpp").read_text(encoding="utf-8")
    start = source.index("bool RandomPlayerbotMgr::RelocateBotNearPlayer")
    end = source.index("void RandomPlayerbotMgr::UpdateAdaptiveLevelCohort", start)
    relocation = source[start:end]
    required = (
        "PathGenerator path(player)",
        "path.GetActualEndPosition()",
        "IsUsableArrivalPath",
        "IsArrivalDistanceSafe",
        "diagnostics.pathUnavailable",
        "diagnostics.wrongZone",
        "diagnostics.visibleToPlayer",
    )
    missing = [marker for marker in required if marker not in relocation]
    if missing:
        raise RuntimeError(f"player-local navmesh contract is incomplete: {missing}")
    if "GetReachableRandomPointOnGround(player" in relocation:
        raise RuntimeError("player-local placement regressed to straight-line arrival checks")

    population_start = source.index("void RandomPlayerbotMgr::UpdatePlayerLocalPopulation")
    population_end = source.index("void RandomPlayerbotMgr::ScheduleRandomize", population_start)
    population = source[population_start:population_end]
    if "player->GetName()" in population or "bot->GetName()" in population:
        raise RuntimeError("player-local telemetry records character identity")

    return {
        "name": "player_local_population_runtime_contract",
        "group": "population",
        "result": "PASS",
        "output": "PASS navmesh_endpoint=1 straight_line_guard=1 bounded_distance=1 sanitized_telemetry=1",
    }


def test_specs(fixtures: Path, telemetry: Path) -> list[TestSpec]:
    chat_tests = [
        TestSpec("chat_policy_test", "conversation", (CHAT / "tests/chat_policy_test.cpp",)),
        TestSpec("intent_route_test", "conversation", (CHAT / "tests/intent_route_test.cpp",)),
        TestSpec("continuity_store_test", "conversation", (CHAT / "tests/continuity_store_test.cpp",)),
        TestSpec("social_coordinator_test", "conversation", (CHAT / "tests/social_coordinator_test.cpp",)),
        TestSpec(
            "query_priority_test",
            "conversation",
            (CHAT / "tests/query_priority_test.cpp", CHAT / "src/mod-ollama-chat_querymanager.cpp"),
            libraries=("-pthread",),
        ),
        TestSpec(
            "rag_manifest_test",
            "conversation",
            (CHAT / "tests/rag_manifest_test.cpp", CHAT / "src/mod-ollama-chat_rag.cpp"),
            arguments=tuple(str(path) for path in prepare_rag_fixtures(fixtures)),
            definitions=("OLLAMA_CHAT_RAG_STANDALONE_TEST",),
            libraries=("-lcrypto",),
        ),
        TestSpec(
            "telemetry_store_test",
            "conversation",
            (CHAT / "tests/telemetry_store_test.cpp", CHAT / "src/mod-ollama-chat_telemetry.cpp"),
            arguments=(str(telemetry),),
            libraries=("-pthread", "-lcrypto"),
        ),
    ]
    economy_tests = [
        TestSpec(path.stem, "economy", (path,))
        for path in sorted((ECONOMY / "tests").glob("*.cpp"))
    ]
    guild_tests = [
        TestSpec(path.stem, "guild", (path,))
        for path in sorted((GUILDS / "tests").glob("*.cpp"))
    ]
    frontend_tests = [
        TestSpec(
            "LivingAzerothChatPolicyTests",
            "frontend-dialogue",
            (PLAYERBOTS / "tests/LivingAzerothChatPolicyTests.cpp",),
        ),
        TestSpec(
            "PlayerLocalPopulationPolicyTests",
            "population",
            (PLAYERBOTS / "tests/PlayerLocalPopulationPolicyTests.cpp",),
        ),
    ]
    return frontend_tests + chat_tests + economy_tests + guild_tests


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--compiler", default=os.environ.get("CXX", "g++"))
    parser.add_argument("--evidence-root", type=Path)
    args = parser.parse_args()

    stamp = now().strftime("%Y%m%d-%H%M%SZ")
    evidence = args.evidence_root or ROOT / "run" / "private" / "gate8" / f"{stamp}-ai-offline"
    binaries = evidence / "bin"
    fixtures = evidence / "fixtures" / "rag"
    telemetry = evidence / "fixtures" / "telemetry"
    binaries.mkdir(parents=True, exist_ok=False)
    log_path = evidence / "offline-tests.log"
    status_path = evidence / "status.json"
    status: dict[str, object] = {
        "change_id": "ai-restoration-gate8-offline",
        "started_utc": now().isoformat(),
        "network_used": False,
        "production_runtime_changed": False,
        "result": "IN_PROGRESS",
        "tests": [],
    }

    return_code = 1
    try:
        with log_path.open("w", encoding="utf-8") as log:
            status["source_commit"] = execute(["git", "rev-parse", "HEAD"], log).stdout.strip()
            status["source_dirty"] = bool(execute(["git", "status", "--porcelain"], log).stdout.strip())
            status["compiler"] = execute([args.compiler, "--version"], log).stdout.splitlines()[0]
            results = run_profile_tests(evidence / "fixtures" / "profiles", log)
            results.append(run_population_runtime_contract_test())
            status["tests"] = results
            for spec in test_specs(fixtures, telemetry):
                executable = binaries / spec.name
                command = [
                    args.compiler,
                    "-std=c++20",
                    "-Wall",
                    "-Wextra",
                    "-Wpedantic",
                    f"-I{CHAT / 'tests/stubs'}",
                    f"-I{CHAT / 'src'}",
                    f"-I{CHAT / 'deps'}",
                    f"-I{ECONOMY / 'src'}",
                    f"-I{GUILDS / 'src'}",
                    f"-I{PLAYERBOTS / 'src/Ai/Base/Actions'}",
                ]
                command.extend(f"-D{definition}" for definition in spec.definitions)
                command.extend(str(path) for path in spec.sources)
                command.extend(("-o", str(executable)))
                command.extend(spec.libraries)
                execute(command, log)
                result = execute([str(executable), *spec.arguments], log)
                results.append(
                    {
                        "name": spec.name,
                        "group": spec.group,
                        "result": "PASS",
                        "sha256": sha256(executable),
                        "output": result.stdout.strip() or "PASS (exit code 0)",
                    }
                )
                status["tests"] = results
            status["tests"] = results
            status["counts"] = {
                group: sum(1 for result in results if result["group"] == group)
                for group in (
                    "configuration",
                    "frontend-dialogue",
                    "conversation",
                    "economy",
                    "guild",
                    "population",
                )
            }
        status["result"] = "PASS"
        return_code = 0
    except (OSError, RuntimeError, subprocess.CalledProcessError) as exc:
        status["result"] = "FAIL"
        status["error"] = str(exc)
    finally:
        status["finished_utc"] = now().isoformat()
        status_path.write_text(json.dumps(status, indent=2) + "\n", encoding="utf-8")

    print(json.dumps(status, indent=2))
    print(f"Evidence: {evidence}")
    return return_code


if __name__ == "__main__":
    raise SystemExit(main())
