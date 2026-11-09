#!/usr/bin/env python3
"""Exercise Gate 10 Ollama routing with synthetic context and mirror its delivery guard."""

from __future__ import annotations

import argparse
import datetime as dt
import hashlib
import json
import os
import re
import sys
from pathlib import Path

import run_gate9_ollama_backend_eval as gate9


ROOT = Path(__file__).resolve().parents[1]
MODEL = "qwen2.5:7b-instruct"
BOUNDED_CONTEXT = (
    "activity=stationary; health=healthy; active_quest_count=2; "
    "group_size=1; area=Goldshire; zone=Elwynn Forest."
)
CASES = (
    ("activity", "What are you doing here?", "I am stationary here right now."),
    ("direction", "Where should I go next?", "I do not know where you should go next."),
    (
        "inventory",
        "Do you have linen to sell?",
        "I cannot confirm any requested item is available to trade.",
    ),
    ("identity", "Who are you?", "I am Rowan, a level 12 Warrior."),
    ("quest", "Which quests are you doing?", "I have 2 active quests right now."),
    (
        "advice",
        "Any advice for a mage?",
        "Practice carefully and learn which of your existing spells suit you.",
    ),
    ("greeting", "Hello there!", "Hello there, traveler."),
    ("general", "The road seems quiet today.", "I am not sure how to answer that."),
)


def canonical(text: str) -> str:
    return " ".join(re.findall(r"[a-z0-9]+", text.lower()))


def is_grounded_reply(intent: str, response: str, contract: str) -> bool:
    value = canonical(response)
    if value == canonical(contract):
        return True
    alternatives = {
        "activity": {
            "i am stationary here",
            "i am currently stationary",
            "currently i am stationary",
        },
        "direction": {
            "i don t know where you should go next",
            "i have no verified destination to recommend",
            "i do not have a verified destination to recommend",
        },
        "inventory": {
            "i can t confirm any requested item is available to trade",
        },
        "identity": {
            "i m rowan a level 12 warrior",
            "my name is rowan and i am a level 12 warrior",
        },
        "quest": set(),
        "advice": {
            "practice carefully and learn which existing spells suit you",
        },
        "greeting": {
            "hello",
            "hello there",
            "hello traveler",
            "greetings",
            "greetings traveler",
            "hello arlen",
            "greetings arlen",
        },
        "general": {
            "i m not sure how to answer that",
        },
    }
    return value in alternatives[intent]


def delivery_shape(response: str) -> tuple[str, list[str]]:
    reasons: list[str] = []
    words = response.split()
    lower = response.lower()
    if not response:
        reasons.append("empty_reply")
    if len(words) > 15:
        reasons.append("over_15_words")
    if len(response.encode("utf-8")) > 200:
        reasons.append("over_200_bytes")
    if any(marker in lower for marker in ("language model", "system prompt", "as an ai", "assistant")):
        reasons.append("modern_or_meta_reference")
    if any(marker in response for marker in ("```", "**", "<think>", "</think>")):
        reasons.append("markup_or_thinking")
    return ("PASS" if not reasons else "FAIL", reasons)


def build_prompt(settings: dict[str, str], intent: str, message: str, contract: str) -> str:
    values = dict(gate9.SYNTHETIC_CONTEXT)
    values.update(
        {
            "player_message": message,
            "bounded_context": BOUNDED_CONTEXT,
            "intent": intent,
            "reply_goal": contract,
        }
    )
    values["extra_info"] = gate9.substitute(
        settings["OllamaChat.ChatExtraInfoTemplate"], values
    )
    return (
        gate9.substitute(settings["OllamaChat.ChatPromptTemplate"], values)
        + "\nAUTHORITATIVE CURRENT SPEAKER: Arlen. Reply to this speaker. "
        + "Address them only as Arlen or without a name. Never address them as a "
        + "different player. Names in older conversation context are not the current "
        + "speaker. Do not repeat or closely paraphrase an earlier reply.\n"
    )


