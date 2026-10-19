#include "nuby/asset_memory_cache.hpp"

#include <cstdlib>
#include <iostream>
#include <string>
#include <unordered_set>
#include <vector>

namespace {
int failures = 0;

void expect(bool condition, const char* message) {
    if (!condition) {
        std::cerr << "FAIL: " << message << '\n';
        ++failures;
    }
}

void testUnusedAssetsEvictLeastRecentlyUsedFirst() {
    nuby::AssetMemoryCache cache;
    cache.update("mesh:old", 60U, 1U);
    cache.update("texture:mid", 30U, 2U);
    cache.update("mesh:new", 40U, 3U);
    cache.update("mesh:active", 500U, 4U);
    const std::unordered_set<std::string> active{"mesh:active"};

    expect(cache.totalBytes() == 630U && cache.unusedBytes(active) == 130U,
           "cache accounts required and unused GPU allocations separately");
    const std::vector<std::string> evicted = cache.evictUnused(active, 50U);
    expect(evicted.size() == 2U && evicted[0] == "mesh:old"
                   && evicted[1] == "texture:mid",
           "least-recently-used resources are evicted deterministically until budget fits");
    expect(cache.totalBytes() == 540U && cache.unusedBytes(active) == 40U
                   && cache.size() == 2U,
           "eviction frees cache accounting while retaining the active resource and newest cache entry");
}

void testRequiredAssetsAreNeverEvictedEvenWhenOverBudget() {
    nuby::AssetMemoryCache cache;
    cache.update("mesh:active-a", 70U, 1U);
    cache.update("mesh:active-b", 50U, 2U);
    cache.update("texture:old", 20U, 0U);
    const std::unordered_set<std::string> required{"mesh:active-a", "mesh:active-b"};

    const std::vector<std::string> evicted = cache.evictUnused(required, 0U);
    expect(evicted.size() == 1U && evicted.front() == "texture:old",
           "only unreferenced entries may be evicted under a zero-byte cache budget");
    expect(cache.unusedBytes(required) == 0U && cache.totalBytes() == 120U,
           "active memory may exceed the cache budget without dropping required resources");
}

void testUpdatesAdjustMemoryAndTiesUseStableKeys() {
    nuby::AssetMemoryCache cache;
    cache.update("mesh:z", 10U, 5U);
    cache.update("mesh:a", 20U, 5U);
    cache.update("mesh:a", 7U, 8U);
    expect(cache.totalBytes() == 17U,
           "updating an existing asset replaces its old byte estimate");
    const std::vector<std::string> evicted = cache.evictUnused({}, 7U);
    expect(evicted.size() == 1U && evicted.front() == "mesh:z"
                   && cache.totalBytes() == 7U,
           "equal-use entries have deterministic lexical ordering");
    cache.clear();
    expect(cache.size() == 0U && cache.totalBytes() == 0U,
           "clear releases all accounting entries");
}
}  // namespace

int main() {
    testUnusedAssetsEvictLeastRecentlyUsedFirst();
    testRequiredAssetsAreNeverEvictedEvenWhenOverBudget();
    testUpdatesAdjustMemoryAndTiesUseStableKeys();
    if (failures != 0) {
        std::cerr << failures << " asset memory cache test(s) failed\n";
        return EXIT_FAILURE;
    }
    std::cout << "All asset memory cache tests passed\n";
    return EXIT_SUCCESS;
}
