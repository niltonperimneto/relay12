# Agent A: architecture detection review

Phase 1 review on 2026-10-02, using the committed
[forced-UMA measurements](../2026-10-02-forced-uma/README.md). No additional GPU
work was required, and no runtime, prefix or submodule was modified.

## Where the false flags appear

The measured result is **UMA=false and CacheCoherentUMA=false**. Both
`D3D12_FEATURE_ARCHITECTURE` and `D3D12_FEATURE_ARCHITECTURE1` return `S_OK`
and overwrite the probe's poisoned output fields. This distinguishes an
explicit false response from success with untouched caller storage.

The result is identical through the normal `d3d12.dll` entry and through
`d3dmt.dll` device creation, which bypasses the Wine D3D12 shim. The latter
also identifies `d3dmt.dll` as the owner of CheckFeatureSupport slot 13. Both
the Unix-call and legacy bridges produce the same response. The inspected
WineCX GPTK `gptk-video/d3d12shim.c::shim_CheckFeatureSupport` calls original
slot 13 and changes tight-alignment and selected format responses; it does
not change either architecture response. DXGI is used to select/initialize
the adapter, and does not receive the architecture-output structure.

These observations localize the false response to the D3DMetal backend
boundary and exclude the inspected DXGI/D3D12 shim as its source. They do
not identify the closed backend's implementation rationale. A constant
compatibility policy, a stub, and translation inside code shared by both
bridge modes remain possible. Distinguishing those requires backend source,
a trace inside its feature-query implementation, or vendor confirmation.
Repeated measurements through the same public boundary cannot resolve that
remaining uncertainty.

## DTL and test-gate review

Upstream DTL queries ARCHITECTURE1 in `QueryArchitectureFlags()`. Relay patch
0025 initializes the output and returns empty flags after a failed query.
Patch 0026 records that HRESULT and preserves the resulting `m_architecture`.
The recorded-HRESULT member is declared before `m_architecture`, so its
default initialization occurs before the constructor's architecture query.
The successful query result is not overwritten later by a default member
initializer.

The force gate requires exact `1` values for both
`D3D11ON12_COMPAT_ForceCoherentUMA` and
`D3D11ON12_COMPAT_UMADirectInitialUpload`, a successful query, one device
node, and node mask 1. It changes only direct initial-upload eligibility.
It does not rewrite application feature responses or broaden ordinary DTL
Map, rename, staging or memory-profile decisions. On12 patch 0028 retains
the candidate restrictions: initial data for an ordinary single-mip,
single-array, non-MSAA RGBA8/BGRA8 texture. Unsupported custom operations
fall back; allocation and device-loss failures propagate.

The existing controls cover missing force, `0`, invalid force/direct
values, force without direct, and auto-profile selection. Existing forced
standard/MSYNC runs show four actual successes per run with no fallbacks,
and byte-exact shader sampling/readback. No gate change is needed for this
diagnostic phase.

## What the GPU evidence establishes

`tests/uma_gpu_probe.hpp` checks actual CUSTOM/WRITE_BACK/L0 heap properties,
performs a GPU texture-to-buffer copy, waits for its fence and compares
every active source byte for RGBA8 and BGRA8. Its padded rows and width 257
exercise pitch handling. The On12 harness additionally samples these
textures in a shader and verifies readback pixels.

These checks establish correctness for the tested initial-upload operation
on this device/runtime. They do not establish persistent mapping coherency,
CPU/GPU concurrent mutation safety, driver-internal zero-copy storage, or a
performance improvement. An implementation may perform internal staging in
WriteToSubresource while still returning the requested custom properties.
DTL's avoided-staging telemetry describes DTL allocations only.

The raw GPU probe is a diagnostic, not a comprehensive device-loss test:
after command submission, a failed queue Signal or SetEventOnCompletion
currently returns through local COM destructors rather than exiting as the
timeout path does. Future fault-injection work should treat completion
failure as terminal before releasing resources potentially still in use.
No such failure occurred in the recorded checks, and this observation does
not warrant repeating the successful timing/correctness matrix.

## Recommendation

Keep the force override opt-in and leave automatic coherency/profile
promotion disabled. Use the already verified forced path for Agent B's
separate transfer measurements; require telemetry showing actual direct
attempts and successes before attributing a timing change to it. Full PEAK
validation also requires evidence that PEAK reaches a candidate upload or
wrapped-resource operation, rather than only creating an On12 device.
