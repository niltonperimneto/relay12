// Copyright (c) relay12 contributors.
// SPDX-License-Identifier: GPL-3.0-only
#pragma once

// Dependency-free replacement for MSVC's _com_error at internal HRESULT
// exception boundaries. HRESULT must be declared by the including translation
// unit so this header remains usable with both Windows SDK and MinGW headers.
class RelayHResultError
{
public:
    explicit RelayHResultError(HRESULT error) noexcept : error_(error) {}

    HRESULT Error() const noexcept
    {
        return error_;
    }

private:
    HRESULT error_;
};
