#include "nuby/renderer.hpp"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <string>
#include <utility>

#include <android/log.h>

namespace nuby {
namespace {

constexpr const char* kLogTag = "NubyEngine";
constexpr float kPi = 3.14159265358979323846f;

using Mat4 = std::array<float, 16>;
using Vec3 = std::array<float, 3>;

Mat4 identity() {
    Mat4 value{};
    value[0] = value[5] = value[10] = value[15] = 1.0f;
    return value;
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

Mat4 translation(float x, float y, float z) {
    Mat4 result = identity();
    result[12] = x;
    result[13] = y;
    result[14] = z;
    return result;
}

Mat4 scale(float x, float y, float z) {
    Mat4 result{};
    result[0] = x;
    result[5] = y;
    result[10] = z;
    result[15] = 1.0f;
    return result;
}

Mat4 rotationX(float angle) {
    Mat4 result = identity();
    const float c = std::cos(angle);
    const float s = std::sin(angle);
    result[5] = c;
    result[9] = -s;
    result[6] = s;
    result[10] = c;
    return result;
}

Mat4 rotationY(float angle) {
    Mat4 result = identity();
    const float c = std::cos(angle);
    const float s = std::sin(angle);
    result[0] = c;
    result[8] = s;
    result[2] = -s;
    result[10] = c;
    return result;
}

Mat4 rotationZ(float angle) {
    Mat4 result = identity();
    const float c = std::cos(angle);
    const float s = std::sin(angle);
    result[0] = c;
    result[4] = -s;
    result[1] = s;
    result[5] = c;
    return result;
}

Vec3 subtract(const Vec3& a, const Vec3& b) {
    return {a[0] - b[0], a[1] - b[1], a[2] - b[2]};
}

Vec3 cross(const Vec3& a, const Vec3& b) {
    return {a[1] * b[2] - a[2] * b[1],
            a[2] * b[0] - a[0] * b[2],
            a[0] * b[1] - a[1] * b[0]};
}

float dot(const Vec3& a, const Vec3& b) {
    return a[0] * b[0] + a[1] * b[1] + a[2] * b[2];
}

Vec3 normalize(const Vec3& value) {
    const float length = std::sqrt(std::max(dot(value, value), 0.000001f));
    return {value[0] / length, value[1] / length, value[2] / length};
}

Mat4 lookAt(const Vec3& eye, const Vec3& center, const Vec3& up) {
    const Vec3 forward = normalize(subtract(center, eye));
    const Vec3 side = normalize(cross(forward, up));
    const Vec3 correctedUp = cross(side, forward);

    Mat4 result = identity();
    result[0] = side[0];
    result[4] = side[1];
    result[8] = side[2];
    result[1] = correctedUp[0];
    result[5] = correctedUp[1];
    result[9] = correctedUp[2];
    result[2] = -forward[0];
    result[6] = -forward[1];
    result[10] = -forward[2];
    result[12] = -dot(side, eye);
    result[13] = -dot(correctedUp, eye);
    result[14] = dot(forward, eye);
    return result;
}

Mat4 perspective(float verticalFov, float aspect, float nearPlane, float farPlane) {
    Mat4 result{};
    const float f = 1.0f / std::tan(verticalFov * 0.5f);
    result[0] = f / aspect;
    result[5] = f;
    result[10] = (farPlane + nearPlane) / (nearPlane - farPlane);
    result[11] = -1.0f;
    result[14] = (2.0f * farPlane * nearPlane) / (nearPlane - farPlane);
    return result;
}

Mat4 orthographic(float left, float right, float bottom, float top,
                  float nearPlane, float farPlane) {
    Mat4 result = identity();
    result[0] = 2.0f / (right - left);
    result[5] = 2.0f / (top - bottom);
    result[10] = -2.0f / (farPlane - nearPlane);
    result[12] = -(right + left) / (right - left);
    result[13] = -(top + bottom) / (top - bottom);
    result[14] = -(farPlane + nearPlane) / (farPlane - nearPlane);
    return result;
}

Mat4 modelMatrix(const Transform& transform) {
    const Mat4 translationMatrix = translation(transform.position[0],
            transform.position[1], transform.position[2]);
    const Mat4 rotationMatrix = multiply(rotationY(transform.rotation[1]),
            multiply(rotationX(transform.rotation[0]), rotationZ(transform.rotation[2])));
    const Mat4 scaleMatrix = scale(transform.scale[0], transform.scale[1], transform.scale[2]);
    return multiply(translationMatrix, multiply(rotationMatrix, scaleMatrix));
}

void appendTriangle(std::vector<Renderer::Vertex>& vertices,
                    const Renderer::Vertex& a,
                    const Renderer::Vertex& b,
                    const Renderer::Vertex& c) {
    vertices.push_back(a);
    vertices.push_back(b);
    vertices.push_back(c);
}

constexpr const char* kVertexShader = R"(
#version 300 es
precision highp float;
layout(location = 0) in vec3 aPosition;
layout(location = 1) in vec3 aNormal;
uniform mat4 uViewProjection;
uniform mat4 uLightViewProjection;
uniform mat4 uModel;
out vec3 vWorldPosition;
out vec3 vWorldNormal;
out vec4 vLightPosition;
void main() {
    vec4 world = uModel * vec4(aPosition, 1.0);
    vWorldPosition = world.xyz;
    vWorldNormal = mat3(uModel) * aNormal;
    vLightPosition = uLightViewProjection * world;
    gl_Position = uViewProjection * world;
}
)";

constexpr const char* kFragmentShader = R"(
#version 300 es
precision highp float;
in vec3 vWorldPosition;
in vec3 vWorldNormal;
in vec4 vLightPosition;
uniform vec3 uColor;
uniform vec3 uEmission;
uniform float uMetallic;
uniform float uRoughness;
uniform float uAmbientOcclusion;
uniform vec3 uCameraPosition;
uniform vec3 uLightDirection;
uniform sampler2D uShadowMap;
uniform int uShadowsEnabled;
uniform int uGrid;
out vec4 fragmentColor;

float shadowAmount(vec3 normal, vec3 lightDirection) {
    if (uShadowsEnabled == 0) return 0.0;
    vec3 projected = vLightPosition.xyz / max(vLightPosition.w, 0.0001);
    projected = projected * 0.5 + 0.5;
    if (projected.x < 0.0 || projected.x > 1.0 ||
        projected.y < 0.0 || projected.y > 1.0 ||
        projected.z < 0.0 || projected.z > 1.0) return 0.0;

    float bias = max(0.0015 * (1.0 - dot(normal, lightDirection)), 0.00045);
    vec2 texel = 1.0 / vec2(textureSize(uShadowMap, 0));
    float occlusion = 0.0;
    for (int y = -1; y <= 1; ++y) {
        for (int x = -1; x <= 1; ++x) {
            float closestDepth = texture(uShadowMap,
                    projected.xy + vec2(float(x), float(y)) * texel).r;
            occlusion += projected.z - bias > closestDepth ? 1.0 : 0.0;
        }
    }
    return occlusion / 9.0;
}

float distributionGGX(float normalHalf, float roughness) {
    const float pi = 3.14159265;
    float alpha = roughness * roughness;
    float alphaSquared = alpha * alpha;
    float denominator = normalHalf * normalHalf * (alphaSquared - 1.0) + 1.0;
    return alphaSquared / max(pi * denominator * denominator, 0.001);
}

float geometrySchlickGGX(float normalDirection, float roughness) {
    float r = roughness + 1.0;
    float k = (r * r) * 0.125;
    return normalDirection / max(normalDirection * (1.0 - k) + k, 0.001);
}

vec3 fresnelSchlick(float viewHalf, vec3 baseReflectance) {
    return baseReflectance + (1.0 - baseReflectance) * pow(1.0 - viewHalf, 5.0);
}

void main() {
    vec3 normal = normalize(vWorldNormal);
    vec3 lightDirection = normalize(-uLightDirection);
    float shadow = shadowAmount(normal, lightDirection);
    float visibility = 1.0 - shadow * 0.82;

    if (uGrid == 1) {
        vec2 coordinate = vWorldPosition.xz * 0.5;
        vec2 derivative = max(fwidth(coordinate), vec2(0.0001));
        vec2 gridDistance = abs(fract(coordinate - 0.5) - 0.5) / derivative;
        float line = 1.0 - min(min(gridDistance.x, gridDistance.y), 1.0);
        float fade = 1.0 - smoothstep(2.0, 15.0, length(vWorldPosition.xz));
        float centerGlow = exp(-length(vWorldPosition.xz) * 0.48);
        vec3 base = vec3(0.012, 0.019, 0.043) * (0.48 + visibility * 0.52);
        vec3 gridColor = vec3(0.12, 0.67, 0.72)
                * (line * fade * 0.48 * (0.38 + visibility * 0.62) + centerGlow * 0.12);
        fragmentColor = vec4(base + gridColor, 1.0);
        return;
    }

    vec3 viewDirection = normalize(uCameraPosition - vWorldPosition);
    vec3 halfDirection = normalize(lightDirection + viewDirection);
    float normalLight = max(dot(normal, lightDirection), 0.0);
    float normalView = max(dot(normal, viewDirection), 0.001);
    float normalHalf = max(dot(normal, halfDirection), 0.0);
    float viewHalf = max(dot(viewDirection, halfDirection), 0.0);
    float roughness = clamp(uRoughness, 0.06, 1.0);
    float metallic = clamp(uMetallic, 0.0, 1.0);

    vec3 baseReflectance = mix(vec3(0.04), uColor, metallic);
    vec3 fresnel = fresnelSchlick(viewHalf, baseReflectance);
    float distribution = distributionGGX(normalHalf, roughness);
    float geometryTerm = geometrySchlickGGX(normalView, roughness)
                       * geometrySchlickGGX(max(normalLight, 0.001), roughness);
    vec3 specular = (distribution * geometryTerm * fresnel)
                  / max(4.0 * normalView * max(normalLight, 0.001), 0.001);
    vec3 diffuseWeight = (vec3(1.0) - fresnel) * (1.0 - metallic);
    vec3 radiance = vec3(2.3, 2.15, 1.88);
    vec3 directLight = (diffuseWeight * uColor / 3.14159265 + specular)
                     * radiance * normalLight * visibility;

    float skyAmount = clamp(normal.y * 0.5 + 0.5, 0.0, 1.0);
    vec3 ambientLight = mix(vec3(0.075, 0.085, 0.14),
                            vec3(0.22, 0.29, 0.46), skyAmount);
    vec3 ambient = uColor * ambientLight * uAmbientOcclusion * (1.0 - metallic * 0.55);
    vec3 color = ambient + directLight + uEmission;
    color = (color * (2.51 * color + 0.03))
          / max(color * (2.43 * color + 0.59) + 0.14, vec3(0.001));
    color = pow(clamp(color, 0.0, 1.0), vec3(1.0 / 2.2));
    fragmentColor = vec4(color, 1.0);
}
)";

constexpr const char* kShadowVertexShader = R"(
#version 300 es
precision highp float;
layout(location = 0) in vec3 aPosition;
uniform mat4 uLightViewProjection;
uniform mat4 uModel;
void main() {
    gl_Position = uLightViewProjection * uModel * vec4(aPosition, 1.0);
}
)";

