# Backend GPU de Nuby (OpenGL 4.5)

Este módulo es el punto de entrada de la migración a GPU. Usa un contexto **OpenGL 4.5 Core** real creado por GLFW, VBO/VAO, shaders GLSL y `glfwSwapBuffers` con VSync. No usa rasterización CPU.

## Dependencias de desarrollo

- CMake 3.24+
- compilador C++20
- GLFW 3.3+
- driver OpenGL 4.5+

Debian/Ubuntu:

```bash
sudo apt-get install cmake g++ libglfw3-dev libgl1-mesa-dev
```

## Construir y ejecutar

```bash
cd nuby_engine
cmake -S . -B build-gpu -DCMAKE_BUILD_TYPE=Release
cmake --build build-gpu --target nuby_gpu_demo -j
./build-gpu/nuby_gpu_demo
```

La ventana presenta un triángulo de tres vértices mediante el pipeline GPU.

## Contrato de renderer

El motor entrega `std::span<const nuby::gpu::Vertex>` a:

```cpp
renderer.upload_geometry(vertices);
renderer.begin_frame(r, g, b);
renderer.draw();
renderer.end_frame();
```

`upload_geometry` actualiza el VBO; `draw` enlaza el VAO y emite `glDrawArrays`. La siguiente etapa de la migración debe traducir las listas de layout/paint existentes a lotes de `Vertex` (rectángulos: dos triángulos) y texturas de glifos, sin volver a invocar `SoftwareRasterizer`.
