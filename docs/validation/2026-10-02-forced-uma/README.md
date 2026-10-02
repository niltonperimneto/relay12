# Coherency report source and forced direct-upload experiment

The measured device reports **UMA=false and CacheCoherentUMA=false**, not
UMA=true with only coherency false. Both ARCHITECTURE query versions return
S_OK and overwrite poisoned output fields. The same values appear when the
probe directly creates a device through `d3dmt.dll`, bypassing the Wine D3D12
shim, and when the legacy bridge replaces the Unix-call bridge.

The owning query module is `d3d12.dll` on the ordinary path and `d3dmt.dll` on
the raw path. Wine's D3D12 shim delegates these queries without altering them;
DXGI receives no architecture-output structure. The false values are already
present at the D3DMetal backend boundary. This rules out zeroing by the inspected
Wine DXGI/D3D12 shim. D3DMetal's closed implementation does not expose whether
the response is a stub or an intentional emulation policy. Calling it a proven
Apple cache-coherency limitation would exceed the evidence.

Raw CUSTOM/WRITE_BACK/L0 initialization passes on both entry points. The probe
checks the resource's actual heap properties and copies GPU texture bytes to
readback: both RGBA8 and BGRA8 have zero mismatches over 9,252 active bytes,
with width 257, height 9, source pitch 1,040 and readback pitch 1,280. This tests
GPU visibility after WriteToSubresource, beyond a successful CPU Map.

## Test override

[DTL patch 0026](../../../patches/dtl/0026-gate-forced-coherent-uma-upload-test.patch)
adds `D3D11ON12_COMPAT_ForceCoherentUMA=1`. It requires the existing
`D3D11ON12_COMPAT_UMADirectInitialUpload=1`, a successful architecture query and
a single-node device using mask 1. It admits only the existing narrow direct
initial-upload candidate. It leaves the raw architecture flags, ordinary Map
and copy decisions, memory-profile selection and synchronization contracts intact.
Unsupported custom creation/initialization still falls back; allocation and
device-loss failures retain their propagation. Diagnostics distinguish requested
force, active forced admission, actual direct successes and avoided staging bytes.

The raw probe was compiled with pinned llvm-mingw 20240619 UCRT Clang using
the CI flags (`-O2 -static -Wall -Wextra -Werror -fno-exceptions -fno-rtti`).
Both patch variants apply to throwaway clones of the pinned DTL revision, and
local architecture gates and 196 Python tests pass.
[CI for a0d9776](https://github.com/niltonperimneto/relay12/actions/runs/36956903658)
passes both semaphore and ring lanes. The hardware tests use its semaphore
artifacts, with the matching builtin host installed in a dedicated runtime copy.

## Forced On12 results

With both flags set to `1`, all four initial textures per run use the direct
candidate, with four direct successes, zero fallbacks and 45,072 avoided DTL
staging bytes. RGBA8/BGRA8 shader sampling and readback are byte-exact under both
[MSYNC](forced-msync-direct.log) and
[standard synchronization](forced-none-direct.log). Raw UMA and coherency flags
remain false. The direct runs report zero upload-pool allocations and retained
upload bytes; that is not a physical-memory or driver-internal zero-copy claim.

Force alone does not activate the candidate: the runner's legacy and balanced
control modes complete the normal path under both sync methods. Additional
[controls](controls.json) pass with force absent, `0`, invalid force `11`, and
invalid direct flag `11`, all with zero attempts. Force plus direct on an `auto`
profile completes four direct uploads while the effective profile stays legacy
with reason `uma-unavailable`. MSYNC startup is confirmed in each MSYNC log.
This validates the opt-in path on the tested device without changing automatic
profile eligibility. Performance and PEAK qualification remain separate work.

## Evidence

- [Normal entry queries](shim.log)
- [Backend entry queries](backend.log)
- [Backend queries through the legacy bridge](backend-legacy-bridge.log)
- [Backend custom texture GPU check](backend-gpu.log)
- [Normal entry custom texture GPU check](shim-gpu.log)
- [MSYNC force-policy matrix](forced-msync.json)
- [Standard force-policy matrix](forced-none.json)
- [Admission controls](controls.json)
- [Machine-readable result](result.json)

Hardware: Apple A18 Pro / 8 GiB / macOS 27.2. Runtime: isolated WineCX GPTK
4.7.54 / Wine 11.17 / D3DMetal 4.0b2. These checks do not establish performance
improvement or enable automatic coherency/profile promotion.
