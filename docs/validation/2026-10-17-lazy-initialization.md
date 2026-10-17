# Deferred D3D11On12 initialization: local validation

Test date: 2026-10-17. Relay12 base: `a269405`; branch: `feat/lazy-dtl-init`.
The implementation is patch 0027 plus the allocation-free core preflight export.
It was developed in a separate worktree to preserve the existing checkout's
uncommitted changes. No hub pin or installed runtime was changed.

## Environment and artifacts

- macOS 27.2, build `26B5112a`, Apple A18 Pro; x86_64 Wine through Rosetta.
- Pinned llvm-mingw `20240619` UCRT macOS toolchain; C/C++ compiled with
  `-Wall -Wextra -Werror`, C++ with exceptions and RTTI disabled.
- Wine host source: `7dbc5b5322a6ef3fb04bdc643c64b188fd641149`, with the
  complete patches 0001–0027 applied. Both D3D11 and On12 host targets built.
- Runtime fixture: an APFS clone of installed runtime 4.8.22, WineCX
  `feda99e3831bc4cdf37a8d57a01d319b7acd5920`, with imported D3DMetal and its
  existing real translation driver. Only the disposable clone received the
  rebuilt host and test core/router. The prefix was newly created for these tests.
- New core DLL SHA-256:
  `63919de3c64631927cb719e3fcf7e8124d28b725da12665a85c642b5a8590031`.
- New On12 host DLL SHA-256:
  `1e8d413c7090ad72e3e1d5769edd70f9a7d338e2f3ea95b77ea527acba83b12d`.
- Baseline host from runtime 4.8.22 SHA-256:
  `90de9ed93a36bc410578e985f01866f09ed852e61087d056f86b652dec80ed7c`.

## Observed results

- All 196 Python tests passed. The DDI layout, header, interface-acquisition,
  shared-state, adapter-argument and security gates passed. Actionlint, the
  patched Wine backend gate and the core's PE ordinal/import audit passed.
- Patch 0027 passes forward checking against the pinned tree with patches
  0001–0026, and reverse checking against the compiled tree.
- The new lazy frontend test fails against the baseline host because ordinary
  creation already loads the driver. It passes with the rebuilt host.
- The compiled test verifies unused creation, QI/COM identity and destruction
  without loading DTL; eight concurrent first resource creations open one
  adapter/device; references balance; first-use failures remain terminal and
  observable through HRESULT-returning and void methods. Rejection after adapter
  creation closes the partial device/adapter exactly once. A separate process
  with the driver disabled passes the missing-driver case.
- The existing compiled public frontend resource-lifetime and unsupported-method
  suite passes with the new host and recording driver.
- A real-device probe creates the caller's D3D12 device/queue, creates On12 and
  verifies COM identity and the correct DXGI adapter LUID while DTL stays unloaded.
  Releasing the unused frontend also leaves DTL unloaded. In a separate run,
  first Flush loads the real DTL driver and GetDeviceRemovedReason returns S_OK.
- `tests/e2e_d3d11_wrapped.cpp` passes with the new host: RGBA and BGRA each
  complete three acquire/clear/release cycles, with all 4096 caller-readback
  pixels red and the caller's fence completing. Teardown passes. The same probe
  passes with the baseline host.

The real backend emits Apple Neural Engine compilation, primitive-topology and
protected-resource diagnostics in the applicable probes. They are present in the
baseline runs too; the tested API/readback results above remain explicit. Wine's
unchanged resource parser also emits its existing Bison grammar-conflict warnings
during native tool compilation.

- In a 6-trial counterbalanced PEAK (`Direct3D 12 [level 12.2]`, `1920x1080`
  windowed, `MTL_HUD_LOG_ENABLED=1`) benchmark with authenticated headless Steam
  reaching the rendered 3D main menu, `d3d11on12.dll` stays unloaded throughout
  the session (`loaded_d3d11on12_dtl: false`), GPU memory drops from
  `941.20–941.27 MB` (eager DTL) to `850.33–850.36 MB` (`-90.9 MB`, matching the
  no-op stub's `850.34–850.39 MB`), and process memory drops from
  `2344.6–2354.5 MB` to `2238.6–2321.4 MB`.

## Limits

These results establish the tested lazy lifecycle, wrapped-resource rendering and
PEAK 3D main-menu memory/frame-rate telemetry, not general game/video
compatibility across titles that actively submit D3D11 video frames through On12.
Actual in-game video playback, macOS 26 hardware coverage and complete hub
assembly/package validation remain separate checks. The host and core must be
deployed together; see [the error-timing and compatibility contract](../LAZY-INITIALIZATION.md).
