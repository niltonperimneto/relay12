#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-only
#
# The core-layer callback table handed to the driver must be complete.
#
# D3D10DDIARG_CREATEDEVICE.pWDDM2_6UMCallbacks is the runtime's half of the
# DDI contract.  The driver calls back through it and does not test the
# entries first: Microsoft's D3D11On12 Device::PostSubmit dereferences
# pfnPerformAmortizedProcessingCb on every batch flush.  A table that leaves
# it null does not degrade -- it crashes inside the driver, on the first
# frame, with a stack that names none of this code.
#
# That failure is also invisible to the rest of the build.  The table is a
# plain structure the core allocates zeroed, so an entry nobody assigns
# compiles, links, and passes every test that never reaches a flush.  The
# mock driver now rejects a null entry at CreateDevice, which catches it in
# the portable tests, but only for the entries the mock thinks to check and
# only on the paths the mock is wired into.  This gate reads the assignment.
#
# Four rules per mandatory entry:
#
#   1. It must name a member that exists in the pinned
#      D3DWDDM2_6DDI_CORELAYER_DEVICECALLBACKS.  Without this the list below
#      would silently stop checking anything the moment a field is renamed
#      upstream -- the failure mode every gate here is written to avoid.
#   2. The core must assign it, and must assign it a named handler rather
#      than a null literal.
#   3. That handler must be defined in the core, as a CALLBACK.  An
#      assignment naming a declaration that is never defined would not link,
#      but one naming an unrelated function would, so the definition is
#      matched rather than assumed.
#   4. The assignment must appear before pfnCreateDevice is called.  That
#      call is when the driver receives the table's address and may begin
#      using it; an assignment afterwards is a race the compiler will not
#      mention.
#
# Usage:
#   python3 scripts/check_core_callbacks.py \
#       [--header relay12-d3d11/ddi/wine_d3d11ddi.h] \
#       [--core relay12-d3d11/d3d11on12core.cpp]

import argparse
import pathlib
import re
import sys

from check_adapter_args import struct_body, without_comments

CALLBACKS = "D3DWDDM2_6DDI_CORELAYER_DEVICECALLBACKS"

# Mandatory because the driver dereferences these without checking, so the
# reason has to travel with the name: a bare list invites someone to prune it.
MANDATORY = {
    "pfnSetErrorCb":
        "the driver reports every DDI failure through it, so without it a "
        "rejected creation is indistinguishable from a successful one",
    "pfnPerformAmortizedProcessingCb":
        "D3D11On12's Device::PostSubmit dereferences it on every flush",
}

NULL_LITERALS = ("nullptr", "NULL", "0")


def check_core_callbacks(header_text, core_text):
    """Errors describing every mandatory core-layer callback the core omits."""
    header = without_comments(header_text)
    core = without_comments(core_text)
    members = struct_body(header, CALLBACKS)
    errors = []

    if members is None:
        return [f"core callbacks: the header declares no {CALLBACKS}"]

    # The first creation call, because that is the deadline for every entry.
    creation = core.find("pfnCreateDevice(")
    if creation < 0:
        errors.append("core callbacks: the core never calls pfnCreateDevice, "
                      "so the table is never handed to a driver")

    for name, why in sorted(MANDATORY.items()):
        if not re.search(rf"\b{re.escape(name)}\s*;", members):
            errors.append(
                f"core callbacks: {CALLBACKS} has no member {name}; this "
                "gate's mandatory list has drifted from the header")
            continue

        assignments = list(re.finditer(
            rf"coreCallbacks\.{re.escape(name)}\s*=\s*([A-Za-z_]\w*)\s*;",
            core))
        if not assignments:
            errors.append(f"core callbacks: {name} is never assigned -- {why}")
            continue

        handlers = {match.group(1) for match in assignments}
        for handler in sorted(handlers & set(NULL_LITERALS)):
            errors.append(
                f"core callbacks: {name} is assigned {handler} -- {why}")
        for handler in sorted(handlers - set(NULL_LITERALS)):
            if not re.search(rf"\bCALLBACK\s+{re.escape(handler)}\s*\(", core):
                errors.append(
                    f"core callbacks: {name} is assigned {handler}, which the "
                    "core does not define as a CALLBACK")

        if creation >= 0 and all(m.start() > creation for m in assignments):
            errors.append(
                f"core callbacks: {name} is assigned only after the driver's "
                "CreateDevice has already been given the table")

    return errors


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument(
        "--header", type=pathlib.Path,
        default=pathlib.Path("relay12-d3d11/ddi/wine_d3d11ddi.h"))
    parser.add_argument(
        "--core", type=pathlib.Path,
        default=pathlib.Path("relay12-d3d11/d3d11on12core.cpp"))
    args = parser.parse_args()

    errors = check_core_callbacks(
        args.header.read_text(encoding="utf-8", errors="replace"),
        args.core.read_text(encoding="utf-8", errors="replace"))
    if errors:
        print("\n".join(errors), file=sys.stderr)
        return 1

    print(f"core-layer callback table: ok ({len(MANDATORY)} mandatory "
          "callbacks)")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
