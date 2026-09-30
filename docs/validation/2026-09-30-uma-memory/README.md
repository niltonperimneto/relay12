# UMA hardware validation — 2026-09-30

Hardware: Apple A18 Pro, 8 GiB, macOS 27.2. Runtime: WineCX GPTK
4.7.54 / Wine 11.17 with D3DMetal 4.0b2, Metal 4 and the Unix-call bridge
enabled. Relay12 DLLs came from the semaphore artifact for
`402d3458efbc2f7d8ba12c5beab7a31b471fca9f`. Test startup/diagnostics were
corrected in `240c628`; the runtime DLLs were not rebuilt locally.

The pool and upload failure tests pass under both standard synchronization and
MSYNC. The native pool test also passes with ThreadSanitizer. All 193 Python
tests and the repository's local static gates pass.

On the real GPU, CUSTOM/WRITE_BACK/L0 initialization followed by GPU copy and
byte-exact readback passes for RGBA8 and BGRA8, with an odd width and padded
source pitch. The runtime nevertheless reports `UMA=0`, `CacheCoherentUMA=0`.
The capability gate therefore prevents On12 direct initial uploads from being
selected. A successful raw heap probe does not qualify the On12 direct path.

The complete On12 sampling test fails at `CreateShaderResourceView` with
`DXGI_ERROR_UNSUPPORTED`. The transfer-only test fails at staging `Map` with
the same HRESULT in legacy, balanced and balanced+direct modes, under both
standard synchronization and MSYNC. It produces
zero validated transfer frames. There is no valid memory-saving or transfer-time
comparison, and automatic balanced selection remains disabled.

PEAK was launched through real Steam in an isolated copy of the user's bottle,
with `-force-d3d12` and MSYNC. The normal bottle was not modified. These timed
startup runs are smoke tests, not matched gameplay benchmarks. The smoke
checker results and filtered graphics logs are retained alongside this report.
Both legacy and balanced runs selected D3D12, created the On12 device and
presented frames, with no crash or device removal reported. Neither showed
wrapped-resource activity, so the strict smoke checker rejected both runs.
Both reported `ConnectExternalTokenValidationFailed` during EOS login;
no gameplay qualification was obtained. The observed startup rates (25.23 and
33.37 FPS respectively) are single, unmatched runs with different cache/warmup
conditions and must not be interpreted as a performance improvement.

Remaining acceptance work:

1. Resolve the sampling/SRV and staging-readback failures, including BGRA
   readback support, then rerun the byte-exact On12 tests.
2. Investigate the backend's UMA/coherency reports before qualifying direct
   uploads; require actual direct-success telemetry.
3. Exercise PEAK's wrapped-resource path and verify its successful trace markers.
4. Run five alternating trials in a repeatable workload, collecting completed
   cache bytes, process footprint/swap and frame-time distributions. Timed
   startup presentation rates cannot establish a performance improvement.

Logs in this directory are allowlisted excerpts, not complete process logs.
Full local results are retained in
`relay12-work/uma-validation-20260930` beside the repository; Steam/EOS
authentication output is deliberately excluded from the published evidence.
