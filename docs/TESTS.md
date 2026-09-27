# Testing Strategy: Hardening the Clean-Room DDI

## The Core Challenge
Writing a user-mode graphics driver (WDDM/DDI) requires absolute determinism. ABI boundaries, memory alignment, and lock-free concurrency models are unforgiving. Driver code must perfectly bridge the gap between Windows applications and the Apple Silicon translation layer.

Whether authored by human engineers or automated AI agents, C/C++ driver implementations are prone to subtle layout mismatches, un-barriered memory reads, or edge-case null dereferences. The test suite for `relay12` must act as an **absolute deterministic net**, assuming all new code is flawed until proven mathematically and structurally correct.

This document outlines the testing architecture and hardening strategies required to secure the DDI codebase.

---

## 1. Structural Determinism: Multi-Source Golden Layouts
Developers can easily misread documentation, miscount padding bytes, or mix up legacy fields when recalling Windows internals.
* **Dual-Source Validation:** Continue enforcing the rule that every struct must be independently verified against both MSDN and the WDK documentation mirrors.
* **AST-Driven CI Gates:** We use `scripts/gen_ddi_layout.py` combined with Python-based static analysis to parse the generated C/C++ AST. Ensure that every struct defined strictly maps to the JSON golden template. If a developer introduces an extra `Reserved` field, the AST diff will immediately block the PR.
* **Calling Convention Guards:** It is simple to mix up `__stdcall`, `__cdecl`, and `__fastcall`. The test suite wraps invoked function pointers in a macro (`CHECK_STACK`) that asserts the `%rsp` (stack pointer) is identical before and after the call, ensuring stack integrity is not compromised by mismatched conventions.

## 1a. Behavioural Determinism: The Frame Harness
Layout and per-slot callability are necessary and not sufficient. Every driver handle in this DDI is one wrapped pointer, so a frame that bound the vertex buffer where it meant to bind the render target would call the right slots, in the right order, with arguments of the right types — and only the identity of a pointer would be wrong. No layout assertion and no per-slot call can see that.
* **`tests/d3d11ddi_triangle.c`:** drives the promoted device function table through the MVP frame — create, bind, clear, draw, copy to staging, map, verify, unmap, tear down — with recording stubs, then asserts the recorded call sequence against a written-out expected order and checks that each binding received the handle the matching creation produced. The clear stub fills a backing image, draw replaces its centre texel, and readback requires the centre to carry the drawn red and a corner the cleared blue, matching the application-level test. Objects get distinct driver private blocks, allocated by the test the way the runtime allocates them, which is what makes the identity checks discriminating rather than vacuous. Compiled in C and C++ and run under Wine, like the layout harness.
* **What it does not claim:** recording stubs do not prove GPU rendering. `tests/d3d11on12frame.c` additionally drives the real core against a mock driver, including fault injection, mapping validation, retained views, and teardown with outstanding maps. `tests/d3d11on12frontend.c` loads the compiled Wine host to check bound-object lifetime, cycle-free device destruction, and safe rejection of unsupported operations. Real GPU acceptance uses `tests/e2e_d3d11_triangle.cpp`; see [FIRST-FRAME-VALIDATION.md](FIRST-FRAME-VALIDATION.md).

