# Relay12 Completion Implementation Plan

## Objective

Complete Relay12 by extending the existing vertical-slice architecture until
the real Wine frontend can render and read back a deterministic triangle
through D3D11On12, D3D12TranslationLayer, and D3DMetal.

Each pipeline area should land as an independently testable Wine patch. Avoid
combining all remaining callbacks into one large change.

## Current implementation status

Status as of 2026-09-19. This table distinguishes code present in the working
tree from functionality proven on the real D3DMetal path.

| Milestone | Working-tree status | Completion evidence still required |
| :--- | :--- | :--- |
| Input-layout lifecycle and `IASetInputLayout` | Implemented as patch 0013 | MinGW PE build in CI |
| Vertex/pixel shader lifecycle and binding | Implemented as patch 0014 | MinGW PE build in CI |
| Texture2D core lifecycle | Implemented as patch 0015 | MinGW PE build in CI, and the mock-driver run under Wine |
| Render-target views | Not started | Patch 0016 and lifecycle tests |
| Output merger, viewport, and clear | Not started | Patch 0017 and exact DDI-dispatch tests |
| Copy, map, unmap, and readback | Not started | Patch 0018 and byte-exact readback tests |
| D3D11 device/context publication | Deliberately disabled | Complete readiness check and COM publication tests |
| Real D3DMetal triangle | Not proven | Apple Silicon hardware execution with pixel readback |

The branch tip does not yet contain patches 0013 to 0015. A clean status or a
green Python gate alone must not be interpreted as first-frame completion: no
part of the Texture2D slice has been compiled, because the MinGW toolchain is
CI-only.

### Verified baseline

After the shader slice and the initial Texture2D core changes, the following
portable checks pass:

```text
96 Python CI-gate tests
DDI layout model: 53 structures and 463 fields
DDI header provenance gate
Interface-acquisition audit
Shared-state audit
Wine patch-series application and frontend lifecycle audit through patch 0014
```

These checks prove structural consistency and fail-closed lifetime behavior.
They do not prove PE compilation, GPU command execution, synchronization,
Metal rendering, or pixel correctness.

## 1. Basic shader lifecycle and binding — complete

Implemented by `0014-d3d11-route-shaders.patch`.

### Core boundary

- Add an opaque `WineD3D11On12Shader` handle.
- Record its owning device and shader stage.
- Implement vertex-shader creation.
- Implement pixel-shader creation.
- Implement shader destruction.
- Implement `VSSetShader` and `PSSetShader`.
- Allocate private storage through `pfnCalcPrivateShaderSize`.
- Call the appropriate DDI shader creation callback.
- Capture creation errors through `pfnSetErrorCb`.
- Reject stale, foreign-device, and wrong-stage handles.
- Drain active binding operations before destroying a shader.

### Wine frontend

- Extend `d3d11_backend_ops` with shader lifecycle and binding operations.
- Embed a backend handle in `d3d_vertex_shader` and `d3d_pixel_shader`.
- Retain Wine's existing shader parsing and validation.
- Route only the D3D11On12 backend through Relay12.
- Leave WineD3D behavior unchanged.
- Resolve every new core export during backend creation.
- Fail closed when any mandatory export is missing.

### Required tests

- Successful create, bind, unbind, and destroy.
- Vertex/pixel shader type confusion.
- Foreign-device handles.
- Stale handles.
- Injected creation failure.
- Device-teardown cleanup.
- Concurrent binding and destruction.

The implementation also validates the DXBC container header and declared
length before any driver callback, copies bytecode into aligned temporary
storage, supports both D3D11 and D3D10 vertex/pixel binding entry points, and
keeps WineD3D's existing shader path unchanged.

## 2. Texture2D resource lifecycle — complete

Implemented by `0015-d3d11-route-texture2d-resources.patch`.

### Core boundary

- The C-compatible `WineD3D11On12Texture2D` opaque handle.
- Core create/destroy exports at ordinals 17 and 18.
- The publicly documented `D3D10DDIRESOURCE_TYPE` values for Texture2D.
- A resource-kind discriminator on the shared `ResourceState` registry, so
  buffers and textures keep one driver-handle identity.
- Mip information for every mip and array slice, translated initial
  subresource data, and allocation/subresource-count overflow checks.
- Usage, bind, CPU-access/map, miscellaneous, format, sample, mip and array
  metadata forwarded to `D3D11DDIARG_CREATERESOURCE`.

### Wine frontend

- `d3d_texture2d` embeds the backend handle; `d3d11_backend_ops` gains
  `create_texture2d` and `destroy_texture2d`, both mandatory for a backend
  device.
- D3D11 and D3D10 Texture2D creation route through Relay12 for the On12
  backend only. A caller-supplied `wined3d_texture` is the swapchain case and
  stays on the WineD3D path.
- A backend texture owns no WineD3D object and no DXGI resource, so
  `QueryInterface`, the three private-data methods, `GetDesc`, `AddRef`,
  `Release`, and the D3D10 `Map`/`Unmap` pair each test for it and report an
  honest result rather than dereferencing NULL.
- `MipLevels` of zero is resolved at creation so `GetDesc` reports what the
  application actually received.

### Defect found and fixed while landing this

Both `d3d11_backend_ops` tables were positional initialisers. Adding
`create_texture2d` and `destroy_texture2d` to the struct shifted every
WineD3D entry after `destroy_buffer`: `destroy_vertex_shader` received
`wined3d_backend_set_vertex_shader`, the two shader binding ops became NULL,
and the last two members were left uninitialised. Releasing a vertex shader
on the **ordinary WineD3D path** would have called a binding function through
a destroy signature.

