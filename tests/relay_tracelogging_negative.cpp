// SPDX-License-Identifier: GPL-3.0-only
//
// This file must fail to compile.
//
// TraceLoggingHProvider is deliberately a pointer to a distinct incomplete
// type rather than void* or an integer handle. Were it either, the tree's
// event guards would accept any pointer or any zero-comparable value, and a
// later refactor could pass the wrong object without a diagnostic. Nothing
// about the no-op macros themselves would catch that, because they never
// look at their arguments.
//
// The counterpart positive test is tests/relay_tracelogging_test.cpp.

#include "relay_tracelogging.hpp"

void assign_an_unrelated_pointer_to_a_provider_handle(int* unrelated)
{
    TraceLoggingHProvider provider = unrelated;
    (void)provider;
}
