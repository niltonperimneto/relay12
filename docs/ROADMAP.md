# Relay12 Project Roadmap & Architecture Scope

This document serves as the single authoritative source of truth for the **scope**, **completed milestones**, **DDI signature promotion gating status**, **performance research areas**, **active Phase 2 priorities (TODOs)**, and **architectural invariants** of Relay12.

---

## 1. Project Scope & Architecture

### What Relay12 Is
Relay12 is a high-performance Direct3D 11 User-Mode Driver (UMD) DDI router for Wine on macOS / Apple Silicon. It implements the native Windows D3D11 DDI entrypoints (`OpenAdapter10_2`, `CreateDevice`, etc.) and routes them to Microsoft's open-source [D3D11On12](https://github.com/microsoft/D3D11On12) and [D3D12TranslationLayer](https://github.com/microsoft/D3D12TranslationLayer), mapping D3D11 calls directly to D3D12 and Wine's D3DMetal backend.

```
+-------------------------------------------------------+
|                 Windows Game / Title                  |
|               (e.g., Unity 6, PEAK)                   |
+-------------------------------------------------------+
                           |
                           v
+-------------------------------------------------------+
|                     d3d11.dll                         |
+-------------------------------------------------------+
                           |  (D3D11 User-Mode Driver DDI)
                           v
+-------------------------------------------------------+
|             relay12-d3d11 (Wine UMD)                  |
|    - Clean-Room wine_d3d11ddi.h (Natural 8-byte /Zp8) |
|    - PE Boundary & strictResult() Validation          |
|    - UMA Staging & Buffer Pool Management             |
+-------------------------------------------------------+
                           |
                           v
+-------------------------------------------------------+
|         Microsoft D3D11On12 & DTL (Patched)           |
|         - patches/d3d11on12/ & patches/dtl/           |
+-------------------------------------------------------+
                           |  (D3D12 API)
                           v
+-------------------------------------------------------+
|               Wine D3D12 / D3DMetal                   |
+-------------------------------------------------------+
                           |  (Metal API)
                           v
+-------------------------------------------------------+
|                 Apple Silicon GPU                     |
+-------------------------------------------------------+
```

### Scope Boundaries
| In-Scope | Out-of-Scope |
| :--- | :--- |
| Direct3D 11 User-Mode Driver (UMD) DDI implementation. | Rewriting D3D12TranslationLayer or D3D11On12 core logic. |
| Clean-room DDI headers matching MSVC 64-bit natural alignment (`/Zp8`). | Direct Metal backend authoring (delegated to Wine D3DMetal). |
| Specialized UMA memory pooling for unified memory architectures. | Kernel-mode driver (KMD) shims or macOS kernel extensions. |
| Strict PE boundary and interface acquisition verification. | Modifying `third_party/` git trees directly in-place. |
| Targeted patches maintained under `patches/d3d11on12/` and `patches/dtl/`. | Direct D3D12 engine replacements or proprietary DXVK forks. |

---

## 2. Completed Milestones

### Phase 0: Clean-Room DDI & Build Architecture (Completed)
- **Natural 8-Byte Alignment**: Formulated clean-room `wine_d3d11ddi.h` strictly enforcing MSVC 64-bit alignment (`/Zp8`) with no `#pragma pack`. Verified with compile-time assertions and `tests/d3d11ddipadding.c`.
- **PE Boundary & Strict Results**: Standardized all COM interface acquisitions and `QueryInterface` calls through `strictResult()` to ensure Wine compatibility and prevent silent interface leaks.
- **Automated CI Gates**: Deployed `tests/test_ci_gates.py`, `scripts/check_ddi_header.py`, `scripts/check_shared_state.py`, and `scripts/check_interface_acquisition.py`.
- **Subproject Patches (0001–0012)**: Upstreamed isolated translation layer adaptations under `patches/d3d11on12/` and `patches/dtl/`.

### Phase 1: Resource Interop, UMA Telemetry & Game Startup (Completed)
- **Wrapped D3D12 Resources (Patches 0013–0026)**: Implemented `CreateWrappedResource`, command queue fence signaling, and staging readback synchronization for hybrid swapchain presentation.
- **First Frame Verification**: Confirmed first-frame clear and draw execution via `D3D11On12` on Apple Silicon. (Archived in [docs/validation/2026-09-23-first-frame](validation/2026-09-23-first-frame/README.md)).
- **UMA Memory Pools & Telemetry**: Designed three memory allocation profiles (`legacy`, `balanced`, `aggressive`) in `compat/relay_memory_pool.hpp` (see [docs/UMA-MEMORY.md](UMA-MEMORY.md)).
- **Staging Burst Retention & Idle Hysteresis**: Resolved upload pool churn (3.2–3.3× median slowdown during bursts) by implementing configurable burst limits and idle grace periods (`D3D11ON12_COMPAT_UploadBurstCacheMiB=128`, `D3D11ON12_COMPAT_UploadBurstGraceMs=2000`). Validated on hardware in [docs/validation/2026-10-02-uma-burst-policy](validation/2026-10-02-uma-burst-policy/README.md).
- **PEAK Steam-Ready Live Startup under MSYNC**: Authenticated Steam client session handoff, initialized Steamworks API, selected Direct3D 12 feature level 12.2, and rendered the game's main menu in a visible 1408×757 macOS window under MSYNC without `steam_appid.txt` bypass. Verified with stage markers and screenshots in [docs/validation/2026-10-02-peak-steam-ready](validation/2026-10-02-peak-steam-ready/README.md).
- **Phase 1 Empirical Telemetry & Root-Cause Analysis**:
  - *False UMA Reporting*: Wine's D3DMetal backend reports false UMA flags; forced host-visible uploads produce valid pixels, but internal zero-copy requires coherent staging management ([docs/validation/2026-10-02-forced-uma](validation/2026-10-02-forced-uma/README.md)).
  - *P95 Tail Latency*: Correlated P95 jitter with synchronous GPU readback waits rather than CPU staging allocation overhead ([docs/validation/2026-10-02-uma-variance-dispatch](validation/2026-10-02-uma-variance-dispatch/README.md)).

