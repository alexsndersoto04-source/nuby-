#include "nuby/game_project_runtime.hpp"
#include "nuby/scene_io.hpp"

#include <cmath>
#include <cstdlib>
#include <iostream>
#include <limits>
#include <sstream>
#include <string>

namespace {
int failures = 0;

void expect(bool condition, const char* message) {
    if (!condition) {
        std::cerr << "FAIL: " << message << '\n';
        ++failures;
    }
}

std::string projectDescriptor(const std::string& name = "Runtime Test") {
    return "NUBY_PROJECT 1\nNAME \"" + name + "\"\n"
           "PACKAGE_ID com.example.runtime\n"
           "STARTUP_SCENE scenes/start.nuby\nEND_PROJECT\n";
}

std::string startupScene() {
    nuby::Scene scene;
    scene.physicsSettings().gravity = {0.0f, -10.0f, 0.0f};
    nuby::Entity& player = scene.create("Player", nuby::Primitive::Sphere);
    player.transform.position = {0.0f, 2.0f, 0.0f};
    player.rigidbody.enabled = true;
    player.rigidbody.linearDamping = 0.0f;
    player.sphereCollider.enabled = true;
    player.sphereCollider.radius = 0.25f;

    std::ostringstream output;
    std::string error;
    expect(nuby::SceneSerializer::save(scene, output, error),
           "runtime test startup scene serializes");
    return output.str();
}

nuby::ProjectAssetReader readerFor(const std::string& sceneData,
                                    std::string* requestedPath = nullptr) {
    return [&sceneData, requestedPath](const std::string& path,
                                       std::string& contents,
                                       std::string& error) {
        if (requestedPath != nullptr) *requestedPath = path;
        if (path != "scenes/start.nuby") {
            error = "unexpected project asset path";
            return false;
        }
        contents = sceneData;
        return true;
    };
}

void testRuntimeLoadsAndStepsOnlyTheSelectedProject() {
    const std::string sceneData = startupScene();
    std::string requestedPath;
    nuby::GameProjectRuntime runtime;
    std::string error;
    std::istringstream descriptor(projectDescriptor());

    expect(!runtime.loaded() && runtime.project() == nullptr && runtime.scene() == nullptr,
           "project runtime starts unloaded without an active scene");
    expect(runtime.load(descriptor, readerFor(sceneData, &requestedPath), error),
           "project runtime loads manifest and startup scene");
    expect(runtime.loaded() && runtime.project() != nullptr && runtime.scene() != nullptr,
           "successful load exposes active project and scene state");
    expect(runtime.project()->displayName == "Runtime Test"
                   && requestedPath == "scenes/start.nuby"
                   && runtime.scene()->entities().size() == 1,
           "runtime activates only the declared project's startup scene");

    const std::uint32_t playerId = runtime.scene()->entities().front().id;
    const float initialHeight = runtime.scene()->find(playerId)->transform.position[1];
    for (int frame = 0; frame < 30; ++frame) {
        expect(runtime.update(1.0f / 60.0f), "loaded project accepts simulation steps");
    }
    expect(runtime.scene()->find(playerId)->transform.position[1] < initialHeight,
           "project runtime advances its scene physics without demo-specific game code");
}

void testFailedReloadIsTransactionalAndInvalidStepsDoNotAdvance() {
    const std::string sceneData = startupScene();
    nuby::GameProjectRuntime runtime;
    std::string error;
    std::istringstream descriptor(projectDescriptor("Keep Existing"));
    expect(runtime.load(descriptor, readerFor(sceneData), error),
           "initial project is available for runtime reload tests");
    const float previousElapsed = runtime.scene()->elapsedSeconds();
    expect(!runtime.update(std::numeric_limits<float>::quiet_NaN()),
           "runtime rejects non-finite simulation delta");
    expect(!runtime.update(-0.1f), "runtime rejects negative simulation delta");
    expect(std::fabs(runtime.scene()->elapsedSeconds() - previousElapsed) < 1.0e-6f,
           "invalid simulation steps leave the active scene unchanged");

    std::istringstream invalidDescriptor(
            "NUBY_PROJECT 1\nNAME \"Broken\"\nPACKAGE_ID bad-package\n"
            "STARTUP_SCENE scenes/start.nuby\nEND_PROJECT\n");
    expect(!runtime.load(invalidDescriptor, readerFor(sceneData), error),
           "invalid replacement project is rejected");
    expect(runtime.loaded() && runtime.project()->displayName == "Keep Existing"
                   && runtime.scene()->entities().size() == 1
                   && std::fabs(runtime.scene()->elapsedSeconds() - previousElapsed) < 1.0e-6f,
           "failed reload preserves the live project's metadata and simulation state");

    std::istringstream missingSceneDescriptor(projectDescriptor("Missing Scene"));
    const nuby::ProjectAssetReader missingScene = [](const std::string&,
            std::string&, std::string& readError) {
        readError = "startup scene is missing";
        return false;
    };
    expect(!runtime.load(missingSceneDescriptor, missingScene, error),
           "replacement project with an unreadable startup scene is rejected");
    expect(runtime.loaded() && runtime.project()->displayName == "Keep Existing"
                   && runtime.scene()->entities().size() == 1
                   && std::fabs(runtime.scene()->elapsedSeconds() - previousElapsed) < 1.0e-6f,
           "startup asset failure also preserves the active project and scene");
}

void testRuntimeDescriptorSaveAndUnloadLifecycle() {
    const std::string sceneData = startupScene();
    nuby::GameProjectRuntime runtime;
    std::string error;
    std::istringstream descriptor(projectDescriptor("Saveable Runtime"));
    expect(runtime.load(descriptor, readerFor(sceneData), error),
           "runtime loads project before descriptor save");

    std::ostringstream savedDescriptor;
    expect(runtime.saveDescriptor(savedDescriptor, error)
                   && savedDescriptor.str().find("NAME \"Saveable Runtime\"") != std::string::npos,
           "runtime can save the descriptor of its active project");

    runtime.unload();
    expect(!runtime.loaded() && runtime.project() == nullptr && runtime.scene() == nullptr,
           "unload clears the active project and scene");
    expect(!runtime.update(1.0f / 60.0f), "unloaded runtime does not simulate a scene");
    std::ostringstream noProjectDescriptor;
    expect(!runtime.saveDescriptor(noProjectDescriptor, error)
                   && noProjectDescriptor.str().empty(),
           "unloaded runtime cannot write a misleading project descriptor");
}

}  // namespace

int main() {
    testRuntimeLoadsAndStepsOnlyTheSelectedProject();
    testFailedReloadIsTransactionalAndInvalidStepsDoNotAdvance();
    testRuntimeDescriptorSaveAndUnloadLifecycle();
    if (failures != 0) {
        std::cerr << failures << " project runtime test(s) failed\n";
        return EXIT_FAILURE;
    }
    std::cout << "All game project runtime tests passed\n";
    return EXIT_SUCCESS;
}
