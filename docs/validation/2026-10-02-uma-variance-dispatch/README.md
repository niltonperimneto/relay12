# Phase 1 Agent B: transfer variance and allocation cost

## Finding

The balanced upload cache makes this 48-texture burst substantially more expensive
to create, even when timed shader sampling is removed. The earlier frame-time
headline excluded texture creation and therefore hid that cost. Across six
alternating trials per policy, balanced's upload-burst median was higher in every
matched trial: the median of those paired changes was **+229.5% with MSYNC** and
**+219.7% with standard synchronization**. This is a synthetic burst workload on
one device; it does not establish PEAK's performance or an old/new runtime delta.

All 36 measured runs and both prewarm runs passed byte-exact RGBA8/BGRA8 GPU
copy/readback. Each policy/sync cell contains 1,152 measured frames from six
processes. Direct uploads were not exercised: force admission was explicitly
`0`, both reported UMA flags were false, and every direct-attempt counter was zero.
The `direct` row is thus a second run of balanced behavior with an inactive flag.

## Measurements

The values below pool the measured cycles/frames across six trials. Upload is
one burst of 48 texture creations. Cycle includes that burst and all 48 serial
copy/readback/CPU-verification frames. Frame excludes the upload burst. Cycle
medians cannot be reconstructed by adding the other medians.

| Sync | Policy | Frame median ms | Frame P95 ms | Upload-burst median ms | Full-cycle median ms |
| --- | --- | ---: | ---: | ---: | ---: |
| MSYNC | Legacy | 0.991 | 9.603 | 7.067 | 88.552 |
| MSYNC | Balanced | 0.998 | 10.131 | 23.312 | 105.028 |
| MSYNC | Balanced + inactive direct flag | 1.018 | 11.000 | 22.986 | 127.291 |
| Standard | Legacy | 0.984 | 8.748 | 7.058 | 91.682 |
| Standard | Balanced | 1.046 | 9.210 | 24.935 | 117.092 |
| Standard | Balanced + inactive direct flag | 0.994 | 9.403 | 23.444 | 107.222 |

Trials are the units of comparison. Pooling hundreds of frames does not create
hundreds of independent experiments. Pairing each policy with legacy in the same
rotating trial gives the following balanced changes:

| Sync | Median paired frame change | Paired frame range | Median paired upload change | Paired upload range | Median paired full-cycle change |
| --- | ---: | ---: | ---: | ---: | ---: |
| MSYNC | +1.1% | -10.0% to +17.1% | +229.5% | +122.0% to +268.3% | +25.6% |
| Standard | +1.6% | -9.8% to +24.9% | +219.7% | +176.5% to +349.8% | +31.4% |

Each upload delta below compares the median of four measured bursts in the
balanced process with the corresponding legacy process in the same trial:

| Trial | MSYNC upload delta | Standard upload delta |
| --- | ---: | ---: |
| 0 | +223.2% | +176.5% |
| 1 | +243.0% | +208.3% |
| 2 | +268.3% | +349.8% |
| 3 | +235.9% | +344.5% |
| 4 | +122.0% | +231.1% |
| 5 | +200.9% | +206.1% |

The frame result remains uncertain and fails to demonstrate a repeatable speedup.
The upload penalty is consistent across the two synchronization modes and all
matched trials. No confidence claim or automatic profile qualification follows
from this six-trial diagnostic.

## Why the allocation penalty is plausible

Every balanced and inactive-direct run produced exactly the same upload counters:
180 allocations, 60 reuses, 165 trims, 32,440,320 bytes retained at teardown,
34,603,008 bytes peak retained, and zero pending bytes. The readback pool retained
4,325,376 bytes with two allocations and no trimming. These are allocation
counters, not macOS physical residency.

The upload allocations account for 2,162,688 bytes each. A 48-texture burst is
therefore approximately 99 MiB of staging allocations, while the 32 MiB cache can
keep only 15. The observed counts fit exactly: 48 allocations in the first burst,
then 33 new allocations and 15 reuses in each of four further bursts. This yields
180 allocations and 60 reuses; trimming 165 leaves 15 cached allocations. This
matches deliberate bounded retention causing allocation churn. Legacy lacks
comparable allocation counters, so its exact reuse count and memory savings
cannot be claimed from these logs.

Earlier sampling data already showed the same excluded cost: pooled upload
medians were 8.591 ms legacy versus 25.278 ms balanced under MSYNC, and 7.957 ms
versus 29.608 ms under standard synchronization. Transfer-only persistence makes
timed SRV creation/draw an insufficient explanation of the upload penalty.

