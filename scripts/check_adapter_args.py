#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-only
#
# Pin the transcribed adapter-arguments subset against the pinned driver.
#
# relay12-d3d11/d3d11on12core.cpp carries its own copy of SOpenAdapterArgs,
# PrivateCallbacks, PrivateCallbacks2 and c_CurrentD3D11On12InterfaceVersion
# rather than including interface/D3D11On12DDI.h. It has to: that header's
# ID3D11On12DDIDevice declarations name D3DKMT_PRESENT, D3DKMT_HANDLE and
# D3D11DDIARG_CREATEGEOMETRYSHADERWITHSTREAMOUTPUT, which the clean-room
# header does not author and this host never uses. The driver compiles it
# with the licensed SDK overlay on its include path; the core must not have
# that path.
#
# A copy is a drift hazard, and this one is worse than most. The structure is
# passed by address across a module boundary to code compiled separately, so a
# field added upstream would not fail to compile here -- the driver would read
# past the end of a structure this side believes it filled. Nothing else in
# the build would notice.
#
# So the copy is compared to the original on every run. The comparison is on
# normalised member text, in order, which catches a field added, removed,
# reordered, or retyped. Normalisation drops what may legitimately differ
# between the two spellings and nothing else:
#
#   * SAL annotations, which the driver's header carries and a GPL
#     transcription need not;
#   * whitespace, line wrapping, and pointer spacing;
#   * comments;
#   * default member initialisers, which do not affect layout.
#
# Usage:
#   python3 scripts/check_adapter_args.py \
#       [--driver third_party/D3D11On12/interface/D3D11On12DDI.h] \
#       [--core relay12-d3d11/d3d11on12core.cpp]

import argparse
import pathlib
import re
import sys

STRUCTS = ("PrivateCallbacks", "PrivateCallbacks2", "SOpenAdapterArgs")
VERSION_CONSTANT = "c_CurrentD3D11On12InterfaceVersion"

SAL = re.compile(
    r"\b_(?:In|Out|Inout)_(?:opt_)?(?:reads_|writes_|ecount_|bcount_)?"
    r"(?:\([^)]*\))?")


def without_comments(text):
    """Remove C/C++ comments, preserving newlines so locations still line up."""
    def replace(match):
        return "\n" * match.group(0).count("\n")

    return re.sub(r"//[^\n]*|/[*].*?[*]/", replace, text, flags=re.DOTALL)


def struct_body(text, name):
    """The text between the braces of `struct <name> { ... };`.

    Found by brace matching rather than by a regex, because the bodies contain
    function-pointer members whose parameter lists a non-matching pattern
    would run past.
    """
    match = re.search(rf"\bstruct\s+{re.escape(name)}\s*\{{", text)
    if not match:
        return None
    depth = 0
    start = match.end() - 1
    for index in range(start, len(text)):
        if text[index] == "{":
            depth += 1
        elif text[index] == "}":
            depth -= 1
            if depth == 0:
                return text[start + 1:index]
    return None


def normalise_member(member):
    """One member, reduced to what affects the ABI."""
    member = SAL.sub(" ", member)
    # A default initialiser is not layout. Cut at the top-level '=' only, so a
    # function pointer's parameter list is untouched.
    depth = 0
    for index, character in enumerate(member):
        if character in "([":
            depth += 1
        elif character in ")]":
            depth -= 1
        elif character == "=" and depth == 0:
            member = member[:index]
            break
    member = re.sub(r"\s+", " ", member).strip()
    # Pointer and reference spelling is the author's choice, not the ABI's.
    member = re.sub(r"\s*\*\s*", "*", member)
    # `HRESULT(CALLBACK*...` and `HRESULT (CALLBACK*...` are the same
    # declaration; only the author's spacing differs.
    member = re.sub(r"\s+\(", "(", member)
    member = re.sub(r"\(\s*", "(", member)
    member = re.sub(r"\s*\)", ")", member)
    member = re.sub(r"\s*,\s*", ",", member)
    return member


def members(body):
    """Members in declaration order, normalised.

    Split on semicolons at depth zero so a function pointer's parameter list
    does not split the member that contains it.
    """
    result = []
    depth = 0
    current = []
    for character in body:
        if character in "([{":
            depth += 1
        elif character in ")]}":
            depth -= 1
        if character == ";" and depth == 0:
            member = normalise_member("".join(current))
            if member:
                result.append(member)
            current = []
        else:
            current.append(character)
    return result


def version_constant(text):
    match = re.search(
        rf"\b{VERSION_CONSTANT}\s*=\s*(\d+)", text)
    return match.group(1) if match else None


def check(driver_text, core_text):
    """One message per way the copy has drifted from the original."""
    errors = []
    driver = without_comments(driver_text)
    core = without_comments(core_text)

    for name in STRUCTS:
        driver_body = struct_body(driver, name)
        core_body = struct_body(core, name)
        if driver_body is None:
            errors.append(
                f"{name}: not found in the pinned driver header; the driver "
                "was bumped and this gate no longer knows what to compare")
            continue
        if core_body is None:
            errors.append(f"{name}: not found in the core's transcription")
            continue

        driver_members = members(driver_body)
        core_members = members(core_body)
        if driver_members == core_members:
            continue

        errors.append(f"{name}: the transcription has drifted from "
                      "interface/D3D11On12DDI.h")
        for index in range(max(len(driver_members), len(core_members))):
            expected = (driver_members[index]
                        if index < len(driver_members) else "<missing>")
            actual = (core_members[index]
                      if index < len(core_members) else "<missing>")
            if expected != actual:
                errors.append(f"  member {index}: driver has {expected!r}, "
                              f"core has {actual!r}")

    driver_version = version_constant(driver)
    core_version = version_constant(core)
    if driver_version is None:
        errors.append(f"{VERSION_CONSTANT}: not found in the pinned driver "
                      "header")
    elif driver_version != core_version:
        errors.append(
            f"{VERSION_CONSTANT}: driver says {driver_version}, core says "
            f"{core_version}. The driver dereferences Callbacks2 at version 7 "
            "and above, so this is not a cosmetic difference")
    return errors


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument(
        "--driver",
        type=pathlib.Path,
        default=pathlib.Path(
            "third_party/D3D11On12/interface/D3D11On12DDI.h"))
    parser.add_argument(
        "--core",
        type=pathlib.Path,
        default=pathlib.Path("relay12-d3d11/d3d11on12core.cpp"))
    args = parser.parse_args()

    errors = check(args.driver.read_text(errors="replace"),
                   args.core.read_text(errors="replace"))
    if errors:
        print("\n".join(errors), file=sys.stderr)
        return 1
    print("adapter-arguments transcription: ok")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
