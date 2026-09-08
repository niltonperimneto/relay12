# DDI Concurrency & Memory Barrier Testing Strategy

## Overview
This document outlines the testing plan to harden the clean-room D3D11 DDI layer (`relay12-d3d11`) against race conditions, thread safety violations, and missing CPU/GPU memory barriers. 

Because `relay12` bridges the gap between Windows WDDM behavior, Wine's translation layers, and Apple Silicon's execution environment (via GPTK), synchronization bugs here will manifest as intermittent silent memory corruption, tearing, or deadlocks that are nearly impossible to trace in production. This plan specifies what must be tested and how those tests should be integrated.

## 1. Synchronization Object Conformance (Wait/Signal)
The core WDDM graphics dispatch model heavily relies on synchronization callbacks (e.g., `pfnWaitForSynchronizationObjectCb`, `pfnSignalSynchronizationObjectCb`).
**Tests must account for:**
- **Deadlock Avoidance:** Tests should mock the core layer callbacks and intentionally introduce blocking logic to ensure the translation layer handles wait/signal operations asynchronously where appropriate without locking up the entire D3D device lock.
- **Hardware vs. CPU Queues:** WDDM 2.6+ introduces granular sync objects (`pfnSubmitWaitForSyncObjectsToHwQueueCb`). Tests must distinguish between CPU waits and GPU hardware-queue waits. 
- **Deferred vs. Immediate Execution:** Ensure that submitting sync tokens on deferred contexts does not prematurely trigger signal calls on the immediate context.

## 2. Memory Barrier & Visibility Testing
Apple Silicon has deeply different memory barrier physics (ARM memory model vs. x86_64 TSO) and integrates differently with unified memory than a traditional discrete GPU.
**Tests must account for:**
- **Flush Conformance:** Write tests that simulate CPU modification of constant buffers or staging resources. An execution thread must wait on the corresponding DDI synchronization object; if the read happens before the "simulated GPU" completes, the test must catch the tearing.
- **Enhanced Barriers Guard:** Verify that applications using enhanced barriers (`D3D12_FEATURE_OPTIONS12.EnhancedBarriersSupported`) do not bypass older sync mechanisms inside the translation layer.
- **Cache Eviction and Fencing:** Emulate `pfnAcquireResourceCb` and `pfnReleaseResourceCb` (and cross-adapter sync if applicable) to ensure GPU caches are being asked to flush exactly when the DDI contract specifies. 

## 3. Deferred Contexts & Multi-threaded Command Lists
D3D11 allows applications to build command lists concurrently across multiple threads before submitting them.
**Tests must account for:**
- **Global State Pollution:** Tests should spawn multiple worker threads (using Win32 `CreateThread` or `pthreads`) that invoke the DDI simultaneously to generate command lists. They must ensure that internal states inside `d3d11shim.dll` are not clobbered (using thread-local storage/state isolation heavily and appropriately).
- **Interleaved Submission Traps:** Intentionally submit incomplete or fragmented command lists from concurrent threads to verify that the layer rejects them or sequences them flawlessly without crashing due to a torn linked list of commands.

## 4. Automation & Tooling

### ThreadSanitizer is not available for this target

This plan originally called for a `-fsanitize=thread` pass in CI. That cannot be
built, and the reason is recorded here so it is not proposed again:

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

### What replaces it

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

### What the storm deliberately does not test

Not the `D3DWDDM2_6DDI_DEVICEFUNCS` router. 173 of its 178 slots still hold
`PFNWINE_D3D11DDI_UNDECLARED_CB`, so a test calling them would be racing stubs
it wrote itself, and would pass whatever the eventual implementation does. A
test that cannot fail for the right reason is worse than no test, because the
roadmap then reads as though the ground were covered. The storm targets the
code that holds shared mutable state today; §1–§3 above become writable as the
sync-object and resource slots are promoted.

## Implementation Phases
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
