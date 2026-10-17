#include "nuby/scene.hpp"

#include <algorithm>
#include <cmath>
#include <unordered_set>
#include <utility>

namespace nuby {
namespace {
constexpr float kTau = 6.28318530717958647692f;
using Mat4 = std::array<float, 16>;

constexpr std::size_t kMaximumQueuedPhysicsEvents = 1024;

std::uint64_t triggerPairKey(std::uint32_t first, std::uint32_t second) {
    if (first > second) std::swap(first, second);
    return (static_cast<std::uint64_t>(first) << 32U) | second;
}

using Vec3 = std::array<float, 3>;
using Quaternion = std::array<float, 4>;  // x, y, z, w

Vec3 cross(const Vec3& a, const Vec3& b) {
    return {a[1] * b[2] - a[2] * b[1],
            a[2] * b[0] - a[0] * b[2],
            a[0] * b[1] - a[1] * b[0]};
}

Quaternion multiply(const Quaternion& a, const Quaternion& b) {
    return {a[3] * b[0] + a[0] * b[3] + a[1] * b[2] - a[2] * b[1],
            a[3] * b[1] - a[0] * b[2] + a[1] * b[3] + a[2] * b[0],
            a[3] * b[2] + a[0] * b[1] - a[1] * b[0] + a[2] * b[3],
            a[3] * b[3] - a[0] * b[0] - a[1] * b[1] - a[2] * b[2]};
}

Quaternion quaternionFromEuler(const Vec3& euler) {
    const float hx = euler[0] * 0.5f;
    const float hy = euler[1] * 0.5f;
    const float hz = euler[2] * 0.5f;
    const Quaternion qx{std::sin(hx), 0.0f, 0.0f, std::cos(hx)};
    const Quaternion qy{0.0f, std::sin(hy), 0.0f, std::cos(hy)};
    const Quaternion qz{0.0f, 0.0f, std::sin(hz), std::cos(hz)};
    Quaternion result = multiply(qy, multiply(qx, qz));
    const float norm = std::sqrt(result[0] * result[0] + result[1] * result[1]
            + result[2] * result[2] + result[3] * result[3]);
    if (!std::isfinite(norm) || norm <= 1.0e-8f) return {0.0f, 0.0f, 0.0f, 1.0f};
    for (float& component : result) component /= norm;
    return result;
}

Vec3 eulerFromQuaternion(const Quaternion& q) {
    const float x = q[0];
    const float y = q[1];
    const float z = q[2];
    const float w = q[3];
    const float r02 = 2.0f * (x * z + y * w);
    const float r12 = 2.0f * (y * z - x * w);
    const float r22 = 1.0f - 2.0f * (x * x + y * y);
    const float r10 = 2.0f * (x * y + z * w);
    const float r11 = 1.0f - 2.0f * (x * x + z * z);
    const float pitch = std::asin(std::clamp(-r12, -1.0f, 1.0f));
    const float cosinePitch = std::cos(pitch);
    if (std::fabs(cosinePitch) > 1.0e-5f) {
        return {pitch, std::atan2(r02, r22), std::atan2(r10, r11)};
    }
    return {pitch, std::atan2(2.0f * (x * y - z * w),
                              1.0f - 2.0f * (y * y + z * z)), 0.0f};
}

void integrateRotation(Transform& transform, const Vec3& angularVelocity, float dt) {
    const float speed = std::sqrt(angularVelocity[0] * angularVelocity[0]
            + angularVelocity[1] * angularVelocity[1]
            + angularVelocity[2] * angularVelocity[2]);
    if (!std::isfinite(speed) || speed <= 1.0e-8f || !std::isfinite(dt)) return;
    const float halfAngle = speed * dt * 0.5f;
    const float scale = std::sin(halfAngle) / speed;
    const Quaternion delta{angularVelocity[0] * scale, angularVelocity[1] * scale,
                           angularVelocity[2] * scale, std::cos(halfAngle)};
    transform.rotation = eulerFromQuaternion(multiply(delta,
            quaternionFromEuler(transform.rotation)));
}

float inverseSphereInertia(float inverseMass, float radius) {
    if (inverseMass <= 0.0f || !std::isfinite(radius) || radius <= 1.0e-6f) return 0.0f;
    // A solid sphere has I = 2/5 m r^2.
    return 2.5f * inverseMass / (radius * radius);
}

void applyAngularImpulse(RigidbodyComponent& body, float inverseInertia,
                         const Vec3& arm, const Vec3& impulse) {
    if (inverseInertia <= 0.0f) return;
    const Vec3 torque = cross(arm, impulse);
    for (std::size_t axis = 0; axis < 3; ++axis) {
        body.angularVelocity[axis] += torque[axis] * inverseInertia;
    }
}

void queuePhysicsEvent(std::vector<PhysicsEvent>& events, PhysicsEventType type,
                       std::uint32_t first, std::uint32_t second) {
    if (events.size() >= kMaximumQueuedPhysicsEvents) events.erase(events.begin());
    if (first > second) std::swap(first, second);
    events.push_back({type, first, second});
}

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

void updateAnimation(AnimationComponent& animation, Transform& transform, float deltaSeconds) {
    if (!animation.enabled) return;

    const float authoredDuration = std::isfinite(animation.durationSeconds)
            ? std::max(animation.durationSeconds, 0.0f) : 0.0f;
    const float duration = std::max({authoredDuration, animation.position.durationSeconds(),
            animation.rotation.durationSeconds(), animation.scale.durationSeconds()});
    if (animation.playing) {
        const float current = std::isfinite(animation.timeSeconds) ? animation.timeSeconds : 0.0f;
        const float rate = std::isfinite(animation.playbackRate) ? animation.playbackRate : 0.0f;
        const float next = current + deltaSeconds * rate;
        if (duration <= 0.0f) {
            animation.timeSeconds = 0.0f;
            animation.playing = false;
        } else if (animation.looping) {
            animation.timeSeconds = std::fmod(next, duration);
            if (animation.timeSeconds < 0.0f) animation.timeSeconds += duration;
        } else if (next >= duration) {
            animation.timeSeconds = duration;
            animation.playing = false;
        } else if (next <= 0.0f && rate < 0.0f) {
            animation.timeSeconds = 0.0f;
            animation.playing = false;
        } else {
            animation.timeSeconds = std::clamp(next, 0.0f, duration);
        }
    }

    transform.position = animation.position.sample(animation.timeSeconds, transform.position);
    transform.rotation = animation.rotation.sample(animation.timeSeconds, transform.rotation);
    transform.scale = animation.scale.sample(animation.timeSeconds, transform.scale);
}
}  // namespace

bool Vector3Track::addKeyframe(float timeSeconds, const std::array<float, 3>& value) {
    if (!std::isfinite(timeSeconds) || timeSeconds < 0.0f
            || !std::isfinite(value[0]) || !std::isfinite(value[1])
            || !std::isfinite(value[2])) {
        return false;
    }

    const auto position = std::lower_bound(keyframes.begin(), keyframes.end(), timeSeconds,
            [](const Vector3Keyframe& keyframe, float time) {
                return keyframe.timeSeconds < time;
            });
    if (position != keyframes.end() && position->timeSeconds == timeSeconds) {
        position->value = value;
    } else {
        keyframes.insert(position, Vector3Keyframe{timeSeconds, value});
    }
    return true;
}

std::array<float, 3> Vector3Track::sample(float timeSeconds,
                                          const std::array<float, 3>& fallback) const {
    if (keyframes.empty()) return fallback;
    if (!std::isfinite(timeSeconds)) timeSeconds = 0.0f;
    if (timeSeconds <= keyframes.front().timeSeconds) return keyframes.front().value;
    if (timeSeconds >= keyframes.back().timeSeconds) return keyframes.back().value;

    const auto next = std::upper_bound(keyframes.begin(), keyframes.end(), timeSeconds,
            [](float time, const Vector3Keyframe& keyframe) {
                return time < keyframe.timeSeconds;
            });
    const Vector3Keyframe& right = *next;
    const Vector3Keyframe& left = *(next - 1);
    const float span = right.timeSeconds - left.timeSeconds;
    const float blend = span > 0.0f ? (timeSeconds - left.timeSeconds) / span : 0.0f;
    return {left.value[0] + (right.value[0] - left.value[0]) * blend,
            left.value[1] + (right.value[1] - left.value[1]) * blend,
            left.value[2] + (right.value[2] - left.value[2]) * blend};
}

float Vector3Track::durationSeconds() const {
    return keyframes.empty() ? 0.0f : keyframes.back().timeSeconds;
}

Entity& Scene::create(std::string name, Primitive primitive) {
    entities_.push_back(Entity{});
    Entity& entity = entities_.back();
    entity.id = nextId_++;
    entity.name = std::move(name);
    entity.primitive = primitive;
    return entity;
}

bool Scene::destroyEntity(std::uint32_t id) {
    if (find(id) == nullptr) return false;

    std::unordered_set<std::uint32_t> removedIds{id};
    bool addedChild = true;
    while (addedChild) {
        addedChild = false;
        for (const Entity& entity : entities_) {
            if (removedIds.find(entity.parentId) != removedIds.end()
                    && removedIds.insert(entity.id).second) {
                addedChild = true;
            }
        }
    }

    for (auto pair = activeTriggerPairs_.begin(); pair != activeTriggerPairs_.end();) {
        const std::uint32_t first = static_cast<std::uint32_t>(*pair >> 32U);
        const std::uint32_t second = static_cast<std::uint32_t>(*pair);
        if (removedIds.count(first) != 0 || removedIds.count(second) != 0) {
            queuePhysicsEvent(physicsEvents_, PhysicsEventType::TriggerExit, first, second);
            pair = activeTriggerPairs_.erase(pair);
        } else {
            ++pair;
        }
    }

    entities_.erase(std::remove_if(entities_.begin(), entities_.end(),
            [&removedIds](const Entity& entity) {
                return removedIds.find(entity.id) != removedIds.end();
            }), entities_.end());
    if (removedIds.find(activeCameraId_) != removedIds.end()) activeCameraId_ = 0;
    return true;
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

bool Scene::setCharacterMoveInput(std::uint32_t id, float horizontal, float forward) {
    Entity* entity = find(id);
    if (entity == nullptr || !entity->characterController.enabled
            || !std::isfinite(horizontal) || !std::isfinite(forward)) {
        return false;
    }
    const float magnitude = std::hypot(horizontal, forward);
    if (magnitude > 1.0f) {
        horizontal /= magnitude;
        forward /= magnitude;
    }
    entity->characterController.moveInput = {horizontal, forward};
    return true;
}

bool Scene::jumpCharacter(std::uint32_t id) {
    Entity* entity = find(id);
    if (entity == nullptr) return false;
    CharacterControllerComponent& controller = entity->characterController;
    if (!controller.enabled || !controller.grounded || !entity->rigidbody.enabled
            || !std::isfinite(controller.jumpSpeed) || controller.jumpSpeed <= 0.0f) {
        return false;
    }
    entity->rigidbody.velocity[1] = controller.jumpSpeed;
    controller.grounded = false;
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

void Scene::collectPointLights(std::size_t maximum,
                               std::vector<PointLightData>& output) const {
    output.clear();
    if (maximum == 0) return;
    output.reserve(std::min(maximum, entities_.size()));

    for (const Entity& entity : entities_) {
        const PointLightComponent& light = entity.pointLight;
        if (!light.enabled || !std::isfinite(light.intensity) || light.intensity <= 0.0f
                || !std::isfinite(light.range) || light.range <= 0.0f
                || !std::isfinite(light.color[0]) || !std::isfinite(light.color[1])
                || !std::isfinite(light.color[2])) {
            continue;
        }
        const Mat4 world = worldMatrix(entity.id);
        if (!std::isfinite(world[12]) || !std::isfinite(world[13])
                || !std::isfinite(world[14])) {
            continue;
        }
        const std::array<float, 3> color{std::max(light.color[0], 0.0f),
                                         std::max(light.color[1], 0.0f),
                                         std::max(light.color[2], 0.0f)};
        output.push_back({{world[12], world[13], world[14]}, color,
                          light.intensity, light.range});
        if (output.size() == maximum) break;
    }
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
    physicsSettings_ = PhysicsSettings{};
    physicsAccumulator_ = 0.0f;
    activeTriggerPairs_.clear();
    triggerPairsScratch_.clear();
    colliderIndicesScratch_.clear();
    physicsEvents_.clear();
}

void Scene::consumePhysicsEvents(std::vector<PhysicsEvent>& output) {
    output.clear();
    output.swap(physicsEvents_);
}

void Scene::simulatePhysics(float deltaSeconds) {
    const float step = physicsSettings_.fixedTimeStep;
    if (!std::isfinite(step) || step <= 0.0f || physicsSettings_.maximumSubsteps == 0) {
        physicsAccumulator_ = 0.0f;
        return;
    }
    if (!std::isfinite(physicsSettings_.gravity[0])
            || !std::isfinite(physicsSettings_.gravity[1])
            || !std::isfinite(physicsSettings_.gravity[2])) {
        physicsAccumulator_ = 0.0f;
        return;
    }
    physicsAccumulator_ += deltaSeconds;
    const unsigned int maximumSubsteps = std::min(physicsSettings_.maximumSubsteps, 16U);
    std::vector<std::size_t>& colliderIndices = colliderIndicesScratch_;
    colliderIndices.clear();
    colliderIndices.reserve(entities_.size());
    std::vector<std::uint64_t>& currentTriggerPairs = triggerPairsScratch_;
    currentTriggerPairs.clear();

    for (unsigned int substep = 0; substep < maximumSubsteps
            && physicsAccumulator_ >= step; ++substep) {
        const float dt = step;
        for (Entity& entity : entities_) {
            RigidbodyComponent& body = entity.rigidbody;
            if (!body.enabled || entity.parentId != 0 || !std::isfinite(body.mass)
                    || body.mass < 1.0e-6f || !std::isfinite(body.linearDamping)
                    || !std::isfinite(body.gravityScale)
                    || !std::isfinite(entity.transform.position[0])
                    || !std::isfinite(entity.transform.position[1])
                    || !std::isfinite(entity.transform.position[2])) {
                continue;
            }
            CharacterControllerComponent& controller = entity.characterController;
            if (controller.enabled) {
                const bool wasGrounded = controller.grounded;
                controller.grounded = false;
                if (std::isfinite(controller.maximumSpeed) && controller.maximumSpeed > 0.0f
                        && std::isfinite(controller.acceleration) && controller.acceleration > 0.0f
                        && std::isfinite(controller.moveInput[0])
                        && std::isfinite(controller.moveInput[1])) {
                    const float airControl = wasGrounded ? 1.0f : 0.4f;
                    const float maximumChange = controller.acceleration * airControl * dt;
                    float inputX = std::clamp(controller.moveInput[0], -1.0f, 1.0f);
                    float inputZ = std::clamp(controller.moveInput[1], -1.0f, 1.0f);
                    const float inputLength = std::hypot(inputX, inputZ);
                    if (inputLength > 1.0f) {
                        inputX /= inputLength;
                        inputZ /= inputLength;
                    }
                    const float horizontal = inputX * controller.maximumSpeed;
                    const float forward = inputZ * controller.maximumSpeed;
                    const float horizontalDelta = horizontal - body.velocity[0];
                    const float forwardDelta = forward - body.velocity[2];
                    body.velocity[0] += std::clamp(horizontalDelta, -maximumChange, maximumChange);
                    body.velocity[2] += std::clamp(forwardDelta, -maximumChange, maximumChange);
                }
            }
            for (std::size_t axis = 0; axis < 3; ++axis) {
                if (!std::isfinite(body.velocity[axis])) body.velocity[axis] = 0.0f;
                body.velocity[axis] += physicsSettings_.gravity[axis] * body.gravityScale * dt;
            }
            const float damping = std::max(body.linearDamping, 0.0f);
            const float dampingFactor = std::exp(-damping * dt);
            for (float& velocity : body.velocity) velocity *= dampingFactor;
            const float angularDampingFactor = std::exp(-std::max(body.angularDamping, 0.0f) * dt);
            for (float& velocity : body.angularVelocity) {
                if (!std::isfinite(velocity)) velocity = 0.0f;
                velocity *= angularDampingFactor;
            }
            for (std::size_t axis = 0; axis < 3; ++axis) {
                entity.transform.position[axis] += body.velocity[axis] * dt;
            }
            integrateRotation(entity.transform, body.angularVelocity, dt);
        }

        if (physicsSettings_.floorEnabled && std::isfinite(physicsSettings_.floorHeight)
                && std::isfinite(physicsSettings_.floorRestitution)
                && std::isfinite(physicsSettings_.floorFriction)) {
            for (Entity& entity : entities_) {
                RigidbodyComponent& body = entity.rigidbody;
                const SphereColliderComponent& collider = entity.sphereCollider;
                if (!body.enabled || !std::isfinite(body.mass) || body.mass < 1.0e-6f
                        || entity.parentId != 0 || !collider.enabled || !std::isfinite(collider.radius)
                        || collider.radius <= 0.0f || !std::isfinite(collider.restitution)
                        || !std::isfinite(collider.friction)
                        || !std::isfinite(entity.transform.scale[0])
                        || !std::isfinite(entity.transform.scale[1])
                        || !std::isfinite(entity.transform.scale[2])) {
                    continue;
                }
                const float scale = std::max({std::fabs(entity.transform.scale[0]),
                        std::fabs(entity.transform.scale[1]),
                        std::fabs(entity.transform.scale[2])});
                const float radius = collider.radius * scale;
                const float penetration = physicsSettings_.floorHeight + radius
                        - entity.transform.position[1];
                if (penetration < -0.0001f || !std::isfinite(penetration)) continue;
                if (penetration > 0.0f) entity.transform.position[1] += penetration;
                const float inverseMass = 1.0f / body.mass;
                const float inverseInertia = inverseSphereInertia(inverseMass, radius);
                const Vec3 contactArm{0.0f, -radius, 0.0f};
                const Vec3 spinVelocity = cross(body.angularVelocity, contactArm);
                const float contactNormalVelocity = body.velocity[1] + spinVelocity[1];
                const float restitution = std::clamp(std::min(collider.restitution,
                        physicsSettings_.floorRestitution), 0.0f, 1.0f);
                float normalImpulse = 0.0f;
                if (contactNormalVelocity < 0.0f) {
                    normalImpulse = -(1.0f + restitution) * contactNormalVelocity / inverseMass;
                    body.velocity[1] += normalImpulse * inverseMass;
                }
                if (entity.characterController.enabled && body.velocity[1] <= 0.0f) {
                    entity.characterController.grounded = true;
                }
                const Vec3 contactVelocity{body.velocity[0] + spinVelocity[0], 0.0f,
                                           body.velocity[2] + spinVelocity[2]};
                const float tangentSpeed = std::hypot(contactVelocity[0], contactVelocity[2]);
                const float friction = std::clamp(std::min(collider.friction,
                        physicsSettings_.floorFriction), 0.0f, 1.0f);
                const float tangentDenominator = inverseMass
                        + radius * radius * inverseInertia;
                if (tangentSpeed > 1.0e-6f && tangentDenominator > 0.0f
                        && normalImpulse > 0.0f) {
                    const float tangentImpulse = std::min(tangentSpeed / tangentDenominator,
                            normalImpulse * friction);
                    const Vec3 impulse{-contactVelocity[0] / tangentSpeed * tangentImpulse,
                                       0.0f,
                                       -contactVelocity[2] / tangentSpeed * tangentImpulse};
                    body.velocity[0] += impulse[0] * inverseMass;
                    body.velocity[2] += impulse[2] * inverseMass;
                    applyAngularImpulse(body, inverseInertia, contactArm, impulse);
                }
            }
        }

        colliderIndices.clear();
        for (std::size_t index = 0; index < entities_.size(); ++index) {
            const Entity& entity = entities_[index];
            const SphereColliderComponent& collider = entity.sphereCollider;
            if (!collider.enabled || entity.parentId != 0
                    || !std::isfinite(collider.radius) || collider.radius <= 0.0f
                    || !std::isfinite(collider.restitution) || !std::isfinite(collider.friction)
                    || !std::isfinite(entity.transform.scale[0])
                    || !std::isfinite(entity.transform.scale[1])
                    || !std::isfinite(entity.transform.scale[2])
                    || !std::isfinite(entity.transform.position[0])
                    || !std::isfinite(entity.transform.position[1])
                    || !std::isfinite(entity.transform.position[2])) continue;
            const float scale = std::max({std::fabs(entity.transform.scale[0]),
                    std::fabs(entity.transform.scale[1]), std::fabs(entity.transform.scale[2])});
            if (std::isfinite(collider.radius * scale) && collider.radius * scale > 0.0f) {
                colliderIndices.push_back(index);
            }
        }

        std::sort(colliderIndices.begin(), colliderIndices.end(), [this](std::size_t left,
                                                                         std::size_t right) {
            const Entity& a = entities_[left];
            const Entity& b = entities_[right];
            const float aScale = std::max({std::fabs(a.transform.scale[0]),
                    std::fabs(a.transform.scale[1]), std::fabs(a.transform.scale[2])});
            const float bScale = std::max({std::fabs(b.transform.scale[0]),
                    std::fabs(b.transform.scale[1]), std::fabs(b.transform.scale[2])});
            return a.transform.position[0] - a.sphereCollider.radius * aScale
                    < b.transform.position[0] - b.sphereCollider.radius * bScale;
        });

        for (std::size_t firstColliderIndex = 0;
                firstColliderIndex < colliderIndices.size(); ++firstColliderIndex) {
            Entity& first = entities_[colliderIndices[firstColliderIndex]];
            const SphereColliderComponent& firstCollider = first.sphereCollider;
            const float firstScale = std::max({std::fabs(first.transform.scale[0]),
                    std::fabs(first.transform.scale[1]), std::fabs(first.transform.scale[2])});
            const float firstRadius = firstCollider.radius * firstScale;
            for (std::size_t secondColliderIndex = firstColliderIndex + 1;
                    secondColliderIndex < colliderIndices.size(); ++secondColliderIndex) {
                Entity& second = entities_[colliderIndices[secondColliderIndex]];
                const SphereColliderComponent& secondCollider = second.sphereCollider;
                const float secondScale = std::max({std::fabs(second.transform.scale[0]),
                        std::fabs(second.transform.scale[1]), std::fabs(second.transform.scale[2])});
                const float secondRadius = secondCollider.radius * secondScale;
                if (second.transform.position[0] - secondRadius
                        > first.transform.position[0] + firstRadius) break;
                float dx = second.transform.position[0] - first.transform.position[0];
                float dy = second.transform.position[1] - first.transform.position[1];
                float dz = second.transform.position[2] - first.transform.position[2];
                const float distanceSquared = dx * dx + dy * dy + dz * dz;
                const float combinedRadius = firstRadius + secondRadius;
                if (!std::isfinite(distanceSquared) || distanceSquared >= combinedRadius * combinedRadius) continue;
                if (firstCollider.trigger || secondCollider.trigger) {
                    currentTriggerPairs.push_back(triggerPairKey(first.id, second.id));
                    continue;
                }
                const float distance = std::sqrt(std::max(distanceSquared, 0.0f));
                if (distance > 1.0e-6f) {
                    dx /= distance;
                    dy /= distance;
                    dz /= distance;
                } else {
                    dx = 1.0f;
                    dy = 0.0f;
                    dz = 0.0f;
                }
                RigidbodyComponent& firstBody = first.rigidbody;
                RigidbodyComponent& secondBody = second.rigidbody;
                const float firstInverseMass = firstBody.enabled && std::isfinite(firstBody.mass)
                        && firstBody.mass >= 1.0e-6f ? 1.0f / firstBody.mass : 0.0f;
                const float secondInverseMass = secondBody.enabled && std::isfinite(secondBody.mass)
                        && secondBody.mass >= 1.0e-6f ? 1.0f / secondBody.mass : 0.0f;
                const float inverseMassSum = firstInverseMass + secondInverseMass;
                if (inverseMassSum <= 0.0f) continue;
                const float penetration = combinedRadius - distance;
                const float correction = std::max(penetration - 0.001f, 0.0f) * 0.8f
                        / inverseMassSum;
                for (std::size_t axis = 0; axis < 3; ++axis) {
                    const float normal = axis == 0 ? dx : axis == 1 ? dy : dz;
                    first.transform.position[axis] -= normal * correction * firstInverseMass;
                    second.transform.position[axis] += normal * correction * secondInverseMass;
                }
                const float firstInverseInertia = inverseSphereInertia(firstInverseMass, firstRadius);
                const float secondInverseInertia = inverseSphereInertia(secondInverseMass, secondRadius);
                const Vec3 firstArm{dx * firstRadius, dy * firstRadius, dz * firstRadius};
                const Vec3 secondArm{-dx * secondRadius, -dy * secondRadius, -dz * secondRadius};
                const Vec3 firstSpinVelocity = firstInverseMass > 0.0f
                        ? cross(firstBody.angularVelocity, firstArm) : Vec3{0.0f, 0.0f, 0.0f};
                const Vec3 secondSpinVelocity = secondInverseMass > 0.0f
                        ? cross(secondBody.angularVelocity, secondArm) : Vec3{0.0f, 0.0f, 0.0f};
                const Vec3 relativeVelocity{
                    secondBody.velocity[0] + secondSpinVelocity[0]
                            - firstBody.velocity[0] - firstSpinVelocity[0],
                    secondBody.velocity[1] + secondSpinVelocity[1]
                            - firstBody.velocity[1] - firstSpinVelocity[1],
                    secondBody.velocity[2] + secondSpinVelocity[2]
                            - firstBody.velocity[2] - firstSpinVelocity[2]};
                const float relativeNormalVelocity = relativeVelocity[0] * dx
                        + relativeVelocity[1] * dy + relativeVelocity[2] * dz;
                if (relativeNormalVelocity >= 0.0f) continue;
                const float restitution = std::clamp(std::min(firstCollider.restitution,
                        secondCollider.restitution), 0.0f, 1.0f);
                const float impulse = -(1.0f + restitution) * relativeNormalVelocity
                        / inverseMassSum;
                const Vec3 normalImpulse{impulse * dx, impulse * dy, impulse * dz};
                if (firstInverseMass > 0.0f) {
                    firstBody.velocity[0] -= normalImpulse[0] * firstInverseMass;
                    firstBody.velocity[1] -= normalImpulse[1] * firstInverseMass;
                    firstBody.velocity[2] -= normalImpulse[2] * firstInverseMass;
                    applyAngularImpulse(firstBody, firstInverseInertia, firstArm,
                            {-normalImpulse[0], -normalImpulse[1], -normalImpulse[2]});
                }
                if (secondInverseMass > 0.0f) {
                    secondBody.velocity[0] += normalImpulse[0] * secondInverseMass;
                    secondBody.velocity[1] += normalImpulse[1] * secondInverseMass;
                    secondBody.velocity[2] += normalImpulse[2] * secondInverseMass;
                    applyAngularImpulse(secondBody, secondInverseInertia, secondArm, normalImpulse);
                }

                const float tangentX = relativeVelocity[0] - relativeNormalVelocity * dx;
                const float tangentY = relativeVelocity[1] - relativeNormalVelocity * dy;
                const float tangentZ = relativeVelocity[2] - relativeNormalVelocity * dz;
                const float tangentSpeed = std::sqrt(tangentX * tangentX + tangentY * tangentY
                                                     + tangentZ * tangentZ);
                const float rotationalDenominator = firstRadius * firstRadius * firstInverseInertia
                        + secondRadius * secondRadius * secondInverseInertia;
                if (tangentSpeed > 1.0e-6f) {
                    const float friction = std::sqrt(std::max(0.0f,
                            firstCollider.friction * secondCollider.friction));
                    const float tangentDenominator = inverseMassSum + rotationalDenominator;
                    const float tangentImpulse = std::min(tangentSpeed / tangentDenominator,
                                                           impulse * friction);
                    const Vec3 frictionImpulse{-tangentX / tangentSpeed * tangentImpulse,
                                               -tangentY / tangentSpeed * tangentImpulse,
                                               -tangentZ / tangentSpeed * tangentImpulse};
                    if (firstInverseMass > 0.0f) {
                        firstBody.velocity[0] -= frictionImpulse[0] * firstInverseMass;
                        firstBody.velocity[1] -= frictionImpulse[1] * firstInverseMass;
                        firstBody.velocity[2] -= frictionImpulse[2] * firstInverseMass;
                        applyAngularImpulse(firstBody, firstInverseInertia, firstArm,
                                {-frictionImpulse[0], -frictionImpulse[1],
                                 -frictionImpulse[2]});
                    }
                    if (secondInverseMass > 0.0f) {
                        secondBody.velocity[0] += frictionImpulse[0] * secondInverseMass;
                        secondBody.velocity[1] += frictionImpulse[1] * secondInverseMass;
                        secondBody.velocity[2] += frictionImpulse[2] * secondInverseMass;
                        applyAngularImpulse(secondBody, secondInverseInertia, secondArm,
                                frictionImpulse);
                    }
                }
            }
        }
        std::sort(currentTriggerPairs.begin(), currentTriggerPairs.end());
        for (const std::uint64_t key : currentTriggerPairs) {
            if (!std::binary_search(activeTriggerPairs_.begin(), activeTriggerPairs_.end(), key)) {
                queuePhysicsEvent(physicsEvents_, PhysicsEventType::TriggerEnter,
                        static_cast<std::uint32_t>(key >> 32U),
                        static_cast<std::uint32_t>(key));
            }
        }
        for (const std::uint64_t key : activeTriggerPairs_) {
            if (!std::binary_search(currentTriggerPairs.begin(), currentTriggerPairs.end(), key)) {
                queuePhysicsEvent(physicsEvents_, PhysicsEventType::TriggerExit,
                        static_cast<std::uint32_t>(key >> 32U),
                        static_cast<std::uint32_t>(key));
            }
        }
        activeTriggerPairs_.swap(currentTriggerPairs);
        currentTriggerPairs.clear();
        physicsAccumulator_ -= step;
    }
    if (physicsAccumulator_ >= step) physicsAccumulator_ = std::fmod(physicsAccumulator_, step);
}

void Scene::update(float deltaSeconds) {
    const float dt = std::isfinite(deltaSeconds)
            ? std::clamp(deltaSeconds, 0.0f, 0.05f) : 0.0f;
    elapsedSeconds_ += dt;

    for (Entity& entity : entities_) {
        // Animation establishes authored local transforms first. Orbit and
        // pulse own position and scale when enabled; an authored rotation track
        // takes precedence over procedural spin.
        updateAnimation(entity.animation, entity.transform, dt);

        const bool animationOwnsRotation = entity.animation.enabled
                && !entity.animation.rotation.keyframes.empty();
        if (!animationOwnsRotation
                && (entity.spin.radiansPerSecond[0] != 0.0f
                    || entity.spin.radiansPerSecond[1] != 0.0f
                    || entity.spin.radiansPerSecond[2] != 0.0f)) {
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
    simulatePhysics(dt);
}

}  // namespace nuby
