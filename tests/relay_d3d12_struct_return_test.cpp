// SPDX-License-Identifier: GPL-3.0-only
//
// Contract test for the D3D12 struct-return wrappers.
//
// This is the shim whose mistakes are silent. Every other compatibility
// helper fails loudly when it is wrong -- a bad ownership type leaks or
// double-frees under a test, a bad HRESULT mapping returns the wrong code.
// A bad struct-return expansion compiles, returns a zeroed or stale
// descriptor, and the caller keeps going on it. Thirty call sites in
// patches/dtl/0007-compile-all-translation-units.patch read resource
// descriptions, descriptor heap starts and allocation info through these.
//
// The mock defines both ABI shapes itself rather than including the real
// d3d12.h. What is under test is the macro: which branch it selects, that the
// value comes back, and that nothing is evaluated twice. Whether the branch
// matches the real header's shape is settled by the DTL build compiling
// against DirectX-Headers, which no unit test can stand in for.
//
// Compiled twice natively, once per selector, and once in the MinGW/Wine
// lane, so neither branch can rot while the other is exercised.

#include <cassert>
#include <cstdio>

// Included before the mock, and before the type aliases below, because the
// mock's shape has to agree with the branch the header selects. Reading
// RELAY_D3D12_STRUCT_RETURN_VIA_OUT_PARAM ahead of this include saw it
// undefined, so on MinGW -- where the header defaults it to 1 -- the mock
// compiled the MSVC shape while the wrappers expanded to the out-parameter
// one. The wrappers are macros, so the D3D12 type names in their replacement
// lists are not looked up until a call site expands them; defining those
// aliases after this include is therefore still in time.
#include "relay_d3d12_struct_return.hpp"

#define D3D12_RESOURCE_DESC MockDesc
#define D3D12_HEAP_PROPERTIES MockDesc

namespace
{
    int failures = 0;

    void check(bool condition, const char* what)
    {
        if (condition)
        {
            std::printf("[ ok ] %s\n", what);
        }
        else
        {
            std::printf("[fail] %s\n", what);
            ++failures;
        }
    }

    // Stands in for the D3D12 types the wrappers return. A distinct value per
    // field so a wrapper that returned a default-constructed object, or wrote
    // into the wrong one, is visible rather than plausible.
    struct MockDesc
    {
        unsigned width = 0;
        unsigned height = 0;
    };

    const unsigned EXPECTED_WIDTH = 0xd3d12u;
    const unsigned EXPECTED_HEIGHT = 0x51ceu;

    // How many times the object expression was evaluated. A macro that named
    // its object argument twice would double this, and on a real call site
    // the argument is often a function call such as
    // pSrc->GetUnderlyingResource().
    int objectEvaluations = 0;
    int argumentEvaluations = 0;

    struct MockDevice
    {
        int descCalls = 0;
        int heapPropertyCalls = 0;
        unsigned lastNodeMask = 0;
        unsigned lastHeapType = 0;

#if RELAY_D3D12_STRUCT_RETURN_VIA_OUT_PARAM
        // The shape MinGW's generated headers expose: the hidden return slot
        // is an explicit leading parameter and the method returns a pointer
        // to it.
        MockDesc* GetDesc(MockDesc* out)
        {
            ++descCalls;
            out->width = EXPECTED_WIDTH;
            out->height = EXPECTED_HEIGHT;
            return out;
        }

        MockDesc* GetCustomHeapProperties(MockDesc* out, unsigned nodeMask,
                                          unsigned heapType)
        {
            ++heapPropertyCalls;
            lastNodeMask = nodeMask;
            lastHeapType = heapType;
            out->width = nodeMask;
            out->height = heapType;
            return out;
        }
#else
        // The shape MSVC presents: an ordinary by-value return.
        MockDesc GetDesc()
        {
            ++descCalls;
            MockDesc desc;
            desc.width = EXPECTED_WIDTH;
            desc.height = EXPECTED_HEIGHT;
            return desc;
        }

        MockDesc GetCustomHeapProperties(unsigned nodeMask, unsigned heapType)
        {
            ++heapPropertyCalls;
            lastNodeMask = nodeMask;
            lastHeapType = heapType;
            MockDesc properties;
            properties.width = nodeMask;
            properties.height = heapType;
            return properties;
        }
#endif
    };

    MockDevice device;

    // Returns the object, counting each evaluation.
    MockDevice* evaluateObject()
    {
        ++objectEvaluations;
        return &device;
    }

    unsigned evaluateArgument(unsigned value)
    {
        ++argumentEvaluations;
        return value;
    }
}

int main()
{
    std::printf("[note] selector RELAY_D3D12_STRUCT_RETURN_VIA_OUT_PARAM=%d\n",
                RELAY_D3D12_STRUCT_RETURN_VIA_OUT_PARAM);

    // The value the callee produced has to come back out of the macro. On the
    // out-parameter branch this is the whole point: the wrapper declares the
    // slot, passes it, and must return the filled object rather than the
    // pointer or a fresh default.
    MockDesc desc = RelayD3D12ResourceDesc(&device);
    check(desc.width == EXPECTED_WIDTH && desc.height == EXPECTED_HEIGHT,
          "the descriptor written by the callee comes back from the wrapper");
    check(device.descCalls == 1, "the underlying method ran exactly once");

    // Usable as a prvalue. ImmediateContext.cpp does exactly this:
    // RelayD3D12ResourceDesc(pResource).Width
    check(RelayD3D12ResourceDesc(&device).width == EXPECTED_WIDTH,
          "the result is a prvalue whose members are directly readable");

    // The object expression must be evaluated once. Real call sites pass
    // things like pSrc->GetUnderlyingResource(), so a macro naming the
    // argument twice would call into the tree twice per descriptor read.
    objectEvaluations = 0;
    device.descCalls = 0;
    MockDesc fromCall = RelayD3D12ResourceDesc(evaluateObject());
    check(objectEvaluations == 1,
          "the object expression is evaluated exactly once");
    check(device.descCalls == 1 && fromCall.width == EXPECTED_WIDTH,
          "evaluating once still produced the descriptor");

    // Trailing arguments must be single-evaluation too, and must arrive in
    // the right order -- the out-parameter branch inserts the hidden slot
    // ahead of them, which is exactly where an ordering mistake would hide.
    argumentEvaluations = 0;
    objectEvaluations = 0;
    MockDesc properties = RelayD3D12CustomHeapProperties(
            evaluateObject(), evaluateArgument(7u), evaluateArgument(9u));
    check(objectEvaluations == 1 && argumentEvaluations == 2,
          "object and trailing arguments are each evaluated once");
    check(device.lastNodeMask == 7u && device.lastHeapType == 9u,
          "trailing arguments arrive in order after the hidden return slot");
    check(properties.width == 7u && properties.height == 9u,
          "the heap properties written by the callee come back");

    if (failures)
    {
        std::printf("[fail] %d check(s) failed\n", failures);
        return 1;
    }
    std::printf("[ ok ] the struct-return wrappers preserve value and "
                "evaluation count\n");
    return 0;
}
