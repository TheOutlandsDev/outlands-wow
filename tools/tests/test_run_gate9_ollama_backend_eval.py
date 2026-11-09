import unittest

import run_gate9_ollama_backend_eval as backend_eval


class Gate9OllamaBackendEvalTests(unittest.TestCase):
    def test_parse_settings_rejects_incomplete_config(self):
        with self.assertRaisesRegex(RuntimeError, "active Ollama config is missing"):
            backend_eval.parse_settings("OllamaChat.Model = qwen2.5:7b-instruct\n")

    def test_build_prompt_uses_synthetic_identity_and_authoritative_speaker(self):
        settings = {
            "OllamaChat.ChatExtraInfoTemplate": "Location: {bot_area}.",
            "OllamaChat.ChatPromptTemplate": (
                "You are {bot_name}. {player_name} said: {player_message}. {extra_info}"
            ),
        }
        prompt = backend_eval.build_prompt(settings, "Hello there")
        self.assertIn("You are Rowan.", prompt)
        self.assertIn("Arlen said: Hello there.", prompt)
        self.assertIn("Location: Goldshire.", prompt)
        self.assertIn("AUTHORITATIVE CURRENT SPEAKER: Arlen", prompt)

    def test_grounded_profile_marks_unknown_information(self):
        settings = {
            "OllamaChat.ChatExtraInfoTemplate": "Location: {bot_area}.",
            "OllamaChat.ChatPromptTemplate": "unused",
        }
        prompt = backend_eval.build_prompt(settings, "Do you have linen?", "grounded-v2")
        self.assertIn("BOT FACTS:", prompt)
        self.assertIn("NEW MESSAGE FROM Arlen: Do you have linen?", prompt)
        self.assertIn("Unknown information remains unknown.", prompt)

    def test_routed_profile_includes_bounded_reply_goal(self):
        settings = {
            "OllamaChat.ChatExtraInfoTemplate": "Location: {bot_area}.",
            "OllamaChat.ChatPromptTemplate": "unused",
        }
        prompt = backend_eval.build_prompt(
            settings,
            "Do you have linen?",
            "routed-v3",
            "inventory_boundary",
        )
        self.assertIn("REPLY GOAL:", prompt)
        self.assertIn("nothing available to offer", prompt)
        self.assertIn("Reply as Rowan in first person", prompt)

    def test_normalize_matches_bounded_runtime_policy(self):
        reply = 'Assistant:  "A short\nanswer with ordinary words for this test only"'
        normalized = backend_eval.normalize_reply(reply, max_words=5)
        self.assertEqual(normalized, '"A short answer with ordinary')
        self.assertLessEqual(len(normalized.split()), 5)

    def test_assess_flags_meta_and_latency(self):
        result, failures, normalized = backend_eval.assess(
            "As an AI language model, I cannot answer that request.", 10_001, ""
        )
        self.assertEqual(result, "REVIEW")
        self.assertIn("modern_or_meta_reference", failures)
        self.assertIn("over_10_second_budget", failures)
        self.assertEqual(normalized, "As an AI language model, I cannot answer that request.")

    def test_assess_accepts_short_in_character_reply(self):
        result, failures, _ = backend_eval.assess(
            "I am checking supplies before heading beyond the inn.", 900, ""
        )
        self.assertEqual(result, "PASS")
        self.assertEqual(failures, [])

    def test_assess_allows_bot_to_identify_itself_when_asked(self):
        result, failures, _ = backend_eval.assess(
            "I'm Rowan, an ordinary warrior here.",
            900,
            "",
            "in_character",
        )
        self.assertEqual(result, "PASS")
        self.assertEqual(failures, [])

    def test_assess_flags_unverified_relationship_origin_and_activity(self):
        result, failures, _ = backend_eval.assess(
            "I'm passing through from Goldshire, friend Rowan.",
            900,
            "",
            "ordinary_intent",
        )
        self.assertEqual(result, "REVIEW")
        self.assertIn("bot_name_self_reference", failures)
        self.assertIn("invented_relationship", failures)
        self.assertIn("invented_origin", failures)
        self.assertIn("invented_activity", failures)


if __name__ == "__main__":
    unittest.main()
