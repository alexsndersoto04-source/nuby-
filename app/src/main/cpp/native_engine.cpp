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
        JNIEnv*, jclass, jint action, jfloat x, jfloat y) {
    if (gRenderer) gRenderer->touch(action, x, y);
}
