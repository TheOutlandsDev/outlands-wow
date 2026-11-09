import io
import hashlib
import json
import os
import tempfile
import unittest
from pathlib import Path
from unittest import mock

import gate3_server_controller as controller


class Gate5ControllerUnitTests(unittest.TestCase):
    def test_mysql_literal_escapes_route_text(self):
        self.assertEqual(controller.mysql_literal("a'b\\c"), "'a''b\\\\c'")

    def test_tailscale_preflight_requires_exact_identity_and_empty_configs(self):
        status = {
            "BackendState": "Running",
            "Self": {
                "HostName": controller.TAILSCALE_HOSTNAME,
                "TailscaleIPs": [controller.TAILSCALE_ADDRESS],
            },
        }
        with mock.patch.object(
            controller, "tailscale_json", side_effect=[status, {}, {}]
        ):
            result = controller.require_empty_tailscale_transport(io.StringIO())
        self.assertEqual(result["hostname"], controller.TAILSCALE_HOSTNAME)
        self.assertFalse(result["serve_preexisting"])
        self.assertFalse(result["funnel_preexisting"])

    def test_tailscale_preflight_refuses_existing_serve_config(self):
        status = {
            "BackendState": "Running",
            "Self": {
                "HostName": controller.TAILSCALE_HOSTNAME,
                "TailscaleIPs": [controller.TAILSCALE_ADDRESS],
            },
        }
        with mock.patch.object(
            controller,
            "tailscale_json",
            side_effect=[status, {"TCP": {"443": {}}}, {}],
        ), self.assertRaises(controller.GateError):
            controller.require_empty_tailscale_transport(io.StringIO())

    def test_funnel_status_shared_serve_config_is_not_funnel(self):
        serve_only = {"TCP": {"3724": {"TCPForward": "192.0.2.10:3724"}}}
        self.assertFalse(controller.tailscale_funnel_enabled(serve_only))
        serve_only["AllowFunnel"] = {"node:3724": True}
        self.assertTrue(controller.tailscale_funnel_enabled(serve_only))

    def test_gate5_container_address_comes_from_internal_network(self):
        details = [
            {
                "NetworkSettings": {
                    "Networks": {controller.NETWORK: {"IPAddress": "192.0.2.11"}}
                },
            }
        ]
        completed = mock.Mock(stdout=json.dumps(details))
        with mock.patch.object(controller, "run_capture", return_value=completed):
            result = controller.container_network_address("auth", io.StringIO())
        self.assertEqual(result, "192.0.2.11")


