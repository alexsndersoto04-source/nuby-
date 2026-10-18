#include "nuby/async_scene_asset_loader.hpp"

#include <algorithm>
#include <exception>
#include <limits>
#include <utility>

namespace nuby {
namespace {
constexpr std::size_t kMaximumAssetPathLength = 1024;
constexpr std::size_t kMaximumOutstandingRequests = 8;
}

AsyncSceneAssetLoader::AsyncSceneAssetLoader(Reader reader)
        : reader_(std::move(reader)), worker_(&AsyncSceneAssetLoader::workerLoop, this) {}

AsyncSceneAssetLoader::~AsyncSceneAssetLoader() {
    {
        std::lock_guard<std::mutex> lock(mutex_);
        stopping_ = true;
        requests_.clear();
        completed_.clear();
        activeTokens_.clear();
    }
    wakeCondition_.notify_all();
    if (worker_.joinable()) worker_.join();
}

bool AsyncSceneAssetLoader::request(std::uint32_t requestId, std::string assetPath) {
    if (requestId == 0U || assetPath.empty()
            || assetPath.size() > kMaximumAssetPathLength || !reader_) {
        return false;
    }
    std::lock_guard<std::mutex> lock(mutex_);
    if (stopping_ || nextToken_ == std::numeric_limits<std::uint64_t>::max()) return false;
    if (activeTokens_.find(requestId) == activeTokens_.end()
            && activeTokens_.size() >= kMaximumOutstandingRequests) {
        return false;
    }

    const std::uint64_t token = nextToken_++;
    activeTokens_[requestId] = token;
    requests_.erase(std::remove_if(requests_.begin(), requests_.end(),
            [requestId](const Request& queued) { return queued.requestId == requestId; }),
            requests_.end());
    completed_.erase(std::remove_if(completed_.begin(), completed_.end(),
            [requestId](const SceneAssetLoadResult& result) {
                return result.requestId == requestId;
            }), completed_.end());
    requests_.push_back({requestId, token, std::move(assetPath)});
    wakeCondition_.notify_one();
    return true;
}

void AsyncSceneAssetLoader::cancel(std::uint32_t requestId) {
    std::lock_guard<std::mutex> lock(mutex_);
    activeTokens_.erase(requestId);
    requests_.erase(std::remove_if(requests_.begin(), requests_.end(),
            [requestId](const Request& queued) { return queued.requestId == requestId; }),
            requests_.end());
    completed_.erase(std::remove_if(completed_.begin(), completed_.end(),
            [requestId](const SceneAssetLoadResult& result) {
                return result.requestId == requestId;
            }), completed_.end());
    if (requests_.empty() && !workerBusy_) idleCondition_.notify_all();
}

void AsyncSceneAssetLoader::cancelAll() {
    std::lock_guard<std::mutex> lock(mutex_);
    activeTokens_.clear();
    requests_.clear();
    completed_.clear();
    if (!workerBusy_) idleCondition_.notify_all();
}

void AsyncSceneAssetLoader::poll(std::vector<SceneAssetLoadResult>& results) {
    results.clear();
    std::lock_guard<std::mutex> lock(mutex_);
    for (SceneAssetLoadResult& result : completed_) {
        const auto current = activeTokens_.find(result.requestId);
        if (current == activeTokens_.end()) continue;
        results.push_back(std::move(result));
        activeTokens_.erase(current);
    }
    completed_.clear();
    if (requests_.empty() && !workerBusy_) idleCondition_.notify_all();
}

std::size_t AsyncSceneAssetLoader::outstandingCount() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return activeTokens_.size();
}

bool AsyncSceneAssetLoader::waitUntilIdle(std::chrono::milliseconds timeout) {
    std::unique_lock<std::mutex> lock(mutex_);
    return idleCondition_.wait_for(lock, timeout,
            [this]() { return requests_.empty() && !workerBusy_; });
}

bool AsyncSceneAssetLoader::isCurrentLocked(const Request& request) const {
    const auto current = activeTokens_.find(request.requestId);
    return current != activeTokens_.end() && current->second == request.token;
}

void AsyncSceneAssetLoader::workerLoop() {
    for (;;) {
        Request request;
        {
            std::unique_lock<std::mutex> lock(mutex_);
            wakeCondition_.wait(lock,
                    [this]() { return stopping_ || !requests_.empty(); });
            if (stopping_) return;
            request = std::move(requests_.front());
            requests_.pop_front();
            if (!isCurrentLocked(request)) {
                if (requests_.empty() && !workerBusy_) idleCondition_.notify_all();
                continue;
            }
            workerBusy_ = true;
        }

        SceneAssetLoadResult result;
        result.requestId = request.requestId;
        result.assetPath = request.assetPath;
        try {
            result.success = reader_(request.assetPath, result.scene, result.error);
            if (result.success) {
                result.error.clear();
            } else if (result.error.empty()) {
                result.error = "asset reader rejected the scene request";
            }
        } catch (const std::exception& exception) {
            result.error = std::string("asset reader threw an exception: ") + exception.what();
        } catch (...) {
            result.error = "asset reader threw an unknown exception";
        }

        {
            std::lock_guard<std::mutex> lock(mutex_);
            workerBusy_ = false;
            if (isCurrentLocked(request)) completed_.push_back(std::move(result));
            if (requests_.empty()) idleCondition_.notify_all();
        }
    }
}

}  // namespace nuby
