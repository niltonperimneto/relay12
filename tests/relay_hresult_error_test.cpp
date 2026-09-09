// SPDX-License-Identifier: GPL-3.0-only
#include <cstdint>
#include <cstdlib>
#include <stdexcept>

using HRESULT = std::int32_t;

#include "relay_hresult_error.hpp"

namespace
{
constexpr HRESULT S_OK = 0;
constexpr HRESULT E_FAIL = static_cast<HRESULT>(0x80004005u);
constexpr HRESULT E_INVALIDARG = static_cast<HRESULT>(0x80070057u);
constexpr HRESULT E_OUTOFMEMORY = static_cast<HRESULT>(0x8007000eu);

bool Failed(HRESULT result)
{
    return result < 0;
}

void throwFailure(HRESULT result)
{
    if (Failed(result))
    {
        throw RelayHResultError(result);
    }
}

HRESULT injectedBoundary(HRESULT injected)
{
    try
    {
        throwFailure(injected);
        return S_OK;
    }
    catch (RelayHResultError& error)
    {
        return error.Error();
    }
}

HRESULT successSentinelBoundary()
{
    try
    {
        // DTL deliberately throws S_OK when a deferred PSO is unavailable.
        throw RelayHResultError(S_OK);
    }
    catch (RelayHResultError& error)
    {
        return error.Error();
    }
}
}

int main()
{
    const HRESULT injected[] = {E_FAIL, E_INVALIDARG, E_OUTOFMEMORY};
    for (HRESULT result : injected)
    {
        if (injectedBoundary(result) != result)
        {
            return EXIT_FAILURE;
        }
    }

    if (injectedBoundary(S_OK) != S_OK || successSentinelBoundary() != S_OK)
    {
        return EXIT_FAILURE;
    }

    try
    {
        throw std::runtime_error("non-HRESULT fault");
    }
    catch (RelayHResultError&)
    {
        return EXIT_FAILURE;
    }
    catch (const std::runtime_error&)
    {
        return EXIT_SUCCESS;
    }

    return EXIT_FAILURE;
}
