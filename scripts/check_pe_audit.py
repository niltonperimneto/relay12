#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-only
#
# Audit the built PE modules' exports, imports and runtime dependencies.
#
# These rules were inline in .github/workflows/pull-request.yml, where they
# could not be run before pushing and could not be tested at all. A gate with
# no test passes everything the day its pattern stops matching, which for an
# export-ordinal check means the router silently stops looking like Apple's
# forwarder.
#
# What each rule is for:
#
#   exports  Apple's d3d11.dll exports exactly three entry points at ordinals
#            1 to 3, and an application may bind to them by ordinal.  The
#            router has to present the same table, with the Wine-private
#            status entry point above it rather than among it.
#   imports  Both modules resolve __wine_dbg_output at run time instead of
#            linking it, which is what keeps their import tables down to
#            kernel32 and the C runtime.  If that ever became a real import,
#            this is what says so.
#   runtime  A libstdc++ or libgcc_s dependency would make these modules
#            undeployable in a Wine prefix, and it appears by accident: one
#            unguarded C++ construct is enough.
#
# Usage:
#   python3 scripts/check_pe_audit.py d3d11shim.dll d3d11on12core.dll
#   python3 scripts/check_pe_audit.py --runtime-only d3d11on12coretest.exe

import argparse
import os
import re
import subprocess
import sys

DEFAULT_OBJDUMP = "x86_64-w64-mingw32-objdump"

# Ordinals 1 to 3 are Apple's exact export table.  The fourth is the
# Wine-private status entry point deployment gates on, because
# DXGI_ERROR_UNSUPPORTED cannot distinguish "unsupported here" from "installed
# wrong".  It is named so it cannot collide with a future Apple export.
EXPECTED_EXPORTS = {
    "d3d11shim.dll": {
        1: "D3D11CreateDevice",
        2: "D3D11CreateDeviceAndSwapChain",
        3: "D3D11On12CreateDevice",
        4: "WineD3D11ShimGetStatus",
    },
    # Transcribed from relay12-d3d11/d3d11on12core.def, deliberately rather
    # than parsed from it.  Fifteen Wine frontend patches resolve these by
    # name through GetProcAddress, and the ordinals are a published contract,
    # so this table is an independent statement of the intended ABI: a .def
    # that reorders or renumbers keeps every name present and still breaks
    # ordinal-bound consumers, and comparing the build against the .def would
    # only prove the linker honoured whatever it was handed.
    #
    # Two layers, because the transcription can go stale -- it sat at four
    # entries here while the .def had grown to eighteen.  This table is what
    # the built DLL is audited against; the test that the transcription still
    # matches the .def is test_the_expected_tables_match_the_def_files.  So a
    # new or moved export fails loudly and has to be adopted here on purpose,
    # which is the intent, rather than being picked up silently.
    "d3d11on12core.dll": {
        1: "WineD3D11On12GetABIVersion",
        2: "WineD3D11On12CreateDeviceV1",
        3: "WineD3D11On12GetInterface",
        4: "WineD3D11On12OpenAdapterV1",
        5: "WineD3D11On12CreateBufferV1",
        6: "WineD3D11On12DestroyBufferV1",
        7: "WineD3D11On12SetVertexBuffersV1",
        8: "WineD3D11On12SetIndexBufferV1",
        9: "WineD3D11On12CreateInputLayoutV1",
        10: "WineD3D11On12DestroyInputLayoutV1",
        11: "WineD3D11On12SetInputLayoutV1",
        12: "WineD3D11On12CreateVertexShaderV1",
        13: "WineD3D11On12CreatePixelShaderV1",
        14: "WineD3D11On12DestroyShaderV1",
        15: "WineD3D11On12SetVertexShaderV1",
        16: "WineD3D11On12SetPixelShaderV1",
        17: "WineD3D11On12CreateTexture2DV1",
        18: "WineD3D11On12DestroyTexture2DV1",
        19: "WineD3D11On12CreateRenderTargetViewV1",
        20: "WineD3D11On12DestroyRenderTargetViewV1",
        21: "WineD3D11On12SetRenderTargetV1",
        22: "WineD3D11On12SetViewportV1",
        23: "WineD3D11On12ClearRenderTargetV1",
        24: "WineD3D11On12CopyTexture2DV1",
        25: "WineD3D11On12MapTexture2DV1",
        26: "WineD3D11On12UnmapTexture2DV1",
        27: "WineD3D11On12CheckFrameSupportV1",
        28: "WineD3D11On12CreateWrappedTexture2DV1",
        29: "WineD3D11On12SetWrappedOwnershipV1",
        30: "WineD3D11On12CreateDeferredContextV1",
        31: "WineD3D11On12DestroyDeferredContextV1",
        32: "WineD3D11On12CreateCommandListV1",
        33: "WineD3D11On12DestroyCommandListV1",
        34: "WineD3D11On12ExecuteCommandListV1",
        35: "WineD3D11On12CreateBlendStateV1",
        36: "WineD3D11On12CreateDepthStencilStateV1",
        37: "WineD3D11On12CreateRasterizerStateV1",
        38: "WineD3D11On12DestroyPipelineStateV1",
        39: "WineD3D11On12SetBlendStateV1",
        40: "WineD3D11On12SetDepthStencilStateV1",
        41: "WineD3D11On12SetRasterizerStateV1",
    },
}