class Gate6ControllerUnitTests(unittest.TestCase):
    def test_offline_state_normalization_is_exact_and_transactional(self):
        with mock.patch.object(
            controller,
            "mysql_query",
            side_effect=["10", "0"],
        ) as query:
            result = controller.normalize_offline_character_state(
                10, io.StringIO()
            )
        self.assertEqual(
            result,
            {
                "column": "la_ahfinal11_characters.characters.online",
                "before": 10,
                "rows_changed": 10,
                "after": 0,
            },
        )
        self.assertIn("START TRANSACTION", query.call_args_list[0].args[0])
        self.assertIn("WHERE `online` <> 0", query.call_args_list[0].args[0])

    def test_offline_state_normalization_rejects_count_mismatch(self):
        with mock.patch.object(
            controller,
            "mysql_query",
            side_effect=["9", "0"],
        ), self.assertRaises(controller.GateError):
            controller.normalize_offline_character_state(10, io.StringIO())

    def test_backup_root_requires_operator_owned_mode_0700(self):
        with tempfile.TemporaryDirectory() as temporary:
            path = Path(temporary) / "living-azeroth"
            path.mkdir(mode=0o700)
            os.chmod(path, 0o700)
            self.assertEqual(controller.require_gate6_backup_root(path), path.resolve())
            os.chmod(path, 0o750)
            with self.assertRaises(controller.GateError):
                controller.require_gate6_backup_root(path)

    def test_gate6_server_command_uses_operational_image_and_label(self):
        completed = mock.Mock(returncode=0, stdout="container-id\n")
        with mock.patch.object(controller, "docker_object_exists", return_value=False), mock.patch.object(
            controller, "run_capture", return_value=completed
        ) as run:
            result = controller.start_server(
                "authserver",
                Path("/private/acore-password"),
                io.StringIO(),
                image=controller.OPERATIONAL_RUNTIME_IMAGE,
                change_id=controller.GATE6_CHANGE_ID,
            )
        self.assertEqual(result, controller.AUTH_CONTAINER)
        command = run.call_args.args[0]
        self.assertIn(controller.OPERATIONAL_RUNTIME_IMAGE, command)
        self.assertIn(f"io.outlandswow.gate={controller.GATE6_CHANGE_ID}", command)
        self.assertIn("com.server.managed=true", command)
        self.assertIn("com.server.service=living-azeroth", command)
        self.assertIn("com.server.role=authserver", command)
        self.assertIn("com.server.resource-policy=living-azeroth-v1", command)
        self.assertEqual(command[command.index("--cpus") + 1], "0.5")
        self.assertEqual(command[command.index("--memory") + 1], "512m")
        self.assertEqual(command[command.index("--memory-swap") + 1], "512m")
        self.assertEqual(command[command.index("--pids-limit") + 1], "1024")

    def test_server_only_world_command_uses_reduced_bounded_resources(self):
        completed = mock.Mock(returncode=0, stdout="container-id\n")
        with mock.patch.object(
            controller, "docker_object_exists", return_value=False
        ), mock.patch.object(controller, "run_capture", return_value=completed) as run:
            controller.start_server(
                "worldserver",
                Path("/private/acore-password"),
                io.StringIO(),
                image=controller.OPERATIONAL_RUNTIME_IMAGE,
                change_id=controller.GATE6_CHANGE_ID,
                ai_profile="disabled",
            )
        command = run.call_args.args[0]
        self.assertEqual(command[command.index("--cpus") + 1], "2")
        self.assertEqual(command[command.index("--memory") + 1], "6g")
        self.assertEqual(command[command.index("--memory-swap") + 1], "6g")
        self.assertEqual(command[command.index("--pids-limit") + 1], "1024")
        self.assertNotIn("LIVING_AZEROTH_AI_PROFILE=disabled", command)

    def test_tailscale_snapshot_reports_only_bounded_self_state(self):
        status = {
            "BackendState": "Running",
            "Self": {
                "HostName": controller.TAILSCALE_HOSTNAME,
                "TailscaleIPs": [controller.TAILSCALE_ADDRESS],
                "UserID": 123,
            },
            "Peer": {"secret-peer-data": {}},
        }
        serve = {"TCP": {"3724": {"TCPForward": "172.1.0.2:3724"}}}
        with mock.patch.object(
            controller, "tailscale_json", side_effect=[status, serve, {}]
        ):
            result = controller.tailscale_transport_snapshot(io.StringIO())
        self.assertEqual(result["serve_tcp_ports"], [3724])
        self.assertTrue(result["expected_address_present"])
        self.assertNotIn("Peer", result)
        self.assertNotIn("UserID", result)

    def test_operational_start_refuses_existing_runtime_state(self):
        with mock.patch.object(
            controller,
            "load_runtime_state",
            return_value={"schema_version": 1, "change_id": controller.GATE6_CHANGE_ID},
        ), self.assertRaises(controller.GateError):
            controller.operational_start(
                {}, Path("root"), Path("app"), 120, False, "disabled", io.StringIO()
            )

    def test_gate8_world_command_has_bounded_profile_resources(self):
        completed = mock.Mock(returncode=0, stdout="container-id\n")
        with mock.patch.object(controller, "docker_object_exists", return_value=False), mock.patch.object(
            controller, "run_capture", return_value=completed
        ) as run:
            controller.start_server(
                "worldserver",
                Path("/private/acore-password"),
                io.StringIO(),
                image=controller.GATE8_RUNTIME_IMAGE,
                change_id=controller.GATE8_CHANGE_ID,
                ai_profile="bots-dialogue-small",
            )
        command = run.call_args.args[0]
        self.assertIn("LIVING_AZEROTH_AI_PROFILE=bots-dialogue-small", command)
        self.assertIn("--cpus", command)
        self.assertIn("--memory", command)
        self.assertIn("--pids-limit", command)
        self.assertEqual(command[command.index("--cpus") + 1], "4")
        self.assertEqual(command[command.index("--memory") + 1], "8g")
        self.assertEqual(command[command.index("--memory-swap") + 1], "8g")
        self.assertNotIn("--publish", command)

    def test_resource_limit_validation_rejects_unbounded_container(self):
        details = [
            {
                "HostConfig": {
                    "PortBindings": {},
                    "ReadonlyRootfs": False,
                    "NanoCpus": 0,
                    "Memory": 0,
                    "MemorySwap": 0,
                    "PidsLimit": 0,
                },
                "NetworkSettings": {"Networks": {controller.NETWORK: {}}},
                "Config": {"User": "", "Labels": {}},
            }
        ]
        completed = mock.Mock(returncode=0, stdout=json.dumps(details))
        with mock.patch.object(
            controller, "run_capture", return_value=completed
        ), self.assertRaisesRegex(controller.GateError, "resource limits differ"):
            controller.assert_no_host_bindings(
                controller.DATABASE_CONTAINER,
                io.StringIO(),
                expected_resource_budget=controller.DATABASE_RESOURCE_BUDGET,
            )

    def test_gate9_world_command_joins_only_game_and_private_ai_networks(self):
        completed = mock.Mock(returncode=0, stdout="container-id\n")
        with mock.patch.object(controller, "docker_object_exists", return_value=False), mock.patch.object(
            controller, "run_capture", return_value=completed
        ) as run:
            controller.start_server(
                "worldserver",
                Path("/private/acore-password"),
                io.StringIO(),
                image=controller.GATE9_RUNTIME_IMAGE,
                change_id=controller.GATE9_CHANGE_ID,
                ai_profile="bots-ollama-dialogue-small",
            )
        command = run.call_args.args[0]
        self.assertEqual(command.count("--network"), 2)
        self.assertIn(controller.NETWORK, command)
        self.assertIn(controller.AI_NETWORK, command)
        self.assertIn("LIVING_AZEROTH_AI_PROFILE=bots-ollama-dialogue-small", command)
        self.assertNotIn("LIVING_AZEROTH_AI_BACKEND_PROBE=1", command)
        self.assertNotIn("--publish", command)

    def test_gate8_dialogue_requires_local_policy_marker(self):
        log = "\n".join(
            (
                "AI_PROFILE: name=bots-dialogue-small playerbot_limit=10 autonomous_world_systems=off",
                "mod-playerbots initialized",
                "AHBot: disabled by configuration",
                "[Ollama Chat] Config loaded: Enabled = false",
                "[Ollama Chat Social] configured enabled=false",
                "[Ollama Chat Telemetry] disabled_by_config",
                "Living Azeroth Chat v2 config: enabled=true world=false addressed=100 unaddressed=0 bot=0",
            )
        )
        controller.assert_operational_ai_controls(log, "bots-dialogue-small")

    def test_gate8_local_profile_selects_accepted_local_image(self):
        baseline = {
            "runtime_image": {
                "authserver_sha256": "auth",
                "worldserver_sha256": "world",
            }
        }
        actual = {
            "identity": f"{controller.GATE8_LOCAL_RUNTIME_IMAGE_ID}|amd64/linux",
            "source_commit": controller.GATE8_LOCAL_RUNTIME_SOURCE_REVISION,
            "authserver_sha256": controller.GATE8_LOCAL_RUNTIME_AUTHSERVER_SHA256,
            "worldserver_sha256": controller.GATE8_LOCAL_RUNTIME_WORLDSERVER_SHA256,
        }
        with mock.patch.object(
            controller, "inspect_runtime_image", return_value=actual
        ) as inspect:
            result = controller.verify_gate8_runtime_image(
                baseline, "bots-local-dialogue-small", io.StringIO()
            )
        inspect.assert_called_once_with(controller.GATE8_LOCAL_RUNTIME_IMAGE, mock.ANY)
        self.assertTrue(result["candidate_identity_verified"])
        self.assertTrue(result["binary_hashes_match_accepted_candidate"])
        self.assertFalse(result["binary_hashes_match_gate6"])
        self.assertEqual(result["accepted_run"], controller.GATE8_LOCAL_ACCEPTED_RUN)

    def test_gate9_profile_selects_exact_accepted_ollama_image(self):
        baseline = {
            "runtime_image": {
                "authserver_sha256": "old-auth",
                "worldserver_sha256": "old-world",
            }
        }
        actual = {
            "identity": f"{controller.GATE9_RUNTIME_IMAGE_ID}|amd64/linux",
            "source_commit": controller.GATE9_RUNTIME_SOURCE_REVISION,
            "authserver_sha256": controller.GATE9_RUNTIME_AUTHSERVER_SHA256,
            "worldserver_sha256": controller.GATE9_RUNTIME_WORLDSERVER_SHA256,
        }
        with mock.patch.object(
            controller, "inspect_runtime_image", return_value=actual
        ) as inspect:
            result = controller.verify_gate8_runtime_image(
                baseline, "bots-ollama-dialogue-small", io.StringIO()
            )
        inspect.assert_called_once_with(controller.GATE9_RUNTIME_IMAGE, mock.ANY)
        self.assertEqual(result["accepted_run"], controller.GATE9_ACCEPTED_RUN)
        self.assertFalse(result["binary_hashes_match_gate6"])

    def test_gate10_profile_selects_exact_accepted_routed_image(self):
        baseline = {
            "runtime_image": {
                "authserver_sha256": "old-auth",
                "worldserver_sha256": "old-world",
            }
        }
        actual = {
            "identity": f"{controller.GATE10_RUNTIME_IMAGE_ID}|amd64/linux",
            "source_commit": controller.GATE10_RUNTIME_SOURCE_REVISION,
            "authserver_sha256": controller.GATE10_RUNTIME_AUTHSERVER_SHA256,
            "worldserver_sha256": controller.GATE10_RUNTIME_WORLDSERVER_SHA256,
        }
        with mock.patch.object(
            controller, "inspect_runtime_image", return_value=actual
        ) as inspect:
            result = controller.verify_gate8_runtime_image(
                baseline, "bots-ollama-routed-small", io.StringIO()
            )
        inspect.assert_called_once_with(controller.GATE10_RUNTIME_IMAGE, mock.ANY)
        self.assertEqual(result["accepted_run"], controller.GATE10_ACCEPTED_RUN)
        self.assertFalse(result["binary_hashes_match_gate6"])

    def test_gate10_requires_routing_marker(self):
        log = "\n".join(
            (
                "AI_PROFILE: name=bots-ollama-routed-small playerbot_limit=10 autonomous_world_systems=off",
                "mod-playerbots initialized",
                "AHBot: disabled by configuration",
                "Living Azeroth Chat v2 config: enabled=false",
                "[Ollama Chat] Config loaded: Enabled = true, Model = qwen2.5:7b-instruct",
                "[Ollama Chat Social] configured enabled=false",
                "[Ollama Chat Telemetry] disabled_by_config",
                "[Ollama Chat] Scheduler started: workers=1, max_queue=8, per_player=1, per_bot=1",
                "[Ollama Chat] Gate10 routing: deterministic=true bounded_context=true reply_word_cap=15",
            )
        )
        controller.assert_operational_ai_controls(log, "bots-ollama-routed-small")

    def test_gate9_requires_private_model_and_scheduler_markers(self):
        log = "\n".join(
            (
                "AI_PROFILE: name=bots-ollama-dialogue-small playerbot_limit=10 autonomous_world_systems=off",
                "mod-playerbots initialized",
                "AHBot: disabled by configuration",
                "Living Azeroth Chat v2 config: enabled=false",
                "[Ollama Chat] Config loaded: Enabled = true, Model = qwen2.5:7b-instruct",
                "[Ollama Chat Social] configured enabled=false",
                "[Ollama Chat Telemetry] disabled_by_config",
                "[Ollama Chat] Scheduler started: workers=1, max_queue=8, per_player=1, per_bot=1",
            )
        )
        controller.assert_operational_ai_controls(log, "bots-ollama-dialogue-small")

    def test_private_ollama_rejects_non_loopback_binding(self):
        details = {
            "State": {"Running": True},
            "HostConfig": {
                "PortBindings": {"11434/tcp": [{"HostIp": "0.0.0.0", "HostPort": "11434"}]}
            },
            "NetworkSettings": {"Networks": {"ollama_default": {}}},
        }
        completed = mock.Mock(returncode=0, stdout=json.dumps([details]))
        with mock.patch.object(controller, "docker_object_exists", return_value=True), mock.patch.object(
            controller, "run_capture", return_value=completed
        ), self.assertRaisesRegex(controller.GateError, "host loopback"):
            controller.verify_private_ollama(io.StringIO())

    def test_gate8_local_profile_rejects_unaccepted_world_binary(self):
        baseline = {
            "runtime_image": {
                "authserver_sha256": "old-auth",
                "worldserver_sha256": "old-world",
            }
        }
        actual = {
            "identity": f"{controller.GATE8_LOCAL_RUNTIME_IMAGE_ID}|amd64/linux",
            "source_commit": controller.GATE8_LOCAL_RUNTIME_SOURCE_REVISION,
            "authserver_sha256": controller.GATE8_LOCAL_RUNTIME_AUTHSERVER_SHA256,
            "worldserver_sha256": "not-the-accepted-worldserver",
        }
        with mock.patch.object(
            controller, "inspect_runtime_image", return_value=actual
        ), self.assertRaisesRegex(controller.GateError, "accepted candidate"):
            controller.verify_gate8_runtime_image(
                baseline, "bots-local-dialogue-small", io.StringIO()
            )

    def test_promoted_runtime_requires_exact_gate6_image_identity(self):
        expected = {
            "identity": "sha256:expected|amd64/linux",
            "source_commit": "abc123",
            "authserver_sha256": "auth-hash",
            "worldserver_sha256": "world-hash",
        }
        actual = {**expected, "identity": "sha256:different|amd64/linux"}
        with mock.patch.object(
            controller, "inspect_runtime_image", return_value=actual
        ), self.assertRaises(controller.GateError):
            controller.verify_promoted_runtime_image(
                {"runtime_image": expected}, io.StringIO()
            )

    def test_restart_access_defaults_can_preserve_false_state(self):
        state = {
            "schema_version": 1,
            "change_id": controller.GATE6_CHANGE_ID,
            "player_access_enabled": False,
        }
        self.assertFalse(bool(state.get("player_access_enabled")))

    def test_operational_stop_recovers_owned_power_loss_containers(self):
        state = {
            "schema_version": 1,
            "change_id": controller.GATE8_CHANGE_ID,
            "player_access_enabled": True,
            "ai_profile": "bots-local-dialogue-small",
            "expected_metrics": {},
        }
        stopped = {
            "result": "PASS",
            "unclean_shutdown_recovered": True,
            "unclean_containers": list(controller.CONTAINERS),
        }
        with mock.patch.object(controller, "load_runtime_state", return_value=state), mock.patch.object(
            controller, "docker_object_exists", return_value=True
        ), mock.patch.object(
            controller, "reset_gate5_tailscale", return_value={"serve_empty": True}
        ), mock.patch.object(
            controller, "stop_gate6_stack", return_value=stopped
        ) as stop, mock.patch.object(
            controller, "GATE6_RUNTIME_STATE", mock.Mock()
        ) as runtime_state:
            result = controller.operational_stop(Path("evidence"), io.StringIO())
        self.assertEqual(result["stack"], stopped)
        stop.assert_called_once_with(
            {},
            Path("evidence"),
            "operational",
            mock.ANY,
            mock.ANY,
            ai_profile="bots-local-dialogue-small",
            allow_unclean_shutdown=True,
            expected_change_id=controller.GATE8_CHANGE_ID,
        )
        runtime_state.unlink.assert_called_once_with(missing_ok=True)


