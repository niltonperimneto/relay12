// SPDX-License-Identifier: GPL-3.0-only
#include "relay_memory_pool.hpp"
#include <atomic>
#include <cstdlib>
#include <cstdio>
#include <memory>
#include <thread>
#include <vector>

static std::atomic<bool> failAllocation{false};
template<class T> struct FailAllocator {
    using value_type = T;
    FailAllocator() = default;
    template<class U> FailAllocator(const FailAllocator<U>&) noexcept {}
    T* allocate(std::size_t count) {
        if (failAllocation.exchange(false)) throw std::bad_alloc();
        return std::allocator<T>{}.allocate(count);
    }
    void deallocate(T* p, std::size_t count) noexcept { std::allocator<T>{}.deallocate(p, count); }
    template<class U> bool operator==(const FailAllocator<U>&) const noexcept { return true; }
    template<class U> bool operator!=(const FailAllocator<U>&) const noexcept { return false; }
};
static void check(bool value) { if (!value) std::abort(); }
struct Resource {
    std::atomic<unsigned>* completed;
    unsigned fence;
    ~Resource() { check(completed->load() >= fence); }
};
using Ptr = std::unique_ptr<Resource>;
using Pool = relay12::BoundedMemoryPool<Ptr, FailAllocator<Ptr>>;
static Ptr make(std::atomic<unsigned>& completed, unsigned fence)
{
    return Ptr(new Resource{&completed, fence});
}
struct TestClock {
    std::atomic<std::uint64_t> milliseconds{0};
    static std::uint64_t read(void* context) noexcept
    {
        return static_cast<TestClock*>(context)->milliseconds.load();
    }
};
static void checkBurstReuse(std::atomic<unsigned>& completed)
{
    constexpr std::uint64_t mib = 1ull << 20, resourceBytes = 99 * mib / 48;
    TestClock clock;
    Pool pool(relay12::PoolLimits{32 * mib, 128 * mib, 2000}, TestClock::read, &clock);
    std::vector<Ptr> working(48);
    for (unsigned cycle = 0; cycle < 5; ++cycle)
    {
        clock.milliseconds = cycle * 100;
        for (unsigned i = 0; i < 48; ++i)
        {
            working[i] = pool.take(i, 0);
            if (!working[i]) { pool.allocated(); working[i] = make(completed, 0); }
        }
        for (unsigned i = 0; i < 48; ++i)
            check(pool.put(i, resourceBytes, 0, std::move(working[i])));
        check(pool.snapshot().completed == 99 * mib);
    }
    auto stats = pool.snapshot();
    check(stats.allocations == 48 && stats.reuses == 192 && stats.trims == 0);
    check(stats.softLimit == 32 * mib && stats.burstLimit == 128 * mib);
    check(stats.graceMilliseconds == 2000 && stats.effectiveLimit == 128 * mib && stats.burstActive);
    clock.milliseconds = 2399;
    check(!pool.take(100, 0)); // failed lookups must not prolong the working set
    check(pool.snapshot().completed == 99 * mib);
    clock.milliseconds = 2400;
    check(pool.snapshot().effectiveLimit == 32 * mib && !pool.snapshot().burstActive);
    check(pool.snapshot().completed == 99 * mib); // observation does not reclaim
    pool.trim(0);
    check(pool.snapshot().completed <= 32 * mib && pool.snapshot().trims == 33);
}
int main()
{
    using namespace relay12;
    std::uint64_t setting = 99;
    check(ParsePoolSetting("0", 1024, setting) && setting == 0);
    check(ParsePoolSetting("00032", 1024, setting) && setting == 32);
    check(ParsePoolSetting("1024", 1024, setting) && setting == 1024);
    const char* invalidSettings[] = {nullptr, "", "-1", "+1", " 1", "1 ", "1.0", "0x10", "1025",
        "18446744073709551616", "999999999999999999999999999"};
    for (const auto* invalid : invalidSettings)
    {
        setting = 99;
        check(!ParsePoolSetting(invalid, 1024, setting) && setting == 99);
    }
    const auto uintMaximum = (std::numeric_limits<std::uint64_t>::max)();
    check(ParsePoolSetting("18446744073709551615", uintMaximum, setting) && setting == uintMaximum);
    check(!ParsePoolSetting("18446744073709551616", uintMaximum, setting) && setting == uintMaximum);
    check(ParsePoolSetting("000", 0, setting) && setting == 0);
    check(!ParsePoolSetting("1", 0, setting) && setting == 0);
    check(ParseMemoryProfile(nullptr) == MemoryProfile::Auto);
    check(ParseMemoryProfile("bogus") == MemoryProfile::Invalid);
    check(!SelectBalancedMemory(MemoryProfile::Invalid, true, 8ull << 30));
    check(!SelectBalancedMemory(MemoryProfile::Legacy, true, 8ull << 30));
    check(!SelectBalancedMemory(MemoryProfile::Auto, false, 8ull << 30));
    check(!SelectBalancedMemory(MemoryProfile::Auto, true, 0));
    check(!SelectBalancedMemory(MemoryProfile::Auto, true, (8ull << 30) + 1));
    check(!SelectBalancedMemory(MemoryProfile::Auto, true, 8ull << 30));
    check(SelectBalancedMemory(MemoryProfile::Auto, true, 8ull << 30, true));
    check(!SelectBalancedMemory(MemoryProfile::Auto, false, 8ull << 30, true));
    check(!SelectBalancedMemory(MemoryProfile::Auto, true, 0, true));
    check(!SelectBalancedMemory(MemoryProfile::Auto, true, (8ull << 30) + 1, true));
    check(SelectBalancedMemory(MemoryProfile::Balanced, false, 0));
    std::atomic<unsigned> completed{0};
    {
        Pool pool(64);
        check(pool.put(1, 64, 2, make(completed, 2)));
        check(pool.put(2, 128, 3, make(completed, 3)));
        check(pool.snapshot().pending == 192 && pool.snapshot().completed == 0);
        check(!pool.take(1, 1));
        completed = 2; pool.trim(2);
        check(pool.snapshot().retained == 192); // pending does not consume idle budget
        auto reused = pool.take(1, 2);
        check(bool(reused));
        pool.allocated();
        check(pool.put(1, 64, 2, std::move(reused)));
        completed = 3; pool.trim(3);
        check(pool.snapshot().retained == 64 && pool.snapshot().pending == 0);
        check(pool.snapshot().trims == 1 && pool.snapshot().reuses == 1);
        check(pool.snapshot().allocations == 1 && pool.snapshot().peakRetained == 192);
        pool.trim(1); // delayed fence observations must not regress completion
        check(pool.snapshot().pending == 0);
    }
    {
        Pool pool(64);
        auto resource = make(completed, 0);
        failAllocation = true;
        check(!pool.put(1, 64, 0, std::move(resource)));
        check(bool(resource) && pool.snapshot().retained == 0);
        check(!pool.put(1, 0, 0, std::move(resource)) && bool(resource));
        check(pool.put(1, 128, 0, std::move(resource))); // oversized completed resource released
        check(pool.snapshot().retained == 0);
    }
    {
        Pool pool(64);
        check(pool.put(1, 40, 0, make(completed, 0)));
        check(pool.put(2, 40, 0, make(completed, 0)));
        check(!pool.take(1, 0)); // oldest completed entry evicted first
        check(bool(pool.take(2, 0)));
    }
    {
        completed = 0;
        Pool pool(64);
        std::vector<std::thread> threads;
        for (unsigned t = 0; t < 4; ++t) threads.emplace_back([&] {
            for (unsigned i = 0; i < 200; ++i)
                check(pool.put(i % 4, 64, 10, make(completed, 10)));
        });
        for (auto& thread : threads) thread.join();
        check(pool.snapshot().pending == 800 * 64);
        completed = 10; pool.trim(10);
        check(pool.snapshot().completed == 64 && pool.snapshot().trims == 799);
    }
    checkBurstReuse(completed);
    {
        TestClock clock;
        Pool pool(PoolLimits{32, 128, 2000}, TestClock::read, &clock);
        clock.milliseconds = 100;
        check(pool.put(1, 64, 0, make(completed, 0)));
        clock.milliseconds = 50; // rollback does not underflow elapsed time
        pool.trim(0);
        check(pool.snapshot().completed == 64 && pool.snapshot().burstActive);
        clock.milliseconds = 2099;
        pool.trim(0);
        check(pool.snapshot().completed == 64);
        clock.milliseconds = 2100;
        pool.trim(0);
        check(pool.snapshot().completed == 0 && !pool.snapshot().burstActive);
        check(pool.put(2, 64, 0, make(completed, 0))); // fresh workload restores grace
        check(pool.snapshot().burstActive);
        failAllocation = true;
        auto failed = make(completed, 0);
        clock.milliseconds = 4099;
        check(!pool.put(3, 64, 0, std::move(failed)) && bool(failed));
        clock.milliseconds = 4100;
        pool.trim(0);
        check(pool.snapshot().completed == 0); // failed insertion never renews grace
    }
    {
        TestClock clock;
        completed = 0;
        Pool pool(PoolLimits{32, 128, 2000}, TestClock::read, &clock);
        check(pool.put(1, 64, 0, make(completed, 0)));
        check(pool.put(2, 256, 5, make(completed, 5)));
        pool.trim(0, PoolTrimReason::Pressure);
        check(pool.snapshot().completed == 0 && pool.snapshot().pending == 256);
        check(!pool.snapshot().burstActive);
        check(pool.put(3, 16, 0, make(completed, 0)));
        pool.trim(0, PoolTrimReason::Teardown);
        check(pool.snapshot().retained == 256 && pool.snapshot().pending == 256);
        completed = 5;
        pool.trim(5, PoolTrimReason::Teardown);
        check(pool.snapshot().retained == 0);
    }
    {
        TestClock clock;
        Pool pool(PoolLimits{32, 128, 2000}, TestClock::read, &clock);
        check(pool.put(1, 96, 0, make(completed, 0)));
        check(pool.put(2, 96, 0, make(completed, 0)));
        check(pool.snapshot().completed == 96 && pool.snapshot().trims == 1);
        pool.configure(PoolLimits{64, 32, 0}); // invalid ceiling normalized up to soft
        check(pool.snapshot().softLimit == 64 && pool.snapshot().burstLimit == 64);
        check(pool.snapshot().completed == 0);
        pool.configure(PoolLimits{0, 0, 2000});
        check(pool.put(3, 64, 0, make(completed, 0)));
        check(pool.snapshot().retained == 0);
    }
    {
        TestClock clock;
        completed = 0;
        const auto maximum = (std::numeric_limits<std::uint64_t>::max)();
        Pool pool(PoolLimits{0, maximum, maximum}, TestClock::read, &clock);
        check(pool.put(1, maximum, 5, make(completed, 5)));
        auto extra = make(completed, 0);
        check(!pool.put(2, 1, 0, std::move(extra)) && bool(extra));
        check(pool.snapshot().retained == maximum && pool.snapshot().pending == maximum);
        clock.milliseconds = maximum;
        pool.trim(0); // maximum grace expires without deadline-addition overflow
        check(pool.snapshot().effectiveLimit == 0 && pool.snapshot().pending == maximum);
        completed = 5;
        pool.trim(5, PoolTrimReason::Teardown);
        check(pool.snapshot().retained == 0);
    }
    {
        TestClock clock;
        Pool pool(PoolLimits{32, 128, 2000}, TestClock::read, &clock);
        std::vector<std::thread> threads;
        for (unsigned t = 0; t < 4; ++t) threads.emplace_back([&, t] {
            for (unsigned i = 0; i < 100; ++i)
            {
                auto resource = pool.take(t, 5);
                if (!resource) { pool.allocated(); resource = make(completed, 0); }
                check(pool.put(t, 16, 0, std::move(resource)));
                if (i % 7 == 0) pool.trim(5);
                check(pool.snapshot().completed <= 128);
            }
        });
        for (auto& thread : threads) thread.join();
        check(pool.snapshot().allocations == 4 && pool.snapshot().reuses == 396);
        clock.milliseconds = 2000;
        pool.trim(5);
        check(pool.snapshot().completed <= 32);
    }
    std::puts("[ ok ] memory profiles, fence safety, allocation failure, byte limits, burst reuse, idle grace and concurrency");
}
