#!/usr/bin/env python3
"""Reject MSVC/ATL dependencies removed by the D3D11On12 port series.

Also holds the prepared tree to the port series' behavioural fixes that would
otherwise fail silently -- a dropped patch there still compiles.  With
--dtl-source it checks the prepared D3D12TranslationLayer tree the driver is
built against too.
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


# Patch 0027: the non-blocking PSO switch is read from its compat value.  A
# dropped line compiles, leaves the bit zero, and silently makes
# D3D11ON12_COMPAT_NonBlockingPSOs=1 do nothing.
NON_BLOCKING_PSOS = re.compile(
    r"^\s*args\.UseNonBlockingPSOs\s*=\s*GetCompatValue\(\s*\"NonBlockingPSOs\"",
    re.M)


def check_immediate_context_args(path, text):
    body = IMM_CTX_ARGS.search(text)
    if not body:
        return [f"{path}: GetImmCtxArgs not found"]
    errors = []
    limit = UPLOAD_LIMIT.search(body.group(0))
    if not limit or re.fullmatch(r"0+[uUlL]*", limit.group("value").strip()):
        errors.append(f"{path}: GetImmCtxArgs must set a non-zero "
                      "MaxAllocatedUploadHeapSpacePerCommandList (patch 0025)")
    if not NON_BLOCKING_PSOS.search(body.group(0)):
        errors.append(f"{path}: GetImmCtxArgs must set UseNonBlockingPSOs "
                      "from the NonBlockingPSOs compat value (patch 0027)")
    return errors


# Patch 0026: without the environment fallback no compat value is reachable
# under Wine or D3DMetal, whose dxgi.dll does not export CompatValue.
COMPAT_VALUE = re.compile(
    r"bool\s+GetCompatValue\s*\(.*?\n\}", re.S)


def check_compat_value(path, text):
    body = COMPAT_VALUE.search(text)
    if not body:
        return [f"{path}: GetCompatValue not found"]
    if ("D3D11ON12_COMPAT_" not in body.group(0)
            or "GetEnvironmentVariableA" not in body.group(0)):
        return [f"{path}: GetCompatValue must fall back to "
                "D3D11ON12_COMPAT_<name> (patch 0026)"]
    return []


def function_body(text, name):
    match = re.search(
        r"ImmediateContext::" + name + r"\s*\(\s*\)[^{]*\{.*?\n\}", text, re.S)
    return match.group(0) if match else None


def check_dtl_pso_lookup(path, text):
    """DTL patch 0023: only draws may skip a PSO that is still compiling.

    PreDraw must reach TryGetForUse, or the switch does nothing.  PreDispatch
    must keep the blocking GetForUse: a skipped dispatch (culling, simulation)
    corrupts every later frame, not one.
    """
    errors = []
    draw = function_body(text, "PreDraw")
    dispatch = function_body(text, "PreDispatch")
    if draw is None or dispatch is None:
        return [f"{path}: PreDraw or PreDispatch not found"]
    if "TryGetForUse(" not in draw:
        errors.append(f"{path}: PreDraw must use TryGetForUse when "
                      "UseNonBlockingPSOs is set (dtl patch 0023)")
    if "TryGetForUse(" in dispatch:
        errors.append(f"{path}: PreDispatch must never skip a compute PSO; "
                      "it has to call the blocking GetForUse")
    elif not re.search(r"\bGetForUse\(", dispatch):
        errors.append(f"{path}: PreDispatch no longer calls GetForUse")
    return errors


def check_dtl_tree(dtl_dir):
    inline = dtl_dir / "include" / "ImmediateContext.inl"
    errors = check_dtl_pso_lookup(inline, inline.read_text(errors="replace"))
    # The blit and video-process PSOs are internal and must always wait.
    for directory in (dtl_dir / "include", dtl_dir / "src"):
        for path in sorted(directory.rglob("*")):
            if path.suffix not in (".hpp", ".inl", ".cpp") or path in (
                    inline, dtl_dir / "include" / "PipelineState.hpp"):
                continue
            if "TryGetForUse(" in path.read_text(errors="replace"):
                errors.append(f"{path}: TryGetForUse is for PreDraw only")
    return errors


def check_tree(source_dir, dtl_dir=None):
    errors = []
    device = source_dir / "src" / "device.cpp"
    errors.extend(check_immediate_context_args(
        device, device.read_text(errors="replace")))
    main = source_dir / "src" / "main.cpp"
    errors.extend(check_compat_value(main, main.read_text(errors="replace")))
    if dtl_dir is not None:
        errors.extend(check_dtl_tree(dtl_dir))
    for directory in (source_dir / "include", source_dir / "src"):
        for pattern in ("*.hpp", "*.cpp"):
            for path in sorted(directory.rglob(pattern)):
                errors.extend(check_source(path, path.read_text(errors="replace")))
    return errors


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("source_dir", type=pathlib.Path)
    parser.add_argument("--dtl-source", type=pathlib.Path,
                        help="prepared D3D12TranslationLayer tree")
    args = parser.parse_args()
    errors = check_tree(args.source_dir, args.dtl_source)
    if errors:
        print("\n".join(errors))
        return 1
    print("D3D11On12 portability dependency gate: ok")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
