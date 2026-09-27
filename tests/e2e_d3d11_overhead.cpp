/* SPDX-License-Identifier: GPL-3.0-only
 *
 * Latency and Overhead benchmarking for relay12.
 *
 * This test suite executes the core CPU-bound paths that dictate relay12's
 * translation overhead and flush latency, specifically targeting the bottlenecks
 * outlined in docs/PERFORMANCE-RESEARCH-ROADMAP.md:
 * - Empty Draw Dispatch (Command Batching limit)
 * - Map/Unmap (Memory Marshalling/UMA limit)
 * - State Changes (PSO construction limit)
 * - Flush latency (Explicit barrier/execute costs)
 *
 * It uses QueryPerformanceCounter to output nanosecond-precision durations.
 *
 * It needs a real D3D12 and the full D3D11On12 stack, so CI only compiles it
 * (the validate-d3d11on12 frontend step); the numbers CI can produce come
 * from tests/d3d11on12overhead.c against the mock driver instead.
 *
 * Build: x86_64-w64-mingw32-clang++ -std=c++17 -O2 -Wall -Wextra -Werror -fno-exceptions -fno-rtti -o e2e_d3d11_overhead.exe tests/e2e_d3d11_overhead.cpp -ld3d12 -ld3d11 -luuid
 */

#include <windows.h>
#include <d3d11.h>
#include <d3d12.h>
#include <d3d11on12.h>
#include <stdio.h>
#include <stdint.h>

static void check_hr(HRESULT hr, const char* msg) {
    if (FAILED(hr)) {
        fprintf(stderr, "FATAL: %s failed (0x%08lx)\n", msg, hr);
        ExitProcess(1);
    }
}

static uint64_t get_qpc() {
    LARGE_INTEGER li;
    QueryPerformanceCounter(&li);
    return li.QuadPart;
}

static double get_qpc_freq() {
    LARGE_INTEGER li;
    QueryPerformanceFrequency(&li);
    return (double)li.QuadPart;
}

