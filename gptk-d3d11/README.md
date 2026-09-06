# D3D11On12 router

`d3d11shim.dll` occupies D3DMetal's `d3d11.dll` builtin slot. The original
Apple forwarder must be installed beside it as `d3d11mt.dll`.

The shim forwards Apple's ordinary D3D11 entry points unchanged. It resolves
`WineD3D11On12CreateDeviceV1` from an optional `d3d11on12core.dll` for the
D3D11-on-12 path. Until that core implements the real D3D11 runtime/DDI host,
the public entry point returns `DXGI_ERROR_UNSUPPORTED` with initialized output
parameters.

The core publishes a size/versioned `WineD3D11On12Interface` function table.
The router rejects unknown versions, unexpected structure sizes and missing
required entry points. C++ exceptions, allocation ownership and
implementation-specific C++ types must never cross the module boundary.

Neither module is a Wine builtin, so both report through `OutputDebugStringA`
rather than Wine's `ERR` and `TRACE` macros. Only conditions that would
otherwise fail silently are reported: a missing or wrong `d3d11mt.dll`, an
incompatible core, and the deliberate `DXGI_ERROR_UNSUPPORTED` of this
milestone.

`ddi/` holds the clean-room D3D11 DDI work: the layout-assertion harness and
the rules any declaration group must follow. Proprietary WDK headers must
never be copied there. `tests/d3d11on12coretest.c` validates the core's
boundary with mock COM objects, and `tests/d3d11ddilayout.c` validates the
harness itself; both run under Wine in pull-request CI.
