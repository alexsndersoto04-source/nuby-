#include "nuby/renderer.hpp"

#include <jni.h>
#include <memory>

#include <android/asset_manager_jni.h>

namespace {
std::unique_ptr<nuby::Renderer> gRenderer;
}

extern "C" JNIEXPORT void JNICALL
Java_com_nuby_engine_MainActivity_nativeCreate(JNIEnv* env, jclass, jobject javaAssets) {
    if (!gRenderer) {
        gRenderer = std::make_unique<nuby::Renderer>();
    }
    AAssetManager* assetManager = AAssetManager_fromJava(env, javaAssets);
    gRenderer->initialize(assetManager);
}

extern "C" JNIEXPORT jboolean JNICALL
Java_com_nuby_engine_MainActivity_nativeShadowsEnabled(JNIEnv*, jclass) {
    return gRenderer && gRenderer->shadowsEnabled() ? JNI_TRUE : JNI_FALSE;
}

extern "C" JNIEXPORT jboolean JNICALL
Java_com_nuby_engine_MainActivity_nativePostProcessingEnabled(JNIEnv*, jclass) {
    return gRenderer && gRenderer->postProcessingEnabled() ? JNI_TRUE : JNI_FALSE;
}

extern "C" JNIEXPORT jboolean JNICALL
Java_com_nuby_engine_MainActivity_nativeSampleMeshLoaded(JNIEnv*, jclass) {
    return gRenderer && gRenderer->sampleMeshLoaded() ? JNI_TRUE : JNI_FALSE;
}

extern "C" JNIEXPORT jboolean JNICALL
Java_com_nuby_engine_MainActivity_nativeSampleMaterialLoaded(JNIEnv*, jclass) {
    return gRenderer && gRenderer->sampleMaterialLoaded() ? JNI_TRUE : JNI_FALSE;
}

extern "C" JNIEXPORT void JNICALL
Java_com_nuby_engine_MainActivity_nativeSetGameMode(JNIEnv*, jclass, jboolean enabled) {
    if (gRenderer) gRenderer->setGameMode(enabled == JNI_TRUE);
}

extern "C" JNIEXPORT void JNICALL
Java_com_nuby_engine_MainActivity_nativeSetDemoMode(JNIEnv*, jclass, jint mode) {
    if (gRenderer) gRenderer->setDemoMode(mode);
}

extern "C" JNIEXPORT void JNICALL
Java_com_nuby_engine_MainActivity_nativeAdventureMove(
        JNIEnv*, jclass, jfloat horizontal, jfloat forward) {
    if (gRenderer) gRenderer->setAdventureMoveInput(horizontal, forward);
}

extern "C" JNIEXPORT void JNICALL
Java_com_nuby_engine_MainActivity_nativeAdventureJump(JNIEnv*, jclass) {
    if (gRenderer) gRenderer->jumpAdventureCharacter();
}

extern "C" JNIEXPORT void JNICALL
Java_com_nuby_engine_MainActivity_nativeRestartGame(JNIEnv*, jclass) {
    if (gRenderer) gRenderer->restartGame();
}

extern "C" JNIEXPORT jstring JNICALL
Java_com_nuby_engine_MainActivity_nativeGameStatus(JNIEnv* env, jclass) {
    const std::string status = gRenderer ? gRenderer->gameStatusText() : std::string{};
    return env->NewStringUTF(status.c_str());
}

extern "C" JNIEXPORT void JNICALL
Java_com_nuby_engine_MainActivity_nativeResize(JNIEnv*, jclass, jint width, jint height) {
    if (gRenderer) gRenderer->resize(width, height);
}

extern "C" JNIEXPORT void JNICALL
Java_com_nuby_engine_MainActivity_nativeFrame(JNIEnv*, jclass, jfloat deltaSeconds) {
    if (gRenderer) gRenderer->render(deltaSeconds);
}

extern "C" JNIEXPORT void JNICALL
Java_com_nuby_engine_MainActivity_nativeTouch(
        JNIEnv*, jclass, jint action, jfloat x, jfloat y,
        jfloat pinchDistance, jint pointerCount) {
    if (gRenderer) gRenderer->touch(action, x, y, pinchDistance, pointerCount);
}
