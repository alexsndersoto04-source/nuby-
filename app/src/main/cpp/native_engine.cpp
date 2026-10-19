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

extern "C" JNIEXPORT jint JNICALL
Java_com_nuby_engine_MainActivity_nativeRendererTier(JNIEnv*, jclass) {
    return gRenderer ? static_cast<jint>(gRenderer->rendererTier()) : 0;
}

extern "C" JNIEXPORT jint JNICALL
Java_com_nuby_engine_MainActivity_nativeSceneAssetStatus(JNIEnv*, jclass) {
    return gRenderer ? static_cast<jint>(gRenderer->sceneAssetStatus()) : 0;
}

extern "C" JNIEXPORT jstring JNICALL
Java_com_nuby_engine_MainActivity_nativeAssetMemoryStatus(JNIEnv* env, jclass) {
    const std::string status = gRenderer
            ? gRenderer->assetMemoryStatusText() : std::string("CACHE ~0MB");
    return env->NewStringUTF(status.c_str());
}

extern "C" JNIEXPORT jstring JNICALL
Java_com_nuby_engine_MainActivity_nativeFrameTimingStatus(JNIEnv* env, jclass) {
    const std::string status = gRenderer
            ? gRenderer->frameTimingStatusText() : std::string("CPU P95 --");
    return env->NewStringUTF(status.c_str());
}

extern "C" JNIEXPORT void JNICALL
Java_com_nuby_engine_MainActivity_nativeSetGameMode(JNIEnv*, jclass, jboolean enabled) {
    if (gRenderer) gRenderer->setGameMode(enabled == JNI_TRUE);
}

extern "C" JNIEXPORT void JNICALL
Java_com_nuby_engine_MainActivity_nativeSetDemoMode(JNIEnv*, jclass, jint mode) {
    if (gRenderer) gRenderer->setDemoMode(mode);
}

extern "C" JNIEXPORT jstring JNICALL
Java_com_nuby_engine_MainActivity_nativeLoadProject(
        JNIEnv* env, jclass, jstring descriptorAssetPath) {
    if (!gRenderer) return env->NewStringUTF("Renderer is not initialized");
    if (descriptorAssetPath == nullptr) {
        return env->NewStringUTF("Project descriptor path is empty");
    }
    const char* pathChars = env->GetStringUTFChars(descriptorAssetPath, nullptr);
    if (pathChars == nullptr) return nullptr;
    const std::string path(pathChars);
    env->ReleaseStringUTFChars(descriptorAssetPath, pathChars);
    const bool loaded = gRenderer->loadProject(path);
    const std::string status = loaded
            ? "Loaded project"
            : gRenderer->projectStatusText();
    return env->NewStringUTF(status.c_str());
}

extern "C" JNIEXPORT jboolean JNICALL
Java_com_nuby_engine_MainActivity_nativeUnloadProject(JNIEnv*, jclass) {
    if (!gRenderer || !gRenderer->projectLoaded()) return JNI_FALSE;
    gRenderer->unloadProject();
    return JNI_TRUE;
}

extern "C" JNIEXPORT jobjectArray JNICALL
Java_com_nuby_engine_MainActivity_nativeSaveEditorProject(JNIEnv* env, jclass) {
    if (!gRenderer) return nullptr;
    std::string descriptor;
    std::string scene;
    std::string error;
    if (!gRenderer->saveEditorProject(descriptor, scene, error)) return nullptr;
    jclass stringClass = env->FindClass("java/lang/String");
    if (stringClass == nullptr) return nullptr;
    jobjectArray result = env->NewObjectArray(2, stringClass, nullptr);
    if (result == nullptr) return nullptr;
    jstring descriptorText = env->NewStringUTF(descriptor.c_str());
    jstring sceneText = env->NewStringUTF(scene.c_str());
    if (descriptorText == nullptr || sceneText == nullptr) return nullptr;
    env->SetObjectArrayElement(result, 0, descriptorText);
    env->SetObjectArrayElement(result, 1, sceneText);
    env->DeleteLocalRef(descriptorText);
    env->DeleteLocalRef(sceneText);
    return result;
}

