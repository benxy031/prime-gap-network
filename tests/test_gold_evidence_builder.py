"""The Gold evidence builder must be found in every shipped runtime layout.

A wrong guess here is not a cosmetic bug: with the helper only at the runtime
root, every Gold publication failed inside a maintenance job, so 373 ticket
folders held an empty gold-publications/ folder and zero discoveries were ever
published. These cases pin the layouts that actually exist.
"""
from pathlib import Path
import os
import sys
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]
sys.path[:0] = [str(ROOT / 'src/engine/runtime_host')]
from horizon_gold_builder import GoldBuilderMissing, candidates, resolve, usable


def executable(path):
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_bytes(b'#!/bin/sh\nexit 0\n')
    path.chmod(0o755)
    return path


class GoldBuilderResolutionTests(unittest.TestCase):
    def setUp(self):
        self.folder = tempfile.TemporaryDirectory()
        self.runtime = Path(self.folder.name) / 'runtime'
        self.bundle = self.runtime / 'EngineRuntime'
        self.bundle.mkdir(parents=True)

    def tearDown(self):
        self.folder.cleanup()

    def test_linux_layout_found_at_runtime_root(self):
        # tools/build_linux_runtime.py: <runtime>/pgn-native-gap-evidence
        expected = executable(self.runtime / 'pgn-native-gap-evidence')
        self.assertEqual(resolve(self.bundle), expected)

    def test_component_layout_found_under_bin(self):
        # The component manifest and the Windows bundle: EngineRuntime/bin/...
        expected = executable(self.bundle / 'bin' / 'pgn-gap-evidence')
        self.assertEqual(resolve(self.bundle), expected)

    def test_native_name_also_found_under_bin(self):
        expected = executable(self.bundle / 'bin' / 'pgn-native-gap-evidence')
        self.assertEqual(resolve(self.bundle), expected)

    def test_windows_names(self):
        expected = executable(self.bundle / 'bin' / 'pgn-gap-evidence.exe')
        self.assertEqual(resolve(self.bundle, nt=True), expected)
        # A Linux binary name must not be returned when Windows names are asked for.
        expected.unlink()
        executable(self.runtime / 'pgn-native-gap-evidence')
        with self.assertRaises(GoldBuilderMissing):
            resolve(self.bundle, nt=True)

    def test_explicit_builder_wins(self):
        executable(self.bundle / 'bin' / 'pgn-gap-evidence')
        chosen = executable(Path(self.folder.name) / 'custom' / 'helper')
        self.assertEqual(resolve(self.bundle, chosen), chosen)

    def test_explicit_missing_builder_is_refused(self):
        with self.assertRaises(GoldBuilderMissing) as caught:
            resolve(self.bundle, Path(self.folder.name) / 'absent')
        self.assertIn('absent', str(caught.exception))

    def test_missing_builder_names_every_path_tried(self):
        with self.assertRaises(GoldBuilderMissing) as caught:
            resolve(self.bundle)
        message = str(caught.exception)
        for path in candidates(self.bundle):
            self.assertIn(str(path), message)

    def test_non_executable_file_is_not_usable(self):
        idle = self.bundle / 'bin' / 'pgn-gap-evidence'
        idle.parent.mkdir(parents=True, exist_ok=True)
        idle.write_bytes(b'not executable\n')
        idle.chmod(0o644)
        self.assertFalse(usable(idle))
        with self.assertRaises(GoldBuilderMissing):
            resolve(self.bundle)

    def test_directory_is_not_a_builder(self):
        (self.bundle / 'bin' / 'pgn-gap-evidence').mkdir(parents=True)
        with self.assertRaises(GoldBuilderMissing):
            resolve(self.bundle)


if __name__ == '__main__':
    unittest.main()
