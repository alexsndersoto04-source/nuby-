#include "nuby/scene.hpp"

#include <algorithm>
#include <cmath>
#include <limits>
#include <unordered_map>
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

float dot(const Vec3& a, const Vec3& b) {
    return a[0] * b[0] + a[1] * b[1] + a[2] * b[2];
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

Vec3 rotateVector(const Quaternion& rotation, const Vec3& vector) {
    const Vec3 imaginary{rotation[0], rotation[1], rotation[2]};
    const Vec3 firstCross = cross(imaginary, vector);
    const Vec3 twiceCross{2.0f * firstCross[0], 2.0f * firstCross[1],
                          2.0f * firstCross[2]};
    const Vec3 secondCross = cross(imaginary, twiceCross);
    return {vector[0] + rotation[3] * twiceCross[0] + secondCross[0],
            vector[1] + rotation[3] * twiceCross[1] + secondCross[1],
            vector[2] + rotation[3] * twiceCross[2] + secondCross[2]};
}

struct CollisionShape {
    bool sphere = false;
    Vec3 center{0.0f, 0.0f, 0.0f};
    Vec3 halfExtents{0.0f, 0.0f, 0.0f};
    Vec3 worldHalfExtents{0.0f, 0.0f, 0.0f};
    std::array<Vec3, 3> axes{{{1.0f, 0.0f, 0.0f},
                              {0.0f, 1.0f, 0.0f},
                              {0.0f, 0.0f, 1.0f}}};
    float radius = 0.0f;
    float restitution = 0.0f;
    float friction = 0.0f;
    bool trigger = false;
    std::uint32_t collisionLayer = 1U;
    std::uint32_t collisionMask = 0xFFFFFFFFU;
};

struct CollisionContact {
    Vec3 normal{1.0f, 0.0f, 0.0f};  // From first shape toward second.
    Vec3 point{0.0f, 0.0f, 0.0f};
    float penetration = 0.0f;
};

bool collideShapes(const CollisionShape& first, const CollisionShape& second,
                   CollisionContact& contact);

bool collisionShape(const Entity& entity, CollisionShape& shape) {
    if (entity.sphereCollider.enabled == entity.boxCollider.enabled || entity.parentId != 0) {
        return false;
    }
    for (float value : entity.transform.position) {
        if (!std::isfinite(value)) return false;
    }
    for (float value : entity.transform.scale) {
        if (!std::isfinite(value)) return false;
    }
    for (float value : entity.transform.rotation) {
        if (!std::isfinite(value)) return false;
    }
    shape.center = entity.transform.position;
    if (entity.sphereCollider.enabled) {
        const SphereColliderComponent& collider = entity.sphereCollider;
        const float scale = std::max({std::fabs(entity.transform.scale[0]),
                std::fabs(entity.transform.scale[1]), std::fabs(entity.transform.scale[2])});
        shape.radius = collider.radius * scale;
        if (!std::isfinite(shape.radius) || shape.radius <= 0.0f
                || !std::isfinite(collider.restitution) || !std::isfinite(collider.friction)) {
            return false;
        }
        shape.sphere = true;
        shape.restitution = collider.restitution;
        shape.friction = collider.friction;
        shape.trigger = collider.trigger;
        shape.collisionLayer = collider.collisionLayer;
        shape.collisionMask = collider.collisionMask;
        shape.halfExtents = {shape.radius, shape.radius, shape.radius};
        shape.worldHalfExtents = shape.halfExtents;
        return true;
    }

    const BoxColliderComponent& collider = entity.boxCollider;
    for (std::size_t axis = 0; axis < 3; ++axis) {
        shape.halfExtents[axis] = collider.halfExtents[axis]
                * std::fabs(entity.transform.scale[axis]);
        if (!std::isfinite(shape.halfExtents[axis]) || shape.halfExtents[axis] <= 0.0f) {
            return false;
        }
    }
    const Quaternion orientation = quaternionFromEuler(entity.transform.rotation);
    shape.axes = {{rotateVector(orientation, {1.0f, 0.0f, 0.0f}),
                   rotateVector(orientation, {0.0f, 1.0f, 0.0f}),
                   rotateVector(orientation, {0.0f, 0.0f, 1.0f})}};
    for (std::size_t worldAxis = 0; worldAxis < 3; ++worldAxis) {
        shape.worldHalfExtents[worldAxis] = 0.0f;
        for (std::size_t localAxis = 0; localAxis < 3; ++localAxis) {
            shape.worldHalfExtents[worldAxis] += shape.halfExtents[localAxis]
                    * std::fabs(shape.axes[localAxis][worldAxis]);
        }
    }
    if (!std::isfinite(collider.restitution) || !std::isfinite(collider.friction)) return false;
    shape.sphere = false;
    shape.restitution = collider.restitution;
    shape.friction = collider.friction;
    shape.trigger = collider.trigger;
    shape.collisionLayer = collider.collisionLayer;
    shape.collisionMask = collider.collisionMask;
    return true;
}

bool colliderPairAllowed(const CollisionShape& first, const CollisionShape& second) {
    return (first.collisionMask & second.collisionLayer) != 0U
            && (second.collisionMask & first.collisionLayer) != 0U;
}

bool raycastShape(const Vec3& origin, const Vec3& rayDirection, float maximumDistance,
                  const CollisionShape& shape, float& distance, Vec3& normal) {
    distance = 0.0f;
    normal = {-rayDirection[0], -rayDirection[1], -rayDirection[2]};
    if (shape.sphere) {
        const Vec3 offset{origin[0] - shape.center[0], origin[1] - shape.center[1],
                origin[2] - shape.center[2]};
        const float b = dot(offset, rayDirection);
        const float c = dot(offset, offset) - shape.radius * shape.radius;
        const float discriminant = b * b - c;
        if (!std::isfinite(discriminant) || discriminant < 0.0f
                || (c > 0.0f && b > 0.0f)) return false;
        const float candidate = -b - std::sqrt(discriminant);
        distance = candidate >= 0.0f ? candidate : 0.0f;
        if (distance > maximumDistance) return false;
        if (distance > 0.0f) {
            const Vec3 hitPosition{origin[0] + rayDirection[0] * distance,
                    origin[1] + rayDirection[1] * distance,
                    origin[2] + rayDirection[2] * distance};
            normal = {hitPosition[0] - shape.center[0],
                    hitPosition[1] - shape.center[1], hitPosition[2] - shape.center[2]};
            const float normalLength = std::sqrt(dot(normal, normal));
            if (!std::isfinite(normalLength) || normalLength <= 1.0e-8f) return false;
            for (float& component : normal) component /= normalLength;
        }
        return true;
    }

    const Vec3 relative{origin[0] - shape.center[0], origin[1] - shape.center[1],
            origin[2] - shape.center[2]};
    float nearDistance = 0.0f;
    float farDistance = maximumDistance;
    for (std::size_t axis = 0; axis < 3; ++axis) {
        const float localOrigin = dot(relative, shape.axes[axis]);
        const float localDirection = dot(rayDirection, shape.axes[axis]);
        if (std::fabs(localDirection) <= 1.0e-8f) {
            if (localOrigin < -shape.halfExtents[axis]
                    || localOrigin > shape.halfExtents[axis]) return false;
            continue;
        }
        const float first = (-shape.halfExtents[axis] - localOrigin) / localDirection;
        const float second = (shape.halfExtents[axis] - localOrigin) / localDirection;
        const float axisNear = std::min(first, second);
        const float axisFar = std::max(first, second);
        if (axisNear > nearDistance) {
            nearDistance = axisNear;
            const float side = localDirection > 0.0f ? -1.0f : 1.0f;
            for (std::size_t component = 0; component < 3; ++component) {
                normal[component] = shape.axes[axis][component] * side;
            }
        }
        farDistance = std::min(farDistance, axisFar);
        if (nearDistance > farDistance) return false;
    }
    if (farDistance < 0.0f || nearDistance > maximumDistance) return false;
    distance = nearDistance;
    return true;
}

float segmentBoxClosestPoints(const Vec3& start, const Vec3& end,
                               const CollisionShape& box, Vec3* segmentPointOutput = nullptr,
                               Vec3* boxPointOutput = nullptr) {
    const Vec3 startOffset{start[0] - box.center[0], start[1] - box.center[1],
            start[2] - box.center[2]};
    const Vec3 endOffset{end[0] - box.center[0], end[1] - box.center[1],
            end[2] - box.center[2]};
    Vec3 localStart{};
    Vec3 localDelta{};
    std::array<float, 8> breakpoints{};
    std::size_t breakpointCount = 2;
    breakpoints[0] = 0.0f;
    breakpoints[1] = 1.0f;
    for (std::size_t axis = 0; axis < 3; ++axis) {
        localStart[axis] = dot(startOffset, box.axes[axis]);
        const float localEnd = dot(endOffset, box.axes[axis]);
        localDelta[axis] = localEnd - localStart[axis];
        if (!std::isfinite(localStart[axis]) || !std::isfinite(localDelta[axis])) {
            return std::numeric_limits<float>::infinity();
        }
        if (std::fabs(localDelta[axis]) <= 1.0e-12f) continue;
        for (const float face : {-box.halfExtents[axis], box.halfExtents[axis]}) {
            const float crossing = (face - localStart[axis]) / localDelta[axis];
            if (crossing > 0.0f && crossing < 1.0f) {
                breakpoints[breakpointCount++] = crossing;
            }
        }
    }
    std::sort(breakpoints.begin(), breakpoints.begin() + breakpointCount);

    const auto distanceAt = [&](float parameter) {
        float distanceSquared = 0.0f;
        for (std::size_t axis = 0; axis < 3; ++axis) {
            const float coordinate = localStart[axis] + localDelta[axis] * parameter;
            const float outside = coordinate < -box.halfExtents[axis]
                    ? coordinate + box.halfExtents[axis]
                    : coordinate > box.halfExtents[axis]
                            ? coordinate - box.halfExtents[axis] : 0.0f;
            distanceSquared += outside * outside;
        }
        return distanceSquared;
    };

    float minimumDistanceSquared = std::numeric_limits<float>::infinity();
    float bestParameter = 0.0f;
    const auto consider = [&](float parameter) {
        const float distanceSquared = distanceAt(parameter);
        if (distanceSquared < minimumDistanceSquared) {
            minimumDistanceSquared = distanceSquared;
            bestParameter = parameter;
        }
    };
    for (std::size_t index = 0; index < breakpointCount; ++index) {
        consider(breakpoints[index]);
    }
    for (std::size_t interval = 0; interval + 1 < breakpointCount; ++interval) {
        const float lower = breakpoints[interval];
        const float upper = breakpoints[interval + 1];
        if (upper <= lower) continue;
        const float midpoint = (lower + upper) * 0.5f;
        float quadratic = 0.0f;
        float linear = 0.0f;
        for (std::size_t axis = 0; axis < 3; ++axis) {
            const float coordinate = localStart[axis] + localDelta[axis] * midpoint;
            float boundary = 0.0f;
            if (coordinate < -box.halfExtents[axis]) {
                boundary = -box.halfExtents[axis];
            } else if (coordinate > box.halfExtents[axis]) {
                boundary = box.halfExtents[axis];
            } else {
                continue;
            }
            quadratic += localDelta[axis] * localDelta[axis];
            linear += localDelta[axis] * (localStart[axis] - boundary);
        }
        if (quadratic > 1.0e-20f) {
            consider(std::clamp(-linear / quadratic, lower, upper));
        }
    }
    Vec3 segmentPoint{};
    for (std::size_t axis = 0; axis < 3; ++axis) {
        segmentPoint[axis] = start[axis] + (end[axis] - start[axis]) * bestParameter;
    }
    const Vec3 relative{segmentPoint[0] - box.center[0], segmentPoint[1] - box.center[1],
            segmentPoint[2] - box.center[2]};
    Vec3 boxPoint = box.center;
    for (std::size_t axis = 0; axis < 3; ++axis) {
        const float coordinate = std::clamp(dot(relative, box.axes[axis]),
                -box.halfExtents[axis], box.halfExtents[axis]);
        for (std::size_t component = 0; component < 3; ++component) {
            boxPoint[component] += box.axes[axis][component] * coordinate;
        }
    }
    if (segmentPointOutput != nullptr) *segmentPointOutput = segmentPoint;
    if (boxPointOutput != nullptr) *boxPointOutput = boxPoint;
    return minimumDistanceSquared;
}

bool colliderCanHitFloor(std::uint32_t layer, std::uint32_t mask,
                         const PhysicsSettings& settings) {
    return (mask & settings.floorCollisionLayer) != 0U
            && (settings.floorCollisionMask & layer) != 0U;
}

bool sweepSphereAgainstShape(const Vec3& start, const Vec3& displacement, float radius,
                             const CollisionShape& target, float& timeOfImpact,
                             Vec3& normal) {
    if (target.sphere) {
        const Vec3 relative{start[0] - target.center[0], start[1] - target.center[1],
                            start[2] - target.center[2]};
        const float combinedRadius = radius + target.radius;
        const float c = dot(relative, relative) - combinedRadius * combinedRadius;
        const float a = dot(displacement, displacement);
        if (!std::isfinite(a) || a <= 1.0e-12f || c <= 0.0f) return false;
        const float b = 2.0f * dot(relative, displacement);
        const float discriminant = b * b - 4.0f * a * c;
        if (!std::isfinite(discriminant) || discriminant < 0.0f) return false;
        const float candidate = (-b - std::sqrt(discriminant)) / (2.0f * a);
        if (candidate < 0.0f || candidate > 1.0f) return false;
        const Vec3 center{start[0] + displacement[0] * candidate,
                          start[1] + displacement[1] * candidate,
                          start[2] + displacement[2] * candidate};
        const Vec3 towardTarget{target.center[0] - center[0],
                                target.center[1] - center[1],
                                target.center[2] - center[2]};
        const float distance = std::sqrt(std::max(dot(towardTarget, towardTarget), 0.0f));
        if (distance <= 1.0e-8f) return false;
        timeOfImpact = candidate;
        for (std::size_t axis = 0; axis < 3; ++axis) normal[axis] = towardTarget[axis] / distance;
        return true;
    }

    // Conservative advancement against the exact closest point on the OBB.
    // Unlike inflating each box face by the sphere radius, this avoids corner
    // false positives while keeping the sweep bounded to a small iteration count.
    float candidate = 0.0f;
    for (unsigned int iteration = 0; iteration < 24; ++iteration) {
        const Vec3 center{start[0] + displacement[0] * candidate,
                          start[1] + displacement[1] * candidate,
                          start[2] + displacement[2] * candidate};
        const Vec3 relative{center[0] - target.center[0], center[1] - target.center[1],
                            center[2] - target.center[2]};
        Vec3 closest = target.center;
        for (std::size_t axis = 0; axis < 3; ++axis) {
            const float local = std::clamp(dot(relative, target.axes[axis]),
                    -target.halfExtents[axis], target.halfExtents[axis]);
            for (std::size_t component = 0; component < 3; ++component) {
                closest[component] += target.axes[axis][component] * local;
            }
        }
        Vec3 towardTarget{closest[0] - center[0], closest[1] - center[1],
                          closest[2] - center[2]};
        const float distanceSquared = dot(towardTarget, towardTarget);
        if (!std::isfinite(distanceSquared)) return false;
        if (distanceSquared <= (radius + 1.0e-5f) * (radius + 1.0e-5f)) {
            const float distance = std::sqrt(std::max(distanceSquared, 0.0f));
            if (distance <= 1.0e-8f || candidate <= 1.0e-7f) return false;
            timeOfImpact = candidate;
            for (std::size_t axis = 0; axis < 3; ++axis) {
                normal[axis] = towardTarget[axis] / distance;
            }
            return true;
        }
        const float distance = std::sqrt(distanceSquared);
        for (float& component : towardTarget) component /= distance;
        const float closingDistance = dot(displacement, towardTarget);
        if (closingDistance <= 1.0e-8f) return false;
        const float advance = (distance - radius) / closingDistance;
        if (!std::isfinite(advance) || advance <= 1.0e-7f) return false;
        candidate += advance;
        if (candidate > 1.0f) return false;
    }
    return false;
}

bool sweepCapsuleAgainstShape(const Vec3& start, const Vec3& end,
                              const Vec3& displacement, float radius,
                              const CollisionShape& target, float& timeOfImpact,
                              Vec3& contactPoint, Vec3& normal) {
    float candidate = 0.0f;
    for (unsigned int iteration = 0; iteration < 32; ++iteration) {
        const Vec3 first{start[0] + displacement[0] * candidate,
                start[1] + displacement[1] * candidate,
                start[2] + displacement[2] * candidate};
        const Vec3 second{end[0] + displacement[0] * candidate,
                end[1] + displacement[1] * candidate,
                end[2] + displacement[2] * candidate};
        for (std::size_t axis = 0; axis < 3; ++axis) {
            if (!std::isfinite(first[axis]) || !std::isfinite(second[axis])) return false;
        }

        Vec3 capsulePoint{};
        Vec3 targetPoint{};
        float targetRadius = 0.0f;
        float distanceSquared = 0.0f;
        if (target.sphere) {
            const Vec3 segment{second[0] - first[0], second[1] - first[1],
                    second[2] - first[2]};
            const float segmentLengthSquared = dot(segment, segment);
            const Vec3 fromStart{target.center[0] - first[0], target.center[1] - first[1],
                    target.center[2] - first[2]};
            const float parameter = segmentLengthSquared > 1.0e-20f
                    ? std::clamp(dot(fromStart, segment) / segmentLengthSquared, 0.0f, 1.0f)
                    : 0.0f;
            capsulePoint = {first[0] + segment[0] * parameter,
                    first[1] + segment[1] * parameter,
                    first[2] + segment[2] * parameter};
            const Vec3 between{capsulePoint[0] - target.center[0],
                    capsulePoint[1] - target.center[1], capsulePoint[2] - target.center[2]};
            distanceSquared = dot(between, between);
            if (!std::isfinite(distanceSquared)) return false;
            targetRadius = target.radius;
            const float distance = std::sqrt(std::max(distanceSquared, 0.0f));
            if (distance > 1.0e-8f) {
                for (std::size_t axis = 0; axis < 3; ++axis) {
                    normal[axis] = between[axis] / distance;
                }
            } else {
                const float displacementLength = std::sqrt(dot(displacement, displacement));
                normal = displacementLength > 1.0e-8f
                        ? Vec3{-displacement[0] / displacementLength,
                               -displacement[1] / displacementLength,
                               -displacement[2] / displacementLength}
                        : Vec3{1.0f, 0.0f, 0.0f};
            }
            for (std::size_t axis = 0; axis < 3; ++axis) {
                targetPoint[axis] = target.center[axis] + normal[axis] * targetRadius;
            }
        } else {
            distanceSquared = segmentBoxClosestPoints(first, second, target,
                    &capsulePoint, &targetPoint);
            if (!std::isfinite(distanceSquared)) return false;
            const Vec3 between{capsulePoint[0] - targetPoint[0],
                    capsulePoint[1] - targetPoint[1], capsulePoint[2] - targetPoint[2]};
            const float distance = std::sqrt(std::max(distanceSquared, 0.0f));
            if (distance > 1.0e-8f) {
                for (std::size_t axis = 0; axis < 3; ++axis) {
                    normal[axis] = between[axis] / distance;
                }
            } else {
                const Vec3 relative{capsulePoint[0] - target.center[0],
                        capsulePoint[1] - target.center[1],
                        capsulePoint[2] - target.center[2]};
                std::size_t nearestAxis = 0;
                float nearestFaceDistance = std::numeric_limits<float>::max();
                float nearestSide = 1.0f;
                for (std::size_t axis = 0; axis < 3; ++axis) {
                    const float local = dot(relative, target.axes[axis]);
                    const float faceDistance = target.halfExtents[axis] - std::fabs(local);
                    if (faceDistance < nearestFaceDistance) {
                        nearestFaceDistance = faceDistance;
                        nearestAxis = axis;
                        nearestSide = local >= 0.0f ? 1.0f : -1.0f;
                    }
                }
                for (std::size_t component = 0; component < 3; ++component) {
                    normal[component] = target.axes[nearestAxis][component] * nearestSide;
                    targetPoint[component] = target.center[component]
                            + normal[component] * target.halfExtents[nearestAxis];
                }
            }
        }
        if (!std::isfinite(distanceSquared)) return false;
        const float distance = std::sqrt(std::max(distanceSquared, 0.0f));
        const float separation = distance - radius - targetRadius;
        if (separation <= 1.0e-5f) {
            timeOfImpact = candidate;
            contactPoint = targetPoint;
            return true;
        }

        const Vec3 towardTarget{-normal[0], -normal[1], -normal[2]};
        const float closingDistance = dot(displacement, towardTarget);
        if (!std::isfinite(closingDistance) || closingDistance <= 1.0e-8f) return false;
        const float advance = separation / closingDistance;
        if (!std::isfinite(advance) || advance <= 0.0f) return false;
        candidate += std::max(advance, 1.0e-7f);
        if (candidate > 1.0f) return false;
    }
    return false;
}

bool sweepBoxAgainstShape(const CollisionShape& startBox, const Vec3& displacement,
                          const CollisionShape& target, float& timeOfImpact,
                          Vec3& contactPoint, Vec3& normal) {
    const auto supportPoint = [](const CollisionShape& shape, const Vec3& direction) {
        Vec3 point = shape.center;
        for (std::size_t axis = 0; axis < 3; ++axis) {
            const float projection = dot(shape.axes[axis], direction);
            const float side = projection > 1.0e-6f ? 1.0f
                    : projection < -1.0e-6f ? -1.0f : 0.0f;
            for (std::size_t component = 0; component < 3; ++component) {
                point[component] += shape.axes[axis][component]
                        * shape.halfExtents[axis] * side;
            }
        }
        return point;
    };

    CollisionContact initialContact;
    if (collideShapes(startBox, target, initialContact)) {
        for (std::size_t axis = 0; axis < 3; ++axis) {
            normal[axis] = -initialContact.normal[axis];
        }
        if (target.sphere) {
            for (std::size_t axis = 0; axis < 3; ++axis) {
                contactPoint[axis] = target.center[axis] + normal[axis] * target.radius;
            }
        } else {
            contactPoint = supportPoint(target, normal);
        }
        timeOfImpact = 0.0f;
        return true;
    }

    if (target.sphere) {
        float candidate = 0.0f;
        for (unsigned int iteration = 0; iteration < 32; ++iteration) {
            CollisionShape movingBox = startBox;
            for (std::size_t axis = 0; axis < 3; ++axis) {
                movingBox.center[axis] += displacement[axis] * candidate;
            }
            const Vec3 relative{target.center[0] - movingBox.center[0],
                    target.center[1] - movingBox.center[1],
                    target.center[2] - movingBox.center[2]};
            Vec3 closest = movingBox.center;
            for (std::size_t axis = 0; axis < 3; ++axis) {
                const float coordinate = std::clamp(dot(relative, movingBox.axes[axis]),
                        -movingBox.halfExtents[axis], movingBox.halfExtents[axis]);
                for (std::size_t component = 0; component < 3; ++component) {
                    closest[component] += movingBox.axes[axis][component] * coordinate;
                }
            }
            Vec3 fromTarget{closest[0] - target.center[0], closest[1] - target.center[1],
                    closest[2] - target.center[2]};
            const float distanceSquared = dot(fromTarget, fromTarget);
            if (!std::isfinite(distanceSquared)) return false;
            const float distance = std::sqrt(std::max(distanceSquared, 0.0f));
            if (distance > 1.0e-8f) {
                for (float& component : fromTarget) component /= distance;
                normal = fromTarget;
            } else {
                const float speed = std::sqrt(dot(displacement, displacement));
                normal = speed > 1.0e-8f
                        ? Vec3{-displacement[0] / speed, -displacement[1] / speed,
                               -displacement[2] / speed}
                        : Vec3{1.0f, 0.0f, 0.0f};
            }
            const float separation = distance - target.radius;
            if (separation <= 1.0e-5f) {
                timeOfImpact = candidate;
                for (std::size_t axis = 0; axis < 3; ++axis) {
                    contactPoint[axis] = target.center[axis] + normal[axis] * target.radius;
                }
                return true;
            }
            const Vec3 towardTarget{-normal[0], -normal[1], -normal[2]};
            const float closingDistance = dot(displacement, towardTarget);
            if (!std::isfinite(closingDistance) || closingDistance <= 1.0e-8f) return false;
            const float advance = separation / closingDistance;
            if (!std::isfinite(advance) || advance <= 0.0f) return false;
            candidate += std::max(advance, 1.0e-7f);
            if (candidate > 1.0f) return false;
        }
        return false;
    }

    const Vec3 centerDelta{target.center[0] - startBox.center[0],
            target.center[1] - startBox.center[1], target.center[2] - startBox.center[2]};
    float entryTime = 0.0f;
    float exitTime = 1.0f;
    Vec3 entryNormal{-displacement[0], -displacement[1], -displacement[2]};
    const float displacementLength = std::sqrt(dot(displacement, displacement));
    if (displacementLength > 1.0e-8f) {
        for (float& component : entryNormal) component /= displacementLength;
    } else {
        entryNormal = {1.0f, 0.0f, 0.0f};
    }
    const auto testAxis = [&](Vec3 axis) {
        const float axisLengthSquared = dot(axis, axis);
        if (axisLengthSquared <= 1.0e-10f) return true;
        const float inverseLength = 1.0f / std::sqrt(axisLengthSquared);
        for (float& component : axis) component *= inverseLength;
        float firstRadius = 0.0f;
        float secondRadius = 0.0f;
        for (std::size_t localAxis = 0; localAxis < 3; ++localAxis) {
            firstRadius += startBox.halfExtents[localAxis]
                    * std::fabs(dot(axis, startBox.axes[localAxis]));
            secondRadius += target.halfExtents[localAxis]
                    * std::fabs(dot(axis, target.axes[localAxis]));
        }
        const float radius = firstRadius + secondRadius;
        const float signedDistance = dot(centerDelta, axis);
        const float speed = dot(displacement, axis);
        if (std::fabs(speed) <= 1.0e-8f) return std::fabs(signedDistance) <= radius;
        const float firstTime = (signedDistance - radius) / speed;
        const float secondTime = (signedDistance + radius) / speed;
        const float axisEntry = std::min(firstTime, secondTime);
        const float axisExit = std::max(firstTime, secondTime);
        if (axisEntry > entryTime) {
            entryTime = axisEntry;
            const float impactDistance = signedDistance - speed * axisEntry;
            for (std::size_t component = 0; component < 3; ++component) {
                entryNormal[component] = impactDistance >= 0.0f ? -axis[component] : axis[component];
            }
        }
        exitTime = std::min(exitTime, axisExit);
        return entryTime <= exitTime;
    };

    for (const Vec3& axis : startBox.axes) {
        if (!testAxis(axis)) return false;
    }
    for (const Vec3& axis : target.axes) {
        if (!testAxis(axis)) return false;
    }
    for (const Vec3& firstAxis : startBox.axes) {
        for (const Vec3& secondAxis : target.axes) {
            if (!testAxis(cross(firstAxis, secondAxis))) return false;
        }
    }
    if (exitTime < 0.0f || entryTime > 1.0f) return false;
    timeOfImpact = std::clamp(entryTime, 0.0f, 1.0f);
    normal = entryNormal;
    contactPoint = supportPoint(target, normal);
    return true;
}

bool collideShapes(const CollisionShape& first, const CollisionShape& second,
                   CollisionContact& contact) {
    if (first.sphere && second.sphere) return false;
    if (first.sphere != second.sphere) {
        const CollisionShape& sphere = first.sphere ? first : second;
        const CollisionShape& box = first.sphere ? second : first;
        const Vec3 relative{sphere.center[0] - box.center[0],
                sphere.center[1] - box.center[1], sphere.center[2] - box.center[2]};
        Vec3 local{};
        Vec3 closest = box.center;
        for (std::size_t axis = 0; axis < 3; ++axis) {
            local[axis] = dot(relative, box.axes[axis]);
            const float coordinate = std::clamp(local[axis],
                    -box.halfExtents[axis], box.halfExtents[axis]);
            for (std::size_t component = 0; component < 3; ++component) {
                closest[component] += box.axes[axis][component] * coordinate;
            }
        }
        Vec3 sphereToBox{closest[0] - sphere.center[0], closest[1] - sphere.center[1],
                         closest[2] - sphere.center[2]};
        const float distanceSquared = dot(sphereToBox, sphereToBox);
        if (!std::isfinite(distanceSquared)) return false;
        if (distanceSquared > 1.0e-12f) {
            const float distance = std::sqrt(distanceSquared);
            if (distance >= sphere.radius) return false;
            for (float& component : sphereToBox) component /= distance;
            contact.normal = sphereToBox;
            contact.penetration = sphere.radius - distance;
        } else {
            std::size_t nearestAxis = 0;
            float nearestFaceDistance = std::numeric_limits<float>::max();
            for (std::size_t axis = 0; axis < 3; ++axis) {
                const float faceDistance = box.halfExtents[axis] - std::fabs(local[axis]);
                if (faceDistance < nearestFaceDistance) {
                    nearestFaceDistance = faceDistance;
                    nearestAxis = axis;
                }
            }
            const float side = local[nearestAxis] >= 0.0f ? 1.0f : -1.0f;
            for (std::size_t component = 0; component < 3; ++component) {
                contact.normal[component] = -side * box.axes[nearestAxis][component];
            }
            contact.penetration = sphere.radius + nearestFaceDistance;
            const float correction = side * box.halfExtents[nearestAxis] - local[nearestAxis];
            for (std::size_t component = 0; component < 3; ++component) {
                closest[component] += box.axes[nearestAxis][component] * correction;
            }
        }
        const Vec3 sphereSurface{sphere.center[0] + contact.normal[0] * sphere.radius,
                sphere.center[1] + contact.normal[1] * sphere.radius,
                sphere.center[2] + contact.normal[2] * sphere.radius};
        contact.point = {(sphereSurface[0] + closest[0]) * 0.5f,
                (sphereSurface[1] + closest[1]) * 0.5f,
                (sphereSurface[2] + closest[2]) * 0.5f};
        if (!first.sphere) {
            for (float& component : contact.normal) component = -component;
        }
        return contact.penetration > 0.0f;
    }

    const Vec3 centerDelta{second.center[0] - first.center[0],
            second.center[1] - first.center[1], second.center[2] - first.center[2]};
    float minimumOverlap = std::numeric_limits<float>::max();
    Vec3 minimumAxis{1.0f, 0.0f, 0.0f};
    const auto testAxis = [&](Vec3 axis) {
        const float axisLengthSquared = dot(axis, axis);
        if (axisLengthSquared <= 1.0e-10f) return true;
        const float inverseLength = 1.0f / std::sqrt(axisLengthSquared);
        for (float& component : axis) component *= inverseLength;
        float firstRadius = 0.0f;
        float secondRadius = 0.0f;
        for (std::size_t localAxis = 0; localAxis < 3; ++localAxis) {
            firstRadius += first.halfExtents[localAxis]
                    * std::fabs(dot(axis, first.axes[localAxis]));
            secondRadius += second.halfExtents[localAxis]
                    * std::fabs(dot(axis, second.axes[localAxis]));
        }
        const float signedDistance = dot(centerDelta, axis);
        const float overlap = firstRadius + secondRadius - std::fabs(signedDistance);
        if (overlap <= 0.0f) return false;
        if (overlap < minimumOverlap) {
            minimumOverlap = overlap;
            minimumAxis = signedDistance >= 0.0f
                    ? axis : Vec3{-axis[0], -axis[1], -axis[2]};
        }
        return true;
    };
    for (const Vec3& axis : first.axes) {
        if (!testAxis(axis)) return false;
    }
    for (const Vec3& axis : second.axes) {
        if (!testAxis(axis)) return false;
    }
    for (const Vec3& firstAxis : first.axes) {
        for (const Vec3& secondAxis : second.axes) {
            if (!testAxis(cross(firstAxis, secondAxis))) return false;
        }
    }
    contact.normal = minimumAxis;
    contact.penetration = minimumOverlap;
    const auto supportPoint = [](const CollisionShape& shape, const Vec3& direction) {
        Vec3 point = shape.center;
        for (std::size_t axis = 0; axis < 3; ++axis) {
            const float projection = dot(shape.axes[axis], direction);
            const float side = projection > 1.0e-6f ? 1.0f
                    : projection < -1.0e-6f ? -1.0f : 0.0f;
            for (std::size_t component = 0; component < 3; ++component) {
                point[component] += shape.axes[axis][component]
                        * shape.halfExtents[axis] * side;
            }
        }
        return point;
    };
    const Vec3 firstPoint = supportPoint(first, contact.normal);
    const Vec3 oppositeNormal{-contact.normal[0], -contact.normal[1], -contact.normal[2]};
    const Vec3 secondPoint = supportPoint(second, oppositeNormal);
    contact.point = {(firstPoint[0] + secondPoint[0]) * 0.5f,
            (firstPoint[1] + secondPoint[1]) * 0.5f,
            (firstPoint[2] + secondPoint[2]) * 0.5f};
    return true;
}

Vec3 inverseInertiaTimes(const CollisionShape& shape, float inverseMass,
                         const Vec3& torque) {
    if (inverseMass <= 0.0f) return {0.0f, 0.0f, 0.0f};
    if (shape.sphere) {
        const float inverseInertia = inverseSphereInertia(inverseMass, shape.radius);
        return {torque[0] * inverseInertia, torque[1] * inverseInertia,
                torque[2] * inverseInertia};
    }
    Vec3 localAngularDelta{};
    for (std::size_t axis = 0; axis < 3; ++axis) {
        const std::size_t next = (axis + 1) % 3;
        const std::size_t last = (axis + 2) % 3;
        const float denominator = shape.halfExtents[next] * shape.halfExtents[next]
                + shape.halfExtents[last] * shape.halfExtents[last];
        const float inverseInertia = denominator > 1.0e-12f
                ? 3.0f * inverseMass / denominator : 0.0f;
        localAngularDelta[axis] = dot(torque, shape.axes[axis]) * inverseInertia;
    }
    Vec3 worldAngularDelta{0.0f, 0.0f, 0.0f};
    for (std::size_t axis = 0; axis < 3; ++axis) {
        for (std::size_t component = 0; component < 3; ++component) {
            worldAngularDelta[component] += shape.axes[axis][component]
                    * localAngularDelta[axis];
        }
    }
    return worldAngularDelta;
}

float angularImpulseDenominator(const CollisionShape& shape, float inverseMass,
                                const Vec3& arm, const Vec3& axis) {
    const Vec3 inverseInertia = inverseInertiaTimes(shape, inverseMass, cross(arm, axis));
    return std::max(0.0f, dot(cross(inverseInertia, arm), axis));
}

void applyShapeAngularImpulse(RigidbodyComponent& body, const CollisionShape& shape,
                              float inverseMass, const Vec3& arm, const Vec3& impulse) {
    const Vec3 angularDelta = inverseInertiaTimes(shape, inverseMass, cross(arm, impulse));
    for (std::size_t axis = 0; axis < 3; ++axis) body.angularVelocity[axis] += angularDelta[axis];
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

void advanceSkeletalAnimation(SkeletalAnimationComponent& animation, float deltaSeconds) {
    if (!animation.enabled || !animation.playing) return;
    const float duration = std::isfinite(animation.durationSeconds)
            ? std::max(animation.durationSeconds, 0.0f) : 0.0f;
    const float current = std::isfinite(animation.timeSeconds)
            ? std::clamp(animation.timeSeconds, 0.0f, duration) : 0.0f;
    const float rate = std::isfinite(animation.playbackRate) ? animation.playbackRate : 0.0f;
    if (duration <= 0.0f) {
        animation.timeSeconds = 0.0f;
        animation.playing = false;
        return;
    }
    const float next = current + deltaSeconds * rate;
    if (!std::isfinite(next)) {
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
}  // namespace

bool Scene::raycast(const std::array<float, 3>& origin,
                    const std::array<float, 3>& direction, float maximumDistance,
                    RaycastHit& hit, std::uint32_t queryLayer,
                    std::uint32_t queryMask) const {
    hit = RaycastHit{};
    if (!std::isfinite(maximumDistance) || maximumDistance < 0.0f
            || queryLayer == 0U || queryMask == 0U) return false;
    for (float component : origin) {
        if (!std::isfinite(component)) return false;
    }
    for (float component : direction) {
        if (!std::isfinite(component)) return false;
    }
    const float directionLength = std::sqrt(dot(direction, direction));
    if (!std::isfinite(directionLength) || directionLength <= 1.0e-8f) return false;
    const Vec3 rayDirection{direction[0] / directionLength,
            direction[1] / directionLength, direction[2] / directionLength};

    bool found = false;
    float closestDistance = maximumDistance;
    for (const Entity& entity : entities_) {
        CollisionShape shape;
        if (!collisionShape(entity, shape)
                || (queryMask & shape.collisionLayer) == 0U
                || (shape.collisionMask & queryLayer) == 0U) continue;
        float distance = 0.0f;
        Vec3 normal{};
        if (!raycastShape(origin, rayDirection, maximumDistance, shape, distance, normal)
                || (found && distance >= closestDistance)) continue;
        found = true;
        closestDistance = distance;
        hit.entityId = entity.id;
        hit.distance = distance;
        for (std::size_t axis = 0; axis < 3; ++axis) {
            hit.position[axis] = origin[axis] + rayDirection[axis] * distance;
            hit.normal[axis] = normal[axis];
        }
    }
    return found;
}

std::size_t Scene::raycastAll(const std::array<float, 3>& origin,
                              const std::array<float, 3>& direction,
                              float maximumDistance, std::vector<RaycastHit>& hits,
                              std::uint32_t queryLayer, std::uint32_t queryMask) const {
    hits.clear();
    if (!std::isfinite(maximumDistance) || maximumDistance < 0.0f
            || queryLayer == 0U || queryMask == 0U) return 0;
    for (float component : origin) {
        if (!std::isfinite(component)) return 0;
    }
    for (float component : direction) {
        if (!std::isfinite(component)) return 0;
    }
    const float directionLength = std::sqrt(dot(direction, direction));
    if (!std::isfinite(directionLength) || directionLength <= 1.0e-8f) return 0;
    const Vec3 rayDirection{direction[0] / directionLength,
            direction[1] / directionLength, direction[2] / directionLength};

    for (const Entity& entity : entities_) {
        CollisionShape shape;
        if (!collisionShape(entity, shape)
                || (queryMask & shape.collisionLayer) == 0U
                || (shape.collisionMask & queryLayer) == 0U) continue;
        RaycastHit hit;
        if (!raycastShape(origin, rayDirection, maximumDistance, shape,
                          hit.distance, hit.normal)) continue;
        hit.entityId = entity.id;
        for (std::size_t axis = 0; axis < 3; ++axis) {
            hit.position[axis] = origin[axis] + rayDirection[axis] * hit.distance;
        }
        hits.push_back(hit);
    }
    std::stable_sort(hits.begin(), hits.end(), [](const RaycastHit& first,
                                                   const RaycastHit& second) {
        return first.distance < second.distance;
    });
    return hits.size();
}

bool Scene::sphereCast(const std::array<float, 3>& origin,
                       const std::array<float, 3>& direction, float radius,
                       float maximumDistance, SphereCastHit& hit,
                       std::uint32_t queryLayer, std::uint32_t queryMask) const {
    hit = SphereCastHit{};
    if (!std::isfinite(radius) || radius <= 0.0f || !std::isfinite(maximumDistance)
            || maximumDistance < 0.0f || queryLayer == 0U || queryMask == 0U) return false;
    const float queryRadiusSquared = radius * radius;
    if (!std::isfinite(queryRadiusSquared)) return false;
    for (float component : origin) {
        if (!std::isfinite(component)) return false;
    }
    for (float component : direction) {
        if (!std::isfinite(component)) return false;
    }
    const float directionLength = std::sqrt(dot(direction, direction));
    if (!std::isfinite(directionLength) || directionLength <= 1.0e-8f) return false;
    const Vec3 unitDirection{direction[0] / directionLength,
            direction[1] / directionLength, direction[2] / directionLength};
    const Vec3 displacement{unitDirection[0] * maximumDistance,
            unitDirection[1] * maximumDistance, unitDirection[2] * maximumDistance};

    bool found = false;
    float closestDistance = maximumDistance;
    for (const Entity& entity : entities_) {
        CollisionShape target;
        if (!collisionShape(entity, target)
                || (queryMask & target.collisionLayer) == 0U
                || (target.collisionMask & queryLayer) == 0U) continue;

        float distance = 0.0f;
        Vec3 contactPoint{};
        Vec3 normal{};
        bool initiallyOverlapping = false;
        if (target.sphere) {
            Vec3 fromTarget{origin[0] - target.center[0], origin[1] - target.center[1],
                    origin[2] - target.center[2]};
            const float distanceSquared = dot(fromTarget, fromTarget);
            const float combinedRadius = radius + target.radius;
            const float combinedRadiusSquared = combinedRadius * combinedRadius;
            if (!std::isfinite(distanceSquared) || !std::isfinite(combinedRadiusSquared)) continue;
            initiallyOverlapping = distanceSquared <= combinedRadiusSquared;
            if (initiallyOverlapping) {
                const float centerDistance = std::sqrt(std::max(distanceSquared, 0.0f));
                if (centerDistance > 1.0e-8f) {
                    for (float& component : fromTarget) component /= centerDistance;
                    normal = fromTarget;
                } else {
                    normal = {-unitDirection[0], -unitDirection[1], -unitDirection[2]};
                }
                for (std::size_t axis = 0; axis < 3; ++axis) {
                    contactPoint[axis] = target.center[axis] + normal[axis] * target.radius;
                }
            }
        } else {
            const Vec3 relative{origin[0] - target.center[0], origin[1] - target.center[1],
                    origin[2] - target.center[2]};
            contactPoint = target.center;
            Vec3 local{};
            for (std::size_t axis = 0; axis < 3; ++axis) {
                local[axis] = dot(relative, target.axes[axis]);
                const float coordinate = std::clamp(local[axis], -target.halfExtents[axis],
                        target.halfExtents[axis]);
                for (std::size_t component = 0; component < 3; ++component) {
                    contactPoint[component] += target.axes[axis][component] * coordinate;
                }
            }
            Vec3 fromSurface{origin[0] - contactPoint[0], origin[1] - contactPoint[1],
                    origin[2] - contactPoint[2]};
            const float distanceSquared = dot(fromSurface, fromSurface);
            if (!std::isfinite(distanceSquared)) continue;
            initiallyOverlapping = distanceSquared <= queryRadiusSquared;
            if (initiallyOverlapping) {
                if (distanceSquared > 1.0e-12f) {
                    const float centerDistance = std::sqrt(distanceSquared);
                    for (float& component : fromSurface) component /= centerDistance;
                    normal = fromSurface;
                } else {
                    std::size_t nearestAxis = 0;
                    float nearestFaceDistance = std::numeric_limits<float>::max();
                    for (std::size_t axis = 0; axis < 3; ++axis) {
                        const float faceDistance = target.halfExtents[axis]
                                - std::fabs(local[axis]);
                        if (faceDistance < nearestFaceDistance) {
                            nearestFaceDistance = faceDistance;
                            nearestAxis = axis;
                        }
                    }
                    const float side = local[nearestAxis] >= 0.0f ? 1.0f : -1.0f;
                    for (std::size_t component = 0; component < 3; ++component) {
                        normal[component] = target.axes[nearestAxis][component] * side;
                        contactPoint[component] = target.center[component]
                                + target.axes[nearestAxis][component]
                                        * side * target.halfExtents[nearestAxis];
                    }
                }
            }
        }

        if (!initiallyOverlapping) {
            if (maximumDistance <= 0.0f) continue;
            float fraction = 0.0f;
            Vec3 sweepNormal{};
            if (!sweepSphereAgainstShape(origin, displacement, radius, target,
                                         fraction, sweepNormal)) continue;
            distance = fraction * maximumDistance;
            const Vec3 castCenter{origin[0] + displacement[0] * fraction,
                    origin[1] + displacement[1] * fraction,
                    origin[2] + displacement[2] * fraction};
            if (target.sphere) {
                Vec3 fromSurface{castCenter[0] - target.center[0],
                        castCenter[1] - target.center[1], castCenter[2] - target.center[2]};
                const float normalLength = std::sqrt(dot(fromSurface, fromSurface));
                if (!std::isfinite(normalLength) || normalLength <= 1.0e-8f) continue;
                for (std::size_t axis = 0; axis < 3; ++axis) {
                    normal[axis] = fromSurface[axis] / normalLength;
                    contactPoint[axis] = target.center[axis] + normal[axis] * target.radius;
                }
            } else {
                const Vec3 relative{castCenter[0] - target.center[0],
                        castCenter[1] - target.center[1], castCenter[2] - target.center[2]};
                contactPoint = target.center;
                for (std::size_t axis = 0; axis < 3; ++axis) {
                    const float coordinate = std::clamp(dot(relative, target.axes[axis]),
                            -target.halfExtents[axis], target.halfExtents[axis]);
                    for (std::size_t component = 0; component < 3; ++component) {
                        contactPoint[component] += target.axes[axis][component] * coordinate;
                    }
                }
                Vec3 fromSurface{castCenter[0] - contactPoint[0],
                        castCenter[1] - contactPoint[1], castCenter[2] - contactPoint[2]};
                const float normalLength = std::sqrt(dot(fromSurface, fromSurface));
                if (normalLength > 1.0e-8f) {
                    for (std::size_t axis = 0; axis < 3; ++axis) {
                        normal[axis] = fromSurface[axis] / normalLength;
                    }
                } else {
                    for (std::size_t axis = 0; axis < 3; ++axis) {
                        normal[axis] = -sweepNormal[axis];
                    }
                }
            }
        }

        if (found && distance >= closestDistance) continue;
        found = true;
        closestDistance = distance;
        hit.entityId = entity.id;
        hit.distance = distance;
        hit.position = contactPoint;
        hit.normal = normal;
    }
    return found;
}

bool Scene::capsuleCast(const std::array<float, 3>& start,
                        const std::array<float, 3>& end,
                        const std::array<float, 3>& direction, float radius,
                        float maximumDistance, CapsuleCastHit& hit,
                        std::uint32_t queryLayer, std::uint32_t queryMask) const {
    hit = CapsuleCastHit{};
    if (!std::isfinite(radius) || radius <= 0.0f || !std::isfinite(maximumDistance)
            || maximumDistance < 0.0f || queryLayer == 0U || queryMask == 0U
            || !std::isfinite(radius * radius)) return false;
    for (std::size_t axis = 0; axis < 3; ++axis) {
        if (!std::isfinite(start[axis]) || !std::isfinite(end[axis])
                || !std::isfinite(direction[axis])) return false;
    }
    const float directionLength = std::sqrt(dot(direction, direction));
    if (!std::isfinite(directionLength) || directionLength <= 1.0e-8f) return false;
    const Vec3 unitDirection{direction[0] / directionLength,
            direction[1] / directionLength, direction[2] / directionLength};
    const Vec3 displacement{unitDirection[0] * maximumDistance,
            unitDirection[1] * maximumDistance, unitDirection[2] * maximumDistance};

    bool found = false;
    float closestDistance = maximumDistance;
    for (const Entity& entity : entities_) {
        CollisionShape target;
        if (!collisionShape(entity, target)
                || (queryMask & target.collisionLayer) == 0U
                || (target.collisionMask & queryLayer) == 0U) continue;
        float fraction = 0.0f;
        Vec3 contactPoint{};
        Vec3 normal{};
        if (!sweepCapsuleAgainstShape(start, end, displacement, radius, target,
                                      fraction, contactPoint, normal)) continue;
        const float distance = fraction * maximumDistance;
        if (found && distance >= closestDistance) continue;
        found = true;
        closestDistance = distance;
        hit.entityId = entity.id;
        hit.distance = distance;
        hit.position = contactPoint;
        hit.normal = normal;
    }
    return found;
}

bool Scene::boxCast(const std::array<float, 3>& center,
                    const std::array<float, 3>& halfExtents,
                    const std::array<float, 3>& rotation,
                    const std::array<float, 3>& direction, float maximumDistance,
                    BoxCastHit& hit, std::uint32_t queryLayer,
                    std::uint32_t queryMask) const {
    hit = BoxCastHit{};
    if (!std::isfinite(maximumDistance) || maximumDistance < 0.0f
            || queryLayer == 0U || queryMask == 0U) return false;
    for (std::size_t axis = 0; axis < 3; ++axis) {
        if (!std::isfinite(center[axis]) || !std::isfinite(halfExtents[axis])
                || halfExtents[axis] <= 0.0f || !std::isfinite(rotation[axis])
                || !std::isfinite(direction[axis])) return false;
    }
    const float directionLength = std::sqrt(dot(direction, direction));
    if (!std::isfinite(directionLength) || directionLength <= 1.0e-8f) return false;
    const Vec3 unitDirection{direction[0] / directionLength,
            direction[1] / directionLength, direction[2] / directionLength};
    const Vec3 displacement{unitDirection[0] * maximumDistance,
            unitDirection[1] * maximumDistance, unitDirection[2] * maximumDistance};
    CollisionShape query;
    query.center = center;
    query.halfExtents = halfExtents;
    const Quaternion orientation = quaternionFromEuler(rotation);
    query.axes = {{rotateVector(orientation, {1.0f, 0.0f, 0.0f}),
                   rotateVector(orientation, {0.0f, 1.0f, 0.0f}),
                   rotateVector(orientation, {0.0f, 0.0f, 1.0f})}};
    query.collisionLayer = queryLayer;
    query.collisionMask = queryMask;

    bool found = false;
    float closestDistance = maximumDistance;
    for (const Entity& entity : entities_) {
        CollisionShape target;
        if (!collisionShape(entity, target)
                || (queryMask & target.collisionLayer) == 0U
                || (target.collisionMask & queryLayer) == 0U) continue;
        float fraction = 0.0f;
        Vec3 contactPoint{};
        Vec3 normal{};
        if (!sweepBoxAgainstShape(query, displacement, target, fraction,
                                  contactPoint, normal)) continue;
        const float distance = fraction * maximumDistance;
        if (found && distance >= closestDistance) continue;
        found = true;
        closestDistance = distance;
        hit.entityId = entity.id;
        hit.distance = distance;
        hit.position = contactPoint;
        hit.normal = normal;
    }
    return found;
}

std::size_t Scene::overlapSphere(const std::array<float, 3>& center, float radius,
                                 std::vector<std::uint32_t>& entityIds,
                                 std::uint32_t queryLayer,
                                 std::uint32_t queryMask) const {
    entityIds.clear();
    if (!std::isfinite(radius) || radius <= 0.0f || queryLayer == 0U || queryMask == 0U) {
        return 0;
    }
    for (float component : center) {
        if (!std::isfinite(component)) return 0;
    }

    CollisionShape query;
    query.sphere = true;
    query.center = center;
    query.radius = radius;
    query.halfExtents = {radius, radius, radius};
    query.worldHalfExtents = query.halfExtents;
    query.collisionLayer = queryLayer;
    query.collisionMask = queryMask;
    for (const Entity& entity : entities_) {
        CollisionShape target;
        if (!collisionShape(entity, target) || !colliderPairAllowed(query, target)) continue;
        bool overlaps = false;
        if (target.sphere) {
            const Vec3 difference{target.center[0] - center[0],
                    target.center[1] - center[1], target.center[2] - center[2]};
            const float combinedRadius = radius + target.radius;
            overlaps = dot(difference, difference) < combinedRadius * combinedRadius;
        } else {
            CollisionContact contact;
            overlaps = collideShapes(query, target, contact);
        }
        if (overlaps) entityIds.push_back(entity.id);
    }
    return entityIds.size();
}

std::size_t Scene::overlapBox(const std::array<float, 3>& center,
                              const std::array<float, 3>& halfExtents,
                              const std::array<float, 3>& rotation,
                              std::vector<std::uint32_t>& entityIds,
                              std::uint32_t queryLayer,
                              std::uint32_t queryMask) const {
    entityIds.clear();
    if (queryLayer == 0U || queryMask == 0U) return 0;
    for (std::size_t axis = 0; axis < 3; ++axis) {
        if (!std::isfinite(center[axis]) || !std::isfinite(halfExtents[axis])
                || halfExtents[axis] <= 0.0f || !std::isfinite(rotation[axis])) return 0;
    }

    CollisionShape query;
    query.center = center;
    query.halfExtents = halfExtents;
    const Quaternion orientation = quaternionFromEuler(rotation);
    query.axes = {{rotateVector(orientation, {1.0f, 0.0f, 0.0f}),
                   rotateVector(orientation, {0.0f, 1.0f, 0.0f}),
                   rotateVector(orientation, {0.0f, 0.0f, 1.0f})}};
    for (std::size_t worldAxis = 0; worldAxis < 3; ++worldAxis) {
        for (std::size_t localAxis = 0; localAxis < 3; ++localAxis) {
            query.worldHalfExtents[worldAxis] += halfExtents[localAxis]
                    * std::fabs(query.axes[localAxis][worldAxis]);
        }
    }
    query.collisionLayer = queryLayer;
    query.collisionMask = queryMask;
    for (const Entity& entity : entities_) {
        CollisionShape target;
        if (!collisionShape(entity, target) || !colliderPairAllowed(query, target)) continue;
        CollisionContact contact;
        if (collideShapes(query, target, contact)) entityIds.push_back(entity.id);
    }
    return entityIds.size();
}

std::size_t Scene::overlapCapsule(const std::array<float, 3>& start,
                                  const std::array<float, 3>& end, float radius,
                                  std::vector<std::uint32_t>& entityIds,
                                  std::uint32_t queryLayer,
                                  std::uint32_t queryMask) const {
    entityIds.clear();
    if (!std::isfinite(radius) || radius <= 0.0f || queryLayer == 0U || queryMask == 0U) {
        return 0;
    }
    for (std::size_t axis = 0; axis < 3; ++axis) {
        if (!std::isfinite(start[axis]) || !std::isfinite(end[axis])) return 0;
    }
    const float radiusSquared = radius * radius;
    if (!std::isfinite(radiusSquared)) return 0;
    const Vec3 segment{end[0] - start[0], end[1] - start[1], end[2] - start[2]};
    const float segmentLengthSquared = dot(segment, segment);
    if (!std::isfinite(segmentLengthSquared)) return 0;

    CollisionShape query;
    query.collisionLayer = queryLayer;
    query.collisionMask = queryMask;
    for (const Entity& entity : entities_) {
        CollisionShape target;
        if (!collisionShape(entity, target) || !colliderPairAllowed(query, target)) continue;
        bool overlaps = false;
        if (target.sphere) {
            const float combinedRadius = radius + target.radius;
            const float combinedRadiusSquared = combinedRadius * combinedRadius;
            if (!std::isfinite(combinedRadiusSquared)) continue;
            const Vec3 fromStart{target.center[0] - start[0], target.center[1] - start[1],
                    target.center[2] - start[2]};
            const float parameter = segmentLengthSquared > 1.0e-20f
                    ? std::clamp(dot(fromStart, segment) / segmentLengthSquared, 0.0f, 1.0f)
                    : 0.0f;
            const Vec3 difference{fromStart[0] - segment[0] * parameter,
                    fromStart[1] - segment[1] * parameter,
                    fromStart[2] - segment[2] * parameter};
            const float distanceSquared = dot(difference, difference);
            overlaps = std::isfinite(distanceSquared)
                    && distanceSquared < combinedRadiusSquared;
        } else {
            const float distanceSquared = segmentBoxClosestPoints(start, end, target);
            overlaps = std::isfinite(distanceSquared) && distanceSquared < radiusSquared;
        }
        if (overlaps) entityIds.push_back(entity.id);
    }
    return entityIds.size();
}

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

bool Scene::appendEntities(const Scene& fragment,
                           std::vector<std::uint32_t>& appendedIds) {
    appendedIds.clear();
    const std::vector<Entity>& source = fragment.entities_;
    if (source.size() > 4096U || entities_.size() > 4096U - source.size()) return false;
    if (source.empty()) return true;

    constexpr std::uint32_t kMaximumId = std::numeric_limits<std::uint32_t>::max() - 1U;
    if (nextId_ == 0U || nextId_ >= kMaximumId
            || source.size() > static_cast<std::uint64_t>(kMaximumId) - nextId_) {
        return false;
    }

    std::unordered_map<std::uint32_t, std::uint32_t> remappedIds;
    remappedIds.reserve(source.size());
    for (std::size_t index = 0; index < source.size(); ++index) {
        const Entity& entity = source[index];
        const std::uint32_t newId = nextId_ + static_cast<std::uint32_t>(index);
        if (entity.id == 0U || entity.camera.enabled || entity.orbitCamera.enabled
                || !remappedIds.emplace(entity.id, newId).second) {
            return false;
        }
    }
    for (const Entity& entity : source) {
        if (entity.parentId != 0U && remappedIds.find(entity.parentId) == remappedIds.end()) {
            return false;
        }
        std::uint32_t parentId = entity.parentId;
        for (std::size_t depth = 0; parentId != 0U; ++depth) {
            if (depth >= source.size()) return false;
            const auto parent = std::find_if(source.begin(), source.end(),
                    [parentId](const Entity& candidate) { return candidate.id == parentId; });
            if (parent == source.end()) return false;
            parentId = parent->parentId;
        }
    }

    std::vector<Entity> additions;
    additions.reserve(source.size());
    appendedIds.reserve(source.size());
    for (const Entity& entity : source) {
        Entity copy = entity;
        copy.id = remappedIds.at(entity.id);
        copy.parentId = entity.parentId == 0U ? 0U : remappedIds.at(entity.parentId);
        appendedIds.push_back(copy.id);
        additions.push_back(std::move(copy));
    }
    entities_.reserve(entities_.size() + additions.size());
    for (Entity& entity : additions) entities_.push_back(std::move(entity));
    nextId_ += static_cast<std::uint32_t>(source.size());
    return true;
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
    for (auto pair = activeCollisionPairs_.begin(); pair != activeCollisionPairs_.end();) {
        const std::uint32_t first = static_cast<std::uint32_t>(*pair >> 32U);
        const std::uint32_t second = static_cast<std::uint32_t>(*pair);
        if (removedIds.count(first) != 0 || removedIds.count(second) != 0) {
            queuePhysicsEvent(physicsEvents_, PhysicsEventType::CollisionExit, first, second);
            pair = activeCollisionPairs_.erase(pair);
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
    if (!controller.enabled || !entity->rigidbody.enabled
            || !std::isfinite(controller.jumpSpeed) || controller.jumpSpeed <= 0.0f) {
        return false;
    }
    const bool insideCoyoteWindow = std::isfinite(controller.coyoteTimeRemaining)
            && controller.coyoteTimeRemaining > 0.0f;
    if (controller.grounded || insideCoyoteWindow) {
        entity->rigidbody.velocity[1] = controller.jumpSpeed;
        controller.grounded = false;
        controller.coyoteTimeRemaining = 0.0f;
        controller.jumpBufferRemaining = 0.0f;
        return true;
    }
    if (!std::isfinite(controller.jumpBufferSeconds)
            || controller.jumpBufferSeconds <= 0.0f) return false;
    const float buffer = std::clamp(controller.jumpBufferSeconds, 0.0f, 1.0f);
    controller.jumpBufferRemaining = std::max(controller.jumpBufferRemaining, buffer);
    return controller.jumpBufferRemaining > 0.0f;
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
    ambientLighting_ = AmbientLighting{};
    physicsSettings_ = PhysicsSettings{};
    physicsAccumulator_ = 0.0f;
    activeTriggerPairs_.clear();
    triggerPairsScratch_.clear();
    activeCollisionPairs_.clear();
    collisionPairsScratch_.clear();
    colliderIndicesScratch_.clear();
    dynamicColliderIndicesScratch_.clear();
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
    std::vector<std::size_t>& dynamicColliderIndices = dynamicColliderIndicesScratch_;
    dynamicColliderIndices.clear();
    dynamicColliderIndices.reserve(entities_.size());
    std::vector<std::uint64_t>& currentTriggerPairs = triggerPairsScratch_;
    currentTriggerPairs.clear();
    std::vector<std::uint64_t>& currentCollisionPairs = collisionPairsScratch_;
    currentCollisionPairs.clear();
    std::vector<Vec3> previousPositions(entities_.size());

    for (unsigned int substep = 0; substep < maximumSubsteps
            && physicsAccumulator_ >= step; ++substep) {
        const float dt = step;
        previousPositions.resize(entities_.size());
        for (std::size_t index = 0; index < entities_.size(); ++index) {
            previousPositions[index] = entities_[index].transform.position;
        }
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
                const float coyoteDuration = std::isfinite(controller.coyoteTimeSeconds)
                        ? std::clamp(controller.coyoteTimeSeconds, 0.0f, 1.0f) : 0.0f;
                if (wasGrounded) {
                    controller.coyoteTimeRemaining = coyoteDuration;
                } else if (std::isfinite(controller.coyoteTimeRemaining)) {
                    controller.coyoteTimeRemaining = std::max(
                            0.0f, controller.coyoteTimeRemaining - dt);
                } else {
                    controller.coyoteTimeRemaining = 0.0f;
                }
                const float bufferedJump = std::isfinite(controller.jumpBufferRemaining)
                        ? std::clamp(controller.jumpBufferRemaining, 0.0f, 1.0f) : 0.0f;
                const bool consumeBufferedJump = wasGrounded && bufferedJump > 0.0f
                        && std::isfinite(controller.jumpSpeed) && controller.jumpSpeed > 0.0f;
                if (consumeBufferedJump) {
                    body.velocity[1] = controller.jumpSpeed;
                    controller.jumpBufferRemaining = 0.0f;
                    controller.coyoteTimeRemaining = 0.0f;
                } else {
                    controller.jumpBufferRemaining = std::max(0.0f, bufferedJump - dt);
                }
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

        // Sweep every collider pair containing a sphere. The OBB orientation is
        // frozen at its end-of-step pose during the sweep; translation is relative,
        // so moving spheres can hit moving boxes and other moving spheres.
        colliderIndices.clear();
        dynamicColliderIndices.clear();
        for (std::size_t index = 0; index < entities_.size(); ++index) {
            CollisionShape shape;
            if (!collisionShape(entities_[index], shape)) continue;
            colliderIndices.push_back(index);
            const RigidbodyComponent& body = entities_[index].rigidbody;
            if (body.enabled && std::isfinite(body.mass) && body.mass >= 1.0e-6f) {
                dynamicColliderIndices.push_back(index);
            }
        }
        for (const std::size_t firstIndex : dynamicColliderIndices) {
            Entity& firstEntity = entities_[firstIndex];
            CollisionShape firstShape;
            if (!collisionShape(firstEntity, firstShape)) continue;
            for (const std::size_t secondIndex : colliderIndices) {
                if (secondIndex == firstIndex) continue;
                Entity& secondEntity = entities_[secondIndex];
                CollisionShape secondShape;
                if (!collisionShape(secondEntity, secondShape)
                        || (!firstShape.sphere && !secondShape.sphere)
                        || !colliderPairAllowed(firstShape, secondShape)) continue;
                const RigidbodyComponent& secondBody = secondEntity.rigidbody;
                const bool targetDynamic = secondBody.enabled && std::isfinite(secondBody.mass)
                        && secondBody.mass >= 1.0e-6f;
                if (targetDynamic && firstIndex > secondIndex) continue;

                Entity* sphereEntity = firstShape.sphere ? &firstEntity : &secondEntity;
                Entity* targetEntity = firstShape.sphere ? &secondEntity : &firstEntity;
                CollisionShape sphereShape = firstShape.sphere ? firstShape : secondShape;
                CollisionShape targetShape = firstShape.sphere ? secondShape : firstShape;
                const std::size_t sphereIndex = firstShape.sphere ? firstIndex : secondIndex;
                const std::size_t targetIndex = firstShape.sphere ? secondIndex : firstIndex;
                const float sphereInverseMass = sphereEntity->rigidbody.enabled
                        && std::isfinite(sphereEntity->rigidbody.mass)
                        && sphereEntity->rigidbody.mass >= 1.0e-6f
                        ? 1.0f / sphereEntity->rigidbody.mass : 0.0f;
                const float targetInverseMass = targetEntity->rigidbody.enabled
                        && std::isfinite(targetEntity->rigidbody.mass)
                        && targetEntity->rigidbody.mass >= 1.0e-6f
                        ? 1.0f / targetEntity->rigidbody.mass : 0.0f;
                if (sphereInverseMass + targetInverseMass <= 0.0f) continue;

                const Vec3 sphereStart = previousPositions[sphereIndex];
                const Vec3 targetStart = previousPositions[targetIndex];
                const Vec3 sphereDisplacement{
                        sphereEntity->transform.position[0] - sphereStart[0],
                        sphereEntity->transform.position[1] - sphereStart[1],
                        sphereEntity->transform.position[2] - sphereStart[2]};
                const Vec3 targetDisplacement{
                        targetEntity->transform.position[0] - targetStart[0],
                        targetEntity->transform.position[1] - targetStart[1],
                        targetEntity->transform.position[2] - targetStart[2]};
                const Vec3 relativeDisplacement{
                        sphereDisplacement[0] - targetDisplacement[0],
                        sphereDisplacement[1] - targetDisplacement[1],
                        sphereDisplacement[2] - targetDisplacement[2]};
                const float relativeTravelSquared = dot(relativeDisplacement,
                        relativeDisplacement);
                if (!std::isfinite(relativeTravelSquared)
                        || relativeTravelSquared <= sphereShape.radius
                                * sphereShape.radius * 0.25f) continue;

                targetShape.center = targetStart;
                const float targetSweepRadius = targetShape.sphere ? targetShape.radius
                        : std::sqrt(dot(targetShape.halfExtents, targetShape.halfExtents));
                const Vec3 relativeStart{sphereStart[0] - targetStart[0],
                        sphereStart[1] - targetStart[1], sphereStart[2] - targetStart[2]};
                const Vec3 relativeEnd{
                        sphereEntity->transform.position[0]
                                - targetEntity->transform.position[0],
                        sphereEntity->transform.position[1]
                                - targetEntity->transform.position[1],
                        sphereEntity->transform.position[2]
                                - targetEntity->transform.position[2]};
                bool sweptBoundsOverlap = true;
                for (std::size_t axis = 0; axis < 3; ++axis) {
                    const float targetExtent = targetShape.sphere || targetInverseMass <= 0.0f
                            ? targetShape.worldHalfExtents[axis] : targetSweepRadius;
                    const float extent = sphereShape.radius + targetExtent;
                    if (std::min(relativeStart[axis], relativeEnd[axis]) > extent
                            || std::max(relativeStart[axis], relativeEnd[axis]) < -extent) {
                        sweptBoundsOverlap = false;
                        break;
                    }
                }
                if (!sweptBoundsOverlap) continue;

                float timeOfImpact = 0.0f;
                Vec3 normal{0.0f, 0.0f, 0.0f};
                if (!sweepSphereAgainstShape(sphereStart, relativeDisplacement,
                        sphereShape.radius, targetShape, timeOfImpact, normal)) continue;
                if (sphereShape.trigger || targetShape.trigger) {
                    currentTriggerPairs.push_back(triggerPairKey(
                            sphereEntity->id, targetEntity->id));
                    continue;
                }
                currentCollisionPairs.push_back(triggerPairKey(
                        sphereEntity->id, targetEntity->id));

                const Vec3 sphereVelocity = sphereInverseMass > 0.0f
                        ? sphereEntity->rigidbody.velocity : Vec3{0.0f, 0.0f, 0.0f};
                const Vec3 targetVelocity = targetInverseMass > 0.0f
                        ? targetEntity->rigidbody.velocity : Vec3{0.0f, 0.0f, 0.0f};
                const float approachSpeed =
                        (sphereVelocity[0] - targetVelocity[0]) * normal[0]
                        + (sphereVelocity[1] - targetVelocity[1]) * normal[1]
                        + (sphereVelocity[2] - targetVelocity[2]) * normal[2];
                const float inverseMassSum = sphereInverseMass + targetInverseMass;
                if (approachSpeed <= 0.0f || inverseMassSum <= 0.0f) continue;

                for (std::size_t axis = 0; axis < 3; ++axis) {
                    if (sphereInverseMass > 0.0f) {
                        sphereEntity->transform.position[axis] = sphereStart[axis]
                                + sphereDisplacement[axis] * timeOfImpact;
                    }
                    if (targetInverseMass > 0.0f) {
                        targetEntity->transform.position[axis] = targetStart[axis]
                                + targetDisplacement[axis] * timeOfImpact;
                    }
                }
                const float restitution = std::clamp(std::min(sphereShape.restitution,
                        targetShape.restitution), 0.0f, 1.0f);
                const float impulseMagnitude = (1.0f + restitution) * approachSpeed
                        / inverseMassSum;
                if (sphereInverseMass > 0.0f) {
                    for (std::size_t axis = 0; axis < 3; ++axis) {
                        sphereEntity->rigidbody.velocity[axis] -= normal[axis]
                                * impulseMagnitude * sphereInverseMass;
                    }
                }
                if (targetInverseMass > 0.0f) {
                    for (std::size_t axis = 0; axis < 3; ++axis) {
                        targetEntity->rigidbody.velocity[axis] += normal[axis]
                                * impulseMagnitude * targetInverseMass;
                    }
                }
                const float remainingTime = dt * (1.0f - timeOfImpact);
                if (sphereInverseMass > 0.0f) {
                    for (std::size_t axis = 0; axis < 3; ++axis) {
                        sphereEntity->transform.position[axis]
                                += sphereEntity->rigidbody.velocity[axis] * remainingTime;
                    }
                }
                if (targetInverseMass > 0.0f) {
                    for (std::size_t axis = 0; axis < 3; ++axis) {
                        targetEntity->transform.position[axis]
                                += targetEntity->rigidbody.velocity[axis] * remainingTime;
                    }
                }
            }
        }

        // A swept sphere can hit the infinite physics floor even when it crosses
        // the entire plane between fixed steps.
        if (physicsSettings_.floorEnabled && std::isfinite(physicsSettings_.floorHeight)
                && std::isfinite(physicsSettings_.floorRestitution)) {
            for (std::size_t index = 0; index < entities_.size(); ++index) {
                Entity& entity = entities_[index];
                if (!entity.sphereCollider.enabled || entity.boxCollider.enabled
                        || entity.parentId != 0 || !entity.rigidbody.enabled
                        || !colliderCanHitFloor(entity.sphereCollider.collisionLayer,
                                entity.sphereCollider.collisionMask, physicsSettings_)
                        || !std::isfinite(entity.rigidbody.mass)
                        || entity.rigidbody.mass < 1.0e-6f) continue;
                const float scale = std::max({std::fabs(entity.transform.scale[0]),
                        std::fabs(entity.transform.scale[1]), std::fabs(entity.transform.scale[2])});
                const float radius = entity.sphereCollider.radius * scale;
                const Vec3 start = previousPositions[index];
                const Vec3 displacement{entity.transform.position[0] - start[0],
                        entity.transform.position[1] - start[1],
                        entity.transform.position[2] - start[2]};
                if (!std::isfinite(radius) || radius <= 0.0f || displacement[1] >= -1.0e-8f
                        || start[1] <= physicsSettings_.floorHeight + radius + 1.0e-4f
                        || displacement[1] * displacement[1] <= radius * radius * 0.25f) continue;
                const float timeOfImpact = (physicsSettings_.floorHeight + radius - start[1])
                        / displacement[1];
                if (!std::isfinite(timeOfImpact) || timeOfImpact <= 1.0e-5f
                        || timeOfImpact > 1.0f) continue;
                for (std::size_t axis = 0; axis < 3; ++axis) {
                    entity.transform.position[axis] = start[axis]
                            + displacement[axis] * timeOfImpact;
                }
                const float restitution = std::clamp(std::min(
                        entity.sphereCollider.restitution, physicsSettings_.floorRestitution),
                        0.0f, 1.0f);
                const float downwardSpeed = entity.rigidbody.velocity[1];
                if (downwardSpeed < 0.0f) {
                    entity.rigidbody.velocity[1] -= (1.0f + restitution) * downwardSpeed;
                }
                const float remainingTime = dt * (1.0f - timeOfImpact);
                for (std::size_t axis = 0; axis < 3; ++axis) {
                    entity.transform.position[axis] += entity.rigidbody.velocity[axis]
                            * remainingTime;
                }
            }
        }

        if (physicsSettings_.floorEnabled && std::isfinite(physicsSettings_.floorHeight)
                && std::isfinite(physicsSettings_.floorRestitution)
                && std::isfinite(physicsSettings_.floorFriction)) {
            for (Entity& entity : entities_) {
                RigidbodyComponent& body = entity.rigidbody;
                const SphereColliderComponent& collider = entity.sphereCollider;
                if (!body.enabled || !std::isfinite(body.mass) || body.mass < 1.0e-6f
                        || entity.parentId != 0 || entity.boxCollider.enabled
                        || !collider.enabled
                        || !colliderCanHitFloor(collider.collisionLayer,
                                collider.collisionMask, physicsSettings_)
                        || !std::isfinite(collider.radius)
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
            const Vec3 up{0.0f, 1.0f, 0.0f};
            for (Entity& entity : entities_) {
                CollisionShape shape;
                if (!entity.boxCollider.enabled || !collisionShape(entity, shape)
                        || !colliderCanHitFloor(shape.collisionLayer, shape.collisionMask,
                                physicsSettings_)
                        || !entity.rigidbody.enabled || !std::isfinite(entity.rigidbody.mass)
                        || entity.rigidbody.mass < 1.0e-6f) continue;
                const float halfHeight = shape.worldHalfExtents[1];
                const float penetration = physicsSettings_.floorHeight + halfHeight
                        - entity.transform.position[1];
                if (!std::isfinite(penetration) || penetration < -0.0001f) continue;
                if (penetration > 0.0f) entity.transform.position[1] += penetration;
                RigidbodyComponent& body = entity.rigidbody;
                const float inverseMass = 1.0f / body.mass;
                Vec3 arm{0.0f, 0.0f, 0.0f};
                for (std::size_t axis = 0; axis < 3; ++axis) {
                    const float vertical = shape.axes[axis][1];
                    if (std::fabs(vertical) <= 1.0e-6f) continue;
                    const float side = vertical > 0.0f ? -1.0f : 1.0f;
                    for (std::size_t component = 0; component < 3; ++component) {
                        arm[component] += shape.axes[axis][component]
                                * shape.halfExtents[axis] * side;
                    }
                }
                const Vec3 spin = cross(body.angularVelocity, arm);
                const float normalVelocity = body.velocity[1] + spin[1];
                const float normalDenominator = inverseMass
                        + angularImpulseDenominator(shape, inverseMass, arm, up);
                float normalImpulse = 0.0f;
                if (normalVelocity < 0.0f && normalDenominator > 0.0f) {
                    normalImpulse = -(1.0f + std::clamp(std::min(
                            entity.boxCollider.restitution, physicsSettings_.floorRestitution),
                            0.0f, 1.0f)) * normalVelocity / normalDenominator;
                    const Vec3 impulse{0.0f, normalImpulse, 0.0f};
                    for (std::size_t axis = 0; axis < 3; ++axis) {
                        body.velocity[axis] += impulse[axis] * inverseMass;
                    }
                    applyShapeAngularImpulse(body, shape, inverseMass, arm, impulse);
                }
                if (entity.characterController.enabled && body.velocity[1] <= 0.0f) {
                    entity.characterController.grounded = true;
                }
                const Vec3 contactVelocity{body.velocity[0] + spin[0],
                        body.velocity[1] + spin[1], body.velocity[2] + spin[2]};
                const Vec3 tangent{contactVelocity[0], 0.0f, contactVelocity[2]};
                const float tangentSpeed = std::hypot(tangent[0], tangent[2]);
                if (tangentSpeed > 1.0e-6f && normalImpulse > 0.0f) {
                    const Vec3 direction{-tangent[0] / tangentSpeed, 0.0f,
                                         -tangent[2] / tangentSpeed};
                    const float tangentDenominator = inverseMass
                            + angularImpulseDenominator(shape, inverseMass, arm, direction);
                    const float friction = std::clamp(std::min(
                            entity.boxCollider.friction, physicsSettings_.floorFriction),
                            0.0f, 1.0f);
                    const float tangentImpulse = std::min(tangentSpeed
                            / std::max(tangentDenominator, 1.0e-8f), normalImpulse * friction);
                    const Vec3 impulse{direction[0] * tangentImpulse, 0.0f,
                                       direction[2] * tangentImpulse};
                    for (std::size_t axis = 0; axis < 3; ++axis) {
                        body.velocity[axis] += impulse[axis] * inverseMass;
                    }
                    applyShapeAngularImpulse(body, shape, inverseMass, arm, impulse);
                }
            }
        }

        colliderIndices.clear();
        for (std::size_t index = 0; index < entities_.size(); ++index) {
            const Entity& entity = entities_[index];
            const SphereColliderComponent& collider = entity.sphereCollider;
            if (!collider.enabled || entity.boxCollider.enabled || entity.parentId != 0
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
                if ((firstCollider.collisionMask & secondCollider.collisionLayer) == 0U
                        || (secondCollider.collisionMask & firstCollider.collisionLayer) == 0U) {
                    continue;
                }
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
                currentCollisionPairs.push_back(triggerPairKey(first.id, second.id));
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
        const bool hasBoxCollider = std::any_of(entities_.begin(), entities_.end(),
                [](const Entity& entity) { return entity.boxCollider.enabled; });
        if (hasBoxCollider) {
            colliderIndices.clear();
            for (std::size_t index = 0; index < entities_.size(); ++index) {
                CollisionShape shape;
                if (collisionShape(entities_[index], shape)) colliderIndices.push_back(index);
            }
            std::sort(colliderIndices.begin(), colliderIndices.end(), [this](
                    std::size_t left, std::size_t right) {
                CollisionShape a;
                CollisionShape b;
                collisionShape(entities_[left], a);
                collisionShape(entities_[right], b);
                return a.center[0] - a.worldHalfExtents[0]
                        < b.center[0] - b.worldHalfExtents[0];
            });
            for (std::size_t firstIndex = 0; firstIndex < colliderIndices.size(); ++firstIndex) {
                Entity& first = entities_[colliderIndices[firstIndex]];
                CollisionShape firstShape;
                if (!collisionShape(first, firstShape)) continue;
                const float firstMaximumX = firstShape.center[0]
                        + firstShape.worldHalfExtents[0];
                for (std::size_t secondIndex = firstIndex + 1;
                        secondIndex < colliderIndices.size(); ++secondIndex) {
                    Entity& second = entities_[colliderIndices[secondIndex]];
                    CollisionShape secondShape;
                    if (!collisionShape(second, secondShape)) continue;
                    if (secondShape.center[0] - secondShape.worldHalfExtents[0]
                            > firstMaximumX) break;
                    if (firstShape.sphere && secondShape.sphere) continue;
                    if (!colliderPairAllowed(firstShape, secondShape)) continue;

                    CollisionContact contact;
                    if (!collideShapes(firstShape, secondShape, contact)) continue;
                    if (firstShape.trigger || secondShape.trigger) {
                        currentTriggerPairs.push_back(triggerPairKey(first.id, second.id));
                        continue;
                    }
                    currentCollisionPairs.push_back(triggerPairKey(first.id, second.id));
                    RigidbodyComponent& firstBody = first.rigidbody;
                    RigidbodyComponent& secondBody = second.rigidbody;
                    const float firstInverseMass = firstBody.enabled && std::isfinite(firstBody.mass)
                            && firstBody.mass >= 1.0e-6f ? 1.0f / firstBody.mass : 0.0f;
                    const float secondInverseMass = secondBody.enabled && std::isfinite(secondBody.mass)
                            && secondBody.mass >= 1.0e-6f ? 1.0f / secondBody.mass : 0.0f;
                    const float inverseMassSum = firstInverseMass + secondInverseMass;
                    if (inverseMassSum <= 0.0f) continue;
                    const float correction = std::max(contact.penetration - 0.001f, 0.0f)
                            * 0.8f / inverseMassSum;
                    for (std::size_t axis = 0; axis < 3; ++axis) {
                        first.transform.position[axis] -= contact.normal[axis]
                                * correction * firstInverseMass;
                        second.transform.position[axis] += contact.normal[axis]
                                * correction * secondInverseMass;
                    }
                    const Vec3 firstArm{contact.point[0] - first.transform.position[0],
                            contact.point[1] - first.transform.position[1],
                            contact.point[2] - first.transform.position[2]};
                    const Vec3 secondArm{contact.point[0] - second.transform.position[0],
                            contact.point[1] - second.transform.position[1],
                            contact.point[2] - second.transform.position[2]};
                    const auto pointVelocity = [](const RigidbodyComponent& body, float inverseMass,
                                                  const Vec3& arm) {
                        if (inverseMass <= 0.0f) return Vec3{0.0f, 0.0f, 0.0f};
                        const Vec3 spin = cross(body.angularVelocity, arm);
                        return Vec3{body.velocity[0] + spin[0], body.velocity[1] + spin[1],
                                    body.velocity[2] + spin[2]};
                    };
                    const auto relativeVelocity = [&]() {
                        const Vec3 firstVelocity = pointVelocity(firstBody, firstInverseMass, firstArm);
                        const Vec3 secondVelocity = pointVelocity(secondBody, secondInverseMass, secondArm);
                        return Vec3{secondVelocity[0] - firstVelocity[0],
                                secondVelocity[1] - firstVelocity[1],
                                secondVelocity[2] - firstVelocity[2]};
                    };
                    const auto applyImpulse = [&](const Vec3& impulse) {
                        if (firstInverseMass > 0.0f) {
                            for (std::size_t axis = 0; axis < 3; ++axis) {
                                firstBody.velocity[axis] -= impulse[axis] * firstInverseMass;
                            }
                            applyShapeAngularImpulse(firstBody, firstShape, firstInverseMass,
                                    firstArm, {-impulse[0], -impulse[1], -impulse[2]});
                        }
                        if (secondInverseMass > 0.0f) {
                            for (std::size_t axis = 0; axis < 3; ++axis) {
                                secondBody.velocity[axis] += impulse[axis] * secondInverseMass;
                            }
                            applyShapeAngularImpulse(secondBody, secondShape, secondInverseMass,
                                    secondArm, impulse);
                        }
                    };
                    Vec3 relative = relativeVelocity();
                    const float normalSpeed = dot(relative, contact.normal);
                    const float normalDenominator = inverseMassSum
                            + angularImpulseDenominator(firstShape, firstInverseMass,
                                    firstArm, contact.normal)
                            + angularImpulseDenominator(secondShape, secondInverseMass,
                                    secondArm, contact.normal);
                    float normalImpulse = 0.0f;
                    if (normalSpeed < 0.0f && normalDenominator > 1.0e-8f) {
                        const float restitution = std::clamp(std::min(firstShape.restitution,
                                secondShape.restitution), 0.0f, 1.0f);
                        normalImpulse = -(1.0f + restitution) * normalSpeed / normalDenominator;
                        const Vec3 impulse{contact.normal[0] * normalImpulse,
                                contact.normal[1] * normalImpulse,
                                contact.normal[2] * normalImpulse};
                        applyImpulse(impulse);
                    }
                    if (normalImpulse <= 0.0f) continue;
                    relative = relativeVelocity();
                    const float relativeNormal = dot(relative, contact.normal);
                    Vec3 tangent{relative[0] - relativeNormal * contact.normal[0],
                            relative[1] - relativeNormal * contact.normal[1],
                            relative[2] - relativeNormal * contact.normal[2]};
                    const float tangentSpeed = std::sqrt(dot(tangent, tangent));
                    if (tangentSpeed <= 1.0e-6f) continue;
                    for (float& component : tangent) component /= tangentSpeed;
                    const float tangentDenominator = inverseMassSum
                            + angularImpulseDenominator(firstShape, firstInverseMass, firstArm, tangent)
                            + angularImpulseDenominator(secondShape, secondInverseMass, secondArm, tangent);
                    if (tangentDenominator <= 1.0e-8f) continue;
                    const float friction = std::clamp(std::sqrt(std::max(0.0f,
                            firstShape.friction * secondShape.friction)), 0.0f, 1.0f);
                    const float frictionImpulse = std::min(tangentSpeed / tangentDenominator,
                            normalImpulse * friction);
                    const Vec3 impulse{-tangent[0] * frictionImpulse,
                            -tangent[1] * frictionImpulse, -tangent[2] * frictionImpulse};
                    applyImpulse(impulse);
                }
            }
        }

        std::sort(currentTriggerPairs.begin(), currentTriggerPairs.end());
        currentTriggerPairs.erase(std::unique(currentTriggerPairs.begin(),
                currentTriggerPairs.end()), currentTriggerPairs.end());
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

        std::sort(currentCollisionPairs.begin(), currentCollisionPairs.end());
        currentCollisionPairs.erase(std::unique(currentCollisionPairs.begin(),
                currentCollisionPairs.end()), currentCollisionPairs.end());
        for (const std::uint64_t key : currentCollisionPairs) {
            if (!std::binary_search(activeCollisionPairs_.begin(), activeCollisionPairs_.end(), key)) {
                queuePhysicsEvent(physicsEvents_, PhysicsEventType::CollisionEnter,
                        static_cast<std::uint32_t>(key >> 32U),
                        static_cast<std::uint32_t>(key));
            }
        }
        for (const std::uint64_t key : activeCollisionPairs_) {
            if (!std::binary_search(currentCollisionPairs.begin(), currentCollisionPairs.end(), key)) {
                queuePhysicsEvent(physicsEvents_, PhysicsEventType::CollisionExit,
                        static_cast<std::uint32_t>(key >> 32U),
                        static_cast<std::uint32_t>(key));
            }
        }
        activeCollisionPairs_.swap(currentCollisionPairs);
        currentCollisionPairs.clear();
        physicsAccumulator_ -= step;
    }
    if (physicsAccumulator_ >= step) physicsAccumulator_ = std::fmod(physicsAccumulator_, step);
}

void Scene::refreshOrbitCameraCollisions(float deltaSeconds) {
    const float dt = std::isfinite(deltaSeconds)
            ? std::clamp(deltaSeconds, 0.0f, 0.05f) : 0.0f;
    for (Entity& entity : entities_) {
        OrbitCameraController& camera = entity.orbitCamera;
        if (!camera.enabled || !camera.collisionEnabled) {
            camera.resolvedDistance = -1.0f;
            continue;
        }
        const float requestedDistance = std::isfinite(camera.distance)
                ? std::max(camera.distance, 0.1f) : 0.1f;
        const bool zoomChanged = std::isfinite(camera.lastRequestedDistance)
                && camera.lastRequestedDistance > 0.0f
                && std::fabs(requestedDistance - camera.lastRequestedDistance) > 1.0e-4f;
        if (zoomChanged || !std::isfinite(camera.resolvedDistance)
                || camera.resolvedDistance <= 0.0f) {
            camera.resolvedDistance = requestedDistance;
        }
        camera.lastRequestedDistance = requestedDistance;
        const float horizontal = std::cos(camera.pitch) * requestedDistance;
        const std::array<float, 3> offset{
            std::sin(camera.yaw) * horizontal,
            std::sin(camera.pitch) * requestedDistance + 0.12f,
            std::cos(camera.yaw) * horizontal};
        const float boomLength = std::sqrt(dot(offset, offset));
        if (!std::isfinite(boomLength) || boomLength <= 1.0e-6f) {
            camera.resolvedDistance = requestedDistance;
            continue;
        }
        std::array<float, 3> direction{};
        for (std::size_t axis = 0; axis < direction.size(); ++axis) {
            direction[axis] = offset[axis] / boomLength;
        }
        float safeDistance = requestedDistance;
        SphereCastHit hit;
        if (sphereCast(camera.target, direction, camera.collisionRadius,
                       boomLength, hit, camera.collisionLayer, camera.collisionMask)) {
            const float safeBoomLength = std::min(boomLength,
                    std::max(camera.collisionMinimumDistance,
                            hit.distance - camera.collisionPadding));
            safeDistance = requestedDistance * (safeBoomLength / boomLength);
        }
        if (!std::isfinite(camera.resolvedDistance) || camera.resolvedDistance <= 0.0f) {
            camera.resolvedDistance = requestedDistance;
        }
        if (safeDistance < camera.resolvedDistance) {
            // Retraction is immediate to avoid rendering through a newly entered wall.
            camera.resolvedDistance = safeDistance;
        } else if (safeDistance > camera.resolvedDistance) {
            const float speed = std::max(0.0f, camera.collisionRecoverySpeed);
            const float blend = speed > 0.0f ? 1.0f - std::exp(-speed * dt) : 0.0f;
            camera.resolvedDistance += (safeDistance - camera.resolvedDistance) * blend;
        }
    }
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
        SkeletalAnimationComponent& skeletal = entity.skeletalAnimation;
        const float transitionDuration = entity.skeletalLocomotion.transitionDurationSeconds;
        if (!skeletal.transitionFromClip.empty()) {
            if (!std::isfinite(transitionDuration) || transitionDuration <= 0.0f) {
                skeletal.transitionFromClip.clear();
                skeletal.transitionFromTimeSeconds = 0.0f;
                skeletal.transitionElapsedSeconds = 0.0f;
                skeletal.transitionFromLooping = true;
            } else {
                skeletal.transitionElapsedSeconds += dt;
                const float rate = std::isfinite(skeletal.playbackRate)
                        ? skeletal.playbackRate : 0.0f;
                skeletal.transitionFromTimeSeconds = std::max(0.0f,
                        skeletal.transitionFromTimeSeconds + dt * rate);
                if (skeletal.transitionElapsedSeconds >= transitionDuration) {
                    skeletal.transitionFromClip.clear();
                    skeletal.transitionFromTimeSeconds = 0.0f;
                    skeletal.transitionElapsedSeconds = 0.0f;
                    skeletal.transitionFromLooping = true;
                }
            }
        }
        advanceSkeletalAnimation(skeletal, dt);

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
    refreshOrbitCameraCollisions(dt);

    for (Entity& entity : entities_) {
        SkeletalLocomotionComponent& locomotion = entity.skeletalLocomotion;
        SkeletalAnimationComponent& animation = entity.skeletalAnimation;
        if (!locomotion.enabled || !animation.enabled || entity.parentId == 0
                || !std::isfinite(locomotion.walkSpeedThreshold)
                || !std::isfinite(locomotion.runSpeedThreshold)
                || locomotion.idleClip.empty() || locomotion.walkClip.empty()
                || locomotion.runClip.empty() || locomotion.jumpClip.empty()) {
            continue;
        }
        const Entity* character = find(entity.parentId);
        if (character == nullptr || !character->characterController.enabled
                || !character->rigidbody.enabled) {
            continue;
        }
        const float planarSpeed = std::hypot(character->rigidbody.velocity[0],
                                             character->rigidbody.velocity[2]);
        if (!std::isfinite(planarSpeed)) continue;
        const float walkThreshold = std::max(0.0f, locomotion.walkSpeedThreshold);
        const float runThreshold = std::max(walkThreshold, locomotion.runSpeedThreshold);
        const char* selectedClip = !character->characterController.grounded
                ? locomotion.jumpClip.c_str()
                : planarSpeed >= runThreshold ? locomotion.runClip.c_str()
                : planarSpeed >= walkThreshold ? locomotion.walkClip.c_str()
                : locomotion.idleClip.c_str();
        if (animation.clipName != selectedClip) {
            const float transitionDuration = locomotion.transitionDurationSeconds;
            if (!animation.clipName.empty() && std::isfinite(transitionDuration)
                    && transitionDuration > 0.0f) {
                animation.transitionFromClip = animation.clipName;
                animation.transitionFromTimeSeconds = animation.timeSeconds;
                animation.transitionFromLooping = animation.looping;
                animation.transitionElapsedSeconds = 0.0f;
            } else {
                animation.transitionFromClip.clear();
                animation.transitionFromTimeSeconds = 0.0f;
                animation.transitionFromLooping = true;
                animation.transitionElapsedSeconds = 0.0f;
            }
            animation.clipName = selectedClip;
            animation.timeSeconds = 0.0f;
            animation.playing = true;
            animation.looping = animation.clipName != locomotion.jumpClip;
        }
    }
}

}  // namespace nuby
