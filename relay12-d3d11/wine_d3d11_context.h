/* SPDX-License-Identifier: GPL-3.0-only
 *
 * The immediate context: ID3D11DeviceContext over D3DWDDM2_6DDI_DEVICEFUNCS.
 *
 * What binds the two is the device handle.  In this DDI the immediate context
 * is not a separate object -- every context-level slot takes the
 * D3D10DDI_HDEVICE of the device itself, and it is the deferred contexts that
 * get their own handles.  So the immediate context this constructs is a
 * public COM object with no driver object of its own, dispatching onto the
 * device's handle.
 *
 * Deliberately separable from device creation.  The binding is four values,
 * none of which is an ID3D11Device, so the dispatch can be built and tested
 * against a mock device function table before a real device exists.  That is
 * the only reason this is its own translation unit.
 */
#ifndef WINE_D3D11_CONTEXT_H
#define WINE_D3D11_CONTEXT_H

#include <windows.h>
#include <d3d11.h>

#include "wine_d3d11_host.h"
#include "wine_d3d11ddi_negotiate.h"

#ifdef __cplusplus
# define WINE_D3D11_CONTEXT_LINKAGE extern "C"
# define WINE_D3D11_CONTEXT_NOEXCEPT noexcept
#else
# define WINE_D3D11_CONTEXT_LINKAGE extern
# define WINE_D3D11_CONTEXT_NOEXCEPT
#endif

/* Everything the dispatch needs to reach the driver.
 *
 * deviceFuncs and hDevice come straight out of WineD3D11On12AdapterDevice.
 * hostDevice is the token the handle resolver compares for cross-device
 * rejection; it is the host device object's address and is never dereferenced
 * here.  parentDevice is what GetDevice returns, and may be null only in the
 * dispatch tests, which have no device to return.
 *
 * The structure is copied into the context at construction, so the caller may
 * keep it on the stack. */
typedef struct WineD3D11ContextBinding
{
    UINT size;
    struct D3DWDDM2_6DDI_DEVICEFUNCS *deviceFuncs;
    void *hDrvDevice;
    void *hostDevice;
    ID3D11Device *parentDevice;
} WineD3D11ContextBinding;

/* Build the immediate context.
 *
 * Returns E_INVALIDARG for a malformed binding and E_OUTOFMEMORY if the
 * object cannot be allocated.  On success the caller owns one reference.
 *
 * No slot is called here: an immediate context is a runtime object and the
 * DDI has no create-context entry for it, which is exactly why the device
 * handle is what the dispatch uses. */
WINE_D3D11_CONTEXT_LINKAGE HRESULT wineD3D11CreateImmediateContext(
        const WineD3D11ContextBinding *binding,
        ID3D11DeviceContext **context) WINE_D3D11_CONTEXT_NOEXCEPT;

#endif /* WINE_D3D11_CONTEXT_H */
