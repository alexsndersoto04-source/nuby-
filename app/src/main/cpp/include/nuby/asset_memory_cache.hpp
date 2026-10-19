#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace nuby {

// Accounts asset-memory estimates and chooses deterministic LRU victims among
// resources that are not currently required by a scene. Required entries are never
// selected, even if they alone exceed the unused-resource budget.
class AssetMemoryCache final {
public:
    using ByteCount = std::uint64_t;

    void update(const std::string& key, ByteCount bytes, std::uint64_t lastUse);
    void erase(const std::string& key);
    void clear();

    std::vector<std::string> evictUnused(
            const std::unordered_set<std::string>& requiredKeys,
            ByteCount unusedBudgetBytes);

    ByteCount totalBytes() const;
    ByteCount unusedBytes(const std::unordered_set<std::string>& requiredKeys) const;
    std::size_t size() const { return entries_.size(); }

private:
    struct Entry {
        ByteCount bytes = 0;
        std::uint64_t lastUse = 0;
    };

    std::unordered_map<std::string, Entry> entries_;
};

}  // namespace nuby