---

## 3. DDI Signature Promotion Gating Status

`wine_d3d11ddi.h` defines the complete `D3DWDDM2_6DDI_DEVICEFUNCS` table (178 slots). 59 of 138 distinct callback types are fully promoted covering 76 of 178 slots. Slots are promoted as complete structure groups become validated and needed by runtime paths:

| Family | Promoted Slots | Status | Unblocking Types & Criteria |
| :--- | :--- | :--- | :--- |
| **Device Lifecycle** | `pfnCalcPrivateDeviceSize`, `pfnDestroyDevice` | Done | `D3D10DDIARG_CALCPRIVATEDEVICESIZE`. Completes entry/exit for device state. |
| **Command Lists** | `pfnAbandonCommandList`, `pfnCommandListExecute`, `pfnDestroyCommandList`, `pfnRecycleCommandList`, `pfnRecycleDestroyCommandList`, `pfnCalcPrivateCommandListSize`, `pfnCreateCommandList`, `pfnRecycleCreateCommandList` | Done | `D3D11DDIARG_CREATECOMMANDLIST`, `D3D11DDI_HRTCOMMANDLIST`. Unblocks command recording and execution. |
| **Deferred Contexts** | `pfnCheckDeferredContextHandleSizes`, `pfnCalcDeferredContextHandleSize`, `pfnCalcPrivateDeferredContextSize`, `pfnCreateDeferredContext`, `pfnRecycleCreateDeferredContext` | Done | `D3D11DDI_HANDLETYPE`, `D3D11DDI_HANDLESIZE`, `D3D11DDIARG_CALCPRIVATEDEFERREDCONTEXTSIZE`, `D3D11DDIARG_CREATEDEFERREDCONTEXT`. |
| **Resource Lifecycle** | `pfnCalcPrivateResourceSize`, `pfnCalcPrivateOpenedResourceSize`, `pfnCreateResource`, `pfnOpenResource`, `pfnDestroyResource` | Done | `D3D10DDIARG_CREATERESOURCE`, `D3D11DDIARG_CREATERESOURCE`, `D3D10DDIARG_OPENRESOURCE`. |
| **Views (SRV / RTV)** | `pfnCalcPrivateShaderResourceViewSize`, `pfnCreateShaderResourceView`, `pfnDestroyShaderResourceView`, `pfnCalcPrivateRenderTargetViewSize`, `pfnCreateRenderTargetView`, `pfnDestroyRenderTargetView` | Done | `D3D10DDIARG_CREATERENDERTARGETVIEW`, `D3DWDDM2_0DDIARG_CREATERENDERTARGETVIEW`. DSV and UAV slots remain unpromoted until required. |
| **Shaders (VS / PS)** | `pfnCalcPrivateShaderSize`, `pfnCreateVertexShader`, `pfnCreatePixelShader`, `pfnDestroyShader` | Done | Shared destroy callback. GS, HS, DS, and CS creation remain behind placeholders until stream-output/tessellation layouts are authored. |
| **Pipeline States** | `pfnCreateBlendState`, `pfnCreateDepthStencilState`, `pfnCreateRasterizerState`, `pfnCreateSampler` (+ CalcSize & Destroy) | Done | 12 slots unblocked by state descriptor structs. |
| **Layout, Binding & Draw** | `pfnCreateElementLayout`, `pfnIaSetInputLayout`, `pfnIaSetVertexBuffers`, `pfnIaSetIndexBuffer`, `pfnIaSetTopology`, `pfnSetRenderTargets`, `pfnSetViewports`, `pfnSetBlendState`, `pfnSetDepthStencilState`, `pfnSetRasterizerState`, `pfnClearRenderTargetView`, `pfnDraw`, `pfnDrawIndexed`, `pfnDrawInstanced`, `pfnDrawIndexedInstanced` | Done | Core draw dispatch and OM binding pipeline. |

---

## 4. Performance & Latency Research Scope

