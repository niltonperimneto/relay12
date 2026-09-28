// SPDX-License-Identifier: GPL-3.0-only
// Synthetic handoff comparison, not a DTL or GPU performance claim.
#include "relay_batch_ring.hpp"
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <deque>
#include <mutex>
#include <thread>

using Clock = std::chrono::steady_clock;
constexpr unsigned bound = 5;
// Incremented by the worker thread as well as the main one.
static std::atomic<int> failures{0};
static unsigned count = 100000;

static unsigned work(unsigned value, unsigned iterations)
{
    for (unsigned n = 0; n < iterations; ++n) value = value * 1664525u + 1013904223u;
    return value;
}
static unsigned long long cpuTime()
{
    FILETIME created, exited, kernel, user;
    if (!GetProcessTimes(GetCurrentProcess(), &created, &exited, &kernel, &user)) std::terminate();
    return ((static_cast<unsigned long long>(kernel.dwHighDateTime) << 32) | kernel.dwLowDateTime)
         + ((static_cast<unsigned long long>(user.dwHighDateTime) << 32) | user.dwLowDateTime);
}

template <typename WorkerWait, typename ProducerWait, bool Deque = false>
static void run(const char* name, unsigned trial, unsigned iterations, unsigned long long expected)
{
    struct Payload { unsigned sequence, result; };
    relay12::RelaySpscRing<Payload*, 8> ring;
    WorkerWait ready;
    ProducerWait room;
    std::deque<Payload*> queue;
    std::mutex mutex;
    Payload payload[bound] = {};
    HANDLE submitted = nullptr, consumed = nullptr;
    if constexpr (Deque)
    {
        submitted = CreateSemaphoreW(nullptr, 0, bound, nullptr);
        consumed = CreateSemaphoreW(nullptr, bound, bound, nullptr);
        if (!submitted || !consumed) std::terminate();
    }
    unsigned long long checksum = 0;
    const auto cpuStart = cpuTime();
    const auto start = Clock::now();
    std::thread worker([&] {
        for (unsigned i = 0; i < count; ++i)
        {
            Payload* p;
            if constexpr (Deque)
            {
                if (WaitForSingleObject(submitted, INFINITE) != WAIT_OBJECT_0) std::terminate();
                std::lock_guard<std::mutex> lock(mutex);
                p = queue.front(); queue.pop_front();
            }
            else
            {
                ready.wait_until([&] { return !ring.empty(); });
                p = ring.front();
            }
            if (p->sequence != i) ++failures;
            p->result = work(p->sequence, iterations);
            checksum += p->result;
            if constexpr (Deque)
            {
                if (!ReleaseSemaphore(consumed, 1, nullptr)) std::terminate();
            }
            else { ring.pop(); room.notify(); }
        }
    });
    for (unsigned i = 0; i < count; ++i)
    {
        if constexpr (Deque)
        {
            if (WaitForSingleObject(consumed, INFINITE) != WAIT_OBJECT_0) std::terminate();
        }
        else room.wait_until([&] { return ring.size() < bound; });
        Payload* p = &payload[i % bound];
        p->sequence = i;
        if constexpr (Deque)
        {
            { std::lock_guard<std::mutex> lock(mutex); queue.push_back(p); }
            if (!ReleaseSemaphore(submitted, 1, nullptr)) std::terminate();
        }
        else
        {
            if (!ring.try_push(p)) std::terminate();
            ready.notify();
        }
    }
    worker.join();
    const auto ns = std::chrono::duration_cast<std::chrono::nanoseconds>(Clock::now() - start).count();
    const auto cpu = cpuTime() - cpuStart;
    if (checksum != expected) ++failures;
    std::printf("%s,%u,%u,%u,%.0f,%.0f\n", name, trial, iterations, count,
                double(ns) / count, double(cpu) * 100 / count);
    std::fflush(stdout);
    if (submitted) CloseHandle(submitted);
    if (consumed) CloseHandle(consumed);
}
template<unsigned S> using Address = relay12::RelayAddressWaiter<relay12::Win32AddressWaitPolicy, S>;
using Run = void (*)(const char*, unsigned, unsigned, unsigned long long);
struct Variant { const char* name; Run run; };
// Upstream's handoff, the ring as DTL builds it (128 spins before sleeping),
// and the ring with no spin at all. A 2026-09-28 sweep of 16 and 32 spins, and
// of a semaphore instead of an address wait under the ring, found nothing
// between the two ends worth keeping; see docs/validation.
static const Variant variants[] = {
    {"deque-semaphore", run<Address<0>, Address<0>, true>},
    {"ring-address-w0-p0", run<Address<0>, Address<0>>},
    {"ring-address-w128-p128", run<Address<128>, Address<128>>},
};
int main(int argc, char** argv)
{
    const bool quick = argc == 2 && !std::strcmp(argv[1], "--quick");
    if (argc > 1 && !quick) return 2;
    if (quick) count = 5000;
    std::puts("variant,trial,work_iterations,batches,ns_per_batch,cpu_ns_per_batch");
    for (unsigned iterations : {0u, 256u})
    {
        unsigned long long expected = 0;
        for (unsigned i = 0; i < count; ++i) expected += work(i, iterations);
        for (unsigned trial = 0; trial < (quick ? 3u : 7u); ++trial)
        {
            // Rotate the starting variant and reverse alternate trials to
            // reduce order bias. Compare each workload separately.
            constexpr unsigned size = sizeof(variants) / sizeof(variants[0]);
            for (unsigned j = 0; j < size; ++j)
            {
                const unsigned index = (trial + (trial % 2 ? size - 1 - j : j)) % size;
                variants[index].run(variants[index].name, trial, iterations, expected);
            }
        }
    }
    return failures ? 1 : 0;
}
