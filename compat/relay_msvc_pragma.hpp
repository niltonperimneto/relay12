// Copyright (c) relay12 contributors.
// SPDX-License-Identifier: GPL-3.0-only
#pragma once

// D3D11On12 suppresses MSVC's C4127 ("conditional expression is constant") at
// four DDI entry points, where the ENTRYPOINT_END_REPORT_HR_AND_RETURN_VALUE
// macro expands to an `if` on a constant. GCC has no such warning and rejects
// the pragma outright under -Werror=unknown-pragmas.
//
// Why this is a macro rather than an #ifdef around the pragma. MSVC's
// `warning(suppress: n)` applies to the next *line*, so bracketing it with
// #ifdef/#endif would put the #endif between the pragma and the statement it
// is meant to cover, and the suppression would silently stop working on the
// compiler that needs it -- a regression no build in this repository could
// catch, because none of them is MSVC. _Pragma keeps the pragma immediately
// adjacent to the statement on MSVC and expands to nothing everywhere else.
//
// Deliberately not covered by a contract test: on GCC the only property that
// matters is that it compiles in the position the call sites use, which the
// cross-compile already proves, and the MSVC behaviour cannot be exercised
// from this repository at all. A test here would assert what the build
// already asserts.
#ifdef _MSC_VER
#define RELAY_SUPPRESS_CONSTANT_CONDITIONAL _Pragma("warning(suppress: 4127)")
#else
#define RELAY_SUPPRESS_CONSTANT_CONDITIONAL
#endif
