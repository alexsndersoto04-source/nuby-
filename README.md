# Nuby Engine

Motor propio nativo para Android, con renderizado 3D en C++/OpenGL ES 3 y una ruta de compilación reproducible mediante GitHub Actions. El proyecto no usa Unity, Unreal ni Godot.

## Estado

**Fase 1 — base Android y prueba de renderizado en curso.** Esta primera entrega establece el APK nativo, la integración C++/Java, el renderizador inicial y el flujo CI. No es todavía el motor completo, el juego de muestra ni el editor final.

La escena inicial es una prueba técnica interactiva del renderizador. Arrastra el dedo sobre la pantalla para orbitar la cámara. El Redmi 9C es el dispositivo de referencia; las decisiones gráficas se validarán en el teléfono, no se darán por buenas solo porque compile en CI.

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
