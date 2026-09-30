# SPDX-License-Identifier: GPL-3.0-only
import importlib.util
from pathlib import Path
import unittest

spec = importlib.util.spec_from_file_location('uma_runner', Path(__file__).resolve().parents[1] / 'scripts/run-uma-memory.py')
runner = importlib.util.module_from_spec(spec)
spec.loader.exec_module(runner)


class UmaMeasurement(unittest.TestCase):
    def test_excludes_warmup_and_preserves_telemetry(self):
        result = runner.summarize('''[frame] cycle=0 texture=0 wall_ms=900.0
[frame] cycle=1 texture=0 wall_ms=1.0
[frame] cycle=1 texture=1 wall_ms=3.0
[frame] cycle=2 texture=0 wall_ms=2.0
d3d11on12 memory telemetry: profile=balanced
''')
        self.assertEqual(result['transfer_frames'], 3)
        self.assertEqual(result['median_ms'], 2)
        self.assertEqual(result['p95_ms'], 3)
        self.assertEqual(len(result['memory_telemetry']), 1)

    def test_missing_samples_are_not_zero_cost(self):
        result = runner.summarize('[fail] could not create device')
        self.assertIsNone(result['median_ms'])
        self.assertIsNone(result['p95_ms'])
