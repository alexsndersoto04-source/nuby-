#include "nuby/async_scene_asset_loader.hpp"

#include <chrono>
#include <condition_variable>
#include <cstdlib>
#include <iostream>
#include <limits>
#include <mutex>
#include <stdexcept>
#include <string>

namespace {
int failures = 0;

void expect(bool condition, const char* message) {
    if (!condition) {
        std::cerr << "FAIL: " << message << '\n';
        ++failures;
    }
}

void testCancelledLoadsCannotPublishStaleScenes() {
    std::mutex mutex;
    std::condition_variable condition;
    bool slowReadStarted = false;
    bool releaseSlowRead = false;
    nuby::AsyncSceneAssetLoader loader(
            [&mutex, &condition, &slowReadStarted, &releaseSlowRead](
                    const std::string& path, nuby::Scene& scene, std::string&) {
                if (path == "slow.scene") {
                    std::unique_lock<std::mutex> lock(mutex);
                    slowReadStarted = true;
                    condition.notify_all();
                    condition.wait(lock, [&releaseSlowRead]() { return releaseSlowRead; });
                    scene.create("stale scene", nuby::Primitive::Sphere);
                    return true;
                }
                scene.create("fresh scene", nuby::Primitive::Sphere);
                return true;
            });

    expect(loader.request(4, "slow.scene"), "loader accepts a valid async request");
    bool started = false;
    {
        std::unique_lock<std::mutex> lock(mutex);
        started = condition.wait_for(lock, std::chrono::seconds(2),
                [&slowReadStarted]() { return slowReadStarted; });
    }
    expect(started, "worker begins scene IO away from the requesting thread");
    if (!started) {
        {
            std::lock_guard<std::mutex> lock(mutex);
            releaseSlowRead = true;
        }
        condition.notify_all();
        return;
    }

    loader.cancel(4);
    expect(loader.request(4, "fresh.scene"),
           "a cancelled zone can be requested again with a new generation token");
    for (std::uint32_t id = 5; id <= 11; ++id) {
        expect(loader.request(id, "queued.scene"),
               "bounded worker accepts requests up to its configured capacity");
    }
    expect(!loader.request(12, "overflow.scene"),
           "bounded worker rejects excess outstanding work rather than growing without limit");
    for (std::uint32_t id = 5; id <= 11; ++id) loader.cancel(id);
    {
        std::lock_guard<std::mutex> lock(mutex);
        releaseSlowRead = true;
    }
    condition.notify_all();

    expect(loader.waitUntilIdle(std::chrono::seconds(2)),
           "worker drains the fresh request after the stale read finishes");
    std::vector<nuby::SceneAssetLoadResult> results;
    loader.poll(results);
    expect(results.size() == 1 && results[0].requestId == 4
                   && results[0].assetPath == "fresh.scene" && results[0].success
                   && results[0].scene.entities().size() == 1
                   && results[0].scene.entities()[0].name == "fresh scene",
           "only the newest uncancelled scene result is made visible to the caller");
    expect(loader.outstandingCount() == 0,
           "polling a completed result releases its outstanding-request slot");
}

void testQueuedSceneReadsAreStablePriorityOrdered() {
    std::mutex mutex;
    std::condition_variable condition;
    bool blockerStarted = false;
    bool releaseBlocker = false;
    nuby::AsyncSceneAssetLoader loader(
            [&mutex, &condition, &blockerStarted, &releaseBlocker](
                    const std::string& path, nuby::Scene& scene, std::string&) {
                if (path == "block.scene") {
                    std::unique_lock<std::mutex> lock(mutex);
                    blockerStarted = true;
                    condition.notify_all();
                    condition.wait(lock, [&releaseBlocker]() { return releaseBlocker; });
                }
                scene.create(path, nuby::Primitive::Sphere);
                return true;
            });
    expect(loader.request(1U, "block.scene"),
           "priority test occupies the worker before queuing scene requests");
    bool started = false;
    {
        std::unique_lock<std::mutex> lock(mutex);
        started = condition.wait_for(lock, std::chrono::seconds(2),
                [&blockerStarted]() { return blockerStarted; });
    }
    expect(started, "priority test blocker reaches the reader");
    if (!started) {
        {
            std::lock_guard<std::mutex> lock(mutex);
            releaseBlocker = true;
        }
        condition.notify_all();
        return;
    }

    expect(loader.request(2U, "low.scene", 1.0)
                   && loader.request(3U, "high-a.scene", 10.0)
                   && loader.request(4U, "high-b.scene", 10.0),
           "loader accepts queued work with explicit priorities");
    expect(!loader.request(5U, "invalid-priority.scene",
                          std::numeric_limits<double>::infinity()),
           "non-finite scheduling priorities are rejected");
    {
        std::lock_guard<std::mutex> lock(mutex);
        releaseBlocker = true;
    }
    condition.notify_all();

    expect(loader.waitUntilIdle(std::chrono::seconds(2)),
           "priority queue drains after the active read completes");
    std::vector<nuby::SceneAssetLoadResult> results;
    loader.poll(results);
    expect(results.size() == 4U && results[0].requestId == 1U
                   && results[1].requestId == 3U && results[2].requestId == 4U
                   && results[3].requestId == 2U,
           "higher-priority queued reads run first while equal priority remains FIFO");
}

void testReaderFailuresAndExceptionsAreReportedWithoutKillingWorker() {
    nuby::AsyncSceneAssetLoader loader(
            [](const std::string& path, nuby::Scene& scene, std::string& error) {
                if (path == "throw.scene") throw std::runtime_error("decoder fault");
                if (path == "bad.scene") {
                    error = "malformed scene";
                    return false;
                }
                scene.create("recovered", nuby::Primitive::Sphere);
                return true;
            });
    expect(!loader.request(0, "zero.scene") && !loader.request(1, "")
                   && !loader.request(2, std::string(1025, 'x')),
           "invalid IDs, empty paths and oversized paths are rejected");

    expect(loader.request(1, "bad.scene") && loader.request(2, "throw.scene")
                   && loader.request(3, "good.scene"),
           "valid work remains queueable after invalid requests are rejected");
    expect(loader.waitUntilIdle(std::chrono::seconds(2)),
           "worker completes failure, exception and recovery cases");
    std::vector<nuby::SceneAssetLoadResult> results;
    loader.poll(results);
    expect(results.size() == 3 && !results[0].success
                   && results[0].error == "malformed scene"
                   && !results[1].success
                   && results[1].error.find("decoder fault") != std::string::npos
                   && results[2].success && results[2].scene.entities().size() == 1,
           "load failures and thrown exceptions are reported while later requests continue");

    expect(loader.request(8, "cancelled.scene"), "another request can be queued");
    loader.cancelAll();
    expect(loader.outstandingCount() == 0,
           "cancelAll invalidates both queued requests and unpolled results");
}
}  // namespace

int main() {
    testCancelledLoadsCannotPublishStaleScenes();
    testQueuedSceneReadsAreStablePriorityOrdered();
    testReaderFailuresAndExceptionsAreReportedWithoutKillingWorker();
    if (failures != 0) {
        std::cerr << failures << " async scene asset loader test(s) failed\n";
        return EXIT_FAILURE;
    }
    std::cout << "All asynchronous scene asset loader tests passed\n";
    return EXIT_SUCCESS;
}
