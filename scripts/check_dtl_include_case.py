#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-only
"""Reject local DTL includes whose spelling differs from the real filename."""

import argparse
import collections
import pathlib
import re
import sys

SOURCE_SUFFIXES = {".c", ".cc", ".cpp", ".cxx", ".h", ".hpp", ".inl"}
INCLUDE = re.compile(r'^\s*#\s*include\s*[<"]([^>"]+)[>"]')


def without_comments(text):
    """Remove C/C++ comments while retaining line numbers."""
    def replace(match):
        return "\n" * match.group(0).count("\n")

    return re.sub(r"//[^\n]*|/[*].*?[*]/", replace, text, flags=re.DOTALL)


def filename_index(source_dir):
    """Map a case-folded basename to every spelling present in the tree."""
    index = collections.defaultdict(set)
    for path in source_dir.rglob("*"):
        if path.is_file():
            index[path.name.casefold()].add(path.name)
    return index


def check_tree(source_dir):
    index = filename_index(source_dir)
    errors = []
    for path in sorted(source_dir.rglob("*")):
        if not path.is_file() or path.suffix.lower() not in SOURCE_SUFFIXES:
            continue
        relative = path.relative_to(source_dir).as_posix()
        text = without_comments(path.read_text(errors="replace"))
        for number, line in enumerate(text.splitlines(), 1):
            match = INCLUDE.match(line)
            if not match:
                continue
            requested = pathlib.PurePosixPath(match.group(1).replace("\\", "/")).name
            spellings = sorted(index.get(requested.casefold(), ()))
            # No local case-insensitive match means this is an external header.
            if not spellings or requested in spellings:
                continue
            errors.append(
                f"{relative}:{number}: include {requested!r} does not match "
                f"local filename spelling: {', '.join(spellings)}")
    return errors


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("source_dir", type=pathlib.Path)
    args = parser.parse_args()
    errors = check_tree(args.source_dir)
    if errors:
        print("\n".join(errors), file=sys.stderr)
        return 1
    print("DTL local include filename case: ok")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
