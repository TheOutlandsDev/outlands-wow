import json
import tempfile
import unittest
from pathlib import Path

import living_azeroth_observability as observability


class LivingAzerothObservabilityTests(unittest.TestCase):
    def healthy_status(self):
        return {
            "managed_runtime": True,
            "player_access_enabled": True,
            "ai_profile": "bots-local-dialogue-small",
            "ai_network": False,
            "ai_network_internal": False,
            "ollama": {"required": False, "healthy": False},
            "host_protected_listeners": [3724, 8085],
            "tailscale": {
                "serve_tcp_ports": [3724, 8085],
                "funnel_enabled": False,
            },
            "containers": {
                name: {
                    "exists": True,
                    "running": True,
                    "restart_count": 0,
                    "host_port_bindings": {},
                }
                for name in observability.EXPECTED_CONTAINERS
            },
            "database_metrics": {"online_characters": 10, "accounts": 123},
        }

    def test_render_contains_only_bounded_aggregate_state(self):
        output = observability.render_metrics(
            self.healthy_status(),
            {
                "successful_count": 4,
                "latest_created_timestamp_seconds": 1234,
                "latest_bytes": 400_000_000,
            },
            2000,
            False,
        )
        self.assertIn("living_azeroth_runtime_healthy 1", output)
        self.assertIn("living_azeroth_security_boundary_ok 1", output)
        self.assertIn('profile="bots-local-dialogue-small"', output)
        self.assertIn('metric="online_characters"} 10', output)
        self.assertIn("living_azeroth_ollama_required 0", output)
        self.assertIn("living_azeroth_ollama_backend_healthy 1", output)
        self.assertIn("living_azeroth_development_offline 0", output)
        self.assertIn("living_azeroth_desired_online 1", output)
        self.assertIn("living_azeroth_backup_required 1", output)
        self.assertNotIn("100.64.0.10", output)

    def test_ollama_profile_requires_private_healthy_internal_boundary(self):
        status = self.healthy_status()
        status["ai_profile"] = "bots-ollama-dialogue-small"
        status["ai_network"] = True
        status["ai_network_internal"] = True
        status["ollama"] = {
            "required": True,
            "healthy": True,
            "host_binding": "127.0.0.1:11434",
            "tailscale_published": False,
        }
        self.assertTrue(observability.security_boundary_ok(status))
        output = observability.render_metrics(
            status,
            {
                "successful_count": 4,
                "latest_created_timestamp_seconds": 1234,
                "latest_bytes": 400_000_000,
            },
            2000,
            False,
        )
        self.assertIn("living_azeroth_ollama_required 1", output)
        self.assertIn("living_azeroth_ollama_backend_healthy 1", output)
        status["ollama"]["host_binding"] = "0.0.0.0:11434"
        self.assertFalse(observability.security_boundary_ok(status))

    def test_security_boundary_rejects_funnel(self):
        status = self.healthy_status()
        status["tailscale"]["funnel_enabled"] = True
        self.assertFalse(observability.security_boundary_ok(status))
        self.assertFalse(observability.runtime_healthy(status))

    def test_development_offline_metrics_are_explicit_and_non_paging(self):
        status = self.healthy_status()
        status["managed_runtime"] = False
        status["player_access_enabled"] = False
        status["host_protected_listeners"] = []
        status["tailscale"]["serve_tcp_ports"] = []
        for value in status["containers"].values():
            value["exists"] = False
            value["running"] = False
        output = observability.render_metrics(
            status,
            {
                "successful_count": 9,
                "latest_created_timestamp_seconds": 1234,
                "latest_bytes": 400_000_000,
            },
            2000,
            True,
        )
        self.assertIn("living_azeroth_development_offline 1", output)
        self.assertIn("living_azeroth_desired_online 0", output)
        self.assertIn("living_azeroth_backup_required 0", output)
        self.assertIn("living_azeroth_runtime_healthy 0", output)
        summary = observability.sanitized_summary(
            status,
            {
                "successful_count": 9,
                "latest_created_timestamp_seconds": 1234,
                "latest_bytes": 400_000_000,
            },
            True,
        )
        self.assertTrue(summary["development_offline"])

    def test_backup_snapshot_ignores_failed_or_invalid_manifests(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            good = root / "20260911-120000Z-logical"
            bad = root / "20260911-130000Z-logical"
            good.mkdir()
            bad.mkdir()
            (good / "manifest.json").write_text(
                '{"result":"PASS","created_utc":"2026-09-11T12:00:00+00:00","bytes":42}',
                encoding="utf-8",
            )
            (bad / "manifest.json").write_text("not-json", encoding="utf-8")
            snapshot = observability.backup_snapshot(root)
        self.assertEqual(snapshot["successful_count"], 1)
        self.assertEqual(snapshot["latest_bytes"], 42)

    def test_alerts_are_guarded_by_explicit_development_state(self):
        root = Path(__file__).resolve().parents[2]
        rules = (
            root / "operations" / "observability" / "living-azeroth-alerts.yml"
        ).read_text(encoding="utf-8")
        self.assertIn("LivingAzerothDesiredStateMissing", rules)
        self.assertIn("LivingAzerothDevelopmentOfflineBoundaryFailed", rules)
        self.assertIn("living_azeroth_desired_online == 1", rules)
        self.assertIn("living_azeroth_backup_required == 1", rules)
        self.assertIn("LivingAzerothBackupBaselineMissing", rules)

    def test_dashboard_shows_planned_offline_without_panel_overlap(self):
        root = Path(__file__).resolve().parents[2]
        dashboard = json.loads(
            (
                root
                / "operations"
                / "observability"
                / "living-azeroth-operations.json"
            ).read_text(encoding="utf-8")
        )
        panels = dashboard["panels"]
        self.assertEqual(len({panel["id"] for panel in panels}), len(panels))
        desired = next(panel for panel in panels if panel["id"] == 17)
        runtime = next(panel for panel in panels if panel["id"] == 2)
        backup = next(panel for panel in panels if panel["id"] == 7)
        self.assertEqual(
            desired["targets"][0]["expr"], "living_azeroth_development_offline"
        )
        self.assertIn("living_azeroth_desired_online", runtime["targets"][0]["expr"])
        self.assertIn("living_azeroth_backup_required", backup["targets"][0]["expr"])
        for index, left in enumerate(panels):
            a = left["gridPos"]
            for right in panels[index + 1 :]:
                b = right["gridPos"]
                overlaps = not (
                    a["x"] + a["w"] <= b["x"]
                    or b["x"] + b["w"] <= a["x"]
                    or a["y"] + a["h"] <= b["y"]
                    or b["y"] + b["h"] <= a["y"]
                )
                self.assertFalse(
                    overlaps,
                    f"dashboard panels overlap: {left['id']} and {right['id']}",
                )


if __name__ == "__main__":
    unittest.main()
