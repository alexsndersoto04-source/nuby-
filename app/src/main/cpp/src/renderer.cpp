#include "nuby/renderer.hpp"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <string>
#include <unordered_set>
#include <utility>

#include <android/log.h>

namespace nuby {
using Vertex = MeshVertex;

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

void appendTriangle(std::vector<MeshVertex>& vertices,
                    const MeshVertex& a,
                    const MeshVertex& b,
                    const MeshVertex& c) {
    vertices.push_back(a);
    vertices.push_back(b);
    vertices.push_back(c);
}

constexpr const char* kVertexShader = R"(
#version 300 es
precision highp float;
layout(location = 0) in vec3 aPosition;
layout(location = 1) in vec3 aNormal;
layout(location = 2) in vec2 aUv;
uniform mat4 uViewProjection;
uniform mat4 uLightViewProjection;
uniform mat4 uModel;
out vec3 vWorldPosition;
out vec3 vWorldNormal;
out vec2 vUv;
out vec4 vLightPosition;
void main() {
    vec4 world = uModel * vec4(aPosition, 1.0);
    vWorldPosition = world.xyz;
    vWorldNormal = mat3(uModel) * aNormal;
    vUv = aUv;
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
uniform int uPointLightCount;
uniform vec4 uPointLightPositionRange[4];
uniform vec4 uPointLightColorIntensity[4];
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

vec3 evaluateDirectLight(vec3 normal, vec3 viewDirection, vec3 lightDirection,
                         vec3 baseColor, float metallic, float roughness, vec3 radiance) {
    float normalLight = max(dot(normal, lightDirection), 0.0);
    if (normalLight <= 0.0) return vec3(0.0);

    vec3 halfDirection = normalize(lightDirection + viewDirection);
    float normalView = max(dot(normal, viewDirection), 0.001);
    float normalHalf = max(dot(normal, halfDirection), 0.0);
    float viewHalf = max(dot(viewDirection, halfDirection), 0.0);
    vec3 baseReflectance = mix(vec3(0.04), baseColor, metallic);
    vec3 fresnel = fresnelSchlick(viewHalf, baseReflectance);
    float distribution = distributionGGX(normalHalf, roughness);
    float geometryTerm = geometrySchlickGGX(normalView, roughness)
                       * geometrySchlickGGX(max(normalLight, 0.001), roughness);
    vec3 specular = (distribution * geometryTerm * fresnel)
                  / max(4.0 * normalView * max(normalLight, 0.001), 0.001);
    vec3 diffuseWeight = (vec3(1.0) - fresnel) * (1.0 - metallic);
    return (diffuseWeight * baseColor / 3.14159265 + specular)
            * radiance * normalLight;
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
    float roughness = clamp(uRoughness, 0.06, 1.0);
    float metallic = clamp(uMetallic, 0.0, 1.0);
    vec3 directLight = evaluateDirectLight(normal, viewDirection, lightDirection,
            uColor, metallic, roughness, vec3(2.3, 2.15, 1.88)) * visibility;

    for (int index = 0; index < 4; ++index) {
        if (index >= uPointLightCount) break;
        vec4 positionRange = uPointLightPositionRange[index];
        vec3 toLight = positionRange.xyz - vWorldPosition;
        float distanceSquared = max(dot(toLight, toLight), 0.04);
        float distanceToLight = sqrt(distanceSquared);
        float rangeFraction = distanceToLight / max(positionRange.w, 0.01);
        float rangeFade = clamp(1.0 - rangeFraction * rangeFraction
                                      * rangeFraction * rangeFraction, 0.0, 1.0);
        float attenuation = rangeFade * rangeFade / distanceSquared;
        vec4 colorIntensity = uPointLightColorIntensity[index];
        vec3 pointRadiance = colorIntensity.rgb * colorIntensity.a * attenuation;
        directLight += evaluateDirectLight(normal, viewDirection,
                toLight / max(distanceToLight, 0.001), uColor, metallic, roughness,
                pointRadiance);
    }

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

constexpr const char* kFullscreenVertexShader = R"(
#version 300 es
precision highp float;
layout(location = 0) in vec2 aPosition;
layout(location = 1) in vec2 aUv;
out vec2 vUv;
void main() {
    vUv = aUv;
    gl_Position = vec4(aPosition, 0.0, 1.0);
}
)";

constexpr const char* kBloomExtractFragmentShader = R"(
#version 300 es
precision highp float;
in vec2 vUv;
uniform sampler2D uScene;
out vec4 fragmentColor;
void main() {
    vec3 color = texture(uScene, vUv).rgb;
    float brightness = max(max(color.r, color.g), color.b);
    float contribution = max(brightness - 0.72, 0.0) / max(brightness, 0.001);
    fragmentColor = vec4(color * contribution, 1.0);
}
)";

constexpr const char* kBlurFragmentShader = R"(
#version 300 es
precision highp float;
in vec2 vUv;
uniform sampler2D uSource;
uniform vec2 uDirection;
uniform vec2 uTexelSize;
out vec4 fragmentColor;
void main() {
    vec3 result = texture(uSource, vUv).rgb * 0.227027;
    result += texture(uSource, vUv + uDirection * uTexelSize * 1.384615).rgb * 0.316216;
    result += texture(uSource, vUv - uDirection * uTexelSize * 1.384615).rgb * 0.316216;
    result += texture(uSource, vUv + uDirection * uTexelSize * 3.230769).rgb * 0.070270;
    result += texture(uSource, vUv - uDirection * uTexelSize * 3.230769).rgb * 0.070270;
    fragmentColor = vec4(result, 1.0);
}
)";

