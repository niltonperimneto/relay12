#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-only
"""Prove clean-room DDI compiler dependencies exclude the SDK/WDK overlay."""

import argparse
import pathlib
import shlex
import sys


def dependencies(depfile):
    text = depfile.read_text(errors="replace").replace("\\\n", " ")
    _, separator, values = text.partition(":")
    if not separator:
        raise ValueError(f"{depfile}: malformed dependency file")
    return [pathlib.Path(value) for value in shlex.split(values)]


def check(overlay, depfiles):
    overlay = overlay.resolve()
    errors = []
    for depfile in depfiles:
        try:
            paths = dependencies(depfile)
        except (OSError, ValueError) as error:
            errors.append(str(error))
            continue
        for path in paths:
            resolved = path.resolve()
            if resolved == overlay or overlay in resolved.parents:
                errors.append(
                    f"{depfile}: clean-room compile resolved proprietary "
                    f"overlay dependency {resolved}")
    return errors


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--overlay", required=True, type=pathlib.Path)
    parser.add_argument("depfiles", nargs="+", type=pathlib.Path)
    args = parser.parse_args()
    errors = check(args.overlay, args.depfiles)
    if errors:
        print("\n".join(errors), file=sys.stderr)
        return 1
    print("clean-room DDI dependency isolation: ok")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
