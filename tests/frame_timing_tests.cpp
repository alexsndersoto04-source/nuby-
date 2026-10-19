#include "nuby/frame_timing.hpp"

#include <cmath>
#include <cstdlib>
#include <iostream>
#include <limits>
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

void testRollingPercentilesAndWindowEviction() {
    nuby::FrameTimingWindow window;
    window.record(std::numeric_limits<double>::quiet_NaN());
    window.record(-1.0);
    expect(window.snapshot().sampleCount == 0U,
           "non-finite and negative frame durations are ignored");
    for (int sample = 1; sample <= 10; ++sample) window.record(sample);
    const nuby::FrameTimingStats initial = window.snapshot();
    expect(initial.sampleCount == 10U && std::fabs(initial.averageMilliseconds - 5.5) < 1e-9
                   && initial.p95Milliseconds == 10.0 && initial.maximumMilliseconds == 10.0,
           "rolling telemetry calculates average, nearest-rank p95 and maximum");
    for (std::size_t sample = 0; sample < nuby::FrameTimingWindow::kCapacity; ++sample) {
        window.record(20.0);
    }
    const nuby::FrameTimingStats rolled = window.snapshot();
    expect(rolled.sampleCount == nuby::FrameTimingWindow::kCapacity
                   && rolled.maximumMilliseconds == 20.0
                   && rolled.p95Milliseconds == 20.0,
           "fixed rolling window evicts old frames and bounds its memory");
}

void testConcurrentFrameRecordingAndSnapshots() {
    nuby::FrameTimingWindow window;
    std::thread writer([&window]() {
        for (int sample = 0; sample < 20000; ++sample) {
            window.record(static_cast<double>((sample % 25) + 1));
        }
    });
    for (int snapshot = 0; snapshot < 100; ++snapshot) {
        const nuby::FrameTimingStats stats = window.snapshot();
        expect(stats.sampleCount <= nuby::FrameTimingWindow::kCapacity
                       && std::isfinite(stats.averageMilliseconds)
                       && std::isfinite(stats.p95Milliseconds),
               "UI-thread snapshots stay coherent while the GL thread records frames");
    }
    writer.join();
    expect(window.snapshot().sampleCount == nuby::FrameTimingWindow::kCapacity,
           "concurrent frame samples fill but never exceed the fixed-capacity window");
}
}  // namespace

int main() {
    testRollingPercentilesAndWindowEviction();
    testConcurrentFrameRecordingAndSnapshots();
    if (failures != 0) {
        std::cerr << failures << " frame timing test(s) failed\n";
        return EXIT_FAILURE;
    }
    std::cout << "All frame timing tests passed\n";
    return EXIT_SUCCESS;
}
