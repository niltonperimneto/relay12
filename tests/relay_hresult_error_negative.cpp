// SPDX-License-Identifier: GPL-3.0-only
#include <cstdint>
#include <stdexcept>

using HRESULT = std::int32_t;

#include "relay_hresult_error.hpp"

int main()
{
    try
    {
        throw std::runtime_error("must cross the HRESULT-only boundary");
    }
    catch (RelayHResultError&)
    {
        // Reaching this return would mean the replacement widened the catch.
        return 0;
    }
}
