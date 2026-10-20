#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace nuby {

using SkeletalMatrix4 = std::array<float, 16>;

struct SkeletalQuaternion {
    float x = 0.0f;
    float y = 0.0f;
    float z = 0.0f;
    float w = 1.0f;
};

enum class SkeletalInterpolation : std::uint8_t {
    Linear = 0,
    Step = 1,
    CubicSpline = 2
};

struct SkeletalVectorKeyframe {
    float timeSeconds = 0.0f;
    std::array<float, 3> value{0.0f, 0.0f, 0.0f};
    std::array<float, 3> inTangent{0.0f, 0.0f, 0.0f};
    std::array<float, 3> outTangent{0.0f, 0.0f, 0.0f};
};

struct SkeletalQuaternionKeyframe {
    float timeSeconds = 0.0f;
    SkeletalQuaternion value;
    SkeletalQuaternion inTangent{0.0f, 0.0f, 0.0f, 0.0f};
    SkeletalQuaternion outTangent{0.0f, 0.0f, 0.0f, 0.0f};
};

struct SkeletalVectorTrack {
    SkeletalInterpolation interpolation = SkeletalInterpolation::Linear;
    std::vector<SkeletalVectorKeyframe> keyframes;

    bool addKeyframe(float timeSeconds, const std::array<float, 3>& value,
                     const std::array<float, 3>& inTangent = {0.0f, 0.0f, 0.0f},
                     const std::array<float, 3>& outTangent = {0.0f, 0.0f, 0.0f});
    std::array<float, 3> sample(float timeSeconds,
                                const std::array<float, 3>& fallback) const;
};

struct SkeletalQuaternionTrack {
    SkeletalInterpolation interpolation = SkeletalInterpolation::Linear;
    std::vector<SkeletalQuaternionKeyframe> keyframes;

    bool addKeyframe(float timeSeconds, const SkeletalQuaternion& value,
                     const SkeletalQuaternion& inTangent = {0.0f, 0.0f, 0.0f, 0.0f},
                     const SkeletalQuaternion& outTangent = {0.0f, 0.0f, 0.0f, 0.0f});
    SkeletalQuaternion sample(float timeSeconds,
                              const SkeletalQuaternion& fallback) const;
};

struct SkeletalJoint {
    std::string name;
    // Joints are stored parent-first. A root has parentIndex == -1.
    std::int32_t parentIndex = -1;
    std::array<float, 3> bindTranslation{0.0f, 0.0f, 0.0f};
    SkeletalQuaternion bindRotation;
    std::array<float, 3> bindScale{1.0f, 1.0f, 1.0f};
    // Matrix-authored static nodes preserve affine transforms that TRS cannot encode.
    // Such nodes cannot also carry animated TRS tracks.
    bool bindMatrixAuthored = false;
    SkeletalMatrix4 bindLocalMatrix{
        1.0f, 0.0f, 0.0f, 0.0f,
        0.0f, 1.0f, 0.0f, 0.0f,
        0.0f, 0.0f, 1.0f, 0.0f,
        0.0f, 0.0f, 0.0f, 1.0f};
    SkeletalMatrix4 inverseBindMatrix{
        1.0f, 0.0f, 0.0f, 0.0f,
        0.0f, 1.0f, 0.0f, 0.0f,
        0.0f, 0.0f, 1.0f, 0.0f,
        0.0f, 0.0f, 0.0f, 1.0f};
};

struct SkeletalAnimationTrack {
    SkeletalVectorTrack translation;
    SkeletalQuaternionTrack rotation;
    SkeletalVectorTrack scale;
};

struct SkeletalAnimationClip {
    float durationSeconds = 0.0f;
    bool looping = true;
    std::vector<SkeletalAnimationTrack> joints;
};

struct SkeletalPose {
    // Column-major matrices mapping mesh-space positions to the animated pose.
    std::vector<SkeletalMatrix4> jointMatrices;
};

struct SkeletalVertexInfluence {
    std::array<std::uint16_t, 4> jointIndices{0, 0, 0, 0};
    std::array<float, 4> weights{1.0f, 0.0f, 0.0f, 0.0f};
};

struct SkinnedVertexPose {
    std::array<float, 3> position{0.0f, 0.0f, 0.0f};
    std::array<float, 3> normal{0.0f, 1.0f, 0.0f};
};

// CPU reference implementation for four-weight linear-blend skinning. It is
// intended for importer tests and as a correctness oracle for GPU skinning.
bool skinVertexReference(const SkeletalPose& pose,
                         const std::array<float, 3>& position,
                         const std::array<float, 3>& normal,
                         const SkeletalVertexInfluence& influences,
                         SkinnedVertexPose& output, std::string& error);

// Validates a parent-first skeleton and a clip with one track per joint.
// At most 64 joints and 16,384 keyframes per individual track are supported.
bool validateSkeletalAnimation(const std::vector<SkeletalJoint>& skeleton,
                               const SkeletalAnimationClip& clip,
                               std::string& error);

// Evaluates parented TRS animation using each track's interpolation mode and
// computes mesh-space skin matrices (animated world matrix * inverse bind).
bool evaluateSkeletalPose(const std::vector<SkeletalJoint>& skeleton,
                          const SkeletalAnimationClip& clip, float timeSeconds,
                          SkeletalPose& output, std::string& error);
bool evaluateSkeletalPose(const std::vector<SkeletalJoint>& skeleton,
                          const SkeletalAnimationClip& clip, float timeSeconds,
                          bool looping, SkeletalPose& output, std::string& error);
bool evaluateBlendedSkeletalPose(const std::vector<SkeletalJoint>& skeleton,
                                 const SkeletalAnimationClip& fromClip,
                                 float fromTimeSeconds, bool fromLooping,
                                 const SkeletalAnimationClip& toClip,
                                 float toTimeSeconds, bool toLooping,
                                 float blendAmount, SkeletalPose& output,
                                 std::string& error);

}  // namespace nuby
