import unittest

import verify_gate7_runtime_status as verifier


def running_status(player_access: bool) -> dict[str, object]:
    ports = [3724, 8085] if player_access else []
    return {
        "result": "PASS",
        "managed_runtime": True,
        "player_access_enabled": player_access,
        "published_ports": [],
        "containers": {
            name: {
                "exists": True,
                "running": True,
                "restart_count": 0,
                "host_port_bindings": {},
            }
            for name in verifier.CONTAINER_NAMES
        },
        "database_metrics": {"online_characters": 0},
        "host_protected_listeners": ports,
        "tailscale": {
            "serve_empty": not player_access,
            "serve_tcp_ports": ports,
            "funnel_enabled": False,
        },
    }


class Gate7RuntimeVerifierTests(unittest.TestCase):
    def test_verifies_server_only(self):
        result = verifier.verify_running(running_status(False), False)
        self.assertEqual(result["mode"], "server-only")

    def test_verifies_tailscale_player_access(self):
        result = verifier.verify_running(running_status(True), True)
        self.assertEqual(result["tailscale_tcp_ports"], [3724, 8085])

    def test_rejects_docker_host_binding(self):
        status = running_status(True)
        first = next(iter(status["containers"].values()))
        first["host_port_bindings"] = {"8085/tcp": [{"HostIp": "0.0.0.0"}]}
        with self.assertRaises(RuntimeError):
            verifier.verify_running(status, True)


if __name__ == "__main__":
    unittest.main()