constexpr const char* kShadowFragmentShader = R"(
#version 300 es
precision highp float;
void main() { }
)";

}  // namespace

GLuint Renderer::compileShader(GLenum type, const char* source) {
    const GLuint shader = glCreateShader(type);
    glShaderSource(shader, 1, &source, nullptr);
    glCompileShader(shader);

    GLint compiled = GL_FALSE;
    glGetShaderiv(shader, GL_COMPILE_STATUS, &compiled);
    if (compiled == GL_FALSE) {
        GLint logLength = 0;
        glGetShaderiv(shader, GL_INFO_LOG_LENGTH, &logLength);
        std::string log(static_cast<std::size_t>(std::max(logLength, 1)), '\0');
        glGetShaderInfoLog(shader, logLength, nullptr, log.data());
        __android_log_print(ANDROID_LOG_ERROR, kLogTag, "Shader compile failed: %s", log.c_str());
        glDeleteShader(shader);
        return 0;
    }
    return shader;
}

GLuint Renderer::createProgram(const char* vertexSource, const char* fragmentSource) {
    const GLuint vertex = compileShader(GL_VERTEX_SHADER, vertexSource);
    const GLuint fragment = compileShader(GL_FRAGMENT_SHADER, fragmentSource);
    if (vertex == 0 || fragment == 0) {
        if (vertex != 0) glDeleteShader(vertex);
        if (fragment != 0) glDeleteShader(fragment);
        return 0;
    }

    const GLuint program = glCreateProgram();
    glAttachShader(program, vertex);
    glAttachShader(program, fragment);
    glLinkProgram(program);
    glDeleteShader(vertex);
    glDeleteShader(fragment);

    GLint linked = GL_FALSE;
    glGetProgramiv(program, GL_LINK_STATUS, &linked);
    if (linked == GL_FALSE) {
        GLint logLength = 0;
        glGetProgramiv(program, GL_INFO_LOG_LENGTH, &logLength);
        std::string log(static_cast<std::size_t>(std::max(logLength, 1)), '\0');
        glGetProgramInfoLog(program, logLength, nullptr, log.data());
        __android_log_print(ANDROID_LOG_ERROR, kLogTag, "Program link failed: %s", log.c_str());
        glDeleteProgram(program);
        return 0;
    }
    return program;
}

