#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-only
#
# Every piece of shared mutable state in the router and the core must be safe
# to touch from any thread, and must be visibly so.
#
# D3D11 devices are used concurrently by design, and neither of these modules
# is a Wine builtin, so neither gets Wine's locking for free.  What they have
# instead is two disciplines: a one-shot latch is a `volatile LONG` moved only
# by `Interlocked*`, and anything published once at startup is published
# through an `INIT_ONCE`.  Both work.  Neither is enforced by the compiler, and
# a race added here would not fail a test -- it would fail a game, on someone
# else's machine, once.
#
# tests/ddi_thread_stress.c hammers this state from twelve threads, which is
# the right way to find a broken latch and the wrong way to prove there is no
# unguarded one: the storm only reaches the state it happens to call. This
# gate reads every definition instead.
#
# Three rules, and one annotation for the case the rules cannot express.
#
#   1. A definition at namespace scope that is not const must be an
#      `INIT_ONCE`, a `volatile LONG`, or annotated (rule 3).
#   2. A `volatile LONG` must never be assigned or stepped directly.  It moves
#      through `Interlocked*` or it does not move.
#   3. Anything else must carry
#
#          /* shared-state: published once through <once> */
#
#      naming an `INIT_ONCE` defined in the same file.  Then every function
#      that touches it must either be that `INIT_ONCE`'s callback, or must
#      call `InitOnceExecuteOnce` on it before the first mention.  That call
#      is the happens-before which makes the read safe; without it the
#      annotation would just be a way to opt out.
#
# Usage:
#   python3 scripts/check_shared_state.py [PATH ...]

import argparse
import pathlib
import re
import sys

DEFAULT_ROOTS = ["relay12-d3d11"]

# A definition at namespace scope: no leading whitespace, and it is not a
# function, a label, a preprocessor line, or a continuation.
DEFINITION = re.compile(
    r"^(?P<declaration>[A-Za-z_][\w:<>,* ]*?[ *&])(?P<name>\w+)"
    r"\s*(?:=[^;]*)?;\s*$")

# `volatile LONG reportedNoHost;` and friends.
LATCH = re.compile(r"^volatile\s+LONG\s+$")
ONCE = re.compile(r"^INIT_ONCE\s+$")

ANNOTATION = re.compile(
    r"/\*\s*shared-state: published once through (?P<once>\w+)\s*\*/")

CONST = re.compile(r"\b(?:const|constexpr)\b")

# Assignment or a step, as opposed to being read or having its address taken.
def written_directly(name):
    return re.compile(
        rf"(?<![\w.>])(?:\+\+|--)\s*{re.escape(name)}\b"
        rf"|(?<![\w.>]){re.escape(name)}\s*(?:\+\+|--)"
        rf"|(?<![\w.>]){re.escape(name)}\s*(?:[-+*/|&^]|<<|>>)?=(?!=)")


def namespace_scope_definitions(text):
    """(line number, declaration, name) for each definition at column zero."""
    found = []
    for number, line in enumerate(text.splitlines(), 1):
        if line.startswith(("#", "//", "/*", " ", "\t", "}")):
            continue
        match = DEFINITION.match(line)
        if not match:
            continue
        declaration = match.group("declaration")
        # `return x;` and `using x = ...;` are not definitions, and neither is
        # a typedef or a forward declaration of a function.
        if declaration.split()[0] in {"return", "using", "typedef", "extern",
                                      "namespace", "template", "friend"}:
            continue
        found.append((number, declaration, match.group("name")))
    return found


# Keywords that take a parenthesised operand, so that the identifier before
# the parameter list is not mistaken for one.
NOT_A_FUNCTION_NAME = {
    "if", "for", "while", "switch", "catch", "return", "sizeof", "alignof",
    "decltype", "static_assert", "noexcept", "throw", "new", "delete",
}


def function_bodies(text):
    """{name: (first line, last line)} for each brace-matched definition.

    Anchored on a `{` alone at column zero, which in this codebase's style is
    a function body opening at namespace scope and nothing else.  The name is
    then read backwards from it: a parameter list may span lines, so the walk
    continues until the parentheses balance rather than assuming the
    signature is one line.  A gate that needed a C++ parser would not run at
    all, but one anchored on the wrong shape is worse -- it finds no functions
    and reports success, which is why tests/test_ci_gates.py pins the set of
    functions this returns.
    """
    bodies = {}
    lines = text.splitlines()

    for index, line in enumerate(lines):
        if line != "{":
            continue

        # Walk back over the signature until its parentheses balance.
        signature = ""
        for start in range(index - 1, max(-1, index - 12), -1):
            signature = lines[start] + "\n" + signature
            if signature.count("(") and signature.count(
                    "(") == signature.count(")"):
                break
        else:
            continue

        name = None
        for match in re.finditer(r"(\w+)\s*\(", signature):
            if match.group(1) not in NOT_A_FUNCTION_NAME:
                name = match.group(1)
                break
        if name is None:
            continue

        depth = 0
        for end in range(index, len(lines)):
            depth += lines[end].count("{") - lines[end].count("}")
            if depth <= 0:
                bodies[name] = (index + 1, end + 1)
                break
    return bodies


def callbacks_for(text):
    """{INIT_ONCE name: callback name} from every InitOnceExecuteOnce call."""
    return {
        once: callback
        for once, callback in re.findall(
            r"InitOnceExecuteOnce\s*\(\s*&(\w+)\s*,\s*(\w+)", text)
    }


