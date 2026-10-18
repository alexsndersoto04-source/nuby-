#include "nuby/scene_editor.hpp"

#include <cmath>
#include <cstdlib>
#include <iostream>
#include <limits>
#include <string>

namespace {
int failures = 0;

void expect(bool condition, const char* message) {
    if (condition) return;
    std::cerr << "FAIL: " << message << '\n';
    ++failures;
}

void testCreateEditSaveAndReloadProject() {
    nuby::SceneEditorSession editor;
    std::string error;
    expect(editor.createDefaultProject(error) && editor.active(),
           "the editor creates a valid standalone starter project");
    expect(editor.scene() != nullptr && editor.scene()->entities().size() == 2U
                   && editor.scene()->activeCamera() != nullptr,
           "starter scene contains a camera and an engine ground grid");
    expect(editor.addPrimitive(nuby::Primitive::Sphere, error),
           "the editor adds a sphere to the startup scene");
    const nuby::Entity* selected = editor.selectedEntity();
    expect(selected != nullptr && selected->name == "Sphere",
           "newly created object is selected for visual editing");
    const std::uint32_t sphereId = selected == nullptr ? 0U : selected->id;
    expect(selected != nullptr && selected->sphereCollider.enabled,
           "new spheres receive a collider used for on-screen selection");
    nuby::RaycastHit hit;
    expect(editor.scene()->raycast({0.0f, -0.68f, 5.0f}, {0.0f, 0.0f, -1.0f},
                                   10.0f, hit)
                   && hit.entityId == sphereId && editor.selectEntity(hit.entityId),
           "a tapped-object ray can resolve and select its scene entity");
    expect(editor.moveSelected(0.5f, 0.25f, -0.75f)
                   && editor.rotateSelected(0.0f, 0.5f, 0.0f)
                   && editor.scaleSelected(1.5f)
                   && editor.cycleSelectedColor(),
           "object translation, rotation, scale and material palette editing work");
    selected = editor.selectedEntity();
    expect(selected != nullptr && selected->id == sphereId
                   && std::fabs(selected->transform.position[0] - 0.5f) < 1.0e-6f
                   && std::fabs(selected->transform.position[1] + 0.43f) < 1.0e-6f
                   && std::fabs(selected->transform.position[2] + 0.75f) < 1.0e-6f
                   && std::fabs(selected->transform.scale[0] - 0.72f) < 1.0e-6f,
           "scene editor applies transforms to the selected object");
    expect(!editor.moveSelected(std::numeric_limits<float>::infinity(), 0.0f, 0.0f)
                   && !editor.scaleSelected(0.0f)
                   && !editor.scaleSelected(1000.0f),
           "non-finite and out-of-range transform edits are rejected without corrupting the scene");
    expect(editor.duplicateSelected(error) && editor.selectedEntity() != nullptr
                   && editor.selectedEntity()->name == "Sphere Copy"
                   && editor.selectedEntity()->sphereCollider.enabled,
           "duplicates preserve material, transform and selectable collider state");
    expect(editor.selectNext(-1) && editor.selectedEntity() != nullptr
                   && editor.selectedEntity()->id == sphereId,
           "selection navigation wraps deterministically through editable objects");

    std::string descriptor;
    std::string startupScene;
    expect(editor.saveProjectText(descriptor, startupScene, error)
                   && descriptor.find("NUBY_PROJECT 1") == 0U
                   && startupScene.find("NUBY_SCENE 19") == 0U,
           "the project descriptor and versioned scene serialize separately for storage");
    nuby::SceneEditorSession reopened;
    expect(reopened.loadProjectText(descriptor, startupScene, error)
                   && reopened.active() && reopened.scene()->entities().size() == 4U,
           "a saved project reopens with all user-created objects");
    expect(reopened.selectNext(1) && reopened.selectedEntity() != nullptr,
           "a reopened project can resume object editing");
    expect(reopened.deleteSelected() && reopened.scene()->entities().size() == 3U,
           "the selected object can be removed from the scene");
}

void testRejectedProjectLoadPreservesOpenSession() {
    nuby::SceneEditorSession editor;
    std::string error;
    expect(editor.createDefaultProject(error) && editor.addPrimitive(nuby::Primitive::Torus, error),
           "an editor project is ready before testing rejected replacement");
    const std::size_t previousEntityCount = editor.scene()->entities().size();
    expect(!editor.loadProjectText("NUBY_PROJECT 1\n", "invalid scene", error)
                   && editor.active()
                   && editor.scene()->entities().size() == previousEntityCount,
           "a malformed replacement project does not destroy the current editor session");
    expect(!editor.addPrimitive(nuby::Primitive::Ground, error),
           "the editor refuses to create a second ground or unsupported object type");
    editor.close();
    std::string descriptor;
    std::string startupScene;
    expect(!editor.active() && editor.scene() == nullptr
                   && !editor.saveProjectText(descriptor, startupScene, error),
           "closing an editor session releases its project state");
}

}  // namespace

int main() {
    testCreateEditSaveAndReloadProject();
    testRejectedProjectLoadPreservesOpenSession();
    if (failures != 0) {
        std::cerr << failures << " scene editor test(s) failed\n";
        return EXIT_FAILURE;
    }
    std::cout << "All scene editor tests passed\n";
    return EXIT_SUCCESS;
}
