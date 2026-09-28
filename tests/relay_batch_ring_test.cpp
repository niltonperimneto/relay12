// SPDX-License-Identifier: GPL-3.0-only
//
// Contract test for the lock-free batch handoff (compat/relay_batch_ring.hpp).
//
// D3D12TranslationLayer's worker thread is the only consumer of these
// primitives, and no CI lane runs that worker against a real D3D12 device. A
// lost wake there hangs the device on the next flush; a lost or duplicated
// batch corrupts rendering. So the properties are pinned here, where they can
// be exercised deterministically:
//
//   * FIFO with nothing lost or duplicated across 10^6 items through a ring
//     bounded like the worker's (five outstanding batches);
//   * a producer blocked on a full ring and a consumer blocked on an empty one
//     are both woken;
//   * for_each_queued concurrent with pops, under the sync lock, only ever
//     sees live, ordered elements;
//   * shutdown while the consumer sleeps wakes it;
//   * a consumer that never sleeps receives no wake at all.
//
// Built natively with the condition-variable policy and, in the MinGW/Wine
// lane, with WaitOnAddress, which is the policy the driver uses.

#include "relay_batch_ring.hpp"

#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <mutex>
#include <thread>
#include <vector>

using namespace relay12;

namespace
{
    std::atomic<int> failures{ 0 };

    void check(bool condition, const char* what)
    {
        if (!condition)
        {
            std::printf("[fail] %s\n", what);
            ++failures;
        }
    }

#if defined(_WIN32)
    using Policy = Win32AddressWaitPolicy;
    using SyncLock = RelaySrwLock;
#else
    using Policy = ConditionVariableWaitPolicy;
    using SyncLock = std::mutex;
#endif
    using Waiter = RelayAddressWaiter<Policy>;

    constexpr std::size_t c_MaxOutstanding = 5;

    // Values are encoded as pointers; 0 is reserved for "empty".
    std::uintptr_t* encode(std::uint64_t v) { return reinterpret_cast<std::uintptr_t*>(static_cast<std::uintptr_t>(v + 1)); }
    std::uint64_t decode(std::uintptr_t* p) { return static_cast<std::uint64_t>(reinterpret_cast<std::uintptr_t>(p)) - 1; }

    using Ring = RelaySpscRing<std::uintptr_t*, 8>;

    // Mirrors the worker handoff: the producer waits for room below the
    // outstanding bound, the consumer waits for work or exit.
    struct Handoff
    {
        Ring ring;
        Waiter submitted;
        Waiter consumed;
        std::atomic<bool> exit{ false };

        void push(std::uint64_t v)
        {
            consumed.wait_until([&] { return ring.size() < c_MaxOutstanding; });
            bool pushed = ring.try_push(encode(v));
            check(pushed, "push below the outstanding bound succeeds");
            submitted.notify();
        }
        // Returns false on exit with an empty ring.
        bool pop(std::uint64_t& v)
        {
            submitted.wait_until([&] { return !ring.empty() || exit.load(std::memory_order_acquire); });
            auto p = ring.front();
            if (!p)
            {
                return false;
            }
            v = decode(p);
            ring.pop();
            consumed.notify();
            return true;
        }
        void shutdown()
        {
            exit.store(true, std::memory_order_seq_cst);
            submitted.notify();
        }
    };

    void test_fifo_million()
    {
        constexpr std::uint64_t c_Count = 1000000;
        Handoff h;
        std::uint64_t expected = 0;
        bool ordered = true;
        std::thread consumer([&] {
            std::uint64_t v;
            while (h.pop(v))
            {
                if (v != expected)
                {
                    ordered = false;
                }
                ++expected;
            }
        });
        for (std::uint64_t i = 0; i < c_Count; ++i)
        {
            h.push(i);
        }
        h.shutdown();
        consumer.join();
        check(ordered, "10^6 items arrive in FIFO order");
        check(expected == c_Count, "10^6 items arrive with none lost or duplicated");
        check(h.ring.empty(), "the ring drains completely before exit");
    }

    void test_blocked_producer_and_consumer_are_woken()
    {
        // Producer fills the ring, then blocks; the consumer, started late,
        // must wake it.
        {
            Handoff h;
            for (std::uint64_t i = 0; i < c_MaxOutstanding; ++i)
            {
                h.push(i);
            }
            std::atomic<bool> producerDone{ false };
            std::thread producer([&] {
                h.push(c_MaxOutstanding);
                producerDone = true;
            });
            std::this_thread::sleep_for(std::chrono::milliseconds(50));
            check(!producerDone, "a producer at the outstanding bound blocks");
            std::uint64_t v;
            check(h.pop(v) && v == 0, "the blocked producer's ring yields its oldest item");
            producer.join();
            check(producerDone, "a producer blocked on a full ring is woken by a pop");
        }
        // Consumer blocks on an empty ring; a late push must wake it.
        {
            Handoff h;
            std::atomic<bool> got{ false };
            std::uint64_t value = 0;
            std::thread consumer([&] {
                std::uint64_t v;
                if (h.pop(v))
                {
                    value = v;
                    got = true;
                }
            });
            std::this_thread::sleep_for(std::chrono::milliseconds(50));
            check(!got, "a consumer on an empty ring blocks");
            h.push(42);
            consumer.join();
            check(got && value == 42, "a consumer blocked on an empty ring is woken by a push");
            check(h.submitted.wakes() >= 1, "waking a sleeping consumer goes through the wait policy");
        }
    }

