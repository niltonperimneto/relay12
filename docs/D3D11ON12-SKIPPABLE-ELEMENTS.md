# D3D11On12 to D3DMetal: Skippable DDI Elements

## Overview
The goal of `relay12` is to seamlessly bridge Windows D3D11 games through `D3D11On12`, translating them to D3D12, which is then handled by D3DMetal (via GPTK) on macOS. 

Because we are targeting the Apple Silicon unified memory environment via a translation layer—and *not* writing a native hardware driver for a Windows machine—many complex sections of the WDDM User-Mode DDI (UM DDI) can be safely skipped, stubbed, or intentionally flagged as `DXGI_ERROR_UNSUPPORTED`. 

Attempting to implement these features would waste engineering time on concepts that D3DMetal either does not support natively or that upper layers (like Wine's DXGI) already emulate effectively.

This document serves as the guide for which DDI endpoints can remain as `PFNWINE_D3D11DDI_UNDECLARED_CB` placeholders or receive trivial stub implementations.

---

## 1. The Kernel Display & Escape Paths
Apple Silicon natively handles its own window management and compositing (Quartz/Metal). Wine's DXGI layer already correctly emulates swap chains and intercepts presentation calls before they require low-level DDI involvement.
* **Scanout & DirectFlip (`pfnCheckDirectFlipSupport`, `pfnQueryScanoutCaps`, `pfnPrepareScanoutTransformation`)**: You do not need to hardware-composite to the screen layout. Stub these to report basic fallback capability or unsupported, forcing the runtime to avoid direct kernel scanouts and use standard presentation.
* **Escape Probe (`pfnEscapeCb`)**: Used by native drivers to ping the display kernel for device removal state. Since display-kernel paths are explicitly disabled in this port, we can answer this directly inside our host rather than attempting a translation to Wine's D3DKMT layer.

## 2. Deferred Contexts (Temporary Skip)
* **Status:** Postponed
* **Reasoning:** While structural modeling for `CreateDeferredContext` and `CreateCommandList` has been implemented, translating the actual recording and replay semantics is highly complex. The D3D11 Runtime is designed to smoothly fallback to software emulation for deferred contexts if the driver explicitly states it does not support them natively.
* **Action:** Retain the structs but return `DXGI_ERROR_UNSUPPORTED` in the implementations until the core immediate-context rendering is completely stable.

## 3. Hardware Protection / DRM
Technologies like Microsoft PlayReady and secure video paths require hardware decryption keys and deep Windows Kernel integration.
* **Endpoints:** `pfnSetHardwareProtection`, `pfnSetHardwareProtectionState`
* **Action:** D3DMetal and Wine do not natively proxy Windows hardware DRM keys. These can be skipped/stubbed permanently.

## 4. Tiled & Cross-Adapter Resources
* **Tiled Resources (Sparse Binding):** 
  * **Endpoints:** `pfnUpdateTileMappings`, `pfnCopyTileMappings`, `pfnCopyTiles`, `pfnResizeTilePool`.
  * **Reasoning:** While D3D12 and Metal do support sparse textures, very few D3D11 games depend on them to boot, and safely mapping them across the 11On12 bridge introduces extreme complexity. They are safe to stub for the MVP.
* **Cross-Adapter Shared Resources:**
  * **Reasoning:** SLI/Crossfire (multi-GPU) concepts are absent in Apple Silicon's unified memory environment. All driver logic can assume a single-node queue sharing model.

## 5. Telemetry & Shader Cache
* **Debug Binaries (`pfnAssignDebugBinary`)**: Exists for tooling. A safe no-op.
* **Shader Cache Sessions (`pfnCreateShaderCacheSession`, `pfnDestroyShaderCacheSession`, `pfnSetShaderCacheSession`)**: 
  * **Reasoning:** Native Windows drivers use this to tie into the OS-level shader cache. We can bypass this endpoint completely. Wine and GPTK will manage their own native Metal Shader caches optimally below the D3D12 layer.

---

## The "Triangle on Screen" Strategy
To achieve the MVP (Minimum Viable Product) of rendering a frame via `relay12 -> D3D11On12 -> D3DMetal`, the implementation effort must immediately pivot away from the elements listed above.

The next authoring phase must strictly promote and implement only:
1. **Resource Management:** `CreateResource`, `OpenResource` — done, see `docs/DDI-REMAINING-ROADMAP.md` §3.1.
2. **Basic Views:** `CreateRenderTargetView`, `CreateShaderResourceView` — done, see `docs/DDI-REMAINING-ROADMAP.md` §3.1. Depth-stencil and unordered-access views are deliberately not part of this pair and stay unpromoted.
3. **Core Shaders:** `CreateVertexShader`, `CreatePixelShader` — done, see `docs/DDI-REMAINING-ROADMAP.md` §3.1. Geometry, hull, domain, and compute shader creation are deliberately not part of this pair and stay unpromoted.
4. **Base Pipeline State:** Blend, Depth-Stencil, Rasterizer, and Sampler bounds — done, see `docs/DDI-REMAINING-ROADMAP.md` §3.1.

Everything else must remain locked inside `PFNWINE_D3D11DDI_UNDECLARED_CB` placeholders, allowing the compiler to statically enforce their absence while the core rendering loop is brought online.

## After creation: the frame

All four items above are now authored, and the group that consumes them has followed — element layout, input-assembly binding, shader binding, render-target binding, viewports, the three state binds, the clear, and the draw. `tests/d3d11ddi_triangle.c` drives them in frame order and checks the handles flow from each creation to the binding that consumes it, so the "Triangle on Screen" path is expressible against the declarations end to end.

Two things stand between that and a triangle:

1. **Readback.** `pfnResourceMap`, `pfnResourceUnmap` and `pfnResourceCopy` are still placeholders. Promoting them needs `D3D10_DDI_MAP` and a fully modelled `D3D10DDI_MAPPED_SUBRESOURCE` — the first structure in this effort whose members a test must actually read, so it cannot stay an incomplete type.
2. **A host.** Nothing stands behind the function table: `relay12-d3d11/d3d11on12core.cpp` still reports the device as validated but not translated. Declarations and harnesses are not an implementation, and no test in this repository currently claims otherwise.
