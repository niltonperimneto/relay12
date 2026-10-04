/* SPDX-License-Identifier: GPL-3.0-only
 *
 * Synthetic D3D11-on-12 benchmark for real D3D12 (D3DMetal through Whisky).
 *
 * Each scenario isolates one change from docs/ROADMAP.md
 * and renders real pixels, so a result cannot come from work the stack
 * quietly dropped:
 *
 *   draws   2000 draws a frame from one static vertex buffer, rebound at a
 *           new offset every eight draws.  Mostly batch recording and the
 *           handoff to DTL's worker: the ring against the semaphore
 *           (dtl 0022/0024).
 *   upload  64 draws a frame, each from a vertex buffer created that frame
 *           with initial data, which DTL copies through its upload heap:
 *           streamed geometry.  What the upload-heap submit limit
 *           (d3d11on12 0025) is about.
 *   pso     200 draws a frame, the first two of which use pixel shaders no
 *           pipeline has been built for yet.  What non-blocking PSOs
 *           (dtl 0023, d3d11on12 0026/0027) are about.  Each process mixes a
 *           nonce into its shaders, so D3DMetal's persistent shader cache can
 *           never make a later run cheaper than an earlier one.
 *
 * A frame ends the way a presenting game's does: the target is copied to one
 * of three staging textures, the context is flushed, and the copy from two
 * frames back is mapped, which bounds the GPU to two frames in flight.  Each
 * frame records the CPU time spent issuing it and the interval since the
 * previous frame ended.  The last frame of every trial is read back and its
 * centre must hold the colour the last draw wrote.
 *
 * Only operations Relay12 routes are used: no constant buffers, shader
 * resource views or dynamic-resource maps, which it rejects.  Geometry and
 * colour come from vertex buffers.
 *
 * Every trial creates its own D3D12 and D3D11On12 devices and destroys them,
 * so with RELAY12_TELEMETRY=1 (which this program sets) the core and the
 * driver report each trial's counters as it ends; the "[bench] trial end"
 * marker on stderr precedes them.  --nonblocking sets
 * D3D11ON12_COMPAT_NonBlockingPSOs=1 before any device exists.
 *
 * Results are one JSON object per trial, appended to --out and echoed to
 * stdout.  Trials interleave scenarios so drift over the run spreads evenly.
 *
 * A small window is opened and its messages pumped before any device
 * exists, and every frame: D3DMetal's initialisation waited forever in a
 * windowless process, while the same runtime started PEAK, which opens its
 * window first.  Nothing is presented to it.
 *
 * It needs a real D3D12 under the full D3D11On12 stack, so CI only compiles
 * it; the numbers CI can produce come from tests/d3d11on12overhead.c against
 * the mock driver instead.
 *
 * Usage: e2e_d3d11_overhead.exe [--scenario all|draws|upload|pso]
 *            [--frames N] [--trials N] [--nonblocking] [--label TEXT]
 *            [--out PATH]
 *
 * Build: x86_64-w64-mingw32-clang++ -std=c++17 -O2 -Wall -Wextra -Werror -fno-exceptions -fno-rtti -o e2e_d3d11_overhead.exe tests/e2e_d3d11_overhead.cpp -ld3d12 -ldxgi -ld3dcompiler -luuid -luser32
 */

#include <windows.h>
#include <initguid.h>
#include <d3d11.h>
#include <d3d12.h>
#include <d3d11on12.h>
#include <dxgi1_4.h>
#include <d3dcompiler.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

