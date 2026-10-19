#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

namespace nuby {

enum class AudioBus : std::uint8_t { Effects, Music };

struct AudioClip {
    std::uint32_t sampleRate = 0U;
    std::uint16_t channels = 0U;
    std::vector<float> samples;

    std::size_t frameCount() const {
        return channels == 0U ? 0U : samples.size() / channels;
    }
    bool valid() const;
};

struct AudioVoiceHandle {
    std::uint32_t slot = 0xFFFFFFFFU;
    std::uint32_t generation = 0U;
    bool valid() const { return slot != 0xFFFFFFFFU; }
};

struct AudioPlayOptions {
    AudioBus bus = AudioBus::Effects;
    float gain = 1.0f;
    std::uint8_t priority = 64U;
    bool looping = false;
    bool positional = false;
    std::array<float, 3> position{0.0f, 0.0f, 0.0f};
    float referenceDistance = 1.0f;
    float maximumDistance = 24.0f;
    float rolloff = 1.0f;
};

struct AudioListener {
    std::array<float, 3> position{0.0f, 0.0f, 0.0f};
    std::array<float, 3> right{1.0f, 0.0f, 0.0f};
};

// Small real-time mixer: commands are serialized with the render callback,
// voices resample linearly to the device rate, and output is interleaved PCM16.
class AudioEngine final {
public:
    static constexpr std::size_t kMaximumVoices = 24U;
    static constexpr std::uint32_t kOutputSampleRate = 44100U;

    static bool decodeWav(const std::vector<std::uint8_t>& bytes,
                          std::shared_ptr<AudioClip>& output, std::string& error);

    AudioVoiceHandle play(std::shared_ptr<const AudioClip> clip,
                          const AudioPlayOptions& options = {});
    bool stop(AudioVoiceHandle handle, float fadeSeconds = 0.0f);
    bool pause(AudioVoiceHandle handle, bool paused);
    bool setVoicePosition(AudioVoiceHandle handle, const std::array<float, 3>& position);
    bool isVoiceActive(AudioVoiceHandle handle) const;
    void pauseAll(bool paused);
    void stopAll(float fadeSeconds = 0.0f);
    void setListener(const AudioListener& listener);
    void setBusGain(AudioBus bus, float gain);
    void setMasterGain(float gain);
    void renderPcm16(std::int16_t* interleavedStereo, std::size_t frameCount);
    std::size_t activeVoiceCount() const;

private:
    struct Voice {
        std::shared_ptr<const AudioClip> clip;
        AudioPlayOptions options;
        double cursor = 0.0;
        float fadeGain = 1.0f;
        float fadeTarget = 1.0f;
        std::size_t fadeFramesRemaining = 0U;
        std::uint32_t generation = 1U;
        std::uint64_t sequence = 0U;
        bool active = false;
        bool paused = false;
    };

    static bool validOptions(const AudioPlayOptions& options);
    Voice* findVoice(AudioVoiceHandle handle);
    const Voice* findVoice(AudioVoiceHandle handle) const;

    mutable std::mutex mutex_;
    std::array<Voice, kMaximumVoices> voices_{};
    AudioListener listener_;
    float effectsGain_ = 1.0f;
    float musicGain_ = 1.0f;
    float masterGain_ = 1.0f;
    std::uint64_t nextSequence_ = 1U;
    bool globallyPaused_ = false;
};

}  // namespace nuby