def check_source(path, text):
    errors = []
    lines = text.splitlines()
    bodies = function_bodies(text)
    callbacks = callbacks_for(text)
    onces = {
        name
        for _, declaration, name in namespace_scope_definitions(text)
        if ONCE.match(declaration)
    }

    for number, declaration, name in namespace_scope_definitions(text):
        if CONST.search(declaration) or ONCE.match(declaration):
            continue

        if LATCH.match(declaration):
            errors.extend(check_latch(path, name, lines, bodies))
            continue

        # Anything else needs the annotation, on the definition or on one of
        # the two lines above it.
        context = "\n".join(lines[max(0, number - 3):number])
        annotated = ANNOTATION.search(context)
        if not annotated:
            errors.append(
                f"{path}:{number}: '{name}' is shared mutable state at "
                "namespace scope; make it a volatile LONG moved by "
                "Interlocked*, an INIT_ONCE, or annotate it with "
                "'shared-state: published once through <once>'")
            continue

        once = annotated.group("once")
        if once not in onces:
            errors.append(
                f"{path}:{number}: '{name}' is annotated as published through "
                f"'{once}', which is not an INIT_ONCE in this file")
            continue

        errors.extend(check_published(path, name, once, callbacks.get(once),
                                      lines, bodies))

    return errors


def check_latch(path, name, lines, bodies):
    """A latch moves through Interlocked* or it does not move."""
    errors = []
    written = written_directly(name)
    for number, line in enumerate(lines, 1):
        code = line.split("//", 1)[0]
        if not written.search(code):
            continue
        if in_body(number, bodies) is None and code.strip().startswith(
                ("volatile", "INIT_ONCE")):
            continue
        errors.append(
            f"{path}:{number}: '{name}' is a one-shot latch and is written "
            "directly here; it must move through an Interlocked* call so a "
            "lost race reports twice rather than never")
    return errors


def initializers_for(once, lines, bodies):
    """Every function whose call is enough to have executed `once`.

    The direct caller of InitOnceExecuteOnce, and then anything that calls
    one of those, to a fixpoint.  The router wraps the call in initialize()
    and every entry point calls that, so a gate that only looked for the
    literal InitOnceExecuteOnce would report all four entry points and be
    turned off within the day.
    """
    initializers = {
        function
        for function, (first, last) in bodies.items()
        if any(f"InitOnceExecuteOnce(&{once}" in lines[number - 1]
               for number in range(first, last + 1))
    }

    while True:
        grown = set(initializers)
        for function, (first, last) in bodies.items():
            if function in grown:
                continue
            body = "\n".join(lines[first - 1:last])
            if any(re.search(rf"(?<![\w.>]){re.escape(known)}\s*\(", body)
                   for known in initializers):
                grown.add(function)
        if grown == initializers:
            return initializers
        initializers = grown


def first_barrier(function, once, initializers, lines, bodies):
    """The line at which `function` has certainly executed `once`."""
    first, last = bodies[function]
    reached = [
        number
        for number in range(first, last + 1)
        if f"InitOnceExecuteOnce(&{once}" in lines[number - 1]
        or any(re.search(rf"(?<![\w.>]){re.escape(known)}\s*\(",
                         lines[number - 1].split("//", 1)[0])
               for known in initializers if known != function)
    ]
    return min(reached) if reached else None


def check_published(path, name, once, callback, lines, bodies):
    """A published value is touched only after the INIT_ONCE has run."""
    errors = []
    if callback is None:
        return [f"{path}: '{name}' is annotated as published through '{once}', "
                "but nothing calls InitOnceExecuteOnce on it"]

    initializers = initializers_for(once, lines, bodies)
    mention = re.compile(rf"(?<![\w.>]){re.escape(name)}\b")

    for function, (first, last) in sorted(bodies.items()):
        # Only the callback is exempt: it is where the value is written, and it
        # runs inside the INIT_ONCE rather than after it.  Everything else is
        # checked for ordering, including the functions that execute the
        # INIT_ONCE themselves -- those are precisely where a touch could be
        # moved one line too early.
        if function == callback:
            continue
        touches = [
            number
            for number in range(first, last + 1)
            if mention.search(lines[number - 1].split("//", 1)[0])
        ]
        if not touches:
            continue

        barrier = first_barrier(function, once, initializers, lines, bodies)
        if barrier is None:
            errors.append(
                f"{path}:{touches[0]}: '{function}' touches '{name}' without "
                f"executing '{once}' first, so it may read a value no barrier "
                "published")
        elif barrier > min(touches):
            errors.append(
                f"{path}:{touches[0]}: '{function}' touches '{name}' at line "
                f"{touches[0]}, before it executes '{once}' at line "
                f"{barrier}")
    return errors


def in_body(number, bodies):
    for function, (first, last) in bodies.items():
        if first <= number <= last:
            return function
    return None


def sources(roots):
    for root in roots:
        path = pathlib.Path(root)
        if path.is_file():
            yield path
        else:
            yield from sorted(path.rglob("*.cpp"))


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("roots", nargs="*", default=DEFAULT_ROOTS,
                        metavar="PATH")
    args = parser.parse_args()

    errors = []
    scanned = 0
    for path in sources(args.roots):
        scanned += 1
        errors.extend(check_source(path, path.read_text()))

    if errors:
        for error in errors:
            print(error, file=sys.stderr)
        return 1

    print(f"shared state audit: ok ({scanned} sources)")
    return 0


if __name__ == "__main__":
    sys.exit(main())
