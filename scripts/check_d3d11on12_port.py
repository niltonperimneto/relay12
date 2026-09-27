#!/usr/bin/env python3
"""Reject MSVC/ATL dependencies removed by the D3D11On12 port series.

Also holds the prepared tree to the port series' behavioural fixes that would
otherwise fail silently -- a dropped patch there still compiles.
"""

import argparse
import pathlib
import re


PROHIBITED = (
    (re.compile(r"\b_com_error\b"), "MSVC _com_error"),
    (re.compile(r"\bCComPtr\s*<"), "ATL CComPtr"),
    (re.compile(r"\bCComHeapPtr\s*<"), "ATL CComHeapPtr"),
)

INCLUDE_WITH_BACKSLASH = re.compile(
    r"^\s*#\s*include\s*[<\"][^>\"]*\\[^>\"]*[>\"]")


def check_source(path, text):
    errors = []
    for line_number, line in enumerate(text.splitlines(), 1):
        executable = line.split("//", 1)[0]
        if INCLUDE_WITH_BACKSLASH.search(executable):
            errors.append(
                f"{path}:{line_number}: include path uses a Windows "
                "separator")
        for pattern, dependency in PROHIBITED:
            if pattern.search(executable):
                errors.append(
                    f"{path}:{line_number}: executable use of {dependency}")
    return errors


# GetImmCtxArgs builds DTL's CreationArgs, which zero-initialise.  A zero
# MaxAllocatedUploadHeapSpacePerCommandList is not "use the default": DTL takes
# min(256 MB, field), so zero makes every upload trigger a submit.  Patch 0025
# sets it; this keeps a rebased series from quietly losing that.
IMM_CTX_ARGS = re.compile(
    r"CreationArgs\s+GetImmCtxArgs\s*\(.*?\breturn\s+args\s*;", re.S)
UPLOAD_LIMIT = re.compile(
    r"^\s*args\.MaxAllocatedUploadHeapSpacePerCommandList\s*=\s*(?P<value>[^;]+);",
    re.M)


def check_immediate_context_args(path, text):
    body = IMM_CTX_ARGS.search(text)
    if not body:
        return [f"{path}: GetImmCtxArgs not found"]
    limit = UPLOAD_LIMIT.search(body.group(0))
    if not limit or re.fullmatch(r"0+[uUlL]*", limit.group("value").strip()):
        return [f"{path}: GetImmCtxArgs must set a non-zero "
                "MaxAllocatedUploadHeapSpacePerCommandList (patch 0025)"]
    return []


def check_tree(source_dir):
    errors = []
    device = source_dir / "src" / "device.cpp"
    errors.extend(check_immediate_context_args(
        device, device.read_text(errors="replace")))
    for directory in (source_dir / "include", source_dir / "src"):
        for pattern in ("*.hpp", "*.cpp"):
            for path in sorted(directory.rglob(pattern)):
                errors.extend(check_source(path, path.read_text(errors="replace")))
    return errors


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("source_dir", type=pathlib.Path)
    args = parser.parse_args()
    errors = check_tree(args.source_dir)
    if errors:
        print("\n".join(errors))
        return 1
    print("D3D11On12 portability dependency gate: ok")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
