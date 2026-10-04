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
[upload] cycle=0 wall_ms=100.0
[upload] cycle=1 wall_ms=10.0
[upload] cycle=2 wall_ms=20.0
d3d11on12 memory telemetry: profile=balanced
d3d11on12 pool telemetry: pool=upload retained_bytes=32440320 peak_retained_bytes=34603008 pending_bytes=0 completed_bytes=32440320 allocations=180 reuses=60 trims=165
''')
        self.assertEqual(result['transfer_frames'], 3)
        self.assertEqual(result['median_ms'], 2)
        self.assertEqual(result['p95_ms'], 3)
        self.assertEqual(len(result['memory_telemetry']), 2)
        self.assertEqual(result['upload_median_ms'], 15.0)
        self.assertIn('pool_stats', result)
        self.assertEqual(result['pool_stats']['upload']['allocations'], 180)
        self.assertEqual(result['pool_stats']['upload']['reuses'], 60)
        self.assertEqual(result['pool_stats']['upload']['trims'], 165)

    def test_missing_samples_are_not_zero_cost(self):
        result = runner.summarize('[fail] could not create device')
        self.assertIsNone(result['median_ms'])
        self.assertIsNone(result['p95_ms'])
