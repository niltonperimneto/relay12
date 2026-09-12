// Copyright (c) relay12 contributors.
// SPDX-License-Identifier: GPL-3.0-only
#pragma once

#include <type_traits>

// A stand-in for MSVC's __if_exists, which GCC cannot parse at all.
//
// D3D11On12's CopyViewDimensions copies whichever dimension fields the
// destination descriptor happens to declare, across a family of view
// descriptors with different shapes. MSVC expresses that with
// __if_exists(TDest::Field), a compile-time member-existence test with no
// equivalent outside MSVC and no way to emulate it in the preprocessor.
//
// The standard spelling is a detection trait plus `if constexpr`. Both are
// C++17, and MSVC supports them too, so the call sites end up with one code
// path for every compiler rather than an #ifdef with a GCC arm nobody
// exercises on Windows and an MSVC arm nobody exercises here.
//
// Declare a detector once per member name, at namespace scope:
//
//     RELAY_DEFINE_MEMBER_DETECTOR(MipSlice)
//
// then test it where __if_exists would have been:
//
//     if constexpr (relay_has_MipSlice<TDest>::value) { ... }
//
// Inside a template this discards the branch without instantiating it, which
// is what makes a reference to a member the type does not have legal --
// exactly the property __if_exists provided.
//
// The detector deliberately tests only the type it is asked about, matching
// __if_exists(TDest::Field). Where a destination field exists but the source
// lacks it, this fails to compile just as the MSVC original did; that is a
// real mismatch rather than something to paper over.
#define RELAY_DEFINE_MEMBER_DETECTOR(Member)                                  \
    template <typename RelayDetectT, typename = void>                         \
    struct relay_has_##Member : std::false_type                               \
    {                                                                         \
    };                                                                        \
    template <typename RelayDetectT>                                          \
    struct relay_has_##Member<                                                \
            RelayDetectT,                                                     \
            std::void_t<decltype(std::declval<RelayDetectT &>().Member)>>     \
        : std::true_type                                                      \
    {                                                                         \
    };