## Variance and methodology

The older five-trial sampling run had standard legacy trial medians from
1.0165 to 2.5435 ms, a sample coefficient of variation of 47.4%. Its pooled
balanced frame change was +16.1%, while the median of paired trial changes was
+27.0%. Under MSYNC those summaries were -6.5% and -8.5%. These differing summaries
and substantial spread are reasons to preserve per-trial results and avoid a
single pooled headline.

The new matrix uses the matching `a0d9776` host/driver/artifacts, a dedicated
APFS-copied test prefix, an initial wineserver teardown, one untimed prewarm
process, a shared server across measured processes, and six trials covering all
six orderings of the three policies. No other agents ran GPU work or heavy builds
during the measurement window. MSYNC bootstrapped once in the prewarm process;
standard logs contain no MSYNC bootstrap. Both prewarm logs were checked manually
for final success. The current runner itself does not enforce prewarm success.
A short prewarm can reduce startup differences; it does not prove thermal or
frequency stabilization.

`--transfer-only` removes timed SRV creation, PS binding and draw. It still
compiles/creates shaders during initialization, outside the measured frames.
The frame timer includes blocking staging Map, comparison of every byte in an
approximately 2 MiB image, Unmap and source-resource release. CPU verification,
GPU completion, scheduling, allocation retirement and cache state contribute to
this wall time. It measures neither GPU execution alone nor pure CPU dispatch
cost. Startup/shader cache effects, one short warmup, trial order and uncontrolled
host activity/thermal state remain potential sources of variance. Standard and
MSYNC matrices were run sequentially, so their absolute differences are not a
paired synchronization comparison.

The newer shared-session/prewarm runner options were already uncommitted work.
Agent B preserved them and made no script or test edits. This matrix changes
both artifacts and setup relative to the older five-trial sampling matrix;
differences between those matrices cannot be attributed to transfer-only alone.
The old `402d345` build cannot complete the readback workload and provides no
valid timing baseline.

## Recommended next steps

1. Report upload, complete cycle and frame metrics together; add trial-level
   summaries and paired deltas rather than treating pooled frames as independent
   measurements. Record the actual order, force flag, successful prewarm, artifact
   hashes and source revision automatically.
2. Check prewarm return code and final workload success before continuing; name it
   startup prewarming, and avoid claiming measured thermal stabilization without
   host telemetry. Keep shared/cold session behavior explicit in every report.
3. Evaluate an explicit workload-sensitive upload-cache policy using burst sizes
   below and above the cap, allocation/reuse telemetry, and host footprint/swap.
   Bounded caches trade retained memory for reallocation cost. Maintain the current
   default until a policy improves the intended constrained-memory workload and
   preserves safe fence retirement; increasing a cap by itself is not evidence of
   a better tradeoff.
4. Benchmark the separately qualified forced direct-upload path with both flags
   enabled, nonzero direct-success evidence, and the same six-order design. Keep
   this admission experiment separate from normal capability-gated results.
5. Retest PEAK using a workload both builds can complete before making gameplay or
   old/new overhead claims. Add separate GPU completion and CPU verification
   timing if isolating dispatch cost is required.

## Evidence

[Transfer analysis](transfer-analysis.json), [older sampling analysis](previous-analysis.json),
[artifact and runner provenance](provenance.json),
[MSYNC trials](transfer-msync/result.json), and
[standard trials](transfer-none/result.json) contain the summaries and paired
values. Filtered per-run logs beside each result preserve every raw upload,
frame, cycle and telemetry sample. Original full logs remain in the local sibling
workspace `relay12-work/variance-dispatch-20261002/transfer-msync` and
`transfer-none`; no Steam/authentication logs are included.

The measured invocation for each sync mode used:

```sh
D3DM_WINE_UNIX_CALL=1 D3DM_MTL4=1 CX_ACTIVE_GRAPHICS_BACKEND=d3dmetal \
D3D11ON12_COMPAT_ForceCoherentUMA=0 WINEDEBUG=-all \
python3 scripts/run-uma-memory.py \
  --wine /path/to/matching/Wine/bin/wine64 --artifacts /path/to/a0d9776/artifacts \
  --prefix /path/to/dedicated/prefix --output /path/to/results \
  --sync msync --trials 6 --timeout 120 --transfer-only \
  --wineserver-session shared --prewarm-runs 1
```