Renderer::Mesh Renderer::uploadMesh(const std::vector<Vertex>& vertices) {
    Mesh mesh{};
    if (vertices.empty()) return mesh;

    glGenVertexArrays(1, &mesh.vao);
    glGenBuffers(1, &mesh.vbo);
    glBindVertexArray(mesh.vao);
    glBindBuffer(GL_ARRAY_BUFFER, mesh.vbo);
    glBufferData(GL_ARRAY_BUFFER,
                 static_cast<GLsizeiptr>(vertices.size() * sizeof(Vertex)),
                 vertices.data(), GL_STATIC_DRAW);
    glEnableVertexAttribArray(0);
    glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, sizeof(Vertex),
                          reinterpret_cast<const void*>(offsetof(Vertex, position)));
    glEnableVertexAttribArray(1);
    glVertexAttribPointer(1, 3, GL_FLOAT, GL_FALSE, sizeof(Vertex),
                          reinterpret_cast<const void*>(offsetof(Vertex, normal)));
    glBindVertexArray(0);
    mesh.vertexCount = static_cast<GLsizei>(vertices.size());
    return mesh;
}

Renderer::Mesh Renderer::makeSphere(int latitudeSegments, int longitudeSegments) {
    std::vector<Vertex> vertices;
    vertices.reserve(static_cast<std::size_t>(latitudeSegments * longitudeSegments * 6));

    const auto vertexAt = [](float latitude, float longitude) {
        const float sinLatitude = std::sin(latitude);
        const float x = sinLatitude * std::cos(longitude);
        const float y = std::cos(latitude);
        const float z = sinLatitude * std::sin(longitude);
        return Vertex{{x, y, z}, {x, y, z}};
    };

    for (int lat = 0; lat < latitudeSegments; ++lat) {
        const float lat0 = kPi * static_cast<float>(lat) / static_cast<float>(latitudeSegments);
        const float lat1 = kPi * static_cast<float>(lat + 1) / static_cast<float>(latitudeSegments);
        for (int lon = 0; lon < longitudeSegments; ++lon) {
            const float lon0 = 2.0f * kPi * static_cast<float>(lon) / static_cast<float>(longitudeSegments);
            const float lon1 = 2.0f * kPi * static_cast<float>(lon + 1) / static_cast<float>(longitudeSegments);
            const Vertex a = vertexAt(lat0, lon0);
            const Vertex b = vertexAt(lat1, lon0);
            const Vertex c = vertexAt(lat1, lon1);
            const Vertex d = vertexAt(lat0, lon1);
            appendTriangle(vertices, a, b, c);
            appendTriangle(vertices, a, c, d);
        }
    }
    return uploadMesh(vertices);
}

