#include "nuby/asset_memory_cache.hpp"

#include <algorithm>
#include <utility>

namespace nuby {

void AssetMemoryCache::update(const std::string& key, ByteCount bytes,
                              std::uint64_t lastUse) {
    if (key.empty()) return;
    entries_[key] = {bytes, lastUse};
}

void AssetMemoryCache::erase(const std::string& key) {
    entries_.erase(key);
}

void AssetMemoryCache::clear() {
    entries_.clear();
}

std::vector<std::string> AssetMemoryCache::evictUnused(
        const std::unordered_set<std::string>& requiredKeys,
        ByteCount unusedBudgetBytes) {
    struct Candidate {
        std::string key;
        Entry entry;
    };
    std::vector<Candidate> candidates;
    ByteCount unusedTotal = 0;
    for (const auto& item : entries_) {
        if (requiredKeys.find(item.first) != requiredKeys.end()) continue;
        candidates.push_back({item.first, item.second});
        unusedTotal = item.second.bytes > UINT64_MAX - unusedTotal
                ? UINT64_MAX : unusedTotal + item.second.bytes;
    }
    if (unusedTotal <= unusedBudgetBytes) return {};

    std::sort(candidates.begin(), candidates.end(),
            [](const Candidate& left, const Candidate& right) {
                if (left.entry.lastUse != right.entry.lastUse) {
                    return left.entry.lastUse < right.entry.lastUse;
                }
                return left.key < right.key;
            });

    std::vector<std::string> evicted;
    for (const Candidate& candidate : candidates) {
        if (unusedTotal <= unusedBudgetBytes) break;
        const auto entry = entries_.find(candidate.key);
        if (entry == entries_.end()) continue;
        unusedTotal = entry->second.bytes > unusedTotal ? 0
                : unusedTotal - entry->second.bytes;
        evicted.push_back(entry->first);
        entries_.erase(entry);
    }
    return evicted;
}

AssetMemoryCache::ByteCount AssetMemoryCache::totalBytes() const {
    ByteCount total = 0;
    for (const auto& item : entries_) {
        total = item.second.bytes > UINT64_MAX - total
                ? UINT64_MAX : total + item.second.bytes;
    }
    return total;
}

AssetMemoryCache::ByteCount AssetMemoryCache::unusedBytes(
        const std::unordered_set<std::string>& requiredKeys) const {
    ByteCount total = 0;
    for (const auto& item : entries_) {
        if (requiredKeys.find(item.first) != requiredKeys.end()) continue;
        total = item.second.bytes > UINT64_MAX - total
                ? UINT64_MAX : total + item.second.bytes;
    }
    return total;
}

}  // namespace nuby
