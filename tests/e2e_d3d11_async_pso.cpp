/* SPDX-License-Identifier: GPL-3.0-only
 *
 * Deterministic test for D3D11ON12_COMPAT_NonBlockingPSOs (dtl patch 0023,
 * d3d11on12 patches 0026 and 0027).
 *
 * Whether a pipeline is still compiling is normally a race.  This test
 * decides it: it swaps the CreateGraphicsPipelineState and
 * CreateComputePipelineState slots of the caller's ID3D12Device vtable for
 * wrappers that block on an event before calling the real function.  The
 * translation layer creates pipelines on its thread pool, so the block lands
 * there and the test controls when each pipeline becomes ready.
 *
 * With the switch on, it asserts:
 *   1. while the graphics pipeline is held, a clear followed by a draw leaves
 *      the target at the clear colour: the draw was skipped;
 *   2. the calling thread never blocked while doing so (a watchdog turns a
 *      hang into a failure);
 *   3. once the pipeline is released, the same draw renders;
 *   4. a dispatch whose compute pipeline is held still runs once it is
 *      released: compute is never skipped.
 * With the switch off, the held draw is rendered after the release, as
 * upstream always does.
 *
 * It needs a real D3D12 under the full D3D11On12 stack.  CI compiles it; it
 * runs through Whisky on D3DMetal or wherever such a stack exists.  A
 * dxgi.dll that exports CompatValue (Windows) ignores the environment switch,
 * and this test then reports that the switch had no effect.
 *
 * Build: x86_64-w64-mingw32-clang++ -std=c++17 -O2 -Wall -Wextra -Werror -fno-exceptions -fno-rtti -o e2e_d3d11_async_pso.exe tests/e2e_d3d11_async_pso.cpp -ld3d12 -ld3d11 -ld3dcompiler -luuid
 */

#include <windows.h>
#include <initguid.h>
#include <d3d11.h>
#include <d3d12.h>
#include <d3d11on12.h>
#include <d3dcompiler.h>
#include <stdio.h>
#include <string.h>

