import re

with open('relay12-d3d11/d3d11on12core.cpp', 'r') as f:
    text = f.read()

# 1. Add realDeviceFuncs and telemetry counters to AdapterState
adapter_state_orig = """    RenderTargetState *renderTargets;
    alignas(8) unsigned char privateDevice[1];"""

adapter_state_new = """    RenderTargetState *renderTargets;
    D3DWDDM2_6DDI_DEVICEFUNCS realDeviceFuncs;
    volatile LONG64 tele_draw_count, tele_draw_ns;
    volatile LONG64 tele_flush_count, tele_flush_ns;
    alignas(8) unsigned char privateDevice[1];"""

text = text.replace(adapter_state_orig, adapter_state_new)

# 2. Inject Proxies and QPC helpers before createDriverDevice
proxies = """
static uint64_t get_qpc_nanoseconds() {
    LARGE_INTEGER li;
    QueryPerformanceCounter(&li);
    LARGE_INTEGER freq;
    QueryPerformanceFrequency(&freq);
    return (li.QuadPart * 1000000000ULL) / freq.QuadPart;
}

static VOID APIENTRY Proxy_pfnDraw(D3D10DDI_HDEVICE hDevice, UINT VertexCount, UINT StartVertexLocation) {
    AdapterState* state = reinterpret_cast<AdapterState*>(
        static_cast<unsigned char*>(hDevice.pDrvPrivate) - offsetof(AdapterState, privateDevice));
    uint64_t start = get_qpc_nanoseconds();
    state->realDeviceFuncs.pfnDraw(hDevice, VertexCount, StartVertexLocation);
    uint64_t end = get_qpc_nanoseconds();
    InterlockedAdd64(&state->tele_draw_ns, end - start);
    InterlockedIncrement64(&state->tele_draw_count);
}

static VOID APIENTRY Proxy_pfnDrawIndexed(D3D10DDI_HDEVICE hDevice, UINT IndexCount, UINT StartIndexLocation, INT BaseVertexLocation) {
    AdapterState* state = reinterpret_cast<AdapterState*>(
        static_cast<unsigned char*>(hDevice.pDrvPrivate) - offsetof(AdapterState, privateDevice));
    uint64_t start = get_qpc_nanoseconds();
    state->realDeviceFuncs.pfnDrawIndexed(hDevice, IndexCount, StartIndexLocation, BaseVertexLocation);
    uint64_t end = get_qpc_nanoseconds();
    InterlockedAdd64(&state->tele_draw_ns, end - start);
    InterlockedIncrement64(&state->tele_draw_count);
}

static VOID APIENTRY Proxy_pfnDrawInstanced(D3D10DDI_HDEVICE hDevice, UINT VertexCountPerInstance, UINT InstanceCount, UINT StartVertexLocation, UINT StartInstanceLocation) {
    AdapterState* state = reinterpret_cast<AdapterState*>(
        static_cast<unsigned char*>(hDevice.pDrvPrivate) - offsetof(AdapterState, privateDevice));
    uint64_t start = get_qpc_nanoseconds();
    state->realDeviceFuncs.pfnDrawInstanced(hDevice, VertexCountPerInstance, InstanceCount, StartVertexLocation, StartInstanceLocation);
    uint64_t end = get_qpc_nanoseconds();
    InterlockedAdd64(&state->tele_draw_ns, end - start);
    InterlockedIncrement64(&state->tele_draw_count);
}

static VOID APIENTRY Proxy_pfnDrawIndexedInstanced(D3D10DDI_HDEVICE hDevice, UINT IndexCountPerInstance, UINT InstanceCount, UINT StartIndexLocation, INT BaseVertexLocation, UINT StartInstanceLocation) {
    AdapterState* state = reinterpret_cast<AdapterState*>(
        static_cast<unsigned char*>(hDevice.pDrvPrivate) - offsetof(AdapterState, privateDevice));
    uint64_t start = get_qpc_nanoseconds();
    state->realDeviceFuncs.pfnDrawIndexedInstanced(hDevice, IndexCountPerInstance, InstanceCount, StartIndexLocation, BaseVertexLocation, StartInstanceLocation);
    uint64_t end = get_qpc_nanoseconds();
    InterlockedAdd64(&state->tele_draw_ns, end - start);
    InterlockedIncrement64(&state->tele_draw_count);
}

static VOID APIENTRY Proxy_pfnFlush(D3D10DDI_HDEVICE hDevice) {
    AdapterState* state = reinterpret_cast<AdapterState*>(
        static_cast<unsigned char*>(hDevice.pDrvPrivate) - offsetof(AdapterState, privateDevice));
    uint64_t start = get_qpc_nanoseconds();
    state->realDeviceFuncs.pfnFlush(hDevice);
    uint64_t end = get_qpc_nanoseconds();
    InterlockedAdd64(&state->tele_flush_ns, end - start);
    InterlockedIncrement64(&state->tele_flush_count);
}

HRESULT createDriverDevice"""

text = text.replace("HRESULT createDriverDevice", proxies)

# 3. Hook the proxies after CreateDevice succeeds
create_dev_orig = """    if (!state->deviceFuncs.pfnDestroyDevice)
        return DXGI_ERROR_UNSUPPORTED;
    state->deviceCreated = true;

    out->negotiatedInterfaceVersion = selectedInterface;"""

create_dev_new = """    if (!state->deviceFuncs.pfnDestroyDevice)
        return DXGI_ERROR_UNSUPPORTED;
    state->deviceCreated = true;

    // Telemetry injection
    state->realDeviceFuncs = state->deviceFuncs;
    if (state->deviceFuncs.pfnDraw) state->deviceFuncs.pfnDraw = Proxy_pfnDraw;
    if (state->deviceFuncs.pfnDrawIndexed) state->deviceFuncs.pfnDrawIndexed = Proxy_pfnDrawIndexed;
    if (state->deviceFuncs.pfnDrawInstanced) state->deviceFuncs.pfnDrawInstanced = Proxy_pfnDrawInstanced;
    if (state->deviceFuncs.pfnDrawIndexedInstanced) state->deviceFuncs.pfnDrawIndexedInstanced = Proxy_pfnDrawIndexedInstanced;
    if (state->deviceFuncs.pfnFlush) state->deviceFuncs.pfnFlush = Proxy_pfnFlush;

    out->negotiatedInterfaceVersion = selectedInterface;"""

text = text.replace(create_dev_orig, create_dev_new)

# 4. Print telemetry report on destruction
destroy_orig = """    if (state->adapterOpened && state->adapterFuncs.pfnCloseAdapter)
        state->adapterFuncs.pfnCloseAdapter(state->hAdapter);"""

destroy_new = """    if (state->deviceCreated) {
        char report[512];
        double draw_avg = state->tele_draw_count ? (double)state->tele_draw_ns / state->tele_draw_count : 0.0;
        double flush_avg = state->tele_flush_count ? (double)state->tele_flush_ns / state->tele_flush_count : 0.0;
        snprintf(report, sizeof(report), 
            "d3d11on12core telemetry: [Draws: %lld total, %.2fns avg cost] | [Flushes: %lld total, %.2fns avg cost]\\n",
            (long long)state->tele_draw_count, draw_avg,
            (long long)state->tele_flush_count, flush_avg);
        wineD3D11DiagReport(report);
    }
    
    if (state->adapterOpened && state->adapterFuncs.pfnCloseAdapter)
        state->adapterFuncs.pfnCloseAdapter(state->hAdapter);"""

text = text.replace(destroy_orig, destroy_new)

with open('relay12-d3d11/d3d11on12core.cpp', 'w') as f:
    f.write(text)
