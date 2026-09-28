// Copyright (c) relay12 contributors.
// SPDX-License-Identifier: GPL-3.0-only
#pragma once

// Lock-free handoff primitives for D3D12TranslationLayer's batch worker.
//
// Upstream hands batches between the recording thread and the worker with two
// Win32 semaphores and a mutex-guarded std::deque. Under Wine every semaphore
// call is a wineserver round trip unless esync/msync is active, and the
// recording thread polls one of them every ten recorded commands. These
// replace that handoff:
//
//   * RelaySpscRing is a bounded single-producer/single-consumer queue of
//     pointers. The producer owns `head`, the consumer owns `tail`; each side
//     reads the other's index with acquire and publishes its own with release.
//   * RelayAddressWaiter lets one side sleep until a predicate over the ring
//     holds. The waiting side spins briefly, then advertises itself in
//     `sleepers` and re-checks; the notifying side publishes its index and
//     then checks `sleepers`. A seq_cst fence on each side makes this Dekker's
//     pattern, so at least one of them observes the other: either the waiter
//     sees the new index and does not sleep, or the notifier sees the sleeper
//     and wakes it. A notifier with no sleeper makes no system call.
//
// The waiter sleeps on its own `word`, not on a ring index, so a wake that is
// not an index change (worker shutdown) cannot be lost: the notifier bumps the
// word before waking, and the sleeper passes the value it read before
// advertising itself, so a bump in between makes the wait return at once.
//
// The sleep itself is a policy. Win32AddressWaitPolicy is WaitOnAddress, which
// Wine implements over NtWaitForAlertByThreadId without the server; the
// condition-variable policy exists so the host test can run the same code.

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <type_traits>

#if defined(_WIN32)
#include <windows.h>
#include <synchapi.h>
#else
#include <condition_variable>
#include <mutex>
#endif

namespace relay12
{
    inline void RelayCpuRelax() noexcept
    {
#if defined(_WIN32)
        YieldProcessor();
#elif defined(__x86_64__) || defined(__i386__)
        __builtin_ia32_pause();
#elif defined(__aarch64__)
        __asm__ __volatile__("yield");
#endif
    }

    template <typename T, std::size_t Capacity>
    class RelaySpscRing
    {
        static_assert(std::is_pointer<T>::value, "the ring carries pointers; empty is reported as nullptr");
        static_assert(Capacity != 0 && (Capacity & (Capacity - 1)) == 0, "capacity must be a power of two");
        static_assert(std::atomic<std::uint64_t>::is_always_lock_free, "ring indices must be lock-free");
        static constexpr std::uint64_t c_Mask = Capacity - 1;

    public:
        static constexpr std::size_t capacity() noexcept { return Capacity; }

        // Producer only.
        bool try_push(T value) noexcept
        {
            if (!value) return false; // nullptr is the empty sentinel.
            const std::uint64_t head = m_Head.load(std::memory_order_relaxed);
            if (head - m_Tail.load(std::memory_order_acquire) >= Capacity)
            {
                return false;
            }
            m_Slots[head & c_Mask] = value;
            m_Head.store(head + 1, std::memory_order_release);
            return true;
        }

        // Consumer only. Returns the oldest queued element without removing
        // it, or nullptr when the ring is empty.
        T front() const noexcept
        {
            const std::uint64_t tail = m_Tail.load(std::memory_order_relaxed);
            if (m_Head.load(std::memory_order_acquire) == tail)
            {
                return nullptr;
            }
            return m_Slots[tail & c_Mask];
        }

        // Consumer only. Requires a non-empty ring; releases the slot to the
        // producer, and with it every write the consumer made before the pop.
        void pop() noexcept
        {
            const std::uint64_t tail = m_Tail.load(std::memory_order_relaxed);
            m_Tail.store(tail + 1, std::memory_order_release);
        }

        // Consumer only.
        bool try_pop(T& out) noexcept
        {
            T value = front();
            if (!value)
            {
                return false;
            }
            out = value;
            pop();
            return true;
        }

        // Either side. Exact for the calling side's own index; the other
        // side's may move immediately afterwards.
        std::size_t size() const noexcept
        {
            const std::uint64_t tail = m_Tail.load(std::memory_order_acquire);
            return static_cast<std::size_t>(m_Head.load(std::memory_order_acquire) - tail);
        }
        bool empty() const noexcept { return size() == 0; }

        // Count of elements the consumer has released. Lets the producer see
        // that the consumer made progress without keeping its own counter.
        std::uint64_t consumed() const noexcept { return m_Tail.load(std::memory_order_acquire); }