namespace {

const UINT TARGET_SIZE = 64;
const FLOAT CLEAR_COLOUR[4] = {1.0f, 0.0f, 0.0f, 1.0f};
const UINT CLEAR_TEXEL = 0xff0000ffu;  /* R8G8B8A8 red, little-endian */
const UINT DRAW_TEXEL = 0xff00ff00u;   /* green */
const UINT COMPUTE_VALUE = 0xc0ffeeu;

/* Long enough that a hang is not a slow machine; short enough for CI. */
const DWORD WATCHDOG_MS = 20000;
const DWORD READY_TIMEOUT_MS = 10000;
/* How long a held compute pipeline stays held. */
const DWORD COMPUTE_HOLD_MS = 300;

/* Fullscreen triangle from the vertex ID: no input layout, no buffers. */
const char VERTEX_SHADER[] =
    "float4 main(uint id : SV_VertexID) : SV_POSITION\n"
    "{\n"
    "    float2 uv = float2((id << 1) & 2, id & 2);\n"
    "    return float4(uv * float2(2, -2) + float2(-1, 1), 0, 1);\n"
    "}\n";

const char PIXEL_SHADER[] =
    "float4 main() : SV_TARGET { return float4(0, 1, 0, 1); }\n";

const char COMPUTE_SHADER[] =
    "RWBuffer<uint> output : register(u0);\n"
    "[numthreads(1, 1, 1)] void main() { output[0] = 0xc0ffee; }\n";

/* ID3D12Device vtable order: IUnknown (0-2), ID3D12Object (3-6),
 * GetNodeCount 7, CreateCommandQueue 8, CreateCommandAllocator 9. */
const size_t GRAPHICS_PSO_SLOT = 10;
const size_t COMPUTE_PSO_SLOT = 11;

typedef HRESULT (STDMETHODCALLTYPE *CreateGraphicsFn)(ID3D12Device *,
        const D3D12_GRAPHICS_PIPELINE_STATE_DESC *, REFIID, void **);
typedef HRESULT (STDMETHODCALLTYPE *CreateComputeFn)(ID3D12Device *,
        const D3D12_COMPUTE_PIPELINE_STATE_DESC *, REFIID, void **);

CreateGraphicsFn realCreateGraphics;
CreateComputeFn realCreateCompute;
/* Manual-reset: set means "not held". */
HANDLE graphicsGate;
HANDLE computeGate;
volatile LONG graphicsCreates;
volatile LONG computeCreates;

HRESULT STDMETHODCALLTYPE heldCreateGraphics(ID3D12Device *device,
        const D3D12_GRAPHICS_PIPELINE_STATE_DESC *desc, REFIID iid, void **out)
{
    InterlockedIncrement(&graphicsCreates);
    WaitForSingleObject(graphicsGate, INFINITE);
    return realCreateGraphics(device, desc, iid, out);
}

HRESULT STDMETHODCALLTYPE heldCreateCompute(ID3D12Device *device,
        const D3D12_COMPUTE_PIPELINE_STATE_DESC *desc, REFIID iid, void **out)
{
    InterlockedIncrement(&computeCreates);
    WaitForSingleObject(computeGate, INFINITE);
    return realCreateCompute(device, desc, iid, out);
}

bool patchSlot(void **vtable, size_t slot, void *replacement, void **original)
{
    DWORD protection;
    if (!VirtualProtect(&vtable[slot], sizeof(void *), PAGE_READWRITE, &protection))
        return false;
    *original = vtable[slot];
    vtable[slot] = replacement;
    VirtualProtect(&vtable[slot], sizeof(void *), protection, &protection);
    return true;
}

HANDLE watchdogArmed;
HANDLE watchdogDone;
const char *watchdogPhase = "";

DWORD WINAPI watchdog(void *)
{
    for (;;)
    {
        WaitForSingleObject(watchdogArmed, INFINITE);
        if (WaitForSingleObject(watchdogDone, WATCHDOG_MS) != WAIT_OBJECT_0)
        {
            fprintf(stderr, "FAIL: the calling thread blocked during '%s'\n",
                    watchdogPhase);
            fflush(stderr);
            ExitProcess(2);
        }
    }
}

void arm(const char *phase)
{
    watchdogPhase = phase;
    ResetEvent(watchdogDone);
    SetEvent(watchdogArmed);
}

void disarm()
{
    SetEvent(watchdogDone);
}

DWORD WINAPI releaseComputeLater(void *)
{
    Sleep(COMPUTE_HOLD_MS);
    SetEvent(computeGate);
    return 0;
}

bool compile(const char *source, const char *target, ID3DBlob **blob)
{
    ID3DBlob *errors = NULL;
    HRESULT hr = D3DCompile(source, strlen(source), NULL, NULL, NULL, "main",
            target, 0, 0, blob, &errors);
    if (FAILED(hr))
    {
        fprintf(stderr, "FAIL: D3DCompile(%s) 0x%08lx %s\n", target, hr,
                errors ? (const char *)errors->GetBufferPointer() : "");
    }
    if (errors)
        errors->Release();
    return SUCCEEDED(hr);
}

struct Frame
{
    ID3D11Device *device = NULL;
    ID3D11DeviceContext *context = NULL;
    ID3D11Texture2D *target = NULL;
    ID3D11RenderTargetView *view = NULL;
    ID3D11Texture2D *staging = NULL;
    ID3D11VertexShader *vs = NULL;
    ID3D11PixelShader *ps = NULL;
    ID3D11ComputeShader *cs = NULL;
    ID3D11Buffer *buffer = NULL;
    ID3D11UnorderedAccessView *uav = NULL;
    ID3D11Buffer *bufferStaging = NULL;

