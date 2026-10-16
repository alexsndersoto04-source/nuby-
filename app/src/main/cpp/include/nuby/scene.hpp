#pragma once

#include <array>
#include <cstdint>
#include <string>
#include <vector>

namespace nuby {

enum class Primitive : std::uint8_t {
    Sphere,
    Torus,
    Ground,
    ImportedMesh
};

struct Transform {
    std::array<float, 3> position{0.0f, 0.0f, 0.0f};
    std::array<float, 3> rotation{0.0f, 0.0f, 0.0f};
    std::array<float, 3> scale{1.0f, 1.0f, 1.0f};
};

struct Material {
    std::array<float, 3> color{1.0f, 1.0f, 1.0f};
    std::array<float, 3> emission{0.0f, 0.0f, 0.0f};
    float metallic = 0.0f;
    float roughness = 0.58f;
    float ambientOcclusion = 1.0f;
    bool proceduralGrid = false;
};

struct SpinComponent {
    std::array<float, 3> radiansPerSecond{0.0f, 0.0f, 0.0f};
};

struct OrbitComponent {
    bool enabled = false;
    std::array<float, 3> center{0.0f, 0.0f, 0.0f};
    float radius = 0.0f;
    float angularSpeed = 0.0f;
    float phase = 0.0f;
    float verticalAmplitude = 0.0f;
    float verticalFrequency = 1.0f;
};

struct PulseComponent {
    bool enabled = false;
    std::array<float, 3> baseScale{1.0f, 1.0f, 1.0f};
    float amplitude = 0.0f;
    float frequency = 1.0f;
    float phase = 0.0f;
};

struct Entity {
    std::uint32_t id = 0;
    std::string name;
    std::string meshAsset;
    Primitive primitive = Primitive::Sphere;
    Transform transform;
    Material material;
    SpinComponent spin;
    OrbitComponent orbit;
    PulseComponent pulse;
    bool visible = true;
};

// Small data-oriented scene container. Rendering and game systems consume the
// same entities; it is intentionally independent of Android and GLES.
class Scene final {
public:
    Entity& create(std::string name, Primitive primitive);
    void update(float deltaSeconds);
    void clear();

    const std::vector<Entity>& entities() const { return entities_; }
    std::vector<Entity>& entities() { return entities_; }
    float elapsedSeconds() const { return elapsedSeconds_; }

private:
    std::vector<Entity> entities_;
    std::uint32_t nextId_ = 1;
    float elapsedSeconds_ = 0.0f;
};

}  // namespace nuby
