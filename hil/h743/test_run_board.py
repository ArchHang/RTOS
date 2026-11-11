"""Host-only tests for H743 target identity checks before a flash write."""

import json
from pathlib import Path
import struct
import tempfile
from types import SimpleNamespace
import unittest
from unittest.mock import Mock, patch

import run_board as module


revision_v = "Device ID   : 0x450\nRevision ID : Rev V\n"
revision_x = revision_v.replace("Rev V", "Rev X")
revision_y = revision_v.replace("Rev V", "Rev Y")
uid_a = "0123456789abcdeffedcba98"
uid_b = "1123456789abcdeffedcba98"
response_a = revision_v + "0x1FF1E800 : 01234567 89ABCDEF FEDCBA98\n"
response_b = response_a.replace("01234567", "11234567")
identity_a = {"revision": "V", "uid": uid_a}
identity_b = {"revision": "V", "uid": uid_b}


class FlashPreflight(unittest.TestCase):
    def setUp(self):
        self.directory = tempfile.TemporaryDirectory()
        self.addCleanup(self.directory.cleanup)
        self.args = SimpleNamespace(batch=Path(self.directory.name), probe="probe")

    def test_cubeprogrammer_log_shape(self):
        response = "Reading 32-bit memory content\n" + response_a
        with patch.object(module, "programmer", return_value=response) as cli:
            identity = module.read_target_identity(self.args, "ignore")
            self.assertEqual(identity["revision"], "V")
            self.assertRegex(identity["uid"], "^[0-9a-f]{24}$")
            self.assertEqual(cli.call_args.args[1], ["-r32", "0x1ff1e800", "12"])

    def test_unsupported_revision_and_incomplete_uid_fail(self):
        with patch.object(module, "programmer", side_effect=(
            revision_y + response_a.splitlines()[-1],
            revision_x + "0x1FF1E800 : 01234567 89ABCDEF\n",
        )):
            for _ in range(2):
                with self.assertRaisesRegex(RuntimeError, "complete UID"):
                    module.read_target_identity(self.args, "ignore")

    def test_backup_target_swap_cannot_create_manifest(self):
        opts = revision_v + "SWAP_BANK : 0x0\nnWRP0 : 0x1\n"
        with patch.object(module, "programmer", side_effect=(opts, response_a, response_b)):
            with patch.object(module, "read_flash", side_effect=(b"same", b"same")):
                with self.assertRaisesRegex(RuntimeError, "identity reads disagree"):
                    module.backup(self.args)
        self.assertFalse((self.args.batch / "backup_verified.json").exists())

    def test_successful_backup_binds_uid_revision_and_probe(self):
        opts = revision_v + "SWAP_BANK : 0x0\nnWRP0 : 0x1\n"
        with patch.object(module, "programmer", side_effect=(opts, response_a, response_a)):
            with patch.object(module, "read_flash", return_value=b"same"):
                module.backup(self.args)
        manifest = json.loads((self.args.batch / "backup_verified.json").read_text())
        self.assertEqual({key: manifest[key] for key in identity_a}, identity_a)
        self.assertEqual(manifest["probe_serial"], "probe")

    def test_legacy_manifest_cannot_authorize_write(self):
        data = b"firmware"
        for name in ("original_flash.bin", "original_flash_second_read.bin"):
            (self.args.batch / name).write_bytes(data)
        (self.args.batch / "backup_verified.json").write_text(json.dumps(
            {"hash": module.digest(data), "probe_serial": "probe", "revision": "V"}))
        with patch.object(module, "FLASH_SIZE", len(data)), patch.object(module, "programmer") as cli:
            with self.assertRaisesRegex(RuntimeError, "Legacy captures"):
                module.restore(self.args)
            cli.assert_not_called()

    def test_restore_wrong_uid_stops_before_flash_write(self):
        with patch.object(module, "verified_backup", return_value=(b"firmware", identity_a)):
            with patch.object(module, "programmer", return_value=response_b) as cli:
                with self.assertRaisesRegex(RuntimeError, "does not match"):
                    module.restore(self.args)
                self.assertEqual(len(cli.call_args_list), 1)
                self.assertEqual(cli.call_args.args[1][0], "-r32")

    def test_restore_fallback_checks_identity_again(self):
        with patch.object(module, "verified_backup", return_value=(b"original", identity_a)):
            with patch.object(module, "read_flash", return_value=b"changed"):
                with patch.object(module, "programmer", side_effect=(response_a, "", response_b)) as cli:
                    with self.assertRaisesRegex(RuntimeError, "does not match"):
                        module.restore(self.args)
                    self.assertEqual([call.args[1][0] for call in cli.call_args_list], ["-r32", "-d", "-r32"])

    def test_capture_wrong_uid_stops_before_uart_and_flash(self):
        self.args.tag = "capture"
        self.args.image = self.args.batch / "firmware.bin"
        self.args.image.write_bytes(struct.pack("<II", 0x20020000, 0x08000009) + bytes(32))
        with patch.object(module, "verified_backup", return_value=(b"firmware", identity_a)):
            with patch.object(module, "serial", Mock()) as serial, patch.object(
                    module, "programmer", return_value=response_b) as cli:
                with self.assertRaisesRegex(RuntimeError, "does not match"):
                    module.capture(self.args)
                serial.Serial.assert_not_called()
                self.assertEqual([call.args[1][0] for call in cli.call_args_list], ["-r32"])


if __name__ == "__main__":
    unittest.main(verbosity=2)
