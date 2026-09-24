#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-only
#
# Tests for scripts/check_peak_smoke_log.py.
#
# A qualifying pair of logs passes; then each criterion is broken on its own,
# because a gate only ever shown passing input is indistinguishable from one
# that checks nothing.  Absent evidence is broken as deliberately as bad
# evidence: an empty log must not qualify.
#
# Run: python3 -m unittest discover -s tests -p 'test_*.py'

import json
import pathlib
import subprocess
import sys
import tempfile
import unittest

REPOSITORY = pathlib.Path(__file__).resolve().parent.parent
SCRIPT = REPOSITORY / "scripts" / "check_peak_smoke_log.py"
sys.path.insert(0, str(REPOSITORY / "scripts"))

import check_peak_smoke_log as smoke  # noqa: E402

# Trimmed from a real PEAK Player.log (Unity 6000.3.15f1, Wine/D3DMetal,
# Apple A18 Pro, 2026-09-24): the D3D11 path, then Unity's crash handler.
REAL_D3D11_CRASH = """\
Initialize engine version: 6000.3.15f1 (c1aa84e375f6)
Forcing GfxDevice: Direct3D 11
GfxDevice: creating device client; kGfxThreadingModeClientWorkerJobs
Direct3D:
    Version:  Direct3D 11.0 [level 11.1]
    Renderer: Apple A18 Pro (ID=0x0)
    Vendor:   Unknown (ID=106b)
    VRAM:     2730 MB
    Driver:   65535.65535.65535.65535
Crash!!!
A crash has been intercepted by the crash handler. For call stack and other details, see the latest crash report generated in:
"""

# Trimmed from the same install launched with -force-d3d12 on stock D3DMetal
# (2026-09-24): D3D11On12CreateDevice is unsupported, and Unity silently
# retries and settles on Direct3D 11.  Note the missing newline Unity leaves
# after the failure line.
REAL_ON12_FALLBACK = """\
Forcing GfxDevice: Direct3D 12
GfxDevice: creating device client; kGfxThreadingModeDirect
d3d12: could not load DirectML.dll.
d3d12: failed to create D3D11On12 device (0x887a0004).[D3D12 Device Filter] Vendor Name: NVIDIA
GfxDevice: creating device client; kGfxThreadingModeDirect
d3d12: could not load DirectML.dll.
d3d12: failed to create D3D11On12 device (0x887a0004).GfxDevice: creating device client; kGfxThreadingModeDirect
Direct3D:
    Version:  Direct3D 11.0 [level 11.1]
    Renderer: AMD Compatibility Mode (ID=0x66af)
"""

PLAYER_D3D12 = """\
Initialize engine version: 6000.3.15f1 (c1aa84e375f6)
Forcing GfxDevice: Direct3D 12
Direct3D:
    Version:  Direct3D 12.0 [level 12.1]
    Renderer: Apple A18 Pro (ID=0x0)
"""

WINE_QUALIFYING = """\
d3d11on12core: validate caller device and queue (hr=0x00000000)
d3d11on12core: enter driver CreateDevice (hr=0x00000000)
d3d11on12core: leave driver CreateDevice (hr=0x00000000)
d3d11on12core: leave driver wrapped resource creation (hr=0x00000000)
d3d11on12core: leave driver present (hr=0x00000000)
"""


# Metal's performance HUD, from a real PEAK run on D3D12 through D3DMetal
# (2026-09-24, MTL_HUD_LOG_ENABLED=1), trimmed to the leading fields.
REAL_METAL_HUD = """\
2026-09-24 06:42:13.540 wine64[34741:1354250] metal-HUD: 93,664.89,1593.53,117.46,0.00,16.67
2026-09-24 06:42:14.510 wine64[34741:1354246] metal-HUD: 195,664.89,1593.53,16.67,4.08,0.53
2026-09-24 06:42:15.508 wine64[34741:1354250] metal-HUD: 297,664.89,1593.53,31.43,2.23,16.67
"""

WINE_D3D12_PRESENTING = WINE_QUALIFYING.replace(
    "d3d11on12core: leave driver present (hr=0x00000000)\n", REAL_METAL_HUD)


def result(player, wine):
    checks, unsupported = smoke.evaluate(player, wine)
    return {check.name: check for check in checks}, unsupported


class QualifyingRun(unittest.TestCase):
    def test_complete_evidence_qualifies(self):
        checks, _ = result(PLAYER_D3D12, WINE_QUALIFYING)
        self.assertEqual([name for name, c in checks.items() if not c.passed], [])

    def test_every_documented_criterion_is_checked(self):
        checks, _ = result(PLAYER_D3D12, WINE_QUALIFYING)
        self.assertEqual(set(checks), {
            "graphics-api", "no-d3d11-fallback", "relay12-stages",
            "wrapped-resources", "presented-frame", "no-device-removal",
            "no-crash"})