Renderer::Mesh Renderer::makeTorus(int ringSegments, int tubeSegments,
                                    float majorRadius, float minorRadius) {
    std::vector<Vertex> vertices;
    vertices.reserve(static_cast<std::size_t>(ringSegments * tubeSegments * 6));

    const auto vertexAt = [majorRadius, minorRadius](float ring, float tube) {
        const float cosTube = std::cos(tube);
        const float sinTube = std::sin(tube);
        const float cosRing = std::cos(ring);
        const float sinRing = std::sin(ring);
        const float radial = majorRadius + minorRadius * cosTube;
        return Vertex{{radial * cosRing, minorRadius * sinTube, radial * sinRing},
                      {cosTube * cosRing, sinTube, cosTube * sinRing}};
    };

    for (int ring = 0; ring < ringSegments; ++ring) {
        const float ring0 = 2.0f * kPi * static_cast<float>(ring) / static_cast<float>(ringSegments);
        const float ring1 = 2.0f * kPi * static_cast<float>(ring + 1) / static_cast<float>(ringSegments);
        for (int tube = 0; tube < tubeSegments; ++tube) {
            const float tube0 = 2.0f * kPi * static_cast<float>(tube) / static_cast<float>(tubeSegments);
            const float tube1 = 2.0f * kPi * static_cast<float>(tube + 1) / static_cast<float>(tubeSegments);
            const Vertex a = vertexAt(ring0, tube0);
            const Vertex b = vertexAt(ring1, tube0);
            const Vertex c = vertexAt(ring1, tube1);
            const Vertex d = vertexAt(ring0, tube1);
            appendTriangle(vertices, a, b, c);
            appendTriangle(vertices, a, c, d);
        }
    }
    return uploadMesh(vertices);
}