# kernel32 plus one C runtime, and nothing else.  Which C runtime depends on
# the toolchain build: llvm-mingw publishes an msvcrt and a ucrt variant of
# every release, and the two spell the same snprintf/fputs/fflush calls as
# either msvcrt.dll or a set of api-ms-win-crt-* stub libraries.  Both are
# accepted because the choice belongs to whoever pins the toolchain, and the
# rule this gate exists to enforce is unaffected by it: what must not appear
# is a third-party or graphics import, which would mean __wine_dbg_output or
# a driver entry point stopped being resolved at run time.
#
# A Wine prefix has to supply whichever set is used.  msvcrt is a Wine
# builtin; the api-ms-win-crt-* stubs are builtins too in current Wine, which
# is why the ucrt variant is deployable, but it is eight modules of surface
# rather than two.
CRT_IMPORTS = (
    {"msvcrt.dll"},
    {
        "api-ms-win-crt-environment-l1-1-0.dll",
        "api-ms-win-crt-heap-l1-1-0.dll",
        "api-ms-win-crt-private-l1-1-0.dll",
        "api-ms-win-crt-runtime-l1-1-0.dll",
        "api-ms-win-crt-stdio-l1-1-0.dll",
        "api-ms-win-crt-string-l1-1-0.dll",
        "api-ms-win-crt-time-l1-1-0.dll",
    },
)

EXPECTED_IMPORTS = {
    "d3d11shim.dll": tuple({"kernel32.dll"} | crt for crt in CRT_IMPORTS),
    "d3d11on12core.dll": tuple({"kernel32.dll"} | crt for crt in CRT_IMPORTS),
}

CXX_RUNTIME = re.compile(r"libstdc\+\+|libgcc_s", re.IGNORECASE)


def parse_exports(text):
    """Ordinal to name, from `objdump -p` output.

    Two output formats are accepted, because the toolchain changed under
    this gate once already and it failed silently rather than loudly.  GNU
    binutils objdump prints a name-pointer table indexed from zero with the
    ordinal base stated separately, so an export's ordinal is the sum;
    reading the indices as ordinals would pass a module whose base was not
    1.  llvm-objdump, which is what `x86_64-w64-mingw32-objdump` resolves to
    under llvm-mingw, prints each entry's ordinal directly instead.

    Neither format matching is an error, never an empty result.  Returning
    {} for unrecognised output is what let the llvm-mingw migration turn
    this check into a no-op: the caller compared an empty table and got a
    silent exit code with nothing to read.
    """
    if "[Ordinal/Name Pointer] Table" in text:
        base_match = re.search(r"Ordinal Base\s+(\d+)", text)
        if not base_match:
            raise ValueError("no export ordinal base in the objdump output")
        base = int(base_match.group(1))

        _, _, after = text.partition("[Ordinal/Name Pointer] Table")
        table = after.split("\n\n", 1)[0]
        return {
            base + int(index): name
            for index, name in re.findall(r"\[\s*(\d+)\]\s+(\S+)", table)
        }

    if "Export Table:" in text:
        # llvm-objdump prints " Ordinal      RVA  Name" and then one row per
        # export.  Forwarders have no RVA column, so the name is optional in
        # the row pattern and rows without one are skipped.
        _, _, after = text.partition("Export Table:")
        table = after.split("\n\n", 1)[0]
        exports = {}
        for line in table.splitlines():
            row = re.match(r"\s*(\d+)\s+(?:0x[0-9a-fA-F]+)?\s*(\S+)?\s*$",
                           line)
            if row and row.group(2):
                exports[int(row.group(1))] = row.group(2)
        return exports

    raise ValueError(
        "no export table in the objdump output; neither the GNU binutils "
        "'[Ordinal/Name Pointer] Table' nor the llvm-objdump 'Export Table:' "
        "heading was present, so the objdump in use prints a third format "
        "this gate cannot read")