class Gate7ControllerUnitTests(unittest.TestCase):
    def make_backup(self, root: Path, name: str, contents: bytes) -> str:
        directory = root / name
        directory.mkdir(mode=0o700)
        os.chmod(directory, 0o700)
        dump = directory / "living-azeroth.sql"
        dump.write_bytes(contents)
        os.chmod(dump, 0o600)
        digest = hashlib.sha256(contents).hexdigest()
        manifest = {
            "schema_version": 1,
            "result": "PASS",
            "change_id": controller.GATE6_CHANGE_ID,
            "dump_file": dump.name,
            "bytes": len(contents),
            "sha256": digest,
            "credentials_recorded": False,
            "player_identity_recorded": False,
        }
        manifest_path = directory / "manifest.json"
        manifest_path.write_text(json.dumps(manifest), encoding="utf-8")
        os.chmod(manifest_path, 0o600)
        return digest

    def test_retention_pins_gate6_and_keeps_newest_ordinary_backups(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary) / "living-azeroth"
            root.mkdir(mode=0o700)
            os.chmod(root, 0o700)
            pinned = self.make_backup(
                root, "20260101-000000Z-logical", b"accepted-gate6"
            )
            self.make_backup(root, "20260102-000000Z-logical", b"old")
            self.make_backup(root, "20260103-000000Z-logical", b"newer")
            self.make_backup(root, "20260104-000000Z-logical", b"newest")
            with mock.patch.object(
                controller,
                "require_gate6_baseline",
                return_value={"backup": {"sha256": pinned}},
            ), mock.patch.object(
                controller,
                "GATE6_ACCEPTED_BACKUP_DIRECTORY",
                "20260101-000000Z-logical",
            ):
                result = controller.apply_operational_backup_retention(root, 2)
            self.assertEqual(
                result["removed_directory_names"], ["20260102-000000Z-logical"]
            )
            self.assertTrue((root / "20260101-000000Z-logical").is_dir())
            self.assertTrue((root / "20260103-000000Z-logical").is_dir())
            self.assertTrue((root / "20260104-000000Z-logical").is_dir())

    def test_retention_refuses_symlinked_entry_before_removal(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary) / "living-azeroth"
            root.mkdir(mode=0o700)
            os.chmod(root, 0o700)
            pinned = self.make_backup(
                root, "20260101-000000Z-logical", b"accepted-gate6"
            )
            (root / "20260102-000000Z-logical").symlink_to(
                root / "20260101-000000Z-logical", target_is_directory=True
            )
            with mock.patch.object(
                controller,
                "require_gate6_baseline",
                return_value={"backup": {"sha256": pinned}},
            ), mock.patch.object(
                controller,
                "GATE6_ACCEPTED_BACKUP_DIRECTORY",
                "20260101-000000Z-logical",
            ), self.assertRaises(controller.GateError):
                controller.apply_operational_backup_retention(root, 2)


if __name__ == "__main__":
    unittest.main()
