/* SPDX-License-Identifier: GPL-3.0-only
 * Constant-buffer binding, buffer map/unmap families, and buffer and
 * Texture2D updates, read back byte for byte from the mock driver. */
#define COBJMACROS
#include <windows.h>
#include <d3d12.h>
#include <stdio.h>
#include <string.h>
#include "d3d11on12core.h"
#include "d3d11on12mocks.h"

static int failures;
#define CHECK(x) do { if (!(x)) { printf("[fail] line %d: %s\n", __LINE__, #x); ++failures; } } while (0)

enum { FAMILY_CB_DISCARD = 1, FAMILY_CB_NO_OVERWRITE, FAMILY_IA_DISCARD,
        FAMILY_IA_NO_OVERWRITE, FAMILY_RESOURCE_DISCARD, FAMILY_STAGING };

static void *(WINAPI *bound_cb)(UINT, UINT);
static void (WINAPI *record)(LONG *, LONG *, LONG *, LONG *, LONG *, LONG *, LONG *, LONG *, int *, int *);
static SIZE_T (WINAPI *read_resource)(void *, void *, SIZE_T);

static HRESULT open_adapter(struct mock_device *device, struct mock_queue *queue,
        WineD3D11On12AdapterDevice *out)
{
    IUnknown *queues[1];

    mock_device_init(device);
    device->support_device1 = 1;
    mock_queue_init(queue, device, D3D12_COMMAND_LIST_TYPE_DIRECT);
    queues[0] = (IUnknown *)&queue->ID3D12CommandQueue_iface;
    out->size = sizeof(*out);
    return WineD3D11On12OpenAdapterV1((IUnknown *)&device->ID3D12Device_iface, queues, 1, 0, out);
}

static HRESULT make_buffer(WineD3D11On12AdapterDevice *device, UINT bytes, D3D11_USAGE usage,
        UINT bind, UINT cpu, const void *initial, WineD3D11On12Buffer *out)
{
    D3D11_BUFFER_DESC desc = {bytes, usage, bind, cpu, 0, 0};
    D3D11_SUBRESOURCE_DATA data = {initial, 0, 0};

    out->size = sizeof(*out);
    return WineD3D11On12CreateBufferV1(device, &desc, initial ? &data : NULL, out);
}

/* Eight threads each create a dynamic constant buffer, bind it to their own
 * slot in both stages, map, write, unmap, update a shared buffer, and destroy
 * their buffer while it is still bound -- so destruction's unbind races the
 * other threads' binds, maps and updates on the same device. */
struct storm
{
    WineD3D11On12AdapterDevice *device;
    WineD3D11On12Buffer *shared;
    HANDLE start;
    volatile LONG unexpected;
    LONG next_slot;
};

static DWORD WINAPI storm_thread(void *param)
{
    struct storm *storm = param;
    const UINT slot = (UINT)InterlockedIncrement(&storm->next_slot) % 14;
    BYTE data[16] = {0};
    UINT i;

    WaitForSingleObject(storm->start, INFINITE);
    for (i = 0; i < 200; ++i)
    {
        WineD3D11On12Buffer buffer = {0}, *bind = &buffer;
        D3D11_MAPPED_SUBRESOURCE mapped = {0};

        if (make_buffer(storm->device, 16, D3D11_USAGE_DYNAMIC, D3D11_BIND_CONSTANT_BUFFER,
                D3D11_CPU_ACCESS_WRITE, NULL, &buffer) != S_OK
                || WineD3D11On12SetConstantBuffersV1(storm->device, WINE_D3D11ON12_SHADER_VERTEX,
                        slot, 1, &bind) != S_OK
                || WineD3D11On12SetConstantBuffersV1(storm->device, WINE_D3D11ON12_SHADER_PIXEL,
                        slot, 1, &bind) != S_OK
                || WineD3D11On12MapBufferV1(storm->device, &buffer, D3D11_MAP_WRITE_DISCARD, 0,
                        &mapped) != S_OK || !mapped.pData)
            InterlockedIncrement(&storm->unexpected);
        if (mapped.pData) memset(mapped.pData, (int)i, 16);
        if ((mapped.pData && WineD3D11On12UnmapBufferV1(storm->device, &buffer) != S_OK)
                || WineD3D11On12UpdateBufferV1(storm->device, storm->shared, NULL, data, 0, 0) != S_OK
                || WineD3D11On12DestroyBufferV1(&buffer) != S_OK)
            InterlockedIncrement(&storm->unexpected);
    }
    return 0;
}

