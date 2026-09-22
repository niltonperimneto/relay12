// Copyright (c) relay12 contributors.
// SPDX-License-Identifier: GPL-3.0-only
#pragma once

// Supplies the unsigned SIZE_T-safe arithmetic helpers the pinned
// llvm-mingw's <intsafe.h> does not declare. Microsoft's own intsafe.h
// declares both the signed SSIZET* family and the unsigned SIZET* family;
// this MinGW header ships only the former. Must be included after
// <intsafe.h>, which already defines INTSAFE_E_ARITHMETIC_OVERFLOW and
// SIZE_T -- this header does not redefine either.
//
// Deliberately not guarded against a future toolchain adding these: if a
// pin bump ever does, the resulting redefinition error is a louder and
// more useful signal than a silent, unverified guard would be, and is the
// point at which this header should simply be deleted.

inline HRESULT SIZETAdd(SIZE_T Augend, SIZE_T Addend, SIZE_T *pResult)
{
    // Unsigned overflow wraps rather than invoking undefined behavior, so
    // the sum can be formed first and then checked: an overflowing
    // addition always yields a result smaller than either operand.
    const SIZE_T sum = Augend + Addend;

    if (sum < Augend)
    {
        *pResult = static_cast<SIZE_T>(-1);
        return INTSAFE_E_ARITHMETIC_OVERFLOW;
    }
    *pResult = sum;
    return S_OK;
}

inline HRESULT SIZETMult(SIZE_T Multiplicand, SIZE_T Multiplier, SIZE_T *pResult)
{
    // Checked before multiplying, not after: this avoids relying on
    // wraparound semantics for the overflow test itself, and avoids ever
    // dividing by a zero multiplier.
    if (Multiplicand != 0
            && Multiplier > static_cast<SIZE_T>(-1) / Multiplicand)
    {
        *pResult = static_cast<SIZE_T>(-1);
        return INTSAFE_E_ARITHMETIC_OVERFLOW;
    }
    *pResult = Multiplicand * Multiplier;
    return S_OK;
}
