# Nuby Engine

Motor propio nativo para Android, con renderizado 3D en C++/OpenGL ES 3 y una ruta de compilación reproducible mediante GitHub Actions. El proyecto no usa Unity, Unreal ni Godot.

## Estado

**Fase 1 — base Android y compilación remota completadas. Fase 2 — núcleo de escenas implementado. Fase 3 — renderizado PBR, sombras dinámicas y bloom aceptados visualmente en el Redmi 9C; ruta OBJ compilada en CI y validada en el Redmi 9C (58 FPS en la prueba reportada). Fase 4 — física básica vista en el Redmi 9C: caída, rebote y reposo confirmados; la captura reportó 56 FPS con sombras/bloom/OBJ activos (lectura puntual).** El APK se genera de forma remota mediante GitHub Actions. El renderizador y el núcleo de escenas son código propio; no se usa Unity, Unreal ni Godot. Todavía faltan sistemas de juego, el juego de muestra y el editor final.

La escena actual es un laboratorio interactivo del renderizador y del modelo de entidades: arrastra el dedo para orbitar la cámara. El HUD muestra FPS, GPU, estado de sombras/bloom y si el modelo OBJ de muestra se cargó o se usó la geometría de respaldo. El núcleo incluye entidades identificadas, búsqueda por ID, destrucción en cascada de jerarquías padre-hijo, matrices de transformación de mundo, cámara activa con proyección configurable, órbita y zoom por gesto de pellizco, además de componentes de animación por claves, giro, órbita y pulso. El núcleo incluye serialización de escenas versionada y sin dependencias externas: guarda jerarquías, recursos, componentes y configuración física, valida referencias y carga de forma transaccional para no reemplazar una escena con un archivo defectuoso; el formato v2 mantiene lectura de escenas v1. El editor todavía no tiene una interfaz de guardar/cargar. Las reglas se prueban automáticamente fuera de Android. La fase actual incorpora cuerpos rígidos traslacionales, gravedad, paso fijo limitado, colisiones de esferas, restitución/fricción y contacto con un plano de suelo; la escena de laboratorio contiene una esfera física visible. Los triggers esféricos generan eventos de entrada/salida en una cola acotada que el juego consume después de cada actualización. Esta iteración aún no calcula rotación/inercia ni CCD, y solo simula cuerpos en la raíz de la jerarquía. La física de caída, rebote y reposo se probó en el Redmi 9C; la captura mostró 56 FPS con sombras, bloom y malla activa, pero no es una medición sostenida. La nueva API de eventos de trigger queda pendiente de compilar y probar en CI. El renderizador precarga y reutiliza por ruta las mallas OBJ referenciadas por la escena, evitando cargas repetidas y usando geometría de respaldo si un recurso no se puede importar. La iluminación combina la luz direccional con hasta cuatro luces puntuales por entidad, con atenuación por distancia dentro de su alcance; las luces locales no proyectan sombras en esta iteración. También incorpora materiales metálicos/dieléctricos controlados por rugosidad, Cook–Torrance/GGX con tone mapping ACES, sombras direccionales con mapa de profundidad de 512 × 512 y filtrado PCF, y bloom con extracción y desenfoque a un cuarto de resolución. En el Redmi 9C se reportaron lecturas puntuales de 61 FPS con PBR y sombras, y 57 FPS con sombras y bloom; no son pruebas de rendimiento sostenido.

## Importación de geometría OBJ

El motor incluye un parser propio en C++17, sin dependencia de motor ni parser externo. La primera muestra empaquetada (`meshes/orbital_shard.obj`) carga posiciones, coordenadas UV y normales; admite índices OBJ negativos, triangula polígonos convexos y genera normales de cara cuando faltan. El HUD indica `MESH ON` si el recurso se cargó, o `MESH FALLBACK` si no. Esta iteración aún no importa materiales MTL ni texturas y la carga/renderizado del modelo en Android debe confirmarse en el Redmi 9C antes de considerarla validada en dispositivo.

## Compilación automática

El workflow `.github/workflows/android.yml` ejecuta las pruebas nativas y compila Android en cada push, pull request o ejecución manual. Para no generar un artefacto descargable por cada integración, el APK solo se publica al iniciar el workflow manualmente (`workflow_dispatch`) o al hacer push de un commit cuyo mensaje incluya `[apk-milestone]`. Usa esa marca únicamente al cerrar un hito que vaya a probarse en el teléfono. Abre **Actions → Android build → la ejecución del hito → Artifacts** y descarga `nuby-engine-debug-apk`. Los pushes/PR normales siguen compilando y probando sin publicar APK.

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
4. Sistemas de juego: animación, física (en progreso), audio, interfaz y guardado.
5. Juego de muestra completo creado con el motor.
6. Editor Android táctil para crear proyectos.
7. Optimización, pruebas finales y documentación.
