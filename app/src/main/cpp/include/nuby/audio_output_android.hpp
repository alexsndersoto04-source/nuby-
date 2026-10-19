#pragma once

#include "nuby/audio_engine.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <mutex>

#include <SLES/OpenSLES.h>
#include <SLES/OpenSLES_Android.h>
#include <SLES/OpenSLES_Platform.h>

namespace nuby {

// OpenSL ES PCM sink used by Android. The platform thread only pulls blocks from
// AudioEngine; clips, voices and mixing policy remain portable and unit-tested.
class AndroidAudioOutput final {
public:
    AndroidAudioOutput() = default;
    ~AndroidAudioOutput();
    AndroidAudioOutput(const AndroidAudioOutput&) = delete;
    AndroidAudioOutput& operator=(const AndroidAudioOutput&) = delete;

    bool start(AudioEngine& engine);
    void stop();
    void pause();
    void resume();
    bool running() const;

private:
    void stopLocked();
    static void onBufferQueue(SLAndroidSimpleBufferQueueItf queue, void* context);
    void refill(SLAndroidSimpleBufferQueueItf queue);

    static constexpr std::size_t kFramesPerBuffer = 512U;
    using PcmBuffer = std::array<std::int16_t, kFramesPerBuffer * 2U>;

    mutable std::mutex lifecycleMutex_;
    AudioEngine* engine_ = nullptr;
    SLObjectItf engineObject_ = nullptr;
    SLEngineItf slEngine_ = nullptr;
    SLObjectItf outputMixObject_ = nullptr;
    SLObjectItf playerObject_ = nullptr;
    SLPlayItf playInterface_ = nullptr;
    SLAndroidSimpleBufferQueueItf bufferQueue_ = nullptr;
    std::array<PcmBuffer, 2U> buffers_{};
    std::size_t nextBuffer_ = 0U;
};

}  // namespace nuby
