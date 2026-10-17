# Deferred D3D11On12 initialization

Patch 0027 retains the existing standalone Wine frontend. Creation and COM
interface queries can complete without loading `d3d11on12.dll` or opening a DTL
adapter. The wrapper retains the caller's D3D12 device and queue. Feature-level
and creation-flag access, private data, DXGI adapter identity and GetD3D12Device
remain available without allocating translation resources.

The additive `WineD3D11On12ValidateDeviceV1` core export at ordinal 45 validates
flags, feature-level arguments, direct queue, queue/device identity, node mask,
ID3D12Device1 availability and selected feature level. It makes no driver calls.
ABI version 3 and existing ordinals are unchanged. The new host requires this
export and refuses an older core cleanly. Deploy the host and core together;
older hosts can continue using the new core.

Each wrapper uses INIT_ONCE to open and validate its adapter exactly once when
backend work is requested. Resource creation, binding, draw, flush and wrapped
resource operations keep their existing dispatch. Unsupported frontend methods
remain unsupported. Empty ClearState and final release do not initialize DTL.
The retained device and queue are released even when the wrapper was never used.

Input validation still fails during creation. Driver loading, adapter allocation
and mandatory driver-operation failures now occur on first backend use. This is
an intentional error-timing change: applications relying on creation failing
when DTL is absent may take a different fallback path. The original HRESULT is
latched, returned by subsequent backend operations and exposed through
GetDeviceRemovedReason. Void methods report their existing diagnostics and do
not dispatch after initialization failure. A partially opened adapter is closed
before failure is published; initialization is not retried.

Compiled mock tests check allocation avoidance and activation/lifetime behavior.
They do not establish a particular memory saving, video playback or complete game
compatibility. The proposed 91 MB GPU / 105 MB process-memory reduction requires
matched workload measurements after this implementation.

[Local build, lifecycle and GPU readback evidence](validation/2026-10-17-lazy-initialization.md).
