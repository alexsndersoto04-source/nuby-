#pragma once

#include <array>
#include <cstdint>
#include <span>

struct GLFWwindow;

namespace nuby::gpu {

// Datos que el motor envía al GPU. Posición en NDC y color lineal por vértice.
struct Vertex {
    float position[2];
    float color[3];
};

// Backend OpenGL 4.5 Core. No contiene rasterización CPU: solo sube buffers,
// graba el estado de dibujo en VAO y despacha draw calls a la GPU.
class Renderer final {
public:
    explicit Renderer(GLFWwindow* window);
    ~Renderer();
    Renderer(const Renderer&) = delete;
    Renderer& operator=(const Renderer&) = delete;

    void resize(int framebuffer_width, int framebuffer_height) noexcept;
    void upload_geometry(std::span<const Vertex> vertices);
    void begin_frame(float red, float green, float blue, float alpha = 1.0F) const noexcept;
    void draw() const noexcept;
    void end_frame() const noexcept; // Presentación: glfwSwapBuffers (doble búfer)

private:
    GLFWwindow* window_{};
    std::uint32_t program_{};
    std::uint32_t vao_{};
    std::uint32_t vbo_{};
    int vertex_count_{};
};

} // namespace nuby::gpu
