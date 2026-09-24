#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-only
"""Run the first-frame harness in an explicitly dedicated Wine/GPTK prefix."""
import argparse
import os
from pathlib import Path
import subprocess
import sys


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--wine", type=Path, required=True)
    parser.add_argument("--prefix", type=Path, required=True)
    parser.add_argument("--artifacts", type=Path, required=True)
    parser.add_argument("--timeout", type=int, default=120)
    parser.add_argument("--test", choices=("triangle", "wrapped"), default="triangle")
    args = parser.parse_args()
    # Keep the bin/ launcher location: wine64 commonly links into lib/wine/.
    wine = args.wine.absolute()
    artifacts = args.artifacts.resolve()
    prefix = args.prefix.resolve()
    if args.timeout <= 0:
        parser.error("timeout must be positive")
    required = ("d3d11.dll", "d3d11on12core.dll", "d3d11on12host.dll",
                "d3d11on12.dll", "dxilconv.dll", f"d3d11_e2e_{args.test}.exe")
    for name in required:
        if not (artifacts / name).is_file():
            parser.error(f"missing runtime artifact: {artifacts / name}")
    marker = prefix / ".relay12-frame-prefix"
    if prefix.exists() and any(prefix.iterdir()) and not marker.is_file():
        parser.error("use an empty prefix or one created by this script")
    runtime = wine.parent.parent
    shared = runtime / "lib/external/libd3dshared.dylib"
    if not wine.is_file() or not shared.is_file():
        parser.error("--wine must name a configured Wine/GPTK runtime with D3DMetal")
    prefix.mkdir(parents=True, exist_ok=True)
    marker.touch()
    env = dict(os.environ, WINEPREFIX=str(prefix), WINEDEBUG=os.environ.get("WINEDEBUG", "-all"),
               RELAY12_EXPERIMENTAL_FRAME="1", RELAY12_TRACE_CREATION="1",
               CX_APPLEGPTK_LIBD3DSHARED_PATH=str(shared),
               WINEDLLOVERRIDES="d3d11,d3d11on12,d3d11on12core,dxilconv=n;"
               "d3d11on12host,d3d12,dxgi=b;mscoree,mshtml=")
    try:
        result = subprocess.run([str(wine), str(artifacts / required[-1])],
                                cwd=artifacts, env=env, timeout=args.timeout)
        return result.returncode
    except subprocess.TimeoutExpired:
        print(f"GPU harness timed out after {args.timeout} seconds", file=sys.stderr)
        return 124
    finally:
        # This prefix is dedicated to the harness, never an application bottle.
        subprocess.run([str(wine.parent / "wineserver"), "-k"], env=env,
                       timeout=10, check=False)


if __name__ == "__main__":
    sys.exit(main())
