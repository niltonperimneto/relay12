# PEAK direct startup diagnostic — 2026-10-02

An explicitly authorized diagnostic temporarily supplied `steam_appid.txt` with
app ID **3527290** in the marked cloned PEAK directory and directly launched
`PEAK.exe -force-d3d12` under MSYNC. The file was removed during cleanup. This
was a startup diagnostic; it does not qualify authenticated Steam gameplay.
The normal game bottle and installed runtime were untouched.

The matching runtime and bundle are from `e2b99d2`, rather than the later pool
policy build tested by Agent B. [Placement checks](../2026-10-02-phase1-agent-c/placement.json)
establish that the app-local DLLs and installed builtin host match.

## Observed startup

PEAK appeared after approximately 1.1 seconds and produced a fresh Unity log.
Targeted loader traces confirmed native `d3d11.dll`, `d3d11on12core.dll`,
`d3d11on12.dll`, `UnityPlayer.dll` and `steam_api64.dll`, with builtin
`d3d11on12host.dll`, `d3d12.dll` and `dxgi.dll`. Unity logged D3D12 graphics
initialization. There were no DLL import failures, unhandled page faults,
device-removal reports or MSYNC failure markers.

The fresh Unity log reports **Steamworks SteamAPI_Init failed**, followed by
repeated Steamworks-not-initialized messages. Providing the app ID therefore
allows the game process and graphics initialization to begin, but does not
resolve the current Steam initialization block. The process was still alive
at the recorded 24.3-second observation; no natural termination was observed.
This evidence establishes an application Steam initialization failure and
does not establish a rendering crash. Without a useful thread sample, it does
not prove that Steam initialization is the sole cause of the main-thread wait
or identify the exact wait site.

The native thread sampler began but timed out after 15 seconds. The diagnostic
harness then stopped only its own launched PEAK process through its cleanup
handler and removed the temporary app-ID file. Steam's existing visible client
was preserved. The `seh` dispatch counts include thread-naming/debug-output
codes (`406d1388`, `40010006`, `4001000a`); handled exception dispatch alone is
not crash evidence. No useful native thread sample was obtained.

[Structured evidence](result.json) contains module kinds and safe event classes.
Raw Wine and Unity logs remain in the private test workspace because Unity and
Steam logs may contain authentication material. No frame-time claim follows
from this heavily traced diagnostic. Next: confirm Steam initialization in the
isolated client, then obtain an authenticated launch and gameplay measurements.