constexpr const char* kCompositeFragmentShader = R"(
#version 300 es
precision highp float;
in vec2 vUv;
uniform sampler2D uScene;
uniform sampler2D uBloom;
out vec4 fragmentColor;
void main() {
    vec3 sceneColor = texture(uScene, vUv).rgb;
    vec3 bloomColor = texture(uBloom, vUv).rgb;
    fragmentColor = vec4(clamp(sceneColor + bloomColor * 0.32, 0.0, 1.0), 1.0);
}
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
    glEnableVertexAttribArray(2);
    glVertexAttribPointer(2, 2, GL_FLOAT, GL_FALSE, sizeof(Vertex),
                          reinterpret_cast<const void*>(offsetof(Vertex, uv)));
    glBindVertexArray(0);
    mesh.vertexCount = static_cast<GLsizei>(vertices.size());
    return mesh;
}

bool Renderer::loadObjAsset(const std::string& assetPath, Mesh& mesh) {
    if (assetManager_ == nullptr) {
        __android_log_print(ANDROID_LOG_WARN, kLogTag,
                            "Cannot load %s: Android asset manager is unavailable", assetPath.c_str());
        return false;
    }

    AAsset* asset = AAssetManager_open(assetManager_, assetPath.c_str(), AASSET_MODE_BUFFER);
    if (asset == nullptr) {
        __android_log_print(ANDROID_LOG_ERROR, kLogTag,
                            "OBJ asset not found: %s", assetPath.c_str());
        return false;
    }

    constexpr off_t kMaximumAssetBytes = 16 * 1024 * 1024;
    const off_t length = AAsset_getLength(asset);
    if (length <= 0 || length > kMaximumAssetBytes) {
        AAsset_close(asset);
        __android_log_print(ANDROID_LOG_ERROR, kLogTag,
                            "OBJ asset has invalid size: %s", assetPath.c_str());
        return false;
    }

    std::string source(static_cast<std::size_t>(length), '\0');
    std::size_t bytesRead = 0;
    while (bytesRead < source.size()) {
        const int count = AAsset_read(asset, source.data() + bytesRead,
                                      source.size() - bytesRead);
        if (count <= 0) break;
        bytesRead += static_cast<std::size_t>(count);
    }
    AAsset_close(asset);
    if (bytesRead != source.size()) {
        __android_log_print(ANDROID_LOG_ERROR, kLogTag,
                            "Could not read complete OBJ asset: %s", assetPath.c_str());
        return false;
    }

    std::vector<MeshVertex> vertices;
    std::string error;
    if (!parseObj(source, vertices, error)) {
        __android_log_print(ANDROID_LOG_ERROR, kLogTag,
                "OBJ parse failed for %s: %s", assetPath.c_str(), error.c_str());
        return false;
    }
    mesh = uploadMesh(vertices);
    __android_log_print(ANDROID_LOG_INFO, kLogTag,
            "Loaded OBJ %s (%zu triangle vertices)", assetPath.c_str(), vertices.size());
    return mesh.vao != 0;
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
    scene_.physicsSettings().floorEnabled = true;
    scene_.physicsSettings().floorHeight = -1.22f;
    scene_.physicsSettings().floorRestitution = 0.38f;
    scene_.physicsSettings().floorFriction = 0.48f;
    sphere_ = makeSphere(28, 40);
    torus_ = makeTorus(64, 14, 1.0f, 0.025f);
    ground_ = makeGround();
    importedMeshes_.clear();

    Entity& camera = scene_.create("Main Camera", Primitive::None);
    camera.visible = false;
    camera.camera.enabled = true;
    camera.orbitCamera.enabled = true;
    scene_.setActiveCamera(camera.id);

    Entity& ground = scene_.create("Horizon Grid", Primitive::Ground);
    ground.material.color = {0.02f, 0.04f, 0.08f};
    ground.material.proceduralGrid = true;

    Entity& firstRing = scene_.create("Aether Ring", Primitive::Torus);
    firstRing.transform.position = {0.0f, 0.05f, 0.0f};
    firstRing.transform.rotation[0] = 1.02f;
    firstRing.spin.radiansPerSecond[1] = 0.21f;
    firstRing.material.color = {0.10f, 0.75f, 0.78f};
    firstRing.material.emission = {0.02f, 0.34f, 0.38f};
    firstRing.animation.enabled = true;
    firstRing.animation.durationSeconds = 4.0f;
    firstRing.animation.position.addKeyframe(0.0f, {0.0f, 0.05f, 0.0f});
    firstRing.animation.position.addKeyframe(2.0f, {0.0f, 0.12f, 0.0f});
    firstRing.animation.position.addKeyframe(4.0f, {0.0f, 0.05f, 0.0f});
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

    Entity& physicsOrb = scene_.create("Physics Sample", Primitive::Sphere);
    physicsOrb.transform.position = {1.0f, 0.48f, 0.7f};
    physicsOrb.transform.scale = {0.19f, 0.19f, 0.19f};
    physicsOrb.rigidbody.enabled = true;
    physicsOrb.rigidbody.mass = 0.8f;
    physicsOrb.rigidbody.linearDamping = 0.06f;
    physicsOrb.sphereCollider.enabled = true;
    physicsOrb.sphereCollider.radius = 1.0f;
    physicsOrb.sphereCollider.restitution = 0.52f;
    physicsOrb.sphereCollider.friction = 0.42f;
    physicsOrb.material.color = {0.96f, 0.55f, 0.20f};
    physicsOrb.material.emission = {0.32f, 0.09f, 0.018f};
    physicsOrb.material.metallic = 0.26f;
    physicsOrb.material.roughness = 0.24f;

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
    const std::uint32_t coreId = core.id;

    Entity& innerCore = scene_.create("Core Light", Primitive::Sphere);
    innerCore.transform.scale = {0.36f, 0.36f, 0.36f};
    scene_.setParent(innerCore.id, coreId);
    innerCore.pulse.enabled = true;
    innerCore.pulse.baseScale = innerCore.transform.scale;
    innerCore.pulse.amplitude = 0.10f;
    innerCore.pulse.frequency = 0.42f;
    innerCore.material.color = {0.16f, 0.78f, 0.88f};
    innerCore.material.emission = {0.20f, 0.74f, 0.88f};
    innerCore.material.metallic = 0.08f;
    innerCore.material.roughness = 0.16f;

    for (int index = 0; index < 3; ++index) {
        const bool useImportedMesh = index == 1;
        Entity& satellite = scene_.create("Orbiting Shard " + std::to_string(index + 1),
                useImportedMesh ? Primitive::ImportedMesh : Primitive::Sphere);
        if (useImportedMesh) satellite.meshAsset = "meshes/orbital_shard.obj";
        const float size = useImportedMesh ? 0.13f : 0.075f + 0.018f * static_cast<float>(index);
        const float orbit = 1.46f + 0.08f * static_cast<float>(index);
        satellite.transform.scale = {size, size, size};
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

    Entity& cyanLight = scene_.create("Aether Point Light", Primitive::None);
    cyanLight.visible = false;
    cyanLight.pointLight.enabled = true;
    cyanLight.pointLight.color = {0.10f, 0.62f, 1.0f};
    cyanLight.pointLight.intensity = 6.5f;
    cyanLight.pointLight.range = 5.0f;
    cyanLight.orbit.enabled = true;
    cyanLight.orbit.radius = 1.55f;
    cyanLight.orbit.angularSpeed = 0.34f;
    cyanLight.orbit.verticalAmplitude = 0.62f;
    cyanLight.orbit.verticalFrequency = 0.72f;

    Entity& emberLight = scene_.create("Ember Point Light", Primitive::None);
    emberLight.visible = false;
    emberLight.transform.position = {-1.35f, 0.48f, -0.82f};
    emberLight.pointLight.enabled = true;
    emberLight.pointLight.color = {1.0f, 0.23f, 0.075f};
    emberLight.pointLight.intensity = 4.5f;
    emberLight.pointLight.range = 4.2f;

    loadSceneAssets();
}

void Renderer::loadSceneAssets() {
    std::unordered_set<std::string> attempted;
    for (const Entity& entity : scene_.entities()) {
        if (entity.primitive != Primitive::ImportedMesh || entity.meshAsset.empty()
                || !attempted.insert(entity.meshAsset).second) {
            continue;
        }
        Mesh mesh{};
        if (loadObjAsset(entity.meshAsset, mesh)) {
            importedMeshes_.emplace(entity.meshAsset, mesh);
        }
    }
}

const Renderer::Mesh* Renderer::meshFor(const Entity& entity) const {
    switch (entity.primitive) {
        case Primitive::Sphere: return &sphere_;
        case Primitive::Torus: return &torus_;
        case Primitive::Ground: return &ground_;
        case Primitive::ImportedMesh: {
            const auto found = importedMeshes_.find(entity.meshAsset);
            return found == importedMeshes_.end() ? &sphere_ : &found->second;
        }
        case Primitive::None: return nullptr;
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
        const Mesh* mesh = meshFor(entity);
        if (mesh == nullptr || mesh->vao == 0) continue;
        const Mat4 model = scene_.worldMatrix(entity.id);
        glUniformMatrix4fv(shadowProgramModel_, 1, GL_FALSE, model.data());
        glBindVertexArray(mesh->vao);
        glDrawArrays(GL_TRIANGLES, 0, mesh->vertexCount);
    }

    glBindVertexArray(0);
    glDisable(GL_POLYGON_OFFSET_FILL);
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    glViewport(0, 0, width_, height_);
}

void Renderer::initializePostProcessing() {
    bloomExtractProgram_ = createProgram(kFullscreenVertexShader, kBloomExtractFragmentShader);
    blurProgram_ = createProgram(kFullscreenVertexShader, kBlurFragmentShader);
    compositeProgram_ = createProgram(kFullscreenVertexShader, kCompositeFragmentShader);
    if (bloomExtractProgram_ == 0 || blurProgram_ == 0 || compositeProgram_ == 0) {
        __android_log_print(ANDROID_LOG_WARN, kLogTag,
                            "Post-processing shaders unavailable; rendering directly to screen");
        return;
    }

    postUniforms_.extractScene = glGetUniformLocation(bloomExtractProgram_, "uScene");
    postUniforms_.blurSource = glGetUniformLocation(blurProgram_, "uSource");
    postUniforms_.blurDirection = glGetUniformLocation(blurProgram_, "uDirection");
    postUniforms_.blurTexelSize = glGetUniformLocation(blurProgram_, "uTexelSize");
    postUniforms_.compositeScene = glGetUniformLocation(compositeProgram_, "uScene");
    postUniforms_.compositeBloom = glGetUniformLocation(compositeProgram_, "uBloom");

    constexpr float quadVertices[] = {
        -1.0f, -1.0f, 0.0f, 0.0f,
         1.0f, -1.0f, 1.0f, 0.0f,
         1.0f,  1.0f, 1.0f, 1.0f,
        -1.0f, -1.0f, 0.0f, 0.0f,
         1.0f,  1.0f, 1.0f, 1.0f,
        -1.0f,  1.0f, 0.0f, 1.0f
    };
    glGenVertexArrays(1, &fullscreenVao_);
    glGenBuffers(1, &fullscreenVbo_);
    glBindVertexArray(fullscreenVao_);
    glBindBuffer(GL_ARRAY_BUFFER, fullscreenVbo_);
    glBufferData(GL_ARRAY_BUFFER, sizeof(quadVertices), quadVertices, GL_STATIC_DRAW);
    glEnableVertexAttribArray(0);
    glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 4 * sizeof(float), nullptr);
    glEnableVertexAttribArray(1);
    glVertexAttribPointer(1, 2, GL_FLOAT, GL_FALSE, 4 * sizeof(float),
                          reinterpret_cast<const void*>(2 * sizeof(float)));
    glBindVertexArray(0);
}

