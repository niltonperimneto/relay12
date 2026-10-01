#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-only
#
# Tests for the opt-in DDI telemetry in relay12-d3d11/d3d11on12core.cpp and
# for scripts/summarize_overhead.py, which reads what it reports.
#
# The first telemetry draft wrapped pfnFlush as VOID(HDEVICE) when the slot is
# BOOL(HDEVICE, UINT, UINT).  C++ rejects that assignment, but nothing here
# compiles on a developer machine (AGENTS.md section 5.2), so the mismatch
# would have cost a CI round trip at best.  These tests read every proxy the
# core installs, look up the slot it replaces in D3DWDDM2_6DDI_DEVICEFUNCS,
# and compare the proxy's signature with the slot's typedef.
#
# Run: python3 -m unittest discover -s tests -p 'test_*.py'

import importlib.util
import pathlib
import re
import tempfile
import unittest
import unittest.mock

REPOSITORY = pathlib.Path(__file__).resolve().parent.parent
CORE = REPOSITORY / "relay12-d3d11" / "d3d11on12core.cpp"
HEADER = REPOSITORY / "relay12-d3d11" / "ddi" / "wine_d3d11ddi.h"

SPEC = importlib.util.spec_from_file_location(
    "summarize_overhead", REPOSITORY / "scripts" / "summarize_overhead.py")
summarize = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(summarize)


# windows.h spellings of the same type: VOID is a macro for void.
ALIASES = {"VOID": "void"}


def parameter_types(parameters):
    """The types of a parameter list, with the parameter names dropped."""
    types = []
    for parameter in parameters.split(","):
        words = parameter.split()
        if not words:
            continue
        # The last word is the name unless the parameter is unnamed, which a
        # single word (a bare type) is.
        types.append(" ".join(words[:-1] if len(words) > 1 else words))
    return types


def typedef_signature(header, name):
    match = re.search(
        rf"typedef\s+(?P<ret>\w+)\s*\(\s*\*\s*{name}\s*\)\s*\((?P<params>[^)]*)\)",
        header)
    if not match:
        raise AssertionError(f"no typedef for {name}")
    return ALIASES.get(match.group("ret"), match.group("ret")), \
        parameter_types(match.group("params"))


def function_signature(source, name):
    match = re.search(
        rf"^(?P<ret>\w+)\s+{name}\s*\((?P<params>[^)]*)\)\s*(?:noexcept\s*)?\n\{{",
        source, re.M)
    if not match:
        raise AssertionError(f"no definition of {name}")
    return ALIASES.get(match.group("ret"), match.group("ret")), \
        parameter_types(match.group("params"))


def slot_typedefs(header):
    body = re.search(r"(?:typedef )?struct D3DWDDM2_6DDI_DEVICEFUNCS\s*\{(?P<body>.*?)\}",
                     header, re.S)
    if not body:
        raise AssertionError("no D3DWDDM2_6DDI_DEVICEFUNCS declaration")
    return dict((slot, typedef) for typedef, slot in
                re.findall(r"^\s*(\w+)\s+(pfn\w+);", body.group("body"), re.M))


def installed_proxies(source):
    body = re.search(r"^void installTelemetry\(.*?^\}", source, re.S | re.M)
    if not body:
        raise AssertionError("no installTelemetry definition")
    return body.group(0), re.findall(r"funcs\.(pfn\w+)\s*=\s*(telemetry\w+);",
                                     body.group(0))


class ProxySignatures(unittest.TestCase):
    def setUp(self):
        self.source = CORE.read_text()
        self.header = HEADER.read_text()

    def test_every_proxy_matches_the_slot_it_replaces(self):
        slots = slot_typedefs(self.header)
        _, proxies = installed_proxies(self.source)
        self.assertEqual(
            {slot for slot, _ in proxies},
            {"pfnDraw", "pfnDrawIndexed", "pfnDrawInstanced",
             "pfnDrawIndexedInstanced", "pfnFlush", "pfnCommandListExecute"})
        for slot, proxy in proxies:
            with self.subTest(slot=slot):
                self.assertEqual(function_signature(self.source, proxy),
                                 typedef_signature(self.header, slots[slot]))

    def test_flush_proxy_returns_the_driver_result(self):
        # The regression that motivated this file.
        ret, params = function_signature(self.source, "telemetryFlush")
        self.assertEqual(ret, "BOOL")
        self.assertEqual(params, ["D3D10DDI_HDEVICE", "UINT", "UINT"])

    def test_saved_slots_use_the_slot_typedefs(self):
        slots = slot_typedefs(self.header)
        fields = dict((name, typedef) for typedef, name in re.findall(
            r"^\s+(PFN\w+)\s+(\w+);",
            re.search(r"struct DeviceTelemetry\s*\{.*?\};", self.source,
                      re.S).group(0), re.M))
        body, proxies = installed_proxies(self.source)
        for slot, _ in proxies:
            saved = re.search(rf"telemetry\.(\w+)\s*=\s*funcs\.{slot};", body)
            with self.subTest(slot=slot):
                self.assertIsNotNone(saved, f"{slot} is not saved")
                self.assertEqual(fields[saved.group(1)], slots[slot])


def function_body(source, name):
    # The definition, not a forward declaration: the signature ends in a brace.
    body = re.search(rf"^\w[\w\s\*&]*\b{name}\([^;{{]*?\)\s*(?:noexcept\s*)?\n\{{.*?^\}}",
                     source, re.S | re.M)
    if not body:
        raise AssertionError(f"no definition of {name}")
    return body.group(0)


