// Copyright (c) relay12 contributors.
// SPDX-License-Identifier: GPL-3.0-only
#pragma once

// Compiles D3D12TranslationLayer's ETW event sites to nothing.
//
// traceloggingprovider.h is a Windows SDK header MinGW-w64 does not ship, and
// it was the recorded compiler boundary. Nothing in this port wants the
// events behind it:
//
//   * Every TraceLoggingWrite in the pinned tree is already guarded by
//     `if (g_hTracelogging)`, the tree never calls TRACELOGGING_DEFINE_PROVIDER
//     or TraceLoggingRegister, and upstream's own README says that in the
//     default configuration no provider is created and no data is sent. This
//     port never calls SetTraceloggingProvider, so the events were already
//     inert at run time. Making them inert at compile time changes no
//     behaviour.
//
//   * They are measurement counters, not error handling. Where one sits next
//     to a failure it is beside the error path rather than on it:
//     PipelineState.cpp logs inside `if (FAILED(hr))` and then calls
//     ThrowFailure(hr) regardless. Compiling the event out leaves every
//     HRESULT path exactly as it was, which is what the port-quality roadmap
//     requires of this step.
//
// Why the events are not rerouted to Wine's debug output. __wine_dbg_output is
// a Wine-internal, unversioned export. The relay modules resolve it at run
// time precisely so that their import tables stay at kernel32 and msvcrt,
// which the PE import audit enforces; linking it into a static library that
// D3D11On12 consumes would put it in the import table for good. TraceLogging
// is also a structured-field API, so a printf-style sink would mean inventing
// a formatting layer with no consumer and no test. A no-op is honest where a
// half-formatted log line would not be.

// A distinct incomplete type, so a handle cannot be confused with another
// pointer. Every use in the pinned tree is a pointer: a global initialised to
// nullptr, a setter parameter, and truth tests in the event guards.
struct RelayTraceLoggingProvider;
using TraceLoggingHProvider = RelayTraceLoggingProvider*;

// The replacement lists below deliberately do not name their parameters. An
// unused macro argument is never expanded and never evaluated, which is what
// makes this safe: the field macros in the call sites (TraceLoggingInt32,
// TraceLoggingHResult, TraceLoggingPackedMetadata and the rest) need no
// definitions, and an argument with a side effect cannot fire. Both
// properties are pinned by tests/relay_tracelogging_test.cpp; a future
// "improvement" that consumes __VA_ARGS__ would break them.
//
// Parenthesised so the result is a complete expression, and a void expression
// so that using it as the sole statement of an unbraced if or else is still
// valid.
#define TraceLoggingWrite(...) ((void)0)

// Always false, so the guarded blocks are unreachable rather than removed.
// Leaving the code in place keeps the diff against the pinned revision small
// and keeps a later rebase onto a newer upstream cheap.
#define TraceLoggingProviderEnabled(...) (false)
