/* SPDX-License-Identifier: GPL-3.0-only
 *
 * Vertex and pixel shader lifecycle and binding, through the core boundary.
 *
 * What this suite exists to pin is the shape of the path, not just its return
 * codes. The pinned D3D11On12 driver never fills pfnCreateVertexShader or
 * pfnCreatePixelShader on the immediate device -- FillContextDDIs assigns
 * pfnCalcPrivateShaderSize, pfnDestroyShader, pfnVsSetShader and
 * pfnPsSetShader and stops, and the only creation entry it implements is the
 * ID3D11On12DDIDevice sub-object's, whose own header calls it the shader
 * create "which take[s] the full containers instead of driver bytecode".
 * tests/d3d11on12mockdriver.c reproduces exactly that: the two table slots
 * stay NULL, so a host that reached for them would fault here rather than in
 * the field.
 *
 * The properties asserted:
 *
 *   * every argument rejection happens before any driver call, so a malformed
 *     request cannot reach the driver;
 *   * creation sizes the private block through the table, creates through the
 *     sub-object, and hands back a block the driver initialised;
 *   * the caller's container is passed through, not copied. The driver copies
 *     it inside the call, so a host-side copy would be a second one nobody
 *     frees -- and the caller's buffer is scribbled afterwards to show the
 *     host never reads it again;
 *   * a shader binds only to the stage it was created for, and a null shader
 *     unbinds instead of failing;
 *   * destruction reaches the driver once and is idempotent afterwards.
 */

#include <stdio.h>
#include <string.h>

#include "d3d11on12core.h"
#include "d3d11on12mocks.h"

static int failures;

static void check(int condition, const char *what)
{
    if (condition)
    {
        printf("[ ok ] %s\n", what);
    }
    else
    {
        printf("[fail] %s\n", what);
        ++failures;
    }
}

/* Mirrors tests/d3d11on12mockdriver.c. The size word is the handshake: the
 * mock refuses a report structure that is not the one it writes. */
struct mock_shader_report
{
    UINT size;
    LONG sizeCalls;
    LONG vertexCreateCalls;
    LONG pixelCreateCalls;
    LONG destroyCalls;
    LONG vertexSetCalls;
    LONG pixelSetCalls;
    const void *lastBytecode;
    UINT lastBytecodeSize;
    const void *lastLinkage;
    void *lastBoundVertexShader;
    void *lastBoundPixelShader;
};

typedef HRESULT (WINAPI *get_shader_report_fn)(struct mock_shader_report *);

static get_shader_report_fn get_report;

static struct mock_shader_report report(void)
{
    struct mock_shader_report out;

    memset(&out, 0, sizeof(out));
    out.size = sizeof(out);
    if (FAILED(get_report(&out)))
    {
        printf("[fail] the mock driver refused the shader report structure\n");
        ++failures;
        memset(&out, 0, sizeof(out));
    }
    return out;
}

static void initialize_shader(WineD3D11On12Shader *shader)
{
    memset(shader, 0, sizeof(*shader));
    shader->size = sizeof(*shader);
    /* Poisoned so a successful call is visibly a write. */
    shader->stage = 0xdeadbeefu;
}

/* A stand-in container. Nothing here parses DXBC -- the core passes the bytes
 * through and the driver is what would reject them -- so what matters is that
 * it is a distinct buffer at a known address and length. */
static BYTE vertex_bytecode[64];
static BYTE pixel_bytecode[48];

