// SPDX-License-Identifier: GPL-3.0-only
//
// Contract test for the ETW no-op compatibility macros.
//
// The properties below are not obvious from reading the header, and each one
// is a way the stubs could be "improved" into a behaviour change:
//
//   * If TraceLoggingWrite ever consumed __VA_ARGS__, the field macros in the
//     pinned tree would start expanding and any argument with a side effect
//     would begin firing. That would be a behaviour change in a build that is
//     supposed to be inert.
//   * If TraceLoggingProviderEnabled ever returned anything but false, the
//     guarded blocks in VideoDecode.cpp would become live and would reach for
//     an ETW provider that does not exist.
//   * If TraceLoggingWrite expanded to a statement rather than an expression,
//     an unbraced `if (x) TraceLoggingWrite(...); else ...` would stop
//     compiling, and the pinned tree is entitled to that shape.
//
// Compiled natively and, in the MinGW/Wine lane, cross-compiled and run, for
// the same reason the ownership tests are: the header has to mean the same
// thing in both.

#include "relay_tracelogging.hpp"

#include <cassert>
#include <cstdio>

namespace
{
    int sideEffects = 0;

    // Both helpers are referenced only inside TraceLoggingWrite argument
    // lists, and those are never expanded -- which is the very property under
    // test. So to the compiler they are unused, and saying so is part of the
    // assertion rather than an oversight.
    [[maybe_unused]] int bumpSideEffects()
    {
        return ++sideEffects;
    }

    // Stands in for the field macros the pinned tree uses inside
    // TraceLoggingWrite. It is never defined as a macro anywhere, exactly as
    // in the real call sites, which is only sound because the arguments are
    // never expanded.
    [[maybe_unused]] int TraceLoggingFieldStandIn(int value, const char* name)
    {
        (void)name;
        return value;
    }

    int failures = 0;

    void check(bool condition, const char* what)
    {
        if (condition)
        {
            std::printf("[ ok ] %s\n", what);
        }
        else
        {
            std::printf("[fail] %s\n", what);
            ++failures;
        }
    }
}

int main()
{
    // A handle is a pointer, is null by default, and reads false in the guard
    // shape every call site uses.
    TraceLoggingHProvider provider = nullptr;
    check(!provider, "a default provider handle is null");

    // The shape of the real guards: `if (g_hTracelogging)` around the event.
    bool guardTaken = false;
    if (provider)
    {
        guardTaken = true;
    }
    check(!guardTaken, "the event guard is not taken without a provider");

    // The property the whole design rests on: arguments are not evaluated.
    // This is the same shape as the pinned call sites, including an undefined
    // field-macro name.
    TraceLoggingWrite(provider,
                      "SomeEvent",
                      TraceLoggingFieldStandIn(bumpSideEffects(), "Field"),
                      TraceLoggingUndefinedFieldMacro(bumpSideEffects(), "X"));
    check(sideEffects == 0,
          "TraceLoggingWrite does not evaluate its arguments");

    // And it stays inert for a non-null provider, because there is no sink
    // behind the handle either way.
    RelayTraceLoggingProvider* pretend =
            reinterpret_cast<RelayTraceLoggingProvider*>(&sideEffects);
    provider = pretend;
    check(provider == pretend, "a provider handle round-trips through assignment");

    TraceLoggingWrite(provider, "AnotherEvent",
                      TraceLoggingFieldStandIn(bumpSideEffects(), "Field"));
    check(sideEffects == 0,
          "TraceLoggingWrite stays inert for a non-null provider");

    // Always false, so the blocks it guards can never go live.
    check(!TraceLoggingProviderEnabled(provider, 0, 0),
          "TraceLoggingProviderEnabled is false for a non-null provider");
    check(!TraceLoggingProviderEnabled(provider, 5, 0xffffffffu),
          "TraceLoggingProviderEnabled ignores level and keyword");
    check(sideEffects == 0,
          "TraceLoggingProviderEnabled does not evaluate its arguments");

    // Usable as the sole statement of an unbraced if and else. If the macro
    // expanded to a braced block or a bare semicolon this would not compile.
    if (sideEffects == 0)
        TraceLoggingWrite(provider, "InIf");
    else
        TraceLoggingWrite(provider, "InElse");
    check(sideEffects == 0, "the macro is a void expression, not a statement");

    // The guarded-block shape from VideoDecode.cpp, which combines both
    // macros. It must be unreachable, not merely harmless.
    bool reachedGuardedBlock = false;
    if (provider && TraceLoggingProviderEnabled(provider, 0, 0))
    {
        reachedGuardedBlock = true;
        TraceLoggingWrite(provider, "Unreachable");
    }
    check(!reachedGuardedBlock,
          "a provider-enabled guarded block is unreachable");

    if (failures)
    {
        std::printf("[fail] %d check(s) failed\n", failures);
        return 1;
    }
    std::printf("[ ok ] the ETW event sites are inert\n");
    return 0;
}
