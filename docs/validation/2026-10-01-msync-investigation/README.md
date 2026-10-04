# MSYNC runtime investigation — 2026-10-01

Status: launch isolation improved; PEAK/MSYNC rendering is **not validated**.

The installed test runtime at `~/relay12-work/runtime/Wine` was exercised with
fresh disposable prefixes, then an initialized prefix with server shutdown and
wait between modes. The Relay bundle identifies source revision
`6373fdd439a4863cebed37e70c66806a456dac53`.

| Check | Standard Wine sync | MSYNC | ESYNC |
| --- | --- | --- | --- |
| Initialized-prefix `cmd /c echo SYNC_PROBE_OK` | Exit 0 | Exit 0 | Exit 0 |
| `peak_on12_probe.exe`, 30-second limit | Timeout | Timeout | Timeout |

The first standard-sync prefix initialization exceeded a 25-second limit;
subsequent command tests on an initialized prefix passed in every mode.
MSYNC logged both Mach-port bootstrap and “up and running.” The graphics probe
logs reached MoltenVK initialization and never printed the On12 verdict. This
does not establish the active D3D12 backend or an MSYNC-specific graphics bug.
All test servers were stopped after the probes.

The PEAK smoke runner refused the existing marked test prefix because its game
directory lacks `d3d11.dll`. The game was not launched or restaged. This test
layout therefore does not currently reproduce the previously successful smoke
configuration. Local logs are retained at `~/relay12-work/msync-investigation/`.

## PEAK recovery procedure

Run this procedure in a disposable prefix made from the signed-in Steam test
copy. Never add `.relay12-peak-prefix` to a player's bottle.

1. **Stage and audit the game directory.** Beside `PEAK.exe`, install the
   matching `d3d11.dll`, `d3d11on12.dll`, `d3d11on12core.dll`, and
   `d3d11on12host.dll`. The runner refuses an incomplete set and records every
   file in `launch.json`. The runtime's builtin
   `lib/wine/x86_64-windows/d3d11on12host.dll` must have the same hash as the
   staged host; otherwise Wine can silently select the old frontend.
2. **Use explicit DLL routing.** The runner sets
   `WINEDLLOVERRIDES=d3d11=n,b;d3d11on12,d3d11on12core,dxilconv=n;d3d11on12host,d3d12,dxgi=b;...`.
   The `d3d11=n,b` entry requires the Relay12 native shim first while retaining
   Wine's builtin fallback for ordinary D3D11 applications; the On12 and DXIL
   modules remain native-only. Check the exact value in `launch.json`, not the
   parent shell.
3. **Capture startup diagnostics.** `run-peak-smoke.py` now appends
   `warn+d3d11,+relay,+d3d12` to `WINEDEBUG`, enables Relay12 creation tracing
   and Metal HUD logging, and writes `wine.log`. Start with `--sync none`,
   then repeat with `--sync msync` (and `--sync esync` if needed), using a
   fresh output directory for each run. The prefix server is stopped and
   waited on before every mode change.
4. **Verify Unity's actual log.** Unity normally writes
   `drive_c/users/<user>/AppData/LocalLow/<Company>/<Game>/Player.log`, not
   the host output directory. After the run the runner searches every user
   profile, copies the newest standard log to `out/Player.log`, and records
   its source in `player-log-source.txt`. If no log exists, inspect
   `startup-failure.json` and `wine.log`; this distinguishes Steam
   authentication/launch failure from On12 initialization.
5. **Require first-frame evidence.** Run the checker on the captured log. A
   valid result must select Direct3D 12, create the On12 device, show wrapped
   activity, and advance Metal HUD frame counts. The resulting `result.json`
   includes mean FPS, one-percent-low FPS, minimum FPS, frame count, and
   timing windows. A timeout, absent `Player.log`, static HUD, MSYNC Mach-port
   error, missing DXIL converter, or unsupported feature level is a failed
   startup—not a performance result.

Example:

```sh
python3 scripts/run-peak-smoke.py --wine "$RUNTIME/bin/wine64" \
  --prefix "$PREFIX" --out "$RUN/msync" --sync msync \
  --steam-wait 90 --wait 240
```

Before retrying a no-log run, sign in to Steam in the isolated runtime copy
and confirm that `steam.exe -applaunch 3527290` produces a PEAK process. Do
not treat a timed-out Steam handoff as evidence of an MSYNC rendering bug.

## Integration change

The smoke runner now accepts `--sync none|esync|msync`, clears conflicting sync
settings, pins the loader and server, and stops and waits for the marked test
prefix's server before starting Steam. It records selected settings in
`launch.json`. Default mode is explicitly `none` rather than inherited.

Wine's `dlls/ntdll/unix/msync.c:msync_init()` explicitly exits when a non-MSYNC
client connects to an MSYNC server; the MSYNC client also requires the server's
Mach bootstrap service. Merely changing the game's environment while Steam
and wineserver remain alive is therefore not a valid mode-switch test. This
is an integration hazard found in source, not a proven cause of the reported
PEAK crash. The Whisky UI/runtime launcher has not been changed in this pass.

Validation: all 147 Python tests and the six toolchain-independent layout,
header, interface, shared-state, adapter and security gates passed.

## Remaining work

1. Recover the exact successful PEAK runtime, DLL routing and launch environment;
   reconcile its runtime-installed modules with the runner's app-local layout.
2. Obtain a passing D3DMetal/On12 probe with that configuration, then repeat with
   MSYNC, ESYNC and standard sync, restarting the prefix between each mode.
3. Run PEAK and require positive On12 and advancing Metal frame evidence for
   each mode, followed by gameplay, exit and relaunch checks.
4. Apply equivalent whole-prefix synchronization consistency to the Whisky
   launch/settings lifecycle, preserving user applications rather than silently
   stopping a live player bottle. Fix Wine or Relay internals only against a
   reproducible failure and its captured stack/logs.