int main(void)
{
    WineD3D11On12AdapterDevice adapterDevice;
    WineD3D11On12Shader vertexShader;
    WineD3D11On12Shader pixelShader;
    WineD3D11On12Shader strayShader;
    struct mock_shader_report before;
    struct mock_shader_report after;
    struct mock_device device;
    struct mock_queue queue;
    IUnknown *queue_objects[1];
    HMODULE mock_driver;
    HRESULT hr;
    size_t index;

    for (index = 0; index < sizeof(vertex_bytecode); ++index)
        vertex_bytecode[index] = (BYTE)(index + 1);
    for (index = 0; index < sizeof(pixel_bytecode); ++index)
        pixel_bytecode[index] = (BYTE)(0x80 + index);

    mock_device_init(&device);
    device.support_device1 = 1;
    mock_queue_init(&queue, &device, D3D12_COMMAND_LIST_TYPE_DIRECT);
    queue_objects[0] = (IUnknown *)&queue.ID3D12CommandQueue_iface;

    /* Argument rejection first, with no opened lifecycle at all. A null
     * adapter/device pair cannot name a driver, so nothing can be called. */
    initialize_shader(&vertexShader);
    hr = WineD3D11On12CreateShaderV1(NULL, WINE_D3D11ON12_SHADER_VERTEX,
            vertex_bytecode, sizeof(vertex_bytecode), &vertexShader);
    check(hr == E_INVALIDARG, "create rejects a null adapter/device pair");
    check(vertexShader.stage == 0 && vertexShader.runtimeState == NULL,
          "a rejected create clears the caller's shader structure");

    memset(&adapterDevice, 0, sizeof(adapterDevice));
    adapterDevice.size = sizeof(adapterDevice);

    hr = WineD3D11On12CreateShaderV1(&adapterDevice,
            WINE_D3D11ON12_SHADER_VERTEX, vertex_bytecode,
            sizeof(vertex_bytecode), NULL);
    check(hr == E_INVALIDARG, "create rejects a null out-structure");

    initialize_shader(&vertexShader);
    vertexShader.size = sizeof(vertexShader) - 1;
    hr = WineD3D11On12CreateShaderV1(&adapterDevice,
            WINE_D3D11ON12_SHADER_VERTEX, vertex_bytecode,
            sizeof(vertex_bytecode), &vertexShader);
    check(hr == E_INVALIDARG, "create rejects a mismatched structure size");
    check(vertexShader.stage == 0xdeadbeefu,
          "a rejected size leaves the caller's structure untouched");

    mock_driver = LoadLibraryW(L"d3d11on12.dll");
    get_report = mock_driver ? (get_shader_report_fn)(void *)GetProcAddress(
            mock_driver, "WineD3D11On12MockDriverGetShaderReport") : NULL;
    check(get_report != NULL, "the shader mock driver is loaded");
    if (!get_report)
    {
        printf("[fail] cannot continue without the mock driver\n");
        return 1;
    }

    memset(&adapterDevice, 0, sizeof(adapterDevice));
    adapterDevice.size = sizeof(adapterDevice);
    hr = WineD3D11On12OpenAdapterV1((IUnknown *)&device.ID3D12Device_iface,
            queue_objects, 1, 0, &adapterDevice);
    check(hr == S_OK, "the adapter and device open for the shader tests");
    if (FAILED(hr))
        return 1;

    /* Still argument rejection, but now against a real lifecycle, so a leak
     * past the checks would reach the driver and show up in the counters. */
    before = report();

    initialize_shader(&strayShader);
    hr = WineD3D11On12CreateShaderV1(&adapterDevice, 2u, vertex_bytecode,
            sizeof(vertex_bytecode), &strayShader);
    check(hr == E_INVALIDARG, "create rejects an unnamed stage");

    initialize_shader(&strayShader);
    hr = WineD3D11On12CreateShaderV1(&adapterDevice,
            WINE_D3D11ON12_SHADER_VERTEX, NULL, sizeof(vertex_bytecode),
            &strayShader);
    check(hr == E_INVALIDARG, "create rejects a null container");

    initialize_shader(&strayShader);
    hr = WineD3D11On12CreateShaderV1(&adapterDevice,
            WINE_D3D11ON12_SHADER_VERTEX, vertex_bytecode, 0, &strayShader);
    check(hr == E_INVALIDARG, "create rejects a zero-length container");

    after = report();
    check(after.sizeCalls == before.sizeCalls
            && after.vertexCreateCalls == before.vertexCreateCalls
            && after.pixelCreateCalls == before.pixelCreateCalls,
          "every rejected create happens before any driver call");

    /* The vertex shader. */
    initialize_shader(&vertexShader);
    hr = WineD3D11On12CreateShaderV1(&adapterDevice,
            WINE_D3D11ON12_SHADER_VERTEX, vertex_bytecode,
            sizeof(vertex_bytecode), &vertexShader);
    check(hr == S_OK, "a vertex shader is created");
    check(vertexShader.stage == WINE_D3D11ON12_SHADER_VERTEX,
          "the created vertex shader records its stage");
    check(vertexShader.runtimeState != NULL
            && vertexShader.hDrvShader != NULL,
          "a created vertex shader publishes owned driver state");

    after = report();
    check(after.sizeCalls == before.sizeCalls + 1,
          "creation sizes the private block through the DDI table");
    check(after.vertexCreateCalls == before.vertexCreateCalls + 1
            && after.pixelCreateCalls == before.pixelCreateCalls,
          "creation reaches only the vertex stage's sub-object method");
    check(after.lastBytecode == (const void *)vertex_bytecode,
          "the driver is shown the caller's own container, not a copy");
    check(after.lastBytecodeSize == sizeof(vertex_bytecode),
          "the driver is shown the caller's own container length");
    check(after.lastLinkage == NULL,
          "no class linkage is claimed, because no interface binding exists");

    /* The pixel shader, which must get its own private block. */
    initialize_shader(&pixelShader);
    hr = WineD3D11On12CreateShaderV1(&adapterDevice,
            WINE_D3D11ON12_SHADER_PIXEL, pixel_bytecode,
            sizeof(pixel_bytecode), &pixelShader);
    check(hr == S_OK, "a pixel shader is created");
    check(pixelShader.stage == WINE_D3D11ON12_SHADER_PIXEL,
          "the created pixel shader records its stage");
    check(pixelShader.hDrvShader != NULL
            && pixelShader.hDrvShader != vertexShader.hDrvShader,
          "each shader owns a distinct private block");

    after = report();
    check(after.pixelCreateCalls == before.pixelCreateCalls + 1,
          "creation reaches the pixel stage's sub-object method");
    check(after.lastBytecode == (const void *)pixel_bytecode
            && after.lastBytecodeSize == sizeof(pixel_bytecode),
          "the pixel container is passed through unchanged");

    /* Non-retention. The driver copied inside the call, so the container is
     * the caller's to reuse the moment it returned. Everything below runs
     * against bytes no shader was built from. */
    memset(vertex_bytecode, 0xcc, sizeof(vertex_bytecode));
    memset(pixel_bytecode, 0xcc, sizeof(pixel_bytecode));

    /* Binding. */
    hr = WineD3D11On12SetShaderV1(&adapterDevice,
            WINE_D3D11ON12_SHADER_VERTEX, &vertexShader);
    check(hr == S_OK, "the vertex shader binds after its container is reused");
    hr = WineD3D11On12SetShaderV1(&adapterDevice, WINE_D3D11ON12_SHADER_PIXEL,
            &pixelShader);
    check(hr == S_OK, "the pixel shader binds after its container is reused");

    after = report();
    check(after.vertexSetCalls == before.vertexSetCalls + 1
            && after.pixelSetCalls == before.pixelSetCalls + 1,
          "each bind reaches its own stage's DDI slot exactly once");
    check(after.lastBoundVertexShader == vertexShader.hDrvShader,
          "the vertex stage is bound to the vertex shader's driver handle");
    check(after.lastBoundPixelShader == pixelShader.hDrvShader,
          "the pixel stage is bound to the pixel shader's driver handle");

    /* Cross-stage binding, which the driver would accept as well-formed. */
    hr = WineD3D11On12SetShaderV1(&adapterDevice,
            WINE_D3D11ON12_SHADER_VERTEX, &pixelShader);
    check(hr == E_INVALIDARG,
          "a pixel shader is refused on the vertex stage");
    hr = WineD3D11On12SetShaderV1(&adapterDevice, WINE_D3D11ON12_SHADER_PIXEL,
            &vertexShader);
    check(hr == E_INVALIDARG,
          "a vertex shader is refused on the pixel stage");

    hr = WineD3D11On12SetShaderV1(&adapterDevice, 2u, &vertexShader);
    check(hr == E_INVALIDARG, "binding to an unnamed stage is refused");

    initialize_shader(&strayShader);
    hr = WineD3D11On12SetShaderV1(&adapterDevice,
            WINE_D3D11ON12_SHADER_VERTEX, &strayShader);
    check(hr == E_INVALIDARG, "binding a shader that was never created is "
          "refused");

    before = report();
    check(before.vertexSetCalls == after.vertexSetCalls
            && before.pixelSetCalls == after.pixelSetCalls,
          "every refused bind happens before the driver is called");

    /* Unbinding, which is a null shader rather than an error. */
    hr = WineD3D11On12SetShaderV1(&adapterDevice,
            WINE_D3D11ON12_SHADER_VERTEX, NULL);
    check(hr == S_OK, "a null shader unbinds the vertex stage");
    after = report();
    check(after.vertexSetCalls == before.vertexSetCalls + 1
            && after.lastBoundVertexShader == NULL,
          "unbinding reaches the driver with a zeroed handle");

    /* Destruction. */
    before = report();
    hr = WineD3D11On12DestroyShaderV1(&adapterDevice, &vertexShader);
    check(hr == S_OK, "the vertex shader is destroyed");
    check(vertexShader.runtimeState == NULL && vertexShader.hDrvShader == NULL
            && vertexShader.stage == 0,
          "destruction clears every owned output");

    after = report();
    check(after.destroyCalls == before.destroyCalls + 1,
          "destruction reaches the driver's initialised block once");

    hr = WineD3D11On12DestroyShaderV1(&adapterDevice, &vertexShader);
    check(hr == S_OK, "a repeated destroy is idempotent");
    before = report();
    check(before.destroyCalls == after.destroyCalls,
          "a repeated destroy invokes no driver callback");

    hr = WineD3D11On12DestroyShaderV1(NULL, &pixelShader);
    check(hr == E_INVALIDARG,
          "destroy rejects a null adapter/device pair");
    hr = WineD3D11On12DestroyShaderV1(&adapterDevice, NULL);
    check(hr == E_INVALIDARG, "destroy rejects a null shader structure");

    hr = WineD3D11On12DestroyShaderV1(&adapterDevice, &pixelShader);
    check(hr == S_OK, "the pixel shader is destroyed");
    after = report();
    check(after.destroyCalls == before.destroyCalls + 1,
          "destroying the pixel shader reaches the driver once");

    /* Shaders must be destroyable before the device that made them, and the
     * device must still close cleanly afterwards. */
    hr = WineD3D11On12CloseAdapterDeviceV1(&adapterDevice);
    check(hr == S_OK, "the lifecycle closes after its shaders are destroyed");

    /* With the lifecycle closed, the pair no longer names a driver. */
    initialize_shader(&strayShader);
    hr = WineD3D11On12CreateShaderV1(&adapterDevice,
            WINE_D3D11ON12_SHADER_VERTEX, vertex_bytecode,
            sizeof(vertex_bytecode), &strayShader);
    check(hr == E_INVALIDARG, "create refuses a closed adapter/device pair");

    /* Closing a lifecycle whose shaders the caller never released. The host
     * owns those blocks, so it is the one that must release them -- and it
     * must tell the driver before the device they belong to goes away. */
    memset(&adapterDevice, 0, sizeof(adapterDevice));
    adapterDevice.size = sizeof(adapterDevice);
    hr = WineD3D11On12OpenAdapterV1((IUnknown *)&device.ID3D12Device_iface,
            queue_objects, 1, 0, &adapterDevice);
    check(hr == S_OK, "the adapter reopens for the abandoned-shader case");

    initialize_shader(&vertexShader);
    hr = WineD3D11On12CreateShaderV1(&adapterDevice,
            WINE_D3D11ON12_SHADER_VERTEX, vertex_bytecode,
            sizeof(vertex_bytecode), &vertexShader);
    check(hr == S_OK, "a vertex shader is created on the reopened device");
    initialize_shader(&pixelShader);
    hr = WineD3D11On12CreateShaderV1(&adapterDevice,
            WINE_D3D11ON12_SHADER_PIXEL, pixel_bytecode,
            sizeof(pixel_bytecode), &pixelShader);
    check(hr == S_OK, "a pixel shader is created on the reopened device");

    before = report();
    hr = WineD3D11On12CloseAdapterDeviceV1(&adapterDevice);
    check(hr == S_OK, "the lifecycle closes with shaders still outstanding");
    after = report();
    check(after.destroyCalls == before.destroyCalls + 2,
          "closing destroys both abandoned shaders through the driver");

    /* The caller's stale structures are unreachable rather than dangerous:
     * every shader entry point needs the pair that close just cleared, so a
     * late destroy cannot reach the freed block. */
    hr = WineD3D11On12DestroyShaderV1(&adapterDevice, &vertexShader);
    check(hr == E_INVALIDARG,
          "destroying an abandoned shader after close is refused, not a "
          "double free");
    hr = WineD3D11On12SetShaderV1(&adapterDevice,
            WINE_D3D11ON12_SHADER_VERTEX, &pixelShader);
    check(hr == E_INVALIDARG, "binding after close is refused");

    if (mock_driver)
        FreeLibrary(mock_driver);

    if (failures)
    {
        printf("[fail] %d check(s) failed\n", failures);
        return 1;
    }
    printf("[ ok ] the shader lifecycle owns, binds, and releases its "
           "shaders\n");
    return 0;
}
