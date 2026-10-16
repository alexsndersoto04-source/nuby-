#include "nuby/scene.hpp"

#include <algorithm>
#include <cmath>
#include <utility>

namespace nuby {
namespace {
constexpr float kTau = 6.28318530717958647692f;
using Mat4 = std::array<float, 16>;

Mat4 identity() {
    Mat4 result{};
    result[0] = result[5] = result[10] = result[15] = 1.0f;
    return result;
}

Mat4 multiply(const Mat4& a, const Mat4& b) {
    Mat4 result{};
    for (int column = 0; column < 4; ++column) {
        for (int row = 0; row < 4; ++row) {
            for (int k = 0; k < 4; ++k) {
                result[column * 4 + row] += a[k * 4 + row] * b[column * 4 + k];
            }
        }
    }
    return result;
}

Mat4 localMatrix(const Transform& transform) {
    const float cx = std::cos(transform.rotation[0]);
    const float sx = std::sin(transform.rotation[0]);
    const float cy = std::cos(transform.rotation[1]);
    const float sy = std::sin(transform.rotation[1]);
    const float cz = std::cos(transform.rotation[2]);
    const float sz = std::sin(transform.rotation[2]);

    Mat4 rotationX = identity();
    rotationX[5] = cx;
    rotationX[9] = -sx;
    rotationX[6] = sx;
    rotationX[10] = cx;

    Mat4 rotationY = identity();
    rotationY[0] = cy;
    rotationY[8] = sy;
    rotationY[2] = -sy;
    rotationY[10] = cy;

    Mat4 rotationZ = identity();
    rotationZ[0] = cz;
    rotationZ[4] = -sz;
    rotationZ[1] = sz;
    rotationZ[5] = cz;

    Mat4 translation{};
    translation[0] = translation[5] = translation[10] = translation[15] = 1.0f;
    translation[12] = transform.position[0];
    translation[13] = transform.position[1];
    translation[14] = transform.position[2];

    Mat4 scale{};
    scale[0] = transform.scale[0];
    scale[5] = transform.scale[1];
    scale[10] = transform.scale[2];
    scale[15] = 1.0f;

    const Mat4 rotation = multiply(rotationY, multiply(rotationX, rotationZ));
    return multiply(translation, multiply(rotation, scale));
}
}  // namespace

Entity& Scene::create(std::string name, Primitive primitive) {
    entities_.push_back(Entity{});
    Entity& entity = entities_.back();
    entity.id = nextId_++;
    entity.name = std::move(name);
    entity.primitive = primitive;
    return entity;
}

Entity* Scene::find(std::uint32_t id) {
    const auto found = std::find_if(entities_.begin(), entities_.end(),
            [id](const Entity& entity) { return entity.id == id; });
    return found == entities_.end() ? nullptr : &*found;
}

const Entity* Scene::find(std::uint32_t id) const {
    const auto found = std::find_if(entities_.begin(), entities_.end(),
            [id](const Entity& entity) { return entity.id == id; });
    return found == entities_.end() ? nullptr : &*found;
}

bool Scene::setParent(std::uint32_t childId, std::uint32_t parentId) {
    Entity* child = find(childId);
    if (child == nullptr) return false;
    if (parentId == 0) {
        child->parentId = 0;
        return true;
    }
    if (childId == parentId || find(parentId) == nullptr) return false;

    const Entity* ancestor = find(parentId);
    for (std::size_t depth = 0; ancestor != nullptr && depth < entities_.size(); ++depth) {
        if (ancestor->id == childId) return false;
        if (ancestor->parentId == 0) {
            child->parentId = parentId;
            return true;
        }
        ancestor = find(ancestor->parentId);
    }
    return false;
}

bool Scene::setActiveCamera(std::uint32_t id) {
    const Entity* candidate = find(id);
    if (candidate == nullptr || !candidate->camera.enabled) return false;
    activeCameraId_ = id;
    return true;
}

Entity* Scene::activeCamera() {
    Entity* candidate = find(activeCameraId_);
    return candidate != nullptr && candidate->camera.enabled ? candidate : nullptr;
}

const Entity* Scene::activeCamera() const {
    const Entity* candidate = find(activeCameraId_);
    return candidate != nullptr && candidate->camera.enabled ? candidate : nullptr;
}

bool Scene::orbitActiveCamera(float deltaX, float deltaY) {
    Entity* camera = activeCamera();
    if (camera == nullptr || !camera->orbitCamera.enabled
            || !std::isfinite(deltaX) || !std::isfinite(deltaY)) {
        return false;
    }

    OrbitCameraController& controller = camera->orbitCamera;
    controller.yaw = std::remainder(controller.yaw + deltaX * controller.yawPerPixel, kTau);
    const float minimumPitch = std::min(controller.minimumPitch, controller.maximumPitch);
    const float maximumPitch = std::max(controller.minimumPitch, controller.maximumPitch);
    controller.pitch = std::clamp(controller.pitch + deltaY * controller.pitchPerPixel,
                                   minimumPitch, maximumPitch);
    return true;
}

bool Scene::zoomActiveCamera(float distanceFactor) {
    Entity* camera = activeCamera();
    if (camera == nullptr || !camera->orbitCamera.enabled
            || !std::isfinite(distanceFactor) || distanceFactor <= 0.0f) {
        return false;
    }

    OrbitCameraController& controller = camera->orbitCamera;
    const float minimumDistance = std::max(0.1f,
            std::min(controller.minimumDistance, controller.maximumDistance));
    const float maximumDistance = std::max(minimumDistance,
            std::max(controller.minimumDistance, controller.maximumDistance));
    controller.distance = std::clamp(controller.distance * distanceFactor,
                                      minimumDistance, maximumDistance);
    return true;
}

std::array<float, 16> Scene::worldMatrix(std::uint32_t id) const {
    const Entity* current = find(id);
    if (current == nullptr) return identity();

    Mat4 result = identity();
    for (std::size_t depth = 0; current != nullptr && depth < entities_.size(); ++depth) {
        result = multiply(localMatrix(current->transform), result);
        if (current->parentId == 0) break;
        current = find(current->parentId);
    }
    return result;
}

void Scene::clear() {
    entities_.clear();
    nextId_ = 1;
    activeCameraId_ = 0;
    elapsedSeconds_ = 0.0f;
}

void Scene::update(float deltaSeconds) {
    const float dt = std::clamp(deltaSeconds, 0.0f, 0.05f);
    elapsedSeconds_ += dt;

    for (Entity& entity : entities_) {
        if (entity.spin.radiansPerSecond[0] != 0.0f
                || entity.spin.radiansPerSecond[1] != 0.0f
                || entity.spin.radiansPerSecond[2] != 0.0f) {
            for (std::size_t axis = 0; axis < 3; ++axis) {
                entity.transform.rotation[axis] += entity.spin.radiansPerSecond[axis] * dt;
            }
        }

        if (entity.orbit.enabled) {
            const float angle = entity.orbit.phase + elapsedSeconds_ * entity.orbit.angularSpeed;
            entity.transform.position[0] = entity.orbit.center[0]
                    + std::cos(angle) * entity.orbit.radius;
            entity.transform.position[1] = entity.orbit.center[1]
                    + std::sin(angle * entity.orbit.verticalFrequency)
                      * entity.orbit.verticalAmplitude;
            entity.transform.position[2] = entity.orbit.center[2]
                    + std::sin(angle) * entity.orbit.radius;
        }

        if (entity.pulse.enabled) {
            const float wave = std::sin(elapsedSeconds_ * entity.pulse.frequency * kTau
                                        + entity.pulse.phase);
            const float multiplier = 1.0f + entity.pulse.amplitude * wave;
            for (std::size_t axis = 0; axis < 3; ++axis) {
                entity.transform.scale[axis] = std::max(
                        0.001f, multiplier * entity.pulse.baseScale[axis]);
            }
        }
    }
}

}  // namespace nuby
