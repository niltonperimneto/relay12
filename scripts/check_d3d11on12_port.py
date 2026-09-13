#!/usr/bin/env python3
"""Reject MSVC/ATL dependencies removed by the D3D11On12 port series."""

import argparse
import pathlib
import re


PROHIBITED = (
    (re.compile(r"\b_com_error\b"), "MSVC _com_error"),
    (re.compile(r"\bCComPtr\s*<"), "ATL CComPtr"),
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


def check_tree(source_dir):
    errors = []
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
