#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-only
#
# Reject D3D12 struct-returning calls that bypass the compatibility wrappers.
#
# COM methods returning a structure by value are declared differently by
# MSVC and by MinGW's generated headers, which expose the hidden return slot
# as an explicit leading parameter. compat/relay_d3d12_struct_return.hpp hides
# that difference behind one wrapper per method.
#
# On MinGW a bypassed call already fails to compile, because the signature
# does not match. This gate exists for the other direction: on the MSVC
# branch, and in review, a direct call reads as correct and would only break
# when someone builds the cross-compiled lane. Naming it here turns a
# cross-compile failure into a message that says which call and which file.
#
# What it deliberately does not check. A bare GetDesc is not in the list:
# D3D12TranslationLayer defines its own GetDesc() wrappers, on VideoDecode and
# VideoDecoderHeap among others, whose callers are correct and would be
# flagged by a textual rule that cannot see types. The five names below exist
# only on D3D12 interfaces and are not re-declared anywhere in the tree, so
# matching them textually is sound.
#
# Usage:
#   python3 scripts/check_dtl_struct_return.py PREPARED_SOURCE_DIR

import argparse
import pathlib
import re
import sys

SOURCE_SUFFIXES = {".h", ".hpp", ".inl", ".cpp"}

# Method, and the wrapper a call site is supposed to use instead.
WRAPPED_METHODS = {
    "GetCPUDescriptorHandleForHeapStart": "RelayD3D12CPUDescriptorHeapStart",
    "GetGPUDescriptorHandleForHeapStart": "RelayD3D12GPUDescriptorHeapStart",
    "GetCustomHeapProperties": "RelayD3D12CustomHeapProperties",
    "GetAdapterLuid": "RelayD3D12AdapterLuid",
    "GetResourceAllocationInfo": "RelayD3D12ResourceAllocationInfo",
}

DIRECT_CALL = re.compile(
    r"->\s*(" + "|".join(sorted(WRAPPED_METHODS)) + r")\s*\(")


def without_comments(text):
    """Remove C/C++ comments, preserving newlines so lines still line up."""
    def replace(match):
        return "\n" * match.group(0).count("\n")

    return re.sub(r"//[^\n]*|/[*].*?[*]/", replace, text, flags=re.DOTALL)


def is_relay_compat(relative):
    """Whether a path is one of relay12's own compatibility headers.

    The wrappers themselves contain the only legitimate direct calls, and the
    prepare script copies them into the tree being scanned.
    """
    return relative.name.startswith("relay_")


def check_source(path, text):
    """One message per direct call, naming the wrapper it should have used."""
    errors = []
    for number, line in enumerate(without_comments(text).splitlines(), 1):
        for match in DIRECT_CALL.finditer(line):
            method = match.group(1)
            errors.append(
                f"{path}:{number}: {method} returns a structure by value and "
                f"is declared differently by MSVC and MinGW; call it through "
                f"{WRAPPED_METHODS[method]}")
    return errors


def check_tree(source_dir):
    errors = []
    for path in sorted(source_dir.rglob("*")):
        if not path.is_file() or path.suffix not in SOURCE_SUFFIXES:
            continue
        relative = path.relative_to(source_dir)
        if is_relay_compat(relative):
            continue
        errors.extend(
            check_source(relative.as_posix(),
                         path.read_text(errors="replace")))
    return errors


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("source_dir", type=pathlib.Path)
    args = parser.parse_args()
    errors = check_tree(args.source_dir)
    if errors:
        print("\n".join(errors), file=sys.stderr)
        return 1
    print("DTL struct-return wrapper coverage: ok")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
