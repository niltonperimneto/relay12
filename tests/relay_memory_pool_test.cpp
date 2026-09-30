// SPDX-License-Identifier: GPL-3.0-only
#include "relay_memory_pool.hpp"
#include <atomic>
#include <cstdlib>
#include <cstdio>
#include <memory>
#include <thread>
#include <vector>

static std::atomic<bool> failAllocation{false};
void* operator new(std::size_t size)
{
    if (failAllocation.exchange(false)) throw std::bad_alloc();
    if (void* p = std::malloc(size ? size : 1)) return p;
    throw std::bad_alloc();
}
void operator delete(void* p) noexcept { std::free(p); }
void operator delete(void* p, std::size_t) noexcept { std::free(p); }
static void check(bool value) { if (!value) std::abort(); }
struct Resource {
    std::atomic<unsigned>* completed;
    unsigned fence;
    ~Resource() { check(completed->load() >= fence); }
};
using Ptr = std::unique_ptr<Resource>;
using Pool = relay12::BoundedMemoryPool<Ptr>;
static Ptr make(std::atomic<unsigned>& completed, unsigned fence)
{
    return Ptr(new Resource{&completed, fence});
}
int main()
{
    using namespace relay12;
    check(ParseMemoryProfile(nullptr) == MemoryProfile::Auto);
    check(ParseMemoryProfile("bogus") == MemoryProfile::Invalid);
    check(!SelectBalancedMemory(MemoryProfile::Invalid, true, 8ull << 30));
    check(!SelectBalancedMemory(MemoryProfile::Legacy, true, 8ull << 30));
    check(!SelectBalancedMemory(MemoryProfile::Auto, false, 8ull << 30));
    check(!SelectBalancedMemory(MemoryProfile::Auto, true, 0));
    check(!SelectBalancedMemory(MemoryProfile::Auto, true, (8ull << 30) + 1));
    check(SelectBalancedMemory(MemoryProfile::Auto, true, 8ull << 30));
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
    std::puts("[ ok ] memory profile, fence safety, allocation failure, byte limits and concurrent returns");
}
