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
