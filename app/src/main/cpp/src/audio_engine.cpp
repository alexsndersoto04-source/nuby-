#include "nuby/audio_engine.hpp"

#include <algorithm>
#include <cmath>
#include <limits>
#include <utility>

namespace nuby {
namespace {
constexpr std::size_t kMaximumWavBytes = 16U * 1024U * 1024U;
constexpr std::size_t kMaximumDecodedSamples = 8U * 1024U * 1024U;
constexpr std::uint32_t kMinimumSampleRate = 8000U;
constexpr std::uint32_t kMaximumSampleRate = 96000U;
constexpr float kPi = 3.14159265358979323846f;

std::uint16_t readU16(const std::vector<std::uint8_t>& bytes, std::size_t offset) {
    return static_cast<std::uint16_t>(bytes[offset])
            | static_cast<std::uint16_t>(static_cast<std::uint16_t>(bytes[offset + 1U]) << 8U);
}

std::uint32_t readU32(const std::vector<std::uint8_t>& bytes, std::size_t offset) {
    return static_cast<std::uint32_t>(bytes[offset])
            | (static_cast<std::uint32_t>(bytes[offset + 1U]) << 8U)
            | (static_cast<std::uint32_t>(bytes[offset + 2U]) << 16U)
            | (static_cast<std::uint32_t>(bytes[offset + 3U]) << 24U);
}

bool fourCC(const std::vector<std::uint8_t>& bytes, std::size_t offset,
            const char* value) {
    return bytes[offset] == static_cast<std::uint8_t>(value[0])
            && bytes[offset + 1U] == static_cast<std::uint8_t>(value[1])
            && bytes[offset + 2U] == static_cast<std::uint8_t>(value[2])
            && bytes[offset + 3U] == static_cast<std::uint8_t>(value[3]);
}

float decodeSample(const std::uint8_t* bytes, std::uint16_t bitsPerSample) {
    if (bitsPerSample == 8U) {
        return (static_cast<float>(bytes[0]) - 128.0f) / 128.0f;
    }
    if (bitsPerSample == 16U) {
        const std::uint16_t encoded = static_cast<std::uint16_t>(bytes[0])
                | static_cast<std::uint16_t>(static_cast<std::uint16_t>(bytes[1]) << 8U);
        const std::int32_t value = encoded >= 0x8000U
                ? static_cast<std::int32_t>(encoded) - 65536
                : static_cast<std::int32_t>(encoded);
        return static_cast<float>(value) / 32768.0f;
    }
    if (bitsPerSample == 24U) {
        std::int32_t value = static_cast<std::int32_t>(bytes[0])
                | (static_cast<std::int32_t>(bytes[1]) << 8U)
                | (static_cast<std::int32_t>(bytes[2]) << 16U);
        if ((value & 0x00800000) != 0) value -= 0x01000000;
        return static_cast<float>(value) / 8388608.0f;
    }
    const std::uint32_t encoded = static_cast<std::uint32_t>(bytes[0])
            | (static_cast<std::uint32_t>(bytes[1]) << 8U)
            | (static_cast<std::uint32_t>(bytes[2]) << 16U)
            | (static_cast<std::uint32_t>(bytes[3]) << 24U);
    const std::int64_t value = encoded >= 0x80000000U
            ? static_cast<std::int64_t>(encoded) - 4294967296LL
            : static_cast<std::int64_t>(encoded);
    return static_cast<float>(static_cast<double>(value) / 2147483648.0);
}

std::int16_t toPcm16(float sample) {
    const float bounded = std::clamp(sample, -1.0f, 1.0f);
    const long quantized = std::lround(bounded * (bounded < 0.0f ? 32768.0f : 32767.0f));
    return static_cast<std::int16_t>(std::clamp(quantized, -32768L, 32767L));
}

}  // namespace

bool AudioClip::valid() const {
    if (sampleRate < kMinimumSampleRate || sampleRate > kMaximumSampleRate
            || (channels != 1U && channels != 2U) || samples.empty()
            || samples.size() % channels != 0U || samples.size() > kMaximumDecodedSamples) {
        return false;
    }
    return std::all_of(samples.begin(), samples.end(), [](float value) {
        return std::isfinite(value) && value >= -1.0f && value <= 1.0f;
    });
}

bool AudioEngine::decodeWav(const std::vector<std::uint8_t>& bytes,
                            std::shared_ptr<AudioClip>& output, std::string& error) {
    if (bytes.size() < 12U || bytes.size() > kMaximumWavBytes
            || !fourCC(bytes, 0U, "RIFF") || !fourCC(bytes, 8U, "WAVE")) {
        error = "WAV is truncated, oversized, or missing RIFF/WAVE signatures";
        return false;
    }
    const std::uint64_t declaredEnd64 = static_cast<std::uint64_t>(readU32(bytes, 4U)) + 8U;
    if (declaredEnd64 < 12U || declaredEnd64 > bytes.size()) {
        error = "WAV RIFF length exceeds the provided data";
        return false;
    }
    const std::size_t declaredEnd = static_cast<std::size_t>(declaredEnd64);
    bool haveFormat = false;
    bool haveData = false;
    std::uint16_t format = 0U;
    std::uint16_t channels = 0U;
    std::uint16_t bitsPerSample = 0U;
    std::uint16_t blockAlign = 0U;
    std::uint32_t sampleRate = 0U;
    std::size_t dataOffset = 0U;
    std::size_t dataLength = 0U;

    std::size_t offset = 12U;
    while (offset < declaredEnd) {
        if (declaredEnd - offset < 8U) {
            error = "WAV chunk header is truncated";
            return false;
        }
        const std::uint32_t chunkLength32 = readU32(bytes, offset + 4U);
        const std::size_t chunkLength = static_cast<std::size_t>(chunkLength32);
        const std::size_t chunkData = offset + 8U;
        if (chunkLength > declaredEnd - chunkData) {
            error = "WAV chunk exceeds RIFF bounds";
            return false;
        }
        if (fourCC(bytes, offset, "fmt ")) {
            if (haveFormat || chunkLength < 16U) {
                error = "WAV format chunk is duplicate or shorter than PCM metadata";
                return false;
            }
            format = readU16(bytes, chunkData);
            channels = readU16(bytes, chunkData + 2U);
            sampleRate = readU32(bytes, chunkData + 4U);
            const std::uint32_t byteRate = readU32(bytes, chunkData + 8U);
            blockAlign = readU16(bytes, chunkData + 12U);
            bitsPerSample = readU16(bytes, chunkData + 14U);
            if (format != 1U || (channels != 1U && channels != 2U)
                    || sampleRate < kMinimumSampleRate || sampleRate > kMaximumSampleRate
                    || (bitsPerSample != 8U && bitsPerSample != 16U
                        && bitsPerSample != 24U && bitsPerSample != 32U)) {
                error = "WAV must use mono/stereo integer PCM at a supported sample rate";
                return false;
            }
            const std::uint32_t expectedAlign = static_cast<std::uint32_t>(channels)
                    * static_cast<std::uint32_t>(bitsPerSample / 8U);
            const std::uint64_t expectedByteRate = static_cast<std::uint64_t>(sampleRate)
                    * static_cast<std::uint64_t>(expectedAlign);
            if (blockAlign != expectedAlign || expectedByteRate != byteRate
                    || expectedByteRate > std::numeric_limits<std::uint32_t>::max()) {
                error = "WAV byte rate or block alignment is inconsistent";
                return false;
            }
            haveFormat = true;
        } else if (fourCC(bytes, offset, "data")) {
            if (haveData) {
                error = "WAV contains multiple data chunks";
                return false;
            }
            dataOffset = chunkData;
            dataLength = chunkLength;
            haveData = true;
        }
        const std::size_t paddedLength = chunkLength + (chunkLength & 1U);
        if (paddedLength > declaredEnd - chunkData) {
            error = "WAV chunk padding exceeds RIFF bounds";
            return false;
        }
        offset = chunkData + paddedLength;
    }
    if (!haveFormat || !haveData || dataLength == 0U || dataLength % blockAlign != 0U) {
        error = "WAV is missing PCM format/data or has a partial sample frame";
        return false;
    }
    const std::size_t sampleCount = dataLength / (bitsPerSample / 8U);
    if (sampleCount > kMaximumDecodedSamples) {
        error = "WAV decoded sample count exceeds the configured safety limit";
        return false;
    }
    auto decoded = std::make_shared<AudioClip>();
    decoded->sampleRate = sampleRate;
    decoded->channels = channels;
    decoded->samples.resize(sampleCount);
    const std::size_t bytesPerSample = bitsPerSample / 8U;
    for (std::size_t sample = 0U; sample < sampleCount; ++sample) {
        decoded->samples[sample] = decodeSample(
                bytes.data() + dataOffset + sample * bytesPerSample, bitsPerSample);
    }
    if (!decoded->valid()) {
        error = "WAV decoded to invalid PCM sample data";
        return false;
    }
    output = std::move(decoded);
    error.clear();
    return true;
}

bool AudioEngine::validOptions(const AudioPlayOptions& options) {
    if (!std::isfinite(options.gain) || options.gain < 0.0f || options.gain > 4.0f
            || !std::isfinite(options.referenceDistance) || options.referenceDistance <= 0.0f
            || !std::isfinite(options.maximumDistance)
            || options.maximumDistance < options.referenceDistance
            || !std::isfinite(options.rolloff) || options.rolloff < 0.0f
            || static_cast<std::uint8_t>(options.bus)
                    > static_cast<std::uint8_t>(AudioBus::Music)) {
        return false;
    }
    return std::all_of(options.position.begin(), options.position.end(), [](float value) {
        return std::isfinite(value);
    });
}

AudioVoiceHandle AudioEngine::play(std::shared_ptr<const AudioClip> clip,
                                   const AudioPlayOptions& options) {
    if (!clip || !clip->valid() || !validOptions(options)) return {};
    std::lock_guard<std::mutex> lock(mutex_);
    std::size_t selected = voices_.size();
    for (std::size_t index = 0U; index < voices_.size(); ++index) {
        if (!voices_[index].active) {
            selected = index;
            break;
        }
    }
    if (selected == voices_.size()) {
        selected = 0U;
        for (std::size_t index = 1U; index < voices_.size(); ++index) {
            if (voices_[index].options.priority < voices_[selected].options.priority
                    || (voices_[index].options.priority == voices_[selected].options.priority
                        && voices_[index].sequence < voices_[selected].sequence)) {
                selected = index;
            }
        }
        if (options.priority < voices_[selected].options.priority) return {};
    }
    Voice& voice = voices_[selected];
    ++voice.generation;
    if (voice.generation == 0U) ++voice.generation;
    voice.clip = std::move(clip);
    voice.options = options;
    voice.cursor = 0.0;
    voice.fadeGain = 1.0f;
    voice.fadeTarget = 1.0f;
    voice.fadeFramesRemaining = 0U;
    voice.sequence = nextSequence_++;
    if (nextSequence_ == 0U) nextSequence_ = 1U;
    voice.active = true;
    voice.paused = false;
    return {static_cast<std::uint32_t>(selected), voice.generation};
}

AudioEngine::Voice* AudioEngine::findVoice(AudioVoiceHandle handle) {
    if (!handle.valid() || handle.slot >= voices_.size()) return nullptr;
    Voice& voice = voices_[handle.slot];
    return voice.active && voice.generation == handle.generation ? &voice : nullptr;
}

const AudioEngine::Voice* AudioEngine::findVoice(AudioVoiceHandle handle) const {
    if (!handle.valid() || handle.slot >= voices_.size()) return nullptr;
    const Voice& voice = voices_[handle.slot];
    return voice.active && voice.generation == handle.generation ? &voice : nullptr;
}

bool AudioEngine::stop(AudioVoiceHandle handle, float fadeSeconds) {
    if (!std::isfinite(fadeSeconds) || fadeSeconds < 0.0f || fadeSeconds > 60.0f) return false;
    std::lock_guard<std::mutex> lock(mutex_);
    Voice* voice = findVoice(handle);
    if (voice == nullptr) return false;
    if (fadeSeconds == 0.0f) {
        voice->active = false;
        voice->clip.reset();
        ++voice->generation;
        return true;
    }
    voice->fadeTarget = 0.0f;
    const double frames = static_cast<double>(fadeSeconds) * kOutputSampleRate;
    voice->fadeFramesRemaining = static_cast<std::size_t>(std::max(1.0, std::ceil(frames)));
    return true;
}

bool AudioEngine::pause(AudioVoiceHandle handle, bool paused) {
    std::lock_guard<std::mutex> lock(mutex_);
    Voice* voice = findVoice(handle);
    if (voice == nullptr) return false;
    voice->paused = paused;
    return true;
}

void AudioEngine::pauseAll(bool paused) {
    std::lock_guard<std::mutex> lock(mutex_);
    globallyPaused_ = paused;
}

void AudioEngine::stopAll(float fadeSeconds) {
    if (!std::isfinite(fadeSeconds) || fadeSeconds < 0.0f || fadeSeconds > 60.0f) return;
    std::lock_guard<std::mutex> lock(mutex_);
    if (fadeSeconds == 0.0f) {
        for (Voice& voice : voices_) {
            if (voice.active) {
                voice.active = false;
                voice.clip.reset();
                ++voice.generation;
            }
        }
        return;
    }
    const double frames = static_cast<double>(fadeSeconds) * kOutputSampleRate;
    const std::size_t fadeFrames = static_cast<std::size_t>(std::max(1.0, std::ceil(frames)));
    for (Voice& voice : voices_) {
        if (voice.active) {
            voice.fadeTarget = 0.0f;
            voice.fadeFramesRemaining = fadeFrames;
        }
    }
}

void AudioEngine::setListener(const AudioListener& listener) {
    if (!std::all_of(listener.position.begin(), listener.position.end(), [](float value) {
                return std::isfinite(value);
            }) || !std::all_of(listener.right.begin(), listener.right.end(), [](float value) {
                return std::isfinite(value);
            })) return;
    std::lock_guard<std::mutex> lock(mutex_);
    listener_ = listener;
}

void AudioEngine::setBusGain(AudioBus bus, float gain) {
    if (!std::isfinite(gain) || gain < 0.0f || gain > 4.0f) return;
    std::lock_guard<std::mutex> lock(mutex_);
    if (bus == AudioBus::Effects) effectsGain_ = gain;
    else if (bus == AudioBus::Music) musicGain_ = gain;
}

void AudioEngine::setMasterGain(float gain) {
    if (!std::isfinite(gain) || gain < 0.0f || gain > 4.0f) return;
    std::lock_guard<std::mutex> lock(mutex_);
    masterGain_ = gain;
}

void AudioEngine::renderPcm16(std::int16_t* output, std::size_t frameCount) {
    if (output == nullptr || frameCount > std::numeric_limits<std::size_t>::max() / 2U) return;
    std::lock_guard<std::mutex> lock(mutex_);
    if (globallyPaused_) {
        std::fill(output, output + frameCount * 2U, 0);
        return;
    }
    std::array<float, kMaximumVoices> leftGains{};
    std::array<float, kMaximumVoices> rightGains{};
    for (std::size_t voiceIndex = 0U; voiceIndex < voices_.size(); ++voiceIndex) {
        const Voice& voice = voices_[voiceIndex];
        if (!voice.active || voice.paused || !voice.clip) continue;
        float gain = voice.options.gain * masterGain_
                * (voice.options.bus == AudioBus::Effects ? effectsGain_ : musicGain_);
        float leftPan = 1.0f;
        float rightPan = 1.0f;
        if (voice.options.positional) {
            std::array<float, 3> relative{};
            float distanceSquared = 0.0f;
            for (std::size_t axis = 0U; axis < relative.size(); ++axis) {
                relative[axis] = voice.options.position[axis] - listener_.position[axis];
                distanceSquared += relative[axis] * relative[axis];
            }
            const float distance = std::sqrt(std::max(distanceSquared, 0.0f));
            if (distance >= voice.options.maximumDistance) {
                gain = 0.0f;
            } else {
                const float refSquared = voice.options.referenceDistance
                        * voice.options.referenceDistance;
                gain *= refSquared / (refSquared + voice.options.rolloff
                        * std::max(0.0f, distanceSquared - refSquared));
            }
            const float rightLengthSquared = listener_.right[0] * listener_.right[0]
                    + listener_.right[1] * listener_.right[1]
                    + listener_.right[2] * listener_.right[2];
            if (distance > 1.0e-5f && rightLengthSquared > 1.0e-8f) {
                const float invRightLength = 1.0f / std::sqrt(rightLengthSquared);
                const float pan = std::clamp((relative[0] * listener_.right[0]
                        + relative[1] * listener_.right[1]
                        + relative[2] * listener_.right[2])
                        / (distance / invRightLength), -1.0f, 1.0f);
                const float angle = (pan + 1.0f) * (kPi * 0.25f);
                leftPan = std::cos(angle);
                rightPan = std::sin(angle);
            }
        }
        leftGains[voiceIndex] = gain * leftPan;
        rightGains[voiceIndex] = gain * rightPan;
    }

    for (std::size_t frame = 0U; frame < frameCount; ++frame) {
        float left = 0.0f;
        float right = 0.0f;
        for (std::size_t voiceIndex = 0U; voiceIndex < voices_.size(); ++voiceIndex) {
            Voice& voice = voices_[voiceIndex];
            if (!voice.active || voice.paused || !voice.clip) continue;
            const std::size_t sourceFrames = voice.clip->frameCount();
            if (sourceFrames == 0U) {
                voice.active = false;
                voice.clip.reset();
                ++voice.generation;
                continue;
            }
            if (voice.cursor >= static_cast<double>(sourceFrames)) {
                if (voice.options.looping) {
                    voice.cursor = std::fmod(voice.cursor, static_cast<double>(sourceFrames));
                } else {
                    voice.active = false;
                    voice.clip.reset();
                    ++voice.generation;
                    continue;
                }
            }
            const std::size_t firstFrame = static_cast<std::size_t>(voice.cursor);
            const std::size_t nextFrame = firstFrame + 1U < sourceFrames
                    ? firstFrame + 1U : (voice.options.looping ? 0U : firstFrame);
            const float fraction = static_cast<float>(voice.cursor
                    - static_cast<double>(firstFrame));
            const std::size_t channels = voice.clip->channels;
            const float* samples = voice.clip->samples.data();
            float sampleLeft = 0.0f;
            float sampleRight = 0.0f;
            if (channels == 1U) {
                const float first = samples[firstFrame];
                const float second = samples[nextFrame];
                sampleLeft = first + (second - first) * fraction;
                sampleRight = sampleLeft;
            } else {
                const float l0 = samples[firstFrame * 2U];
                const float l1 = samples[nextFrame * 2U];
                const float r0 = samples[firstFrame * 2U + 1U];
                const float r1 = samples[nextFrame * 2U + 1U];
                sampleLeft = l0 + (l1 - l0) * fraction;
                sampleRight = r0 + (r1 - r0) * fraction;
            }
            const float fade = voice.fadeGain;
            left += sampleLeft * leftGains[voiceIndex] * fade;
            right += sampleRight * rightGains[voiceIndex] * fade;
            if (voice.fadeFramesRemaining > 0U) {
                voice.fadeGain += (voice.fadeTarget - voice.fadeGain)
                        / static_cast<float>(voice.fadeFramesRemaining);
                --voice.fadeFramesRemaining;
                if (voice.fadeFramesRemaining == 0U) {
                    voice.fadeGain = voice.fadeTarget;
                    if (voice.fadeTarget <= 0.0f) {
                        voice.active = false;
                        voice.clip.reset();
                        ++voice.generation;
                        continue;
                    }
                }
            }
            voice.cursor += static_cast<double>(voice.clip->sampleRate)
                    / static_cast<double>(kOutputSampleRate);
            if (!voice.options.looping
                    && voice.cursor >= static_cast<double>(sourceFrames)) {
                voice.active = false;
                voice.clip.reset();
                ++voice.generation;
            }
        }
        output[frame * 2U] = toPcm16(left);
        output[frame * 2U + 1U] = toPcm16(right);
    }
}

std::size_t AudioEngine::activeVoiceCount() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return static_cast<std::size_t>(std::count_if(voices_.begin(), voices_.end(),
            [](const Voice& voice) { return voice.active; }));
}

}  // namespace nuby
