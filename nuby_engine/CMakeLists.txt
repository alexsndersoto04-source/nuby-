cmake_minimum_required(VERSION 3.24)
project(nuby_gpu LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

find_package(OpenGL REQUIRED)
find_package(glfw3 3.3 REQUIRED)

add_executable(nuby_gpu_demo
  src/gpu/main.cpp
  src/gpu/renderer_gl45.cpp
)
target_include_directories(nuby_gpu_demo PRIVATE include)
target_link_libraries(nuby_gpu_demo PRIVATE glfw OpenGL::GL)
target_compile_options(nuby_gpu_demo PRIVATE -Wall -Wextra -Wpedantic)
