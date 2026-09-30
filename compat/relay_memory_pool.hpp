// SPDX-License-Identifier: GPL-3.0-only
#pragma once
#include <algorithm>
#include <cstdint>
#include <cstring>
#include <limits>
#include <list>
#include <mutex>
#include <memory>
#include <new>
#include <utility>

namespace relay12 {
enum class MemoryProfile { Auto, Legacy, Balanced, Invalid };
inline MemoryProfile ParseMemoryProfile(const char* value) noexcept
{
    if (!value || !*value || !std::strcmp(value, "auto")) return MemoryProfile::Auto;
    if (!std::strcmp(value, "legacy")) return MemoryProfile::Legacy;
    if (!std::strcmp(value, "balanced")) return MemoryProfile::Balanced;
    return MemoryProfile::Invalid;
}
// Promotion stays disabled until the documented real-GPU acceptance run passes.
// Explicit balanced remains available for that run; capability alone is not a
// hardware correctness or frame-time result.
inline constexpr bool AutomaticMemoryProfileQualified = false;
inline bool SelectBalancedMemory(MemoryProfile profile, bool uma, std::uint64_t physicalBytes,
                                 bool qualified = AutomaticMemoryProfileQualified) noexcept
{
    if (profile == MemoryProfile::Balanced) return true;
    return qualified && profile == MemoryProfile::Auto && uma && physicalBytes && physicalBytes <= (8ull << 30);
}
struct PoolCounters {
    std::uint64_t retained = 0, peakRetained = 0, pending = 0, completed = 0;
    std::uint64_t allocations = 0, reuses = 0, trims = 0;
};

// Only retired resources enter this pool. "Retained" includes pending GPU
// references; only completed entries count towards the reclaimable byte cap.
// Fence values belong to one timeline. Caller drains that timeline at teardown.
template<class Resource, class Allocator = std::allocator<Resource>>
class BoundedMemoryPool {
    struct Entry {
        std::uint64_t key, bytes, fence;
        Resource resource;
        Entry(std::uint64_t k, std::uint64_t b, std::uint64_t f)
            : key(k), bytes(b), fence(f) {}
    };
    using EntryAllocator = typename std::allocator_traits<Allocator>::template rebind_alloc<Entry>;
    std::list<Entry, EntryAllocator> entries;
    std::mutex mutex;
    const std::uint64_t cap;
    std::uint64_t completedFence = 0;
    PoolCounters counters;

    void classify() noexcept
    {
        counters.pending = counters.completed = 0;
        for (const auto& entry : entries)
            (entry.fence <= completedFence ? counters.completed : counters.pending) += entry.bytes;
    }
    void advanceFence(std::uint64_t completed) noexcept
    {
        if (completed > completedFence)
        {
            completedFence = completed;
            classify();
        }
    }
    void trimLocked() noexcept
    {
        // Oldest return first; never wait or release an unfinished entry.
        for (auto it = entries.begin(); it != entries.end() && counters.completed > cap;)
        {
            if (it->fence <= completedFence)
            {
                counters.completed -= it->bytes;
                counters.retained -= it->bytes;
                ++counters.trims;
                it = entries.erase(it);
            }
            else ++it;
        }
    }
public:
    explicit BoundedMemoryPool(std::uint64_t limit) noexcept : cap(limit) {}
    // On metadata allocation failure the caller retains ownership and must
    // use its fence-aware deferred deletion path. Never silently drop it.
    bool put(std::uint64_t key, std::uint64_t bytes, std::uint64_t fence, Resource&& resource) noexcept
    {
        std::lock_guard<std::mutex> lock(mutex);
        if (!bytes || bytes > (std::numeric_limits<std::uint64_t>::max)() - counters.retained) return false;
        try { entries.emplace_back(key, bytes, fence); }
        catch (const std::bad_alloc&) { return false; }
        entries.back().resource = std::move(resource);
        counters.retained += bytes;
        (fence <= completedFence ? counters.completed : counters.pending) += bytes;
        counters.peakRetained = (std::max)(counters.peakRetained, counters.retained);
        trimLocked();
        return true;
    }
    Resource take(std::uint64_t key, std::uint64_t completed)
    {
        std::lock_guard<std::mutex> lock(mutex);
        advanceFence(completed);
        for (auto it = entries.begin(); it != entries.end(); ++it)
        {
            if (it->key == key && it->fence <= completedFence)
            {
                Resource result = std::move(it->resource);
                counters.retained -= it->bytes;
                counters.completed -= it->bytes;
                entries.erase(it);
                ++counters.reuses;
                trimLocked();
                return result;
            }
        }
        trimLocked();
        return Resource{};
    }
    void allocated() noexcept
    {
        std::lock_guard<std::mutex> lock(mutex);
        ++counters.allocations;
    }
    void trim(std::uint64_t completed) noexcept
    {
        std::lock_guard<std::mutex> lock(mutex);
        advanceFence(completed);
        trimLocked();
    }
    PoolCounters snapshot() noexcept
    {
        std::lock_guard<std::mutex> lock(mutex);
        return counters;
    }
};
}