Renderer::Mesh Renderer::makeGround() {
    const float extent = 32.0f;
    const Vertex a{{-extent, -1.22f, -extent}, {0.0f, 1.0f, 0.0f}};
    const Vertex b{{ extent, -1.22f, -extent}, {0.0f, 1.0f, 0.0f}};
    const Vertex c{{ extent, -1.22f,  extent}, {0.0f, 1.0f, 0.0f}};
    const Vertex d{{-extent, -1.22f,  extent}, {0.0f, 1.0f, 0.0f}};
    std::vector<Vertex> vertices;
    vertices.reserve(6);
    appendTriangle(vertices, a, c, b);
    appendTriangle(vertices, a, d, c);
    return uploadMesh(vertices);
}

void Renderer::buildScene() {
    scene_.clear();
    sphere_ = makeSphere(28, 40);
    torus_ = makeTorus(64, 14, 1.0f, 0.025f);
    ground_ = makeGround();

    Entity& ground = scene_.create("Horizon Grid", Primitive::Ground);
    ground.material.color = {0.02f, 0.04f, 0.08f};
    ground.material.proceduralGrid = true;

    Entity& firstRing = scene_.create("Aether Ring", Primitive::Torus);
    firstRing.transform.position = {0.0f, 0.05f, 0.0f};
    firstRing.transform.rotation[0] = 1.02f;
    firstRing.spin.radiansPerSecond[1] = 0.21f;
    firstRing.material.color = {0.10f, 0.75f, 0.78f};
    firstRing.material.emission = {0.02f, 0.34f, 0.38f};
    firstRing.material.metallic = 0.58f;
    firstRing.material.roughness = 0.24f;

    Entity& secondRing = scene_.create("Halo Ring", Primitive::Torus);
    secondRing.transform.position = {0.0f, 0.02f, 0.0f};
    secondRing.transform.rotation[0] = -0.72f;
    secondRing.transform.scale = {1.28f, 1.28f, 1.28f};
    secondRing.spin.radiansPerSecond[1] = -0.15f;
    secondRing.material.color = {0.92f, 0.48f, 0.22f};
    secondRing.material.emission = {0.26f, 0.08f, 0.015f};
    secondRing.material.metallic = 0.72f;
    secondRing.material.roughness = 0.27f;

    Entity& contactHalo = scene_.create("Ground Halo", Primitive::Torus);
    contactHalo.transform.position = {0.0f, -1.16f, 0.0f};
    contactHalo.transform.scale = {1.52f, 1.52f, 1.52f};
    contactHalo.material.color = {0.10f, 0.42f, 0.50f};
    contactHalo.material.emission = {0.015f, 0.09f, 0.12f};
    contactHalo.material.metallic = 0.35f;
    contactHalo.material.roughness = 0.38f;

    Entity& core = scene_.create("Aether Core", Primitive::Sphere);
    core.transform.scale = {0.70f, 0.70f, 0.70f};
    core.spin.radiansPerSecond[1] = 0.18f;
    core.orbit.enabled = true;
    core.orbit.verticalAmplitude = 0.07f;
    core.orbit.angularSpeed = 1.15f;
    core.material.color = {0.29f, 0.25f, 0.78f};
    core.material.emission = {0.035f, 0.025f, 0.16f};
    core.material.metallic = 0.18f;
    core.material.roughness = 0.31f;

    Entity& innerCore = scene_.create("Core Light", Primitive::Sphere);
    innerCore.transform.scale = {0.25f, 0.25f, 0.25f};
    innerCore.orbit.enabled = true;
    innerCore.orbit.verticalAmplitude = 0.07f;
    innerCore.orbit.angularSpeed = 1.15f;
    innerCore.pulse.enabled = true;
    innerCore.pulse.baseScale = innerCore.transform.scale;
    innerCore.pulse.amplitude = 0.10f;
    innerCore.pulse.frequency = 0.42f;
    innerCore.material.color = {0.16f, 0.78f, 0.88f};
    innerCore.material.emission = {0.20f, 0.74f, 0.88f};
    innerCore.material.metallic = 0.08f;
    innerCore.material.roughness = 0.16f;

    for (int index = 0; index < 3; ++index) {
        Entity& satellite = scene_.create("Orbiting Shard " + std::to_string(index + 1),
                                          Primitive::Sphere);
        const float orbit = 1.46f + 0.08f * static_cast<float>(index);
        satellite.transform.scale = {0.075f + 0.018f * static_cast<float>(index),
                                     0.075f + 0.018f * static_cast<float>(index),
                                     0.075f + 0.018f * static_cast<float>(index)};
        satellite.orbit.enabled = true;
        satellite.orbit.radius = orbit;
        satellite.orbit.angularSpeed = 0.48f + 0.08f * static_cast<float>(index);
        satellite.orbit.phase = 2.0f * kPi * static_cast<float>(index) / 3.0f;
        satellite.orbit.verticalAmplitude = 0.44f;
        satellite.orbit.verticalFrequency = 1.7f;
        if (index == 1) {
            satellite.material.color = {0.95f, 0.43f, 0.20f};
        } else {
            satellite.material.color = {0.18f, 0.79f, 0.78f};
        }
        satellite.material.emission = {satellite.material.color[0] * 0.28f,
                                       satellite.material.color[1] * 0.20f,
                                       satellite.material.color[2] * 0.16f};
        satellite.material.metallic = 0.24f;
        satellite.material.roughness = 0.29f;
    }
}