namespace {

const UINT TARGET_SIZE = 256;
const int WARMUP_FRAMES = 30;
const int STAGING_COUNT = 3;
const int MAX_FRAMES = 10000;

enum Scenario { SCENARIO_DRAWS, SCENARIO_UPLOAD, SCENARIO_PSO, SCENARIO_COUNT };
const char *const SCENARIO_NAMES[SCENARIO_COUNT] = {"draws", "upload", "pso"};
const int DRAWS_PER_FRAME[SCENARIO_COUNT] = {2000, 64, 200};
/* Triangles in the static vertex buffer, the last of which is the centre's. */
const int GRID_TRIANGLES = 2001;
const int NEW_PIPELINES_PER_FRAME = 2;

const char VERTEX_SHADER[] =
    "struct input { float2 position : POSITION; float4 colour : COLOR; };\n"
    "struct output { float4 position : SV_Position; float4 colour : COLOR; };\n"
    "output main(input i)\n"
    "{\n"
    "    output o;\n"
    "    o.position = float4(i.position, 0.0, 1.0);\n"
    "    o.colour = i.colour;\n"
    "    return o;\n"
    "}\n";

/* The two %.10e are the process nonce and the variant, each an integer
 * below 2^24 scaled by 1e-9: exact enough as floats that every variant's
 * bytecode, and so its pipeline, is distinct, and far too small to move an
 * 8-bit channel. */
const char PIXEL_SHADER_FORMAT[] =
    "struct input { float4 position : SV_Position; float4 colour : COLOR; };\n"
    "float4 main(input i) : SV_Target\n"
    "{\n"
    "    return saturate(i.colour + float4(%.10e, %.10e, 0.0, 0.0));\n"
    "}\n";

struct Vertex
{
    float position[2];
    float colour[4];
};

/* The last draw of every frame covers the centre in green, and the readback
 * checks for it; no other triangle is green or reaches the centre. */
const BYTE CENTRE_RGBA[4] = {0x00, 0xff, 0x00, 0xff};

struct Options
{
    bool run[SCENARIO_COUNT] = {true, true, true};
    int frames = 300;
    int trials = 5;
    bool nonblocking = false;
    const char *label = "";
    const char *out = NULL;
};

template <typename T>
void release(T *&object)
{
    if (object)
        object->Release();
    object = NULL;
}

bool failed(const char *what, HRESULT hr)
{
    if (SUCCEEDED(hr))
        return false;
    printf("[fail] %s: hr=0x%08lx\n", what, (unsigned long)hr);
    return true;
}

ID3DBlob *compile(const char *source, const char *target)
{
    ID3DBlob *code = NULL;
    ID3DBlob *errors = NULL;
    HRESULT hr = D3DCompile(source, strlen(source), NULL, NULL, NULL, "main",
            target, D3DCOMPILE_OPTIMIZATION_LEVEL3, 0, &code, &errors);
    if (FAILED(hr))
        printf("[fail] compiling %s: %s\n", target,
                errors ? (const char *)errors->GetBufferPointer() : "no log");
    release(errors);
    return SUCCEEDED(hr) ? code : NULL;
}

double qpc_ms(LONGLONG ticks)
{
    static LONGLONG frequency;
    if (!frequency)
    {
        LARGE_INTEGER value;
        QueryPerformanceFrequency(&value);
        frequency = value.QuadPart;
    }
    return (double)ticks * 1000.0 / (double)frequency;
}

LONGLONG now()
{
    LARGE_INTEGER value;
    QueryPerformanceCounter(&value);
    return value.QuadPart;
}

int compare_doubles(const void *a, const void *b)
{
    double x = *(const double *)a, y = *(const double *)b;
    return x < y ? -1 : x > y ? 1 : 0;
}

struct Summary
{
    double mean, median, p95, p99, max;
};

Summary summarize(const double *values, int count)
{
    static double sorted[MAX_FRAMES];
    Summary s = {};
    if (count <= 0)
        return s;
    memcpy(sorted, values, sizeof(double) * count);
    qsort(sorted, count, sizeof(double), compare_doubles);
    double total = 0.0;
    for (int i = 0; i < count; ++i)
        total += sorted[i];
    auto at = [&](double q) { return sorted[(int)(q * (count - 1) + 0.5)]; };
    s.mean = total / count;
    s.median = at(0.5);
    s.p95 = at(0.95);
    s.p99 = at(0.99);
    s.max = sorted[count - 1];
    return s;
}

void write_summary(FILE *file, const char *name, const Summary &s)
{
    fprintf(file, "\"%s\":{\"mean\":%.4f,\"median\":%.4f,\"p95\":%.4f,\"p99\":%.4f,\"max\":%.4f}",
            name, s.mean, s.median, s.p95, s.p99, s.max);
}

/* Everything one trial owns, released in reverse order at the end. */
struct Trial
{
    ID3D12Device *device12 = NULL;
    ID3D12CommandQueue *queue = NULL;
    ID3D11Device *device = NULL;
    ID3D11DeviceContext *context = NULL;
    ID3D11Texture2D *target = NULL;
    ID3D11RenderTargetView *view = NULL;
    ID3D11Texture2D *staging[STAGING_COUNT] = {};
    ID3D11VertexShader *vs = NULL;
    ID3D11PixelShader *ps = NULL;
    ID3D11PixelShader **variants = NULL;
    int variant_count = 0;
    ID3D11InputLayout *layout = NULL;
    ID3D11Buffer *grid = NULL;

