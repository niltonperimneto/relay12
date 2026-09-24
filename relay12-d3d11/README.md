# relay12-d3d11: Router, Core Boundary & Clean-Room DDI Host

This directory contains the primary implementation components of `relay12`:

- **PE Router (`d3d11shim.cpp`):** Drop-in router for `d3d11.dll`. Forwards standard D3D11 device creation to native `d3d11mt.dll` (e.g. Apple D3DMetal or WineD3D) and routes `D3D11On12CreateDevice` to the core boundary.
- **Core Boundary (`d3d11on12core.cpp`):** Validates caller-supplied `ID3D12Device` and direct `ID3D12CommandQueue` instances, enforces two-tier COM identity, funnels acquisitions through `strictResult()`, and owns the DDI adapter/device lifecycle.
- **Diagnostic Logging (`wine_d3d11_diag.h`):** Deduplicated logging layering `__wine_dbg_output`, `OutputDebugStringA`, and standard error.
- **Clean-Room WDDM DDI Host (`ddi/`):** Clean-room authored DDI declarations (`wine_d3d11ddi.h`) with strict `/Zp8` natural alignment and zero proprietary WDK dependencies.

For detailed architecture, state models, and design invariants, see [`docs/D3D11ON12.md`](../docs/D3D11ON12.md) and [`AGENTS.md`](../AGENTS.md).

