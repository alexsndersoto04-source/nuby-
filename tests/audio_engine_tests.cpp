#include "nuby/audio_engine.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <fstream>
#include <iostream>
#include <limits>
#include <memory>
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

void appendU16(std::vector<std::uint8_t>& bytes, std::uint16_t value) {
    bytes.push_back(static_cast<std::uint8_t>(value & 0xFFU));
    bytes.push_back(static_cast<std::uint8_t>((value >> 8U) & 0xFFU));
}

void appendU32(std::vector<std::uint8_t>& bytes, std::uint32_t value) {
    for (unsigned int shift = 0U; shift < 32U; shift += 8U) {
        bytes.push_back(static_cast<std::uint8_t>((value >> shift) & 0xFFU));
    }
}

void appendChunk(std::vector<std::uint8_t>& bytes, const char* tag,
                 const std::vector<std::uint8_t>& payload) {
    for (int index = 0; index < 4; ++index) bytes.push_back(static_cast<std::uint8_t>(tag[index]));
    appendU32(bytes, static_cast<std::uint32_t>(payload.size()));
    bytes.insert(bytes.end(), payload.begin(), payload.end());
    if ((payload.size() & 1U) != 0U) bytes.push_back(0U);
}

std::vector<std::uint8_t> makeWav(std::uint16_t channels, std::uint16_t bits,
                                  const std::vector<std::uint8_t>& pcm,
                                  bool dataBeforeFormat = false) {
    const std::uint16_t block = static_cast<std::uint16_t>(channels * (bits / 8U));
    std::vector<std::uint8_t> format;
    appendU16(format, 1U);
    appendU16(format, channels);
    appendU32(format, 44100U);
    appendU32(format, 44100U * block);
    appendU16(format, block);
    appendU16(format, bits);
    std::vector<std::uint8_t> body;
    if (dataBeforeFormat) {
        appendChunk(body, "data", pcm);
        appendChunk(body, "fmt ", format);
    } else {
        appendChunk(body, "fmt ", format);
        appendChunk(body, "data", pcm);
    }
    std::vector<std::uint8_t> wav{'R', 'I', 'F', 'F'};
    appendU32(wav, static_cast<std::uint32_t>(body.size() + 4U));
    wav.insert(wav.end(), {'W', 'A', 'V', 'E'});
    wav.insert(wav.end(), body.begin(), body.end());
    return wav;
}

std::shared_ptr<nuby::AudioClip> constantClip(float value, std::size_t frames = 64U) {
    auto clip = std::make_shared<nuby::AudioClip>();
    clip->sampleRate = 44100U;
    clip->channels = 1U;
    clip->samples.assign(frames, value);
    return clip;
}

void testWavPcmDecodeAndChunkOrdering() {
    const auto wav = makeWav(1U, 16U, {0x00U, 0x00U, 0xFFU, 0x7FU, 0x00U, 0x80U}, true);
    std::shared_ptr<nuby::AudioClip> clip;
    std::string error;
    expect(nuby::AudioEngine::decodeWav(wav, clip, error),
           "PCM WAV decodes when data precedes its format chunk");
    expect(error.empty() && clip != nullptr && clip->channels == 1U
                   && clip->sampleRate == 44100U && clip->frameCount() == 3U,
           "decoded clip exposes validated rate, channel count and frame count");
    if (clip != nullptr && clip->samples.size() == 3U) {
        expect(std::fabs(clip->samples[0]) < 0.00001f
                       && clip->samples[1] > 0.9999f
                       && clip->samples[2] == -1.0f,
               "signed little-endian PCM16 maps to normalized floating point");
    }

    std::shared_ptr<nuby::AudioClip> unsigned8;
    expect(nuby::AudioEngine::decodeWav(makeWav(1U, 8U, {0U, 128U, 255U}),
                                        unsigned8, error)
                   && unsigned8 != nullptr && unsigned8->samples[0] == -1.0f
                   && unsigned8->samples[1] == 0.0f && unsigned8->samples[2] > 0.99f,
           "unsigned PCM8 is centered and normalized");

    std::shared_ptr<nuby::AudioClip> signed24;
    expect(nuby::AudioEngine::decodeWav(
                   makeWav(1U, 24U, {0x00U, 0x00U, 0x80U, 0xFFU, 0xFFU, 0x7FU}),
                   signed24, error)
                   && signed24 != nullptr && signed24->samples[0] == -1.0f
                   && signed24->samples[1] > 0.9999f,
           "signed PCM24 sign extension is handled correctly");

    std::shared_ptr<nuby::AudioClip> signed32;
    expect(nuby::AudioEngine::decodeWav(makeWav(1U, 32U,
                                                {0U, 0U, 0U, 0x80U,
                                                 0xFFU, 0xFFU, 0xFFU, 0x7FU}),
                                        signed32, error)
                   && signed32 != nullptr && signed32->samples[0] == -1.0f
                   && signed32->samples[1] > 0.9999f,
           "signed PCM32 extrema decode without signed-overflow assumptions");
}

