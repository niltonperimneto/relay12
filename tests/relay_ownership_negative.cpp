// SPDX-License-Identifier: GPL-3.0-only
#include "relay_ownership.hpp"

struct MockCom
{
    unsigned AddRef() noexcept { return 2; }
    unsigned Release() noexcept { return 1; }
};

int main()
{
    MockCom object;
    RelayComPtr<MockCom> live(&object);
    // Exposing a live owner as an output slot would leak its reference. The
    // compatibility contract must fail closed here, as ATL does in debug.
    (void)&live;
    return 0;
}
