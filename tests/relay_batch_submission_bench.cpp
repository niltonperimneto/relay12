// SPDX-License-Identifier: GPL-3.0-only
// Synthetic Win32 handoff comparison; not a DTL or GPU performance claim.
#include "relay_batch_ring.hpp"
#include <chrono>
#include <cstdio>
#include <deque>
#include <mutex>
#include <thread>

constexpr unsigned count = 100000;
constexpr unsigned bound = 5;
using Clock = std::chrono::steady_clock;
static int failures;

static void run(bool ringMode)
{
    relay12::RelaySpscRing<unsigned*, 8> ring;
    relay12::RelayBatchWaiter ready, room;
    std::deque<unsigned*> queue;
    std::mutex mutex;
    unsigned payload[bound] = {};
    HANDLE submitted = CreateSemaphoreW(nullptr, 0, bound, nullptr);
    HANDLE consumed = CreateSemaphoreW(nullptr, bound, bound, nullptr);
    if (!submitted || !consumed) { ++failures; return; }
    const auto start = Clock::now();
    std::thread worker([&] {
        for (unsigned i = 0; i < count; ++i)
        {
            unsigned* p;
            if (ringMode)
            {
                ready.wait_until([&] { return !ring.empty(); });
                p = ring.front();
            }
            else
            {
                if (WaitForSingleObject(submitted, INFINITE) != WAIT_OBJECT_0) std::terminate();
                std::lock_guard<std::mutex> lock(mutex);
                p = queue.front(); queue.pop_front();
            }
            if (*p != i) ++failures;
            if (ringMode) { ring.pop(); room.notify(); }
            else if (!ReleaseSemaphore(consumed, 1, nullptr)) std::terminate();
        }
    });
    for (unsigned i = 0; i < count; ++i)
    {
        if (ringMode) room.wait_until([&] { return ring.size() < bound; });
        else if (WaitForSingleObject(consumed, INFINITE) != WAIT_OBJECT_0) std::terminate();
        unsigned* p = &payload[i % bound];
        *p = i;
        if (ringMode)
        {
            if (!ring.try_push(p)) std::terminate();
            ready.notify();
        }
        else
        {
            { std::lock_guard<std::mutex> lock(mutex); queue.push_back(p); }
            if (!ReleaseSemaphore(submitted, 1, nullptr)) std::terminate();
        }
    }
    worker.join();
    const auto ns = std::chrono::duration_cast<std::chrono::nanoseconds>(Clock::now() - start).count();
    std::printf("%s batches=%u ns_per_batch=%.0f address_wakes=%llu\n",
            ringMode ? "ring" : "semaphore", count, double(ns) / count,
            static_cast<unsigned long long>(ready.wakes() + room.wakes()));
    CloseHandle(submitted); CloseHandle(consumed);
}
int main()
{
    for (unsigned trial = 0; trial < 3; ++trial) { run(trial % 2); run(!(trial % 2)); }
    return failures ? 1 : 0;
}