void Renderer::releasePostProcessing() {
    if (sceneFramebuffer_ != 0) glDeleteFramebuffers(1, &sceneFramebuffer_);
    if (sceneColorTexture_ != 0) glDeleteTextures(1, &sceneColorTexture_);
    if (sceneDepthBuffer_ != 0) glDeleteRenderbuffers(1, &sceneDepthBuffer_);
    glDeleteFramebuffers(static_cast<GLsizei>(bloomFramebuffers_.size()), bloomFramebuffers_.data());
    glDeleteTextures(static_cast<GLsizei>(bloomTextures_.size()), bloomTextures_.data());
    sceneFramebuffer_ = 0;
    sceneColorTexture_ = 0;
    sceneDepthBuffer_ = 0;
    bloomFramebuffers_ = {0, 0};
    bloomTextures_ = {0, 0};
    postWidth_ = 0;
    postHeight_ = 0;
    postProcessingAvailable_ = false;
}

void Renderer::resizePostProcessing() {
    if (width_ <= 0 || height_ <= 0) return;
    if (postProcessingAvailable_ && sceneColorTexture_ != 0
            && postWidth_ == width_ && postHeight_ == height_) return;
    releasePostProcessing();
    if (bloomExtractProgram_ == 0 || blurProgram_ == 0 || compositeProgram_ == 0
            || fullscreenVao_ == 0) return;

    bloomWidth_ = std::max(width_ / 4, 1);
    bloomHeight_ = std::max(height_ / 4, 1);
    glGenTextures(1, &sceneColorTexture_);
    glBindTexture(GL_TEXTURE_2D, sceneColorTexture_);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, width_, height_, 0,
                 GL_RGBA, GL_UNSIGNED_BYTE, nullptr);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);

    glGenRenderbuffers(1, &sceneDepthBuffer_);
    glBindRenderbuffer(GL_RENDERBUFFER, sceneDepthBuffer_);
    glRenderbufferStorage(GL_RENDERBUFFER, GL_DEPTH_COMPONENT16, width_, height_);

    glGenFramebuffers(1, &sceneFramebuffer_);
    glBindFramebuffer(GL_FRAMEBUFFER, sceneFramebuffer_);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0,
                           GL_TEXTURE_2D, sceneColorTexture_, 0);
    glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_DEPTH_ATTACHMENT,
                              GL_RENDERBUFFER, sceneDepthBuffer_);
    bool complete = glCheckFramebufferStatus(GL_FRAMEBUFFER) == GL_FRAMEBUFFER_COMPLETE;

    glGenFramebuffers(static_cast<GLsizei>(bloomFramebuffers_.size()), bloomFramebuffers_.data());
    glGenTextures(static_cast<GLsizei>(bloomTextures_.size()), bloomTextures_.data());
    for (std::size_t index = 0; index < bloomFramebuffers_.size(); ++index) {
        glBindTexture(GL_TEXTURE_2D, bloomTextures_[index]);
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, bloomWidth_, bloomHeight_, 0,
                     GL_RGBA, GL_UNSIGNED_BYTE, nullptr);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
        glBindFramebuffer(GL_FRAMEBUFFER, bloomFramebuffers_[index]);
        glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0,
                               GL_TEXTURE_2D, bloomTextures_[index], 0);
        complete = complete
                && glCheckFramebufferStatus(GL_FRAMEBUFFER) == GL_FRAMEBUFFER_COMPLETE;
    }

    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    glBindRenderbuffer(GL_RENDERBUFFER, 0);
    glBindTexture(GL_TEXTURE_2D, 0);
    postProcessingAvailable_ = complete;
    if (!postProcessingAvailable_) {
        __android_log_print(ANDROID_LOG_WARN, kLogTag,
                            "Bloom framebuffers unavailable; rendering directly to screen");
        releasePostProcessing();
        return;
    }
    postWidth_ = width_;
    postHeight_ = height_;
    __android_log_print(ANDROID_LOG_INFO, kLogTag,
            "Quarter-resolution bloom enabled (%dx%d)", bloomWidth_, bloomHeight_);
}

