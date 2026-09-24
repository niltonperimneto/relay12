#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-only
"""Compile clean-room enum values against the separately built driver's ABI.

Only integer expectations cross the boundary. The probe uses the real driver's
include configuration; the clean-room host never includes licensed headers.
"""
import argparse
import json
from pathlib import Path
import re
import shlex
import subprocess
import tempfile


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("compile_commands", type=Path)
    parser.add_argument("--header", type=Path, default=Path(__file__).resolve().parents[1]
                        / "relay12-d3d11/ddi/wine_d3d11ddi.h")
    args = parser.parse_args()
    entries = json.loads(args.compile_commands.read_text())
    entry = next(item for item in entries if Path(item["file"]).name == "resource.cpp")
    original = entry.get("arguments") or shlex.split(entry["command"])
    if "-DDYNAMIC_LOAD_DXCORE=1" not in original:
        parser.error("driver compile is missing DYNAMIC_LOAD_DXCORE=1; DTL class layout differs")
    command = []
    skip = False
    for value in original:
        if skip:
            skip = False
            continue
        if value == "-o":
            skip = True
        elif value not in ("-c", entry["file"]):
            command.append(value)
    header = args.header.read_text()
    names = ("D3D10DDIRESOURCE_BUFFER", "D3D10DDIRESOURCE_TEXTURE1D",
             "D3D10DDIRESOURCE_TEXTURE2D", "D3D10DDIRESOURCE_TEXTURE3D",
             "D3D10DDIRESOURCE_TEXTURECUBE", "D3D11DDIRESOURCE_BUFFEREX")
    # This flag adds m_DXCore to ImmediateContext. A PRIVATE definition in
    # DTL previously gave its consumer different offsets for every later field.
    assertions = ['static_assert(DYNAMIC_LOAD_DXCORE == 1, "DTL consumer class layout mismatch");']
    for name in names:
        match = re.search(r"^#define " + name + r" (\d+)u$", header, re.M)
        if not match:
            parser.error(f"missing numeric clean-room constant: {name}")
        assertions.append(f'static_assert({name} == {match[1]}, "{name} ABI mismatch");')
    # Integer layout facts only cross the clean-room/driver boundary.
    for expression, expected in (("sizeof(D3D10DDI_HKMRESOURCE)", 4),
            ("alignof(D3D10DDI_HKMRESOURCE)", 4),
            ("sizeof(D3D10DDIARG_OPENRESOURCE)", 40),
            ("offsetof(D3D10DDIARG_OPENRESOURCE, hKMResource)", 16),
            ("offsetof(D3D10DDIARG_OPENRESOURCE, pPrivateDriverData)", 24),
            ("offsetof(D3D10DDIARG_OPENRESOURCE, PrivateDriverDataSize)", 32)):
        assertions.append(f'static_assert({expression} == {expected}, "wrapped-resource ABI mismatch");')
    with tempfile.TemporaryDirectory(prefix="relay12-resource-abi-") as directory:
        probe = Path(directory) / "resource_abi.cpp"
        probe.write_text('#include "pch.hpp"\n' + "\n".join(assertions) + "\n")
        subprocess.run(command + ["-fsyntax-only", str(probe)],
                       cwd=entry["directory"], check=True)
    print("driver resource-kind ABI: all six clean-room values agree")


if __name__ == "__main__":
    main()
