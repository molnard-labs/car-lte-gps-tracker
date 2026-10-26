"""Offline tests for provisioning; fake generator, no device or network access."""

import contextlib
import csv
import io
from pathlib import Path
import shutil
import sys
import unittest
from unittest import mock
import uuid

import provision_tracker


class ProvisioningTests(unittest.TestCase):
    def setUp(self):
        self.private_root = Path(__file__).resolve().parent / "provisioning-private"
        self.root = self.private_root / ("test-" + uuid.uuid4().hex)
        self.root.mkdir(parents=True)
        self.idf = self.root / "idf"
        self.output = self.root / "output"
        self.generator = self.idf / "components/nvs_flash/nvs_partition_generator/nvs_partition_gen.py"
        self.generator.parent.mkdir(parents=True)
        self.generator.write_text(
            "import pathlib,sys\npathlib.Path(sys.argv[3]).write_bytes(bytes(int(sys.argv[4],0)))\n",
            encoding="utf-8",
        )

    def tearDown(self):
        # Delete only the UUID fixture this test created inside the ignored area.
        self.assertEqual(self.root.resolve().parent, self.private_root.resolve())
        self.assertTrue(self.root.name.startswith("test-"))
        shutil.rmtree(self.root)

    def generate(self, name):
        return provision_tracker.provision(name, self.idf, sys.executable, self.output)

    def test_unique_passwords_and_exact_factory_namespace(self):
        first, second = self.generate("car-one"), self.generate("car-two")
        password = (first / "setup-password.txt").read_text().strip()
        self.assertEqual(len(password), 32)
        self.assertNotEqual(password, (second / "setup-password.txt").read_text().strip())
        self.assertEqual((first / "factory.bin").stat().st_size, 65536)
        with (first / "factory.csv").open(newline="") as stream:
            rows = list(csv.reader(stream))
        self.assertEqual(rows, [
            ["key", "type", "encoding", "value"],
            ["provision", "namespace", "", ""],
            ["setup_password", "data", "string", password],
        ])

    def test_existing_bundle_is_never_replaced(self):
        bundle = self.generate("car")
        before = {path.name: path.read_bytes() for path in bundle.iterdir()}
        with self.assertRaises(FileExistsError):
            self.generate("car")
        self.assertEqual(before, {path.name: path.read_bytes() for path in bundle.iterdir()})

    def test_path_labels_cannot_escape_private_directory(self):
        for name in ("", "../car", "a/b", "a\\b", "/tmp/car", "-car", "CON", "nul", "LPT1", "a" * 65):
            with self.subTest(name=name), self.assertRaises(ValueError):
                self.generate(name)
        self.assertFalse(self.output.exists())

    def test_missing_generator_creates_no_secret_files(self):
        self.generator.unlink()
        with self.assertRaises(ValueError):
            self.generate("car")
        self.assertFalse(self.output.exists())

    def test_generator_diagnostics_cannot_disclose_input(self):
        self.generator.write_text(
            "import pathlib,sys\nprint(pathlib.Path(sys.argv[2]).read_text())\nsys.exit(2)\n",
            encoding="utf-8",
        )
        stdout, stderr = io.StringIO(), io.StringIO()
        with contextlib.redirect_stdout(stdout), contextlib.redirect_stderr(stderr), self.assertRaises(RuntimeError) as caught:
            self.generate("car")
        password = (self.output / "car/setup-password.txt").read_text().strip()
        self.assertNotIn(password, stdout.getvalue() + stderr.getvalue() + str(caught.exception))
        self.assertFalse((self.output / "car/factory.bin").exists())

    def test_cli_reports_paths_without_passphrase(self):
        original = provision_tracker.provision
        def generate_private(device_id, idf_path, idf_python):
            return original(device_id, idf_path, idf_python, self.output)
        output = io.StringIO()
        with mock.patch.object(sys, "argv", ["provision_tracker.py", "--device-id", "car", "--idf-path", str(self.idf)]), \
             mock.patch.object(provision_tracker, "provision", generate_private), contextlib.redirect_stdout(output):
            self.assertEqual(provision_tracker.main(), 0)
        password = (self.output / "car/setup-password.txt").read_text().strip()
        self.assertIn("factory.bin", output.getvalue())
        self.assertNotIn(password, output.getvalue())


if __name__ == "__main__":
    unittest.main()
