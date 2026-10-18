#include "nuby/skeletal_animation.hpp"

#include <algorithm>
#include <cmath>
#include <limits>
#include <utility>

namespace nuby {
namespace {
constexpr std::size_t kMaximumJoints = 64;
constexpr std::size_t kMaximumTrackKeyframes = 16384;
constexpr float kMaximumAuthoredValue = 1000000.0f;

bool finiteVector(const std::array<float, 3>& value) {
    return std::isfinite(value[0]) && std::isfinite(value[1]) && std::isfinite(value[2]);
}

float quaternionLengthSquared(const SkeletalQuaternion& value) {
    return value.x * value.x + value.y * value.y + value.z * value.z + value.w * value.w;
}

bool validQuaternion(const SkeletalQuaternion& value) {
    const float lengthSquared = quaternionLengthSquared(value);
    return std::isfinite(value.x) && std::isfinite(value.y) && std::isfinite(value.z)
            && std::isfinite(value.w) && std::isfinite(lengthSquared)
            && lengthSquared > 1.0e-12f;
}

SkeletalQuaternion normalizeQuaternion(const SkeletalQuaternion& value) {
    const float inverseLength = 1.0f / std::sqrt(quaternionLengthSquared(value));
    return {value.x * inverseLength, value.y * inverseLength,
            value.z * inverseLength, value.w * inverseLength};
}

SkeletalQuaternion interpolateQuaternion(const SkeletalQuaternion& fromValue,
                                          const SkeletalQuaternion& toValue,
                                          float amount) {
    SkeletalQuaternion from = normalizeQuaternion(fromValue);
    SkeletalQuaternion to = normalizeQuaternion(toValue);
    float cosine = from.x * to.x + from.y * to.y + from.z * to.z + from.w * to.w;
    if (cosine < 0.0f) {
        cosine = -cosine;
        to = {-to.x, -to.y, -to.z, -to.w};
    }
    cosine = std::clamp(cosine, -1.0f, 1.0f);
    if (cosine > 0.9995f) {
        return normalizeQuaternion({from.x + amount * (to.x - from.x),
                from.y + amount * (to.y - from.y),
                from.z + amount * (to.z - from.z),
                from.w + amount * (to.w - from.w)});
    }
    const float angle = std::acos(cosine);
    const float sine = std::sin(angle);
    if (std::fabs(sine) <= 1.0e-7f) return from;
    const float fromWeight = std::sin((1.0f - amount) * angle) / sine;
    const float toWeight = std::sin(amount * angle) / sine;
    return normalizeQuaternion({fromWeight * from.x + toWeight * to.x,
            fromWeight * from.y + toWeight * to.y,
            fromWeight * from.z + toWeight * to.z,
            fromWeight * from.w + toWeight * to.w});
}

SkeletalMatrix4 identityMatrix() {
    SkeletalMatrix4 result{};
    result[0] = result[5] = result[10] = result[15] = 1.0f;
    return result;
}

SkeletalMatrix4 multiplyMatrix(const SkeletalMatrix4& left,
                               const SkeletalMatrix4& right) {
    SkeletalMatrix4 result{};
    for (std::size_t column = 0; column < 4; ++column) {
        for (std::size_t row = 0; row < 4; ++row) {
            for (std::size_t inner = 0; inner < 4; ++inner) {
                result[column * 4 + row] += left[inner * 4 + row]
                        * right[column * 4 + inner];
            }
        }
    }
    return result;
}

SkeletalMatrix4 composeTransform(const std::array<float, 3>& translation,
                                const SkeletalQuaternion& rotationValue,
                                const std::array<float, 3>& scale) {
    const SkeletalQuaternion rotation = normalizeQuaternion(rotationValue);
    const float xx = rotation.x * rotation.x;
    const float yy = rotation.y * rotation.y;
    const float zz = rotation.z * rotation.z;
    const float xy = rotation.x * rotation.y;
    const float xz = rotation.x * rotation.z;
    const float yz = rotation.y * rotation.z;
    const float wx = rotation.w * rotation.x;
    const float wy = rotation.w * rotation.y;
    const float wz = rotation.w * rotation.z;

    SkeletalMatrix4 result = identityMatrix();
    result[0] = (1.0f - 2.0f * (yy + zz)) * scale[0];
    result[1] = (2.0f * (xy + wz)) * scale[0];
    result[2] = (2.0f * (xz - wy)) * scale[0];
    result[4] = (2.0f * (xy - wz)) * scale[1];
    result[5] = (1.0f - 2.0f * (xx + zz)) * scale[1];
    result[6] = (2.0f * (yz + wx)) * scale[1];
    result[8] = (2.0f * (xz + wy)) * scale[2];
    result[9] = (2.0f * (yz - wx)) * scale[2];
    result[10] = (1.0f - 2.0f * (xx + yy)) * scale[2];
    result[12] = translation[0];
    result[13] = translation[1];
    result[14] = translation[2];
    return result;
}

bool validTrackTime(float timeSeconds, float durationSeconds,
                    float previousTime, bool hasPrevious) {
    return std::isfinite(timeSeconds) && timeSeconds >= 0.0f
            && timeSeconds <= durationSeconds
            && (!hasPrevious || timeSeconds > previousTime);
}

std::array<float, 3> sampleVectorTrack(const SkeletalVectorTrack& track,
                                       float timeSeconds,
                                       const std::array<float, 3>& fallback) {
    if (track.keyframes.empty() || timeSeconds <= track.keyframes.front().timeSeconds) {
        return track.keyframes.empty() ? fallback : track.keyframes.front().value;
    }
    if (timeSeconds >= track.keyframes.back().timeSeconds) return track.keyframes.back().value;
    const auto upper = std::upper_bound(track.keyframes.begin(), track.keyframes.end(), timeSeconds,
            [](float time, const SkeletalVectorKeyframe& keyframe) {
                return time < keyframe.timeSeconds;
            });
    const SkeletalVectorKeyframe& right = *upper;
    const SkeletalVectorKeyframe& left = *(upper - 1);
    const float span = right.timeSeconds - left.timeSeconds;
    if (span <= 0.0f) return right.value;
    const float amount = (timeSeconds - left.timeSeconds) / span;
    return {left.value[0] + (right.value[0] - left.value[0]) * amount,
            left.value[1] + (right.value[1] - left.value[1]) * amount,
            left.value[2] + (right.value[2] - left.value[2]) * amount};
}

bool validateVectorTrack(const SkeletalVectorTrack& track, float durationSeconds,
                         std::string& error) {
    if (track.keyframes.size() > kMaximumTrackKeyframes) {
        error = "Skeletal animation vector track exceeds the keyframe limit";
        return false;
    }
    float previousTime = 0.0f;
    bool hasPrevious = false;
    for (const SkeletalVectorKeyframe& keyframe : track.keyframes) {
        if (!validTrackTime(keyframe.timeSeconds, durationSeconds, previousTime, hasPrevious)
                || !finiteVector(keyframe.value)
                || std::fabs(keyframe.value[0]) > kMaximumAuthoredValue
                || std::fabs(keyframe.value[1]) > kMaximumAuthoredValue
                || std::fabs(keyframe.value[2]) > kMaximumAuthoredValue) {
            error = "Skeletal animation vector track contains invalid keyframes";
            return false;
        }
        previousTime = keyframe.timeSeconds;
        hasPrevious = true;
    }
    return true;
}

bool validateQuaternionTrack(const SkeletalQuaternionTrack& track, float durationSeconds,
                             std::string& error) {
    if (track.keyframes.size() > kMaximumTrackKeyframes) {
        error = "Skeletal animation quaternion track exceeds the keyframe limit";
        return false;
    }
    float previousTime = 0.0f;
    bool hasPrevious = false;
    for (const SkeletalQuaternionKeyframe& keyframe : track.keyframes) {
        if (!validTrackTime(keyframe.timeSeconds, durationSeconds, previousTime, hasPrevious)
                || !validQuaternion(keyframe.value)) {
            error = "Skeletal animation quaternion track contains invalid keyframes";
            return false;
        }
        previousTime = keyframe.timeSeconds;
        hasPrevious = true;
    }
    return true;
}
}  // namespace

bool SkeletalVectorTrack::addKeyframe(float timeSeconds,
                                      const std::array<float, 3>& value) {
    if (!std::isfinite(timeSeconds) || timeSeconds < 0.0f || !finiteVector(value)) return false;
    const auto position = std::lower_bound(keyframes.begin(), keyframes.end(), timeSeconds,
            [](const SkeletalVectorKeyframe& keyframe, float time) {
                return keyframe.timeSeconds < time;
            });
    if (position != keyframes.end() && position->timeSeconds == timeSeconds) {
        position->value = value;
    } else {
        if (keyframes.size() >= kMaximumTrackKeyframes) return false;
        keyframes.insert(position, {timeSeconds, value});
    }
    return true;
}

std::array<float, 3> SkeletalVectorTrack::sample(
        float timeSeconds, const std::array<float, 3>& fallback) const {
    if (!std::isfinite(timeSeconds) || keyframes.empty()) return fallback;
    return sampleVectorTrack(*this, timeSeconds, fallback);
}

bool SkeletalQuaternionTrack::addKeyframe(float timeSeconds,
                                          const SkeletalQuaternion& value) {
    if (!std::isfinite(timeSeconds) || timeSeconds < 0.0f || !validQuaternion(value)) return false;
    const auto position = std::lower_bound(keyframes.begin(), keyframes.end(), timeSeconds,
            [](const SkeletalQuaternionKeyframe& keyframe, float time) {
                return keyframe.timeSeconds < time;
            });
    const SkeletalQuaternion normalized = normalizeQuaternion(value);
    if (position != keyframes.end() && position->timeSeconds == timeSeconds) {
        position->value = normalized;
    } else {
        if (keyframes.size() >= kMaximumTrackKeyframes) return false;
        keyframes.insert(position, {timeSeconds, normalized});
    }
    return true;
}

SkeletalQuaternion SkeletalQuaternionTrack::sample(
        float timeSeconds, const SkeletalQuaternion& fallback) const {
    if (!std::isfinite(timeSeconds) || keyframes.empty()) {
        return validQuaternion(fallback) ? normalizeQuaternion(fallback) : SkeletalQuaternion{};
    }
    if (timeSeconds <= keyframes.front().timeSeconds) {
        return normalizeQuaternion(keyframes.front().value);
    }
    if (timeSeconds >= keyframes.back().timeSeconds) {
        return normalizeQuaternion(keyframes.back().value);
    }
    const auto upper = std::upper_bound(keyframes.begin(), keyframes.end(), timeSeconds,
            [](float time, const SkeletalQuaternionKeyframe& keyframe) {
                return time < keyframe.timeSeconds;
            });
    const SkeletalQuaternionKeyframe& right = *upper;
    const SkeletalQuaternionKeyframe& left = *(upper - 1);
    const float span = right.timeSeconds - left.timeSeconds;
    if (span <= 0.0f) return normalizeQuaternion(right.value);
    return interpolateQuaternion(left.value, right.value,
                                 (timeSeconds - left.timeSeconds) / span);
}

bool validateSkeletalAnimation(const std::vector<SkeletalJoint>& skeleton,
                               const SkeletalAnimationClip& clip,
                               std::string& error) {
    if (skeleton.empty() || skeleton.size() > kMaximumJoints
            || clip.joints.size() != skeleton.size()
            || !std::isfinite(clip.durationSeconds) || clip.durationSeconds < 0.0f
            || clip.durationSeconds > kMaximumAuthoredValue) {
        error = "Skeleton or animation clip header is invalid";
        return false;
    }
    for (std::size_t index = 0; index < skeleton.size(); ++index) {
        const SkeletalJoint& joint = skeleton[index];
        if (joint.name.size() > 1024 || joint.parentIndex < -1
                || joint.parentIndex >= static_cast<std::int32_t>(index)
                || !finiteVector(joint.bindTranslation) || !finiteVector(joint.bindScale)
                || !validQuaternion(joint.bindRotation)) {
            error = "Skeleton contains an invalid joint or non-parent-first hierarchy";
            return false;
        }
        for (float value : joint.bindTranslation) {
            if (std::fabs(value) > kMaximumAuthoredValue) {
                error = "Skeleton bind translation exceeds the supported range";
                return false;
            }
        }
        for (float value : joint.bindScale) {
            if (std::fabs(value) > kMaximumAuthoredValue) {
                error = "Skeleton bind scale exceeds the supported range";
                return false;
            }
        }
        for (float value : joint.inverseBindMatrix) {
            if (!std::isfinite(value) || std::fabs(value) > kMaximumAuthoredValue) {
                error = "Skeleton inverse bind matrix is invalid";
                return false;
            }
        }
    }
    std::size_t totalKeyframes = 0;
    constexpr std::size_t kMaximumClipKeyframes = 65536;
    for (const SkeletalAnimationTrack& track : clip.joints) {
        if (track.translation.keyframes.size() > kMaximumClipKeyframes - totalKeyframes) {
            error = "Skeletal animation clip exceeds the total keyframe limit";
            return false;
        }
        totalKeyframes += track.translation.keyframes.size();
        if (track.rotation.keyframes.size() > kMaximumClipKeyframes - totalKeyframes) {
            error = "Skeletal animation clip exceeds the total keyframe limit";
            return false;
        }
        totalKeyframes += track.rotation.keyframes.size();
        if (track.scale.keyframes.size() > kMaximumClipKeyframes - totalKeyframes) {
            error = "Skeletal animation clip exceeds the total keyframe limit";
            return false;
        }
        totalKeyframes += track.scale.keyframes.size();
        if (!validateVectorTrack(track.translation, clip.durationSeconds, error)
                || !validateQuaternionTrack(track.rotation, clip.durationSeconds, error)
                || !validateVectorTrack(track.scale, clip.durationSeconds, error)) return false;
    }
    error.clear();
    return true;
}

bool skinVertexReference(const SkeletalPose& pose,
                         const std::array<float, 3>& position,
                         const std::array<float, 3>& normal,
                         const SkeletalVertexInfluence& influences,
                         SkinnedVertexPose& output, std::string& error) {
    if (pose.jointMatrices.empty() || !finiteVector(position) || !finiteVector(normal)) {
        error = "Skinned vertex inputs are invalid";
        return false;
    }
    float weightSum = 0.0f;
    for (std::size_t influence = 0; influence < influences.weights.size(); ++influence) {
        const float weight = influences.weights[influence];
        if (!std::isfinite(weight) || weight < 0.0f) {
            error = "Skinned vertex weights must be finite and non-negative";
            return false;
        }
        if (weight == 0.0f) continue;
        if (influences.jointIndices[influence] >= pose.jointMatrices.size()) {
            error = "Skinned vertex references a joint outside the pose";
            return false;
        }
        weightSum += weight;
    }
    if (!std::isfinite(weightSum) || weightSum <= 1.0e-8f) {
        error = "Skinned vertex has no effective joint weight";
        return false;
    }

    SkeletalMatrix4 blended{};
    for (std::size_t influence = 0; influence < influences.weights.size(); ++influence) {
        const float weight = influences.weights[influence];
        if (weight == 0.0f) continue;
        const SkeletalMatrix4& matrix = pose.jointMatrices[influences.jointIndices[influence]];
        const float normalizedWeight = weight / weightSum;
        for (std::size_t element = 0; element < blended.size(); ++element) {
            if (!std::isfinite(matrix[element])) {
                error = "Skinning pose contains a non-finite joint matrix";
                return false;
            }
            blended[element] += matrix[element] * normalizedWeight;
        }
    }

    const float homogeneousW = blended[3] * position[0] + blended[7] * position[1]
            + blended[11] * position[2] + blended[15];
    if (!std::isfinite(homogeneousW) || std::fabs(homogeneousW) <= 1.0e-8f) {
        error = "Skinned vertex produced an invalid homogeneous coordinate";
        return false;
    }
    SkinnedVertexPose candidate;
    candidate.position = {
            (blended[0] * position[0] + blended[4] * position[1]
                    + blended[8] * position[2] + blended[12]) / homogeneousW,
            (blended[1] * position[0] + blended[5] * position[1]
                    + blended[9] * position[2] + blended[13]) / homogeneousW,
            (blended[2] * position[0] + blended[6] * position[1]
                    + blended[10] * position[2] + blended[14]) / homogeneousW};
    const std::array<float, 3> transformedNormal{
            blended[0] * normal[0] + blended[4] * normal[1] + blended[8] * normal[2],
            blended[1] * normal[0] + blended[5] * normal[1] + blended[9] * normal[2],
            blended[2] * normal[0] + blended[6] * normal[1] + blended[10] * normal[2]};
    const float normalLengthSquared = transformedNormal[0] * transformedNormal[0]
            + transformedNormal[1] * transformedNormal[1]
            + transformedNormal[2] * transformedNormal[2];
    if (!finiteVector(candidate.position) || !finiteVector(transformedNormal)
            || !std::isfinite(normalLengthSquared) || normalLengthSquared <= 1.0e-12f) {
        error = "Skinned vertex produced invalid position or normal data";
        return false;
    }
    const float inverseNormalLength = 1.0f / std::sqrt(normalLengthSquared);
    candidate.normal = {transformedNormal[0] * inverseNormalLength,
            transformedNormal[1] * inverseNormalLength,
            transformedNormal[2] * inverseNormalLength};
    output = candidate;
    error.clear();
    return true;
}

bool evaluateSkeletalPose(const std::vector<SkeletalJoint>& skeleton,
                          const SkeletalAnimationClip& clip, float timeSeconds,
                          SkeletalPose& output, std::string& error) {
    if (!std::isfinite(timeSeconds) || timeSeconds < 0.0f
            || !validateSkeletalAnimation(skeleton, clip, error)) {
        if (error.empty()) error = "Skeletal animation time is invalid";
        return false;
    }
    float sampleTime = timeSeconds;
    if (clip.durationSeconds > 0.0f) {
        if (clip.looping) {
            sampleTime = std::fmod(sampleTime, clip.durationSeconds);
        } else {
            sampleTime = std::min(sampleTime, clip.durationSeconds);
        }
    } else {
        sampleTime = 0.0f;
    }

    SkeletalPose candidate;
    candidate.jointMatrices.resize(skeleton.size());
    std::vector<SkeletalMatrix4> worldMatrices(skeleton.size());
    for (std::size_t index = 0; index < skeleton.size(); ++index) {
        const SkeletalJoint& joint = skeleton[index];
        const SkeletalAnimationTrack& track = clip.joints[index];
        const std::array<float, 3> translation = track.translation.sample(
                sampleTime, joint.bindTranslation);
        const SkeletalQuaternion rotation = track.rotation.sample(
                sampleTime, joint.bindRotation);
        const std::array<float, 3> scale = track.scale.sample(sampleTime, joint.bindScale);
        const SkeletalMatrix4 local = composeTransform(translation, rotation, scale);
        worldMatrices[index] = joint.parentIndex < 0 ? local
                : multiplyMatrix(worldMatrices[static_cast<std::size_t>(joint.parentIndex)], local);
        candidate.jointMatrices[index] = multiplyMatrix(worldMatrices[index],
                                                        joint.inverseBindMatrix);
        for (float value : candidate.jointMatrices[index]) {
            if (!std::isfinite(value)) {
                error = "Evaluated skeletal pose contains a non-finite matrix";
                return false;
            }
        }
    }
    output = std::move(candidate);
    error.clear();
    return true;
}

}  // namespace nuby
