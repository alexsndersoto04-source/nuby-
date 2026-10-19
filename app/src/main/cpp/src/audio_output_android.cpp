#include "nuby/audio_output_android.hpp"

namespace nuby {

AndroidAudioOutput::~AndroidAudioOutput() {
    stop();
}

bool AndroidAudioOutput::start(AudioEngine& engine) {
    std::lock_guard<std::mutex> lock(lifecycleMutex_);
    stopLocked();
    engine_ = &engine;
    if (slCreateEngine(&engineObject_, 0U, nullptr, 0U, nullptr, nullptr) != SL_RESULT_SUCCESS
            || (*engineObject_)->Realize(engineObject_, SL_BOOLEAN_FALSE) != SL_RESULT_SUCCESS
            || (*engineObject_)->GetInterface(
                    engineObject_, SL_IID_ENGINE, &slEngine_) != SL_RESULT_SUCCESS) {
        stopLocked();
        return false;
    }
    if ((*slEngine_)->CreateOutputMix(slEngine_, &outputMixObject_, 0U, nullptr, nullptr)
                    != SL_RESULT_SUCCESS
            || (*outputMixObject_)->Realize(outputMixObject_, SL_BOOLEAN_FALSE)
                    != SL_RESULT_SUCCESS) {
        stopLocked();
        return false;
    }

    SLDataLocator_AndroidSimpleBufferQueue bufferLocator{
            SL_DATALOCATOR_ANDROIDSIMPLEBUFFERQUEUE,
            static_cast<SLuint32>(buffers_.size())};
    SLDataFormat_PCM pcmFormat{
            SL_DATAFORMAT_PCM,
            2U,
            SL_SAMPLINGRATE_44_1,
            SL_PCMSAMPLEFORMAT_FIXED_16,
            SL_PCMSAMPLEFORMAT_FIXED_16,
            SL_SPEAKER_FRONT_LEFT | SL_SPEAKER_FRONT_RIGHT,
            SL_BYTEORDER_LITTLEENDIAN};
    SLDataSource audioSource{&bufferLocator, &pcmFormat};
    SLDataLocator_OutputMix outputLocator{SL_DATALOCATOR_OUTPUTMIX, outputMixObject_};
    SLDataSink audioSink{&outputLocator, nullptr};
    const SLInterfaceID interfaceIds[] = {SL_IID_ANDROIDSIMPLEBUFFERQUEUE};
    const SLboolean interfaceRequired[] = {SL_BOOLEAN_TRUE};
    if ((*slEngine_)->CreateAudioPlayer(slEngine_, &playerObject_, &audioSource, &audioSink,
                                        1U, interfaceIds, interfaceRequired)
                    != SL_RESULT_SUCCESS
            || (*playerObject_)->Realize(playerObject_, SL_BOOLEAN_FALSE) != SL_RESULT_SUCCESS
            || (*playerObject_)->GetInterface(
                    playerObject_, SL_IID_PLAY, &playInterface_) != SL_RESULT_SUCCESS
            || (*playerObject_)->GetInterface(playerObject_,
                    SL_IID_ANDROIDSIMPLEBUFFERQUEUE, &bufferQueue_) != SL_RESULT_SUCCESS
            || (*bufferQueue_)->RegisterCallback(bufferQueue_, &AndroidAudioOutput::onBufferQueue,
                                                  this) != SL_RESULT_SUCCESS) {
        stopLocked();
        return false;
    }

    nextBuffer_ = 0U;
    for (PcmBuffer& buffer : buffers_) {
        engine_->renderPcm16(buffer.data(), kFramesPerBuffer);
        if ((*bufferQueue_)->Enqueue(bufferQueue_, buffer.data(),
                static_cast<SLuint32>(buffer.size() * sizeof(buffer.front())))
                != SL_RESULT_SUCCESS) {
            stopLocked();
            return false;
        }
    }
    if ((*playInterface_)->SetPlayState(playInterface_, SL_PLAYSTATE_PLAYING)
            != SL_RESULT_SUCCESS) {
        stopLocked();
        return false;
    }
    return true;
}

void AndroidAudioOutput::stop() {
    std::lock_guard<std::mutex> lock(lifecycleMutex_);
    stopLocked();
}

void AndroidAudioOutput::stopLocked() {
    if (playInterface_ != nullptr) {
        (*playInterface_)->SetPlayState(playInterface_, SL_PLAYSTATE_STOPPED);
        playInterface_ = nullptr;
    }
    if (bufferQueue_ != nullptr) {
        (*bufferQueue_)->Clear(bufferQueue_);
        bufferQueue_ = nullptr;
    }
    if (playerObject_ != nullptr) {
        (*playerObject_)->Destroy(playerObject_);
        playerObject_ = nullptr;
    }
    if (outputMixObject_ != nullptr) {
        (*outputMixObject_)->Destroy(outputMixObject_);
        outputMixObject_ = nullptr;
    }
    slEngine_ = nullptr;
    if (engineObject_ != nullptr) {
        (*engineObject_)->Destroy(engineObject_);
        engineObject_ = nullptr;
    }
    engine_ = nullptr;
}

void AndroidAudioOutput::pause() {
    std::lock_guard<std::mutex> lock(lifecycleMutex_);
    if (playInterface_ != nullptr) {
        (*playInterface_)->SetPlayState(playInterface_, SL_PLAYSTATE_PAUSED);
    }
}

void AndroidAudioOutput::resume() {
    std::lock_guard<std::mutex> lock(lifecycleMutex_);
    if (playInterface_ != nullptr) {
        (*playInterface_)->SetPlayState(playInterface_, SL_PLAYSTATE_PLAYING);
    }
}

bool AndroidAudioOutput::running() const {
    std::lock_guard<std::mutex> lock(lifecycleMutex_);
    return playerObject_ != nullptr;
}

void AndroidAudioOutput::onBufferQueue(SLAndroidSimpleBufferQueueItf queue, void* context) {
    if (context != nullptr) static_cast<AndroidAudioOutput*>(context)->refill(queue);
}

void AndroidAudioOutput::refill(SLAndroidSimpleBufferQueueItf queue) {
    if (engine_ == nullptr || queue == nullptr) return;
    PcmBuffer& buffer = buffers_[nextBuffer_];
    nextBuffer_ = (nextBuffer_ + 1U) % buffers_.size();
    engine_->renderPcm16(buffer.data(), kFramesPerBuffer);
    (*queue)->Enqueue(queue, buffer.data(),
            static_cast<SLuint32>(buffer.size() * sizeof(buffer.front())));
}

}  // namespace nuby
