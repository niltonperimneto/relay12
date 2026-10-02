# Agent C: PEAK launch diagnostics — 2026-10-02

Read-only investigation of the marked isolated PEAK copy finds no DLL placement
mismatch. All five app-local graphics DLLs match the verified `e2b99d2` CI
artifact, and the cloned runtime's installed builtin `d3d11on12host.dll` matches
its staged copy. [Placement hashes](placement.json) record the comparison.
The marker is present, and `steam_appid.txt` is absent. No normal user bottle or
installed runtime was changed.

Wine selects the installed builtin host ahead of the app-local copy when
`d3d11on12host=b`; the runtime-host hash guard therefore matters even when all
adjacent DLLs look correct. This test clone satisfies that guard. The current
uncommitted runner also permits `d3d11=n,b`, requires the app-local bundle, and
searches fresh standard Unity Player.log locations.

## What the existing launch evidence establishes

The [sanitized timeline](timeline.json) distinguishes three attempts:

- The old-frontend traced run logged on at 00:02:01 on October 1, produced a
  PEAK process event at 00:02:14, and rendered D3D12 frames.
- The matched-runtime attempt at 00:24 produced no Player.log or PEAK event.
  Steam was initially signed out in that attempt.
- Steam later recorded a successful logon response at 00:30:37. The 21:56:59
  launch request still produced no later PEAK process event or fresh Player.log,
  and the earlier investigation reports a 120-second handoff timeout.

The first signed-out state does not prove that authentication caused the later
handoff timeout. Neither failed Wine log contains DLL import-failure, unhandled
exception, device-removal or MSYNC failure markers. These logs therefore do not
establish a PEAK crash or a graphics regression. The existing prefix-side Unity
logs date from September 30 and cannot qualify the updated launch.

At the start of this investigation, no Windows Steam, PEAK, Wine or wineserver
process was alive. Ordinary native macOS Steam was running. A successful historical
logon response does not establish current authentication.

## Fresh controlled launch

After Agent B completed its timing runs, one real-Steam launch used this marked
copy and matching runtime under MSYNC. [The structured lifecycle](live-launch.json)
records Steam's initial launcher being replaced by a client after 34.6 seconds;
the original launcher returned 42. A webhelper appeared after 45 seconds. This
replacement is not evidence of PEAK crashing.

The actual `steam.exe -applaunch 3527290` handoff returned **0 after 6.7 seconds**.
It did not reproduce the previous 120-second handoff timeout. During the startup,
handoff and subsequent observation, no PEAK process, fresh Steam PEAK event or
fresh Unity Player.log appeared. No fresh successful Steam logon response was
recorded. Current authentication is therefore unconfirmed; the available logs
cannot distinguish an awaiting-sign-in state from another Steam launch block.

The visible test-copy Steam client is left running for sign-in or inspection.
Credentials must stay in Steam. Next qualification requires confirming that this
client is signed in, launching PEAK from its library, and obtaining fresh process
and Unity logs. DLL placement currently passes; neither PEAK graphics execution
nor a PEAK early crash has been demonstrated on the updated matched runtime.
Raw Steam/authentication logs remain outside the repository; published evidence
contains hashes, event classes and timestamps only.
