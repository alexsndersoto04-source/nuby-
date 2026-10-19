#pragma once

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

// Runs CPU-only asset work on one bounded worker. Results are callbacks that
// the owner executes from its main/render thread, so GPU uploads never run on
// the worker. New requests with the same key invalidate stale work/results.
class AsyncAssetTaskQueue final {
public:
    using Completion = std::function<void()>;
    using Task = std::function<Completion()>;

    explicit AsyncAssetTaskQueue(std::size_t maximumOutstanding = 8U);
    ~AsyncAssetTaskQueue();

    AsyncAssetTaskQueue(const AsyncAssetTaskQueue&) = delete;
    AsyncAssetTaskQueue& operator=(const AsyncAssetTaskQueue&) = delete;

    bool submit(std::string key, Task task);
    void cancel(const std::string& key);
    void cancelAll();
    void poll(std::vector<Completion>& completions,
              std::size_t maximumCompletions = 8U);
    std::size_t outstandingCount() const;
    bool waitUntilIdle(std::chrono::milliseconds timeout);

private:
    struct Request {
        std::string key;
        std::uint64_t token = 0;
        Task task;
    };

    struct Completed {
        std::string key;
        std::uint64_t token = 0;
        Completion completion;
    };

    bool isCurrentLocked(const Request& request) const;
    void workerLoop();

    const std::size_t maximumOutstanding_;
    mutable std::mutex mutex_;
    std::condition_variable wakeCondition_;
    std::condition_variable idleCondition_;
    std::deque<Request> requests_;
    std::deque<Completed> completed_;
    std::unordered_map<std::string, std::uint64_t> activeTokens_;
    std::uint64_t nextToken_ = 1U;
    bool workerBusy_ = false;
    bool stopping_ = false;
    std::thread worker_;
};

}  // namespace nuby