void Renderer::drawFullscreen() {
    glBindVertexArray(fullscreenVao_);
    glDrawArrays(GL_TRIANGLES, 0, 6);
    glBindVertexArray(0);
}

void Renderer::renderBloom() {
    if (!postProcessingAvailable_) return;
    glDisable(GL_DEPTH_TEST);
    glDisable(GL_BLEND);
    glViewport(0, 0, bloomWidth_, bloomHeight_);

    glBindFramebuffer(GL_FRAMEBUFFER, bloomFramebuffers_[0]);
    glClear(GL_COLOR_BUFFER_BIT);
    glUseProgram(bloomExtractProgram_);
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, sceneColorTexture_);
    glUniform1i(postUniforms_.extractScene, 0);
    drawFullscreen();

    glUseProgram(blurProgram_);
    glUniform1i(postUniforms_.blurSource, 0);
    glUniform2f(postUniforms_.blurTexelSize,
                1.0f / static_cast<float>(bloomWidth_),
                1.0f / static_cast<float>(bloomHeight_));
    glBindFramebuffer(GL_FRAMEBUFFER, bloomFramebuffers_[1]);
    glBindTexture(GL_TEXTURE_2D, bloomTextures_[0]);
    glUniform2f(postUniforms_.blurDirection, 1.0f, 0.0f);
    drawFullscreen();

    glBindFramebuffer(GL_FRAMEBUFFER, bloomFramebuffers_[0]);
    glBindTexture(GL_TEXTURE_2D, bloomTextures_[1]);
    glUniform2f(postUniforms_.blurDirection, 0.0f, 1.0f);
    drawFullscreen();

    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    glViewport(0, 0, width_, height_);
    glUseProgram(compositeProgram_);
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, sceneColorTexture_);
    glUniform1i(postUniforms_.compositeScene, 0);
    glActiveTexture(GL_TEXTURE1);
    glBindTexture(GL_TEXTURE_2D, bloomTextures_[0]);
    glUniform1i(postUniforms_.compositeBloom, 1);
    drawFullscreen();
    glActiveTexture(GL_TEXTURE0);
    glEnable(GL_DEPTH_TEST);
}

