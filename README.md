# Nuby Engine

Motor propio nativo para Android, con renderizado 3D en C++/OpenGL ES 3 y una ruta de compilación reproducible mediante GitHub Actions. El proyecto no usa Unity, Unreal ni Godot.

## Estado

**Fase 1 — base Android y compilación remota completadas. Fase 2 — núcleo de escenas implementado. Fase 3 — PBR, sombras dinámicas y bloom en validación.** El proyecto ya genera un APK nativo mediante GitHub Actions. El renderizador y el núcleo de escenas son código propio; no se usa Unity, Unreal ni Godot. Todavía faltan sistemas de juego, el juego de muestra y el editor final.

La escena actual es una prueba interactiva del renderizador y del modelo de entidades: arrastra el dedo para orbitar la cámara. Muestra FPS medidos, el nombre de la GPU detectada y si las sombras y el bloom se activaron, para que el perfilado en el Redmi 9C se base en datos del teléfono. El núcleo incluye entidades identificadas, transformaciones y componentes de giro, órbita y pulso, con pruebas automáticas independientes de Android. El renderizador incorpora materiales metálicos/dieléctricos controlados por rugosidad, iluminación PBR Cook–Torrance/GGX con tone mapping ACES, sombras direccionales con mapa de profundidad de 512 × 512 y filtrado PCF, y bloom con extracción y desenfoque a un cuarto de resolución. Si el dispositivo no admite los framebuffers, usa renderizado directo como respaldo. La compilación CI pasó; el efecto visual y el rendimiento del bloom aún deben medirse en el Redmi.

## Compilación automática

El workflow `.github/workflows/android.yml` compila `app-debug.apk` en cada push, pull request o ejecución manual. En GitHub, abre **Actions → Android build → el último workflow → Artifacts** y descarga `nuby-engine-debug-apk`. Instala el APK en Android para probar la versión de ese hito.

La compilación está configurada para `arm64-v8a` y `armeabi-v7a`, con Android API mínima 24 y OpenGL ES 3.0.

## Compilación local (si hay un entorno Android disponible)

Se requiere JDK 17, Android SDK, NDK `27.2.12479018`, CMake `3.22.1` y Gradle `8.9`:

```sh
gradle --no-daemon :app:assembleDebug
```

El APK queda en `app/build/outputs/apk/debug/app-debug.apk`.

## Dirección del proyecto

1. Base Android y compilación remota.
2. Núcleo del motor: ciclo de vida, escenas, entrada y recursos.
3. Renderizador 3D escalable, perfilado y validado en el Redmi 9C.
4. Sistemas de juego (animación, física, audio, interfaz y guardado).
5. Juego de muestra completo creado con el motor.
6. Editor Android táctil para crear proyectos.
7. Optimización, pruebas finales y documentación.
