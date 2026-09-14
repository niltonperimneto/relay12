/* SPDX-License-Identifier: GPL-3.0-only
 * Minimal native test driver for the core's dynamic OpenAdapter boundary.
 */
#include <windows.h>
#include <dxgi.h>

#include "../relay12-d3d11/ddi/wine_d3d11ddi.h"

static LONG open_calls;
static LONG create_calls;
static LONG destroy_calls;
static LONG close_calls;
static LONG flush_calls;
static LONG draw_calls;
static LONG indexed_draw_calls;
static LONG instanced_draw_calls;
static LONG indexed_instanced_draw_calls;
static unsigned char adapter_private;

static HRESULT mock_get_versions(D3D10DDI_HADAPTER adapter, UINT32 *count,
        UINT64 *versions)
{
    (void)adapter;
    if (!count)
        return E_INVALIDARG;
    *count = 1;
    if (versions)
        versions[0] = WINE_D3D11_DDI_SUPPORTED(
                WINE_D3D11_DDI_INTERFACE_VERSION(3), 1);
    return S_OK;
}

static SIZE_T mock_private_device_size(D3D10DDI_HADAPTER adapter,
        const D3D10DDIARG_CALCPRIVATEDEVICESIZE *args)
{
    (void)adapter;
    (void)args;
    return 64;
}

static void mock_destroy_device(D3D10DDI_HDEVICE device)
{
    (void)device;
    InterlockedIncrement(&destroy_calls);
}

static BOOL mock_flush(D3D10DDI_HDEVICE device, UINT context_type,
        UINT flush_flags)
{
    (void)device;
    (void)context_type;
    (void)flush_flags;
    InterlockedIncrement(&flush_calls);
    return TRUE;
}

static void mock_draw(D3D10DDI_HDEVICE device, UINT vertex_count,
        UINT start_vertex_location)
{
    (void)device;
    (void)vertex_count;
    (void)start_vertex_location;
    InterlockedIncrement(&draw_calls);
}

static void mock_draw_indexed(D3D10DDI_HDEVICE device, UINT index_count,
        UINT start_index, INT base_vertex)
{
    (void)device; (void)index_count; (void)start_index; (void)base_vertex;
    InterlockedIncrement(&indexed_draw_calls);
}

static void mock_draw_instanced(D3D10DDI_HDEVICE device, UINT vertex_count,
        UINT instance_count, UINT start_vertex, UINT start_instance)
{
    (void)device; (void)vertex_count; (void)instance_count;
    (void)start_vertex; (void)start_instance;
    InterlockedIncrement(&instanced_draw_calls);
}

static void mock_draw_indexed_instanced(D3D10DDI_HDEVICE device,
        UINT index_count, UINT instance_count, UINT start_index,
        INT base_vertex, UINT start_instance)
{
    (void)device; (void)index_count; (void)instance_count; (void)start_index;
    (void)base_vertex; (void)start_instance;
    InterlockedIncrement(&indexed_instanced_draw_calls);
}

static HRESULT mock_create_device(D3D10DDI_HADAPTER adapter,
        D3D10DDIARG_CREATEDEVICE *args)
{
    (void)adapter;
    if (!args || !args->pWDDM2_6DeviceFuncs)
        return E_INVALIDARG;
    args->pWDDM2_6DeviceFuncs->pfnDestroyDevice = mock_destroy_device;
    args->pWDDM2_6DeviceFuncs->pfnFlush = mock_flush;
    args->pWDDM2_6DeviceFuncs->pfnDraw = mock_draw;
    args->pWDDM2_6DeviceFuncs->pfnDrawIndexed = mock_draw_indexed;
    args->pWDDM2_6DeviceFuncs->pfnDrawInstanced = mock_draw_instanced;
    args->pWDDM2_6DeviceFuncs->pfnDrawIndexedInstanced =
            mock_draw_indexed_instanced;
    InterlockedIncrement(&create_calls);
    return S_OK;
}

__declspec(dllexport) LONG WINAPI WineD3D11On12MockDriverGetFlushCount(void)
{
    return flush_calls;
}

__declspec(dllexport) LONG WINAPI WineD3D11On12MockDriverGetDrawCount(void)
{
    return draw_calls;
}

__declspec(dllexport) void WINAPI WineD3D11On12MockDriverGetExtendedDrawCounts(
        LONG *indexed, LONG *instanced, LONG *indexed_instanced)
{
    if (indexed) *indexed = indexed_draw_calls;
    if (instanced) *instanced = instanced_draw_calls;
    if (indexed_instanced) *indexed_instanced = indexed_instanced_draw_calls;
}

static HRESULT mock_close_adapter(D3D10DDI_HADAPTER adapter)
{
    (void)adapter;
    InterlockedIncrement(&close_calls);
    return S_OK;
}

__declspec(dllexport) HRESULT WINAPI OpenAdapter_D3D11On12(
        D3D10DDIARG_OPENADAPTER *args, void *private_args)
{
    (void)private_args;
    if (!args || !args->pAdapterFuncs_2)
        return E_INVALIDARG;
    args->hAdapter.pDrvPrivate = &adapter_private;
    args->pAdapterFuncs_2->pfnGetSupportedVersions = mock_get_versions;
    args->pAdapterFuncs_2->pfnCalcPrivateDeviceSize =
            mock_private_device_size;
    args->pAdapterFuncs_2->pfnCreateDevice = mock_create_device;
    args->pAdapterFuncs_2->pfnCloseAdapter = mock_close_adapter;
    InterlockedIncrement(&open_calls);
    return S_OK;
}

__declspec(dllexport) void WINAPI WineD3D11On12MockDriverGetCounts(
        LONG *opened, LONG *created, LONG *destroyed, LONG *closed)
{
    if (opened)
        *opened = open_calls;
    if (created)
        *created = create_calls;
    if (destroyed)
        *destroyed = destroy_calls;
    if (closed)
        *closed = close_calls;
}
