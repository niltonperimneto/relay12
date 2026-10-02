# UMA transfer performance check — 2026-10-02

## Result

The final `e2b99d2` runtime completed five alternating trials each of legacy,
balanced, and balanced+direct on Apple A18 Pro / 8 GiB / macOS 27.2, using
WineCX GPTK 4.7.54 and D3DMetal 4.0b2. The benchmark exercised shader sampling
and readback; each policy contributed 960 measured transfer frames. It ran
under MSYNC and standard synchronization on the isolated runtime whose builtin
host matched the tested artifact.

| Sync | Policy | Median frame ms | P95 frame ms |
| --- | --- | ---: | ---: |
| MSYNC | Legacy | 1.351 | 12.178 |
| MSYNC | Balanced | 1.263 | 13.256 |
| MSYNC | Balanced + direct flag | 1.344 | 13.334 |
| Standard | Legacy | 1.127 | 11.545 |
| Standard | Balanced | 1.308 | 12.064 |
| Standard | Balanced + direct flag | 1.278 | 11.570 |

These runs do not show a consistent performance improvement. Balanced changes
the median by -6.5% under MSYNC and +16.1% under standard sync; its P95 is 8.9%
and 4.5% higher, respectively. Run-to-run medians varied substantially and
frame-time tails were high, so the median deltas are too noisy to call a gain or
regression. The harness measures transfer-frame wall time, not application CPU
overhead or PEAK gameplay FPS.

Balanced teardown telemetry reports 32,440,320 upload-pool bytes and 4,325,376
readback-pool bytes retained, with no pending bytes; peak upload retention was
34,603,008 bytes. Legacy does not expose equivalent pool counters, and these
are allocation counters rather than physical residency. Both direct-flag runs
report zero direct attempts and successes because the device says
`UMA=false` and `CacheCoherentUMA=false`; these timings do not measure direct
uploads.

## Comparison limits

The previous `402d345` artifact cannot run the same sampling/readback workload:
it rejects `CreateShaderResourceView`; transfer-only mode then fails staging
`Map` with `DXGI_ERROR_UNSUPPORTED`. Its logs are in the untracked local test
workspace at `relay12-work/uma-validation-20261002-baseline-402d-msync` and
`relay12-work/uma-validation-20261002-baseline-402d-transfer-msync`. Therefore
there is no valid before/after timing comparison against that build. The old
build failing these calls is functional evidence that the new integration
covers more work, not evidence of speedup.

## Conclusion

This validates correctness and gives a rough profile comparison for the
synthetic transfer workload. It does not validate lower overhead versus the
previous runtime. A defensible performance conclusion still needs a matched PEAK
gameplay run and an old/new workload both builds can complete, with repeated
frame-time and CPU measurements. The updated PEAK launch has not produced a
game log.
