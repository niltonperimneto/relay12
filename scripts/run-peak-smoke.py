#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-only
"""Launch PEAK through Steam in a Relay12 test prefix and judge the run.

This is the runner for the timed PEAK smoke run in docs/TESTS.md section 6.
Steam is started inside the prefix under the given Wine runtime, PEAK is
launched with "steam.exe -applaunch 3527290" so the game gets its app id, its
launch options and a signed-in client the way a player's would, and after the
wait the logs go to check_peak_smoke_log.py, whose exit status this returns.

The prefix must carry a .relay12-peak-prefix marker. Pointing the runner at a
real bottle would let a newer Wine upgrade it on first start, so a prefix
without the marker is refused. The Relay12 DLLs are expected beside PEAK.exe
and the runtime's d3d12 slot to hold the d3d12 shim; this script stages
nothing.

Metal's performance HUD is enabled for the run: its once-a-second frame counts
are the evidence of presented frames when Unity presents through D3D12.
"""
import argparse
import os
from pathlib import Path, PureWindowsPath
import subprocess
import sys
import time

APP_ID = "3527290"
STEAM = r"C:\Program Files (x86)\Steam\steam.exe"
MARKER = ".relay12-peak-prefix"
CHECK = Path(__file__).resolve().parent / "check_peak_smoke_log.py"


def windows_path(path):
    """The Z: drive path Wine maps to a host path."""
    return str(PureWindowsPath("Z:\\", *Path(path).resolve().parts[1:]))


def run_environment(runtime, prefix):
    # The unsupported-operation inventory is read from warn+d3d11. An inherited
    # WINEDEBUG such as "-all" would silence it, so the channel is appended to
    # whatever the caller set; Wine applies the later entry.
    debug = os.environ.get("WINEDEBUG", "-all")
    return dict(os.environ, WINEPREFIX=str(prefix),
                WINEDEBUG=f"{debug},warn+d3d11" if debug else "warn+d3d11",
                RELAY12_EXPERIMENTAL_FRAME="1", RELAY12_TRACE_CREATION="1",
                MTL_HUD_ENABLED="1", MTL_HUD_LOG_ENABLED="1",
                CX_APPLEGPTK_LIBD3DSHARED_PATH=str(runtime / "lib/external/libd3dshared.dylib"),
                WINEDLLOVERRIDES="d3d11,d3d11on12,d3d11on12core,dxilconv=n;"
                "d3d11on12host,d3d12,dxgi=b;mscoree,mshtml=")


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__,
                                     formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--wine", type=Path, required=True, help="the runtime's bin/wine64")
    parser.add_argument("--prefix", type=Path, required=True)
    parser.add_argument("--out", type=Path, required=True, help="directory for the logs and verdict")
    parser.add_argument("--steam-wait", type=int, default=60,
                        help="seconds to let Steam sign in before the launch")
    parser.add_argument("--wait", type=int, default=240, help="seconds to let PEAK run")
    args = parser.parse_args(argv)

    wine = args.wine.absolute()
    prefix = args.prefix.resolve()
    if args.steam_wait < 0 or args.wait <= 0:
        parser.error("waits must be positive")
    if not (prefix / MARKER).is_file():
        parser.error(f"{prefix} has no {MARKER} marker; use a clone made for this run, "
                     "never a player's bottle")
    game = prefix / "drive_c/Program Files (x86)/Steam/steamapps/common/PEAK"
    if not (prefix / "drive_c/Program Files (x86)/Steam/steam.exe").is_file():
        parser.error(f"no Steam install in {prefix}")
    for name in ("PEAK.exe", "d3d11.dll", "d3d11on12core.dll", "d3d11on12host.dll", "d3d11on12.dll"):
        if not (game / name).is_file():
            parser.error(f"missing {game / name}")
    runtime = wine.parent.parent
    if not wine.is_file() or not (runtime / "lib/external/libd3dshared.dylib").is_file():
        parser.error("--wine must name a configured Wine/GPTK runtime with D3DMetal")

    out = args.out.resolve()
    out.mkdir(parents=True, exist_ok=True)
    player_log, wine_log = out / "Player.log", out / "wine.log"
    player_log.unlink(missing_ok=True)
    # A real Steam launch: without this file the game must get its app id from Steam.
    (game / "steam_appid.txt").unlink(missing_ok=True)
    env = run_environment(runtime, prefix)

    with open(wine_log, "w") as log:
        steam = subprocess.Popen([str(wine), STEAM, "-silent", "-cef-disable-gpu",
                                  "-cef-disable-gpu-compositing"],
                                 env=env, stdout=log, stderr=subprocess.STDOUT)
        try:
            time.sleep(args.steam_wait)
            # A second steam.exe hands -applaunch to the running client and exits.
            subprocess.run([str(wine), STEAM, "-applaunch", APP_ID, "-logFile",
                            windows_path(player_log)],
                           env=env, stdout=log, stderr=subprocess.STDOUT, timeout=120, check=False)
            time.sleep(args.wait)
        finally:
            subprocess.run([str(wine.parent / "wineserver"), "-k"], env=env, timeout=30, check=False)
            steam.wait(timeout=30)

    if not player_log.is_file():
        print(f"PEAK wrote no log to {player_log}: it did not start", file=sys.stderr)
        return 1
    return subprocess.run([sys.executable, str(CHECK), "--player-log", str(player_log),
                           "--wine-log", str(wine_log), "--json", str(out / "result.json")],
                          check=False).returncode


if __name__ == "__main__":
    sys.exit(main())
