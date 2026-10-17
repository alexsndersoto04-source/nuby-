#include "nuby/game_project.hpp"

#include "nuby/scene_io.hpp"

#include <cstdlib>
#include <iostream>
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

std::string serializedScene() {
    nuby::Scene scene;
    nuby::Entity& hero = scene.create("Hero", nuby::Primitive::Sphere);
    hero.transform.position = {1.0f, 2.0f, 3.0f};
    std::ostringstream output;
    std::string error;
    expect(nuby::SceneSerializer::save(scene, output, error),
           "test scene can be serialized for the project package");
    return output.str();
}

std::string validDescriptor() {
    return "NUBY_PROJECT 1\nNAME \"Sol y Ceniza\"\n"
           "PACKAGE_ID com.example.solyceniza\n"
           "STARTUP_SCENE scenes/main.nuby\nEND_PROJECT\n";
}

void testProjectLoadsManifestAndStartupSceneTransactionally() {
    const std::string sceneText = serializedScene();
    nuby::GameProject project;
    nuby::Scene scene;
    scene.create("old scene", nuby::Primitive::Torus);
    std::string error;
    std::istringstream descriptor(validDescriptor());
    std::string requestedPath;
    const nuby::ProjectAssetReader readAsset = [&sceneText, &requestedPath](
            const std::string& path, std::string& contents, std::string&) {
        requestedPath = path;
        contents = sceneText;
        return true;
    };

    expect(nuby::GameProjectLoader::load(descriptor, readAsset, project, scene, error),
           "valid project descriptor and startup scene load");
    expect(project.displayName == "Sol y Ceniza"
                   && project.packageIdentifier == "com.example.solyceniza"
                   && project.startupScene == "scenes/main.nuby",
           "project metadata is restored from its standalone descriptor");
    expect(requestedPath == "scenes/main.nuby",
           "project loader requests only the declared startup scene");
    expect(scene.entities().size() == 1 && scene.entities()[0].name == "Hero"
                   && scene.entities()[0].transform.position[2] == 3.0f,
           "the project scene replaces the prior scene only after successful loading");
}

void testUnsafePathsAndInvalidMetadataAreRejectedBeforeAssetAccess() {
    const std::string unsafeDescriptors[] = {
        "NUBY_PROJECT 1\nNAME \"Bad Path\"\nPACKAGE_ID com.example.bad\n"
        "STARTUP_SCENE ../outside.nuby\nEND_PROJECT\n",
        "NUBY_PROJECT 1\nNAME \"Bad Package\"\nPACKAGE_ID com..bad\n"
        "STARTUP_SCENE scenes/main.nuby\nEND_PROJECT\n",
        "NUBY_PROJECT 1\nNAME \"Absolute Path\"\nPACKAGE_ID com.example.bad\n"
        "STARTUP_SCENE /private/scene.nuby\nEND_PROJECT\n"
    };
    int assetReadCount = 0;
    const nuby::ProjectAssetReader readAsset = [&assetReadCount](
            const std::string&, std::string&, std::string&) {
        ++assetReadCount;
        return true;
    };

    for (const std::string& text : unsafeDescriptors) {
        nuby::GameProject project;
        project.displayName = "Keep this project";
        project.packageIdentifier = "org.example.keep";
        project.startupScene = "safe/main.nuby";
        nuby::Scene scene;
        scene.create("Keep this scene", nuby::Primitive::Sphere);
        std::istringstream descriptor(text);
        std::string error;
        expect(!nuby::GameProjectLoader::load(descriptor, readAsset, project, scene, error),
               "unsafe project paths and invalid package identifiers are rejected");
        expect(project.displayName == "Keep this project"
                       && scene.entities().size() == 1
                       && scene.entities()[0].name == "Keep this scene",
               "invalid project metadata leaves output project and scene untouched");
    }
    expect(assetReadCount == 0,
           "invalid project paths are rejected before reading any packaged asset");
}

void testMissingOrMalformedStartupSceneLeavesExistingStateUntouched() {
    nuby::GameProject project;
    project.displayName = "Existing Project";
    project.packageIdentifier = "org.example.existing";
    project.startupScene = "scenes/old.nuby";
    nuby::Scene scene;
    scene.create("Existing Scene", nuby::Primitive::Sphere);
    std::string error;

    std::istringstream missingDescriptor(validDescriptor());
    const nuby::ProjectAssetReader missingAsset = [](const std::string&,
            std::string&, std::string& readError) {
        readError = "file missing";
        return false;
    };
    expect(!nuby::GameProjectLoader::load(missingDescriptor, missingAsset,
                   project, scene, error), "missing startup scene asset is reported");
    expect(project.displayName == "Existing Project" && scene.entities().size() == 1
                   && scene.entities()[0].name == "Existing Scene",
           "missing startup scene does not partially replace current state");

    std::istringstream malformedDescriptor(validDescriptor());
    const nuby::ProjectAssetReader malformedAsset = [](const std::string&,
            std::string& contents, std::string&) {
        contents = "not a Nuby scene";
        return true;
    };
    expect(!nuby::GameProjectLoader::load(malformedDescriptor, malformedAsset,
                   project, scene, error), "malformed startup scene is rejected");
    expect(project.displayName == "Existing Project" && scene.entities().size() == 1
                   && scene.entities()[0].name == "Existing Scene",
           "malformed scene preserves the currently loaded project and scene");
}

void testProjectDescriptorRoundTripAndWriterValidation() {
    nuby::GameProject source;
    source.displayName = "Adventure: Dawn \"Again\"";
    source.packageIdentifier = "com.studio.adventure";
    source.startupScene = "levels/first.nuby";
    std::ostringstream output;
    std::string error;
    expect(nuby::GameProjectLoader::save(source, output, error),
           "valid project metadata can be saved");
    const std::string sceneText = serializedScene();
    nuby::ProjectAssetReader readAsset = [&sceneText](const std::string&,
            std::string& contents, std::string&) {
        contents = sceneText;
        return true;
    };
    nuby::GameProject restored;
    nuby::Scene scene;
    std::istringstream input(output.str());
    expect(nuby::GameProjectLoader::load(input, readAsset, restored, scene, error),
           "saved project descriptor can be loaded again");
    expect(restored.displayName == source.displayName
                   && restored.packageIdentifier == source.packageIdentifier
                   && restored.startupScene == source.startupScene,
           "quoted project display names survive descriptor round-trip");

    source.packageIdentifier = "Not A Package";
    std::ostringstream invalidOutput;
    expect(!nuby::GameProjectLoader::save(source, invalidOutput, error),
           "project writer rejects invalid Android package identifiers");
    expect(invalidOutput.str().empty(), "invalid project metadata writes no partial descriptor");
}
}  // namespace

int main() {
    testProjectLoadsManifestAndStartupSceneTransactionally();
    testUnsafePathsAndInvalidMetadataAreRejectedBeforeAssetAccess();
    testMissingOrMalformedStartupSceneLeavesExistingStateUntouched();
    testProjectDescriptorRoundTripAndWriterValidation();
    if (failures != 0) {
        std::cerr << failures << " game project test(s) failed\n";
        return EXIT_FAILURE;
    }
    std::cout << "All game project tests passed\n";
    return EXIT_SUCCESS;
}
