#pragma once

#include <array>
#include <cstddef>
#include <mutex>

namespace nuby {

struct FrameTimingStats {
    double averageMilliseconds = 0.0;
    double p95Milliseconds = 0.0;
    double maximumMilliseconds = 0.0;
    std::size_t sampleCount = 0U;
};

// Small thread-safe rolling window for renderer CPU frame costs. It is read
// from the Android UI thread while samples are recorded on the GL thread.
class FrameTimingWindow final {
public:
    static constexpr std::size_t kCapacity = 120U;

    void record(double milliseconds);
    FrameTimingStats snapshot() const;

private:
    mutable std::mutex mutex_;
    std::array<double, kCapacity> samples_{};
    std::size_t next_ = 0U;
    std::size_t count_ = 0U;
};

}  // namespace nuby