class AbsentEvidence(unittest.TestCase):
    def test_empty_logs_do_not_qualify(self):
        checks, _ = result("", "")
        self.assertFalse(checks["graphics-api"].passed)
        self.assertIn("not reached", checks["graphics-api"].detail)
        self.assertFalse(checks["no-d3d11-fallback"].passed)
        self.assertFalse(checks["wrapped-resources"].passed)
        self.assertFalse(checks["presented-frame"].passed)

    def test_missing_creation_trace_is_not_an_on12_device(self):
        wine = WINE_QUALIFYING.replace("leave driver CreateDevice", "other")
        checks, _ = result(PLAYER_D3D12, wine)
        self.assertFalse(checks["no-d3d11-fallback"].passed)
        self.assertIn("RELAY12_TRACE_CREATION", checks["no-d3d11-fallback"].detail)

    def test_wrapped_marker_is_required(self):
        wine = WINE_QUALIFYING.replace("wrapped resource creation", "buffer creation")
        checks, _ = result(PLAYER_D3D12, wine)
        self.assertFalse(checks["wrapped-resources"].passed)

    def test_present_marker_is_required(self):
        wine = WINE_QUALIFYING.replace("leave driver present", "leave driver flush")
        checks, _ = result(PLAYER_D3D12, wine)
        self.assertFalse(checks["presented-frame"].passed)

    def test_failed_marker_is_not_evidence(self):
        wine = WINE_QUALIFYING.replace(
            "leave driver present (hr=0x00000000)", "leave driver present (hr=0x80004005)")
        checks, _ = result(PLAYER_D3D12, wine)
        self.assertFalse(checks["presented-frame"].passed)
        self.assertFalse(checks["relay12-stages"].passed)


class PresentedFrameEvidence(unittest.TestCase):
    def test_rising_metal_hud_count_is_a_presented_frame(self):
        checks, _ = result(PLAYER_D3D12, WINE_D3D12_PRESENTING)
        self.assertTrue(checks["presented-frame"].passed)
        self.assertIn("93 to 297", checks["presented-frame"].detail)

    def test_single_metal_hud_report_is_not_enough(self):
        one = REAL_METAL_HUD.splitlines(keepends=True)[0]
        wine = WINE_QUALIFYING.replace(
            "d3d11on12core: leave driver present (hr=0x00000000)\n", one)
        checks, _ = result(PLAYER_D3D12, wine)
        self.assertFalse(checks["presented-frame"].passed)

    def test_stalled_metal_hud_count_is_not_a_presented_frame(self):
        stalled = "metal-HUD: 93,1,2\nmetal-HUD: 93,1,2\nmetal-HUD: 93,1,2\n"
        wine = WINE_QUALIFYING.replace(
            "d3d11on12core: leave driver present (hr=0x00000000)\n", stalled)
        checks, _ = result(PLAYER_D3D12, wine)
        self.assertFalse(checks["presented-frame"].passed)
        self.assertIn("did not advance", checks["presented-frame"].detail)

    def test_missing_hud_names_the_setting(self):
        wine = WINE_QUALIFYING.replace("leave driver present", "leave driver flush")
        checks, _ = result(PLAYER_D3D12, wine)
        self.assertIn("MTL_HUD_LOG_ENABLED=1", checks["presented-frame"].detail)