const Renderer::Mesh* Renderer::meshFor(Primitive primitive) const {
    switch (primitive) {
        case Primitive::Sphere: return &sphere_;
        case Primitive::Torus: return &torus_;
        case Primitive::Ground: return &ground_;
    }
    return nullptr;
}

void Renderer::initializeShadowMap() {
    shadowProgram_ = createProgram(kShadowVertexShader, kShadowFragmentShader);
    if (shadowProgram_ == 0) {
        __android_log_print(ANDROID_LOG_WARN, kLogTag,
                            "Shadow shaders unavailable; continuing without dynamic shadows");
        return;
    }
    shadowProgramLightViewProjection_ = glGetUniformLocation(shadowProgram_, "uLightViewProjection");
    shadowProgramModel_ = glGetUniformLocation(shadowProgram_, "uModel");

    glGenTextures(1, &shadowDepthTexture_);
    glBindTexture(GL_TEXTURE_2D, shadowDepthTexture_);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_DEPTH_COMPONENT16,
                 kShadowMapSize, kShadowMapSize, 0,
                 GL_DEPTH_COMPONENT, GL_UNSIGNED_SHORT, nullptr);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);

    glGenFramebuffers(1, &shadowFramebuffer_);
    glBindFramebuffer(GL_FRAMEBUFFER, shadowFramebuffer_);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_DEPTH_ATTACHMENT,
                           GL_TEXTURE_2D, shadowDepthTexture_, 0);
    const GLenum noColorBuffer = GL_NONE;
    glDrawBuffers(1, &noColorBuffer);
    glReadBuffer(GL_NONE);

    const GLenum status = glCheckFramebufferStatus(GL_FRAMEBUFFER);
    shadowsAvailable_ = status == GL_FRAMEBUFFER_COMPLETE;
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    glBindTexture(GL_TEXTURE_2D, 0);

    if (!shadowsAvailable_) {
        __android_log_print(ANDROID_LOG_WARN, kLogTag,
                "Shadow framebuffer unavailable (0x%x); using unshadowed lighting", status);
        if (shadowFramebuffer_ != 0) glDeleteFramebuffers(1, &shadowFramebuffer_);
        if (shadowDepthTexture_ != 0) glDeleteTextures(1, &shadowDepthTexture_);
        shadowFramebuffer_ = 0;
        shadowDepthTexture_ = 0;
        return;
    }

    __android_log_print(ANDROID_LOG_INFO, kLogTag,
                        "Dynamic shadow map enabled at %dx%d", kShadowMapSize, kShadowMapSize);
}

