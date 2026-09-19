/* SPDX-License-Identifier: GPL-3.0-only
 *
 * The host object convention's one moving part: turning a public COM pointer
 * back into the driver handle behind it.  See wine_d3d11_host.h for why this
 * is a private interface query and not a cast.
 */
#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include "wine_d3d11_host.h"
#include "wine_d3d11_com.h"
#include "wine_d3d11_diag.h"

using wine_d3d11::ComRef;
using wine_d3d11::strictResult;

/* {6f2b9c14-5d3a-4c8e-9f17-2a4d8b6e1c30}
 *
 * Generated for this project and private to it.  Nothing outside these
 * modules may be expected to answer it, which is the property the resolver
 * relies on to reject a foreign object. */
extern "C" const GUID IID_IWineD3D11HostObject =
{
    0x6f2b9c14, 0x5d3a, 0x4c8e,
    { 0x9f, 0x17, 0x2a, 0x4d, 0x8b, 0x6e, 0x1c, 0x30 }
};

namespace
{
/* One guard per repeatable condition, for the reason d3d11on12core.cpp gives:
 * a binding call sits inside the application's draw loop, so a condition
 * reached once is reached every frame. */
volatile LONG reportedForeignObject;
volatile LONG reportedKindMismatch;
volatile LONG reportedDeviceMismatch;
volatile LONG reportedHandleLayout;
}

extern "C" HRESULT wineD3D11HostResolveHandle(IUnknown *object,
        UINT expectedKind, void *expectedDevice, void **pDrvPrivate) noexcept
{
    if (!object || !pDrvPrivate || expectedKind == WINE_D3D11_HOST_KIND_NONE)
        return E_INVALIDARG;

    ComRef<IWineD3D11HostObject> hostObject;
    HRESULT hr = strictResult(object->QueryInterface(IID_IWineD3D11HostObject,
            reinterpret_cast<void **>(hostObject.put())), hostObject);
    if (FAILED(hr))
    {
        wineD3D11DiagReportOnce(&reportedForeignObject,
                "d3d11host: an object passed to this device was not created "
                "by it; the D3D11On12 host cannot submit an object whose "
                "driver handle it does not know.\n");
        return E_NOINTERFACE;
    }

    WineD3D11HostHandle handle = {};
    hr = hostObject.get()->GetHostHandle(&handle);
    if (FAILED(hr))
        return hr;

    /* Size is the callee's statement of which layout it filled.  An object
     * built against a shorter one has not written the members below, and
     * reading them would be reading its padding. */
    if (handle.size != sizeof(handle))
    {
        wineD3D11DiagReportOnce(&reportedHandleLayout,
                "d3d11host: an object reported an interior handle of an "
                "unexpected size; refusing to read members it may not have "
                "written.\n");
        return E_FAIL;
    }

    if (handle.kind != expectedKind)
    {
        wineD3D11DiagReportOnce(&reportedKindMismatch,
                "d3d11host: an object of the wrong kind was passed to a "
                "binding call; every handle in this DDI is one wrapped "
                "pointer, so the driver could not have detected this.\n");
        return E_INVALIDARG;
    }

    if (expectedDevice && handle.device != expectedDevice)
    {
        wineD3D11DiagReportOnce(&reportedDeviceMismatch,
                "d3d11host: an object belonging to a different D3D11 device "
                "was passed to this one.\n");
        return E_INVALIDARG;
    }

    /* Written last, and only once every check has passed. */
    *pDrvPrivate = handle.pDrvPrivate;
    return S_OK;
}

extern "C" HRESULT wineD3D11HostResolveOptionalHandle(IUnknown *object,
        UINT expectedKind, void *expectedDevice, void **pDrvPrivate) noexcept
{
    if (!pDrvPrivate)
        return E_INVALIDARG;

    /* Null means "unbind", in both the public API and the DDI.  Writing null
     * rather than leaving the output alone is correct here precisely because
     * the caller asked about an optional argument: it wants the handle value
     * that expresses absence, and for these handles that value is null. */
    if (!object)
    {
        *pDrvPrivate = nullptr;
        return S_OK;
    }

    return wineD3D11HostResolveHandle(object, expectedKind, expectedDevice,
            pDrvPrivate);
}
