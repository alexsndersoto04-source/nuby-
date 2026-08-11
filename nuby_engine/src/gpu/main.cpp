#include <GLFW/glfw3.h>

#include "nuby/gpu/renderer.hpp"

#include <array>
#include <cstdlib>
#include <exception>
#include <iostream>

namespace {
void glfw_error(int code, const char* message) {
    std::cerr << "GLFW error " << code << ": " << message << '\n';
}
void framebuffer_size(GLFWwindow* window, int width, int height) {
    auto* renderer = static_cast<nuby::gpu::Renderer*>(glfwGetWindowUserPointer(window));
    if (renderer) renderer->resize(width, height);
}
} // namespace

int main() {
    glfwSetErrorCallback(glfw_error);
    if (glfwInit() != GLFW_TRUE) return EXIT_FAILURE;

    // Contexto REAL OpenGL 4.5 Core: sin fixed pipeline ni emulación.
    glfwWindowHint(GLFW_CONTEXT_VERSION_MAJOR, 4);
    glfwWindowHint(GLFW_CONTEXT_VERSION_MINOR, 5);
    glfwWindowHint(GLFW_OPENGL_PROFILE, GLFW_OPENGL_CORE_PROFILE);
#ifdef __APPLE__
    glfwWindowHint(GLFW_OPENGL_FORWARD_COMPAT, GLFW_TRUE);
#endif
    glfwWindowHint(GLFW_DOUBLEBUFFER, GLFW_TRUE);

    GLFWwindow* window = glfwCreateWindow(960, 540, "Nuby GPU — OpenGL 4.5", nullptr, nullptr);
    if (!window) { glfwTerminate(); return EXIT_FAILURE; }
    glfwMakeContextCurrent(window);
    glfwSwapInterval(1); // VSync; cada end_frame presenta el back buffer.

    try {
        nuby::gpu::Renderer renderer(window);
        glfwSetWindowUserPointer(window, &renderer);
        glfwSetFramebufferSizeCallback(window, framebuffer_size);
        int width = 0, height = 0;
        glfwGetFramebufferSize(window, &width, &height);
        renderer.resize(width, height);

        constexpr std::array<nuby::gpu::Vertex, 3> triangle{{
            {{ 0.0F,  0.72F}, {0.10F, 0.45F, 0.95F}},
            {{-0.72F, -0.62F}, {0.15F, 0.75F, 0.50F}},
            {{ 0.72F, -0.62F}, {0.95F, 0.35F, 0.28F}},
        }};
        renderer.upload_geometry(triangle);

        while (!glfwWindowShouldClose(window)) {
            glfwPollEvents();
            renderer.begin_frame(0.07F, 0.08F, 0.10F);
            renderer.draw(); // VAO + VBO + glDrawArrays: trabajo de GPU.
            renderer.end_frame();
        }
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        glfwDestroyWindow(window);
        glfwTerminate();
        return EXIT_FAILURE;
    }
    glfwDestroyWindow(window);
    glfwTerminate();
    return EXIT_SUCCESS;
}
