# Performance & Latency Research Roadmap

Because `relay12` operates as a specialized architectural bridge rather than a monolithic translation engine like DXVK, its latency and overhead characteristics are entirely unique. While it benefits from near-zero CPU state tracking (handled natively by `d3d11.dll`) and zero GPU emulation (handled directly by D3DMetal), the layer introduces overhead specifically at the **Command Batching**, **Pipeline State Caching**, and **Memory Marshalling** boundaries.

The following represent the highest-impact research vectors for minimizing latency and pushing `relay12`'s CPU overhead ceiling.


---

## Core Overhead Characterization (The DDI Telemetry Insights)

Based on direct nanosecond telemetry gathered from `relay12`'s DDI hooks (see `tests/e2e_d3d11_overhead.cpp` and `feat/ddi-telemetry-tracing`), the translation layer's CPU latency falls strictly into three measurable categories. We define this overhead not as inefficient code, but as the fundamental **API impedance mismatch**—the processing tax of safely boxing flexible, unconstrained D3D11 logic into D3D12's strict, explicit, and multi-threaded architecture from within a single, synchronous game thread.

### 1. Synchronous State Churn (The "Draw" Overhead)
* **The Penalty:** Direct3D 11 is an immediate-mode API (states change arbitrarily up until `Draw()`). Direct3D 12 requires all states to be baked into a monolithic Pipeline State Object (PSO). Every time `pfnDraw` is intercepted, `D3D11On12` must evaluate the fragmented state, hash it, and perform a cache lookup. If the engine supplies a new combination, the CPU thread **completely stalls** to synchronously compile a new D3D12 PSO.
* **Impact:** High peak latency in specific draws (micro-stutter).

### 2. Command Serialization (The "Batching" Overhead)
* **The Penalty:** Because the game engine believes it is talking to a D3D11 driver, it commands `relay12` synchronously on a single thread. The CPU overhead measured in draws includes the literal immediate-to-deferred translation cost of `D3D12TranslationLayer` iteratively converting DDI tokens into `ID3D12GraphicsCommandList::DrawInstanced` COM calls.
* **Impact:** Robs the game engine's main thread of CPU cycles that would otherwise go to physics or AI execution.

### 3. Barrier Injection & Serialization (The "Flush" Overhead)
* **The Penalty:** D3D11 drivers implicitly manage read/write resource syncing; D3D12 requires explicit `ResourceBarrier` commands. `D3D11On12` defensively generates hundreds of these explicit transition boundaries per frame. When `pfnFlush` is called, the CPU pays the heavy price of resolving the dependency graph, writing the barriers, closing the list, and crossing the COM boundary to execute the queue.
* **Impact:** A heavy base-layer CPU tax (lowering overall average FPS) as the game thread halts waiting for the queue submission bounds to finalize.


---

## 1. Asynchronous & Persistent PSO Compilation
Right now, if `D3D11On12` encounters a new combination of shaders, blend states, and input layouts, it must instantaneously weave and compile a new Direct3D 12 Pipeline State Object (PSO) entirely on the game's main render thread, causing micro-stutter.

### Research Goals
- **Asynchronous Compilation:** Investigate patching `D3D11On12` or implementing a WDDM DDI intercept that allows for asynchronous pipeline compilation. By returning an empty/dummy PSO to the game until the real compilation finishes on a background worker thread, we could eliminate loading-screen and traversal stutters (similar to `DXVK_ASYNC`).
- **Disk Caching:** Explore intercepting the `GetCache` DDI to serialize and store `D3D11On12`'s built pipelines to macOS disk setups, allowing `relay12` to instantly reload PSOs across application restarts.

## 2. Multi-Threaded Command Submission (CS Worker Thread)
Because `d3d11.dll` handles state tracking synchronously, `relay12` currently generates and submits all D3D12 Command Lists directly on the calling application thread.

### Research Goals
- **Lock-Free Command Rings:** Research building a dedicated **Command Submission (CS) Thread** beneath the DDI layer. The game thread would rapidly write raw commands into a lock-free ring buffer and immediately return, allowing the engine to proceed natively while the dedicated worker thread translates those WDDM commands into D3D12 lists and submits them to D3DMetal independently.

## 3. Apple Silicon Unified Memory Exploitation (UMA)
D3D11 maps memory under the assumption of discrete PCIe bandwidth (VRAM vs. System RAM), and D3D12 cleanly enforces Readback/Upload heaps. However, Apple Silicon utilizes a massive, physically Unified Memory Architecture (UMA).

### Research Goals
- **Eliminating Staging Copies:** When a D3D11 game requests CPU-readback of an active texture, `relay12` currently halts, syncs the D3D12 queue, and copies the data to a staging heap. We should research D3DMetal's `D3D12_HEAP_TYPE_CUSTOM` implementations to map Metal's shared memory pages directly into the game. By mapping memory as both CPU-visible and GPU-visible simultaneously, we could eliminate entire classes of WDDM staging operations.

## 4. Smart Command Flush Heuristics
Because D3D11 is an immediate-mode API, games do not explicitly declare when to execute GPU queues. `relay12` must defer commands until it feels it is "safe" or optimal to execute them, or until `Flush()` is explicitly called. Delaying flushes minimizes CPU overhead by maximizing batching, but hurts latency. Continual flushing minimizes latency but starves the CPU.

### Research Goals
- **Contextual Render Boundaries:** Train `relay12` to identify specific RenderPass logic boundaries inside the raw command stream to construct heuristic execution triggers. For example, explicitly triggering a D3D12 execute queue following heavy Compute Shader Dispatches (`Dispatch`) or massive frame-clears (`ClearRenderTargetView`), rather than waiting for an arbitrary byte-limit on the command queue to be reached.

## 5. Stripping Redundant Resource Barriers
`D3D11On12` is highly defensive and generates hundreds of explicit D3D12 `ResourceBarrier` transition commands per frame to guarantee safety across API calls. However, modern Metal drivers on macOS implicitly track their own dependency graphs and physical resource barriers under the hood.

### Research Goals
- **Barrier Filtering:** Measure what happens if `relay12` actively strips or filters particular D3D12 state transitions (such as `D3D12_RESOURCE_STATE_COMMON` returns) before they are serialized and passed to D3DMetal. If D3DMetal intrinsically enforces its own bounding on Apple Silicon, stripping these explicit barrier commands from `relay12`'s lists could trivially trim significant CPU translation overhead with no loss of stability.