Both tables are now designated initialisers, and
`scripts/check_wine_d3d11_backend.py` requires them to stay that way.

### Coverage

`tests/d3d11on12openadapter.c` drives the core against the mock driver for
mip-chain correctness across array slices, zero-`MipLevels` resolution,
format and bind-flag forwarding, initial data, initial data with a null
pointer, double destruction, five rejected-argument cases, and survival to
device teardown. `tests/d3d11on12mockdriver.c` checks the full mip array
rather than its first entry, because the core derives that array itself.

### Known gaps

- A backend texture does not answer `IDXGISurface`: `d3d_device_create_dxgi_resource`
  needs a wined3d resource to wrap and there is none.
- Mapping a backend texture returns an error; it belongs to the readback
  milestone, which needs per-subresource mapped state and map-mode validation.
- Cross-device rejection is not tested for textures. `DestroyTexture2DV1`
  takes only a handle, so the check the binding entry points can make is not
  expressible here.

## 3. Render-target-view ownership

Create `0016-d3d11-route-render-target-views.patch`.

Add an owned RTV handle and registry supporting:

- `CalcPrivateRenderTargetViewSize`
- `CreateRenderTargetView`
- `DestroyRenderTargetView`

Each RTV must retain or otherwise pin its associated resource until the view
is destroyed.

Validate that:

- The resource belongs to the same device.
- The resource has `D3D11_BIND_RENDER_TARGET`.
- The view format and resource dimension agree.
- Releasing a resource with live views follows D3D11 lifetime semantics.

## 4. Basic output-merger and rasterizer state

Create `0017-d3d11-route-basic-frame-state.patch`.

Implement:

- `OMSetRenderTargets`
- `RSSetViewports`
- `ClearRenderTargetView`

The first-frame milestone may deliberately support only:

- One render target.
- No depth-stencil view.
- No unordered-access views.
- One viewport.
- No scissor requirement.

Broader cases must return an intentional unsupported result rather than being
partially accepted.

Binding methods should hold shared registry locks while resolving handles and
calling the DDI. Destruction should remove objects under an exclusive lock,
drain existing binders, and only then destroy the driver object.

## 5. Copy and readback

Create `0018-d3d11-route-readback.patch`.

Implement:

- Staging Texture2D creation.
- `CopyResource`.
- `Map`.
- `Unmap`.

The mapping implementation must:

- Track mapped state per subresource.
- Validate map modes against resource usage and CPU-access flags.
- Never expose a driver pointer after unmapping.
- Prevent destruction while a mapping is active.
- Propagate device-removal and allocation failures.
- Validate row and depth pitches before accessing pixels.

This milestone enables deterministic pixel validation without relying on
screenshots.

## 6. Publish the complete D3D11 device

Keep the standalone Wine device unpublished until every operation required by
the E2E triangle is routed.

Then update `WineD3D11On12CreateDeviceV1` to return the real:

- `ID3D11Device`
- Immediate `ID3D11DeviceContext`
- Selected feature level

Before publishing the device, perform an internal readiness check that every
mandatory backend operation exists. Relay12 must not return a valid-looking
device that fails on its first normal API call.

## 7. Execute the real D3DMetal triangle

Use `tests/e2e_d3d11_triangle.cpp` with as few changes as possible.

The acceptance criteria are:

- The corner pixel exactly matches the clear color.
- The center pixel exactly matches the triangle color.
- WineD3D fallback is not selected.
- No validation error is reported.
- No device removal occurs.
- Every object returns to its expected lifetime count.
- Repeated execution does not leak memory or leave queued work pending.

The intended path is:

```text
D3D11 application
    -> Wine D3D11 frontend
    -> Relay12 core and clean-room DDI host
    -> Microsoft D3D11On12
    -> D3D12TranslationLayer
    -> D3DMetal
    -> Metal
```

## 8. Wrapped resources and synchronization

After the first real frame works, implement the functionality needed by hybrid
D3D11/D3D12 applications:

- `ID3D11On12Device::CreateWrappedResource`
- Wrapped Texture2D resources.
- Acquire and release ownership transitions.
- Resource-state tracking.
- Queue batching and submission.
- D3D12 fences.
- CPU/GPU waits and memory barriers.
- Device-removed propagation.
- Safe shutdown while work remains in flight.
- Swapchain and presentation integration.
- Direct2D and DirectWrite interoperability.

## Engineering rules

- Keep the public core ABI C-compatible and size-versioned.
- Use opaque handles across DLL boundaries.
- Let Wine perform API-level and bytecode parsing.
- Let the Relay12 core translate validated data into DDI arguments.
- Use one registry per genuinely distinct lifetime category.
- Add fault injection with every new lifecycle operation.
- Never return mock or partial success.
- Keep every Wine patch small enough to audit and bisect.
- Extend `scripts/check_wine_d3d11_backend.py` with every routed operation.
- Preserve WineD3D behavior for non-Relay12 devices.
- Require real GPU evidence before declaring rendering milestones complete.

## Recommended implementation order

```text
Shader lifecycle and binding
    -> Texture2D resources
    -> Render-target views
    -> Output merger, viewport, and clear
    -> Copy, map, and readback
    -> Publish the device and context
    -> Real D3DMetal triangle
    -> Wrapped resources and queue synchronization
```

This order reaches visible, testable output quickly while preserving the ABI,
failure-handling, concurrency, and ownership guarantees established by the
buffer and input-layout work.
