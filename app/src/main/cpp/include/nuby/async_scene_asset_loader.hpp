#pragma once

#include "nuby/scene.hpp"

#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <functional>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

namespace nuby {

struct SceneAssetLoadResult {
    std::uint32_t requestId = 0;
    std::string assetPath;
    bool success = false;
    Scene scene;
    std::string error;
};

// One bounded worker keeps file IO and scene deserialization off the render
// thread. Reader callbacks run only on that worker and must not touch renderer
// state; scene integration and GPU resource creation remain on the caller.
class AsyncSceneAssetLoader final {
public:
    using Reader = std::function<bool(const std::string&, Scene&, std::string&)>;

    explicit AsyncSceneAssetLoader(Reader reader);
    ~AsyncSceneAssetLoader();

    AsyncSceneAssetLoader(const AsyncSceneAssetLoader&) = delete;
    AsyncSceneAssetLoader& operator=(const AsyncSceneAssetLoader&) = delete;

    bool request(std::uint32_t requestId, std::string assetPath);
    void cancel(std::uint32_t requestId);
    void cancelAll();
    void poll(std::vector<SceneAssetLoadResult>& results);
    std::size_t outstandingCount() const;

    // Primarily useful for deterministic shutdown and tests; the frame loop
    // should poll asynchronously rather than block on this function.
    bool waitUntilIdle(std::chrono::milliseconds timeout);

private:
    struct Request {
        std::uint32_t requestId = 0;
        std::uint64_t token = 0;
        std::string assetPath;
    };

    void workerLoop();
    bool isCurrentLocked(const Request& request) const;

    Reader reader_;
    mutable std::mutex mutex_;
    std::condition_variable wakeCondition_;
    std::condition_variable idleCondition_;
    std::deque<Request> requests_;
    std::vector<SceneAssetLoadResult> completed_;
    std::unordered_map<std::uint32_t, std::uint64_t> activeTokens_;
    std::uint64_t nextToken_ = 1;
    bool workerBusy_ = false;
    bool stopping_ = false;
    std::thread worker_;
};

}  // namespace nuby
