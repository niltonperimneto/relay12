# UMA integration follow-up — 2026-10-01

Steps 1–3 of the follow-up are implemented. GPU correctness passes on Apple
A18 Pro / 8 GiB / macOS 27.2 with WineCX GPTK 4.7.54 and D3DMetal 4.0b2.
This report does not qualify gameplay performance or physical-memory savings.

The tested binaries are from commit
`e2b99d224f9524ebed78f8850ae674940c5a5012`.
[CI](https://github.com/niltonperimneto/relay12/actions/runs/36809578897)
passed both semaphore and ring lanes. Local Python validation passes 196 tests,
and all toolchain-independent architecture gates pass. Artifact hashes are in
[artifact-sha256.json](artifact-sha256.json); machine-readable outcomes are in
[result.json](result.json).

## Wrapped resources and sampling

The core emits actual wrapped creation/acquire/release results, including early
failures. Successful ownership no-ops emit no transition marker.
[Wrapped GPU tests](wrapped-msync.log) pass under standard synchronization and
MSYNC: RGBA8 and BGRA8 each complete three acquisition/render/release cycles,
byte-exact readback, fences and teardown. Each run records two creations, six
acquisitions and six releases.

Owned Texture2D pixel SRVs now retain resources correctly, support mip ranges,
and clear conflicting output/input bindings. Mock and public-frontend tests
cover failure propagation, stale/cross-device handles, atomic binding validation,
view retention, teardown, getter resurrection and hazards. The previous transfer
failure was BGRA staging Map rejection; the first RGBA transfer had succeeded.

All six final shader-sampling checks pass byte-exact RGBA8/BGRA8 output:
legacy, balanced and balanced+direct under both
[standard synchronization](sampling-none.json) and [MSYNC](sampling-msync.json).
These are small correctness runs, not the five-trial performance benchmark.
Balanced pools clean up within their limits with no pending bytes.

## Driver-visible UMA contract

[Capability results](capabilities.log) show both ARCHITECTURE versions return
S_OK and actively overwrite poisoned outputs with UMA=false and
CacheCoherentUMA=false. Effective upload heaps use WRITE_COMBINE/L0; readback
uses WRITE_BACK/L0. The Wine video shim delegates these architecture queries
without overriding them. D3DMetal's internal reason is unavailable in its
closed implementation.

Raw custom WRITE_BACK/L0 buffer and texture creation and CPU buffer mapping
pass. That does not qualify the On12 direct-upload path: all six runs report
zero direct attempts and successes. It remains gated, and automatic balanced
selection remains disabled. Physical unified memory alone does not establish
the driver's coherency contract. See Microsoft's
[ARCHITECTURE](https://learn.microsoft.com/en-us/windows/win32/api/d3d12/ns-d3d12-d3d12_feature_data_architecture)
and [ARCHITECTURE1](https://learn.microsoft.com/en-us/windows/win32/api/d3d12/ns-d3d12-d3d12_feature_data_architecture1)
definitions.

## PEAK scope and runtime selection

Wine's installed builtin host takes precedence over the adjacent artifact.
The hardware runners now reject mismatching hosts. The final GPU runs use an
isolated runtime clone with the CI host installed in its builtin directory;
its hash matches the staged artifact. The normal runtime and game bottle were
not modified.

The [earlier traced PEAK startup](peak-traced-core.json) presented D3D12 frames
with the new tracing core and the older installed frontend. It made no wrapped
resource calls and qualifies neither wrapping nor the updated frontend.
The final matched-runtime PEAK launch stopped before game startup because
Steam was signed out; it produced no Player.log. A visible Steam client remains
available in the isolated test copy for sign-in. Authentication is required
before repeating that launch through Steam.

Remaining qualification: retest PEAK with the matching runtime after sign-in,
exercise its wrapped path if actually used, then run matched gameplay and
five-trial memory/transfer measurements. No FPS gain is claimed here.
