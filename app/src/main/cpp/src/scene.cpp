#include "nuby/scene.hpp"

#include <algorithm>
#include <cmath>
#include <utility>

namespace nuby {
namespace {
constexpr float kTau = 6.28318530717958647692f;
}

Entity& Scene::create(std::string name, Primitive primitive) {
    entities_.push_back(Entity{});
    Entity& entity = entities_.back();
    entity.id = nextId_++;
    entity.name = std::move(name);
    entity.primitive = primitive;
    return entity;
}

void Scene::clear() {
    entities_.clear();
    nextId_ = 1;
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
