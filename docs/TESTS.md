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
* **What it does not claim:** nothing renders. There is no host behind the table. The application-level counterpart that would prove pixels is `tests/e2e_d3d11_triangle.cpp`, built but not run by the manual `integration-test.yml` job; when a host exists the two must agree.

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
