"""Run: python -m unittest discover -s tools -p test_version_bump.py -v."""

from contextlib import redirect_stderr, redirect_stdout
import importlib.util
import io
from pathlib import Path
import subprocess
import tempfile
import unittest
from unittest.mock import patch


SPEC = importlib.util.spec_from_file_location(
    "version_bump", Path(__file__).with_name("version-bump.py")
)
bump = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(bump)


class VersionBumpTests(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.addCleanup(self.temp.cleanup)
        self.root = Path(self.temp.name)
        self.firmware = self.root / "firmware/version.txt"
        self.server = self.root / "server/yuanbao/__init__.py"
        self.firmware.parent.mkdir(parents=True)
        self.server.parent.mkdir(parents=True)
        self.firmware.write_bytes(b"1.2.3\r\n")
        self.server.write_bytes(b'# Keep this comment\r\n__version__ = "1.2.3"\r\n')
        self.root_patch = patch.object(bump, "ROOT", self.root)
        self.root_patch.start()
        self.addCleanup(self.root_patch.stop)

    def run_bump(self, *args):
        with redirect_stdout(io.StringIO()), redirect_stderr(io.StringIO()):
            return bump.main(list(args))

    def test_levels_and_exact_version(self):
        for args, expected in [
            ((), "1.2.4"),
            (("minor",), "1.3.0"),
            (("major", "--no-build"), "2.0.0"),
            (("1.0.0",), "1.0.0"),
        ]:
            with self.subTest(args=args):
                self.assertEqual(self.run_bump(*args), 0)
                self.assertEqual(self.firmware.read_bytes(), expected.encode() + b"\r\n")
                self.assertEqual(
                    self.server.read_bytes(),
                    f'# Keep this comment\r\n__version__ = "{expected}"\r\n'.encode(),
                )

    def test_read_only_and_invalid_arguments(self):
        original = self.firmware.read_bytes(), self.server.read_bytes()
        self.assertEqual(self.run_bump("current"), 0)
        for value in ("01.2.3", "1.2", "v1.2.3", "1.2.3-rc.1", "1.2.3+meta", "-1.2.3"):
            self.assertEqual(self.run_bump("--", value), 1)
        for args in (("current", "--build"), ("--build", "--no-build")):
            with self.assertRaises(SystemExit) as error:
                self.run_bump(*args)
            self.assertEqual(error.exception.code, 2)
        self.assertEqual(original, (self.firmware.read_bytes(), self.server.read_bytes()))

    def test_mismatch_refuses_write(self):
        self.server.write_bytes(b'__version__ = "9.0.0"\n')
        self.assertEqual(self.run_bump(), 1)
        self.assertEqual(self.run_bump("current"), 1)
        self.assertEqual(self.firmware.read_bytes(), b"1.2.3\r\n")

    def test_failed_write_restores_both_files(self):
        write = Path.write_bytes
        failed = False

        def fail_once(path, content):
            nonlocal failed
            if path == self.server and not failed:
                failed = True
                raise OSError("Simulated write failure")
            return write(path, content)

        with patch.object(Path, "write_bytes", fail_once):
            self.assertEqual(self.run_bump(), 1)
        self.assertEqual(bump.read_versions(self.root)[0], "1.2.3")

    def test_build_failure_preserves_version_and_propagates_error(self):
        with (
            patch.object(bump, "build_command", return_value=["fake-build"]),
            patch.object(
                bump.subprocess, "run", return_value=subprocess.CompletedProcess([], 7)
            ) as run,
        ):
            self.assertEqual(self.run_bump("patch", "--build"), 1)
            run.assert_called_once_with(["fake-build"], cwd=self.root / "firmware", check=False)
        self.assertEqual(bump.read_versions(self.root)[0], "1.2.4")

    def test_cli_from_another_directory(self):
        result = subprocess.run(
            [__import__("sys").executable, str(Path(bump.__file__)), "current"],
            cwd=self.root,
            capture_output=True,
            text=True,
            check=False,
        )
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertRegex(result.stdout.strip(), r"^\d+\.\d+\.\d+$")
