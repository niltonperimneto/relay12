// SPDX-License-Identifier: GPL-3.0-only
#include "relay_com_ptr.hpp"

#include <cassert>
#include <utility>

struct MockCom
{
    unsigned refs = 1;
    unsigned adds = 0;
    unsigned releases = 0;

    unsigned AddRef() noexcept
    {
        ++adds;
        return ++refs;
    }

    unsigned Release() noexcept
    {
        ++releases;
        return --refs;
    }
};

static void returnOwned(MockCom** output, MockCom* object)
{
    *output = object;
}

int main()
{
    MockCom first;
    {
        RelayComPtr<MockCom> owner(&first);
        assert(first.refs == 2 && first.adds == 1);
        assert(owner && owner.get() == &first && owner->refs == 2);

        RelayComPtr<MockCom> copy(owner);
        assert(first.refs == 3 && first.adds == 2);
        RelayComPtr<MockCom>& copyAlias = copy;
        copy = copyAlias;
        assert(first.refs == 3 && first.adds == 2 && first.releases == 0);

        RelayComPtr<MockCom> moved(std::move(copy));
        assert(!copy && moved.get() == &first && first.refs == 3);
        moved = nullptr;
        assert(first.refs == 2 && first.releases == 1);

        MockCom second;
        returnOwned(&owner, &second);
        assert(first.refs == 1 && first.releases == 2);
        assert(owner.get() == &second && second.refs == 1);

        MockCom* detached = owner.detach();
        assert(detached == &second && !owner && second.releases == 0);
        owner.attach(detached);
        owner.attach(detached);
        assert(second.refs == 1 && second.adds == 0 && second.releases == 0);
    }

    // attach() and COM output parameters both transfer an existing reference.
    assert(first.refs == 1 && first.adds == 2 && first.releases == 2);
    return 0;
}
