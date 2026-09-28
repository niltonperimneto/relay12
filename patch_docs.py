with open("docs/PERFORMANCE-RESEARCH-ROADMAP.md", "r") as f:
    text = f.read()

insertion = """
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
"""

# Replace the first `---` with `insertion` followed by `---`
text = text.replace("---\n\n## 1. Asynchronous", insertion + "\n\n---\n\n## 1. Asynchronous", 1)

with open("docs/PERFORMANCE-RESEARCH-ROADMAP.md", "w") as f:
    f.write(text)
