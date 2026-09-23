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

## Evidence and remaining acceptance

The local portable gates, core/router PE audits, real driver build, and both
Wine frontend builds pass. The expanded mock frame and public frontend
lifetime/unsupported-operation tests pass. Real D3DMetal previously reached
device creation at feature level 11.0 and stopped at the missing converter;
the converter is now available for the next run.

Real GPU pixel acceptance and the full branch validation workflow remain to be
recorded. Keep publication experimental until those checks pass.