class ResourceCensus(unittest.TestCase):
    """The live-object census is only worth reading if nothing bypasses it."""

    def setUp(self):
        self.source = CORE.read_text()

    def test_resources_join_a_device_only_through_the_counted_link(self):
        # A new creation path that links its resource by hand would be
        # invisible to the census, and a leak through it would read as flat.
        link = function_body(self.source, "linkOwnerResource")
        for match in re.finditer(r"(\w+)->resources\s*=\s*([^;]+);", self.source):
            if match.group(2).strip().endswith("->ownerNext"):
                continue    # an unlink or the teardown pop, not a new member
            with self.subTest(statement=match.group(0)):
                self.assertIn(match.group(0), link)

    def test_unlinking_uncounts(self):
        body = function_body(self.source, "unlinkOwnerResource")
        self.assertIn("countOwnerResource(resource, -1)", body)

    def test_every_view_is_counted_both_ways(self):
        self.assertEqual(
            len(re.findall(r"\+\+\w+->viewCount;", self.source)),
            len(re.findall(r"countCensus\(\w+->census\.(?:renderTargets|shaderResourceViews)", self.source)))
        srv_destroy = function_body(self.source, "destroyShaderResourceViewState")
        self.assertIn("InterlockedDecrement(&view->owner->census.shaderResourceViews)", srv_destroy)
        destroy = function_body(self.source, "destroyRenderTargetState")
        self.assertIn("InterlockedDecrement(&view->owner->census.renderTargets)",
                      destroy)

    def test_the_destruction_census_is_taken_before_teardown(self):
        body = function_body(self.source, "destroyAdapterState")
        self.assertLess(body.index("snapshotCensus(state->census)"),
                        body.index("destroyAllDeferredWork(state)"))

    def test_the_census_line_is_not_read_as_a_telemetry_report(self):
        line = ("d3d11on12core census (device destroyed): flushes=0 buffers=0 "
                "textures=2 peak_textures=2 rtvs=1 peak_rtvs=1 "
                "orphaned_textures=0 peak_orphaned_textures=1\n")
        self.assertIsNone(summarize.REPORT.search(line))


class Gating(unittest.TestCase):
    def setUp(self):
        self.source = CORE.read_text()

    def test_nothing_is_wrapped_before_the_switch_is_checked(self):
        body, _ = installed_proxies(self.source)
        switch = body.index("if (!telemetryTicksPerSecond)")
        self.assertLess(body.index("InitOnceExecuteOnce(&telemetryInitOnce"),
                        switch)
        self.assertLess(switch, body.index("funcs.pfn"))

    def test_the_switch_is_the_documented_variable(self):
        self.assertIn('GetEnvironmentVariableW(L"RELAY12_TELEMETRY"',
                      self.source)

    def test_frequency_is_not_queried_per_call(self):
        # Once, in the InitOnce callback; never in a proxy.
        self.assertEqual(self.source.count("QueryPerformanceFrequency("), 1)

    def test_installed_after_the_device_exists(self):
        body = re.search(r"^HRESULT createDriverDevice\(.*?^\}", self.source,
                         re.S | re.M).group(0)
        self.assertLess(body.index("state->deviceCreated = true;"),
                        body.index("installTelemetry(state);"))


OFF_STDOUT = """overhead: telemetry=off
overhead: table_draw_ns=4.1
overhead: entry_flush_ns=20.0
0 failure(s)
"""
ON_STDOUT = OFF_STDOUT.replace("telemetry=off", "telemetry=on").replace(
    "4.1", "31.5")
ON_STDERR = ("d3d11on12core telemetry: draws=600000 draw_avg_ns=25 "
             "slow_draws=0 flushes=50000 flush_avg_ns=40 submits=50000 "
             "opportunistic_submits=0 command_lists=1000 "
             "execute_avg_ns=90\n")


class Summary(unittest.TestCase):
    def run_main(self, off_stdout=OFF_STDOUT, off_stderr="", on_stdout=ON_STDOUT,
                 on_stderr=ON_STDERR):
        with tempfile.TemporaryDirectory() as directory:
            root = pathlib.Path(directory)
            paths = []
            for name, text in (("off.out", off_stdout), ("off.err", off_stderr),
                               ("on.out", on_stdout), ("on.err", on_stderr)):
                (root / name).write_text(text)
                paths.append(str(root / name))
            with unittest.mock.patch("sys.stdout"), \
                    unittest.mock.patch("sys.stderr"):
                return summarize.main(paths + ["--draws", "600000",
                                               "--flushes", "50000",
                                               "--command-lists", "1000"])

    def test_agreeing_runs_pass(self):
        self.assertEqual(self.run_main(), 0)

    def test_a_report_with_telemetry_off_fails(self):
        self.assertEqual(self.run_main(off_stderr=ON_STDERR), 1)

    def test_a_missing_report_fails(self):
        self.assertEqual(self.run_main(on_stderr=""), 1)

    def test_a_dropped_draw_fails(self):
        self.assertEqual(self.run_main(
            on_stderr=ON_STDERR.replace("draws=600000", "draws=599999")), 1)

    def test_an_opportunistic_submit_against_the_mock_fails(self):
        self.assertEqual(self.run_main(on_stderr=ON_STDERR.replace(
            "opportunistic_submits=0", "opportunistic_submits=3")), 1)

    def test_a_dropped_command_list_execute_fails(self):
        self.assertEqual(self.run_main(on_stderr=ON_STDERR.replace(
            "command_lists=1000", "command_lists=999")), 1)

    def test_table_lists_both_runs(self):
        text = summarize.table(summarize.metrics(OFF_STDOUT),
                               summarize.metrics(ON_STDOUT))
        self.assertIn("| `table_draw_ns` | 4.1 | 31.5 |", text)
        self.assertNotIn("telemetry` |", text)


if __name__ == "__main__":
    unittest.main()