void Renderer::initialize(AAssetManager* assetManager) {
    assetManager_ = assetManager;
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
    uniforms_.pointLightCount = glGetUniformLocation(program_, "uPointLightCount");
    uniforms_.pointLightPositionRange = glGetUniformLocation(program_, "uPointLightPositionRange[0]");
    uniforms_.pointLightColorIntensity = glGetUniformLocation(program_, "uPointLightColorIntensity[0]");

    buildScene();
    initializeShadowMap();
    initializePostProcessing();
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
    resizePostProcessing();
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
    glUniform1i(uniforms_.pointLightCount, static_cast<GLint>(pointLights_.size()));
    glUniform4fv(uniforms_.pointLightPositionRange, kMaxPointLights,
                 pointLightPositionRanges_.data());
    glUniform4fv(uniforms_.pointLightColorIntensity, kMaxPointLights,
                 pointLightColorIntensities_.data());
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
    scene_.collectPointLights(kMaxPointLights, pointLights_);
    pointLightPositionRanges_.fill(0.0f);
    pointLightColorIntensities_.fill(0.0f);
    for (std::size_t index = 0; index < pointLights_.size(); ++index) {
        const PointLightData& light = pointLights_[index];
        const std::size_t offset = index * 4;
        pointLightPositionRanges_[offset] = light.position[0];
        pointLightPositionRanges_[offset + 1] = light.position[1];
        pointLightPositionRanges_[offset + 2] = light.position[2];
        pointLightPositionRanges_[offset + 3] = light.range;
        pointLightColorIntensities_[offset] = light.color[0];
        pointLightColorIntensities_[offset + 1] = light.color[1];
        pointLightColorIntensities_[offset + 2] = light.color[2];
        pointLightColorIntensities_[offset + 3] = light.intensity;
    }

    const Vec3 lightPosition{4.5f, 8.2f, 3.4f};
    const Mat4 lightView = lookAt(lightPosition, {0.0f, 0.0f, 0.0f}, {0.0f, 1.0f, 0.0f});
    const Mat4 lightProjection = orthographic(-4.2f, 4.2f, -4.2f, 4.2f, 0.1f, 20.0f);
    lightViewProjection_ = multiply(lightProjection, lightView);
    renderShadowMap(lightViewProjection_);

    const Entity* cameraEntity = scene_.activeCamera();
    const CameraComponent camera = cameraEntity == nullptr
            ? CameraComponent{} : cameraEntity->camera;
    Mat4 view{};
    if (cameraEntity != nullptr && cameraEntity->orbitCamera.enabled) {
        const OrbitCameraController& orbit = cameraEntity->orbitCamera;
        const float distance = std::max(orbit.distance, 0.1f);
        const float horizontal = std::cos(orbit.pitch) * distance;
        const Vec3 target = orbit.target;
        cameraPosition_ = {target[0] + std::sin(orbit.yaw) * horizontal,
                           target[1] + std::sin(orbit.pitch) * distance + 0.12f,
                           target[2] + std::cos(orbit.yaw) * horizontal};
        view = lookAt(cameraPosition_, target, {0.0f, 1.0f, 0.0f});
    } else if (cameraEntity != nullptr) {
        const Mat4 cameraWorld = scene_.worldMatrix(cameraEntity->id);
        cameraPosition_ = {cameraWorld[12], cameraWorld[13], cameraWorld[14]};
        const Vec3 forward{-cameraWorld[8], -cameraWorld[9], -cameraWorld[10]};
        const Vec3 target{cameraPosition_[0] + forward[0],
                          cameraPosition_[1] + forward[1],
                          cameraPosition_[2] + forward[2]};
        const Vec3 up{cameraWorld[4], cameraWorld[5], cameraWorld[6]};
        view = lookAt(cameraPosition_, target, up);
    } else {
        const OrbitCameraController fallback{};
        const float horizontal = std::cos(fallback.pitch) * fallback.distance;
        cameraPosition_ = {std::sin(fallback.yaw) * horizontal,
                           std::sin(fallback.pitch) * fallback.distance + 0.12f,
                           std::cos(fallback.yaw) * horizontal};
        view = lookAt(cameraPosition_, fallback.target, {0.0f, 1.0f, 0.0f});
    }
    const float nearPlane = std::max(camera.nearPlane, 0.01f);
    const float farPlane = std::max(camera.farPlane, nearPlane + 0.01f);
    const Mat4 projection = perspective(std::clamp(camera.verticalFovRadians, 0.1f, 2.8f),
            static_cast<float>(width_) / static_cast<float>(height_), nearPlane, farPlane);
    viewProjection_ = multiply(projection, view);

    if (postProcessingAvailable_) {
        glBindFramebuffer(GL_FRAMEBUFFER, sceneFramebuffer_);
        glViewport(0, 0, width_, height_);
    } else {
        glBindFramebuffer(GL_FRAMEBUFFER, 0);
        glViewport(0, 0, width_, height_);
    }
    glEnable(GL_DEPTH_TEST);
    glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
    for (const Entity& entity : scene_.entities()) {
        if (!entity.visible) continue;
        const Mesh* mesh = meshFor(entity);
        if (mesh == nullptr) continue;
        draw(*mesh, scene_.worldMatrix(entity.id), entity.material);
    }
    if (postProcessingAvailable_) renderBloom();
}

