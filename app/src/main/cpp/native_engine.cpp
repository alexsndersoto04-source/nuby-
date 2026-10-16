#include "nuby/renderer.hpp"

#include <jni.h>
#include <memory>

namespace {
std::unique_ptr<nuby::Renderer> gRenderer;
}

extern "C" JNIEXPORT void JNICALL
Java_com_nuby_engine_MainActivity_nativeCreate(JNIEnv*, jclass) {
    if (!gRenderer) {
        gRenderer = std::make_unique<nuby::Renderer>();
    }
    gRenderer->initialize();
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