void testWavRejectsMalformedAndUnsupportedDataTransactionally() {
    const auto valid = makeWav(2U, 16U, {0U, 0U, 0U, 0U});
    std::shared_ptr<nuby::AudioClip> output = constantClip(0.25f);
    std::string error;
    auto unsupported = valid;
    unsupported[20U] = 3U;  // IEEE float format tag instead of PCM.
    expect(!nuby::AudioEngine::decodeWav(unsupported, output, error)
                   && output->samples[0] == 0.25f && !error.empty(),
           "unsupported formats fail without replacing the caller's previous clip");

    auto truncated = valid;
    truncated.pop_back();
    expect(!nuby::AudioEngine::decodeWav(truncated, output, error),
           "truncated RIFF payload is rejected");

    auto badBlock = valid;
    badBlock[32U] = 1U;
    badBlock[33U] = 0U;
    expect(!nuby::AudioEngine::decodeWav(badBlock, output, error),
           "inconsistent WAV block alignment is rejected");

    expect(!nuby::AudioEngine::decodeWav({}, output, error),
           "empty audio data is rejected safely");
}

void testMixingResamplingLoopingPauseAndStaleHandles() {
    nuby::AudioEngine engine;
    auto clip = std::make_shared<nuby::AudioClip>();
    clip->sampleRate = 22050U;
    clip->channels = 1U;
    clip->samples = {1.0f, 0.0f, -1.0f, 0.0f};
    const auto handle = engine.play(clip);
    expect(handle.valid() && engine.activeVoiceCount() == 1U,
           "valid clip starts a mixer voice");
    std::int16_t output[16]{};
    engine.renderPcm16(output, 8U);
    expect(output[0] == 32767 && output[1] == 32767
                   && output[2] > 16000 && output[2] < 17000
                   && output[4] == 0 && output[8] == -32768,
           "mono PCM is duplicated and linearly resampled to stereo device rate");
    expect(engine.activeVoiceCount() == 0U && !engine.stop(handle),
           "finished one-shot voice is released and its handle becomes stale");

    nuby::AudioPlayOptions loop;
    loop.looping = true;
    const auto looping = engine.play(constantClip(0.5f), loop);
    engine.pauseAll(true);
    std::fill(std::begin(output), std::end(output), 123);
    engine.renderPcm16(output, 4U);
    expect(std::all_of(std::begin(output), std::begin(output) + 8, [](std::int16_t sample) {
                return sample == 0;
            }) && engine.activeVoiceCount() == 1U,
           "global pause produces silence without advancing or destroying looping voices");
    engine.pauseAll(false);
    engine.renderPcm16(output, 4U);
    expect(output[0] > 16000 && output[1] > 16000 && engine.activeVoiceCount() == 1U,
           "loop resumes at its preserved cursor");
    expect(engine.pause(looping, true) && engine.pause(looping, false),
           "individual voice pause can be toggled");
    expect(engine.isVoiceActive(looping)
                   && engine.setVoicePosition(looping, {2.0f, 1.0f, -3.0f})
                   && !engine.setVoicePosition(looping,
                           {std::numeric_limits<float>::quiet_NaN(), 0.0f, 0.0f}),
           "live voice position updates accept finite coordinates only");
    expect(engine.stop(looping) && !engine.isVoiceActive(looping)
                   && engine.activeVoiceCount() == 0U,
           "voice can be stopped immediately and its handle becomes inactive");
}

void testSpatialPanAttenuationAndBuses() {
    nuby::AudioEngine engine;
    nuby::AudioListener listener;
    listener.position = {0.0f, 0.0f, 0.0f};
    listener.right = {1.0f, 0.0f, 0.0f};
    engine.setListener(listener);
    nuby::AudioPlayOptions options;
    options.positional = true;
    options.position = {1.0f, 0.0f, 0.0f};
    options.rolloff = 0.0f;
    const auto handle = engine.play(constantClip(0.5f), options);
    std::int16_t output[8]{};
    engine.renderPcm16(output, 4U);
    expect(handle.valid() && std::abs(output[0]) < 5 && output[1] > 16000,
           "3D panning places a right-side source in the right channel");
    expect(engine.setVoicePosition(handle, {-1.0f, 0.0f, 0.0f}),
           "moving source can update its world position while playing");
    engine.renderPcm16(output, 1U);
    expect(output[0] > 16000 && std::abs(output[1]) < 5,
           "live source movement updates spatial panning on the next mixer block");

    engine.stopAll();
    engine.setBusGain(nuby::AudioBus::Effects, 0.25f);
    options.positional = false;
    options.bus = nuby::AudioBus::Effects;
    engine.play(constantClip(0.5f), options);
    engine.renderPcm16(output, 1U);
    expect(output[0] > 4000 && output[0] < 4200 && output[0] == output[1],
           "effects bus gain is applied without disturbing stereo balance");

    engine.stopAll();
    options.positional = true;
    options.position = {30.0f, 0.0f, 0.0f};
    options.maximumDistance = 24.0f;
    engine.play(constantClip(0.5f), options);
    engine.renderPcm16(output, 1U);
    expect(output[0] == 0 && output[1] == 0,
           "positional voices beyond their maximum range are muted");
}

