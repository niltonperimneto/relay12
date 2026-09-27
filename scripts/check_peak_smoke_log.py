#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-only
#
# Decide whether a timed PEAK run qualifies as the D3D12/On12 smoke test.
#
# docs/TESTS.md section 6 names the smoke run as the second runtime
# checkpoint: its log "must select Direct3D 12.0, must not fall back to D3D11,
# and must show wrapped-resource activity and a presented frame without device
# removal, crash, or initialization timeout."  Read by eye, a log like that is
# judged on what is in it; this gate is judged on what is missing from it too.
# A run that never reached device creation contains no crash and no device
# removal, and must still fail.  So every criterion demands positive evidence,
# and an absent line is a failure, never a pass.
#
# Two logs are read:
#
#   * Unity's Player.log, which names the graphics API it selected in its
#     "Direct3D:" block and records Unity's own crash handler;
#   * the Wine process output of the same run, captured with
#     RELAY12_TRACE_CREATION=1, WINEDEBUG including warn+d3d11, and
#     MTL_HUD_ENABLED=1 MTL_HUD_LOG_ENABLED=1.  It carries the core's stage
#     markers, the frontend's unsupported-operation WARNs, and Metal's own
#     once-a-second "metal-HUD: <frames>,..." lines.
#
# Relay12 evidence is read only from the core's stage markers,
#
#     d3d11on12core: <stage> (hr=0x<8 hex digits>)
#
# emitted by traceCreation() in relay12-d3d11/d3d11on12core.cpp.  That is the
# contract for wrapped-resource activity, which the core does not trace yet: a
# successful stage whose name contains "wrapped".  Until that marker exists a
# real run fails the check.
#
# A presented frame has two sources.  A title that presents through D3D11 on
# Relay12 would show a successful "present" stage.  PEAK does not: Unity's D3D12
# renderer presents through its own D3D12 swap chain, so D3DMetal presents and
# Relay12 never sees it.  Metal's performance HUD sees every present, whoever
# issued it, and its log line starts with a running frame count.  A count that
# rises across two lines is frames being presented; one line, or a count that
# never moves, is not.
#
# The same HUD lines give the frame rate, which is reported and never judged:
# it is the before/after figure for performance work, and a slow run is still
# a qualified one.  Only the frame count and the NSLog timestamp in front of
# each line are used -- the HUD's other fields are undocumented -- so a window's
# rate is frames presented between two lines over the time between them.
# Windows are about a second long, which makes "p1_low" the 1st percentile of
# per-second rates, not of individual frame times; with fewer than a hundred
# windows it is the slowest second.  Lines are grouped by process, and the
# process that presented the most frames is the one measured.
#
# The "On12 <Operation> is not supported." WARNs from patch 0023 are reported
# as an inventory and do not fail the run: the smoke criteria do not mention
# them, and the list is the most direct statement of what PEAK needs next.
#
# Usage:
#   python3 scripts/check_peak_smoke_log.py \
#       --player-log Player.log --wine-log wine.log [--json result.json]
#
# result.json carries the verdict, the checks, the unsupported inventory and a
# "frame_rate" object (null when the HUD lines carry no timestamps).
#
# Exit status: 0 qualified, 1 not qualified, 2 unusable input.

import argparse
import collections
import datetime
import json
import math
import pathlib
import re
import sys

STAGE = re.compile(r"d3d11on12core: (?P<stage>.+?) \(hr=0x(?P<hr>[0-9a-fA-F]{8})\)")
UNITY_VERSION = re.compile(r"^\s+Version:\s+(?P<api>Direct3D \d+(?:\.\d+)?)")
FORCED_D3D11 = re.compile(r"^Forcing GfxDevice: Direct3D 11\b", re.MULTILINE)
# Unity announces no fallback: it logs the failed On12 device, retries, and
# then reports a Direct3D 11 block (observed in PEAK, Unity 6000.3.15f1).
FALLBACK = re.compile(
    r"d3d12: failed to create D3D11On12 device \(0x[0-9a-fA-F]{8}\)"
    r"|falling back to (?:Direct3D|D3D) ?11", re.IGNORECASE)
DEVICE_REMOVAL = re.compile(
    r"DXGI_ERROR_DEVICE_(?:REMOVED|HUNG|RESET)|0x887a000[567]"
    r"|device (?:was )?(?:removed|hung)", re.IGNORECASE)
