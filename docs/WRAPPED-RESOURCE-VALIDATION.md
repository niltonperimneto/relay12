# Experimental wrapped-resource validation

Relay's experimental On12 frontend now wraps caller-owned D3D12 Texture2D
render targets through `ID3D11On12Device::CreateWrappedResource`. Wine patch
0024 resolves core exports 28 and 29 for creation and batched ownership changes.
The existing ABI-v3 table and earlier ordinals are unchanged.

## Supported slice

- One mip, one array slice, single-sample RGBA8 or BGRA8 UNORM Texture2D.
- A default-heap resource on the caller's D3D12 device, with render-target bind
  flags and no CPU-access, miscellaneous, or structured-buffer flags.
- The original resource is retained by the core and driver. A live RTV pins
  the wrapper even after its public texture reference is released.
- Explicit acquire, release, and release-all (`ReleaseWrappedResources(NULL, 0)`).
  Newly wrapped resources start under D3D12 ownership; acquire precedes D3D11 use.
- Input/output states: COMMON/PRESENT, RENDER_TARGET, COPY_SOURCE, COPY_DEST,
  and pixel/non-pixel shader read states. Write states cannot be combined.
- Acquire tells the driver's state tracker the declared input state; release
  asks it to insert transitions to the output state. The application calls
  `Flush` to submit through the original direct queue.
- `D3D11_CREATE_DEVICE_BGRA_SUPPORT` is accepted with the experimental opt-in.

Cross-device resources, duplicate acquisitions, unmatched explicit releases,
duplicate entries within a batch, stale handles, and D3D11 clear/copy/draw of
released resources are rejected. A driver-reported transition failure blocks
further wrapped use on that device: partially emitted GPU transitions cannot
be safely rolled back. Destruction remains idempotent.

The driver uses its local wrapping-handle map; this path does not open a
Windows shared handle or require `ID3D12CompatibilityDevice`. Shared/keyed-mutex
resources, buffers, arrays, multisampling, `IDXGISurface`, Direct2D/DirectWrite,
and swapchain presentation are not qualified by this slice. Immediate-context
calls and external D3D12 queue submissions still need application serialization;
the bounded host lifetime stress does not establish concurrent GPU submission.

## Reproduction and acceptance

Build and stage the artifacts as described in
[FIRST-FRAME-VALIDATION.md](FIRST-FRAME-VALIDATION.md), adding
`d3d11_e2e_wrapped.exe` from the branch workflow, then run:

```sh
python3 scripts/run-d3dmetal-frame.py \
  --wine /path/to/isolated/Wine/bin/wine64 \
  --prefix /path/to/empty/relay-wrapped-prefix \
  --artifacts /path/to/runtime-artifacts \
  --test wrapped
```

`tests/e2e_d3d11_wrapped.cpp` creates the target, queue, fence, command allocator,
and readback buffer through D3D12. Each cycle clears the target blue through
D3D12, acquires it through On12, clears it red through D3D11, releases it to
COPY_SOURCE, and flushes. The caller then copies **the original D3D12 texture**
into its D3D12 readback buffer and checks all 4,096 pixels. It repeats three
cycles for each format and exercises release-all in the middle cycle.

The successful local run used Apple A18 Pro, macOS 27.0 (26A428), Wine 11.17,
and D3DMetal 4.0b2. Both formats passed all three cycles, caller-queue fences
completed, device removal checks succeeded, and the process exited zero after
teardown. The binaries use the pinned llvm-mingw 20240619 UCRT toolchain and
locally built core/Wine host, with the real driver from CI run 35853369494.
This is pixel and ordering evidence; no D3D12 graphics validation layer was
enabled, and it does not certify every possible state combination or a soak.

## Regression coverage

`tests/d3d11on12wrapped.c` drives the core with a mock driver to check null-success
COM queries, foreign devices, unsupported flags and sampling, wrapping-handle
failure, driver-open failure, balanced references, retained RTVs, ownership
batch validation, declared state forwarding, release-all, stale use, repeated
destruction, adapter teardown, transition-error refusal, and eight-thread
ownership versus destruction stress. The branch workflow compiles and runs it.

The clean-room `D3D10DDI_HKMRESOURCE` declaration now matches the documented
32-bit kernel token, with named padding at offset 20 in OPENRESOURCE. Its
40-byte enclosing layout is unchanged. The independent model and separately
compiled driver ABI probe check the layout. No proprietary header was copied.

The next application milestone is a presented frame followed by application-level
D3D12/On12 smoke testing (using reference workloads such as PEAK). Keep the
experimental opt-in until those paths and the longer hardware soak are qualified.