def evaluate(world_container: str, timeout_seconds: int) -> dict[str, object]:
    config_text = gate9.docker("exec", world_container, "cat", gate9.CONFIG_PATH)
    settings = gate9.parse_settings(config_text)
    if settings["OllamaChat.Url"] != "http://ollama:11434/api/generate":
        raise RuntimeError("active Ollama URL is not the private candidate endpoint")
    if settings.get("OllamaChat.DeterministicIntentRouting.Enable") != "1":
        raise RuntimeError("deterministic intent routing is not enabled")
    if settings.get("OllamaChat.BoundedContext.Enable") != "1":
        raise RuntimeError("bounded context is not enabled")
    if settings["OllamaChat.Model"] != MODEL:
        raise RuntimeError("candidate model does not match the Gate 10 model")

    jobs = [
        {
            "case": intent,
            "model": MODEL,
            "prompt": build_prompt(settings, intent, message, contract),
        }
        for intent, message, contract in CASES
    ]
    request = {
        "url": settings["OllamaChat.Url"],
        "system": settings["OllamaChat.SystemPrompt"],
        "timeout_seconds": timeout_seconds,
        "options": {
            "num_predict": int(settings["OllamaChat.NumPredict"]),
            "num_ctx": int(settings["OllamaChat.NumCtx"]),
            "temperature": float(settings["OllamaChat.Temperature"]),
            "top_p": float(settings["OllamaChat.TopP"]),
            "repeat_penalty": float(settings["OllamaChat.RepeatPenalty"]),
        },
        "jobs": jobs,
    }
    raw = gate9.docker(
        "exec",
        "-i",
        world_container,
        "/opt/outlands-wow/.venv/bin/python3",
        "-c",
        gate9.REMOTE_RUNNER,
        input_text=json.dumps(request),
    )
    results = json.loads(raw)["results"]
    contracts = {intent: contract for intent, _message, contract in CASES}
    for item in results:
        intent = str(item["case"])
        model_reply = gate9.normalize_reply(str(item.pop("response")), max_words=15)
        contract = contracts[intent]
        grounded = is_grounded_reply(intent, model_reply, contract)
        delivered = model_reply if grounded else gate9.normalize_reply(contract, max_words=15)
        shape, reasons = delivery_shape(delivered)
        item.update(
            {
                "model_response": model_reply,
                "model_response_sha256": hashlib.sha256(model_reply.encode()).hexdigest(),
                "grounding_fallback": not grounded,
                "delivered_response": delivered,
                "delivered_response_sha256": hashlib.sha256(delivered.encode()).hexdigest(),
                "delivered_word_count": len(delivered.split()),
                "delivery_check": shape,
                "delivery_review_reasons": reasons,
            }
        )

    return {
        "schema_version": 1,
        "created_utc": dt.datetime.now(dt.UTC).isoformat(),
        "scope": "synthetic_backend_only",
        "network_path": f"{world_container}->ollama:11434",
        "model": MODEL,
        "settings_sha256": hashlib.sha256(config_text.encode()).hexdigest(),
        "player_data_recorded": False,
        "credentials_recorded": False,
        "summary": {
            "cases": len(results),
            "delivered_passes": sum(item["delivery_check"] == "PASS" for item in results),
            "grounding_fallbacks": sum(bool(item["grounding_fallback"]) for item in results),
            "max_delivered_words": max(
                (int(item["delivered_word_count"]) for item in results), default=0
            ),
            "max_wall_ms": max((int(item["wall_ms"]) for item in results), default=0),
        },
        "results": results,
    }


def write_evidence(document: dict[str, object]) -> Path:
    stamp = dt.datetime.now(dt.UTC).strftime("%Y%m%d-%H%M%SZ")
    directory = ROOT / "run" / "private" / "gate10" / f"{stamp}-ollama-backend-eval"
    directory.mkdir(parents=True, mode=0o700)
    path = directory / "result.json"
    path.write_text(json.dumps(document, indent=2) + "\n", encoding="utf-8")
    os.chmod(path, 0o600)
    return path


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--world-container", required=True)
    parser.add_argument("--timeout-seconds", type=int, default=15)
    args = parser.parse_args()
    if not 1 <= args.timeout_seconds <= 30:
        parser.error("--timeout-seconds must be between 1 and 30")

    try:
        document = evaluate(args.world_container, args.timeout_seconds)
        path = write_evidence(document)
    except (RuntimeError, OSError, ValueError, json.JSONDecodeError) as error:
        print(f"GATE 10 BACKEND EVAL ERROR: {error}", file=sys.stderr)
        return 2

    summary = document["summary"]
    print(
        f"Gate 10: {summary['delivered_passes']}/{summary['cases']} delivery passes, "
        f"fallbacks={summary['grounding_fallbacks']}, "
        f"max_words={summary['max_delivered_words']}, max={summary['max_wall_ms']}ms"
    )
    for item in document["results"]:
        print(
            f"  {item['case']}: {item['wall_ms']}ms "
            f"fallback={str(item['grounding_fallback']).lower()} "
            f"delivered={item['delivered_response']}"
        )
    print(f"Evidence: {path}")
    return 0 if summary["delivered_passes"] == summary["cases"] else 1


if __name__ == "__main__":
    raise SystemExit(main())
