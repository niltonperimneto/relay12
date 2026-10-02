# UMA memory management

Relay12's balanced profile limits completed staging caches on small UMA devices.
It does not cap application resources, GPU work in flight, or the process's
physical memory. The native D3D12 renderer used by PEAK remains outside this path.

`D3D11ON12_COMPAT_MemoryProfile` accepts `auto` (default), `legacy`, or `balanced`.
The automatic promotion gate is currently disabled pending hardware acceptance;
`auto` therefore retains legacy behavior and reports `qualification-pending`
on otherwise eligible devices. Explicit `balanced` enables testing now. Once
qualified, auto selects balanced when the caller's D3D12 device reports UMA and
`GlobalMemoryStatusEx` reports positive physical memory at or below 8 GiB.
Missing UMA/memory information and invalid settings select legacy. Explicit
balanced is available for controlled tests on other devices. Selection is per
device; it does not depend on model names or an OS-version string.

Balanced completed-upload caches have a 32 MiB soft limit and a 128 MiB
burst limit. Successful allocation, return, or reuse extends a 2,000 ms grace
period. Once grace expires, the next pool operation reclaims completed entries
to the soft limit; there is no timer thread. Readback and decoder caches retain
at most 16 MiB each. Accounting uses D3D12 resource allocation sizes, not source
pixel bytes. This admits the measured 99 MiB upload burst without repeatedly
creating staging buffers, at the cost of temporarily retaining more memory.

Settings are read per device for explicit `balanced`:

| Environment variable | Default | Maximum |
| --- | ---: | ---: |
| `D3D11ON12_COMPAT_UploadCacheMiB` | 32 | 1024 |
| `D3D11ON12_COMPAT_UploadBurstCacheMiB` | 128 | 1024 |
| `D3D11ON12_COMPAT_UploadBurstGraceMs` | 2000 | 60000 |
| `D3D11ON12_COMPAT_ReadbackCacheMiB` | 16 | 1024 |
| `D3D11ON12_COMPAT_DecoderCacheMiB` | 16 | 1024 |

Only decimal digits are accepted. Invalid or oversized values retain the default
and produce a diagnostic. Zero is allowed: zero grace restores the strict soft
cap; zero soft and burst limits disable completed retention. Burst limits below
the soft limit normalize to the soft limit. Telemetry records the actual limits,
grace and effective cap alongside allocation/reuse counters.

Unfinished allocations remain owned until their fence completes, even when they
exceed either limit. Existing retirement and retrieval paths trim completed
entries, oldest return first, without an added GPU wait. Allocation pressure
cancels burst grace and reclaims completed excess to the soft limit before the
existing OOM fallback. Teardown releases completed entries; neither path releases
pending GPU references. Metadata allocation failure transfers ownership to
fence-aware deferred deletion. Legacy uses the original pool implementation. No
submission threshold changes or automatic profile promotion are part of this
policy.

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

## Experimental coherency admission override

`D3D11ON12_COMPAT_ForceCoherentUMA=1` permits a controlled test of the direct
initial-upload path when the backend's architecture report is conservative.
It requires `D3D11ON12_COMPAT_UMADirectInitialUpload=1` as well, a successful
ARCHITECTURE1 query, and a single-node device using node mask 1. Only the exact
value `1` enables either flag. Unsetting the force flag restores capability
gating on the next device creation.

The override bypasses only admission to the existing constrained RGBA8/BGRA8
initial-upload candidate. It preserves `m_architecture` and all application
CheckFeatureSupport results. In particular it does not force Resource.cpp's
staging Map, rename, or copy decisions, alter automatic memory-profile selection,
or remove fences. CUSTOM/WRITE_BACK/L0 creation, Map and WriteToSubresource must
still succeed. Unsupported operations fall back; allocation failures and device
loss retain their error handling. A startup diagnostic names the override, and
teardown telemetry records the raw architecture flags, query HRESULT, force
request, forced admission and direct attempts/successes separately.

For an isolated runtime containing the matching patched host and driver:

```sh
D3D11ON12_COMPAT_ForceCoherentUMA=1 \
  python3 scripts/run-uma-memory.py --wine /path/to/Wine/bin/wine64 \
  --artifacts /path/to/first-frame --prefix /path/to/test-prefix \
  --output /path/to/forced-results --sync msync --trials 5
```

The runner supplies the direct flag only for its direct policy. Legacy and
balanced therefore also check that the force flag alone does not activate direct
uploads. Require valid shader pixels and nonzero direct-success telemetry to
qualify this experiment. Until that evidence is recorded, forced admission is
not automatic coherency or performance qualification.

The tested Wine D3D12 shim creates devices through `d3dmt.dll` and forwards
ARCHITECTURE/ARCHITECTURE1 queries without modifying their results. The probe's
`--capabilities-only --raw-backend` mode calls that backend export in a fresh
process, avoiding the shim's device-vtable wrapper, and reports the module owning
CheckFeatureSupport. `--uma-only --raw-backend` additionally validates custom
texture heap properties and every GPU-copied byte. DXGI initialization still
precedes device creation because the Wine bridge requires it; DXGI does not
receive the architecture-output structures. The measured backend reports
UMA=false as well as CacheCoherentUMA=false. This differs from a UMA=true,
non-coherent report. D3DMetal's closed implementation does not reveal whether its
false fields are deliberate emulation policy or a stub.

