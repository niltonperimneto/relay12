# Burst upload-cache performance validation — 2026-10-02

## Result

The new soft/burst policy removes the repeated upload allocations in this
48-texture workload and brings its upload-burst cost back near legacy.
With the new compiled `971b374` artifact, balanced upload-burst pooled medians
were **5.845 ms with MSYNC** and **6.623 ms with standard synchronization**,
compared with legacy's 5.946 ms and 6.596 ms. These meet the approximate 8 ms
workload target. Paired trial median changes were -3.2% and +1.9%; the spread
does not establish a speedup over legacy.

The previous bounded policy measured 23.312/24.935 ms in the same transfer-only
workload. A same-build strict control, disabling the new grace window, reproduced
22.593/24.184 ms and the allocation churn. This supports the cache-policy mechanism,
although configurations and previous-build matrices ran sequentially rather than
as simultaneous or alternating configuration pairs.

All 72 measured runs and four prewarm processes passed byte-exact RGBA8/BGRA8
GPU copy/readback. Both compiled CI lanes passed. ForceCoherentUMA was explicitly
`0`; raw UMA/coherency flags and every direct-attempt counter were zero. Direct
upload is outside these measurements. No PEAK FPS, application CPU-overhead or
physical-memory qualification follows from this synthetic test.

## Configuration and method