    ~Frame()
    {
        IUnknown *objects[] = {bufferStaging, uav, buffer, cs, ps, vs, staging,
                               view, target, context, device};
        for (IUnknown *object : objects)
            if (object)
                object->Release();
    }
};

#define CHECK(expr)                                                         \
    do {                                                                    \
        HRESULT check_hr = (expr);                                          \
        if (FAILED(check_hr)) {                                             \
            fprintf(stderr, "FAIL: %s 0x%08lx\n", #expr, check_hr);         \
            return false;                                                   \
        }                                                                   \
    } while (0)

bool createFrame(ID3D12Device *d3d12, ID3D12CommandQueue *queue,
        ID3DBlob *vsBlob, ID3DBlob *psBlob, ID3DBlob *csBlob, Frame &frame)
{
    IUnknown *queues[] = {queue};
    CHECK(D3D11On12CreateDevice(d3d12, 0, NULL, 0, queues, 1, 0,
            &frame.device, &frame.context, NULL));

    D3D11_TEXTURE2D_DESC texture = {};
    texture.Width = texture.Height = TARGET_SIZE;
    texture.MipLevels = texture.ArraySize = 1;
    texture.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    texture.SampleDesc.Count = 1;
    texture.Usage = D3D11_USAGE_DEFAULT;
    texture.BindFlags = D3D11_BIND_RENDER_TARGET;
    CHECK(frame.device->CreateTexture2D(&texture, NULL, &frame.target));
    CHECK(frame.device->CreateRenderTargetView(frame.target, NULL, &frame.view));
    texture.Usage = D3D11_USAGE_STAGING;
    texture.BindFlags = 0;
    texture.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    CHECK(frame.device->CreateTexture2D(&texture, NULL, &frame.staging));

    CHECK(frame.device->CreateVertexShader(vsBlob->GetBufferPointer(),
            vsBlob->GetBufferSize(), NULL, &frame.vs));
    CHECK(frame.device->CreatePixelShader(psBlob->GetBufferPointer(),
            psBlob->GetBufferSize(), NULL, &frame.ps));
    CHECK(frame.device->CreateComputeShader(csBlob->GetBufferPointer(),
            csBlob->GetBufferSize(), NULL, &frame.cs));

    D3D11_BUFFER_DESC buffer = {};
    buffer.ByteWidth = 16;
    buffer.Usage = D3D11_USAGE_DEFAULT;
    buffer.BindFlags = D3D11_BIND_UNORDERED_ACCESS;
    CHECK(frame.device->CreateBuffer(&buffer, NULL, &frame.buffer));
    D3D11_UNORDERED_ACCESS_VIEW_DESC uav = {};
    uav.Format = DXGI_FORMAT_R32_UINT;
    uav.ViewDimension = D3D11_UAV_DIMENSION_BUFFER;
    uav.Buffer.NumElements = 4;
    CHECK(frame.device->CreateUnorderedAccessView(frame.buffer, &uav, &frame.uav));
    buffer.Usage = D3D11_USAGE_STAGING;
    buffer.BindFlags = 0;
    buffer.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    CHECK(frame.device->CreateBuffer(&buffer, NULL, &frame.bufferStaging));

    D3D11_VIEWPORT viewport = {0, 0, (FLOAT)TARGET_SIZE, (FLOAT)TARGET_SIZE, 0, 1};
    frame.context->RSSetViewports(1, &viewport);
    frame.context->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    frame.context->VSSetShader(frame.vs, NULL, 0);
    frame.context->PSSetShader(frame.ps, NULL, 0);
    frame.context->OMSetRenderTargets(1, &frame.view, NULL);
    return true;
}

/* Clear, draw, and read back the centre texel.  Only the readback waits. */
bool clearDrawRead(Frame &frame, UINT *texel)
{
    frame.context->ClearRenderTargetView(frame.view, CLEAR_COLOUR);
    frame.context->Draw(3, 0);
    frame.context->CopyResource(frame.staging, frame.target);
    D3D11_MAPPED_SUBRESOURCE mapped;
    CHECK(frame.context->Map(frame.staging, 0, D3D11_MAP_READ, 0, &mapped));
    const BYTE *row = (const BYTE *)mapped.pData + (TARGET_SIZE / 2) * mapped.RowPitch;
    memcpy(texel, row + (TARGET_SIZE / 2) * 4, sizeof(*texel));
    frame.context->Unmap(frame.staging, 0);
    return true;
}

/* Keep drawing until the texel is the draw colour, or time out. */
bool waitForDraw(Frame &frame)
{
    ULONGLONG deadline = GetTickCount64() + READY_TIMEOUT_MS;
    UINT texel = 0;
    do
    {
        if (!clearDrawRead(frame, &texel))
            return false;
        if (texel == DRAW_TEXEL)
            return true;
        Sleep(10);
    } while (GetTickCount64() < deadline);
    fprintf(stderr, "FAIL: draw never rendered once its pipeline was ready "
            "(texel 0x%08x)\n", texel);
    return false;
}

bool dispatchRuns(Frame &frame)
{
    UINT zero[4] = {};
    frame.context->ClearUnorderedAccessViewUint(frame.uav, zero);
    frame.context->CSSetShader(frame.cs, NULL, 0);
    frame.context->CSSetUnorderedAccessViews(0, 1, &frame.uav, NULL);
    const LONG before = computeCreates;
    ResetEvent(computeGate);
    HANDLE releaser = CreateThread(NULL, 0, releaseComputeLater, NULL, 0, NULL);
    if (!releaser)
        return false;
    frame.context->Dispatch(1, 1, 1);
    frame.context->CopyResource(frame.bufferStaging, frame.buffer);
    D3D11_MAPPED_SUBRESOURCE mapped;
    HRESULT hr = frame.context->Map(frame.bufferStaging, 0, D3D11_MAP_READ, 0, &mapped);
    WaitForSingleObject(releaser, INFINITE);
    CloseHandle(releaser);
    SetEvent(computeGate);
    if (FAILED(hr))
    {
        fprintf(stderr, "FAIL: Map(compute readback) 0x%08lx\n", hr);
        return false;
    }
    UINT value = *(const UINT *)mapped.pData;
    frame.context->Unmap(frame.bufferStaging, 0);
    if (computeCreates == before)
    {
        fprintf(stderr, "FAIL: no compute pipeline was created, so the hold "
                "tested nothing\n");
        return false;
    }
    if (value != COMPUTE_VALUE)
    {
        fprintf(stderr, "FAIL: a dispatch with a held compute pipeline was "
                "skipped (0x%08x)\n", value);
        return false;
    }
    return true;
}

bool run(ID3D12Device *d3d12, ID3D12CommandQueue *queue, ID3DBlob *vs,
        ID3DBlob *ps, ID3DBlob *cs, bool nonBlocking)
{
    SetEnvironmentVariableA("D3D11ON12_COMPAT_NonBlockingPSOs",
            nonBlocking ? "1" : NULL);
    Frame frame;
    if (!createFrame(d3d12, queue, vs, ps, cs, frame))
        return false;

    ResetEvent(graphicsGate);
    const LONG before = graphicsCreates;
    UINT texel = 0;
    bool ok;
    if (nonBlocking)
    {
        arm("draw with a held graphics pipeline");
        ok = clearDrawRead(frame, &texel);
        disarm();
        if (ok && graphicsCreates == before)
        {
            fprintf(stderr, "FAIL: no graphics pipeline was created, so the "
                    "hold tested nothing\n");
            ok = false;
        }
        if (ok && texel == DRAW_TEXEL)
        {
            fprintf(stderr, "FAIL: the draw rendered while its pipeline was "
                    "held; D3D11ON12_COMPAT_NonBlockingPSOs had no effect\n");
            ok = false;
        }
        if (ok && texel != CLEAR_TEXEL)
        {
            fprintf(stderr, "FAIL: expected the clear colour, read 0x%08x\n", texel);
            ok = false;
        }
        SetEvent(graphicsGate);
        ok = ok && waitForDraw(frame);
        ok = ok && dispatchRuns(frame);
    }
    else
    {
        /* Blocking is upstream's behaviour: the draw waits for the release,
         * then renders on the first attempt. */
        HANDLE releaser = CreateThread(NULL, 0, [](void *) -> DWORD {
            Sleep(COMPUTE_HOLD_MS);
            SetEvent(graphicsGate);
            return 0;
        }, NULL, 0, NULL);
        ok = releaser && clearDrawRead(frame, &texel);
        if (releaser)
        {
            WaitForSingleObject(releaser, INFINITE);
            CloseHandle(releaser);
        }
        SetEvent(graphicsGate);
        if (ok && graphicsCreates == before)
        {
            fprintf(stderr, "FAIL: no graphics pipeline was created, so the "
                    "hold tested nothing\n");
            ok = false;
        }
        if (ok && texel != DRAW_TEXEL)
        {
            fprintf(stderr, "FAIL: with the switch off the draw was not "
                    "rendered (0x%08x)\n", texel);
            ok = false;
        }
    }
    SetEnvironmentVariableA("D3D11ON12_COMPAT_NonBlockingPSOs", NULL);
    printf("%s: switch %s\n", ok ? "PASS" : "FAIL", nonBlocking ? "on" : "off");
    return ok;
}

} // namespace

int main()
{
    ID3D12Device *d3d12 = NULL;
    if (FAILED(D3D12CreateDevice(NULL, D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(&d3d12))))
    {
        printf("SKIP: no D3D12 device\n");
        return 0;
    }
    ID3D12CommandQueue *queue = NULL;
    D3D12_COMMAND_QUEUE_DESC queueDesc = {};
    queueDesc.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
    if (FAILED(d3d12->CreateCommandQueue(&queueDesc, IID_PPV_ARGS(&queue))))
    {
        fprintf(stderr, "FAIL: CreateCommandQueue\n");
        return 1;
    }

    ID3DBlob *vs = NULL, *ps = NULL, *cs = NULL;
    if (!compile(VERTEX_SHADER, "vs_5_0", &vs) || !compile(PIXEL_SHADER, "ps_5_0", &ps)
            || !compile(COMPUTE_SHADER, "cs_5_0", &cs))
        return 1;

    graphicsGate = CreateEventW(NULL, TRUE, TRUE, NULL);
    computeGate = CreateEventW(NULL, TRUE, TRUE, NULL);
    watchdogArmed = CreateEventW(NULL, FALSE, FALSE, NULL);
    watchdogDone = CreateEventW(NULL, TRUE, FALSE, NULL);
    if (!graphicsGate || !computeGate || !watchdogArmed || !watchdogDone
            || !CreateThread(NULL, 0, watchdog, NULL, 0, NULL))
        return 1;

    void **vtable = *reinterpret_cast<void ***>(d3d12);
    if (!patchSlot(vtable, GRAPHICS_PSO_SLOT, (void *)heldCreateGraphics,
                (void **)&realCreateGraphics)
            || !patchSlot(vtable, COMPUTE_PSO_SLOT, (void *)heldCreateCompute,
                (void **)&realCreateCompute))
    {
        fprintf(stderr, "FAIL: could not patch the ID3D12Device vtable\n");
        return 1;
    }

    bool ok = run(d3d12, queue, vs, ps, cs, true);
    ok = run(d3d12, queue, vs, ps, cs, false) && ok;

    void *unused;
    patchSlot(vtable, GRAPHICS_PSO_SLOT, (void *)realCreateGraphics, &unused);
    patchSlot(vtable, COMPUTE_PSO_SLOT, (void *)realCreateCompute, &unused);
    cs->Release();
    ps->Release();
    vs->Release();
    queue->Release();
    d3d12->Release();
    return ok ? 0 : 1;
}