Microsoft defines coherent UMA through driver-visible heap/cache behavior;
Apple's shared Metal storage still requires CPU/GPU access ordering. Neither the
physical unified memory nor successful raw custom-heap creation alone proves
the direct On12 path is valid. References:
[D3D12 ARCHITECTURE1](https://learn.microsoft.com/en-us/windows/win32/api/d3d12/ns-d3d12-d3d12_feature_data_architecture1),
[Metal shared storage](https://developer.apple.com/documentation/metal/mtlstoragemode/shared).

[2026-10-02 forced-path evidence](validation/2026-10-02-forced-uma/README.md)
confirms that the false reports originate at the D3DMetal backend boundary.
With both flags enabled, four direct initial uploads succeed per correctness run
with byte-exact RGBA8/BGRA8 shader pixels under standard synchronization and
MSYNC. The force-only, absent and malformed-value controls retain capability
gating. Automatic profile promotion and gameplay/performance qualification remain
disabled or pending; these results qualify the scoped experimental path only.

## Burst-policy hardware result

[2026-10-02 matched policy tests](validation/2026-10-02-uma-burst-policy/README.md)
with the compiled `971b374` semaphore artifact measure balanced upload-burst
medians of 5.845 ms with MSYNC and 6.623 ms with standard synchronization,
compared with legacy 5.946/6.596 ms. Same-build zero-grace controls reproduce
22.593/24.184 ms. All 72 measured runs and four prewarm runs pass byte-exact
GPU checks. Balanced now uses 48 allocations and 192 reuses instead of
180 allocations and 60 reuses. The temporary completed cache peaks near
99 MiB; final retained/pending bytes are zero. These results meet the scoped
approximately 8 ms upload target; they do not establish physical-memory savings,
PEAK FPS, or automatic-profile qualification.

The [PEAK direct startup trace](validation/2026-10-02-peak-direct-diagnostic/README.md)
uses the earlier matching `e2b99d2` bundle. It reaches D3D12 initialization and
reports `SteamAPI_Init failed` with repeated Steamworks warnings. No loader,
unhandled-crash, device-removal or MSYNC failure marker appears. A native thread
sample times out, so the exact main-thread wait remains unproven. Authenticated
Steam launch and gameplay measurement remain pending.

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
`--transfer-only` skips shader-resource-view creation and samples GPU copies
instead; it cannot qualify sampling behavior. `--continue-on-failure` records
all profiles but preserves a failing exit status. Both the test and the probe
initialize DXGI before creating a D3D12 device, as required by the tested Wine
bridge.

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

Hardware results: [2026-09-30 A18 Pro validation](validation/2026-09-30-uma-memory/README.md).
Raw GPU transfers and PEAK D3D12/MSYNC startup passed their individual checks.
The sampling/readback failures recorded there are resolved in the follow-up below.

## Follow-up validation

The core now traces completed wrapped-resource creation and ownership transitions
with their actual HRESULTs when `RELAY12_TRACE_CREATION=1`. Ownership no-ops do
not emit success markers. A traced PEAK startup that creates the On12 device but
never calls wrapped creation does not qualify that resource path.

The base frontend now supports owned RGBA8/BGRA8 Texture2D pixel SRVs, retaining
bound views and their textures through backend child references. Binding a
render target clears conflicting pixel SRVs; binding an alias of the current
render target supplies a null SRV. Other resource/view dimensions and shader
stages remain outside this slice. BGRA staging readback is also supported.

`peak_on12_probe.exe --capabilities-only` records both architecture query versions,
with poisoned output fields, and effective custom upload/readback heap properties.
The poison distinguishes an actual false report from S_OK without initialized
output. Capability reports come from the caller's D3D12 device; physical Apple
unified memory alone cannot override the driver-visible coherency contract.

Wine prefers its installed builtin `d3d11on12host.dll` over the adjacent staged
host when that module is bundled with the runtime. Both hardware runners reject
an installed host whose hash differs from the staged artifact. Test with a cloned
runtime containing the matching CI host in
`lib/wine/x86_64-windows/d3d11on12host.dll`; an adjacent copy alone is insufficient
on such runtimes. Keep the host builtin override because the Wine-built PE host
cannot load through the native-only override on the tested runtime.

[2026-10-01 integration results](validation/2026-10-01-uma-followup/README.md)
record passing RGBA8/BGRA8 sampling and wrapped-resource GPU checks under standard
synchronization and MSYNC. Direct upload remains capability-gated. The final PEAK
retest with matching host artifacts awaits Steam sign-in; gameplay performance
and physical-memory savings remain unqualified.

[2026-10-02 transfer timing results](validation/2026-10-02-uma-performance/README.md)
compare five trials per policy under both sync modes. They show no consistent
gain, and the previous build cannot complete the same workload, so they do not
establish a before/after performance change.