    ~Trial()
    {
        for (int i = 0; i < variant_count; ++i)
            release(variants[i]);
        free(variants);
        release(grid);
        release(layout);
        release(ps);
        release(vs);
        for (int i = 0; i < STAGING_COUNT; ++i)
            release(staging[i]);
        release(view);
        release(target);
        if (context)
        {
            context->ClearState();
            context->Flush();
        }
        release(context);
        release(device);
        release(queue);
        release(device12);
    }
};

using CreateOn12 = decltype(&D3D11On12CreateDevice);

CreateOn12 on12_entry()
{
    /* Apple's d3d11.dll exports D3D11On12CreateDevice as a stub; the d3d12
     * interposer patches it to reach Relay12 when RELAY12_EXPERIMENTAL_FRAME
     * is set.  Checking for d3d11on12core afterwards confirms it did. */
    HMODULE d3d11 = LoadLibraryW(L"d3d11.dll");
    FARPROC address = d3d11 ? GetProcAddress(d3d11, "D3D11On12CreateDevice") : NULL;
    CreateOn12 create = NULL;
    memcpy(&create, &address, sizeof(create));
    return create;
}

ID3D11Buffer *vertex_buffer(ID3D11Device *device, const Vertex *vertices, UINT count)
{
    D3D11_BUFFER_DESC desc = {};
    desc.ByteWidth = (UINT)sizeof(Vertex) * count;
    desc.Usage = D3D11_USAGE_DEFAULT;
    desc.BindFlags = D3D11_BIND_VERTEX_BUFFER;
    D3D11_SUBRESOURCE_DATA data = {vertices, 0, 0};
    ID3D11Buffer *buffer = NULL;
    if (failed("CreateBuffer(vertices)", device->CreateBuffer(&desc, &data, &buffer)))
        return NULL;
    return buffer;
}

/* Triangle `index` of the grid: small, off-centre and never green.  The
 * centre triangle is green and covers the middle of the target. */
void grid_triangle(int index, Vertex out[3])
{
    const float corners[3][2] = {{-1.0f, -1.0f}, {0.0f, 1.0f}, {1.0f, -1.0f}};
    bool centre = index == GRID_TRIANGLES - 1;
    /* Cells in a 19x19 grid, skipping the middle one. */
    int cell = index % 360;
    cell += cell >= 180;
    float x = centre ? 0.0f : -0.9f + 0.1f * (float)(cell % 19);
    float y = centre ? 0.0f : -0.9f + 0.1f * (float)(cell / 19);
    float scale = centre ? 0.5f : 0.04f;
    for (int v = 0; v < 3; ++v)
    {
        out[v].position[0] = x + corners[v][0] * scale;
        out[v].position[1] = y + corners[v][1] * scale;
        out[v].colour[0] = centre ? 0.0f : 1.0f;
        out[v].colour[1] = centre ? 1.0f : 0.0f;
        out[v].colour[2] = centre ? 0.0f : (float)(index % 7) / 7.0f;
        out[v].colour[3] = 1.0f;
    }
}

/* Engines create a DXGI factory and pick an adapter before touching D3D12,
 * and D3DMetal depends on it: a D3D12CreateDevice with no factory made first
 * never returns from its WineOS::Init. */
HRESULT create_d3d12_device(ID3D12Device **device)
{
    IDXGIFactory4 *factory = NULL;
    IDXGIAdapter1 *adapter = NULL;
    HRESULT hr = CreateDXGIFactory1(IID_IDXGIFactory4, reinterpret_cast<void **>(&factory));
    if (failed("CreateDXGIFactory1", hr))
        return hr;
    hr = factory->EnumAdapters1(0, &adapter);
    if (!failed("EnumAdapters1", hr))
        hr = D3D12CreateDevice(adapter, D3D_FEATURE_LEVEL_11_0, IID_ID3D12Device,
                reinterpret_cast<void **>(device));
    release(adapter);
    release(factory);
    return hr;
}

bool setup(Trial &t, Scenario scenario, int frames, unsigned nonce, int trial)
{
    HRESULT hr = create_d3d12_device(&t.device12);
    if (failed("D3D12CreateDevice", hr))
        return false;
    D3D12_COMMAND_QUEUE_DESC queue_desc = {};
    queue_desc.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
    hr = t.device12->CreateCommandQueue(&queue_desc, IID_ID3D12CommandQueue,
            reinterpret_cast<void **>(&t.queue));
    if (failed("CreateCommandQueue", hr))
        return false;

    CreateOn12 create = on12_entry();
    if (!create)
    {
        printf("[fail] d3d11.dll exports no D3D11On12CreateDevice\n");
        return false;
    }
    const D3D_FEATURE_LEVEL level = D3D_FEATURE_LEVEL_11_0;
    IUnknown *queues[] = {t.queue};
    hr = create(t.device12, 0, &level, 1, queues, 1, 0, &t.device, &t.context, NULL);
    if (failed("D3D11On12CreateDevice", hr))
        return false;
    if (!GetModuleHandleW(L"d3d11on12core.dll"))
    {
        printf("[fail] the device did not come from Relay12 (d3d11on12core.dll not loaded)\n");
        return false;
    }

    D3D11_TEXTURE2D_DESC texture = {};
    texture.Width = texture.Height = TARGET_SIZE;
    texture.MipLevels = texture.ArraySize = 1;
    texture.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    texture.SampleDesc.Count = 1;
    texture.Usage = D3D11_USAGE_DEFAULT;
    texture.BindFlags = D3D11_BIND_RENDER_TARGET;
    if (failed("CreateTexture2D(target)", t.device->CreateTexture2D(&texture, NULL, &t.target))
            || failed("CreateRenderTargetView", t.device->CreateRenderTargetView(t.target, NULL, &t.view)))
        return false;
    texture.Usage = D3D11_USAGE_STAGING;
    texture.BindFlags = 0;
    texture.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    for (int i = 0; i < STAGING_COUNT; ++i)
        if (failed("CreateTexture2D(staging)", t.device->CreateTexture2D(&texture, NULL, &t.staging[i])))
            return false;

    ID3DBlob *vs_code = compile(VERTEX_SHADER, "vs_5_0");
    if (!vs_code)
        return false;
    hr = t.device->CreateVertexShader(vs_code->GetBufferPointer(), vs_code->GetBufferSize(), NULL, &t.vs);
    if (!failed("CreateVertexShader", hr))
    {
        const D3D11_INPUT_ELEMENT_DESC elements[] = {
            {"POSITION", 0, DXGI_FORMAT_R32G32_FLOAT, 0, 0, D3D11_INPUT_PER_VERTEX_DATA, 0},
            {"COLOR", 0, DXGI_FORMAT_R32G32B32A32_FLOAT, 0, 8, D3D11_INPUT_PER_VERTEX_DATA, 0},
        };
        hr = t.device->CreateInputLayout(elements, 2, vs_code->GetBufferPointer(),
                vs_code->GetBufferSize(), &t.layout);
        failed("CreateInputLayout", hr);
    }
    release(vs_code);
    if (FAILED(hr))
        return false;

    /* Variant 0 is the base shader every scenario draws with.  The pso
     * scenario also creates every variant it will need up front, so shader
     * creation stays outside the timed frames and only pipeline creation
     * lands inside them. */
    int needed = 1 + (scenario == SCENARIO_PSO ? frames * NEW_PIPELINES_PER_FRAME : 0);
    t.variants = (ID3D11PixelShader **)calloc(needed, sizeof(*t.variants));
    if (!t.variants)
        return false;
    for (int i = 0; i < needed; ++i)
    {
        char source[sizeof(PIXEL_SHADER_FORMAT) + 64];
        double variant = (double)(trial * 65536 + i);
        snprintf(source, sizeof(source), PIXEL_SHADER_FORMAT, (double)nonce * 1e-9, variant * 1e-9);
        ID3DBlob *code = compile(source, "ps_5_0");
        if (!code)
            return false;
        hr = t.device->CreatePixelShader(code->GetBufferPointer(), code->GetBufferSize(), NULL, &t.variants[i]);
        release(code);
        if (failed("CreatePixelShader", hr))
            return false;
        t.variant_count = i + 1;
    }
    t.ps = t.variants[0];
    t.ps->AddRef();

    static Vertex grid[GRID_TRIANGLES * 3];
    for (int i = 0; i < GRID_TRIANGLES; ++i)
        grid_triangle(i, &grid[i * 3]);
    if (!(t.grid = vertex_buffer(t.device, grid, GRID_TRIANGLES * 3)))
        return false;

    D3D11_VIEWPORT viewport = {0.0f, 0.0f, (float)TARGET_SIZE, (float)TARGET_SIZE, 0.0f, 1.0f};
    t.context->RSSetViewports(1, &viewport);
    t.context->OMSetRenderTargets(1, &t.view, NULL);
    t.context->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    t.context->IASetInputLayout(t.layout);
    t.context->VSSetShader(t.vs, NULL, 0);
    t.context->PSSetShader(t.ps, NULL, 0);
    return true;
}

/* One frame's draws.  `recorded` is the index among timed frames, or -1
 * during warm-up, which must not consume new pipelines. */
bool issue(Trial &t, Scenario scenario, int recorded)
{
    ID3D11DeviceContext *c = t.context;
    const UINT stride = sizeof(Vertex);
    const int draws = DRAWS_PER_FRAME[scenario];
    UINT offset = 0;
    switch (scenario)
    {
    case SCENARIO_DRAWS:
        for (int i = 0; i < draws; ++i)
        {
            /* Rebinding at the draw's own offset every eight draws is the
             * state traffic an engine's per-mesh binding produces. */
            if (i % 8 == 0)
            {
                offset = (UINT)(i * 3) * stride;
                c->IASetVertexBuffers(0, 1, &t.grid, &stride, &offset);
            }
            c->Draw(3, (UINT)(i % 8) * 3);
        }
        break;
    case SCENARIO_UPLOAD:
    {
        ID3D11Buffer *streamed[64] = {};
        bool ok = true;
        for (int i = 0; i < draws && ok; ++i)
        {
            Vertex triangle[3];
            grid_triangle(i + (recorded < 0 ? 0 : recorded * 7) % 1000, triangle);
            if (!(streamed[i] = vertex_buffer(t.device, triangle, 3)))
            {
                ok = false;
                break;
            }
            c->IASetVertexBuffers(0, 1, &streamed[i], &stride, &offset);
            c->Draw(3, 0);
        }
        for (int i = 0; i < draws; ++i)
            release(streamed[i]);
        if (!ok)
            return false;
        break;
    }
    case SCENARIO_PSO:
        c->IASetVertexBuffers(0, 1, &t.grid, &stride, &offset);
        for (int i = 0; i < draws; ++i)
        {
            if (recorded >= 0 && i < NEW_PIPELINES_PER_FRAME)
                c->PSSetShader(t.variants[1 + recorded * NEW_PIPELINES_PER_FRAME + i], NULL, 0);
            else if (recorded >= 0 && i == NEW_PIPELINES_PER_FRAME)
                c->PSSetShader(t.ps, NULL, 0);
            c->Draw(3, (UINT)i * 3);
        }
        break;
    default:
        return false;
    }
    /* The centre draw is always last and always on the base pipeline, which
     * warm-up has built, so the readback holds even when a new pipeline's
     * draws were skipped. */
    c->PSSetShader(t.ps, NULL, 0);
    offset = 0;
    c->IASetVertexBuffers(0, 1, &t.grid, &stride, &offset);
    c->Draw(3, (UINT)(GRID_TRIANGLES - 1) * 3);
    return true;
}

void pump_messages()
{
    MSG message;
    while (PeekMessageW(&message, NULL, 0, 0, PM_REMOVE))
    {
        TranslateMessage(&message);
        DispatchMessageW(&message);
    }
}

HWND open_window()
{
    WNDCLASSW window_class = {};
    window_class.lpfnWndProc = DefWindowProcW;
    window_class.hInstance = GetModuleHandleW(NULL);
    window_class.lpszClassName = L"relay12_bench";
    RegisterClassW(&window_class);
    HWND window = CreateWindowExW(0, L"relay12_bench", L"Relay12 D3D11On12 benchmark",
            WS_OVERLAPPEDWINDOW, CW_USEDEFAULT, CW_USEDEFAULT, 320, 240,
            NULL, NULL, window_class.hInstance, NULL);
    if (window)
    {
        ShowWindow(window, SW_SHOWNOACTIVATE);
        UpdateWindow(window);
        pump_messages();
    }
    return window;
}

bool check_centre(Trial &t, int staging)
{
    D3D11_MAPPED_SUBRESOURCE mapped;
    if (failed("Map(readback)", t.context->Map(t.staging[staging], 0, D3D11_MAP_READ, 0, &mapped)))
        return false;
    const BYTE *centre = (const BYTE *)mapped.pData + (TARGET_SIZE / 2) * mapped.RowPitch + (TARGET_SIZE / 2) * 4;
    bool ok = memcmp(centre, CENTRE_RGBA, 4) == 0;
    if (!ok)
        printf("[fail] centre texel is %02x%02x%02x%02x, expected 00ff00ff\n",
                centre[0], centre[1], centre[2], centre[3]);
    t.context->Unmap(t.staging[staging], 0);
    return ok;
}

bool run_trial(const Options &options, Scenario scenario, int trial, unsigned nonce, FILE *out)
{
    static double frame_ms[MAX_FRAMES], cpu_ms[MAX_FRAMES];
    const int frames = options.frames;
    bool ok = false, verified = false;
    {
        Trial t;
        fprintf(stderr, "[bench] trial begin scenario=%s trial=%d\n", SCENARIO_NAMES[scenario], trial);
        if (!setup(t, scenario, frames, nonce, trial))
            goto done;

        {
            const float clear[4] = {0.0f, 0.0f, 1.0f, 1.0f};
            LONGLONG previous_end = 0;
            int total = WARMUP_FRAMES + frames;
            int f;
            for (f = 0; f < total; ++f)
            {
                int recorded = f - WARMUP_FRAMES;
                pump_messages();
                LONGLONG start = now();
                t.context->ClearRenderTargetView(t.view, clear);
                if (!issue(t, scenario, recorded))
                    goto done;
                t.context->CopyResource(t.staging[f % STAGING_COUNT], t.target);
                t.context->Flush();
                LONGLONG issued = now();
                if (f >= 2)
                {
                    D3D11_MAPPED_SUBRESOURCE mapped;
                    int oldest = (f - 2) % STAGING_COUNT;
                    if (failed("Map(pacing)", t.context->Map(t.staging[oldest], 0, D3D11_MAP_READ, 0, &mapped)))
                        goto done;
                    t.context->Unmap(t.staging[oldest], 0);
                }
                LONGLONG end = now();
                if (recorded >= 0)
                {
                    cpu_ms[recorded] = qpc_ms(issued - start);
                    frame_ms[recorded] = qpc_ms(end - previous_end);
                }
                previous_end = end;
            }
            verified = check_centre(t, (f - 1) % STAGING_COUNT);
        }
        ok = true;
done:
        fprintf(stderr, "[bench] trial end scenario=%s trial=%d\n", SCENARIO_NAMES[scenario], trial);
    }
    /* The devices are gone by here, so the driver's telemetry for this
     * trial has been written, and it sits between the two markers. */
    fprintf(stderr, "[bench] trial released scenario=%s trial=%d\n", SCENARIO_NAMES[scenario], trial);
    if (!ok)
        return false;

    Summary frame = summarize(frame_ms, frames), cpu = summarize(cpu_ms, frames);
    FILE *targets[2] = {stdout, out};
    for (FILE *file : targets)
    {
        if (!file)
            continue;
        fprintf(file, "{\"label\":\"%s\",\"scenario\":\"%s\",\"trial\":%d,\"nonblocking\":%s,"
                "\"frames\":%d,\"draws_per_frame\":%d,\"verified\":%s,",
                options.label, SCENARIO_NAMES[scenario], trial, options.nonblocking ? "true" : "false",
                frames, DRAWS_PER_FRAME[scenario] + 1, verified ? "true" : "false");
        write_summary(file, "frame_ms", frame);
        fputc(',', file);
        write_summary(file, "cpu_ms", cpu);
        fputs(",\"frame_ms_raw\":[", file);
        for (int i = 0; i < frames; ++i)
            fprintf(file, "%s%.4f", i ? "," : "", frame_ms[i]);
        fputs("]}\n", file);
        fflush(file);
    }
    return verified;
}

bool parse(int argc, char **argv, Options &options)
{
    for (int i = 1; i < argc; ++i)
    {
        const char *arg = argv[i];
        const char *value = i + 1 < argc ? argv[i + 1] : NULL;
        if (!strcmp(arg, "--nonblocking"))
            options.nonblocking = true;
        else if (!strcmp(arg, "--scenario") && value)
        {
            ++i;
            if (strcmp(value, "all"))
            {
                bool known = false;
                for (int s = 0; s < SCENARIO_COUNT; ++s)
                {
                    options.run[s] = !strcmp(value, SCENARIO_NAMES[s]);
                    known |= options.run[s];
                }
                if (!known)
                    return false;
            }
        }
        else if (!strcmp(arg, "--frames") && value)
            options.frames = atoi(argv[++i]);
        else if (!strcmp(arg, "--trials") && value)
            options.trials = atoi(argv[++i]);
        else if (!strcmp(arg, "--label") && value)
            options.label = argv[++i];
        else if (!strcmp(arg, "--out") && value)
            options.out = argv[++i];
        else
            return false;
    }
    return options.frames > 0 && options.frames <= MAX_FRAMES && options.trials > 0;
}

}