/* Map, check the family the core chose, write a pattern, unmap through the
 * matching family, and read the bytes back from the driver's storage. */
static void map_write_unmap(WineD3D11On12AdapterDevice *device, WineD3D11On12Buffer *buffer,
        D3D11_MAP mode, LONG want_family, LONG want_unmap, BYTE pattern, UINT bytes)
{
    D3D11_MAPPED_SUBRESOURCE mapped = {0};
    LONG family = 0, unmap_family = 0, type = 0;
    BYTE back[64] = {0};
    UINT i;

    CHECK(WineD3D11On12MapBufferV1(device, buffer, mode, 0, &mapped) == S_OK);
    record(NULL, NULL, NULL, &family, NULL, &type, NULL, NULL, NULL, NULL);
    CHECK(family == want_family && type == (LONG)mode);
    CHECK(mapped.pData && mapped.RowPitch == bytes);
    if (mapped.pData) memset(mapped.pData, pattern, bytes);
    CHECK(WineD3D11On12UnmapBufferV1(device, buffer) == S_OK);
    record(NULL, NULL, NULL, NULL, &unmap_family, NULL, NULL, NULL, NULL, NULL);
    CHECK(unmap_family == want_unmap);
    CHECK(read_resource(buffer->hDrvResource, back, sizeof(back)) == bytes);
    for (i = 0; i < bytes; ++i) CHECK(back[i] == pattern);
}

