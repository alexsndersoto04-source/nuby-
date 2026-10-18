#include "nuby/skeletal_animation.hpp"

#include <cmath>
#include <cstdlib>
#include <iostream>
#include <limits>
#include <string>
#include <vector>

namespace {
int failures = 0;

void expect(bool condition, const char* message) {
    if (!condition) {
        std::cerr << "FAIL: " << message << '\n';
        ++failures;
    }
}

bool near(float actual, float expected, float epsilon = 0.0001f) {
    return std::fabs(actual - expected) <= epsilon;
}

nuby::SkeletalMatrix4 translated(float x, float y, float z) {
    nuby::SkeletalMatrix4 matrix{
        1.0f, 0.0f, 0.0f, 0.0f,
        0.0f, 1.0f, 0.0f, 0.0f,
        0.0f, 0.0f, 1.0f, 0.0f,
        0.0f, 0.0f, 0.0f, 1.0f};
    matrix[12] = x;
    matrix[13] = y;
    matrix[14] = z;
    return matrix;
}

void testParentedPoseAndInverseBind() {
    std::vector<nuby::SkeletalJoint> skeleton(2);
    skeleton[0].name = "root";
    skeleton[1].name = "child";
    skeleton[1].parentIndex = 0;
    skeleton[1].bindTranslation = {1.0f, 0.0f, 0.0f};
    skeleton[1].inverseBindMatrix = translated(-1.0f, 0.0f, 0.0f);

    nuby::SkeletalAnimationClip clip;
    clip.durationSeconds = 2.0f;
    clip.looping = false;
    clip.joints.resize(skeleton.size());
    expect(clip.joints[0].translation.addKeyframe(2.0f, {2.0f, 0.0f, 0.0f}),
           "translation keys may be authored out of order");
    expect(clip.joints[0].translation.addKeyframe(0.0f, {0.0f, 0.0f, 0.0f}),
           "root translation start key is accepted");
    expect(clip.joints[0].rotation.addKeyframe(0.0f, {}),
           "identity quaternion key is accepted");
    const float halfSqrt = std::sqrt(0.5f);
    expect(clip.joints[0].rotation.addKeyframe(2.0f, {0.0f, 0.0f, halfSqrt, halfSqrt}),
           "quarter-turn quaternion key is accepted");

    nuby::SkeletalPose pose;
    std::string error;
    expect(nuby::evaluateSkeletalPose(skeleton, clip, 0.0f, pose, error),
           "bind-time skeletal pose evaluates");
    expect(pose.jointMatrices.size() == 2
                   && near(pose.jointMatrices[0][12], 0.0f)
                   && near(pose.jointMatrices[1][12], 0.0f)
                   && near(pose.jointMatrices[1][13], 0.0f)
                   && near(pose.jointMatrices[1][0], 1.0f)
                   && near(pose.jointMatrices[1][5], 1.0f),
           "inverse-bind matrices map the bind pose back to mesh space");

    expect(nuby::evaluateSkeletalPose(skeleton, clip, 1.0f, pose, error),
           "interpolated hierarchical pose evaluates");
    expect(near(pose.jointMatrices[0][12], 1.0f)
                   && near(pose.jointMatrices[1][12], 1.0f)
                   && near(pose.jointMatrices[1][13], 0.0f)
                   && near(pose.jointMatrices[1][0], halfSqrt)
                   && near(pose.jointMatrices[1][1], halfSqrt),
           "parent translation and quaternion rotation propagate to child skin matrices");

    expect(nuby::evaluateSkeletalPose(skeleton, clip, 5.0f, pose, error),
           "non-looping animation clamps beyond its end");
    expect(near(pose.jointMatrices[0][12], 2.0f)
                   && near(pose.jointMatrices[1][12], 2.0f)
                   && near(pose.jointMatrices[1][0], 0.0f, 0.001f)
                   && near(pose.jointMatrices[1][1], 1.0f),
           "non-looping clip holds its final translation and rotation");
}

void testLoopingAndQuaternionShortestPath() {
    nuby::SkeletalQuaternionTrack rotations;
    expect(rotations.addKeyframe(1.0f, {0.0f, 0.0f, 0.0f, -1.0f}),
           "negative identity quaternion key is normalized");
    expect(rotations.addKeyframe(0.0f, {0.0f, 0.0f, 0.0f, 1.0f}),
           "rotation keys remain ordered when inserted in reverse");
    const nuby::SkeletalQuaternion middle = rotations.sample(0.5f, {});
    expect(near(middle.x, 0.0f) && near(middle.y, 0.0f)
                   && near(middle.z, 0.0f) && near(std::fabs(middle.w), 1.0f),
           "slerp chooses the shortest quaternion arc without a full spin");

    std::vector<nuby::SkeletalJoint> skeleton(1);
    nuby::SkeletalAnimationClip clip;
    clip.durationSeconds = 2.0f;
    clip.looping = true;
    clip.joints.resize(1);
    clip.joints[0].translation.addKeyframe(0.0f, {0.0f, 0.0f, 0.0f});
    clip.joints[0].translation.addKeyframe(2.0f, {2.0f, 0.0f, 0.0f});
    nuby::SkeletalPose pose;
    std::string error;
    expect(nuby::evaluateSkeletalPose(skeleton, clip, 2.5f, pose, error),
           "looping clip evaluates beyond its duration");
    expect(near(pose.jointMatrices[0][12], 0.5f),
           "looping clip wraps sample time before interpolating");
}

void testCrossfadeAndNonLoopingEvaluation() {
    std::vector<nuby::SkeletalJoint> skeleton(1);
    skeleton[0].name = "root";
    nuby::SkeletalAnimationClip fromClip;
    fromClip.durationSeconds = 1.0f;
    fromClip.joints.resize(1);
    fromClip.joints[0].translation.addKeyframe(0.0f, {0.0f, 0.0f, 0.0f});
    fromClip.joints[0].translation.addKeyframe(1.0f, {0.0f, 0.0f, 0.0f});
    fromClip.joints[0].rotation.addKeyframe(0.0f, {0.0f, 0.0f, 0.0f, 1.0f});
    fromClip.joints[0].rotation.addKeyframe(1.0f, {0.0f, 0.0f, 0.0f, 1.0f});

    nuby::SkeletalAnimationClip toClip;
    toClip.durationSeconds = 1.0f;
    toClip.looping = true;
    toClip.joints.resize(1);
    toClip.joints[0].translation.addKeyframe(0.0f, {0.0f, 0.0f, 0.0f});
    toClip.joints[0].translation.addKeyframe(1.0f, {2.0f, 0.0f, 0.0f});
    toClip.joints[0].rotation.addKeyframe(0.0f, {0.0f, 0.0f, 0.0f, 1.0f});
    toClip.joints[0].rotation.addKeyframe(1.0f, {0.0f, 0.0f, 1.0f, 0.0f});

    nuby::SkeletalPose pose;
    std::string error;
    expect(nuby::evaluateBlendedSkeletalPose(skeleton, fromClip, 0.0f, true,
                   toClip, 1.0f, false, 0.5f, pose, error),
           "two clips can be evaluated as one crossfaded pose");
    expect(pose.jointMatrices.size() == 1
                   && near(pose.jointMatrices[0][12], 1.0f)
                   && near(pose.jointMatrices[0][0], 0.0f, 0.001f)
                   && near(pose.jointMatrices[0][1], 1.0f, 0.001f),
           "crossfade blends local translation and slerps rotation before composition");

    expect(nuby::evaluateSkeletalPose(skeleton, toClip, 1.0f, false, pose, error)
                   && pose.jointMatrices.size() == 1
                   && near(pose.jointMatrices[0][12], 2.0f),
           "non-looping playback holds the final jump pose at clip duration");
    expect(nuby::evaluateSkeletalPose(skeleton, toClip, 1.0f, true, pose, error)
                   && pose.jointMatrices.size() == 1
                   && near(pose.jointMatrices[0][12], 0.0f),
           "looping playback wraps the same clip at its duration");

    const float preservedX = pose.jointMatrices.empty() ? 0.0f : pose.jointMatrices[0][12];
    expect(!nuby::evaluateBlendedSkeletalPose(skeleton, fromClip, 0.0f, true,
                   toClip, 1.0f, false, 1.1f, pose, error),
           "blend amounts outside the normalized range are rejected");
    expect(pose.jointMatrices.size() == 1 && near(pose.jointMatrices[0][12], preservedX),
           "failed blend evaluation leaves the caller's previous pose intact");
}

void testLinearBlendSkinningReference() {
    nuby::SkeletalPose pose;
    pose.jointMatrices = {
        translated(2.0f, 0.0f, 0.0f),
        nuby::SkeletalMatrix4{
            0.0f, 1.0f, 0.0f, 0.0f,
            -1.0f, 0.0f, 0.0f, 0.0f,
            0.0f, 0.0f, 1.0f, 0.0f,
            0.0f, 0.0f, 0.0f, 1.0f}
    };
    nuby::SkeletalVertexInfluence influences;
    influences.jointIndices = {0, 1, 60000, 0};
    influences.weights = {1.0f, 3.0f, 0.0f, 0.0f};
    nuby::SkinnedVertexPose vertex;
    std::string error;
    expect(nuby::skinVertexReference(pose, {1.0f, 0.0f, 0.0f},
                                     {1.0f, 0.0f, 0.0f}, influences, vertex, error),
           "four-influence CPU reference accepts normalized nonzero weights");
    expect(near(vertex.position[0], 0.75f) && near(vertex.position[1], 0.75f)
                   && near(vertex.normal[0], 0.3162278f)
                   && near(vertex.normal[1], 0.9486833f),
           "linear blend skinning weights positions and normalizes the resulting normal");

    const nuby::SkinnedVertexPose preserved = vertex;
    influences.weights[0] = -0.1f;
    expect(!nuby::skinVertexReference(pose, {1.0f, 0.0f, 0.0f},
                                      {1.0f, 0.0f, 0.0f}, influences, vertex, error),
           "negative skin weights are rejected");
    expect(near(vertex.position[0], preserved.position[0])
                   && near(vertex.normal[1], preserved.normal[1]),
           "failed vertex skinning leaves output unchanged");
    influences.weights = {0.0f, 0.0f, 0.0f, 0.0f};
    expect(!nuby::skinVertexReference(pose, {0.0f, 0.0f, 0.0f},
                                      {0.0f, 1.0f, 0.0f}, influences, vertex, error),
           "vertices without effective weights are rejected");
    influences.weights = {1.0f, 0.0f, 0.0f, 0.0f};
    influences.jointIndices[0] = 2;
    expect(!nuby::skinVertexReference(pose, {0.0f, 0.0f, 0.0f},
                                      {0.0f, 1.0f, 0.0f}, influences, vertex, error),
           "out-of-range nonzero joint indices are rejected");
}

void testTrackEditingAndInvalidData() {
    nuby::SkeletalVectorTrack track;
    expect(track.addKeyframe(1.0f, {1.0f, 2.0f, 3.0f}),
           "vector track accepts finite keyframes");
    expect(track.addKeyframe(1.0f, {4.0f, 5.0f, 6.0f}),
           "adding a duplicate time replaces the existing vector key");
    expect(track.keyframes.size() == 1 && near(track.sample(0.0f, {})[0], 4.0f),
           "single-key vector track clamps before its key");
    expect(!track.addKeyframe(-0.1f, {0.0f, 0.0f, 0.0f})
                   && !track.addKeyframe(2.0f, {std::numeric_limits<float>::infinity(),
                                                0.0f, 0.0f}),
           "negative times and non-finite vector values are rejected");

    std::vector<nuby::SkeletalJoint> skeleton(2);
    skeleton[1].parentIndex = 1;
    nuby::SkeletalAnimationClip clip;
    clip.durationSeconds = 1.0f;
    clip.joints.resize(2);
    std::string error;
    expect(!nuby::validateSkeletalAnimation(skeleton, clip, error)
                   && !error.empty(),
           "self-parenting and cyclic non-parent-first joint order are rejected");

    skeleton[1].parentIndex = 0;
    clip.joints.pop_back();
    expect(!nuby::validateSkeletalAnimation(skeleton, clip, error),
           "clip must provide exactly one track per joint");

    clip.joints.resize(2);
    clip.joints[0].rotation.addKeyframe(0.0f,
            {0.0f, 0.0f, 0.0f, std::numeric_limits<float>::quiet_NaN()});
    expect(clip.joints[0].rotation.keyframes.empty(),
           "invalid quaternion animation keys are rejected at insertion");

    nuby::SkeletalPose preserved;
    preserved.jointMatrices.push_back(translated(7.0f, 0.0f, 0.0f));
    expect(!nuby::evaluateSkeletalPose(skeleton, clip, -1.0f, preserved, error),
           "negative playback times are rejected");
    expect(preserved.jointMatrices.size() == 1
                   && near(preserved.jointMatrices[0][12], 7.0f),
           "failed pose evaluation leaves the caller's previous pose intact");
}
}  // namespace

int main() {
    testParentedPoseAndInverseBind();
    testLoopingAndQuaternionShortestPath();
    testCrossfadeAndNonLoopingEvaluation();
    testLinearBlendSkinningReference();
    testTrackEditingAndInvalidData();
    if (failures != 0) {
        std::cerr << failures << " skeletal animation test(s) failed\n";
        return EXIT_FAILURE;
    }
    std::cout << "All skeletal animation tests passed\n";
    return EXIT_SUCCESS;
}
