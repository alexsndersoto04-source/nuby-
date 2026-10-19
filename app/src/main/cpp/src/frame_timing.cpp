#include "nuby/frame_timing.hpp"

#include <algorithm>
#include <cmath>
#include <vector>

namespace nuby {

void FrameTimingWindow::record(double milliseconds) {
    if (!std::isfinite(milliseconds) || milliseconds < 0.0) return;
    std::lock_guard<std::mutex> lock(mutex_);
    samples_[next_] = milliseconds;
    next_ = (next_ + 1U) % samples_.size();
    count_ = std::min(count_ + 1U, samples_.size());
}

FrameTimingStats FrameTimingWindow::snapshot() const {
    std::lock_guard<std::mutex> lock(mutex_);
    FrameTimingStats result;
    result.sampleCount = count_;
    if (count_ == 0U) return result;

    std::vector<double> ordered;
    ordered.reserve(count_);
    const std::size_t first = count_ == samples_.size() ? next_ : 0U;
    for (std::size_t index = 0U; index < count_; ++index) {
        ordered.push_back(samples_[(first + index) % samples_.size()]);
    }
    result.averageMilliseconds = 0.0;
    for (double sample : ordered) result.averageMilliseconds += sample;
    result.averageMilliseconds /= static_cast<double>(ordered.size());
    std::sort(ordered.begin(), ordered.end());
    const std::size_t p95Index = (95U * ordered.size() + 99U) / 100U - 1U;
    result.p95Milliseconds = ordered[p95Index];
    result.maximumMilliseconds = ordered.back();
    return result;
}

}  // namespace nuby
