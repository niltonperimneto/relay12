// Copyright (c) relay12 contributors.
// SPDX-License-Identifier: GPL-3.0-only
#pragma once

#include <objbase.h>
#include "relay_ownership.hpp"

struct RelayCoTaskMemDeleter
{
    void operator()(void* pointer) const noexcept
    {
        CoTaskMemFree(pointer);
    }
};

template <typename T>
using RelayComHeapPtr = RelayHeapPtr<T, RelayCoTaskMemDeleter>;