Relay12 targets CPU overhead in the D3D11-on-12 translation path across 5 core architectural areas:

1. **Asynchronous & Persistent PSO Compilation**:
   - DTL compiles pipeline states asynchronously on background worker threads.
   - Non-blocking PSOs (`D3D11ON12_COMPAT_NonBlockingPSOs=1`) prevent pipeline compilation bubbles on draw calls.
2. **Multi-Threaded Command Submission**:
   - DTL's batched context records commands and hands them off to translation worker threads via lock-free rings.
3. **Apple Silicon Unified Memory (UMA)**:
   - Configurable staging caches and burst retention (`compat/relay_memory_pool.hpp`).
   - Coherent staging minimizes memory marshalling across CPU and GPU on Apple Silicon.
4. **Command Flush Heuristics**:
   - Opportunistic submits vs explicit `Flush()` boundaries to avoid GPU command starvation while avoiding micro-batch pipeline latency.
5. **Redundant Resource Barriers**:
   - Eliminating unnecessary barrier transitions at the translation boundary while maintaining D3DMetal state correctness.

**Synthetic Benchmark Harness**:
[`tests/e2e_d3d11_overhead.cpp`](../tests/e2e_d3d11_overhead.cpp) evaluates real D3D12/D3DMetal hardware across three scenarios (`draws`, `upload`, `pso`), rendering real pixels and verifying readback.

---

## 5. Active Phase 2 Priorities & Project TODOs

The following tasks represent the active, high-priority work items for Relay12:

### [P0] Device & Hybrid Swapchain Presentation Integration
- **Context**: While first-frame rendering and offscreen readbacks pass, interactive gameplay requires continuous presentation connecting `relay12-d3d11` swapchain hooks to `D3D11On12` wrapped backbuffers and Wine DXGI.
- **TODOs**:
  1. Finalize the hybrid presentation path connecting `relay12-d3d11` swapchain hooks to `D3D11On12` wrapped backbuffers.
  2. Ensure command queue fence signaling reliably synchronizes present calls across frame boundaries without tearing or deadlock.
- **Files**: `relay12-d3d11/src/`, `patches/d3d11on12/`.

### [P1] Automatic UMA Profile Qualification
- **Context**: The `auto` memory profile currently retains legacy behavior with `qualification-pending`. Automatic promotion to `balanced` on $\le 8$ GiB Apple Silicon Macs requires passing qualification checks.
- **TODOs**:
  1. Verify hardware qualification criteria for automatic `balanced` memory profile promotion.
  2. Enable auto-selection based on `GlobalMemoryStatusEx` and `sysctl hw.memsize` without requiring explicit environment variables.
- **Files**: `compat/relay_memory_pool.hpp`, `docs/UMA-MEMORY.md`.

### [P2] Progressive DDI Signature Promotion
- **Context**: As additional titles and features are tested against Relay12, remaining unpromoted slots in `D3DWDDM2_6DDI_DEVICEFUNCS` need progressive typing.
- **TODOs**:
  1. Monitor unhandled DDI dispatch calls during title execution.
  2. Implement missing DDI entrypoints progressively (e.g., UAV/DSV views, compute shaders, query mechanisms), validating layout and signature against clean-room invariants.
- **Files**: `relay12-d3d11/ddi/wine_d3d11ddi.h`, `relay12-d3d11/src/`.

### [P2] Advanced Shader Translation & Stream-Output
- **Context**: Extended shader stages (Geometry, Hull, Domain, Compute) and stream-output buffer emulation.
- **TODOs**:
  1. Author clean-room argument structs for stream-output and hull/domain shader stages.
  2. Emulate stream-output using D3D12 indirect buffers where supported.
- **Files**: `relay12-d3d11/ddi/wine_d3d11ddi.h`, `patches/dtl/`.

---

## 6. Architectural Invariants & CI Gates

All code changes submitted to Relay12 must adhere to the following invariants:

1. **Natural 8-Byte Alignment (`/Zp8`)**:
   - `wine_d3d11ddi.h` and all DDI headers must strictly maintain natural MSVC 64-bit alignment.
   - `#pragma pack` is strictly prohibited.
   - Enforced by: `python3 scripts/check_ddi_header.py` and `tests/d3d11ddipadding.c`.

2. **Clean-Room Third-Party Isolation**:
   - Submodules under `third_party/` (`D3D11On12`, `D3D12TranslationLayer`, `DirectX-Headers`) must **never** be edited in-place.
   - All adaptations must reside as reproducible patch files in `patches/d3d11on12/` or `patches/dtl/`.

3. **Strict Interface Acquisition**:
   - All COM interface queries must go through `strictResult()` to ensure deterministic error checking.
   - Enforced by: `python3 scripts/check_interface_acquisition.py`.

4. **CI Validation Suite**:
   ```bash
   # Run unit & gate test suites
   python3 -m unittest discover tests
   python3 tests/test_ci_gates.py

   # Run architectural rule checkers
   python3 scripts/check_ddi_header.py
   python3 scripts/check_shared_state.py
   python3 scripts/check_interface_acquisition.py
   ```