void Renderer::renderShadowMap(const Mat4& lightViewProjection) {
    if (!shadowsAvailable_ || shadowProgram_ == 0) return;

    glBindFramebuffer(GL_FRAMEBUFFER, shadowFramebuffer_);
    glViewport(0, 0, kShadowMapSize, kShadowMapSize);
    glClear(GL_DEPTH_BUFFER_BIT);
    glEnable(GL_DEPTH_TEST);
    glEnable(GL_POLYGON_OFFSET_FILL);
    glPolygonOffset(1.5f, 2.5f);
    glUseProgram(shadowProgram_);
    glUniformMatrix4fv(shadowProgramLightViewProjection_, 1, GL_FALSE,
                       lightViewProjection.data());

    for (const Entity& entity : scene_.entities()) {
        if (!entity.visible || entity.primitive == Primitive::Ground) continue;
        const Mesh* mesh = meshFor(entity.primitive);
        if (mesh == nullptr || mesh->vao == 0) continue;
        const Mat4 model = modelMatrix(entity.transform);
        glUniformMatrix4fv(shadowProgramModel_, 1, GL_FALSE, model.data());
        glBindVertexArray(mesh->vao);
        glDrawArrays(GL_TRIANGLES, 0, mesh->vertexCount);
    }

    glBindVertexArray(0);
    glDisable(GL_POLYGON_OFFSET_FILL);
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    glViewport(0, 0, width_, height_);
}

void Renderer::initialize() {
    program_ = createProgram(kVertexShader, kFragmentShader);
    if (program_ == 0) {
        __android_log_print(ANDROID_LOG_ERROR, kLogTag, "Renderer initialization failed: shader program unavailable");
        return;
    }

    uniforms_.viewProjection = glGetUniformLocation(program_, "uViewProjection");
    uniforms_.model = glGetUniformLocation(program_, "uModel");
    uniforms_.color = glGetUniformLocation(program_, "uColor");
    uniforms_.emission = glGetUniformLocation(program_, "uEmission");
    uniforms_.metallic = glGetUniformLocation(program_, "uMetallic");
    uniforms_.roughness = glGetUniformLocation(program_, "uRoughness");
    uniforms_.ambientOcclusion = glGetUniformLocation(program_, "uAmbientOcclusion");
    uniforms_.cameraPosition = glGetUniformLocation(program_, "uCameraPosition");
    uniforms_.lightDirection = glGetUniformLocation(program_, "uLightDirection");
    uniforms_.lightViewProjection = glGetUniformLocation(program_, "uLightViewProjection");
    uniforms_.shadowMap = glGetUniformLocation(program_, "uShadowMap");
    uniforms_.shadowsEnabled = glGetUniformLocation(program_, "uShadowsEnabled");
    uniforms_.grid = glGetUniformLocation(program_, "uGrid");

    buildScene();
    initializeShadowMap();
    glEnable(GL_DEPTH_TEST);
    glDepthFunc(GL_LEQUAL);
    glDisable(GL_CULL_FACE);
    glDisable(GL_BLEND);
    glClearColor(0.025f, 0.035f, 0.085f, 1.0f);
    __android_log_print(ANDROID_LOG_INFO, kLogTag, "Nuby GLES 3 renderer initialized");
}

