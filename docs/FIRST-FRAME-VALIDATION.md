# Experimental first-frame validation

The acceptance target is an offscreen 64×64 RGBA8 texture with a red triangle
and blue background. The application supplies the D3D12 device and direct
queue, checks device identity through `ID3D11On12Device1`, waits for a fence on
that queue, and checks center/corner bytes through staging readback. It repeats
creation, rendering, and teardown three times. Presentation, shared resources,
additional shader stages, and general application compatibility are deferred.

## Runtime and reproduction

Use a dedicated Wine/GPTK runtime clone configured with D3DMetal. Do not replace
an application's installed DLLs. The first-frame artifact directory contains:

- `d3d11.dll` (Relay router), `d3d11on12core.dll`, and `d3d11on12host.dll`.
- The **real** `d3d11on12.dll`, never the mock used by portable tests.
- `d3d11_e2e_triangle.exe` and the separately built `dxilconv.dll`.

The converter is built from Microsoft DirectXShaderCompiler revision
`416fab6b5c4ba956a320d9131102304da995edfc` by `build-dxilconv.yml`. Keep its
license files with the DLL. The [initial converter build succeeded](https://github.com/niltonperimneto/relay12/actions/runs/35781702119).
It is a runtime dependency of the driver's shader conversion path, separate
from the clean-room host and external SDK/WDK overlay.

```sh
python3 scripts/run-d3dmetal-frame.py \
  --wine /path/to/isolated/Wine/bin/wine64 \
  --prefix /path/to/empty/relay-frame-prefix \
  --artifacts /path/to/first-frame
```

The runner enables `RELAY12_EXPERIMENTAL_FRAME=1` and creation-stage diagnostics,
selects the native Relay/driver/converter DLLs and builtin Wine host/D3DMetal
modules, and imposes a 120-second process timeout. It accepts only an empty
prefix or one it previously marked as dedicated, then stops that prefix's
wineserver after the test. Module paths are printed by the harness. A successful
compile or device creation alone is not a GPU acceptance result.

## Implemented fixes and checks

- Requested and returned feature levels use distinct storage; clearing the
  output can no longer overwrite the test's input before validation.
- Immediate-device shader creation uses the driver's non-COM sub-object;
  the corresponding DDI table slots are intentionally null upstream.
- The offscreen driver skips DXGI table registration when no table is supplied.
  No undocumented DXGI structure layout is fabricated.
- `ID3D12CompatibilityDevice` is optional for owned resources. Operations
  requiring shared-resource compatibility reject its absence explicitly.
- Bound frontend children have private references independent of their public
  COM reference group. This prevents early destruction without a device/context
  reference cycle. RTVs retain their texture through the same mechanism.
- The compiled frontend rejects unsupported operations and newer/video
  interfaces before touching absent WineD3D backing objects.
- Core exports 19–27 add RTV lifecycle, frame state, copy/readback, and frame
  readiness checking. Existing ordinals and the version-3 interface table remain
  unchanged; the experimental host requires all new exports.

## Hardware acceptance — passed 2026-09-23

Commit `7c554bb58e2ad947bcb09175e229affa6abf19a2` passed the
[complete branch validation workflow](https://github.com/niltonperimneto/relay12/actions/runs/35853369494).
The unmodified `first-frame-7c554bb58e2ad947bcb09175e229affa6abf19a2`
artifact from that run, with the converter above, then passed the hardware
harness in a fresh dedicated prefix. No additional rendering fix was required.
This build includes the previously added `pfnPerformAmortizedProcessingCb`
callback needed by driver submission.

The machine ran macOS 27.0 (26A428), Wine 11.17, and D3DMetal 4.0b2 on
Apple A18 Pro. `D3DM_MTL4` was unset; this does not qualify the explicitly
enabled Metal 4 configuration. The process returned zero and all three cycles
completed:

| Check | Result in each cycle |
| --- | --- |
| On12 device and original D3D12 device identity | Passed, feature level 11.0 |
| Real driver, Wine host, and DXBC converter | Loaded from the staged artifact directory |
| Center RGBA bytes | `ff0000ff` — drawn red |
| Corner RGBA bytes | `0000ffff` — cleared blue |
| Fence on caller's direct queue | Value 1 completed within the 10-second limit |
| Device/resource teardown | Completed; harness reported all three teardowns |

The [complete output](validation/2026-09-23-first-frame/hardware-run.log) and
[result and SHA-256 manifest](validation/2026-09-23-first-frame/result.json)
preserve the command, module paths, binary identities, and result. The prefix's
`d3d12.dll` matches the isolated runtime's Apple D3DMetal DLL by SHA-256; the
log also contains D3DMetal diagnostics. MoltenVK enumeration messages in the
same log do not identify the renderer used for this frame.

Nonfatal diagnostics remain in the raw log: D3DMetal reported unsupported
`ID3D12Device3::EnqueueMakeResident` and a primitive-topology conversion during
teardown; an Apple neural-engine compilation diagnostic also appeared. These
did not prevent the required pixels, fence, or teardown. They remain relevant
observations for broader workloads and longer runs.

## Next milestone

The offscreen first-frame gate is complete. Retain the experimental opt-in and
continue with the wrapped-resource milestone described in
[WRAPPED-RESOURCE-VALIDATION.md](WRAPPED-RESOURCE-VALIDATION.md), whose first
RGBA/BGRA ownership slice now passes local hardware readback. The existing conformance probe needs BGRA
creation support and pixel/state verification for that milestone. Presentation,
PEAK's D3D12/On12 smoke test, and a hardware soak remain unqualified; the mock
soak and partial-publication coverage already in this commit do not replace
those application and hardware runs.
