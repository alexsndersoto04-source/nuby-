#define GL_GLEXT_PROTOTYPES
#include <GLFW/glfw3.h>

#include "nuby/gpu/renderer.hpp"

#include <cstddef>
#include <stdexcept>
#include <string>

namespace nuby::gpu {
namespace {
constexpr const char* kVertexShader = R"GLSL(#version 450 core
layout(location = 0) in vec2 in_position;
layout(location = 1) in vec3 in_color;
out vec3 vertex_color;
void main() {
    vertex_color = in_color;
    gl_Position = vec4(in_position, 0.0, 1.0);
}
)GLSL";

constexpr const char* kFragmentShader = R"GLSL(#version 450 core
in vec3 vertex_color;
layout(location = 0) out vec4 out_color;
void main() {
    out_color = vec4(vertex_color, 1.0);
}
)GLSL";

GLuint compile(GLenum type, const char* source) {
    GLuint shader = glCreateShader(type);
    glShaderSource(shader, 1, &source, nullptr);
    glCompileShader(shader);
    GLint success = GL_FALSE;
    glGetShaderiv(shader, GL_COMPILE_STATUS, &success);
    if (success == GL_TRUE) return shader;

    GLint bytes = 0;
    glGetShaderiv(shader, GL_INFO_LOG_LENGTH, &bytes);
    std::string message(static_cast<size_t>(bytes > 1 ? bytes : 1), '\0');
    glGetShaderInfoLog(shader, bytes, nullptr, message.data());
    glDeleteShader(shader);
    throw std::runtime_error("OpenGL shader compilation failed: " + message);
}

GLuint create_program() {
    GLuint vertex = compile(GL_VERTEX_SHADER, kVertexShader);
    GLuint fragment = compile(GL_FRAGMENT_SHADER, kFragmentShader);
    GLuint program = glCreateProgram();
    glAttachShader(program, vertex);
    glAttachShader(program, fragment);
    glLinkProgram(program);
    glDeleteShader(vertex);
    glDeleteShader(fragment);

    GLint success = GL_FALSE;
    glGetProgramiv(program, GL_LINK_STATUS, &success);
    if (success == GL_TRUE) return program;
    GLint bytes = 0;
    glGetProgramiv(program, GL_INFO_LOG_LENGTH, &bytes);
    std::string message(static_cast<size_t>(bytes > 1 ? bytes : 1), '\0');
    glGetProgramInfoLog(program, bytes, nullptr, message.data());
    glDeleteProgram(program);
    throw std::runtime_error("OpenGL program link failed: " + message);
}
} // namespace

Renderer::Renderer(GLFWwindow* window) : window_(window), program_(create_program()) {
    glGenVertexArrays(1, &vao_);
    glGenBuffers(1, &vbo_);
    glBindVertexArray(vao_);
    glBindBuffer(GL_ARRAY_BUFFER, vbo_);
    // El buffer vive en memoria de GPU. El motor lo actualiza con upload_geometry().
    glBufferData(GL_ARRAY_BUFFER, 0, nullptr, GL_DYNAMIC_DRAW);
    glEnableVertexAttribArray(0);
    glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, sizeof(Vertex), reinterpret_cast<void*>(offsetof(Vertex, position)));
    glEnableVertexAttribArray(1);
    glVertexAttribPointer(1, 3, GL_FLOAT, GL_FALSE, sizeof(Vertex), reinterpret_cast<void*>(offsetof(Vertex, color)));
    glBindVertexArray(0);
}

Renderer::~Renderer() {
    if (vbo_) glDeleteBuffers(1, &vbo_);
    if (vao_) glDeleteVertexArrays(1, &vao_);
    if (program_) glDeleteProgram(program_);
}

void Renderer::resize(int width, int height) noexcept { glViewport(0, 0, width, height); }

void Renderer::upload_geometry(std::span<const Vertex> vertices) {
    vertex_count_ = static_cast<int>(vertices.size());
    glBindBuffer(GL_ARRAY_BUFFER, vbo_);
    glBufferData(GL_ARRAY_BUFFER, static_cast<GLsizeiptr>(vertices.size_bytes()), vertices.data(), GL_DYNAMIC_DRAW);
}

void Renderer::begin_frame(float red, float green, float blue, float alpha) const noexcept {
    glClearColor(red, green, blue, alpha);
    glClear(GL_COLOR_BUFFER_BIT);
}

void Renderer::draw() const noexcept {
    if (!vertex_count_) return;
    glUseProgram(program_);
    glBindVertexArray(vao_);
    glDrawArrays(GL_TRIANGLES, 0, vertex_count_);
    glBindVertexArray(0);
}

void Renderer::end_frame() const noexcept { glfwSwapBuffers(window_); }
} // namespace nuby::gpu
