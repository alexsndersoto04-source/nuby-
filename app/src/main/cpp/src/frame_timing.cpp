#include "nuby/frame_timing.hpp"

#include <algorithm>
#include <cmath>

namespace nuby {

void FrameTimingWindow::record(double milliseconds) {
    if (!std::isfinite(milliseconds) || milliseconds < 0.0) return;
    std::lock_guard<std::mutex> lock(mutex_);
    samples_[next_] = milliseconds;
    next_ = (next_ + 1U) % samples_.size();
    count_ = std::min(count_ + 1U, samples_.size());
}

FrameTimingStats FrameTimingWindow::snapshot() const {
    FrameTimingStats result;
    std::array<double, kCapacity> ordered{};
    std::size_t sampleCount = 0U;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        sampleCount = count_;
        if (sampleCount == 0U) return result;
        const std::size_t first = sampleCount == samples_.size() ? next_ : 0U;
        for (std::size_t index = 0U; index < sampleCount; ++index) {
            ordered[index] = samples_[(first + index) % samples_.size()];
        }
    }
    result.sampleCount = sampleCount;
    result.averageMilliseconds = 0.0;
    for (std::size_t index = 0U; index < sampleCount; ++index) {
        result.averageMilliseconds += ordered[index];
    }
    result.averageMilliseconds /= static_cast<double>(sampleCount);
    std::sort(ordered.begin(), ordered.begin() + static_cast<std::ptrdiff_t>(sampleCount));
    const std::size_t p95Index = (95U * sampleCount + 99U) / 100U - 1U;
    result.p95Milliseconds = ordered[p95Index];
    result.maximumMilliseconds = ordered[sampleCount - 1U];
    return result;
}

}  // namespace nuby
