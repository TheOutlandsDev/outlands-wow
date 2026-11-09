import unittest

import run_gate10_ollama_backend_eval as backend_eval


class Gate10OllamaBackendEvalTests(unittest.TestCase):
    def test_canonical_matches_cpp_word_normalization(self):
        self.assertEqual(backend_eval.canonical("I'm Rowan."), "i m rowan")

    def test_grounding_guard_accepts_bounded_paraphrase(self):
        self.assertTrue(
            backend_eval.is_grounded_reply(
                "activity",
                "I am currently stationary.",
                "I am stationary here right now.",
            )
        )

    def test_grounding_guard_rejects_player_noun_and_unverified_claim(self):
        self.assertFalse(
            backend_eval.is_grounded_reply(
                "inventory",
                "I cannot confirm linen is available to trade.",
                "I cannot confirm any requested item is available to trade.",
            )
        )
        self.assertFalse(
            backend_eval.is_grounded_reply(
                "general",
                "The road seems quiet today.",
                "I am not sure how to answer that.",
            )
        )

    def test_delivery_shape_enforces_fifteen_word_limit(self):
        result, reasons = backend_eval.delivery_shape(" ".join(["word"] * 15))
        self.assertEqual(result, "PASS")
        self.assertEqual(reasons, [])
        result, reasons = backend_eval.delivery_shape(" ".join(["word"] * 16))
        self.assertEqual(result, "FAIL")
        self.assertIn("over_15_words", reasons)

    def test_prompt_contains_bounded_contract_and_untrusted_message(self):
        settings = {
            "OllamaChat.ChatExtraInfoTemplate": "unused {bot_area}",
            "OllamaChat.ChatPromptTemplate": (
                "CONTEXT {bounded_context} INTENT {intent} MESSAGE {player_message} "
                "CONTRACT {reply_goal}"
            ),
        }
        prompt = backend_eval.build_prompt(
            settings,
            "inventory",
            "Do you have linen to sell?",
            "I cannot confirm any requested item is available to trade.",
        )
        self.assertIn("activity=stationary", prompt)
        self.assertIn("INTENT inventory", prompt)
        self.assertIn("MESSAGE Do you have linen to sell?", prompt)
        self.assertIn("CONTRACT I cannot confirm", prompt)


if __name__ == "__main__":
    unittest.main()
