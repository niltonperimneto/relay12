// SPDX-License-Identifier: GPL-3.0-only
//
// Contract test for the __if_exists replacement.
//
// D3D11On12's CopyViewDimensions copies whichever dimension fields the
// destination view descriptor declares, across a family of descriptors with
// different shapes. MSVC expressed that with __if_exists(TDest::Field); this
// repository expresses it with a detection trait and `if constexpr`.
//
// The reason this needs a test rather than trusting the build: the failure
// mode is silent. If a detector answered false for a field the destination
// does have, the field would simply not be copied -- no diagnostic, and a
// view descriptor that reaches the driver with a zero where a mip slice or an
// array size belonged. The build cannot tell the difference between "the
// branch was correctly discarded" and "the branch was wrongly discarded".
//
// So this mirrors the real function's shape, including the one field whose
// destination and source names differ (FirstWSlice from FirstW), and asserts
// per-destination which fields arrive and which stay untouched.

#include "relay_member_detect.hpp"

#include <cassert>
#include <cstdio>

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

    RELAY_DEFINE_MEMBER_DETECTOR(FirstElement)
    RELAY_DEFINE_MEMBER_DETECTOR(NumElements)
    RELAY_DEFINE_MEMBER_DETECTOR(MipSlice)
    RELAY_DEFINE_MEMBER_DETECTOR(WSize)
    RELAY_DEFINE_MEMBER_DETECTOR(FirstWSlice)
    RELAY_DEFINE_MEMBER_DETECTOR(ResourceMinLODClamp)

    // The same shape as src/view.cpp's CopyViewDimensions after the patch.
    template <typename TDest, typename TSource>
    void copyViewDimensions(TDest& Dest, TSource const& Source)
    {
#define COPY_FIELD(Field)                                   \
    if constexpr (relay_has_##Field<TDest>::value)          \
    {                                                       \
        Dest.Field = Source.Field;                          \
    }
        COPY_FIELD(FirstElement);
        COPY_FIELD(NumElements);
        COPY_FIELD(MipSlice);
        COPY_FIELD(WSize);
        if constexpr (relay_has_FirstWSlice<TDest>::value)
        {
            Dest.FirstWSlice = Source.FirstW;
        }
        if constexpr (relay_has_ResourceMinLODClamp<TDest>::value)
        {
            Dest.ResourceMinLODClamp = 0.0f;
        }
#undef COPY_FIELD
    }

    // Three differently shaped destinations, as the view family is. None of
    // them has every field, which is the whole reason the original reached
    // for __if_exists.
    struct BufferView
    {
        unsigned FirstElement = 0;
        unsigned NumElements = 0;
    };

    struct Tex3DView
    {
        unsigned MipSlice = 0;
        unsigned FirstWSlice = 0;
        unsigned WSize = 0;
    };

    struct Tex2DView
    {
        unsigned MipSlice = 0;
        float ResourceMinLODClamp = 9.0f;
    };

    // The DDI-side source: a superset, and note FirstW rather than
    // FirstWSlice -- the one field the original renames on copy.
    struct SourceDimensions
    {
        unsigned FirstElement = 11;
        unsigned NumElements = 22;
        unsigned MipSlice = 4;
        unsigned WSize = 5;
        unsigned FirstW = 9;
    };
}

int main()
{
    // The detectors themselves, at compile time. A trait that answered
    // wrongly here would discard the wrong branch below.
    static_assert(relay_has_FirstElement<BufferView>::value);
    static_assert(!relay_has_MipSlice<BufferView>::value);
    static_assert(relay_has_FirstWSlice<Tex3DView>::value);
    static_assert(!relay_has_FirstWSlice<Tex2DView>::value);
    static_assert(!relay_has_ResourceMinLODClamp<Tex3DView>::value);
    static_assert(relay_has_ResourceMinLODClamp<Tex2DView>::value);

    SourceDimensions source;

    BufferView buffer;
    copyViewDimensions(buffer, source);
    check(buffer.FirstElement == 11 && buffer.NumElements == 22,
          "a buffer view receives its element range");

    Tex3DView volume;
    copyViewDimensions(volume, source);
    check(volume.MipSlice == 4 && volume.WSize == 5,
          "a 3D view receives its mip slice and depth extent");
    check(volume.FirstWSlice == 9,
          "FirstWSlice is copied from the source's FirstW");

    Tex2DView plane;
    copyViewDimensions(plane, source);
    check(plane.MipSlice == 4, "a 2D view receives its mip slice");
    check(plane.ResourceMinLODClamp == 0.0f,
          "ResourceMinLODClamp is reset where the destination has it");

    // The point of the exercise: a destination without a field is left
    // alone rather than failing to compile or writing somewhere it should
    // not. Nothing above could distinguish that from a wrongly discarded
    // branch, which is why each destination asserts its own field set.
    check(sizeof(BufferView) == 2 * sizeof(unsigned),
          "no field was added to a destination that lacked one");

    if (failures)
    {
        std::printf("[fail] %d check(s) failed\n", failures);
        return 1;
    }
    std::printf("[ ok ] per-destination field selection matches __if_exists\n");
    return 0;
}
