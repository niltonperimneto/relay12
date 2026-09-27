#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-only
#
# Turn two runs of tests/d3d11on12overhead.exe -- one with RELAY12_TELEMETRY
# unset, one with it set to 1 -- into a Markdown table for the job summary,
# and check the one thing about them that is not noise: the telemetry report.
#
# The timings are informational.  The report is not: with telemetry off the
# core must print none (the proxies were never installed), and with it on it
# must print exactly one, at device destruction, whose counts agree with what
# the benchmark dispatched.  A proxy that dropped or doubled a call, or a flush
# proxy with the wrong signature returning garbage, shows up here as a count
# mismatch rather than as a number nobody reads.
#
# Usage:
#   python3 scripts/summarize_overhead.py OFF_STDOUT OFF_STDERR \
#       ON_STDOUT ON_STDERR --draws N --flushes N --command-lists N \
#       >> "$GITHUB_STEP_SUMMARY"

import argparse
import pathlib
import re
import sys

METRIC = re.compile(r"^overhead: (?P<name>\w+)=(?P<value>\S+)$", re.M)
REPORT = re.compile(r"d3d11on12core telemetry: (?P<fields>[^\n]*)")
FIELD = re.compile(r"(?P<key>\w+)=(?P<value>[\d.]+)")


def metrics(text):
    return {m.group("name"): m.group("value") for m in METRIC.finditer(text)}


def reports(text):
    """Every telemetry report in a stderr capture, as {field: number}."""
    found = []
    for match in REPORT.finditer(text):
        fields = {}
        for field in FIELD.finditer(match.group("fields")):
            value = field.group("value")
            fields[field.group("key")] = float(value) if "." in value \
                else int(value)
        found.append(fields)
    return found


def check_reports(off_stderr, on_stderr, draws, flushes, command_lists):
    errors = []
    if reports(off_stderr):
        errors.append("a telemetry report was printed with RELAY12_TELEMETRY "
                      "unset; the proxies must not be installed")
    on = reports(on_stderr)
    if len(on) != 1:
        errors.append(f"{len(on)} telemetry reports with RELAY12_TELEMETRY=1, "
                      "expected exactly one at device destruction")
        return errors
    report = on[0]
    expected = {
        "draws": draws,
        "flushes": flushes,
        # The mock driver submits on every flush and never on its own, so
        # every submission is an explicit one.
        "submits": flushes,
        "opportunistic_submits": 0,
        # Executes on the immediate table only; draws recorded on a deferred
        # context go through its own table, which is not wrapped.
        "command_lists": command_lists,
    }
    for key, value in expected.items():
        if report.get(key) != value:
            errors.append(f"telemetry {key}={report.get(key)}, expected {value}")
    return errors


def table(off, on):
    names = [name for name in off if name != "telemetry"]
    names += [name for name in on if name not in names and name != "telemetry"]
    lines = [
        "### D3D11On12 host dispatch overhead (mock driver, informational)",
        "",
        "| Metric (ns per call) | telemetry off | telemetry on |",
        "| :--- | ---: | ---: |",
    ]
    for name in names:
        lines.append(f"| `{name}` | {off.get(name, '-')} | {on.get(name, '-')} |")
    return "\n".join(lines) + "\n"


def main(argv=None):
    parser = argparse.ArgumentParser()
    for name in ("off_stdout", "off_stderr", "on_stdout", "on_stderr"):
        parser.add_argument(name, type=pathlib.Path)
    parser.add_argument("--draws", type=int, required=True)
    parser.add_argument("--flushes", type=int, required=True)
    parser.add_argument("--command-lists", type=int, required=True)
    args = parser.parse_args(argv)

    def read(path):
        return path.read_text(errors="replace")

    errors = check_reports(read(args.off_stderr), read(args.on_stderr),
                           args.draws, args.flushes, args.command_lists)
    off, on = metrics(read(args.off_stdout)), metrics(read(args.on_stdout))
    if off.get("telemetry") != "off" or on.get("telemetry") != "on":
        errors.append("the two runs did not see the telemetry switch they "
                      "were given")
    sys.stdout.write(table(off, on))
    if errors:
        print("\n".join(errors), file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
