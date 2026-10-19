#include "nuby/async_asset_task_queue.hpp"

#include <algorithm>
#include <utility>

namespace nuby {

AsyncAssetTaskQueue::AsyncAssetTaskQueue(std::size_t maximumOutstanding)
    : maximumOutstanding_(maximumOutstanding == 0U ? 1U : maximumOutstanding),
      worker_(&AsyncAssetTaskQueue::workerLoop, this) {}

AsyncAssetTaskQueue::~AsyncAssetTaskQueue() {
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

bool AsyncAssetTaskQueue::submit(std::string key, Task task) {
    if (key.empty() || !task) return false;
    std::lock_guard<std::mutex> lock(mutex_);
    if (stopping_) return false;

    const auto existing = activeTokens_.find(key);
    if (existing == activeTokens_.end() && activeTokens_.size() >= maximumOutstanding_) {
        return false;
    }

    const std::uint64_t token = nextToken_++;
    activeTokens_[key] = token;
    requests_.erase(std::remove_if(requests_.begin(), requests_.end(),
            [&key](const Request& request) { return request.key == key; }), requests_.end());
    completed_.erase(std::remove_if(completed_.begin(), completed_.end(),
            [&key](const Completed& result) { return result.key == key; }), completed_.end());
    requests_.push_back({std::move(key), token, std::move(task)});
    wakeCondition_.notify_one();
    return true;
}

void AsyncAssetTaskQueue::cancel(const std::string& key) {
    std::lock_guard<std::mutex> lock(mutex_);
    activeTokens_.erase(key);
    requests_.erase(std::remove_if(requests_.begin(), requests_.end(),
            [&key](const Request& request) { return request.key == key; }), requests_.end());
    completed_.erase(std::remove_if(completed_.begin(), completed_.end(),
            [&key](const Completed& result) { return result.key == key; }), completed_.end());
    if (requests_.empty() && !workerBusy_) idleCondition_.notify_all();
}

void AsyncAssetTaskQueue::cancelAll() {
    std::lock_guard<std::mutex> lock(mutex_);
    activeTokens_.clear();
    requests_.clear();
    completed_.clear();
    if (!workerBusy_) idleCondition_.notify_all();
}

void AsyncAssetTaskQueue::poll(std::vector<Completion>& completions,
                               std::size_t maximumCompletions) {
    std::lock_guard<std::mutex> lock(mutex_);
    while (!completed_.empty() && maximumCompletions > 0U) {
        Completed result = std::move(completed_.front());
        completed_.pop_front();
        const auto active = activeTokens_.find(result.key);
        if (active != activeTokens_.end() && active->second == result.token) {
            activeTokens_.erase(active);
            if (result.completion) completions.push_back(std::move(result.completion));
        }
        --maximumCompletions;
    }
}

std::size_t AsyncAssetTaskQueue::outstandingCount() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return activeTokens_.size();
}

bool AsyncAssetTaskQueue::waitUntilIdle(std::chrono::milliseconds timeout) {
    std::unique_lock<std::mutex> lock(mutex_);
    return idleCondition_.wait_for(lock, timeout,
            [this] { return requests_.empty() && !workerBusy_; });
}

bool AsyncAssetTaskQueue::isCurrentLocked(const Request& request) const {
    const auto active = activeTokens_.find(request.key);
    return active != activeTokens_.end() && active->second == request.token;
}

void AsyncAssetTaskQueue::workerLoop() {
    for (;;) {
        Request request;
        {
            std::unique_lock<std::mutex> lock(mutex_);
            wakeCondition_.wait(lock, [this] { return stopping_ || !requests_.empty(); });
            if (stopping_) return;
            request = std::move(requests_.front());
            requests_.pop_front();
            if (!isCurrentLocked(request)) {
                if (requests_.empty() && !workerBusy_) idleCondition_.notify_all();
                continue;
            }
            workerBusy_ = true;
        }

        Completion completion;
        try {
            completion = request.task();
        } catch (...) {
            // A failed asset task still completes and releases its request slot.
        }

        {
            std::lock_guard<std::mutex> lock(mutex_);
            workerBusy_ = false;
            if (!stopping_ && isCurrentLocked(request)) {
                completed_.push_back({std::move(request.key), request.token,
                                      std::move(completion)});
            }
            if (requests_.empty()) idleCondition_.notify_all();
        }
    }
}

}  // namespace nuby
