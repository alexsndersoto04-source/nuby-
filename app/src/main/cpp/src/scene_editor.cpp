#include "nuby/scene_editor.hpp"

#include "nuby/scene_io.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <limits>
#include <sstream>
#include <utility>

namespace nuby {
namespace {
constexpr const char* kStartupScenePath = "Scenes/main.nuby";
constexpr float kMaximumTransformValue = 1000.0f;
constexpr float kTwoPi = 6.2831853071795864769f;

bool finiteVector(float x, float y, float z) {
    return std::isfinite(x) && std::isfinite(y) && std::isfinite(z);
}

bool isRenderablePrimitive(Primitive primitive) {
    return primitive == Primitive::Sphere || primitive == Primitive::Torus
            || primitive == Primitive::ImportedMesh;
}
}  // namespace

bool SceneEditorSession::createDefaultProject(std::string& error) {
    Scene initialScene;
    Entity& camera = initialScene.create("Main Camera", Primitive::None);
    camera.visible = false;
    camera.camera.enabled = true;
    camera.orbitCamera.enabled = true;
    camera.orbitCamera.target = {0.0f, -0.35f, 0.0f};
    initialScene.setActiveCamera(camera.id);

    Entity& ground = initialScene.create("Ground Grid", Primitive::Ground);
    ground.material.color = {0.02f, 0.04f, 0.08f};
    ground.material.proceduralGrid = true;

    std::ostringstream sceneOutput;
    if (!SceneSerializer::save(initialScene, sceneOutput, error)) return false;
    GameProject project{"Mi proyecto", "com.nuby.userproject", kStartupScenePath};
    std::ostringstream descriptorOutput;
    if (!GameProjectLoader::save(project, descriptorOutput, error)) return false;
    return loadProjectText(descriptorOutput.str(), sceneOutput.str(), error);
}

bool SceneEditorSession::loadProjectText(const std::string& descriptorText,
                                         const std::string& startupSceneText,
                                         std::string& error) {
    std::istringstream descriptor(descriptorText);
    const ProjectAssetReader readAsset = [&startupSceneText](
            const std::string& path, std::string& contents, std::string& readError) {
        if (path != kStartupScenePath) {
            readError = "The editor project references an unavailable startup asset";
            return false;
        }
        contents = startupSceneText;
        return true;
    };
    GameProjectRuntime candidate;
    if (!candidate.load(descriptor, readAsset, error)) return false;
    runtime_ = std::move(candidate);
    selectedEntityId_ = 0;
    return true;
}

bool SceneEditorSession::saveProjectText(std::string& descriptorText,
                                         std::string& startupSceneText,
                                         std::string& error) const {
    if (!runtime_.loaded() || runtime_.scene() == nullptr) {
        error = "No editor project is open";
        return false;
    }
    std::ostringstream descriptor;
    if (!runtime_.saveDescriptor(descriptor, error)) return false;
    std::ostringstream sceneOutput;
    if (!SceneSerializer::save(*runtime_.scene(), sceneOutput, error)) return false;
    descriptorText = descriptor.str();
    startupSceneText = sceneOutput.str();
    return true;
}

void SceneEditorSession::close() {
    runtime_.unload();
    selectedEntityId_ = 0;
}

bool SceneEditorSession::update(float deltaSeconds) {
    return runtime_.update(deltaSeconds);
}

bool SceneEditorSession::isEditable(const Entity& entity) const {
    return isRenderablePrimitive(entity.primitive) && entity.visible;
}

const Entity* SceneEditorSession::selectedEntity() const {
    const Scene* const currentScene = runtime_.scene();
    if (currentScene == nullptr || selectedEntityId_ == 0U) return nullptr;
    const Entity* const entity = currentScene->find(selectedEntityId_);
    return entity != nullptr && isEditable(*entity) ? entity : nullptr;
}

Entity* SceneEditorSession::selectedEntityMutable() {
    Scene* const currentScene = runtime_.scene();
    if (currentScene == nullptr || selectedEntityId_ == 0U) return nullptr;
    Entity* const entity = currentScene->find(selectedEntityId_);
    return entity != nullptr && isEditable(*entity) ? entity : nullptr;
}

bool SceneEditorSession::selectEntity(std::uint32_t entityId) {
    Scene* const currentScene = runtime_.scene();
    if (currentScene == nullptr) return false;
    const Entity* const entity = currentScene->find(entityId);
    if (entity == nullptr || !isEditable(*entity)) return false;
    selectedEntityId_ = entityId;
    return true;
}

std::string SceneEditorSession::selectionStatus() const {
    if (!runtime_.loaded()) return "No project";
    const Entity* const entity = selectedEntity();
    return entity == nullptr ? "Nothing selected" : entity->name;
}

bool SceneEditorSession::addPrimitive(Primitive primitive, std::string& error) {
    if (!runtime_.loaded() || (primitive != Primitive::Sphere && primitive != Primitive::Torus)) {
        error = "The editor can add only spheres and torus meshes right now";
        return false;
    }
    Scene* const currentScene = runtime_.scene();
    if (currentScene == nullptr || currentScene->entities().size() >= 4096U) {
        error = "The scene is unavailable or has reached the entity limit";
        return false;
    }
    std::size_t ordinal = 0;
    for (const Entity& existing : currentScene->entities()) {
        if (isEditable(existing)) ++ordinal;
    }
    Entity& entity = currentScene->create(
            primitive == Primitive::Sphere ? "Sphere" : "Torus", primitive);
    constexpr std::array<float, 3> columnOffsets{{0.0f, -1.2f, 1.2f}};
    entity.transform.position = {
            columnOffsets[ordinal % columnOffsets.size()],
            primitive == Primitive::Sphere ? -0.68f : -0.78f,
            -static_cast<float>(ordinal / columnOffsets.size()) * 1.2f};
    const float size = primitive == Primitive::Sphere ? 0.48f : 0.38f;
    entity.transform.scale = {size, size, size};
    entity.material.color = primitive == Primitive::Sphere
            ? std::array<float, 3>{0.12f, 0.68f, 0.82f}
            : std::array<float, 3>{0.88f, 0.48f, 0.18f};
    entity.material.metallic = 0.18f;
    entity.material.roughness = 0.42f;
    if (primitive == Primitive::Sphere) {
        entity.sphereCollider.enabled = true;
        entity.sphereCollider.radius = 1.0f;
    } else {
        entity.boxCollider.enabled = true;
        entity.boxCollider.halfExtents = {1.05f, 0.08f, 1.05f};
    }
    selectedEntityId_ = entity.id;
    error.clear();
    return true;
}

bool SceneEditorSession::selectNext(int direction) {
    Scene* const currentScene = runtime_.scene();
    if (currentScene == nullptr || direction == 0) return false;
    std::vector<std::uint32_t> selectable;
    selectable.reserve(currentScene->entities().size());
    for (const Entity& entity : currentScene->entities()) {
        if (isEditable(entity)) selectable.push_back(entity.id);
    }
    if (selectable.empty()) {
        selectedEntityId_ = 0;
        return false;
    }
    const std::size_t count = selectable.size();
    const auto current = std::find(selectable.begin(), selectable.end(), selectedEntityId_);
    std::size_t index = current == selectable.end()
            ? (direction > 0 ? 0U : count - 1U)
            : static_cast<std::size_t>(current - selectable.begin());
    if (current != selectable.end()) {
        index = direction > 0 ? (index + 1U) % count : (index + count - 1U) % count;
    }
    selectedEntityId_ = selectable[index];
    return true;
}

bool SceneEditorSession::moveSelected(float x, float y, float z) {
    Entity* const entity = selectedEntityMutable();
    if (entity == nullptr || !finiteVector(x, y, z)) return false;
    std::array<float, 3> candidate{};
    for (std::size_t axis = 0; axis < 3U; ++axis) {
        const float delta = axis == 0U ? x : axis == 1U ? y : z;
        candidate[axis] = entity->transform.position[axis] + delta;
        if (!std::isfinite(candidate[axis])
                || std::fabs(candidate[axis]) > kMaximumTransformValue) return false;
    }
    entity->transform.position = candidate;
    return true;
}

bool SceneEditorSession::rotateSelected(float x, float y, float z) {
    Entity* const entity = selectedEntityMutable();
    if (entity == nullptr || !finiteVector(x, y, z)) return false;
    std::array<float, 3> candidate{};
    for (std::size_t axis = 0; axis < 3U; ++axis) {
        const float delta = axis == 0U ? x : axis == 1U ? y : z;
        const float value = entity->transform.rotation[axis] + delta;
        if (!std::isfinite(value)) return false;
        candidate[axis] = std::remainder(value, kTwoPi);
    }
    entity->transform.rotation = candidate;
    return true;
}

bool SceneEditorSession::scaleSelected(float factor) {
    Entity* const entity = selectedEntityMutable();
    if (entity == nullptr || !std::isfinite(factor) || factor <= 0.0f) return false;
    for (float& scale : entity->transform.scale) {
        const float value = scale * factor;
        if (!std::isfinite(value) || value < 0.05f || value > 50.0f) return false;
    }
    for (float& scale : entity->transform.scale) scale *= factor;
    return true;
}

bool SceneEditorSession::cycleSelectedColor() {
    Entity* const entity = selectedEntityMutable();
    if (entity == nullptr) return false;
    constexpr std::array<std::array<float, 3>, 6> palette{{
            {{0.12f, 0.68f, 0.82f}}, {{0.88f, 0.48f, 0.18f}},
            {{0.35f, 0.76f, 0.34f}}, {{0.72f, 0.34f, 0.78f}},
            {{0.90f, 0.82f, 0.28f}}, {{0.84f, 0.88f, 0.94f}}
    }};
    std::size_t closest = 0;
    float closestDistance = std::numeric_limits<float>::max();
    for (std::size_t index = 0; index < palette.size(); ++index) {
        float distance = 0.0f;
        for (std::size_t channel = 0; channel < 3U; ++channel) {
            const float difference = entity->material.color[channel] - palette[index][channel];
            distance += difference * difference;
        }
        if (distance < closestDistance) {
            closestDistance = distance;
            closest = index;
        }
    }
    entity->material.color = palette[(closest + 1U) % palette.size()];
    return true;
}

bool SceneEditorSession::duplicateSelected(std::string& error) {
    const Entity* const source = selectedEntity();
    Scene* const currentScene = runtime_.scene();
    if (source == nullptr || currentScene == nullptr || currentScene->entities().size() >= 4096U) {
        error = "No editable object is selected or the scene is full";
        return false;
    }
    const std::string name = source->name + " Copy";
    const Primitive primitive = source->primitive;
    const Transform transform = source->transform;
    const Material material = source->material;
    const SphereColliderComponent sphereCollider = source->sphereCollider;
    const BoxColliderComponent boxCollider = source->boxCollider;
    Entity& copy = currentScene->create(name, primitive);
    copy.transform = transform;
    copy.transform.position[0] += 0.35f;
    copy.material = material;
    copy.sphereCollider = sphereCollider;
    copy.boxCollider = boxCollider;
    selectedEntityId_ = copy.id;
    error.clear();
    return true;
}

bool SceneEditorSession::deleteSelected() {
    Scene* const currentScene = runtime_.scene();
    if (currentScene == nullptr || selectedEntity() == nullptr
            || !currentScene->destroyEntity(selectedEntityId_)) return false;
    selectedEntityId_ = 0;
    selectNext(1);
    return true;
}

}  // namespace nuby
