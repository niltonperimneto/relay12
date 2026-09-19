/* SPDX-License-Identifier: GPL-3.0-only
 *
 * The COM ownership and acquisition rules the host shares with the core.
 *
 * Both of these already existed inside d3d11on12core.cpp's anonymous
 * namespace, and this is the same code with the same reasoning, moved so that
 * the host files can obey the rules rather than restate them.  The funnel in
 * particular must be one function: scripts/check_interface_acquisition.py
 * requires every QueryInterface and GetDevice in relay12-d3d11 to pass its
 * holder through strictResult, and a second copy of strictResult would let
 * the two drift while both kept passing the audit.
 *
 * d3d11on12core.cpp still carries its originals.  Collapsing them is a
 * one-line include swap, deliberately left until the device-creation work in
 * flight against that file lands, so that this change does not conflict with
 * it.
 *
 * C++ only, and no libstdc++: HeapAlloc owns every allocation in this module,
 * so there is no operator new to link against and nothing here may introduce
 * one.
 */
#ifndef WINE_D3D11_COM_H
#define WINE_D3D11_COM_H

#include <windows.h>

namespace wine_d3d11
{

/* A scoped reference.  Non-copyable because a copy would have to decide
 * whether to add a reference, and every call site here wants exactly one. */
template<typename Interface>
class ComRef
{
public:
    ComRef() noexcept = default;
    ~ComRef() noexcept
    {
        if (pointer_)
            pointer_->Release();
    }

    ComRef(const ComRef &) = delete;
    ComRef &operator=(const ComRef &) = delete;

    Interface *get() const noexcept { return pointer_; }

    Interface *detach() noexcept
    {
        Interface *result = pointer_;
        pointer_ = nullptr;
        return result;
    }

    /* Releasing first keeps a second acquisition through the same holder from
     * dropping the first reference on the floor. */
    Interface **put() noexcept
    {
        if (pointer_)
        {
            pointer_->Release();
            pointer_ = nullptr;
        }
        return &pointer_;
    }

private:
    Interface *pointer_ = nullptr;
};

/* Every interface this module acquires goes through here.
 *
 * A success code returned with no interface breaks the COM contract, and the
 * D3DMetal payload does it, so the pointer has to be checked at every
 * acquisition and not just at most of them.
 *
 * The holder is taken by reference, not as a pointer value: argument
 * evaluation order is unspecified, so passing acquired.get() alongside the
 * call that fills it could read the pointer before the call writes it and
 * reject every success.  Binding a reference reads nothing. */
template<typename Interface>
HRESULT strictResult(HRESULT hr, const ComRef<Interface> &acquired) noexcept
{
    if (SUCCEEDED(hr) && !acquired.get())
        return E_NOINTERFACE;
    return hr;
}

/* The same rule for a raw out-pointer.
 *
 * The interior handle query below hands out no reference it keeps, so it has
 * no holder to bind.  Overloading rather than relaxing strictResult keeps the
 * audited name on both paths. */
inline HRESULT strictResult(HRESULT hr, void *acquired) noexcept
{
    if (SUCCEEDED(hr) && !acquired)
        return E_NOINTERFACE;
    return hr;
}

} /* namespace wine_d3d11 */

#endif /* WINE_D3D11_COM_H */
