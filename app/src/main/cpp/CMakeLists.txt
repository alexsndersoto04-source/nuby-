cmake_minimum_required(VERSION 3.22.1)
project(nuby_engine LANGUAGES CXX)

add_library(nuby-engine SHARED
    native_engine.cpp
    src/asset_memory_cache.cpp
    src/audio_engine.cpp
    src/audio_output_android.cpp
    src/async_asset_task_queue.cpp
    src/async_scene_asset_loader.cpp
    src/adventure_game.cpp
    src/adventure_save.cpp
    src/game_project.cpp
    src/game_project_runtime.cpp
    src/frame_timing.cpp
    src/gltf_loader.cpp
    src/gpu_upload_planner.cpp
    src/image_loader.cpp
    src/png_loader.cpp
    src/mtl_loader.cpp
    src/renderer.cpp
    src/scene.cpp
    src/scene_editor.cpp
    src/sample_game.cpp
    src/scene_io.cpp
    src/shadow_cascades.cpp
    src/skeletal_animation.cpp
    src/world_streaming.cpp
    src/world_zone_manifest.cpp
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
find_library(opensles-lib OpenSLES)

target_link_libraries(nuby-engine PRIVATE
    ${opengles-v3}
    ${opensles-lib}
    ${android-log}
    ${android-lib}
)
