import json
import subprocess
import unittest
from unittest import mock

import living_azeroth_service_supervisor as supervisor


def completed(status: dict[str, object]) -> subprocess.CompletedProcess[str]:
    return subprocess.CompletedProcess([], 0, stdout=json.dumps(status), stderr="")


def base_status(player_access: bool) -> dict[str, object]:
    ports = [3724, 8085] if player_access else []
    return {
        "result": "PASS",
        "resource_limits_ok": True,
        "managed_runtime": True,
        "player_access_enabled": player_access,
        "published_ports": [],
        "containers": {
            name: {
                "exists": True,
                "running": True,
                "host_port_bindings": {},
            }
            for name in ("world", "auth", "database")
        },
        "host_protected_listeners": ports,
        "tailscale": {
            "serve_empty": not player_access,
            "serve_tcp_ports": ports,
            "funnel_enabled": False,
        },
    }


class Gate7SupervisorTests(unittest.TestCase):
    def test_development_offline_marker_is_explicit(self):
        with mock.patch.object(
            supervisor,
            "DEVELOPMENT_OFFLINE_MARKER",
            mock.Mock(is_file=mock.Mock(return_value=True)),
        ):
            self.assertTrue(supervisor.development_offline_requested())

    def test_accepts_server_only_runtime(self):
        self.assertEqual(supervisor.classify_status(completed(base_status(False))), "healthy")

    def test_accepts_explicit_tailscale_player_runtime(self):
        self.assertEqual(supervisor.classify_status(completed(base_status(True))), "healthy")

    def test_rejects_funnel(self):
        status = base_status(True)
        status["tailscale"]["funnel_enabled"] = True
        self.assertEqual(supervisor.classify_status(completed(status)), "unhealthy")

    def test_rejects_resource_limit_drift(self):
        status = base_status(False)
        status["resource_limits_ok"] = False
        self.assertEqual(supervisor.classify_status(completed(status)), "unhealthy")

    def test_accepts_clean_operator_stop(self):
        status = base_status(False)
        status["managed_runtime"] = False
        status["containers"] = {
            name: {"exists": False, "running": False}
            for name in ("world", "auth", "database")
        }
        self.assertEqual(supervisor.classify_status(completed(status)), "stopped")

    def test_defers_status_timeout_behind_controller_operation(self):
        timeout = subprocess.TimeoutExpired(cmd=["controller", "status"], timeout=30)
        with mock.patch.object(supervisor, "run_controller", side_effect=timeout):
            self.assertEqual(supervisor.poll_controller_status(), ("busy", None))

    def test_detects_managed_runtime_for_power_loss_recovery(self):
        status = base_status(False)
        for value in status["containers"].values():
            value["running"] = False
        process = completed(status)
        self.assertEqual(supervisor.classify_status(process), "unhealthy")
        self.assertTrue(supervisor.managed_runtime_present(process))

    def test_does_not_adopt_unmanaged_objects(self):
        status = base_status(False)
        status["managed_runtime"] = False
        self.assertFalse(supervisor.managed_runtime_present(completed(status)))


if __name__ == "__main__":
    unittest.main()
