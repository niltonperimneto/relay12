# UMA memory management

Relay12's balanced profile limits completed staging caches on small UMA devices.
It does not cap application resources, GPU work in flight, or the process's
physical memory. The native D3D12 renderer used by PEAK remains outside this path.

`D3D11ON12_COMPAT_MemoryProfile` accepts `auto` (default), `legacy`, or `balanced`.
Auto selects balanced when the caller's D3D12 device reports UMA and
`GlobalMemoryStatusEx` reports positive physical memory at or below 8 GiB.
Missing UMA/memory information and invalid settings select legacy. Explicit
balanced is available for controlled tests on other devices. Selection is per
device; it does not depend on model names or an OS-version string.

Balanced upload/readback/decoder caches retain at most 32/16/16 MiB of completed,
reusable allocations. Accounting uses D3D12 resource allocation sizes, not source
pixel bytes. Unfinished allocations remain owned until their fence completes,
even when they exceed the cache limit. Existing retirement and retrieval paths
trim completed entries, oldest return first, without an added GPU wait. Pool
metadata allocation failure transfers ownership to fence-aware deferred deletion.
Legacy uses the original pool implementation. No submission threshold changes
are part of this policy.

`D3D11ON12_COMPAT_UMADirectInitialUpload=1` independently enables an experimental
initial-upload path on cache-coherent UMA. It accepts only owned, single-mip,
single-layer, non-MSAA RGBA8/BGRA8 UNORM Texture2D resources with SRV-only binding,
no misc flags, and DEFAULT/IMMUTABLE usage. Before constructing the DTL resource,
it tries a CUSTOM/WRITE_BACK/L0 texture, Map without requesting a pointer,
WriteToSubresource, then Unmap. Success avoids the initial staging allocation and
GPU upload copy. Unsupported operations discard the unpublished candidate and use
the original path; allocation failures/device loss are propagated. Wrapped
resources, updates and application Map paths remain unchanged. Direct uploads are
off by default because CPU visibility can reduce later GPU sampling efficiency.

The baseline already uses GetCustomHeapProperties for upload/readback resources;
a CUSTOM heap alone is not evidence of fewer copies.

## Validation

`relay_memory_pool_test.cpp` checks profile selection, completed versus pending
bytes, reuse, oldest-first trimming, oversized entries, metadata allocation
failure and concurrent returns. CI runs it natively and under Wine. It can also
be built with native ThreadSanitizer.

`peak_on12_probe.exe` reports effective upload/readback heap properties and physical
memory, then tests GPU-visible contents of custom textures with odd widths and
padded source pitches. `d3d11_e2e_uma.exe` additionally creates D3D11 textures through
On12, samples them using shaders, and checks every readback pixel for RGBA/BGRA.
`--bench` runs five upload bursts with 48 textures per burst, recording upload,
transfer-frame and cycle times plus process working set where available. These
serial transfer frames include readback and are not a game FPS estimate.

Use an isolated prefix and a configured Wine/GPTK runtime; never replace a game
installation merely to run the benchmark. Add the runtime's licensed dxilconv.dll
to the CI first-frame artifact directory, then:

```sh
python3 scripts/run-uma-memory.py --wine /path/to/Wine/bin/wine64 \
  --artifacts /path/to/first-frame --prefix /path/to/new-test-prefix \
  --output /path/to/results --sync msync --trials 5
```

The runner rotates legacy, balanced and balanced+direct, restarting only its
dedicated prefix's wineserver between runs. Repeat with `--sync none` for the
standard synchronization path. It retains raw logs and JSON summaries, excluding
each process's first cycle from frame statistics. Confirm MSYNC startup in logs,
and compare Wine's physical-memory report with host hardware. A supported direct
probe is not evidence the On12 path was exercised: require nonzero direct-success
telemetry too.

With `RELAY12_TELEMETRY=1`, device teardown reports the selection reason, physical
memory, direct attempts/successes/fallbacks and avoided staging bytes. Balanced
pools also report current/peak retained bytes, pending/completed bytes, allocations,
reuse and trimming. Legacy marks these pool counters unavailable; zero must not
be interpreted as zero memory use. These are allocation counters, not physical
residency. Host footprint and swap must be measured separately.

Hardware acceptance requires byte-exact output, no monotonic growth, completed
caches within their limits after cleanup, and measurable staging-memory reduction.
Across five alternating trials, median and p95 transfer-frame time must not regress
by more than 5%. Direct uploads remain opt-in even after passing. Do not claim
macOS 27/Neo qualification until the raw hardware results have been recorded.
