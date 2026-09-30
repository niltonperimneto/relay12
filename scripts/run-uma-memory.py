#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-only
"""Compare UMA policies in an isolated Wine/GPTK prefix; preserve raw samples."""
import argparse
import json
import os
import platform
from pathlib import Path
import re
import statistics
import subprocess
import sys


def summarize(log):
    frames = [float(ms) for cycle, ms in re.findall(r'\[frame\] cycle=(\d+) texture=\d+ wall_ms=([\d.]+)', log)
              if int(cycle) > 0]  # cycle zero warms shaders and pools
    samples = sorted(frames)
    return {'transfer_frames': len(samples),
            'median_ms': statistics.median(samples) if samples else None,
            'p95_ms': samples[min(len(samples)-1, (len(samples)*95 + 99)//100 - 1)] if samples else None,
            'memory_telemetry': [line for line in log.splitlines() if ' memory telemetry:' in line or ' pool telemetry:' in line]}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--wine', type=Path, required=True)
    parser.add_argument('--artifacts', type=Path, required=True)
    parser.add_argument('--prefix', type=Path, required=True)
    parser.add_argument('--output', type=Path, required=True)
    parser.add_argument('--sync', choices=('none', 'msync'), default='msync')
    parser.add_argument('--trials', type=int, default=5)
    parser.add_argument('--timeout', type=int, default=180)
    parser.add_argument('--correctness-only', action='store_true')
    args = parser.parse_args()
    if args.trials < 1 or args.timeout < 1: parser.error('trials and timeout must be positive')
    prefix, artifacts = args.prefix.resolve(), args.artifacts.resolve()
    wine = args.wine.absolute()
    runtime = wine.parent.parent
    for name in ('d3d11.dll', 'd3d11on12.dll', 'd3d11on12core.dll', 'd3d11on12host.dll', 'dxilconv.dll', 'd3d11_e2e_uma.exe'):
        if not (artifacts / name).is_file(): parser.error(f'missing artifact {name}')
    marker = prefix / '.relay12-uma-prefix'
    if prefix.exists() and any(prefix.iterdir()) and not marker.is_file(): parser.error('prefix must be empty or dedicated to this harness')
    shared = runtime / 'lib/external/libd3dshared.dylib'
    if not wine.is_file() or not shared.is_file(): parser.error('requires a Wine/GPTK runtime with D3DMetal')
    prefix.mkdir(parents=True, exist_ok=True); marker.touch()
    args.output.mkdir(parents=True, exist_ok=True)
    env = dict(os.environ, WINEPREFIX=str(prefix), WINESERVER=str(wine.parent / 'wineserver'), WINELOADER=str(wine),
               WINEMSYNC='1' if args.sync == 'msync' else '0', WINEESYNC='0', WINEFSYNC='0', WINEDEBUG='-all',
               CX_APPLEGPTK_LIBD3DSHARED_PATH=str(shared), RELAY12_EXPERIMENTAL_FRAME='1', RELAY12_TELEMETRY='1',
               WINEDLLOVERRIDES='d3d11,d3d11on12,d3d11on12core,dxilconv=n;d3d11on12host,d3d12,dxgi=b;mscoree,mshtml=')
    modes = [('legacy', 'legacy', '0'), ('balanced', 'balanced', '0'), ('direct', 'balanced', '1')]
    records = []
    result_path = args.output / 'result.json'
    def stop():
        for flag in ('-k', '-w'):
            subprocess.run([str(wine.parent / 'wineserver'), flag], env=env, timeout=20,
                           stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL, check=False)
    try:
        for trial in range(args.trials):
            order = modes[trial % 3:] + modes[:trial % 3]
            if trial % 2: order = list(reversed(order))
            for label, profile, direct in order:
                stop()
                env.update(D3D11ON12_COMPAT_MemoryProfile=profile, D3D11ON12_COMPAT_UMADirectInitialUpload=direct)
                command = [str(wine), str(artifacts / 'd3d11_e2e_uma.exe')]
                if not args.correctness_only: command.append('--bench')
                path = args.output / f'{trial}-{label}.log'
                with path.open('w') as log:
                    try:
                        proc = subprocess.run(command, cwd=artifacts, env=env, stdout=log, stderr=subprocess.STDOUT, timeout=args.timeout)
                        code = proc.returncode
                    except subprocess.TimeoutExpired:
                        code = 124
                text = path.read_text(errors='replace')
                if code == 0 and '[ ok ] UMA initialization, shader sampling and readback' not in text:
                    code = 1
                records.append(dict(trial=trial, mode=label, exit_code=code, log=path.name, **summarize(text)))
                result_path.write_text(json.dumps({'sync_requested': args.sync, 'scope': 'D3D11On12 transfer workload; not PEAK FPS or physical residency',
                                                   'wine': str(wine), 'host': platform.platform(),
                                                   'runtime_flags': {key: env.get(key) for key in ('D3DM_WINE_UNIX_CALL', 'D3DM_MTL4', 'WINEMSYNC', 'WINEESYNC', 'WINEFSYNC')},
                                                   'samples': records}, indent=2) + '\n')
                print(f'{trial} {label}: exit={code}', flush=True)
                if code: return 1
    finally:
        stop()
    return 0


if __name__ == '__main__':
    sys.exit(main())