CRASH = re.compile(
    r"^Crash!!!$|A crash has been intercepted by the crash handler"
    r"|^wine: Unhandled|Unhandled exception: ", re.MULTILINE)
UNSUPPORTED = re.compile(r"On12 (?P<op>\w+) is not supported\.")
METAL_HUD = re.compile(r"metal-HUD: (?P<frames>\d+),")
# NSLog's prefix: "2026-09-24 06:42:13.540 wine64[34741:1354250] metal-HUD: 93,"
TIMED_METAL_HUD = re.compile(
    r"^(?P<time>\d{4}-\d{2}-\d{2} \d{2}:\d{2}:\d{2}\.\d+) \S+\[(?P<pid>\d+):\d+\]"
    r" metal-HUD: (?P<frames>\d+),", re.MULTILINE)
WRAPPED_FAILURE = re.compile(r"D3D11On12 (?:wrapped )?ownership (?:failed|rejected)[^\n]*")

Check = collections.namedtuple("Check", "name passed detail")


def failed_hresult(hr):
    return int(hr, 16) & 0x80000000 != 0


def stages(wine_log):
    return [(m["stage"], m["hr"].lower()) for m in STAGE.finditer(wine_log)]


def selected_apis(player_log):
    """The Version line of each Unity "Direct3D:" block, in order."""
    lines = player_log.splitlines()
    apis = []
    for index, line in enumerate(lines):
        if line.strip() != "Direct3D:":
            continue
        for following in lines[index + 1:index + 8]:
            match = UNITY_VERSION.match(following)
            if match:
                apis.append(match["api"])
                break
    return apis


def first_lines(pattern, text, limit=3):
    return [m.group(0).strip() for m in pattern.finditer(text)][:limit]


def check_graphics_api(player_log):
    apis = selected_apis(player_log)
    if not apis:
        return Check("graphics-api", False,
                     "no Direct3D version reported: graphics initialization "
                     "was not reached (timeout or early exit)")
    wrong = [api for api in apis if not re.fullmatch(r"Direct3D 12(?:\.0)?", api)]
    if wrong:
        return Check("graphics-api", False, "selected " + ", ".join(wrong))
    return Check("graphics-api", True, "selected " + apis[-1])


def check_no_fallback(player_log, wine_log, traced):
    evidence = first_lines(FORCED_D3D11, player_log) + first_lines(FALLBACK, player_log)
    # A failure Unity does not log still ends in a Direct3D 11 block: with the
    # On12 device accepted, a later D3D12 check can reject the renderer.
    evidence += [f"Unity settled on {api}" for api in selected_apis(player_log)
                 if api.startswith("Direct3D 11")][:1]
    if evidence:
        return Check("no-d3d11-fallback", False, "; ".join(evidence))
    created = [hr for stage, hr in traced if stage == "leave driver CreateDevice"]
    if not created:
        return Check("no-d3d11-fallback", False,
                     "no 'leave driver CreateDevice' marker: the On12 driver "
                     "was not reached, or RELAY12_TRACE_CREATION=1 was not set")
    if not any(not failed_hresult(hr) for hr in created):
        return Check("no-d3d11-fallback", False,
                     "driver CreateDevice failed: hr=0x" + created[-1])
    return Check("no-d3d11-fallback", True, "On12 driver device created")


def check_relay12_stages(traced):
    failures = [f"{stage} (hr=0x{hr})" for stage, hr in traced if failed_hresult(hr)]
    if failures:
        return Check("relay12-stages", False, "; ".join(failures[:3]))
    return Check("relay12-stages", True, f"{len(traced)} traced stages succeeded")


def check_marker(name, word, traced, wine_log, failure_pattern=None):
    if failure_pattern is not None:
        rejected = first_lines(failure_pattern, wine_log)
        if rejected:
            return Check(name, False, "; ".join(rejected))
    hits = [stage for stage, hr in traced
            if word in stage.lower() and not failed_hresult(hr)]
    if not hits:
        return Check(name, False, f"no successful '{word}' stage marker")
    return Check(name, True, f"{len(hits)} '{word}' stage markers")