extern "C" JNIEXPORT jstring JNICALL
Java_com_nuby_engine_MainActivity_nativeLoadEditorProject(
        JNIEnv* env, jclass, jstring descriptorValue, jstring sceneValue) {
    if (!gRenderer) return env->NewStringUTF("Renderer is not initialized");
    if (descriptorValue == nullptr || sceneValue == nullptr) {
        return env->NewStringUTF("Saved project files are unavailable");
    }
    const char* descriptorChars = env->GetStringUTFChars(descriptorValue, nullptr);
    const char* sceneChars = env->GetStringUTFChars(sceneValue, nullptr);
    if (descriptorChars == nullptr || sceneChars == nullptr) {
        if (descriptorChars != nullptr) env->ReleaseStringUTFChars(descriptorValue, descriptorChars);
        if (sceneChars != nullptr) env->ReleaseStringUTFChars(sceneValue, sceneChars);
        return nullptr;
    }
    const std::string descriptor(descriptorChars);
    const std::string scene(sceneChars);
    env->ReleaseStringUTFChars(descriptorValue, descriptorChars);
    env->ReleaseStringUTFChars(sceneValue, sceneChars);
    std::string error;
    const bool loaded = gRenderer->loadEditorProject(descriptor, scene, error);
    const std::string status = loaded ? "Project loaded" : "Load failed: " + error;
    return env->NewStringUTF(status.c_str());
}

extern "C" JNIEXPORT jboolean JNICALL
Java_com_nuby_engine_MainActivity_nativeEditorAddPrimitive(JNIEnv*, jclass, jint type) {
    return gRenderer && gRenderer->editorAddPrimitive(type) ? JNI_TRUE : JNI_FALSE;
}

extern "C" JNIEXPORT jboolean JNICALL
Java_com_nuby_engine_MainActivity_nativeEditorSelectNext(JNIEnv*, jclass, jint direction) {
    return gRenderer && gRenderer->editorSelectNext(direction) ? JNI_TRUE : JNI_FALSE;
}

extern "C" JNIEXPORT jboolean JNICALL
Java_com_nuby_engine_MainActivity_nativeEditorTranslate(
        JNIEnv*, jclass, jint axis, jfloat amount) {
    return gRenderer && gRenderer->editorTranslate(axis, amount) ? JNI_TRUE : JNI_FALSE;
}

extern "C" JNIEXPORT jboolean JNICALL
Java_com_nuby_engine_MainActivity_nativeEditorRotate(
        JNIEnv*, jclass, jint axis, jfloat amount) {
    return gRenderer && gRenderer->editorRotate(axis, amount) ? JNI_TRUE : JNI_FALSE;
}

extern "C" JNIEXPORT jboolean JNICALL
Java_com_nuby_engine_MainActivity_nativeEditorScale(JNIEnv*, jclass, jfloat factor) {
    return gRenderer && gRenderer->editorScale(factor) ? JNI_TRUE : JNI_FALSE;
}

extern "C" JNIEXPORT jboolean JNICALL
Java_com_nuby_engine_MainActivity_nativeEditorCycleColor(JNIEnv*, jclass) {
    return gRenderer && gRenderer->editorCycleColor() ? JNI_TRUE : JNI_FALSE;
}

extern "C" JNIEXPORT jboolean JNICALL
Java_com_nuby_engine_MainActivity_nativeEditorDuplicate(JNIEnv*, jclass) {
    return gRenderer && gRenderer->editorDuplicate() ? JNI_TRUE : JNI_FALSE;
}

extern "C" JNIEXPORT jboolean JNICALL
Java_com_nuby_engine_MainActivity_nativeEditorDelete(JNIEnv*, jclass) {
    return gRenderer && gRenderer->editorDelete() ? JNI_TRUE : JNI_FALSE;
}

extern "C" JNIEXPORT jstring JNICALL
Java_com_nuby_engine_MainActivity_nativeEditorSelectionStatus(JNIEnv* env, jclass) {
    const std::string status = gRenderer ? gRenderer->editorSelectionStatus() : std::string{};
    return env->NewStringUTF(status.c_str());
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