        // Visits [tail, head) oldest first. The caller must stop the consumer
        // from popping for the duration (the batch worker's sync lock); the
        // producer may keep pushing, and anything it pushes after the head
        // load is simply not visited.
        template <typename TFunc>
        void for_each_queued(TFunc&& fn) const
        {
            const std::uint64_t tail = m_Tail.load(std::memory_order_acquire);
            const std::uint64_t head = m_Head.load(std::memory_order_acquire);
            for (std::uint64_t i = tail; i != head; ++i)
            {
                fn(m_Slots[i & c_Mask]);
            }
        }

    private:
        T m_Slots[Capacity] = {};
        alignas(64) std::atomic<std::uint64_t> m_Head{ 0 };
        alignas(64) std::atomic<std::uint64_t> m_Tail{ 0 };
    };

#if defined(_WIN32)
    // Production policy. Needs -lsynchronization (api-ms-win-core-synch-l1-2-0).
    struct Win32AddressWaitPolicy
    {
        static_assert(sizeof(std::atomic<std::uint64_t>) == sizeof(std::uint64_t), "WaitOnAddress compares the raw word");
        static_assert(std::atomic<std::uint64_t>::is_always_lock_free, "WaitOnAddress compares the raw word");

        void wait(std::atomic<std::uint64_t>& word, std::uint64_t expected) noexcept
        {
            WaitOnAddress(reinterpret_cast<volatile VOID*>(&word), &expected, sizeof(expected), INFINITE);
        }
        void wake_all(std::atomic<std::uint64_t>& word) noexcept
        {
            WakeByAddressAll(reinterpret_cast<PVOID>(&word));
        }
    };

    // SRWLOCK with the std Lockable shape, so std::lock_guard applies. Not
    // recursive; an uncontended acquire makes no system call.
    class RelaySrwLock
    {
    public:
        RelaySrwLock() noexcept { InitializeSRWLock(&m_Lock); }
        RelaySrwLock(RelaySrwLock const&) = delete;
        RelaySrwLock& operator=(RelaySrwLock const&) = delete;
        void lock() noexcept { AcquireSRWLockExclusive(&m_Lock); }
        void unlock() noexcept { ReleaseSRWLockExclusive(&m_Lock); }

    private:
        SRWLOCK m_Lock;
    };
#else
    // Host-test policy. The target is C++17, which has no std::atomic::wait.
    // wake_all takes the mutex before notifying, so a waiter that has checked
    // the word under the mutex but not yet blocked cannot miss the wake.
    struct ConditionVariableWaitPolicy
    {
        void wait(std::atomic<std::uint64_t>& word, std::uint64_t expected)
        {
            std::unique_lock<std::mutex> lock(m_Mutex);
            m_Condition.wait(lock, [&] { return word.load(std::memory_order_acquire) != expected; });
        }
        void wake_all(std::atomic<std::uint64_t>&)
        {
            {
                std::lock_guard<std::mutex> lock(m_Mutex);
            }
            m_Condition.notify_all();
        }

    private:
        std::mutex m_Mutex;
        std::condition_variable m_Condition;
    };
#endif

    template <typename Policy, unsigned SpinCount = 128>
    class RelayAddressWaiter
    {
    public:
        // Returns once ready() holds. ready() must read state the notifying
        // side publishes before calling notify().
        template <typename TPred>
        void wait_until(TPred&& ready)
        {
            for (unsigned i = 0; i < SpinCount; ++i)
            {
                if (ready())
                {
                    return;
                }
                RelayCpuRelax();
            }
            for (;;)
            {
                const std::uint64_t observed = m_Word.load(std::memory_order_acquire);
                m_Sleepers.fetch_add(1, std::memory_order_seq_cst);
                std::atomic_thread_fence(std::memory_order_seq_cst);
                if (ready())
                {
                    m_Sleepers.fetch_sub(1, std::memory_order_relaxed);
                    return;
                }
                m_Policy.wait(m_Word, observed);
                m_Sleepers.fetch_sub(1, std::memory_order_relaxed);
                if (ready())
                {
                    return;
                }
            }
        }

        // Call after publishing the state ready() reads. No syscall when nobody
        // sleeps: one fence and one load.
        void notify()
        {
            std::atomic_thread_fence(std::memory_order_seq_cst);
            if (m_Sleepers.load(std::memory_order_relaxed) != 0)
            {
                m_Word.fetch_add(1, std::memory_order_seq_cst);
                m_Wakes.fetch_add(1, std::memory_order_relaxed);
                m_Policy.wake_all(m_Word);
            }
        }

        // Number of notify() calls that reached the wait policy. Diagnostic.
        std::uint64_t wakes() const noexcept { return m_Wakes.load(std::memory_order_relaxed); }

    private:
        alignas(64) std::atomic<std::uint64_t> m_Word{ 0 };
        std::atomic<std::uint32_t> m_Sleepers{ 0 };
        std::atomic<std::uint64_t> m_Wakes{ 0 };
        Policy m_Policy;
    };

#if defined(_WIN32)
    using RelayBatchWaiter = RelayAddressWaiter<Win32AddressWaitPolicy>;
#endif
}
