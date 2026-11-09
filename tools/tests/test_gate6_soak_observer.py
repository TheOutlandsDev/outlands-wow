import unittest

import observe_gate6_soak as observer


class Gate6SoakObserverTests(unittest.TestCase):
    def passing_status(self):
        return {
            "managed_runtime": True,
            "resource_limits_ok": True,
            "ai_profile": "disabled",
            "player_access_enabled": True,
            "database_volume": True,
            "data_volume": True,
            "host_protected_listeners": [3724, 8085],
            "containers": {
                name: {
                    "running": True,
                    "host_port_bindings": {},
                    "restart_count": 0,
                    "resource_limits_ok": True,
                }
                for name in observer.CONTAINERS
            },
            "tailscale": {
                "backend_state": "Running",
                "expected_address_present": True,
                "serve_tcp_ports": [3724, 8085],
                "funnel_enabled": False,
                "serve_empty": False,
            },
            "ollama": {"required": False, "healthy": False},
            "database_metrics": {"online_characters": 0},
        }

    def test_accepts_exact_private_runtime_state(self):
        result = observer.validate_status(self.passing_status(), 30)
        self.assertEqual(result["running_containers"], 3)
        self.assertEqual(result["tailscale_tcp_ports"], [3724, 8085])

    def test_rejects_any_docker_host_binding(self):
        status = self.passing_status()
        status["containers"][observer.CONTAINERS[0]]["host_port_bindings"] = {
            "8085/tcp": [{"HostIp": "0.0.0.0", "HostPort": "8085"}]
        }
        with self.assertRaises(observer.ObservationError):
            observer.validate_status(status, 30)

    def test_rejects_funnel(self):
        status = self.passing_status()
        status["tailscale"]["funnel_enabled"] = True
        with self.assertRaises(observer.ObservationError):
            observer.validate_status(status, 30)

    def test_accepts_server_only_ollama_profile_with_exact_limits(self):
        status = self.passing_status()
        status["player_access_enabled"] = False
        status["ai_profile"] = "bots-ollama-routed-small"
        status["host_protected_listeners"] = []
        status["tailscale"]["serve_tcp_ports"] = []
        status["tailscale"]["serve_empty"] = True
        status["ollama"] = {"required": True, "healthy": True}
        result = observer.validate_status(
            status,
            30,
            player_access=False,
            expected_ai_profile="bots-ollama-routed-small",
        )
        self.assertEqual(result["tailscale_tcp_ports"], [])
        self.assertTrue(result["resource_limits_ok"])

    def test_rejects_server_only_resource_limit_drift(self):
        status = self.passing_status()
        status["player_access_enabled"] = False
        status["host_protected_listeners"] = []
        status["tailscale"]["serve_tcp_ports"] = []
        status["tailscale"]["serve_empty"] = True
        status["resource_limits_ok"] = False
        with self.assertRaises(observer.ObservationError):
            observer.validate_status(status, 30, player_access=False)


if __name__ == "__main__":
    unittest.main()