    void test_scan_concurrent_with_pops()
    {
        constexpr std::uint64_t c_Count = 200000;
        Handoff h;
        SyncLock syncLock;
        std::atomic<bool> producing{ true };
        std::atomic<bool> scanOk{ true };
        std::uint64_t scans = 0;

        std::thread consumer([&] {
            std::uint64_t last = 0;
            bool first = true;
            for (;;)
            {
                h.submitted.wait_until([&] { return !h.ring.empty() || h.exit.load(std::memory_order_acquire); });
                std::uintptr_t* p;
                {
                    // Pops happen only under the lock, like the worker's
                    // retire step.
                    std::lock_guard<SyncLock> lock(syncLock);
                    p = h.ring.front();
                    if (p)
                    {
                        h.ring.pop();
                    }
                }
                if (!p)
                {
                    return;
                }
                const std::uint64_t v = decode(p);
                if (!first && v != last + 1)
                {
                    scanOk = false;
                }
                first = false;
                last = v;
                h.consumed.notify();
            }
        });
        std::thread scanner([&] {
            while (producing.load(std::memory_order_acquire))
            {
                std::lock_guard<SyncLock> lock(syncLock);
                bool first = true;
                std::uint64_t prev = 0;
                std::size_t seen = 0;
                h.ring.for_each_queued([&](std::uintptr_t* p) {
                    const std::uint64_t v = decode(p);
                    if (!p || v >= c_Count || (!first && v != prev + 1))
                    {
                        scanOk = false;
                    }
                    first = false;
                    prev = v;
                    ++seen;
                });
                if (seen > c_MaxOutstanding)
                {
                    scanOk = false;
                }
                ++scans;
            }
        });
        for (std::uint64_t i = 0; i < c_Count; ++i)
        {
            h.push(i);
        }
        producing = false;
        scanner.join();
        h.shutdown();
        consumer.join();
        check(scanOk, "for_each_queued under the sync lock sees only live, ordered, bounded elements");
        check(scans > 0, "the scanner ran concurrently with the handoff");
    }

    void test_shutdown_wakes_sleeping_consumer()
    {
        Handoff h;
        std::atomic<bool> exited{ false };
        std::thread consumer([&] {
            std::uint64_t v;
            while (h.pop(v))
            {
            }
            exited = true;
        });
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
        check(!exited, "an idle consumer sleeps");
        h.shutdown();
        consumer.join();
        check(exited, "shutdown wakes a sleeping consumer");
    }

    void test_busy_consumer_receives_no_wake()
    {
        // The consumer only spins (it never calls wait_until), so nobody is
        // ever registered as sleeping and notify() must not reach the policy.
        Handoff h;
        constexpr std::uint64_t c_Count = 100000;
        std::atomic<bool> done{ false };
        std::uint64_t received = 0;
        std::thread consumer([&] {
            while (!done.load(std::memory_order_acquire) || !h.ring.empty())
            {
                if (auto p = h.ring.front())
                {
                    (void)p;
                    h.ring.pop();
                    ++received;
                }
            }
        });
        for (std::uint64_t i = 0; i < c_Count; ++i)
        {
            while (!h.ring.try_push(encode(i)))
            {
                RelayCpuRelax();
            }
            h.submitted.notify();
        }
        done = true;
        consumer.join();
        check(received == c_Count, "a spinning consumer receives every item");
        check(h.submitted.wakes() == 0, "notify() with no sleeper makes no wake call");
    }

    void test_capacity_bound()
    {
        Ring ring;
        for (std::uint64_t i = 0; i < Ring::capacity(); ++i)
        {
            check(ring.try_push(encode(i)), "a push below capacity succeeds");
        }
        check(!ring.try_push(encode(99)), "a push at capacity fails without overwriting");
        std::uintptr_t* p = nullptr;
        check(ring.try_pop(p) && decode(p) == 0, "a full ring still yields its oldest item");
        check(ring.try_push(encode(8)), "a pop frees exactly one slot");
        check(ring.size() == Ring::capacity(), "size tracks head minus tail across the wrap");
        check(ring.consumed() == 1, "consumed counts pops");
    }
}

int main()
{
    test_capacity_bound();
    test_fifo_million();
    test_blocked_producer_and_consumer_are_woken();
    test_scan_concurrent_with_pops();
    test_shutdown_wakes_sleeping_consumer();
    test_busy_consumer_receives_no_wake();

    if (failures)
    {
        std::printf("[fail] %d check(s) failed\n", failures.load());
        return 1;
    }
    std::printf("[ ok ] the batch handoff is FIFO, bounded, and loses no wake\n");
    return 0;
}