def parse_imports(text):
    """The set of imported DLL names, lowercased."""
    return {name.lower() for name in re.findall(r"DLL Name:\s+(\S+)", text)}


def find_cxx_runtime(text):
    """The lines naming a C++ runtime dependency, if any."""
    return [line for line in text.splitlines() if CXX_RUNTIME.search(line)]


def audit(path, text, runtime_only, require_export=None):
    module = os.path.basename(path)
    errors = []

    for line in find_cxx_runtime(text):
        errors.append(f"{module}: unexpected C++ runtime dependency: "
                      f"{line.strip()}")

    if require_export:
        # For modules with no recorded ordinal table of their own -- the
        # linked driver, whose export list is upstream's -- assert only that
        # the one entry point the host resolves is present, exactly once.
        try:
            exports = parse_exports(text)
        except ValueError as error:
            return errors + [f"{module}: {error}"]
        found = sorted(o for o, n in exports.items() if n == require_export)
        if len(found) != 1:
            errors.append(
                f"{module}: exports {require_export} {len(found)} times, "
                f"expected exactly once (exports: {sorted(exports.values())})")
        return errors

    if runtime_only:
        return errors

    if module not in EXPECTED_EXPORTS:
        return errors + [
            f"{module}: no expected export table is recorded for this module; "
            "add one to scripts/check_pe_audit.py or pass --runtime-only"
        ]

    try:
        exports = parse_exports(text)
    except ValueError as error:
        return errors + [f"{module}: {error}"]

    if exports != EXPECTED_EXPORTS[module]:
        errors.append(f"{module}: exports {exports}, expected "
                      f"{EXPECTED_EXPORTS[module]}")

    imports = parse_imports(text)
    accepted = EXPECTED_IMPORTS[module]
    if imports not in accepted:
        errors.append(
            f"{module}: imports {sorted(imports)}, expected one of "
            + " or ".join(str(sorted(option)) for option in accepted))

    return errors


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("modules", nargs="+", metavar="MODULE")
    parser.add_argument("--objdump", default=DEFAULT_OBJDUMP,
                        help=f"objdump to run (default: {DEFAULT_OBJDUMP})")
    parser.add_argument("--runtime-only", action="store_true",
                        help="check only for a C++ runtime dependency")
    parser.add_argument("--require-export", metavar="NAME",
                        help="assert NAME is exported exactly once, instead "
                             "of comparing a recorded ordinal table")
    args = parser.parse_args()

    errors = []
    for path in args.modules:
        try:
            text = subprocess.check_output([args.objdump, "-p", path],
                                           text=True)
        except (OSError, subprocess.CalledProcessError) as error:
            errors.append(f"{path}: could not read the PE headers: {error}")
            continue
        errors.extend(audit(path, text, args.runtime_only,
                            args.require_export))

    if errors:
        for error in errors:
            print(error, file=sys.stderr)
        return 1

    checked = ", ".join(os.path.basename(path) for path in args.modules)
    print(f"pe audit: ok ({checked})")
    return 0


if __name__ == "__main__":
    sys.exit(main())
