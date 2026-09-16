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

STRUCTS = ("PrivateCallbacks", "PrivateCallbacks2", "SOpenAdapterArgs",
           "SHADER_DESC")
VERSION_CONSTANT = "c_CurrentD3D11On12InterfaceVersion"

# The interface whose vtable order the core depends on, and the last slot it
# indexes. Everything above CreatePixelShader determines that index, so the
# comparison runs from the first method to this one inclusive and stops: the
# core deliberately does not transcribe what it never calls.
INTERFACE = "ID3D11On12DDIDevice"
INTERFACE_VTBL = "ID3D11On12DDIDeviceVtbl"
LAST_SLOT = "CreatePixelShader"
# The self parameter an explicit vtable needs and a C++ method does not.
THIS_PARAM = f"{INTERFACE}*This"

SAL = re.compile(
    r"\b_(?:In|Out|Inout|COM_Outptr|Outptr|Reserved|Pre|Post|Deref)"
    r"[A-Za-z0-9_]*_(?:\s*\([^()]*\))?")


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


def balanced(text, open_index):
    """The text inside the parentheses that start at `open_index`."""
    depth = 0
    for index in range(open_index, len(text)):
        if text[index] == "(":
            depth += 1
        elif text[index] == ")":
            depth -= 1
            if depth == 0:
                return text[open_index + 1:index], index
    return None, len(text)


def normalise_signature(return_type, name, params):
    """One method, reduced to what fixes its vtable slot and its ABI."""
    params = SAL.sub(" ", params)
    text = f"{return_type} {name}({params})"
    text = re.sub(r"\bCONST\b", "const", text)
    text = re.sub(r"\s+", " ", text).strip()
    text = re.sub(r"\s*\*\s*", "*", text)
    text = re.sub(r"\s*,\s*", ",", text)
    text = re.sub(r"\(\s*", "(", text)
    text = re.sub(r"\s*\)", ")", text)
    return text


def split_params(params):
    """Parameters at depth zero, so a function-pointer argument stays whole."""
    result = []
    depth = 0
    current = []
    for character in params:
        if character in "([<":
            depth += 1
        elif character in ")]>":
            depth -= 1
        if character == "," and depth == 0:
            result.append("".join(current))
            current = []
        else:
            current.append(character)
    tail = "".join(current)
    if tail.strip():
        result.append(tail)
    return result


def driver_methods(text):
    """The interface's methods, in declaration order, up to LAST_SLOT.

    Scanned for the STDMETHOD macros rather than split on semicolons: the
    interface opens with a static CastFrom whose inline body carries its own
    semicolon and no terminator, which a semicolon split would fold into the
    first real method.
    """
    match = re.search(rf"\binterface\s+{re.escape(INTERFACE)}\s*\{{", text)
    if not match:
        return None
    body = text[match.end():]
    pattern = re.compile(
        r"\bSTDMETHOD(_)?\s*\(\s*([^()]*?)\s*\)\s*\(")
    result = []
    for macro in pattern.finditer(body):
        if macro.group(1):
            # STDMETHOD_(type, name): an explicit return type.
            return_type, _, name = macro.group(2).rpartition(",")
            return_type = return_type.strip()
            name = name.strip()
        else:
            return_type, name = "HRESULT", macro.group(2).strip()
        params, _ = balanced(body, macro.end() - 1)
        if params is None:
            break
        result.append((name, normalise_signature(return_type, name, params)))
        if name == LAST_SLOT:
            break
    return result


def core_methods(text):
    """The core's vtable members, in order, with the self parameter removed."""
    body = struct_body(text, INTERFACE_VTBL)
    if body is None:
        return None
    pattern = re.compile(r"([A-Za-z_][\w:]*)\s*\(\s*STDMETHODCALLTYPE\s*\*\s*"
                         r"(\w+)\s*\)\s*\(")
    result = []
    for member in pattern.finditer(body):
        return_type, name = member.group(1), member.group(2)
        params, _ = balanced(body, member.end() - 1)
        if params is None:
            break
        parts = split_params(params)
        # Dropped, not compared: a C++ method's `this` is implicit, so the
        # explicit one is the only legitimate difference between the two
        # spellings. It still has to be there and be the right type.
        if not parts or re.sub(r"\s*\*\s*", "*",
                               re.sub(r"\s+", " ", parts[0]).strip()) \
                != THIS_PARAM:
            result.append((name, f"<{name} takes no {THIS_PARAM}>"))
            continue
        result.append((name, normalise_signature(return_type, name,
                                                 ",".join(parts[1:]))))
    return result


def check_interface(driver_text, core_text):
    """One message per way the vtable transcription has drifted."""
    errors = []
    driver = driver_methods(driver_text)
    core = core_methods(core_text)

    if driver is None:
        return [f"{INTERFACE}: not found in the pinned driver header; the "
                "driver was bumped and this gate no longer knows what to "
                "compare"]
    if core is None:
        return [f"{INTERFACE_VTBL}: not found in the core's transcription"]
    if not driver or driver[-1][0] != LAST_SLOT:
        return [f"{INTERFACE}: {LAST_SLOT} is no longer a method of the "
                "pinned interface, so the slot the core calls does not exist"]

    if driver == core:
        return errors

    errors.append(
        f"{INTERFACE}: the vtable transcription has drifted from "
        "interface/D3D11On12DDI.h. Every slot above "
        f"{LAST_SLOT} fixes its index, so this renumbers the shader "
        "creation calls")
    for index in range(max(len(driver), len(core))):
        expected = driver[index][1] if index < len(driver) else "<missing>"
        actual = core[index][1] if index < len(core) else "<missing>"
        if expected != actual:
            errors.append(f"  slot {index}: driver has {expected!r}, core has "
                          f"{actual!r}")
    return errors


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

    errors.extend(check_interface(driver, core))
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