def check_presented(traced, wine_log):
    presents = [stage for stage, hr in traced
                if "present" in stage.lower() and not failed_hresult(hr)]
    if presents:
        return Check("presented-frame", True, f"{len(presents)} 'present' stage markers")
    counts = [int(m["frames"]) for m in METAL_HUD.finditer(wine_log)]
    if len(counts) >= 2 and counts[-1] > counts[0]:
        return Check("presented-frame", True,
                     f"Metal HUD counted frames {counts[0]} to {counts[-1]} "
                     f"over {len(counts)} reports")
    if counts:
        return Check("presented-frame", False,
                     f"Metal HUD frame count did not advance ({counts[0]} over {len(counts)} reports)")
    return Check("presented-frame", False,
                 "no successful 'present' stage marker and no Metal HUD frame count "
                 "(run with MTL_HUD_ENABLED=1 MTL_HUD_LOG_ENABLED=1)")


def frame_rate(wine_log):
    """Frame-rate summary from the timestamped HUD lines, or None."""
    by_process = collections.defaultdict(list)
    for match in TIMED_METAL_HUD.finditer(wine_log):
        when = datetime.datetime.strptime(match["time"], "%Y-%m-%d %H:%M:%S.%f")
        by_process[match["pid"]].append((when, int(match["frames"])))

    best = None
    for samples in by_process.values():
        rates, frames, seconds = [], 0, 0.0
        for (start, first), (end, last) in zip(samples, samples[1:]):
            elapsed = (end - start).total_seconds()
            # A count that went backwards is a new device or a restarted
            # process; a zero-length window has no rate.
            if elapsed <= 0 or last < first:
                continue
            rates.append((last - first) / elapsed)
            frames += last - first
            seconds += elapsed
        if rates and (best is None or frames > best[1]):
            best = (rates, frames, seconds)
    if best is None:
        return None

    rates, frames, seconds = best
    ordered = sorted(rates)
    # Nearest rank: the smallest rate at or below which 1% of windows fall.
    p1 = ordered[max(0, math.ceil(0.01 * len(ordered)) - 1)]
    return {
        "mean_fps": round(frames / seconds, 2),
        "p1_low_fps": round(p1, 2),
        "min_fps": round(ordered[0], 2),
        "frames": frames,
        "seconds": round(seconds, 3),
        "windows": len(rates),
    }


def check_absent(name, pattern, logs):
    evidence = [line for text in logs for line in first_lines(pattern, text)]
    if evidence:
        return Check(name, False, "; ".join(evidence[:3]))
    return Check(name, True, "none reported")


def evaluate(player_log, wine_log):
    traced = stages(wine_log)
    checks = [
        check_graphics_api(player_log),
        check_no_fallback(player_log, wine_log, traced),
        check_relay12_stages(traced),
        check_marker("wrapped-resources", "wrapped", traced, wine_log, WRAPPED_FAILURE),
        check_presented(traced, wine_log),
        check_absent("no-device-removal", DEVICE_REMOVAL, (player_log, wine_log)),
        check_absent("no-crash", CRASH, (player_log, wine_log)),
    ]
    unsupported = collections.Counter(m["op"] for m in UNSUPPORTED.finditer(wine_log))
    return checks, unsupported


def read_log(parser, path):
    try:
        return path.read_text(encoding="utf-8", errors="replace")
    except OSError as error:
        parser.error(f"cannot read {path}: {error.strerror}")


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--player-log", type=pathlib.Path, required=True)
    parser.add_argument("--wine-log", type=pathlib.Path, required=True)
    parser.add_argument("--json", type=pathlib.Path)
    args = parser.parse_args()

    wine_log = read_log(parser, args.wine_log)
    checks, unsupported = evaluate(read_log(parser, args.player_log), wine_log)
    rate = frame_rate(wine_log)
    qualified = all(check.passed for check in checks)
    for check in checks:
        print(f"{'pass' if check.passed else 'FAIL'} {check.name}: {check.detail}")
    for op, count in unsupported.most_common():
        print(f"unsupported {op}: {count}")
    if rate:
        print(f"frame rate: {rate['mean_fps']} fps mean, {rate['p1_low_fps']} fps "
              f"1% low over {rate['seconds']} s ({rate['windows']} windows)")
    print(f"PEAK smoke run: {'qualified' if qualified else 'not qualified'}")

    if args.json:
        args.json.write_text(json.dumps({
            "qualified": qualified,
            "checks": [check._asdict() for check in checks],
            "unsupported": dict(unsupported.most_common()),
            "frame_rate": rate,
        }, indent=2) + "\n")
    return 0 if qualified else 1


if __name__ == "__main__":
    sys.exit(main())
