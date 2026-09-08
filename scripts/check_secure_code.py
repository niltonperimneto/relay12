#!/usr/bin/env python3
"""
Security and Precision Static Analysis for relay12.
Ensures LLM-generated code does not introduce probabilistic low-level C/C++ errors.
"""
import os
import re
import sys

def check_file(filepath):
    errors = []
    with open(filepath, 'r', encoding='utf-8') as f:
        content = f.readlines()

    for line_idx, line in enumerate(content):
        line_num = line_idx + 1
        clean_line = line.strip()

        # 1. Thread-Safety: Ban native increment/decrement on refcounts
        if re.search(r'\b(refcount|cRef|m_ref)\s*(\+\+|--)', clean_line) or \
           re.search(r'(\+\+|--)\s*(refcount|cRef|m_ref)\b', clean_line):
            errors.append(f"{filepath}:{line_num}: Unprotected refcount mutation. Use InterlockedIncrement/Decrement.")

        # 2. Integer Overflow: Ban unchecked math inside malloc/LocalAlloc calls
        alloc_match = re.search(r'\b(malloc|calloc|LocalAlloc|HeapAlloc)\s*\((.*?)\)', clean_line)
        if alloc_match:
            args = alloc_match.group(2)
            # If there's an asterisk in the allocation argument, it's doing unsafe multiplication
            # (ignoring cast pointers which have asterisks, this is a heuristic).
            # Improved heuristic: look for `sizeof(...) * var`
            if re.search(r'\bsizeof\s*\([^)]*\)\s*\*', args):
                errors.append(f"{filepath}:{line_num}: Unchecked allocation math. Use safe integer bounds checking before allocation.")

    return errors

def main():
    if len(sys.argv) < 2:
        print(f"Usage: {sys.argv[0]} <directory>", file=sys.stderr)
        sys.exit(2)

    scan_dir = sys.argv[1]
    all_errors = []

    for root, _, files in os.walk(scan_dir):
        for name in files:
            if name.endswith(('.c', '.h', '.cpp', '.hpp')):
                filepath = os.path.join(root, name)
                all_errors.extend(check_file(filepath))

    if all_errors:
        for err in all_errors:
            print(err)
        print("\n[FAIL] Security and precision code linting failed.")
        sys.exit(1)

    print("[OK] All security lints passed.")

if __name__ == "__main__":
    main()
