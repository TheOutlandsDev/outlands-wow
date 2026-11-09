import subprocess
import tempfile
import unittest
from pathlib import Path
from unittest import mock

import reconstruct_pinned_source as rebuild


def git(directory: Path, *arguments: str) -> str:
    return subprocess.check_output(["git", "-C", str(directory), *arguments], text=True).strip()


class ReconstructionSafetyTests(unittest.TestCase):
    def test_wrong_checkout_pin_is_rejected(self):
        with tempfile.TemporaryDirectory() as temporary:
            source = Path(temporary) / "upstream"
            source.mkdir()
            git(source, "init", "-q")
            (source / "file.txt").write_text("fixture\n")
            git(source, "add", "file.txt")
            subprocess.run(
                ["git", "-C", str(source), "-c", "user.name=Fixture", "-c", "user.email=fixture@example.invalid", "commit", "-qm", "fixture"],
                check=True,
            )
            with self.assertRaisesRegex(ValueError, "expected"):
                rebuild.check_checkout(source, rebuild.COMPONENTS[0])

    def test_existing_output_is_never_overwritten(self):
        with tempfile.TemporaryDirectory() as temporary:
            output = Path(temporary) / "existing"
            output.mkdir()
            sentinel = output / "keep.txt"
            sentinel.write_text("keep\n")
            with mock.patch.object(rebuild, "check_kit"), mock.patch.object(
                rebuild, "check_checkout", return_value=Path(temporary)
            ), self.assertRaises(FileExistsError):
                rebuild.reconstruct({component.option: Path(temporary) for component in rebuild.COMPONENTS}, output)
            self.assertEqual(sentinel.read_text(), "keep\n")

    def test_unexpected_archive_symlink_is_rejected(self):
        with tempfile.TemporaryDirectory() as temporary:
            source = Path(temporary) / "upstream"
            source.mkdir()
            git(source, "init", "-q")
            (source / "outside").symlink_to("../outside")
            git(source, "add", "outside")
            subprocess.run(
                ["git", "-C", str(source), "-c", "user.name=Fixture", "-c", "user.email=fixture@example.invalid", "commit", "-qm", "fixture"],
                check=True,
            )
            with self.assertRaisesRegex(ValueError, "Unexpected archive symlink"):
                rebuild.export_commit(source, git(source, "rev-parse", "HEAD"), Path(temporary) / "output")


if __name__ == "__main__":
    unittest.main()