class RejectedEvidence(unittest.TestCase):
    def test_real_d3d11_crash_log_is_rejected(self):
        checks, _ = result(REAL_D3D11_CRASH, WINE_QUALIFYING)
        self.assertEqual(checks["graphics-api"].detail, "selected Direct3D 11.0")
        self.assertIn("Forcing GfxDevice: Direct3D 11", checks["no-d3d11-fallback"].detail)
        self.assertFalse(checks["no-crash"].passed)

    def test_real_silent_on12_fallback_is_rejected(self):
        checks, _ = result(REAL_ON12_FALLBACK, WINE_QUALIFYING)
        self.assertEqual(checks["graphics-api"].detail, "selected Direct3D 11.0")
        self.assertIn("failed to create D3D11On12 device (0x887a0004)",
                      checks["no-d3d11-fallback"].detail)
        self.assertTrue(checks["no-crash"].passed)

    def test_unlogged_fallback_after_on12_success_is_rejected(self):
        # PEAK with patch 0025: the On12 device is accepted, D3DMetal refuses
        # a feature query, and Unity reports no failure before using D3D11.
        player = (
            "Forcing GfxDevice: Direct3D 12\n"
            "d3d12: failed to check D3D12 feature support "
            "D3D12_FEATURE_D3D12_TIGHT_ALIGNMENT (0x80070057).\n"
            "Direct3D:\n    Version:  Direct3D 11.0 [level 11.1]\n")
        checks, _ = result(player, WINE_QUALIFYING)
        self.assertFalse(checks["no-d3d11-fallback"].passed)
        self.assertIn("Unity settled on Direct3D 11.0", checks["no-d3d11-fallback"].detail)

    def test_any_later_non_d3d12_device_is_rejected(self):
        player = PLAYER_D3D12 + "Direct3D:\n    Version:  Direct3D 11.0 [level 11.1]\n"
        checks, _ = result(player, WINE_QUALIFYING)
        self.assertFalse(checks["graphics-api"].passed)

    def test_d3d12_version_must_be_the_whole_api_name(self):
        player = PLAYER_D3D12.replace("Direct3D 12.0", "Direct3D 12.1")
        checks, _ = result(player, WINE_QUALIFYING)
        self.assertFalse(checks["graphics-api"].passed)

    def test_fallback_message_is_rejected(self):
        player = PLAYER_D3D12 + "D3D12 init failed, falling back to D3D11\n"
        checks, _ = result(player, WINE_QUALIFYING)
        self.assertFalse(checks["no-d3d11-fallback"].passed)

    def test_failed_driver_device_is_rejected(self):
        wine = WINE_QUALIFYING.replace(
            "leave driver CreateDevice (hr=0x00000000)",
            "leave driver CreateDevice (hr=0x887A0004)")
        checks, _ = result(PLAYER_D3D12, wine)
        self.assertIn("0x887a0004", checks["no-d3d11-fallback"].detail)

    def test_device_removal_is_rejected_from_either_log(self):
        for player, wine in (
                (PLAYER_D3D12 + "d3d12: device removed\n", WINE_QUALIFYING),
                (PLAYER_D3D12, WINE_QUALIFYING + "hr 0x887a0005\n"),
                (PLAYER_D3D12, WINE_QUALIFYING + "DXGI_ERROR_DEVICE_HUNG\n")):
            with self.subTest(player=player[-30:], wine=wine[-30:]):
                checks, _ = result(player, wine)
                self.assertFalse(checks["no-device-removal"].passed)

    def test_wine_crash_is_rejected(self):
        for line in ("wine: Unhandled page fault on read access to 0000000000000000",
                     "Unhandled exception: page fault on read access"):
            with self.subTest(line=line):
                checks, _ = result(PLAYER_D3D12, WINE_QUALIFYING + line + "\n")
                self.assertFalse(checks["no-crash"].passed)

    def test_wrapped_ownership_failure_is_rejected(self):
        wine = WINE_QUALIFYING + "D3D11On12 wrapped ownership failed, hr 0x80070057.\n"
        checks, _ = result(PLAYER_D3D12, wine)
        self.assertFalse(checks["wrapped-resources"].passed)


class UnsupportedInventory(unittest.TestCase):
    def test_unsupported_operations_are_counted_not_failed(self):
        wine = WINE_QUALIFYING + (
            "warn:d3d11:x On12 PSSetConstantBuffers is not supported.\n" * 2
            + "warn:d3d11:x On12 RSSetState is not supported.\n")
        checks, unsupported = result(PLAYER_D3D12, wine)
        self.assertTrue(all(c.passed for c in checks.values()))
        self.assertEqual(unsupported, {"PSSetConstantBuffers": 2, "RSSetState": 1})


class CommandLine(unittest.TestCase):
    def run_script(self, player, wine):
        with tempfile.TemporaryDirectory() as directory:
            root = pathlib.Path(directory)
            (root / "Player.log").write_text(player)
            (root / "wine.log").write_text(wine)
            completed = subprocess.run(
                [sys.executable, str(SCRIPT), "--player-log", str(root / "Player.log"),
                 "--wine-log", str(root / "wine.log"), "--json", str(root / "r.json")],
                capture_output=True, text=True, check=False)
            report = json.loads((root / "r.json").read_text())
        return completed, report

    def test_exit_status_and_json_follow_the_verdict(self):
        completed, report = self.run_script(PLAYER_D3D12, WINE_QUALIFYING)
        self.assertEqual(completed.returncode, 0, completed.stdout)
        self.assertTrue(report["qualified"])
        completed, report = self.run_script(REAL_D3D11_CRASH, "")
        self.assertEqual(completed.returncode, 1)
        self.assertFalse(report["qualified"])
        self.assertIn("not qualified", completed.stdout)

    def test_unreadable_log_is_a_usage_error(self):
        completed = subprocess.run(
            [sys.executable, str(SCRIPT), "--player-log", "/nonexistent/Player.log",
             "--wine-log", "/nonexistent/wine.log"],
            capture_output=True, text=True, check=False)
        self.assertEqual(completed.returncode, 2)


if __name__ == "__main__":
    unittest.main()
