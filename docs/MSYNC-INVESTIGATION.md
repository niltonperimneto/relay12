# MSYNC runtime investigation — 2026-09-27

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