void Renderer::resize(int width, int height) {
    width_ = std::max(width, 1);
    height_ = std::max(height, 1);
    glViewport(0, 0, width_, height_);
}

void Renderer::draw(const Mesh& mesh, const Mat4& model, const Material& material) {
    if (program_ == 0 || mesh.vao == 0) return;

    glUseProgram(program_);
    glUniformMatrix4fv(uniforms_.viewProjection, 1, GL_FALSE, viewProjection_.data());
    glUniformMatrix4fv(uniforms_.lightViewProjection, 1, GL_FALSE,
                       lightViewProjection_.data());
    glUniformMatrix4fv(uniforms_.model, 1, GL_FALSE, model.data());
    glUniform3fv(uniforms_.color, 1, material.color.data());
    glUniform3fv(uniforms_.emission, 1, material.emission.data());
    glUniform1f(uniforms_.metallic, material.metallic);
    glUniform1f(uniforms_.roughness, material.roughness);
    glUniform1f(uniforms_.ambientOcclusion, material.ambientOcclusion);
    glUniform3fv(uniforms_.cameraPosition, 1, cameraPosition_.data());
    glUniform3f(uniforms_.lightDirection, -0.45f, -0.82f, -0.34f);
    glUniform1i(uniforms_.shadowMap, 0);
    glUniform1i(uniforms_.shadowsEnabled, shadowsAvailable_ ? 1 : 0);
    glUniform1i(uniforms_.grid, material.proceduralGrid ? 1 : 0);
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, shadowDepthTexture_);

    glBindVertexArray(mesh.vao);
    glDrawArrays(GL_TRIANGLES, 0, mesh.vertexCount);
    glBindVertexArray(0);
}

void Renderer::render(float deltaSeconds) {
    if (program_ == 0) {
        glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
        return;
    }

    const float dt = std::clamp(deltaSeconds, 0.0f, 0.05f);
    scene_.update(dt);

    const Vec3 lightPosition{4.5f, 8.2f, 3.4f};
    const Mat4 lightView = lookAt(lightPosition, {0.0f, 0.0f, 0.0f}, {0.0f, 1.0f, 0.0f});
    const Mat4 lightProjection = orthographic(-4.2f, 4.2f, -4.2f, 4.2f, 0.1f, 20.0f);
    lightViewProjection_ = multiply(lightProjection, lightView);
    renderShadowMap(lightViewProjection_);

    const float distance = 5.4f;
    const float horizontal = std::cos(pitch_) * distance;
    cameraPosition_ = {std::sin(yaw_) * horizontal,
                       std::sin(pitch_) * distance + 0.12f,
                       std::cos(yaw_) * horizontal};
    const Mat4 view = lookAt(cameraPosition_, {0.0f, 0.0f, 0.0f}, {0.0f, 1.0f, 0.0f});
    const Mat4 projection = perspective(0.76f,
            static_cast<float>(width_) / static_cast<float>(height_), 0.1f, 80.0f);
    viewProjection_ = multiply(projection, view);

    glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
    for (const Entity& entity : scene_.entities()) {
        if (!entity.visible) continue;
        const Mesh* mesh = meshFor(entity.primitive);
        if (mesh == nullptr) continue;
        draw(*mesh, modelMatrix(entity.transform), entity.material);
    }
}

void Renderer::touch(int action, float x, float y) {
    if (action == 0) {
        lastTouchX_ = x;
        lastTouchY_ = y;
        return;
    }
    if (action == 2) {
        const float deltaX = x - lastTouchX_;
        const float deltaY = y - lastTouchY_;
        yaw_ += deltaX * 0.006f;
        pitch_ = std::clamp(pitch_ + deltaY * 0.004f, -0.62f, 0.62f);
        lastTouchX_ = x;
        lastTouchY_ = y;
    }
}

}  // namespace nuby