The measured semaphore artifact comes from
[`971b374c2409ba00b90d637283bc14cb04674b21`](https://github.com/niltonperimneto/relay12/commit/971b374c2409ba00b90d637283bc14cb04674b21),
[CI run 36960774365](https://github.com/niltonperimneto/relay12/actions/runs/36960774365).
The test used Apple A18 Pro / 8 GiB / macOS 27.2, WineCX GPTK 4.7.54 and
D3DMetal 4.0b2. It installed the exact matching builtin host into a dedicated
APFS-cloned runtime. All DLL, host, Wine and graphics-library hashes are recorded
in [provenance](provenance.json).

Both configurations use upload soft 32 MiB, burst 128 MiB and readback/decoder 16 MiB.
The default has a 2,000 ms upload activity grace; strict control uses grace 0.
These values were explicitly set through the documented environment settings,
and every balanced telemetry line confirms them. Each configuration ran six
trials of legacy/balanced/balanced+inactive-direct under each synchronization mode.
The rotating order covers all six policy permutations. One untimed prewarm
process and a shared wineserver preceded each matrix; its final workload success
was checked manually. No other agent ran GPU work or heavy builds during the
measurement window. Matrices were sequential: default MSYNC, default standard,
strict MSYNC, strict standard. Standard/MSYNC absolute timings therefore do not
form a matched synchronization comparison.

Each process executes five cycles of 48 textures. Cycle 0 is excluded; each policy
contributes 1,152 measured frames and 24 measured upload bursts per sync/configuration.
Frame wall time includes blocking staging Map, byte verification of an
approximately 2 MiB image, Unmap and resource release. Upload time measures a
48-texture creation burst. Full cycle measures that burst plus all 48 serial
copy/readback frames. Shader compilation/creation still occurs before timed work;
`--transfer-only` removes timed SRV creation/binding/draw. These are mixed CPU/GPU
wall times. Existing uncommitted runner/test changes were preserved, and Agent B
made no runner or test edits.

## Pooled measurements

These medians/P95 pool measured samples; trials remain the independent comparison
units. Full-cycle medians cannot be reconstructed by adding frame/upload medians.

| Config | Sync | Policy | Frame median ms | Frame P95 ms | Upload median ms | Full-cycle median ms |
| --- | --- | --- | ---: | ---: | ---: | ---: |
| Default | MSYNC | Legacy | 0.945 | 2.107 | 5.946 | 63.314 |
| Default | MSYNC | Balanced | 0.866 | 1.131 | 5.845 | 60.771 |
| Default | MSYNC | Balanced + inactive direct | 0.899 | 1.254 | 5.898 | 62.053 |
| Default | Standard | Legacy | 1.069 | 3.494 | 6.596 | 72.448 |
| Default | Standard | Balanced | 1.052 | 1.730 | 6.623 | 73.956 |
| Default | Standard | Balanced + inactive direct | 1.059 | 2.095 | 6.572 | 74.326 |
| Strict | MSYNC | Legacy | 1.043 | 1.491 | 6.088 | 69.962 |
| Strict | MSYNC | Balanced | 1.036 | 1.374 | 22.593 | 87.603 |
| Strict | MSYNC | Balanced + inactive direct | 1.042 | 1.511 | 23.090 | 89.424 |
| Strict | Standard | Legacy | 1.008 | 8.711 | 7.790 | 95.523 |
| Strict | Standard | Balanced | 1.105 | 10.269 | 24.184 | 123.472 |
| Strict | Standard | Balanced + inactive direct | 0.935 | 8.825 | 23.338 | 104.026 |

## Paired trial comparisons

Each delta pairs balanced with legacy within the same rotated trial, using each
process's median upload burst or full cycle. Median paired upload/full-cycle
changes were -3.2%/-2.9% under default MSYNC and +1.9%/+2.0% under default standard.
Strict controls were +262.8%/+25.3% and +222.0%/+30.8%, respectively.

| Trial | Default MSYNC upload | Default standard upload | Strict MSYNC upload | Strict standard upload |
| --- | ---: | ---: | ---: | ---: |
| 0 | -3.8% | -29.2% | +257.2% | +273.3% |
| 1 | -10.9% | -5.3% | +254.9% | +138.7% |
| 2 | +2.4% | +5.9% | +268.5% | +197.9% |
| 3 | +59.1% | +16.4% | +161.1% | +223.8% |
| 4 | -51.7% | +21.4% | +269.9% | +247.3% |
| 5 | -2.6% | -2.1% | +283.6% | +220.3% |

Default paired full-cycle changes ranged -43.2% to +2.0% under MSYNC and -53.8%
to +42.1% under standard. This spread, process scheduling, mixed CPU verification
and limited trials prevent precise small-overhead claims. The strict upload
penalty is positive in every paired trial; eliminating that repeated allocation
cost has stronger supporting evidence than a claim about small frame differences.

## Allocation and retention evidence

Every balanced and inactive-direct run within each configuration reported identical
upload counters:

| Config | Allocations | Reuses | Final trims | Peak retained bytes | Final retained bytes | Pending bytes |
| --- | ---: | ---: | ---: | ---: | ---: | ---: |
| Default grace 2000 ms | 48 | 192 | 48 | 103,809,024 | 0 | 0 |
| Strict grace 0 | 180 | 60 | 180 | 34,603,008 | 0 | 0 |

Default allocates the first 48 staging resources and reuses all 48 in each of four
later cycles. It temporarily retains approximately 99 MiB, below the 128 MiB burst
limit. Strict can retain only 15 resources in its 32 MiB cache, matching 33 new
allocations per later burst. Teardown now drains completed entries before telemetry:
its releases count as trims, so default's 48 final trims do not imply active-burst
churn. The native pool test proves zero active-burst trims separately. Final
`effective_limit_bytes=33554432` describes the soft policy after activity cancellation;
it is not a measurement of the forced teardown target.

Both configurations finish with readback peak 4,325,376 bytes, zero retained/pending,
and two allocations. Decoder remains empty. The old policy's final 165 trims plus 15
retained allocations becomes 180 final trims with the new teardown drain. Comparing
raw trim totals across teardown implementations without this distinction would
misstate the mechanism.

Temporary 99 MiB retention is an intentional allocation-versus-memory tradeoff.
These allocation counters do not measure macOS physical residency or memory-pressure
savings. This short continuous workload also does not verify the 2-second quiet period
or pressure response on real hardware; dedicated native tests cover those state
transitions. A longer hardware workload, pressure/footprint evidence and PEAK
qualification remain necessary before automatic balanced selection is promoted.

## Evidence and next steps

[Default analysis](default/analysis.json), [strict analysis](strict/analysis.json),
[default MSYNC trials](default/transfer-msync/result.json),
[default standard trials](default/transfer-none/result.json),
[strict MSYNC trials](strict/transfer-msync/result.json), and
[strict standard trials](strict/transfer-none/result.json) preserve trial metrics,
paired values and telemetry. Filtered logs beside the result files retain every
raw frame/upload/cycle sample; original full logs remain in the local sibling
workspace `relay12-work/burst-policy-validation-20261002`. No authentication logs
are included. [Earlier diagnosis](../2026-10-02-uma-variance-dispatch/README.md)
provides the previous artifact's transfer-only measurements.

The scoped allocation/performance target is met by this workload. Keep automatic
promotion pending real memory-pressure/physical-footprint acceptance and repeatable
PEAK measurements. Keep direct-upload admission experiments separate. For future
performance reports, retain upload/full-cycle metrics and paired trial summaries;
frame timing alone excludes the cost corrected here.
