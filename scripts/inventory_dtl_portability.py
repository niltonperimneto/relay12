#!/usr/bin/env python3
"""Inventory known MinGW portability blockers in pinned D3D12TranslationLayer."""

import argparse
import json
import pathlib
import re


EXPECTED_REVISION = "b68ebbc6dab4195f7d4819ea78f2a60c985b853a"
SOURCE_SUFFIXES = {".h", ".hpp", ".inl", ".cpp", ".txt"}
PATTERNS = {
    "atl_headers": re.compile(r"#\s*include\s*[<\"]atl(?:base|com)[.]h[>\"]"),
    "atl_com_ptr": re.compile(r"\bCComPtr\s*<"),
    "atl_heap_ptr": re.compile(r"\bCComHeapPtr\s*<"),
    "com_error": re.compile(r"\b_com_error\b"),
    # Split deliberately. Only the SDK header actually blocked compilation;
    # the event sites are guarded by a provider handle this port never sets,
    # so once the header is replaced they are inert and cost nothing to keep.
    # Conflating the two in one number made a 74 that could not distinguish
    # "does not compile" from "compiles and does nothing".
    "tracelogging_sdk_headers": re.compile(
        r"#\s*include\s*[<\"]traceloggingprovider[.]h[>\"]"),
    "tracelogging_events": re.compile(
        r"\b(?:TraceLogging\w*|g_hTracelogging)\b"),
    "msvc_declspec": re.compile(r"\b__declspec\s*\("),
    "msvc_uuidof": re.compile(r"\b__uuidof\s*\("),
    "wdk_headers": re.compile(
        r"#\s*include\s*[<\"](?:d3dkmthk|d3d12TokenizedProgramFormat)[^>\"]*[>\"]"),
    "dxbc_parser": re.compile(r"\b(?:CDXBCParser|SUPPORTS_DXBC_PARSE)\b"),
    "cmake_msvc_linkage": re.compile(
        r"\b(?:atls|WinPixEventRuntime|DELAYLOAD:dxcore[.]dll)\b"),
}


def is_relay_compat(relative):
    """Whether a path is one of relay12's own compatibility headers.

    scripts/prepare-dtl-source.sh copies these into the prepared tree after
    the patch series, so they sit inside the scanned directory without being
    part of the pinned upstream revision. Counting them would mean every shim
    added occurrences to the category it was written to empty -- the
    TraceLogging shim alone defines three of the identifiers the event pattern
    matches.
    """
    return (relative.parent.as_posix() == "include"
            and relative.name.startswith("relay_"))


def without_comments(text):
    """Remove C/C++ comments while preserving newlines for stable locations."""
    def replace(match):
        return "\n" * match.group(0).count("\n")

    return re.sub(r"//[^\n]*|/[*].*?[*]/", replace, text, flags=re.DOTALL)


def inventory(source_dir, revision=EXPECTED_REVISION):
    categories = {}
    paths = sorted(
        path for path in source_dir.rglob("*")
        if path.is_file() and path.suffix in SOURCE_SUFFIXES
        and not is_relay_compat(path.relative_to(source_dir))
    )
    for name, pattern in PATTERNS.items():
        matches = {}
        total = 0
        for path in paths:
            relative = path.relative_to(source_dir).as_posix()
            text = without_comments(path.read_text(errors="replace"))
            count = len(pattern.findall(text))
            if count:
                matches[relative] = count
                total += count
        categories[name] = {
            "occurrences": total,
            "files": matches,
        }
    return {
        "schema": 1,
        "revision": revision,
        "categories": categories,
    }


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("source_dir", type=pathlib.Path)
    parser.add_argument("--check", type=pathlib.Path)
    args = parser.parse_args()
    actual = inventory(args.source_dir)
    rendered = json.dumps(actual, indent=2, sort_keys=True) + "\n"
    if args.check:
        expected = args.check.read_text()
        if rendered != expected:
            print("D3D12TranslationLayer portability baseline drifted")
            return 1
        print("D3D12TranslationLayer portability baseline: ok")
        return 0
    print(rendered, end="")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