int main(void)
{
    struct mock_device device, other_device;
    struct mock_queue queue, other_queue;
    WineD3D11On12AdapterDevice owner = {0}, other = {0};
    WineD3D11On12Buffer cb = {0}, cb2 = {0}, vb = {0}, dyn_cb = {0}, dyn_vb = {0}, dyn_srv = {0};
    WineD3D11On12Buffer staging = {0}, staging_read = {0}, foreign = {0}, dead = {.size = sizeof(dead)};
    WineD3D11On12Buffer *bind[2];
    WineD3D11On12Texture2D texture = {.size = sizeof(texture)}, bc = {.size = sizeof(bc)};
    WineD3D11On12Texture2D staging_texture = {.size = sizeof(staging_texture)};
    D3D11_TEXTURE2D_DESC td = {0};
    D3D11_MAPPED_SUBRESOURCE mapped = {0};
    D3D11_BOX box;
    BYTE bytes[64], back[64], pixels[8 * 4 * 4], upload[4 * 2 * 4 + 16];
    LONG binds, binds_before, maps, unmaps, maps_after, unmaps_after, updates, updates_before, slot;
    int had_box, bad;
    UINT i, x, y;

    if (FAILED(open_adapter(&device, &queue, &owner)) || FAILED(open_adapter(&other_device, &other_queue, &other)))
    {
        puts("[fail] adapters did not open");
        return 1;
    }
    {
        HMODULE driver = GetModuleHandleW(L"d3d11on12.dll");
        bound_cb = (void *)(UINT_PTR)GetProcAddress(driver, "WineD3D11On12MockDriverGetBoundConstantBuffer");
        record = (void *)(UINT_PTR)GetProcAddress(driver, "WineD3D11On12MockDriverGetBufferRecord");
        read_resource = (void *)(UINT_PTR)GetProcAddress(driver, "WineD3D11On12MockDriverReadResource");
        if (!bound_cb || !record || !read_resource)
        {
            puts("[fail] the mock driver's buffer exports are missing");
            return 1;
        }
    }
    for (i = 0; i < sizeof(bytes); ++i) bytes[i] = (BYTE)i;

    CHECK(make_buffer(&owner, 64, D3D11_USAGE_DEFAULT, D3D11_BIND_CONSTANT_BUFFER, 0, bytes, &cb) == S_OK);
    CHECK(make_buffer(&owner, 64, D3D11_USAGE_DEFAULT, D3D11_BIND_CONSTANT_BUFFER, 0, NULL, &cb2) == S_OK);
    CHECK(make_buffer(&owner, 64, D3D11_USAGE_DEFAULT, D3D11_BIND_VERTEX_BUFFER, 0, NULL, &vb) == S_OK);
    CHECK(make_buffer(&owner, 64, D3D11_USAGE_DYNAMIC, D3D11_BIND_CONSTANT_BUFFER,
            D3D11_CPU_ACCESS_WRITE, NULL, &dyn_cb) == S_OK);
    CHECK(make_buffer(&owner, 32, D3D11_USAGE_DYNAMIC, D3D11_BIND_VERTEX_BUFFER,
            D3D11_CPU_ACCESS_WRITE, NULL, &dyn_vb) == S_OK);
    CHECK(make_buffer(&owner, 16, D3D11_USAGE_DYNAMIC, D3D11_BIND_SHADER_RESOURCE,
            D3D11_CPU_ACCESS_WRITE, NULL, &dyn_srv) == S_OK);
    CHECK(make_buffer(&owner, 48, D3D11_USAGE_STAGING, 0,
            D3D11_CPU_ACCESS_READ | D3D11_CPU_ACCESS_WRITE, NULL, &staging) == S_OK);
    CHECK(make_buffer(&owner, 48, D3D11_USAGE_STAGING, 0, D3D11_CPU_ACCESS_READ, NULL, &staging_read) == S_OK);
    CHECK(make_buffer(&other, 64, D3D11_USAGE_DEFAULT, D3D11_BIND_CONSTANT_BUFFER, 0, NULL, &foreign) == S_OK);
    /* Initial data lands in the driver's storage. */
    CHECK(read_resource(cb.hDrvResource, back, sizeof(back)) == 64 && !memcmp(back, bytes, 64));

    /* Binding: both stages, both ends of the slot range, whole buffers. */
    bind[0] = &cb; bind[1] = &cb2;
    CHECK(WineD3D11On12SetConstantBuffersV1(&owner, WINE_D3D11ON12_SHADER_VERTEX, 0, 2, bind) == S_OK);
    CHECK(bound_cb(0, 0) == cb.hDrvResource && bound_cb(0, 1) == cb2.hDrvResource);
    CHECK(WineD3D11On12SetConstantBuffersV1(&owner, WINE_D3D11ON12_SHADER_PIXEL, 13, 1, bind) == S_OK);
    CHECK(bound_cb(1, 13) == cb.hDrvResource);
    record(&binds_before, NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL);
    /* Rejected before any dispatch: slot bounds, unknown stage, null array,
     * a buffer without the constant-buffer bind flag, another device's. */
    CHECK(WineD3D11On12SetConstantBuffersV1(&owner, WINE_D3D11ON12_SHADER_PIXEL, 14, 1, bind) == E_INVALIDARG);
    CHECK(WineD3D11On12SetConstantBuffersV1(&owner, WINE_D3D11ON12_SHADER_PIXEL, 13, 2, bind) == E_INVALIDARG);
    CHECK(WineD3D11On12SetConstantBuffersV1(&owner, 3, 0, 1, bind) == E_INVALIDARG);
    CHECK(WineD3D11On12SetConstantBuffersV1(&owner, WINE_D3D11ON12_SHADER_VERTEX, 0, 1, NULL) == E_INVALIDARG);
    bind[1] = &vb;
    CHECK(WineD3D11On12SetConstantBuffersV1(&owner, WINE_D3D11ON12_SHADER_VERTEX, 0, 2, bind) == E_INVALIDARG);
    bind[1] = &foreign;
    CHECK(WineD3D11On12SetConstantBuffersV1(&owner, WINE_D3D11ON12_SHADER_VERTEX, 0, 2, bind) == E_INVALIDARG);
    record(&binds, NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL);
    CHECK(binds == binds_before && bound_cb(0, 1) == cb2.hDrvResource);
    /* Null unbinds a slot. */
    bind[0] = NULL;
    CHECK(WineD3D11On12SetConstantBuffersV1(&owner, WINE_D3D11ON12_SHADER_VERTEX, 1, 1, bind) == S_OK);
    CHECK(!bound_cb(0, 1));

    /* Destroying a bound buffer unbinds it everywhere, first. */
    CHECK(WineD3D11On12DestroyBufferV1(&cb) == S_OK);
    CHECK(!bound_cb(0, 0) && !bound_cb(1, 13));
    CHECK(WineD3D11On12DestroyBufferV1(&cb) == S_OK);
    bind[0] = &cb;
    CHECK(WineD3D11On12SetConstantBuffersV1(&owner, WINE_D3D11ON12_SHADER_VERTEX, 0, 1, bind) == E_INVALIDARG);

    /* Every map family, with the matching unmap and the written bytes. */
    map_write_unmap(&owner, &dyn_cb, D3D11_MAP_WRITE_DISCARD, FAMILY_CB_DISCARD, FAMILY_CB_DISCARD, 0xa1, 64);
    map_write_unmap(&owner, &dyn_cb, D3D11_MAP_WRITE_NO_OVERWRITE, FAMILY_CB_NO_OVERWRITE, FAMILY_CB_DISCARD, 0xa2, 64);
    map_write_unmap(&owner, &dyn_vb, D3D11_MAP_WRITE_DISCARD, FAMILY_IA_DISCARD, FAMILY_IA_DISCARD, 0xb1, 32);
    map_write_unmap(&owner, &dyn_vb, D3D11_MAP_WRITE_NO_OVERWRITE, FAMILY_IA_NO_OVERWRITE, FAMILY_IA_DISCARD, 0xb2, 32);
    map_write_unmap(&owner, &dyn_srv, D3D11_MAP_WRITE_DISCARD, FAMILY_RESOURCE_DISCARD, FAMILY_RESOURCE_DISCARD, 0xc1, 16);
    map_write_unmap(&owner, &staging, D3D11_MAP_WRITE, FAMILY_STAGING, FAMILY_STAGING, 0xd1, 48);
    map_write_unmap(&owner, &staging, D3D11_MAP_READ_WRITE, FAMILY_STAGING, FAMILY_STAGING, 0xd2, 48);
    CHECK(WineD3D11On12MapBufferV1(&owner, &staging, D3D11_MAP_READ, D3D11_MAP_FLAG_DO_NOT_WAIT, &mapped) == S_OK);
    CHECK(mapped.pData && ((BYTE *)mapped.pData)[0] == 0xd2);
    CHECK(WineD3D11On12UnmapBufferV1(&owner, &staging) == S_OK);

    /* Misuse: wrong mode for the usage, missing CPU access, flags on a
     * dynamic map, a default buffer, a double map, an unmap without a map. */
    record(NULL, &maps, &unmaps, NULL, NULL, NULL, NULL, NULL, NULL, NULL);
    CHECK(WineD3D11On12MapBufferV1(&owner, &dyn_cb, D3D11_MAP_READ, 0, &mapped) == DXGI_ERROR_INVALID_CALL);
    CHECK(!mapped.pData);
    CHECK(WineD3D11On12MapBufferV1(&owner, &dyn_cb, D3D11_MAP_WRITE_DISCARD,
            D3D11_MAP_FLAG_DO_NOT_WAIT, &mapped) == DXGI_ERROR_INVALID_CALL);
    CHECK(WineD3D11On12MapBufferV1(&owner, &dyn_srv, D3D11_MAP_WRITE_NO_OVERWRITE, 0, &mapped) == DXGI_ERROR_INVALID_CALL);
    CHECK(WineD3D11On12MapBufferV1(&owner, &staging, D3D11_MAP_WRITE_DISCARD, 0, &mapped) == DXGI_ERROR_INVALID_CALL);
    CHECK(WineD3D11On12MapBufferV1(&owner, &staging_read, D3D11_MAP_WRITE, 0, &mapped) == DXGI_ERROR_INVALID_CALL);
    CHECK(WineD3D11On12MapBufferV1(&owner, &cb2, D3D11_MAP_WRITE_DISCARD, 0, &mapped) == DXGI_ERROR_INVALID_CALL);
    CHECK(WineD3D11On12MapBufferV1(&owner, &foreign, D3D11_MAP_WRITE_DISCARD, 0, &mapped) == E_INVALIDARG);
    CHECK(WineD3D11On12MapBufferV1(&owner, &dead, D3D11_MAP_WRITE_DISCARD, 0, &mapped) == E_INVALIDARG);
    CHECK(WineD3D11On12UnmapBufferV1(&owner, &dyn_cb) == DXGI_ERROR_INVALID_CALL);
    CHECK(WineD3D11On12MapBufferV1(&owner, &dyn_cb, D3D11_MAP_WRITE_DISCARD, 0, &mapped) == S_OK);
    CHECK(WineD3D11On12MapBufferV1(&owner, &dyn_cb, D3D11_MAP_WRITE_DISCARD, 0, &mapped) == DXGI_ERROR_INVALID_CALL);
    CHECK(!mapped.pData);
    record(NULL, &maps_after, &unmaps_after, NULL, NULL, NULL, NULL, NULL, NULL, NULL);
    CHECK(maps_after == maps + 1 && unmaps_after == unmaps);
    /* Destroying a mapped buffer unmaps it through its family, then frees. */
    CHECK(WineD3D11On12DestroyBufferV1(&dyn_cb) == S_OK);
    record(NULL, NULL, &unmaps, NULL, &slot, NULL, NULL, NULL, NULL, NULL);
    CHECK(unmaps == unmaps_after + 1 && slot == FAMILY_CB_DISCARD);

    /* Constant-buffer update: whole buffer only, through its own slot. */
    for (i = 0; i < sizeof(bytes); ++i) bytes[i] = (BYTE)(0x40 + i);
    box.left = 0; box.right = 16; box.top = 0; box.bottom = 1; box.front = 0; box.back = 1;
    record(NULL, NULL, NULL, NULL, NULL, NULL, &updates_before, NULL, NULL, NULL);
    CHECK(WineD3D11On12UpdateBufferV1(&owner, &cb2, &box, bytes, 0, 0) == E_INVALIDARG);
    CHECK(WineD3D11On12UpdateBufferV1(&owner, &cb2, NULL, NULL, 0, 0) == E_INVALIDARG);
    CHECK(WineD3D11On12UpdateBufferV1(&owner, &cb2, NULL, bytes, 0, 0) == S_OK);
    record(NULL, NULL, NULL, NULL, NULL, NULL, &updates, &slot, &had_box, NULL);
    CHECK(updates == updates_before + 1 && slot == 2 && !had_box);
    CHECK(read_resource(cb2.hDrvResource, back, sizeof(back)) == 64 && !memcmp(back, bytes, 64));
    /* Other buffers: a byte range, checked against ByteWidth; empty is a no-op. */
    memset(back, 0, sizeof(back));
    box.left = 4; box.right = 12;
    CHECK(WineD3D11On12UpdateBufferV1(&owner, &vb, &box, bytes, 0, 0) == S_OK);
    record(NULL, NULL, NULL, NULL, NULL, NULL, &updates, &slot, &had_box, NULL);
    CHECK(updates == updates_before + 2 && slot == 1 && had_box);
    CHECK(read_resource(vb.hDrvResource, back, sizeof(back)) == 64);
    CHECK(!back[3] && !memcmp(back + 4, bytes, 8) && !back[12]);
    box.right = 65;
    CHECK(WineD3D11On12UpdateBufferV1(&owner, &vb, &box, bytes, 0, 0) == E_INVALIDARG);
    box.right = 12; box.bottom = 2;
    CHECK(WineD3D11On12UpdateBufferV1(&owner, &vb, &box, bytes, 0, 0) == E_INVALIDARG);
    box.bottom = 1; box.left = 12;
    CHECK(WineD3D11On12UpdateBufferV1(&owner, &vb, &box, bytes, 0, 0) == S_OK);
    CHECK(WineD3D11On12UpdateBufferV1(&owner, &dyn_vb, NULL, bytes, 0, 0) == DXGI_ERROR_INVALID_CALL);
    CHECK(WineD3D11On12UpdateBufferV1(&owner, &staging, NULL, bytes, 0, 0) == DXGI_ERROR_INVALID_CALL);
    record(NULL, NULL, NULL, NULL, NULL, NULL, &updates, NULL, NULL, NULL);
    CHECK(updates == updates_before + 2);

    /* Texture2D update: a 4x2 box at (2,1) with a padded row pitch. */
    td.Width = 8; td.Height = 4; td.MipLevels = td.ArraySize = td.SampleDesc.Count = 1;
    td.Format = DXGI_FORMAT_R8G8B8A8_UNORM; td.BindFlags = D3D11_BIND_SHADER_RESOURCE;
    CHECK(WineD3D11On12CreateTexture2DV1(&owner, &td, NULL, &texture) == S_OK);
    for (i = 0; i < sizeof(upload); ++i) upload[i] = (BYTE)(0x80 + i);
    box.left = 2; box.right = 6; box.top = 1; box.bottom = 3; box.front = 0; box.back = 1;
    CHECK(WineD3D11On12UpdateTexture2DV1(&owner, &texture, 0, &box, upload, 4 * 4 + 8, 0) == S_OK);
    CHECK(read_resource(texture.hDrvResource, pixels, sizeof(pixels)) == sizeof(pixels));
    for (y = 0; y < 4; ++y)
        for (x = 0; x < 8; ++x)
        {
            const BYTE *texel = pixels + (y * 8 + x) * 4;
            if (x >= 2 && x < 6 && y >= 1 && y < 3)
                CHECK(!memcmp(texel, upload + (y - 1) * 24 + (x - 2) * 4, 4));
            else
                CHECK(!texel[0] && !texel[3]);
        }
    record(NULL, NULL, NULL, NULL, NULL, NULL, &updates_before, NULL, NULL, NULL);
    CHECK(WineD3D11On12UpdateTexture2DV1(&owner, &texture, 0, &box, upload, 15, 0) == E_INVALIDARG);
    box.right = 9;
    CHECK(WineD3D11On12UpdateTexture2DV1(&owner, &texture, 0, &box, upload, 64, 0) == E_INVALIDARG);
    box.right = 6; box.back = 2;
    CHECK(WineD3D11On12UpdateTexture2DV1(&owner, &texture, 0, &box, upload, 64, 0) == E_INVALIDARG);
    box.back = 1;
    CHECK(WineD3D11On12UpdateTexture2DV1(&owner, &texture, 1, &box, upload, 64, 0) == E_INVALIDARG);
    td.Format = DXGI_FORMAT_BC1_UNORM;
    CHECK(WineD3D11On12CreateTexture2DV1(&owner, &td, NULL, &bc) == S_OK);
    CHECK(WineD3D11On12UpdateTexture2DV1(&owner, &bc, 0, NULL, upload, 64, 0) == DXGI_ERROR_UNSUPPORTED);
    td.Format = DXGI_FORMAT_R8G8B8A8_UNORM; td.BindFlags = 0;
    td.Usage = D3D11_USAGE_STAGING; td.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    CHECK(WineD3D11On12CreateTexture2DV1(&owner, &td, NULL, &staging_texture) == S_OK);
    CHECK(WineD3D11On12UpdateTexture2DV1(&owner, &staging_texture, 0, NULL, upload, 64, 0) == DXGI_ERROR_INVALID_CALL);
    record(NULL, NULL, NULL, NULL, NULL, NULL, &updates, NULL, NULL, &bad);
    CHECK(updates == updates_before && !bad);

    /* Concurrency: binds, maps and updates racing destruction's unbind. */
    {
        struct storm storm = {&owner, &cb2, CreateEventW(NULL, TRUE, FALSE, NULL), 0, 0};
        HANDLE threads[8];
        UINT started = 0;

        CHECK(storm.start != NULL);
        for (i = 0; storm.start && i < 8; ++i)
            if ((threads[started] = CreateThread(NULL, 0, storm_thread, &storm, 0, NULL)))
                ++started;
        if (storm.start) SetEvent(storm.start);
        CHECK(started == 8);
        CHECK(!started || WaitForMultipleObjects(started, threads, TRUE, 30000) == WAIT_OBJECT_0);
        for (i = 0; i < started; ++i) CloseHandle(threads[i]);
        if (storm.start) CloseHandle(storm.start);
        CHECK(storm.unexpected == 0);
        for (i = 0; i < 14; ++i) CHECK(!bound_cb(0, i) && !bound_cb(1, i));
        record(NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL, &bad);
        CHECK(!bad);
    }

    /* Teardown releases live buffers and textures; later destroys are no-ops. */
    CHECK(WineD3D11On12CloseAdapterDeviceV1(&other) == S_OK);
    CHECK(WineD3D11On12CloseAdapterDeviceV1(&owner) == S_OK);
    CHECK(!cb2.runtimeState && !texture.runtimeState && !staging.runtimeState);
    CHECK(WineD3D11On12DestroyBufferV1(&cb2) == S_OK);
    CHECK(WineD3D11On12DestroyTexture2DV1(&texture) == S_OK);
    CHECK(device.refcount == 1 && other_device.refcount == 1);
    printf("[%s] constant buffers, buffer maps and updates\n", failures ? "fail" : " ok ");
    return !!failures;
}
