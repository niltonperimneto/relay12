# SPDX-License-Identifier: GPL-3.0-only
"""Hardware tests must not silently use a runtime's stale builtin host."""
import importlib.util
from pathlib import Path
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]
RUNNERS = []
for filename in ('run-peak-smoke.py', 'run-uma-memory.py'):
    spec = importlib.util.spec_from_file_location(filename, ROOT / 'scripts' / filename)
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    RUNNERS.append(module)


class HostSelection(unittest.TestCase):
    def check(self, installed_contents, expected):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            staged = root / 'staged.dll'
            staged.write_bytes(b'new host')
            if installed_contents is not None:
                installed = root / 'lib/wine/x86_64-windows/d3d11on12host.dll'
                installed.parent.mkdir(parents=True)
                installed.write_bytes(installed_contents)
            for runner in RUNNERS:
                with self.subTest(runner=runner.__name__):
                    self.assertEqual(runner.runtime_host_matches(root, staged), expected)

    def test_stale_installed_builtin_host_is_rejected(self):
        self.check(b'old host', False)

    def test_matching_installed_builtin_host_is_accepted(self):
        self.check(b'new host', True)

    def test_staged_builtin_fallback_when_runtime_has_no_host(self):
        self.check(None, True)
