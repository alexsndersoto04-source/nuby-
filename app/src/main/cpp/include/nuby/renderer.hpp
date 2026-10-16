#pragma once

#include <array>
#include <cstdint>
#include <vector>

#include <GLES3/gl3.h>

#include "nuby/scene.hpp"

namespace nuby {

class Renderer final {
public:
    Renderer() = default;
    ~Renderer() = default;

    void initialize();
    void resize(int width, int height);
    void render(float deltaSeconds);
    void touch(int action, float x, float y);

    struct Vertex {
        float position[3];
        float normal[3];
    };

private:
    struct Mesh {
        GLuint vao = 0;
        GLuint vbo = 0;
        GLsizei vertexCount = 0;
    };

    struct Uniforms {
        GLint viewProjection = -1;
        GLint model = -1;
        GLint color = -1;
        GLint emission = -1;
        GLint cameraPosition = -1;
        GLint lightDirection = -1;
        GLint grid = -1;
    };

    GLuint compileShader(GLenum type, const char* source);
    GLuint createProgram(const char* vertexSource, const char* fragmentSource);
    Mesh uploadMesh(const std::vector<Vertex>& vertices);
    Mesh makeSphere(int latitudeSegments, int longitudeSegments);
    Mesh makeTorus(int ringSegments, int tubeSegments, float majorRadius, float minorRadius);
    Mesh makeGround();
    void draw(const Mesh& mesh, const std::array<float, 16>& model,
              const std::array<float, 3>& color,
              const std::array<float, 3>& emission, bool grid = false);
    void buildScene();

    GLuint program_ = 0;
    Uniforms uniforms_{};
    Mesh sphere_{};
    Mesh torus_{};
    Mesh ground_{};
    Scene scene_;
    int width_ = 1;
    int height_ = 1;
    float yaw_ = 0.45f;
    float pitch_ = 0.18f;
    float lastTouchX_ = 0.0f;
    float lastTouchY_ = 0.0f;
    std::array<float, 16> viewProjection_{};
    std::array<float, 3> cameraPosition_{};
};

}  // namespace nuby
