#pragma once
// Striped concurrent hash map.
// N buckets, each with its own std::shared_mutex (read-write lock).
// Read path: shared lock (many concurrent readers).
// Write path: exclusive lock (per-bucket, not global).

#include <array>
#include <cassert>
#include <cstddef>
#include <functional>
#include <mutex>
#include <optional>
#include <shared_mutex>
#include <unordered_map>
#include <utility>
#include <vector>

namespace forge {

template <typename K, typename V, size_t NumStripes = 64>
class ConcurrentMap {
public:
    ConcurrentMap() = default;

    // Non-copyable, non-movable
    ConcurrentMap(const ConcurrentMap&) = delete;
    ConcurrentMap& operator=(const ConcurrentMap&) = delete;

    /// Insert or update a key-value pair. Returns true if newly inserted.
    bool insert(const K& key, V value) {
        auto& stripe = get_stripe(key);
        std::unique_lock lock(stripe.mutex);
        auto [it, inserted] = stripe.map.emplace(key, std::move(value));
        if (!inserted) {
            it->second = std::move(value);  // overwrite
        }
        return inserted;
    }

    /// Find a value by key. Returns std::nullopt if not found.
    std::optional<V> find(const K& key) const {
        const auto& stripe = get_stripe(key);
        std::shared_lock lock(stripe.mutex);
        auto it = stripe.map.find(key);
        if (it == stripe.map.end()) {
            return std::nullopt;
        }
        return it->second;
    }

    /// Erase a key. Returns true if the key existed.
    bool erase(const K& key) {
        auto& stripe = get_stripe(key);
        std::unique_lock lock(stripe.mutex);
        return stripe.map.erase(key) > 0;
    }

    /// Check whether a key exists.
    bool contains(const K& key) const {
        const auto& stripe = get_stripe(key);
        std::shared_lock lock(stripe.mutex);
        return stripe.map.count(key) > 0;
    }

    /// Apply a function to the value under shared lock. Returns false if key not found.
    /// The function signature: void(const V&)
    template <typename Func>
    bool with_value(const K& key, Func&& func) const {
        const auto& stripe = get_stripe(key);
        std::shared_lock lock(stripe.mutex);
        auto it = stripe.map.find(key);
        if (it == stripe.map.end()) {
            return false;
        }
        func(it->second);
        return true;
    }

    /// Apply a mutating function to the value under exclusive lock.
    /// Returns false if key not found.
    /// The function signature: void(V&)
    template <typename Func>
    bool mutate(const K& key, Func&& func) {
        auto& stripe = get_stripe(key);
        std::unique_lock lock(stripe.mutex);
        auto it = stripe.map.find(key);
        if (it == stripe.map.end()) {
            return false;
        }
        func(it->second);
        return true;
    }

    /// Approximate size (iterates all stripes, not atomic snapshot).
    size_t size() const {
        size_t total = 0;
        for (const auto& stripe : stripes_) {
            std::shared_lock lock(stripe.mutex);
            total += stripe.map.size();
        }
        return total;
    }

    bool empty() const { return size() == 0; }

    /// Collect all keys.
    /// NOTE: not a consistent snapshot — keys may be added/removed between
    /// stripe iterations.  Use only for diagnostics or approximate enumeration.
    std::vector<K> keys() const {
        std::vector<K> result;
        for (const auto& stripe : stripes_) {
            std::shared_lock lock(stripe.mutex);
            for (const auto& [k, v] : stripe.map) {
                result.push_back(k);
            }
        }
        return result;
    }

private:
    struct Stripe {
        mutable std::shared_mutex mutex;
        std::unordered_map<K, V> map;
    };

    Stripe& get_stripe(const K& key) {
        return stripes_[std::hash<K>{}(key) % NumStripes];
    }

    const Stripe& get_stripe(const K& key) const {
        return stripes_[std::hash<K>{}(key) % NumStripes];
    }

    std::array<Stripe, NumStripes> stripes_;
};

}  // namespace forge
