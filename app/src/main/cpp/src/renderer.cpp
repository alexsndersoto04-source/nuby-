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
uniform mat4 uModel;
out vec3 vWorldPosition;
out vec3 vWorldNormal;
void main() {
    vec4 world = uModel * vec4(aPosition, 1.0);
    vWorldPosition = world.xyz;
    vWorldNormal = mat3(uModel) * aNormal;
    gl_Position = uViewProjection * world;
}
)";

constexpr const char* kFragmentShader = R"(
#version 300 es
precision highp float;
in vec3 vWorldPosition;
in vec3 vWorldNormal;
uniform vec3 uColor;
uniform vec3 uEmission;
uniform vec3 uCameraPosition;
uniform vec3 uLightDirection;
uniform int uGrid;
out vec4 fragmentColor;
void main() {
    if (uGrid == 1) {
        vec2 coordinate = vWorldPosition.xz * 0.5;
        vec2 derivative = max(fwidth(coordinate), vec2(0.0001));
        vec2 gridDistance = abs(fract(coordinate - 0.5) - 0.5) / derivative;
        float line = 1.0 - min(min(gridDistance.x, gridDistance.y), 1.0);
        float fade = 1.0 - smoothstep(2.0, 15.0, length(vWorldPosition.xz));
        float centerGlow = exp(-length(vWorldPosition.xz) * 0.48);
        vec3 base = vec3(0.012, 0.019, 0.043);
        vec3 gridColor = vec3(0.12, 0.67, 0.72) * (line * fade * 0.48 + centerGlow * 0.12);
        fragmentColor = vec4(base + gridColor, 1.0);
        return;
    }

    vec3 normal = normalize(vWorldNormal);
    vec3 lightDirection = normalize(-uLightDirection);
    vec3 viewDirection = normalize(uCameraPosition - vWorldPosition);
    vec3 halfDirection = normalize(lightDirection + viewDirection);
    float diffuse = max(dot(normal, lightDirection), 0.0);
    float specular = pow(max(dot(normal, halfDirection), 0.0), 48.0) * 0.42;
    float fresnel = pow(1.0 - max(dot(normal, viewDirection), 0.0), 3.0);

    vec3 color = uColor * (0.16 + diffuse * 0.88);
    color += specular * vec3(0.78, 0.9, 1.0);
    color += fresnel * vec3(0.08, 0.43, 0.58);
    color += uEmission;
    color = vec3(1.0) - exp(-color * 1.18);
    color = pow(color, vec3(1.0 / 2.2));
    fragmentColor = vec4(color, 1.0);
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
    sphere_ = makeSphere(28, 40);
    torus_ = makeTorus(64, 14, 1.0f, 0.025f);
    ground_ = makeGround();
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
    uniforms_.cameraPosition = glGetUniformLocation(program_, "uCameraPosition");
    uniforms_.lightDirection = glGetUniformLocation(program_, "uLightDirection");
    uniforms_.grid = glGetUniformLocation(program_, "uGrid");

    buildScene();
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

void Renderer::draw(const Mesh& mesh, const Mat4& model, const Vec3& color,
                    const Vec3& emission, bool grid) {
    if (program_ == 0 || mesh.vao == 0) return;

    glUseProgram(program_);
    glUniformMatrix4fv(uniforms_.viewProjection, 1, GL_FALSE, viewProjection_.data());
    glUniformMatrix4fv(uniforms_.model, 1, GL_FALSE, model.data());
    glUniform3fv(uniforms_.color, 1, color.data());
    glUniform3fv(uniforms_.emission, 1, emission.data());
    glUniform3fv(uniforms_.cameraPosition, 1, cameraPosition_.data());
    glUniform3f(uniforms_.lightDirection, -0.45f, -0.82f, -0.34f);
    glUniform1i(uniforms_.grid, grid ? 1 : 0);

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
    elapsed_ += dt;

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
    draw(ground_, identity(), {0.02f, 0.04f, 0.08f}, {0.0f, 0.0f, 0.0f}, true);

    const Mat4 ringOne = multiply(translation(0.0f, 0.05f, 0.0f),
            multiply(rotationY(elapsed_ * 0.21f), rotationX(1.02f)));
    draw(torus_, ringOne, {0.10f, 0.75f, 0.78f}, {0.02f, 0.34f, 0.38f});

    const Mat4 ringTwo = multiply(translation(0.0f, 0.02f, 0.0f),
            multiply(rotationY(-elapsed_ * 0.15f),
                    multiply(rotationX(-0.72f), scale(1.28f, 1.28f, 1.28f))));
    draw(torus_, ringTwo, {0.92f, 0.48f, 0.22f}, {0.26f, 0.08f, 0.015f});

    const Mat4 contactHalo = multiply(translation(0.0f, -1.16f, 0.0f),
                                      scale(1.62f, 0.12f, 1.62f));
    draw(torus_, contactHalo, {0.10f, 0.42f, 0.50f}, {0.015f, 0.09f, 0.12f});

    const float bob = std::sin(elapsed_ * 1.15f) * 0.07f;
    const Mat4 core = multiply(translation(0.0f, bob, 0.0f),
            multiply(rotationY(elapsed_ * 0.18f), scale(0.70f, 0.70f, 0.70f)));
    draw(sphere_, core, {0.29f, 0.25f, 0.78f}, {0.035f, 0.025f, 0.16f});

    const float pulse = 0.25f + 0.025f * std::sin(elapsed_ * 2.6f);
    const Mat4 innerCore = multiply(translation(0.0f, bob, 0.0f), scale(pulse, pulse, pulse));
    draw(sphere_, innerCore, {0.16f, 0.78f, 0.88f}, {0.20f, 0.74f, 0.88f});

    for (int index = 0; index < 3; ++index) {
        const float phase = elapsed_ * (0.48f + 0.08f * static_cast<float>(index))
                            + 2.0f * kPi * static_cast<float>(index) / 3.0f;
        const float orbit = 1.46f + 0.08f * static_cast<float>(index);
        const float x = std::cos(phase) * orbit;
        const float y = std::sin(phase * 1.7f) * 0.44f;
        const float z = std::sin(phase) * orbit;
        const float size = 0.075f + 0.018f * static_cast<float>(index);
        const Mat4 satellite = multiply(translation(x, y, z), scale(size, size, size));
        const Vec3 tint = index == 1 ? Vec3{0.95f, 0.43f, 0.20f} : Vec3{0.18f, 0.79f, 0.78f};
        draw(sphere_, satellite, tint, {tint[0] * 0.28f, tint[1] * 0.20f, tint[2] * 0.16f});
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
