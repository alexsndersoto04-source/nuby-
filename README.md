# Nuby Engine

Motor propio nativo para Android, con renderizado 3D en C++/OpenGL ES 3 y una ruta de compilación reproducible mediante GitHub Actions. El proyecto no usa Unity, Unreal ni Godot.

## Estado

**Fase 1 — base Android y compilación remota completadas. Fase 2 — núcleo de escenas implementado. Fase 3 — renderizado PBR, sombras dinámicas y bloom aceptados visualmente en el Redmi 9C; ruta OBJ validada (58 FPS en la prueba reportada). Fase 4 — física básica validada por el usuario en el Redmi 9C: caída, rebote y reposo; captura puntual de 56 FPS con sombras, bloom y OBJ activos. Fase 5 — prototipo técnico Aether Run completado en el Redmi 9C (6/6; captura puntual de 61 FPS); no es el juego final. Fase 6 — primer prototipo de aventura 3D independiente implementado con joystick táctil, salto, cámara de seguimiento, reliquias y puerta de salida; pruebas nativas y compilación Android pasaron en CI, y el usuario la probó en el Redmi 9C; reporta que los controles y las mecánicas funcionan bien. Una captura de la versión anterior muestra 60 FPS con sombras, bloom y malla activos; es una lectura puntual, no sostenida, y los mapas PBR recién añadidos aún necesitan validación visual y de rendimiento en el teléfono.** El APK se genera de forma remota mediante GitHub Actions. El renderizador y el núcleo de escenas son código propio; no se usa Unity, Unreal ni Godot. Esta primera aventura es una rebanada jugable para validar controles y sistemas, no el juego completo ni una promesa de rendimiento; el editor nativo táctil vendrá después.

La escena actual es un laboratorio interactivo del renderizador y del modelo de entidades: arrastra el dedo para orbitar la cámara. El HUD muestra FPS, GPU, estado de sombras/bloom y si el modelo OBJ de muestra se cargó o se usó la geometría de respaldo. El núcleo incluye entidades identificadas, búsqueda por ID, destrucción en cascada de jerarquías padre-hijo, matrices de transformación de mundo, cámara activa con proyección configurable, órbita y zoom por gesto de pellizco, además de componentes de animación por claves, giro, órbita y pulso. El núcleo incluye serialización de escenas versionada y sin dependencias externas: guarda jerarquías, recursos, componentes y configuración física, valida referencias y carga de forma transaccional para no reemplazar una escena con un archivo defectuoso; el formato v4 guarda el controlador de personaje y los mapas de material, y mantiene lectura compatible con escenas v1, v2 y v3. El editor todavía no tiene una interfaz de guardar/cargar. Las reglas se prueban automáticamente fuera de Android. La fase actual incorpora cuerpos rígidos traslacionales, gravedad, paso fijo limitado, colisiones de esferas, restitución/fricción y contacto con un plano de suelo; la escena de laboratorio contiene una esfera física visible. Los triggers esféricos generan eventos de entrada/salida en una cola acotada que el juego consume después de cada actualización. Esta iteración aún no calcula rotación/inercia ni CCD, y solo simula cuerpos en la raíz de la jerarquía. La física de caída, rebote y reposo se probó en el Redmi 9C; la captura mostró 56 FPS con sombras, bloom y malla activa, pero no es una medición sostenida. Los eventos de trigger pasan pruebas automatizadas y compilación Android en CI; la recogida de reliquias también se comprobó durante la prueba de la aventura en el teléfono. Se añadió una base reutilizable de controlador de personaje: entrada horizontal normalizada, aceleración, control aéreo reducido, estado de suelo y salto simple, integrada con la física y la serialización v3. Se usa ahora en un prototipo separado de aventura: explora un patio de ruinas, reúne tres reliquias y alcanza una puerta; el joystick Android controla el movimiento, un botón permite saltar y arrastrar a la derecha orbita la cámara. Es una primera prueba jugable de sistemas, con formas geométricas provisionales: el usuario ya la probó en el Redmi 9C y confirmó que todo funciona bien; no es todavía la aventura completa. Las suites nativas cubren movimiento, salto, seguimiento de cámara, recogida y finalización. El renderizador precarga y reutiliza por ruta las mallas OBJ referenciadas por la escena, evitando cargas repetidas y usando geometría de respaldo si un recurso no se puede importar. La iluminación combina la luz direccional con hasta cuatro luces puntuales por entidad, con atenuación por distancia dentro de su alcance; las luces locales no proyectan sombras en esta iteración. También incorpora materiales metálicos/dieléctricos controlados por rugosidad, Cook–Torrance/GGX con tone mapping ACES, sombras direccionales con mapa de profundidad de 512 × 512 y filtrado PCF, y bloom con extracción y desenfoque a un cuarto de resolución. En el Redmi 9C se reportaron lecturas puntuales de 61 FPS con PBR y sombras, y 57 FPS con sombras y bloom; no son pruebas de rendimiento sostenido.

