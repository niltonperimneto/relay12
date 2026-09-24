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
`atlbase.h` boundary. See `docs/PORT-QUALITY-ROADMAP.md` for the rule that each
negative milestone becomes a positive compile gate when its blocker is removed.

## 2. Memory & Boundary Security: Sanitizers & Padding Traps
Manual C/C++ memory management frequently introduces vulnerabilities, and failing to account for implicit compiler padding leads to "dirty memory" leaking over the ABI boundary.
* **Dirty Memory Initialization:** All test models must allocate memory using a `malloc_dirty()` helper that primes heap space with `0xCC` or `0xAA` values. If a struct initialization drops fields, the padding trap will catch the uninitialized bytes.
* **UBSAN & ASAN Porting:** While `mingw-w64` PE binaries resist standard sanitizers, the internal algorithms (e.g., handle tables, state trackers) must be decoupled from Windows APIs. These internal components can be compiled as a standard ELF binary on Linux/macOS strictly for offline CI testing with AddressSanitizer (`-fsanitize=address`) and UndefinedBehaviorSanitizer (`-fsanitize=undefined`).

## 3. Edge-Case Coverage: Fuzzing the DDI Boundary
It is common during development to focus on the "happy path" and forget to validate null pointers, zero-sized resources, or maliciously malformed command lists.
* **Structured Fuzzing (libFuzzer/AFL++):** A fuzzer target in `tests/fuzz_ddi.cpp` constructs malformed `D3D10DDIARG_CREATEDEVICE`, invalid shaders, and corrupted command lists, feeding them into the DDI entry points to catch unhandled crashes.
* **State Machine Fuzzing:** We fuzz the lifecycle of driver handles (e.g., calling `pfnDestroyCommandList` twice, or calling `pfnCommandListExecute` on an abandoned list) to ensure robust state-tracking defenses are in place.

## 4. Concurrency Guardrails: Strict Synchronization
Complex driver implementations might use an unprotected increment but forget the memory barrier, or use a heavyweight mutex where a spinlock is mandated by the DDI IRQL rules.
* **Isolated Thread Stress (The "Storm"):** The `ddi_thread_stress.c` module must be expanded whenever new shared device state is introduced. It oversubscribes CPU threads to force context switches in the middle of standard functions.
* **AST Concurrency Lints:** `scripts/check_shared_state.py` and `scripts/check_secure_code.py` outright ban standard unprotected `++` or `--` operators on any struct member labeled `refcount` or `volatile`. Every state mutation must be enforced through compiler intrinsics (`Interlocked*`).

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

1. A deterministic triangle on a macOS self-hosted runner using the real
   D3DMetal device and queue, with pixel readback or a screenshot hash.
2. A timed PEAK smoke run whose log must select `Direct3D 12.0`, must not fall
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

## PEAK smoke run

`scripts/check_peak_smoke_log.py` turns the second runtime checkpoint above into
a verdict from Unity's `Player.log` and the Wine output of the same run. Every
criterion needs positive evidence; tests in `tests/test_peak_smoke_log.py` use
real PEAK logs for the D3D11 crash, the silent On12 fallback and Metal HUD
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
