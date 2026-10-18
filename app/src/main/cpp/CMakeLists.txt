cmake_minimum_required(VERSION 3.22.1)
project(nuby_engine LANGUAGES CXX)

add_library(nuby-engine SHARED
    native_engine.cpp
    src/adventure_game.cpp
    src/game_project.cpp
    src/game_project_runtime.cpp
    src/gltf_loader.cpp
    src/image_loader.cpp
    src/mtl_loader.cpp
    src/renderer.cpp
    src/scene.cpp
    src/sample_game.cpp
    src/scene_io.cpp
    src/shadow_cascades.cpp
    src/skeletal_animation.cpp
    src/obj_loader.cpp
)

target_include_directories(nuby-engine PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/include
)

target_compile_features(nuby-engine PRIVATE cxx_std_17)
target_compile_options(nuby-engine PRIVATE
    -Wall -Wextra -Wpedantic
    -Wno-unused-parameter
)

find_library(android-lib android)
find_library(android-log log)
find_library(opengles-v3 GLESv3)

target_link_libraries(nuby-engine PRIVATE
    ${opengles-v3}
    ${android-log}
    ${android-lib}
)