void testVoiceLimitsPriorityStealingAndFade() {
    nuby::AudioEngine engine;
    auto longClip = constantClip(0.5f, 5000U);
    std::array<nuby::AudioVoiceHandle, nuby::AudioEngine::kMaximumVoices> handles{};
    for (std::size_t index = 0U; index < handles.size(); ++index) {
        nuby::AudioPlayOptions options;
        options.priority = index == 0U ? 2U : 10U;
        handles[index] = engine.play(longClip, options);
        expect(handles[index].valid(), "voice pool admits its documented number of voices");
    }
    nuby::AudioPlayOptions lowPriority;
    lowPriority.priority = 1U;
    expect(!engine.play(longClip, lowPriority).valid(),
           "voice pool rejects a new sound below all active priorities");
    nuby::AudioPlayOptions higherPriority;
    higherPriority.priority = 3U;
    const auto replacement = engine.play(longClip, higherPriority);
    expect(replacement.valid() && !engine.stop(handles[0]) && engine.stop(handles[1]),
           "priority stealing is deterministic and invalidates only the stolen handle");

    engine.stopAll();
    const auto fading = engine.play(constantClip(1.0f));
    expect(engine.stop(fading, 2.0f / 44100.0f), "voice accepts a short stop fade");
    std::int16_t output[4]{};
    engine.renderPcm16(output, 2U);
    expect(output[0] > 32000 && output[2] > 15000 && output[2] < 17000
                   && engine.activeVoiceCount() == 0U,
           "stop fades across the requested output frames before releasing the clip");
}

void testPackagedAdventureAudioClipsDecode() {
    for (const char* path : {"app/src/main/assets/audio/jump.wav",
                             "app/src/main/assets/audio/relic.wav",
                             "app/src/main/assets/audio/gate.wav",
                             "app/src/main/assets/audio/gate_hum.wav"}) {
        std::ifstream input(path, std::ios::binary);
        std::vector<std::uint8_t> bytes;
        char sample = 0;
        while (input.get(sample)) {
            bytes.push_back(static_cast<std::uint8_t>(static_cast<unsigned char>(sample)));
        }
        std::shared_ptr<nuby::AudioClip> clip;
        std::string error;
        expect(input.good() || input.eof(), "packaged audio cue is readable from the assets tree");
        expect(nuby::AudioEngine::decodeWav(bytes, clip, error) && clip != nullptr
                       && clip->channels == 1U && clip->sampleRate == 44100U
                       && clip->frameCount() > 10000U,
               "generated adventure cue decodes as usable mono PCM");
    }
}

void testConcurrentCommandsAndMixingAreSerialized() {
    nuby::AudioEngine engine;
    auto clip = constantClip(0.12f, 1024U);
    std::thread callback([&engine]() {
        std::array<std::int16_t, 256U> buffer{};
        for (int block = 0; block < 120; ++block) {
            engine.renderPcm16(buffer.data(), buffer.size() / 2U);
        }
    });
    for (int voice = 0; voice < 120; ++voice) {
        nuby::AudioPlayOptions options;
        options.looping = true;
        options.priority = static_cast<std::uint8_t>(voice % 200);
        const auto handle = engine.play(clip, options);
        if (handle.valid()) engine.stop(handle, 0.001f);
        if ((voice % 3) == 0) engine.pauseAll((voice % 2) == 0);
    }
    engine.pauseAll(false);
    callback.join();
    engine.stopAll();
    expect(engine.activeVoiceCount() == 0U,
           "concurrent render and voice commands remain synchronized without leaks");
}

void testInvalidInputsAndGainBounds() {
    nuby::AudioEngine engine;
    nuby::AudioPlayOptions invalid;
    invalid.gain = std::numeric_limits<float>::quiet_NaN();
    expect(!engine.play(constantClip(0.3f), invalid).valid(),
           "non-finite playback gains are rejected");
    invalid = {};
    invalid.maximumDistance = 0.5f;
    expect(!engine.play(constantClip(0.3f), invalid).valid(),
           "invalid positional distance ranges are rejected");
    engine.setMasterGain(9.0f);
    const auto handle = engine.play(constantClip(0.3f));
    std::int16_t output[2]{};
    engine.renderPcm16(output, 1U);
    expect(handle.valid() && output[0] > 9000 && output[0] < 11000,
           "out-of-range master gain changes are ignored safely");
}

}  // namespace

int main() {
    testWavPcmDecodeAndChunkOrdering();
    testWavRejectsMalformedAndUnsupportedDataTransactionally();
    testMixingResamplingLoopingPauseAndStaleHandles();
    testSpatialPanAttenuationAndBuses();
    testVoiceLimitsPriorityStealingAndFade();
    testPackagedAdventureAudioClipsDecode();
    testConcurrentCommandsAndMixingAreSerialized();
    testInvalidInputsAndGainBounds();
    if (failures != 0) {
        std::cerr << failures << " audio engine test(s) failed\n";
        return EXIT_FAILURE;
    }
    std::cout << "All audio engine tests passed\n";
    return EXIT_SUCCESS;
}
