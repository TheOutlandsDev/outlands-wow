import json
import tempfile
import unittest
from pathlib import Path
from unittest import mock

import wow_launcher as launcher


class LauncherTests(unittest.TestCase):
    def test_manifest_is_locked_to_gate5_tailnet_endpoint(self):
        server = launcher.load_server()
        self.assertEqual(server.address, "100.64.0.10")
        self.assertEqual((server.auth_port, server.world_port), (3724, 8085))
        self.assertEqual(server.build, 12340)

    def test_realmlist_is_replaced_and_restored_byte_exactly(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            realm = root / "realmlist.wtf"
            original = b"set realmlist old.example\r\nset patchlist old.example\r\n"
            realm.write_bytes(original)
            backup = launcher.install_temporary_realmlist(realm, "100.64.0.10")
            self.assertTrue(backup.is_file())
            self.assertEqual(realm.read_bytes(), b"set realmlist 100.64.0.10\r\n")
            self.assertTrue(launcher.restore_realmlist(realm))
            self.assertEqual(realm.read_bytes(), original)
            self.assertFalse(backup.exists())

    def test_existing_backup_is_never_overwritten(self):
        with tempfile.TemporaryDirectory() as temporary:
            realm = Path(temporary) / "realmlist.wtf"
            realm.write_text("original", encoding="ascii")
            launcher.backup_path(realm).write_text("recovery", encoding="ascii")
            with self.assertRaises(launcher.LauncherError):
                launcher.install_temporary_realmlist(realm, "100.64.0.10")
            self.assertEqual(realm.read_text(encoding="ascii"), "original")

    def test_client_discovery_and_single_locale(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            (root / "Wow.exe").touch()
            locale = root / "Data" / "enUS"
            locale.mkdir(parents=True)
            realm = locale / "realmlist.wtf"
            realm.touch()
            with mock.patch.object(launcher, "load_saved_wow_directory", return_value=None):
                self.assertEqual(launcher.find_wow_directory(root), root.resolve())
            self.assertEqual(launcher.find_realmlist(root), realm.resolve())

    def test_network_check_requires_peer_and_both_ports(self):
        payload = {
            "BackendState": "Running",
            "Self": {"TailscaleIPs": ["100.64.0.11"]},
            "Peer": {"node": {"TailscaleIPs": ["100.64.0.10"], "Online": True}},
        }
        completed = mock.Mock(returncode=0, stdout=json.dumps(payload))
        ping = mock.Mock(returncode=0, stdout="pong")
        with mock.patch.object(launcher, "find_tailscale", return_value=Path("tailscale.exe")), mock.patch.object(
            launcher, "run_command", side_effect=[completed, ping]
        ), mock.patch.object(launcher, "tcp_probe", side_effect=[True, True]):
            result = launcher.check_network(launcher.load_server())
        self.assertTrue(result.ready)
        self.assertEqual(result.local_tailscale_ips, ("100.64.0.11",))


if __name__ == "__main__":
    unittest.main()
