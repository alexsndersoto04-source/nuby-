#include "nuby/async_asset_task_queue.hpp"

#include <chrono>
#include <condition_variable>
#include <cstdlib>
#include <iostream>
#include <mutex>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

namespace {
int failures = 0;

void expect(bool condition, const char* message) {
    if (!condition) {
        std::cerr << "FAIL: " << message << '\n';
        ++failures;
    }
}

void testWorkRunsOffThreadAndCompletionRunsWhenPolled() {
    nuby::AsyncAssetTaskQueue queue;
    const std::thread::id callerThread = std::this_thread::get_id();
    std::thread::id workerThread;
    std::thread::id completionThread;
    bool completed = false;
    expect(queue.submit("mesh:a.obj", [&]() {
        workerThread = std::this_thread::get_id();
        return [&]() {
            completionThread = std::this_thread::get_id();
            completed = true;
        };
    }), "queue accepts CPU asset work");
    expect(queue.waitUntilIdle(std::chrono::seconds(2)),
           "worker finishes CPU work without blocking the caller");
    std::vector<nuby::AsyncAssetTaskQueue::Completion> completions;
    queue.poll(completions, 1U);
    for (auto& completion : completions) completion();
    expect(workerThread != std::thread::id{} && workerThread != callerThread,
           "CPU work executes on the dedicated worker thread");
    expect(completed && completionThread == callerThread,
           "completion executes only when polled by the render/caller thread");
    expect(queue.outstandingCount() == 0U,
           "polling a result releases its bounded request slot");
}

void testQueueIsBoundedAndCancellationDropsStaleResults() {
    nuby::AsyncAssetTaskQueue queue(3U);
    std::mutex mutex;
    std::condition_variable condition;
    bool workerStarted = false;
    bool releaseWorker = false;
    int staleCommits = 0;
    int freshCommits = 0;
    expect(queue.submit("same", [&]() {
        std::unique_lock<std::mutex> lock(mutex);
        workerStarted = true;
        condition.notify_all();
        condition.wait(lock, [&]() { return releaseWorker; });
        return [&]() { ++staleCommits; };
    }), "first keyed request is accepted");
    bool started = false;
    {
        std::unique_lock<std::mutex> lock(mutex);
        started = condition.wait_for(lock, std::chrono::seconds(2),
                [&]() { return workerStarted; });
    }
    expect(started, "worker starts the deliberately blocked request");
    if (!started) {
        {
            std::lock_guard<std::mutex> lock(mutex);
            releaseWorker = true;
        }
        condition.notify_all();
        return;
    }

    expect(queue.submit("same", [&]() {
        return [&]() { ++freshCommits; };
    }), "a newer request replaces work using the same asset key");
    expect(queue.submit("other-a", []() { return nuby::AsyncAssetTaskQueue::Completion{}; })
                   && queue.submit("other-b", []() {
                       return nuby::AsyncAssetTaskQueue::Completion{};
                   }), "queue accepts work up to its configured outstanding limit");
    expect(!queue.submit("overflow", []() {
        return nuby::AsyncAssetTaskQueue::Completion{};
    }), "queue rejects excess outstanding work instead of growing without bound");

    {
        std::lock_guard<std::mutex> lock(mutex);
        releaseWorker = true;
    }
    condition.notify_all();
    expect(queue.waitUntilIdle(std::chrono::seconds(2)),
           "worker drains the replaced request and queued work");
    std::vector<nuby::AsyncAssetTaskQueue::Completion> completions;
    queue.poll(completions, 8U);
    for (auto& completion : completions) completion();
    expect(staleCommits == 0 && freshCommits == 1,
           "only the latest completion for a key is published");

    expect(queue.submit("cancel-me", [&]() {
        return [&]() { ++staleCommits; };
    }), "queue accepts another request before cancellation");
    queue.cancel("cancel-me");
    expect(queue.waitUntilIdle(std::chrono::seconds(2)),
           "cancellation drains any work already started by the worker");
    queue.poll(completions, 8U);
    for (auto& completion : completions) completion();
    expect(staleCommits == 0 && queue.outstandingCount() == 0U,
           "cancelled results do not execute and do not leak request slots");
}

void testFailuresDoNotStopTheWorker() {
    nuby::AsyncAssetTaskQueue queue;
    int recovered = 0;
    expect(queue.submit("throws", []() -> nuby::AsyncAssetTaskQueue::Completion {
        throw std::runtime_error("decoder failure");
    }), "queue accepts a task that may fail");
    expect(queue.submit("recovers", [&]() {
        return [&]() { ++recovered; };
    }), "later work remains queueable after a task failure");
    expect(queue.waitUntilIdle(std::chrono::seconds(2)),
           "worker catches task exceptions and keeps processing");
    std::vector<nuby::AsyncAssetTaskQueue::Completion> completions;
    queue.poll(completions, 8U);
    for (auto& completion : completions) completion();
    expect(recovered == 1 && queue.outstandingCount() == 0U,
           "failed requests release capacity and subsequent work completes");
}
}  // namespace

int main() {
    testWorkRunsOffThreadAndCompletionRunsWhenPolled();
    testQueueIsBoundedAndCancellationDropsStaleResults();
    testFailuresDoNotStopTheWorker();
    if (failures != 0) {
        std::cerr << failures << " async asset task queue test(s) failed\n";
        return EXIT_FAILURE;
    }
    std::cout << "All asynchronous asset task queue tests passed\n";
    return EXIT_SUCCESS;
}
