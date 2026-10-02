// SPDX-License-Identifier: GPL-3.0-only
#pragma once
#include <algorithm>
#include <chrono>
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
    std::uint64_t softLimit = 0, burstLimit = 0, graceMilliseconds = 0, effectiveLimit = 0;
    bool burstActive = false;
};
struct PoolLimits {
    std::uint64_t softBytes, burstBytes, graceMilliseconds;
};
enum class PoolTrimReason { Idle, Pressure, Teardown };
inline bool ParsePoolSetting(const char* text, std::uint64_t maximum, std::uint64_t& value) noexcept
{
    if (!text || !*text) return false;
    std::uint64_t parsed = 0;
    for (; *text; ++text)
    {
        if (*text < '0' || *text > '9') return false;
        const auto digit = static_cast<std::uint64_t>(*text - '0');
        if (digit > maximum || parsed > (maximum - digit) / 10) return false;
        parsed = parsed * 10 + digit;
    }
    value = parsed;
    return true;
}

// Only retired resources enter this pool. "Retained" includes pending GPU
// references; only completed entries count towards the reclaimable byte cap.
// Fence values belong to one timeline. Caller drains that timeline at teardown.
// Successful workload activity temporarily admits completed bytes up to burst.
// Reclamation after grace is driven by pool operations, not a timer thread.
// Pressure cancels grace; teardown releases completed entries only. Neither
// operation releases GPU-pending entries, even when they exceed both limits.
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
    PoolLimits limits;
    using Clock = std::uint64_t (*)(void*) noexcept;
    Clock clock;
    void* clockContext;
    std::uint64_t lastClock = 0, lastActivity = 0;
    bool hasActivity = false;
    std::uint64_t completedFence = 0;
    PoolCounters counters;

    static std::uint64_t monotonicMilliseconds(void*) noexcept
    {
        return static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now().time_since_epoch()).count());
    }
    static PoolLimits normalize(PoolLimits value) noexcept
    {
        value.burstBytes = (std::max)(value.softBytes, value.burstBytes);
        return value;
    }
    std::uint64_t now() noexcept
    {
        // Defensive clamping also makes injected-clock rollback harmless.
        lastClock = (std::max)(lastClock, clock(clockContext));
        return lastClock;
    }
    void activity() noexcept
    {
        // Strict pools incur no clock call; only a grace policy needs time.
        hasActivity = limits.graceMilliseconds != 0;
        if (hasActivity) lastActivity = now();
    }
    std::uint64_t activeLimit(std::uint64_t time) const noexcept
    {
        return hasActivity && limits.graceMilliseconds && time - lastActivity < limits.graceMilliseconds
            ? limits.burstBytes : limits.softBytes;
    }
    std::uint64_t policyLimit() noexcept
    {
        return limits.graceMilliseconds ? activeLimit(now()) : limits.softBytes;
    }
    void publishLimits(std::uint64_t cap) noexcept
    {
        counters.softLimit = limits.softBytes;
        counters.burstLimit = limits.burstBytes;
        counters.graceMilliseconds = limits.graceMilliseconds;
        counters.effectiveLimit = cap;
        counters.burstActive = cap > limits.softBytes;
    }

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
    void trimLocked(std::uint64_t cap) noexcept
    {
        publishLimits(cap);
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
    explicit BoundedMemoryPool(std::uint64_t limit) noexcept : BoundedMemoryPool(PoolLimits{limit, limit, 0}) {}
    explicit BoundedMemoryPool(PoolLimits value, Clock clockFunction = nullptr, void* context = nullptr) noexcept
        : limits(normalize(value)), clock(clockFunction ? clockFunction : monotonicMilliseconds), clockContext(context)
    {
        publishLimits(limits.softBytes);
    }
    void configure(PoolLimits value) noexcept
    {
        std::lock_guard<std::mutex> lock(mutex);
        limits = normalize(value);
        trimLocked(policyLimit());
    }
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
        activity();
        trimLocked(activeLimit(lastClock));
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
                activity();
                trimLocked(activeLimit(lastClock));
                return result;
            }
        }
        trimLocked(policyLimit());
        return Resource{};
    }
    void allocated() noexcept
    {
        std::lock_guard<std::mutex> lock(mutex);
        ++counters.allocations;
        activity();
        trimLocked(activeLimit(lastClock));
    }
    void trim(std::uint64_t completed, PoolTrimReason reason = PoolTrimReason::Idle) noexcept
    {
        std::lock_guard<std::mutex> lock(mutex);
        advanceFence(completed);
        if (reason != PoolTrimReason::Idle) hasActivity = false;
        trimLocked(reason == PoolTrimReason::Teardown ? 0 : policyLimit());
    }
    PoolCounters snapshot() noexcept
    {
        std::lock_guard<std::mutex> lock(mutex);
        // Observation updates the reported effective cap but never extends
        // grace or destroys resources; reclamation happens on pool operations.
        publishLimits(policyLimit());
        return counters;
    }
};
}