int main(int argc, char **argv)
{
    setvbuf(stdout, NULL, _IONBF, 0);
    Options options;
    if (!parse(argc, argv, options))
    {
        printf("usage: e2e_d3d11_overhead.exe [--scenario all|draws|upload|pso] [--frames N]"
                " [--trials N] [--nonblocking] [--label TEXT] [--out PATH]\n");
        return 2;
    }

    HWND window = open_window();
    if (!window)
        printf("[bench] no window could be opened; continuing without one\n");

    /* Both are read once, at or before the first device's creation. */
    SetEnvironmentVariableA("RELAY12_TELEMETRY", "1");
    SetEnvironmentVariableA("D3D11ON12_COMPAT_NonBlockingPSOs", options.nonblocking ? "1" : NULL);

    ID3D12Device *probe = NULL;
    if (FAILED(create_d3d12_device(&probe)))
    {
        printf("SKIP: no D3D12 device\n");
        return 0;
    }
    release(probe);

    FILE *out = NULL;
    if (options.out && !(out = fopen(options.out, "a")))
    {
        printf("[fail] cannot open %s\n", options.out);
        return 1;
    }
    unsigned nonce = (unsigned)((GetTickCount64() ^ (GetCurrentProcessId() * 2654435761u)) % 1000u);
    printf("[bench] label=%s nonblocking=%d frames=%d trials=%d nonce=%u\n",
            options.label, options.nonblocking ? 1 : 0, options.frames, options.trials, nonce);

    int failures = 0;
    for (int trial = 0; trial < options.trials; ++trial)
    {
        /* Rotate the starting scenario so no scenario always runs first. */
        for (int k = 0; k < SCENARIO_COUNT; ++k)
        {
            Scenario s = (Scenario)((trial + k) % SCENARIO_COUNT);
            if (options.run[s] && !run_trial(options, s, trial, nonce, out))
                ++failures;
        }
    }
    if (out)
        fclose(out);
    if (window)
        DestroyWindow(window);
    printf("[bench] done failures=%d\n", failures);
    return failures ? 1 : 0;
}
