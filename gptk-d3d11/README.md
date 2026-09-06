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

Every failure the router reports to an application is a documented D3D11
return code, which means `DXGI_ERROR_UNSUPPORTED` even when the real cause is
a broken deployment. `WineD3D11ShimGetStatus`, at ordinal 4 above Apple's
three exports, carries the cause that code cannot: which modules loaded, which
exports resolved, the negotiated core ABI, and the `HRESULT` each entry point
will return. Deployment gates on that, not on the debug log.

Neither module is a Wine builtin, so both report through `wine_d3d11_diag`
rather than Wine's `ERR` and `TRACE` macros. It layers `__wine_dbg_output`,
`OutputDebugStringA`, and `stderr`, because Wine delivers
`OutputDebugStringA` as a `DBG_PRINTEXCEPTION_C` raise and an application that
handles it would otherwise swallow every deployment error. Only conditions
that would fail silently are reported, once each: a missing or wrong
`d3d11mt.dll`, an incompatible core, a device or queue whose COM identity
cannot be established, and the deliberate `DXGI_ERROR_UNSUPPORTED` of this
milestone.

`ddi/` holds the clean-room D3D11 DDI work: the layout-assertion harness and
the rules any declaration group must follow. Proprietary WDK headers must
never be copied there, and the declarations are frozen for the Win64 `x86_64`
ABI at natural alignment: `#pragma pack` is prohibited and CI rejects it.

`tests/d3d11on12coretest.c` validates the core's boundary with mock COM
objects, `tests/d3d11shimstatus.c` validates that the router fails closed with
documented codes when neither module is deployed beside it, and
`tests/d3d11ddilayout.c` validates the layout harness itself. All three run
under Wine in pull-request CI.
