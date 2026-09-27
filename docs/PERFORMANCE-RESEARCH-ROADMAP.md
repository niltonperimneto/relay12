# Performance & Latency Research Roadmap

This roadmap covers CPU overhead in the D3D11-on-12 path: the DDI host in
`relay12-d3d11`, the pinned D3D11On12 driver, and D3D12TranslationLayer (DTL)
beneath it. Each item states what is already true of the pinned sources, what
is proposed, and what evidence decides it. Nothing here lands without a
measurement that shows it helped (see [Measurement](#measurement)).

## Where the time goes for PEAK

PEAK does **not** render through D3D11On12. Unity renders natively on D3D12,
through winecx-gptk's `d3d12shim` into D3DMetal; Relay12's D3D11On12 device
serves only Unity's On12 interop. PEAK's per-frame cost is therefore the
shim's, and the shim's hot path (the `ResourceBarrier` interposer) is tracked
in winecx-gptk rather than here. Items 1-5 pay off for workloads that actually
issue D3D11 draws through D3D11On12.

---

## Measurement

Measurement comes first, because every item below is a trade and the roadmap
previously asserted costs nobody had measured. See the "Performance
measurement" section of [`TESTS.md`](TESTS.md) for how to run each tool.

- **DDI telemetry** (`RELAY12_TELEMETRY=1`). The core wraps the draw and flush
  slots of the driver's function table with a timing proxy and reports, once at
  device destruction, draw and flush counts and average costs, the number of
  draws slower than 1 ms (the host-visible sign of a pipeline-state compile
  wait), and the number of command-list submissions -- counted from the
  driver's post-submit callback -- split into explicit and opportunistic.
  Unset, the proxies are never installed. The switch is read once per process
  through `InitOnce`.
- **Dispatch overhead benchmark** (`tests/d3d11on12overhead.c`). Runs in
  `validate-d3d11on12` against the mock driver with telemetry off and on, and
  publishes both to the job summary. Informational only: a shared runner under
  Wine is too noisy for a threshold. What does fail the job is a telemetry
  report whose counts disagree with what the benchmark dispatched.
- **Application frame rate.** `scripts/check_peak_smoke_log.py` writes a
  `frame_rate` object (mean, 1% low, minimum) into `result.json`, computed from
  the Metal HUD's frame counts and timestamps. Runs are made through Whisky,
  before and after each change.
- **End-to-end D3D11 benchmark** (`tests/e2e_d3d11_overhead.cpp`). Needs a real
  D3D12 under the full stack, so CI only compiles it.

---

## 1. Asynchronous & Persistent PSO Compilation

**Already true.** DTL compiles pipeline state objects on a thread pool:
D3D11On12's `GetImmCtxArgs` sets `UseThreadpoolForPSOCreates = true`
(`third_party/D3D11On12/src/device.cpp`). But the first draw that uses a new
PSO still blocks on it -- `PipelineState::GetForUse` waits on the thread-pool
work item (`third_party/D3D12TranslationLayer/include/PipelineState.hpp`,
`GetForUse`). There is no `ID3D12PipelineLibrary` cache, so every run
recompiles every PSO.

**Proposed.** A persistent cache: a DTL patch that loads and stores an
`ID3D12PipelineLibrary` keyed by the PSO description, under a path Whisky
provides.

**Decided by.** A probe first (`tests/peak_on12_probe.cpp`, run through
Whisky): does D3DMetal report `D3D12_FEATURE_SHADER_CACHE` support, and does
`ID3D12Device1::CreatePipelineLibrary` succeed? If not, this item is closed --
D3DMetal's own Metal shader cache may already cover it. Telemetry's
`slow_draws` is the before/after figure.

**Out of scope.** Returning a placeholder PSO and skipping draws until the real
one compiles (`DXVK_ASYNC`-style). It changes rendering output and needs an
opt-in design of its own.

## 2. Multi-Threaded Command Submission

**Already true.** DTL's `BatchedContext` records the application's calls and
replays them on a worker thread. The core passes `createDevice.Flags == 0`,
which D3D11On12 treats as "use the worker thread"
(`BatchedContextUseWorkerThread` in `third_party/D3D11On12/src/device.cpp`).
The ring this item proposed exists; there is nothing to build.

Deferred contexts use the same machinery. A command list is a DTL batch
recorded on the application's thread, and executing it appends that batch to
the immediate context's, so it is replayed on the same worker thread. The core's
command-list exports (30–34, see [`D3D11ON12.md`](D3D11ON12.md)) are shaped to
keep the calling threads cheap. Recording adds nothing per call. Warm command
lists come from recycled memory and are finished outside the frame lock.
Telemetry counts executions (`command_lists`, `execute_avg_ns`), and the
overhead benchmark times record, finish, execute and destroy.

## 3. Apple Silicon Unified Memory (UMA)

**Already true.** DTL queries `D3D12_FEATURE_ARCHITECTURE1` into
`m_architecture` (`ImmediateContext::QueryArchitectureFlags`), but nothing reads
`isUMA` or `iscacheCoherentUMA`. Staging resources always live in the
`READBACK`/`UPLOAD` heaps chosen by `Resource::GetD3D12HeapType`, and mapping a
dynamic texture for read goes through a copy (`MapDynamicTexture`).

**Proposed.** When the device reports cache-coherent UMA, place CPU-read/write
staging resources in a CPU-visible `CUSTOM` heap (`WRITE_BACK`, `L0`) instead,
removing the copy.

**Decided by.** A probe of `ARCHITECTURE1` and of `CUSTOM` heap creation with
those properties on D3DMetal. Correctness is gated by the mock driver's
byte-exact readback tests.

## 4. Command Flush Heuristics

**A bug, not a heuristic.** DTL submits a command list early when the upload
space allocated since the last submit exceeds
`MaxAllocatedUploadHeapSpacePerCommandList`
(`CommandListManager::SubmitCommandListIfNeeded`). The limit is
`min(256 MB, CreationArgs.MaxAllocatedUploadHeapSpacePerCommandList)`, and
D3D11On12's `GetImmCtxArgs` never sets that field, so it is zero-initialised.
The limit is therefore zero: the first `PostRender` after *any* upload submits
if the GPU is idle. A title that updates a dynamic constant buffer before each
draw -- the common D3D11 pattern -- can pay a submission per draw. The fix is a D3D11On12 patch that sets the
field to DTL's own 256 MB default.

**Proposed afterwards, only if measured.** Telemetry's `opportunistic_submits`
shows whether submit churn remains once the limit is fixed. Only then consider
submitting after heavy `Dispatch` or full render-target clears.

## 5. Stripping Redundant Resource Barriers

**Experiment only.** Whether D3DMetal tolerates missing transitions is unknown,
and stripping DTL's barriers risks silent corruption rather than a crash. Count
barriers per frame first; consider an environment-gated experiment only if the
count is large enough to matter, and otherwise record this item as rejected with
the numbers.