int main() {
    double freq = get_qpc_freq();

    ID3D12Device* d3d12_device = nullptr;
    ID3D12CommandQueue* d3d12_queue = nullptr;
    ID3D11Device* d3d11_device = nullptr;
    ID3D11DeviceContext* d3d11_ctx = nullptr;

    HRESULT hr = D3D12CreateDevice(nullptr, D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(&d3d12_device));
    if (FAILED(hr)) {
        printf("SKIP: No D3D12 device available for overhead tests.\n");
        return 0;
    }

    D3D12_COMMAND_QUEUE_DESC cq_desc = {};
    cq_desc.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
    check_hr(d3d12_device->CreateCommandQueue(&cq_desc, IID_PPV_ARGS(&d3d12_queue)), "CreateCommandQueue");

    IUnknown* queues[] = { d3d12_queue };
    hr = D3D11On12CreateDevice(
        d3d12_device, 0, nullptr, 0,
        queues, 1, 0,
        &d3d11_device, &d3d11_ctx, nullptr
    );
    check_hr(hr, "D3D11On12CreateDevice");

    printf("=== D3D11On12 Latency and Overhead Benchmark ===\n");

    // 1. Draw Overhead (Command batching threshold)
    // We dispatch 100,000 empty instanced draws.
    const int DRAW_COUNT = 100000;
    uint64_t start = get_qpc();
    for (int i = 0; i < DRAW_COUNT; i++) {
        d3d11_ctx->DrawInstanced(3, 1, 0, 0);
    }
    d3d11_ctx->Flush(); // force execute
    uint64_t end = get_qpc();
    double empty_draw_ms = ((end - start) * 1000.0) / freq;
    printf("1. Empty DrawDispatch: %.2f ms (%.2f ns per draw)\n", 
           empty_draw_ms, (empty_draw_ms * 1000000.0) / DRAW_COUNT);

    // 2. Map/Unmap Overhead (Staging/UMA threshold)
    D3D11_BUFFER_DESC buf_desc = {};
    buf_desc.ByteWidth = 1024 * 1024; // 1MB
    buf_desc.Usage = D3D11_USAGE_STAGING;
    buf_desc.BindFlags = 0;
    buf_desc.CPUAccessFlags = D3D11_CPU_ACCESS_READ | D3D11_CPU_ACCESS_WRITE;
    ID3D11Buffer* staging_buf = nullptr;
    check_hr(d3d11_device->CreateBuffer(&buf_desc, nullptr, &staging_buf), "CreateBuffer (Staging)");

    const int MAP_COUNT = 1000;
    start = get_qpc();
    for (int i = 0; i < MAP_COUNT; i++) {
        D3D11_MAPPED_SUBRESOURCE mapped;
        check_hr(d3d11_ctx->Map(staging_buf, 0, D3D11_MAP_READ_WRITE, 0, &mapped), "Map (Staging)");
        d3d11_ctx->Unmap(staging_buf, 0);
    }
    end = get_qpc();
    double map_ms = ((end - start) * 1000.0) / freq;
    printf("2. Staging Map/Unmap:  %.2f ms (%.2f us per map)\n", 
           map_ms, (map_ms * 1000.0) / MAP_COUNT);

    // 3. Flush Overhead (Submit latency)
    const int FLUSH_COUNT = 5000;
    start = get_qpc();
    for (int i = 0; i < FLUSH_COUNT; i++) {
        d3d11_ctx->DrawInstanced(3, 1, 0, 0);
        d3d11_ctx->Flush();
    }
    end = get_qpc();
    double flush_ms = ((end - start) * 1000.0) / freq;
    printf("3. Explicit Flush:     %.2f ms (%.2f us per flush)\n", 
           flush_ms, (flush_ms * 1000.0) / FLUSH_COUNT);

    // 4. PSO Churn Overhead (State change threshold)
    D3D11_BLEND_DESC blend_desc = {};
    blend_desc.RenderTarget[0].BlendEnable = TRUE;
    blend_desc.RenderTarget[0].SrcBlend = D3D11_BLEND_ONE;
    blend_desc.RenderTarget[0].DestBlend = D3D11_BLEND_ZERO;
    blend_desc.RenderTarget[0].BlendOp = D3D11_BLEND_OP_ADD;
    blend_desc.RenderTarget[0].SrcBlendAlpha = D3D11_BLEND_ONE;
    blend_desc.RenderTarget[0].DestBlendAlpha = D3D11_BLEND_ZERO;
    blend_desc.RenderTarget[0].BlendOpAlpha = D3D11_BLEND_OP_ADD;
    blend_desc.RenderTarget[0].RenderTargetWriteMask = D3D11_COLOR_WRITE_ENABLE_ALL;

    ID3D11BlendState* blend_states[100] = {};
    for (int i = 0; i < 100; i++) {
        blend_desc.RenderTarget[0].SrcBlend = (D3D11_BLEND)((i % 16) + 1);
        check_hr(d3d11_device->CreateBlendState(&blend_desc, &blend_states[i]), "CreateBlendState");
    }

    const int STATE_CHANGES = 10000;
    start = get_qpc();
    for (int i = 0; i < STATE_CHANGES; i++) {
        float factor[4] = {0,0,0,0};
        d3d11_ctx->OMSetBlendState(blend_states[i % 100], factor, 0xFFFFFFFF);
        d3d11_ctx->DrawInstanced(3, 1, 0, 0);
    }
    d3d11_ctx->Flush();
    end = get_qpc();
    double state_ms = ((end - start) * 1000.0) / freq;
    printf("4. State Churn+Draw:   %.2f ms (%.2f us per change)\n", 
           state_ms, (state_ms * 1000.0) / STATE_CHANGES);

    for (int i = 0; i < 100; i++) {
        if (blend_states[i]) blend_states[i]->Release();
    }
    staging_buf->Release();
    d3d11_ctx->Release();
    d3d11_device->Release();
    d3d12_queue->Release();
    d3d12_device->Release();

    return 0;
}