void Renderer::touch(int action, float x, float y, float pinchDistance, int pointerCount) {
    constexpr int kActionDown = 0;
    constexpr int kActionUp = 1;
    constexpr int kActionMove = 2;
    constexpr int kActionCancel = 3;
    constexpr int kActionPointerDown = 5;
    constexpr int kActionPointerUp = 6;

    if (action == kActionDown || action == kActionPointerDown) {
        lastTouchX_ = x;
        lastTouchY_ = y;
        lastPinchDistance_ = pointerCount >= 2 && pinchDistance > 0.0f
                ? pinchDistance : 0.0f;
        return;
    }
    if (action == kActionUp || action == kActionCancel || action == kActionPointerUp) {
        lastTouchX_ = x;
        lastTouchY_ = y;
        lastPinchDistance_ = 0.0f;
        return;
    }
    if (action != kActionMove) return;

    const float deltaX = x - lastTouchX_;
    const float deltaY = y - lastTouchY_;
    if (pointerCount >= 2 && pinchDistance > 0.0f) {
        if (lastPinchDistance_ > 0.0f) {
            scene_.zoomActiveCamera(lastPinchDistance_ / pinchDistance);
        }
        lastPinchDistance_ = pinchDistance;
    } else {
        lastPinchDistance_ = 0.0f;
        scene_.orbitActiveCamera(deltaX, deltaY);
    }
    lastTouchX_ = x;
    lastTouchY_ = y;
}

}  // namespace nuby
