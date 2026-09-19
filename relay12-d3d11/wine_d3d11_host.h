/* SPDX-License-Identifier: GPL-3.0-only
 *
 * The host object convention: how a public D3D11 COM object this runtime
 * handed out is turned back into the DDI handle behind it.
 *
 * Every slot in D3DWDDM2_6DDI_DEVICEFUNCS takes driver handles, never COM
 * pointers.  The public API takes COM pointers, never handles.  Something has
 * to bridge the two, and the bridge is the single most dangerous piece of the
 * host: every driver handle in this DDI is one wrapped pointer, so handing a
 * slot the pDrvPrivate of the wrong kind of object would be eight bytes at the
 * right offset that no assertion could catch, and the driver would dereference
 * it as a type it is not.
 *
 * So the bridge is a private COM interface rather than a cast.  Three
 * properties follow from that choice, and all three are the reason for it:
 *
 *   1. An object this runtime did not create answers the query with
 *      E_NOINTERFACE.  A cast would have accepted it.
 *   2. The answer carries the object's kind, so a caller asking for a
 *      render-target view cannot be given a shader's handle.
 *   3. The answer carries the owning device, so an object belonging to a
 *      different device is rejected rather than submitted to this one.
 *
 * Declared for C as well as C++ in the dual style Wine's own generated
 * headers use.  The host is C++, but the validation tests are C so they can
 * build mock objects from the C vtable form, and the two declarations must
 * describe one ABI -- the same reason the clean-room header compiles as both.
 */
#ifndef WINE_D3D11_HOST_H
#define WINE_D3D11_HOST_H

#include <windows.h>
#include <d3d11.h>
#include <d3d11on12.h>

#ifdef __cplusplus
# define WINE_D3D11_HOST_LINKAGE extern "C"
# define WINE_D3D11_HOST_NOEXCEPT noexcept
#else
# define WINE_D3D11_HOST_LINKAGE extern
# define WINE_D3D11_HOST_NOEXCEPT
#endif

/* Which kind of DDI object a handle names.
 *
 * Kinds are not interchangeable even when the handle types have identical
 * layout, which all of them do.  A resolver is always told which kind it
 * wants and refuses any other, so this enumeration is the type system the
 * one-wrapped-pointer handles do not provide.
 *
 * Values are explicit and must never be renumbered: they cross the module
 * boundary inside WineD3D11HostHandle, and the C tests write them literally.
 * Zero is deliberately not a kind, so a zeroed structure names nothing. */
typedef enum WineD3D11HostObjectKind
{
    WINE_D3D11_HOST_KIND_NONE               = 0,
    WINE_D3D11_HOST_KIND_DEVICE             = 1,
    WINE_D3D11_HOST_KIND_CONTEXT            = 2,
    WINE_D3D11_HOST_KIND_RESOURCE           = 3,
    WINE_D3D11_HOST_KIND_RENDERTARGETVIEW   = 4,
    WINE_D3D11_HOST_KIND_SHADERRESOURCEVIEW = 5,
    WINE_D3D11_HOST_KIND_ELEMENTLAYOUT      = 6,
    WINE_D3D11_HOST_KIND_SHADER             = 7,
    WINE_D3D11_HOST_KIND_BLENDSTATE         = 8,
    WINE_D3D11_HOST_KIND_DEPTHSTENCILSTATE  = 9,
    WINE_D3D11_HOST_KIND_RASTERIZERSTATE    = 10,
    WINE_D3D11_HOST_KIND_SAMPLER            = 11,
    WINE_D3D11_HOST_KIND_COMMANDLIST        = 12
} WineD3D11HostObjectKind;

/* What the interior query answers.
 *
 * pDrvPrivate is the driver half of whichever handle `kind` names -- the
 * value that goes into D3D10DDI_HRESOURCE.pDrvPrivate and its siblings.  It
 * is not a COM pointer and must not be released.
 *
 * device is the host device that created the object, compared by pointer for
 * cross-device rejection.  It is deliberately opaque here: a caller resolving
 * a handle needs to know whether two objects agree on their device, not what
 * a device is.
 *
 * size is set by the callee to sizeof(WineD3D11HostHandle) so that a future
 * member cannot be read out of an object built against an older layout. */
typedef struct WineD3D11HostHandle
{
    UINT size;
    UINT kind;
    void *pDrvPrivate;
    void *device;
} WineD3D11HostHandle;

#ifdef __cplusplus
extern "C" {
#endif
/* Defined in wine_d3d11_host.cpp rather than through __uuidof: mingw's
 * __declspec(uuid) support is not something this ABI should rest on, and a
 * named constant works identically from C. */
extern const GUID IID_IWineD3D11HostObject;
#ifdef __cplusplus
}
#endif

#ifdef __cplusplus

struct IWineD3D11HostObject : public IUnknown
{
    virtual HRESULT STDMETHODCALLTYPE GetHostHandle(
            WineD3D11HostHandle *handle) = 0;
};

#else

typedef struct IWineD3D11HostObject IWineD3D11HostObject;

typedef struct IWineD3D11HostObjectVtbl
{
    HRESULT (STDMETHODCALLTYPE *QueryInterface)(IWineD3D11HostObject *self,
            const GUID *riid, void **out);
    ULONG (STDMETHODCALLTYPE *AddRef)(IWineD3D11HostObject *self);
    ULONG (STDMETHODCALLTYPE *Release)(IWineD3D11HostObject *self);
    HRESULT (STDMETHODCALLTYPE *GetHostHandle)(IWineD3D11HostObject *self,
            WineD3D11HostHandle *handle);
} IWineD3D11HostObjectVtbl;

struct IWineD3D11HostObject
{
    const IWineD3D11HostObjectVtbl *lpVtbl;
};

#endif /* __cplusplus */

/* Resolve one public object to the driver handle behind it.
 *
 * expectedKind and expectedDevice are both required, and both are checked
 * before anything is written: a resolver that reported the handle first and
 * validated afterwards would leave a caller holding a handle it had already
 * been told was wrong.  expectedDevice may be null only when the caller has
 * no device to compare against, which is true of exactly one call site -- the
 * device asking an object whether it belongs to it.
 *
 * Returns E_INVALIDARG for a null object, E_NOINTERFACE for an object this
 * runtime did not create, and E_INVALIDARG for a kind or device mismatch.  On
 * any failure *pDrvPrivate is left untouched, because the DDI has no handle
 * value meaning "none" and writing zero would be indistinguishable from a
 * legitimately zero private block. */
WINE_D3D11_HOST_LINKAGE HRESULT wineD3D11HostResolveHandle(IUnknown *object,
        UINT expectedKind, void *expectedDevice, void **pDrvPrivate)
        WINE_D3D11_HOST_NOEXCEPT;

/* The same question asked of an optional argument.
 *
 * A null object is not an error here: the public API uses null to mean "unbind
 * this slot", and the DDI spells the same thing as a handle whose pDrvPrivate
 * is null.  This is the only place that equivalence is written down, so that
 * no individual method has to decide it again. */
WINE_D3D11_HOST_LINKAGE HRESULT wineD3D11HostResolveOptionalHandle(
        IUnknown *object, UINT expectedKind, void *expectedDevice,
        void **pDrvPrivate) WINE_D3D11_HOST_NOEXCEPT;

#endif /* WINE_D3D11_HOST_H */