## Juego de muestra: Aether Run

Prototipo técnico construido sobre el motor propio para validar el ciclo de juego y los controles: el jugador cambia entre tres carriles con gestos horizontales, recoge seis fragmentos cian y evita orbes rojos; gana al reunir el objetivo, pierde al chocar o agotar 45 segundos, y puede reiniciar la partida. Usa entidades, físicas, triggers, cámara y materiales del motor. Las pruebas unitarias y el ensamblado Android pasaron en CI; el usuario completó 6/6 en el Redmi 9C y reportó una captura puntual de 61 FPS con sombras, bloom y malla activa. **Aether Run no es el juego final**: el producto planeado es una aventura 3D completa creada con este motor.

## Importación de geometría OBJ

El motor incluye un parser propio en C++17, sin dependencia de motor ni parser externo. La primera muestra empaquetada (`meshes/orbital_shard.obj`) carga posiciones, coordenadas UV y normales; admite índices OBJ negativos, triangula polígonos convexos y genera normales de cara cuando faltan. El HUD indica `MESH ON` si el recurso se cargó, o `MESH FALLBACK` si no. El importador OBJ todavía no descubre automáticamente materiales MTL, pero las escenas ahora pueden asignar mapas PBR por entidad: albedo sRGB, normales en espacio tangente, metallic/roughness empacado en G/B y oclusión ambiental en R. El renderizador propio los carga en GPU con mipmaps; el decodificador TGA C++ propio admite imágenes true-color sin comprimir y RLE de 24/32 bits, valida límites y no usa librerías externas. La aventura de prueba usa estos mapas de 128 × 128 en las reliquias; el APK compila en CI, pero la apariencia y el coste de GPU de estas texturas todavía están pendientes de validación en el Redmi 9C. Esta base no sustituye el futuro soporte completo de modelos/animaciones de personajes ni el importador de MTL.

## Compilación automática

El workflow `.github/workflows/android.yml` ejecuta las pruebas nativas y compila Android en cada push, pull request o ejecución manual. Para no generar un artefacto descargable por cada integración, el APK solo se publica al iniciar el workflow manualmente (`workflow_dispatch`) o al hacer push de un commit cuyo mensaje incluya `[apk-milestone]`. Usa esa marca únicamente al cerrar un hito que vaya a probarse en el teléfono. Abre **Actions → Android build → la ejecución del hito → Artifacts** y descarga `nuby-engine-debug-apk`. Los pushes/PR normales siguen compilando y probando sin publicar APK.

La compilación está configurada para `arm64-v8a` y `armeabi-v7a`, con Android API mínima 24 y OpenGL ES 3.0.

## Proyectos de juego independientes (base del formato)

El motor ya incluye un lector/escritor nativo de manifiestos `NUBY_PROJECT 1`: guarda nombre visible, identificador Android y escena inicial; el cargador resuelve la escena mediante un lector de recursos inyectable, verifica rutas relativas para impedir salir del paquete y reemplaza proyecto/escena de forma transaccional. Tiene pruebas nativas. **Esto todavía no es el editor ni genera APKs separados**: falta conectarlo al arranque del runtime, al empaquetado de recursos y al workflow de compilación por proyecto. La intención es que cada APK final contenga el runtime necesario y solo el contenido del juego elegido, no el laboratorio ni los prototipos incluidos en esta app de desarrollo.

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
4. Sistemas de juego: animación y física básica validadas; faltan rotación física, audio y más subsistemas.
5. Juego de muestra: aventura 3D completa pendiente de diseñar e implementar; Aether Run solo valida las bases del motor.
6. Editor Android táctil para crear proyectos.
7. Optimización, pruebas finales y documentación.
