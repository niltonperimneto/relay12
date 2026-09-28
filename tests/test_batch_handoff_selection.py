# SPDX-License-Identifier: GPL-3.0-only
"""Exercise source preparation, including the conditional patch dependency."""
import os
from pathlib import Path
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]
UPSTREAM = ROOT / 'third_party/D3D12TranslationLayer'


@unittest.skipUnless((UPSTREAM / 'src/BatchedContext.cpp').exists(),
                     'requires the pinned DTL submodule')
class BatchHandoffSelection(unittest.TestCase):
    def prepare(self, mode):
        temporary = tempfile.TemporaryDirectory(prefix='relay-handoff-')
        self.addCleanup(temporary.cleanup)
        tree = Path(temporary.name) / 'dtl'
        subprocess.run(['git', 'clone', '--quiet', '--no-hardlinks', str(UPSTREAM), str(tree)],
                       check=True, capture_output=True)
        env = os.environ.copy()
        env.pop('RELAY12_BATCH_HANDOFF', None)
        if mode is not None:
            env['RELAY12_BATCH_HANDOFF'] = mode
        result = subprocess.run([str(ROOT / 'scripts/prepare-dtl-source.sh'), str(tree)],
                                env=env, capture_output=True, text=True)
        return tree, result

    def test_default_and_explicit_semaphore(self):
        for mode in (None, 'semaphore'):
            with self.subTest(mode=mode):
                tree, result = self.prepare(mode)
                self.assertEqual(result.returncode, 0, result.stderr)
                source = (tree / 'src/BatchedContext.cpp').read_text()
                self.assertIn('m_BatchSubmittedSemaphore', source)
                self.assertNotIn('DrainCompletions', source)
                self.assertTrue('UseNonBlockingPSOs' in (tree / 'include/ImmediateContext.hpp').read_text())

    def test_ring_includes_completion_fix(self):
        tree, result = self.prepare('ring')
        self.assertEqual(result.returncode, 0, result.stderr)
        source = (tree / 'src/BatchedContext.cpp').read_text()
        self.assertNotIn('m_BatchSubmittedSemaphore', source)
        self.assertIn('return WaitForBatchThreadIdle() || submitted;', source)
        self.assertIn('m_QueuedBatches.consumed() == m_ObservedConsumedBatches', source)

    def test_invalid_mode_leaves_tree_untouched(self):
        tree, result = self.prepare('typo')
        self.assertNotEqual(result.returncode, 0)
        self.assertIn('invalid RELAY12_BATCH_HANDOFF', result.stderr)
        status = subprocess.check_output(['git', '-C', str(tree), 'status', '--porcelain'])
        self.assertEqual(status, b'')
