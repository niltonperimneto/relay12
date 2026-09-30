#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-only
#
# Tests for scripts/run-peak-smoke.py that need no Wine, Steam or PEAK.
#
# The runner's one irreversible hazard is starting a newer Wine in a player's
# bottle, which upgrades it. So the refusal is tested first: a prefix without
# the marker never reaches Wine. The rest pins the Z: path handed to
# -logFile and the environment the verdict depends on.
#
# Run: python3 -m unittest discover -s tests -p 'test_*.py'

import importlib.util
import pathlib
import tempfile
import unittest
from unittest import mock

REPOSITORY = pathlib.Path(__file__).resolve().parent.parent
SPEC = importlib.util.spec_from_file_location(
    "run_peak_smoke", REPOSITORY / "scripts" / "run-peak-smoke.py")
runner = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(runner)


class Refusals(unittest.TestCase):
    def run_main(self, prefix):
        with mock.patch.object(runner.subprocess, "Popen") as popen, \
                mock.patch.object(runner.subprocess, "run") as run, \
                self.assertRaises(SystemExit) as raised, \
                mock.patch("sys.stderr"):
            runner.main(["--wine", "/nonexistent/bin/wine64", "--prefix", str(prefix),
                         "--out", str(prefix / "out")])
        return raised.exception.code, popen, run

    def test_prefix_without_marker_never_reaches_wine(self):
        with tempfile.TemporaryDirectory() as directory:
            code, popen, run = self.run_main(pathlib.Path(directory))
        self.assertEqual(code, 2)
        popen.assert_not_called()
        run.assert_not_called()

    def test_marked_prefix_without_steam_is_refused(self):
        with tempfile.TemporaryDirectory() as directory:
            prefix = pathlib.Path(directory)
            (prefix / runner.MARKER).touch()
            code, popen, run = self.run_main(prefix)
        self.assertEqual(code, 2)
        popen.assert_not_called()
        run.assert_not_called()


class Arguments(unittest.TestCase):
    def test_log_path_is_the_z_drive_view_of_the_host_path(self):
        self.assertEqual(runner.windows_path("/private/tmp/run/Player.log"),
                         "Z:\\private\\tmp\\run\\Player.log")

    def environment(self, **inherited):
        with mock.patch.dict(runner.os.environ, inherited, clear=True):
            return runner.run_environment(pathlib.Path("/runtime"), pathlib.Path("/prefix"))

    def test_environment_carries_the_evidence_the_verdict_reads(self):
        env = self.environment()
        self.assertEqual(env["RELAY12_TRACE_CREATION"], "1")
        self.assertEqual(env["RELAY12_EXPERIMENTAL_FRAME"], "1")
        self.assertEqual(env["MTL_HUD_LOG_ENABLED"], "1")
        self.assertIn("d3d11on12core", env["WINEDLLOVERRIDES"])
        self.assertEqual(env["WINEDEBUG"], "-all,warn+d3d11")

    def test_inherited_winedebug_cannot_silence_the_inventory(self):
        # CI exports WINEDEBUG=-all; the run must still carry warn+d3d11 last.
        for inherited, expected in (("-all", "-all,warn+d3d11"),
                                    ("+seh,-d3d11", "+seh,-d3d11,warn+d3d11"),
                                    ("", "warn+d3d11")):
            with self.subTest(inherited=inherited):
                self.assertEqual(self.environment(WINEDEBUG=inherited)["WINEDEBUG"], expected)


class Synchronization(unittest.TestCase):
    def test_selected_mode_overrides_conflicting_parent_and_runtime(self):
        for mode in ("none", "msync", "esync"):
            with self.subTest(mode=mode), mock.patch.dict(runner.os.environ, {
                    "WINEMSYNC": "1", "WINEESYNC": "1", "WINEFSYNC": "1",
                    "WINESERVER": "/wrong/server", "WINELOADER": "/wrong/loader"}):
                env = runner.run_environment(pathlib.Path("/runtime"), pathlib.Path("/prefix"), mode)
                self.assertEqual(env["WINEMSYNC"], str(int(mode == "msync")))
                self.assertEqual(env["WINEESYNC"], str(int(mode == "esync")))
                self.assertEqual(env["WINEFSYNC"], "0")
                self.assertEqual(env["WINESERVER"], "/runtime/bin/wineserver")
                self.assertEqual(env["WINELOADER"], "/runtime/bin/wine64")

    def test_no_active_server_still_requires_successful_wait(self):
        absent = runner.subprocess.CalledProcessError(1, "wineserver -k")
        with mock.patch.object(runner.subprocess, "run", side_effect=[absent, None]) as run:
            runner.stop_server(pathlib.Path("/runtime/bin/wine64"), {}, None)
        self.assertEqual(run.call_count, 2)
        failed = runner.subprocess.CalledProcessError(1, "wineserver -w")
        with mock.patch.object(runner.subprocess, "run", side_effect=[absent, failed]):
            with self.assertRaises(runner.subprocess.CalledProcessError):
                runner.stop_server(pathlib.Path("/runtime/bin/wine64"), {}, None)

    def test_server_shutdown_waits_and_does_not_ignore_failure(self):
        env = {"WINEPREFIX": "/test-prefix"}
        with mock.patch.object(runner.subprocess, "run") as run:
            runner.stop_server(pathlib.Path("/runtime/bin/wine64"), env, None)
        self.assertEqual([call.args[0] for call in run.call_args_list],
                         [["/runtime/bin/wineserver", "-k"], ["/runtime/bin/wineserver", "-w"]])
        for call in run.call_args_list:
            self.assertEqual(call.kwargs["env"], env)
            self.assertTrue(call.kwargs["check"])
        with mock.patch.object(runner.subprocess, "run", side_effect=runner.subprocess.TimeoutExpired("server", 30)) as run:
            with self.assertRaises(runner.subprocess.TimeoutExpired):
                runner.stop_server(pathlib.Path("/runtime/bin/wine64"), env, None)
            self.assertEqual(run.call_count, 1)


if __name__ == "__main__":
    unittest.main()