## 1b. Fail-Closed Driver Initialization Tests
Real driver interactions start with the adapter and device, requiring strict argument handling before the boundary to the translation layer is even entered.
* **`tests/d3d11on12openadapter.c` & `tests/d3d11on12coretest.c`:** These suites validate the DDI adapter entry point (`WineD3D11On12OpenAdapterV1`), explicitly checking that malformed calls (e.g., null out-structures, size mismatching version handshakes, or invalid interface combinations) are rejected and fail-closed *before* reaching the actual D3D11On12 driver module. This proves defensive design.
* **`tests/d3d11on12openadapter.c`, Texture2D section:** the owned Texture2D lifecycle. The mock driver is resource-kind aware and validates the *whole* mip array across every array slice rather than its first entry, because the core derives that array itself: a stub checking one level would pass on a core that got every later mip wrong. The suite covers a zero-`MipLevels` request resolving to the full chain, format and bind-flag forwarding, initial subresource data, initial data carrying a null pointer (refused before the DDI is touched), double destruction, five rejected-argument cases, and a texture left alive to prove device teardown destroys it and leaves the caller's handle inert.
* **Vertex/pixel shader creation:** the current mock leaves the immediate-device DDI creation slots null, matching the pinned driver. The adapter lifecycle suite verifies creation through the non-COM device sub-object, binding, and destruction.
* **Six-stage shader lifecycle, not currently built:** `tests/d3d11on12shaderlifecycle.c` and the mock's `ID3D11On12DDIDevice` vtable live on the `ddi-device-lifecycle` branch. That branch's PR (#7) was closed when PR #8 consolidated the tree, so it is preserved deliberately and must not be deleted as stale — it is the only copy of this work until the port lands. They pin *which* driver entry creates a shader -- the pinned driver leaves all six `pfnCreate*Shader` table slots null on the immediate device and publishes creation only through the sub-object -- and they assert per-stage private-block sizing, bytecode pass-by-address, cross-stage bind refusal, and destruction-through-the-driver at teardown. They return when the six-stage implementation is ported onto the ordinal-export mechanism; see `docs/DDI-REMAINING-ROADMAP.md`.
* **What these do not claim:** no shader compiles. The mock records the container it was shown and does not parse DXBC, so this proves ownership, routing and lifetime — not that any real bytecode would be accepted.

## 1c. Portability Debt as a Tested Input

`scripts/inventory_dtl_portability.py` inventories the pinned
D3D12TranslationLayer tree by category, occurrence count, and file. Its golden
record is `docs/dtl-portability-baseline.json`. Unit tests verify comment
filtering, locations, counts, and mutation detection. CI also performs a
MinGW expected-failure compile that must reach exactly the currently recorded
`atlbase.h` boundary. See `docs/D3D11ON12.md` §Policy for the rule that each
negative milestone becomes a positive compile gate when its blocker is removed.

## 2. Memory & Boundary Security: Sanitizers & Padding Traps
Manual C/C++ memory management frequently introduces vulnerabilities, and failing to account for implicit compiler padding leads to "dirty memory" leaking over the ABI boundary.
* **Dirty Memory Initialization:** All test models must allocate memory using a `malloc_dirty()` helper that primes heap space with `0xCC` or `0xAA` values. If a struct initialization drops fields, the padding trap will catch the uninitialized bytes.
* **UBSAN & ASAN Porting:** While `mingw-w64` PE binaries resist standard sanitizers, the internal algorithms (e.g., handle tables, state trackers) must be decoupled from Windows APIs. These internal components can be compiled as a standard ELF binary on Linux/macOS strictly for offline CI testing with AddressSanitizer (`-fsanitize=address`) and UndefinedBehaviorSanitizer (`-fsanitize=undefined`).

## 3. Edge-Case Coverage: Fuzzing the DDI Boundary
It is common during development to focus on the "happy path" and forget to validate null pointers, zero-sized resources, or maliciously malformed command lists.
* **Structured Fuzzing (libFuzzer/AFL++):** A fuzzer target in `tests/fuzz_ddi.cpp` constructs malformed `D3D10DDIARG_CREATEDEVICE`, invalid shaders, and corrupted command lists, feeding them into the DDI entry points to catch unhandled crashes.
* **State Machine Fuzzing:** We fuzz the lifecycle of driver handles (e.g., calling `pfnDestroyCommandList` twice, or calling `pfnCommandListExecute` on an abandoned list) to ensure robust state-tracking defenses are in place.

## 4. Concurrency & Thread-Safety Testing

Because `relay12` bridges the gap between Windows WDDM behavior and
translation-layer execution environments, synchronization bugs manifest as
intermittent silent memory corruption, tearing, or deadlocks that are nearly
impossible to trace in production. This section specifies what must be tested
and how.

### 4.1. Synchronization Object Conformance (Wait/Signal)
The core WDDM graphics dispatch model relies on synchronization callbacks (e.g., `pfnWaitForSynchronizationObjectCb`, `pfnSignalSynchronizationObjectCb`).
* **Deadlock Avoidance:** Tests must mock the core layer callbacks and intentionally introduce blocking logic to ensure the translation layer handles wait/signal operations asynchronously where appropriate without locking up the entire D3D device lock.
* **Hardware vs. CPU Queues:** WDDM 2.6+ introduces granular sync objects (`pfnSubmitWaitForSyncObjectsToHwQueueCb`). Tests must distinguish between CPU waits and GPU hardware-queue waits.
* **Deferred vs. Immediate Execution:** Ensure that submitting sync tokens on deferred contexts does not prematurely trigger signal calls on the immediate context.

### 4.2. Memory Barrier & Visibility Testing
Translation-layer targets may have different memory barrier physics than native x86_64 TSO (e.g., ARM memory model on Apple Silicon).
* **Flush Conformance:** Write tests that simulate CPU modification of constant buffers or staging resources. An execution thread must wait on the corresponding DDI synchronization object; if the read happens before the "simulated GPU" completes, the test must catch the tearing.
* **Enhanced Barriers Guard:** Verify that applications using enhanced barriers (`D3D12_FEATURE_OPTIONS12.EnhancedBarriersSupported`) do not bypass older sync mechanisms inside the translation layer.
* **Cache Eviction and Fencing:** Emulate `pfnAcquireResourceCb` and `pfnReleaseResourceCb` (and cross-adapter sync if applicable) to ensure GPU caches are being asked to flush exactly when the DDI contract specifies.

### 4.3. Deferred Contexts & Multi-threaded Command Lists
D3D11 allows applications to build command lists concurrently across multiple threads before submitting them.
* **Global State Pollution:** Tests should spawn multiple worker threads that invoke the DDI simultaneously to generate command lists. They must ensure that internal states inside `d3d11shim.dll` are not clobbered (using thread-local storage/state isolation heavily and appropriately).
* **Interleaved Submission Traps:** Intentionally submit incomplete or fragmented command lists from concurrent threads to verify that the layer rejects them or sequences them flawlessly without crashing due to a torn linked list of commands.

### 4.4. ThreadSanitizer (TSAN) — Not Available for This Target

This plan originally called for a `-fsanitize=thread` pass in CI. That cannot be
built:

```
clang: error: unsupported option '-fsanitize=thread' for target 'x86_64-w64-windows-gnu'
```

TSAN needs a runtime library, and compiler-rt ships it for Linux, macOS,
FreeBSD and NetBSD — not for Windows. GCC has no `libtsan` for its mingw
targets either. Both modules under test are PE binaries importing `kernel32`
and using Win32 synchronization primitives (`INIT_ONCE`, `Interlocked*`), so
there is no configuration in which a TSAN build of them exists.

The escape hatch — building the sources natively as ELF behind a fake
`windows.h` — was considered and rejected. Neither module is portable: the core
is written against D3D12 COM interfaces and the router against `LoadLibraryW`,
so what a native build could link is a shim, and TSAN would be instrumenting
the shim. A green run would say nothing about the shipped DLLs.

### 4.5. What Replaces TSAN

Two things, which between them cover what TSAN would have found here:

- **A storm, run under Wine.** `tests/ddi_thread_stress.c` is a MinGW binary
  linking the same `d3d11on12core.o` and `wine_d3d11_diag.o` the validation
  tests link, driving them from 12 real Win32 threads — oversubscribed against
  any CI runner, so the scheduler preempts inside the boundary rather than
  between calls. It asserts the result of every call on every thread, that
  every output parameter is cleared on every failure path, that every shared
  mock's reference count returns to one, and that the report-once guard is
  taken exactly once. The join is bounded, so a deadlock fails in a minute with
  a named failure instead of hanging the runner until its own timeout. The CI
  step captures the process's stderr and requires each of the seven diagnostic
  latches to have reported exactly once across every thread and iteration,
  which is the flood check the latches exist for.
- **A gate over every definition.** `scripts/check_shared_state.py` reads every
  namespace-scope definition in `relay12-d3d11/*.cpp` and requires each to be
  an `INIT_ONCE`, a `volatile LONG` moved only through `Interlocked*`, or
  annotated as published through a named `INIT_ONCE` — in which case every
  function touching it must execute that `INIT_ONCE` first. The storm reaches
  only the state it happens to call; this reaches all of it.

**Atomic verifications** are the second rule, and hold today: the `refcount`
members in `tests/d3d11on12mocks.h` use `InterlockedIncrement` and
`InterlockedDecrement`, and the seven diagnostic latches in
`d3d11on12core.cpp` move only through `wineD3D11DiagReportOnce`'s
`InterlockedCompareExchange`.

* **AST Concurrency Lints:** `scripts/check_shared_state.py` and `scripts/check_secure_code.py` outright ban standard unprotected `++` or `--` operators on any struct member labeled `refcount` or `volatile`. Every state mutation must be enforced through compiler intrinsics (`Interlocked*`).

### 4.6. What the Storm Deliberately Does Not Test

Not the whole `D3DWDDM2_6DDI_DEVICEFUNCS` router. 102 of its 178 slots still
hold `PFNWINE_D3D11DDI_UNDECLARED_CB`, so a test calling them would be racing stubs
it wrote itself, and would pass whatever the eventual implementation does. A
test that cannot fail for the right reason is worse than no test, because the
roadmap then reads as though the ground were covered. The storm targets the
code that holds shared mutable state today; promoted slots are added as they
land.

### 4.7. Implementation Phases

**Phase A: shared-state gate and Wine-run storm.** Done —
`scripts/check_shared_state.py` and `tests/ddi_thread_stress.c`, both wired into
`.github/workflows/pull-request.yml`, with every gate rule tested in both
directions in `tests/test_ci_gates.py`.

**Phase B: extend the storm as slots are promoted.** Each promoted slot family
that touches device state adds its cases to the storm in the same pull request
that promotes it.

**Phase C: Sync Token Logic Guards.** Blocked. Unit tests for
`pfnSubmitSignalSyncObjectsToHwQueueCb` need its signature, and it is one of the
63 kernel-callback slots that hold an offset and nothing else.

## 5. Security & Precision Lints
* **Integer Overflow Preventions:** Manual sizing requires strict bounds checking on `SIZE_T` inputs (e.g., `pfnCalcPrivateCommandListSize`). Enforce a CI static analysis rule that any parameter used in a memory allocation (e.g., `malloc(size * count)`) must pass through a SafeInt/checked arithmetic boundary to prevent integer overflow exploits.
* **Return Code Strictness:** A static analysis rule ensures that every `HRESULT` or `SIZE_T` returned by an internal API is checked for failure, and that appropriate cleanup (e.g., `goto cleanup`) is executed. No swallowed errors.

---
**Summary for Contributors (Human & Automated):**
When authoring code for this repository, your output will be subjected to deliberate heap corruption, stack-pointer monitoring, multi-threaded hammering, and AST layout extraction. Code defensively, zero-initialize all structs, explicitly type all calling conventions, and check all `HRESULT` return paths.

## 6. Integration-First Test Strategy

The portable suite is a structural and diagnostic gate, not proof that a game
renders through D3DMetal. Development therefore uses a deliberately uneven
split: roughly 20% focused hardening and 80% implementation toward the first
real frame.

Before extending a newly introduced lifetime boundary, add focused coverage
for partial creation, `pfnSetErrorCb`, double destruction, adapter teardown,
stale and cross-device handles, and concurrent create/bind/destroy activity.
The buffer, input-layout, and vertex/pixel shader boundaries now carry that
coverage in the native mock-driver suite, including DDI error injection and
device-teardown invalidation.
Do not delay the rendering path to build exhaustive mocks: mocks cannot prove
barrier, residency, command submission, shader, or presentation correctness.

The runtime checkpoints, in order, are:

1. A deterministic triangle on a self-hosted runner using the real
   D3D12 device and queue, with pixel readback or a screenshot hash.
2. A timed application smoke run (currently validated using the Unity 6
   game PEAK) whose log must select `Direct3D 12.0`, must not fall
   back to D3D11, and must show wrapped-resource activity and a presented
   frame without device removal, crash, or initialization timeout.
3. A soak run that records memory growth, synchronization stalls, and device
   removal over repeated frames.

The EWDK linked-driver build is also a required release gate. If runner or
toolchain availability causes that job to skip, the portable lane may guide
continued development but does not qualify a canary runtime for game testing.

The input-layout boundary now exercises these rules in
`d3d11on12openadapter.c`: null unbinding, stale and cross-device rejection,
`pfnSetErrorCb` allocation failure cleanup, adapter teardown invalidation, and
an eight-thread bind-versus-destroy stress pass. Destruction removes a layout
from the registry while holding the exclusive lock, which first drains every
in-flight shared-lock binding and prevents a new binding from observing the
driver object before `DestroyElementLayout` runs.

## Wrapped-resource ownership slice

`tests/d3d11on12wrapped.c` covers reference transfer, failed wrapping/open,
strict COM acquisition, foreign devices, stale handles, batch prevalidation,
invalid ownership transitions, retained RTVs, adapter teardown, transition
failure refusal, and eight-thread ownership/destruction stress. CI executes it
against the native mock driver.

`tests/e2e_d3d11_wrapped.cpp` uses the original D3D12 texture and D3D12 readback
to verify three acquire/clear/release cycles per RGBA/BGRA format. Run it with
`scripts/run-d3dmetal-frame.py --test wrapped`; see
[WRAPPED-RESOURCE-VALIDATION.md](WRAPPED-RESOURCE-VALIDATION.md). It does not
exercise DXGI surfaces, Direct2D, or presentation.

## Performance measurement

Performance claims need numbers, and the numbers come from three places. The
plan they serve is [`PERFORMANCE-RESEARCH-ROADMAP.md`](PERFORMANCE-RESEARCH-ROADMAP.md).

**DDI telemetry.** Set `RELAY12_TELEMETRY=1` in the application's environment.
The core then times the driver's draw and flush slots and, when the device is
destroyed, writes one line through the diagnostic sink:

```
d3d11on12core telemetry: draws=… draw_avg_ns=… slow_draws=… flushes=… flush_avg_ns=… submits=… opportunistic_submits=…
```

`slow_draws` counts draws over 1 ms, which is how a pipeline-state compile wait
looks from the host. `submits` counts every command-list submission the driver
reported through its post-submit callback; `opportunistic_submits` is the part
the application did not ask for with a flush. With the variable unset the
proxies are not installed at all. The same variable makes the D3D11On12
device, when destroyed, write DTL's pipeline counters to stderr and the
debugger:

```
d3d11on12 pipeline telemetry: non_blocking=… draws_skipped_for_pso=… pso_waits=… pso_wait_ns=…
```

A PSO wait or skip happens on DTL's worker thread, so the host's draw timing
cannot see it; these counters can. `tests/test_telemetry.py` compares each
proxy's signature with its slot's typedef in `ddi/wine_d3d11ddi.h`, since a
mismatch is otherwise found only by the cross compiler in CI.

**Dispatch benchmark.** `tests/d3d11on12overhead.c` runs in
`validate-d3d11on12` against the mock driver, once with telemetry off and once
on. `scripts/summarize_overhead.py` publishes both runs' ns-per-call figures to
the job summary and fails only on correctness: a report printed with telemetry
off, or report counts that disagree with what was dispatched. The timings are
never a threshold -- a shared runner under Wine is too noisy for one.

The benchmark also runs a multithreaded engine's per-frame pattern on a
deferred context: record, finish a command list, execute it, destroy it. It
checks that every list after the first comes from recycled memory. The
lifecycle itself -- refusals, recording errors, the recycle pool's bound,
idempotent destruction, device teardown -- is `tests/d3d11on12deferred.c`.

**Non-blocking PSOs.** `tests/e2e_d3d11_async_pso.cpp` checks
`D3D11ON12_COMPAT_NonBlockingPSOs` deterministically. It swaps the
`CreateGraphicsPipelineState` and `CreateComputePipelineState` slots of a real
`ID3D12Device` vtable for wrappers that wait on an event, so the test decides
when each pipeline is ready. With the switch on, a draw with a held pipeline
must leave the target at the clear colour without blocking the calling thread
(a watchdog turns a hang into a failure), and must render once the pipeline is
released. A dispatch with a held compute pipeline must still run. With the
switch off, the draw must render. It needs a real D3D12 under the full stack,
so CI only compiles it; it runs through Whisky on D3DMetal. The patch gate
(`check_d3d11on12_port.py --dtl-source`) checks that only `PreDraw` can skip.

**Frame rate.** `check_peak_smoke_log.py` adds `frame_rate` to `result.json`:
mean fps, 1% low and minimum, from the Metal HUD's frame counts and NSLog
timestamps. HUD windows are about a second long, so the 1% low is over
per-second rates, not frame times. Compare runs made the same way -- same scene,
same duration, through Whisky -- before and after a change.

## Application smoke run (PEAK validation)

To validate real-world application interoperability, `relay12` uses real-world
engines and games as end-to-end smoke tests. The primary reference workload is
PEAK (Unity 6 hybrid rendering).

`scripts/check_peak_smoke_log.py` turns the second runtime checkpoint above into
a verdict from Unity's `Player.log` and the Wine output of the same run. Every
criterion needs positive evidence; tests in `tests/test_peak_smoke_log.py` use
real application logs for the D3D11 crash, the silent On12 fallback and Metal HUD
output. Unity's D3D12 renderer presents through its own D3D12 swap chain, so a
presented frame is read from Metal's performance HUD (`MTL_HUD_LOG_ENABLED=1`),
whose frame count must rise across reports.

`scripts/run-peak-smoke.py` starts Steam in a marked test prefix, launches PEAK
with `-applaunch`, and returns the verdict. It refuses any prefix without a
`.relay12-peak-prefix` marker, so it cannot upgrade a player's bottle;
`tests/test_run_peak_smoke.py` covers that refusal without Wine.

`tests/peak_on12_probe.cpp` is a hardware check, not a CI one. It repeats
Unity's `D3D11On12CreateDevice` call, lists every interface the objects answer,
and asserts what patch 0025 promises on real D3DMetal: `IDXGIDevice` exists, its
`GetAdapter` and `GetParent` return the D3D12 device's adapter by LUID, and
private data set through `ID3D11Device` reads back through it. The CI half of
0025 is in `tests/d3d11on12frontend.c` against the mock D3D12 device, and
`check_wine_d3d11_backend.py` rejects a tree that loses it.

The probe also prints `perf:` lines, which are recorded and never judged: the
shader-cache and pipeline-library support that decide the persistent PSO cache,
and `ARCHITECTURE1` plus `CUSTOM` heap creation that decide UMA staging (items 1
and 3 of [`PERFORMANCE-RESEARCH-ROADMAP.md`](PERFORMANCE-RESEARCH-ROADMAP.md)).

### Synchronization-mode runs

`run-peak-smoke.py --sync none|esync|msync` selects exactly one mode;
`none` is the default. The runner sets all three Wine sync variables explicitly,
including `WINEFSYNC=0`, and pins `WINESERVER` and `WINELOADER` to the selected
runtime. It stops and waits for the marked test prefix's server before launch
and after the run. This ends any existing applications in that test prefix.
Never use the marker on a player's bottle.

Wine's MSYNC initialization rejects clients whose mode differs from the running
server. A mode change therefore requires restarting the whole prefix, including
Steam; changing only the game's environment is insufficient. `launch.json`
records the selected mode, loader, server, and prefix alongside the smoke logs.
Run each mode sequentially with a separate output directory. A command-line
startup probe passing is not evidence of PEAK rendering or GPU fence correctness;
each mode still needs the positive rendering evidence checked by the smoke judge.

### PSO component lifecycle

`tests/d3d11on12pso.c` opens the core against the mock driver and substitutes
strict, independently tagged state slots. It checks descriptor translation
(including DDI-only depth fields and the WDDM2 rasterizer tail), default bindings,
independent-blend replication, zero padding, missing callbacks, invalid inputs,
allocation-size overflow, driver-reported creation/binding failure, owner/kind
checks, copied and stale handles, retention while bound, command-list state reset,
and idempotent release before/after teardown. A flush counter must remain zero.
It runs in `validate-d3d11on12`; like other mock tests it proves dispatch and
lifetime, not rendered output or a performance gain.
