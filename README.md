# Nuby

**Un navegador web cuyo motor C++20 pinta su propia interfaz.**

Toda la UI (botones, barra de direcciones, resultados de búsqueda) es
rasterizada por el motor Nuby: quien lo usa recibe píxeles calculados por el
motor, no HTML pintado por Chrome. Incluye buscador propio con índice
invertido + BM25, red HTTP/HTTPS real e intérprete JavaScript propio.

## Pruébalo sin instalar nada (WebAssembly)

El motor completo, compilado a WebAssembly, se ejecuta dentro de tu navegador:
**[alexsndersoto04-source.github.io/nuby-/web/](https://alexsndersoto04-source.github.io/nuby-/web/)**
(se publica automáticamente en cada push a `main`, después de pasar las pruebas).

Además hay una build **nativa** con servidor propio:

```bash
cd nuby_engine
make
make test                       # 20 suites de pruebas reales
./bin/nuby_engine --port 8080   # abre http://localhost:8080
```

Tu navegador actúa como monitor remoto (estilo VNC): el motor envía los
píxeles de su framebuffer comprimidos con un RLE propio; clicks, teclado y
rueda viajan de vuelta al motor, que hace hit-testing sobre su propio árbol
de layout.

## Estado honesto

Es un motor real pero mínimo (el proyecto nació como ejercicio y pasó por una
etapa de claims inflados que fueron corregidos). Lee la verdad técnica
completa — qué es real y qué no — en
[`nuby_engine/ARCHITECTURE.md`](nuby_engine/ARCHITECTURE.md).

- ✅ Parser HTML/CSS, layout (bloques/flex/inline), rasterizador software: propios.
- ✅ Buscador BM25 con 995 páginas rastreadas de verdad + indexación en vivo.
- ✅ Red: sockets propios + TLS vía OpenSSL del sistema.
- ✅ JS: intérprete real para un subconjunto documentado.
- ✅ Imágenes PNG con decodificador propio (inflate + filtros, sin librerías).
- ✅ Home propia estilo buscador clásico: wordmark negro, píldora con botón de flecha negro.
- ✅ ☰ Menú funcional: Historial, Descargas (bytes reales), Configuración (zoom de texto, cursor, instantánea, historial) y Acerca de — todo aplicado de verdad.
- ✅ Teclado virtual del móvil: puente con input real + reporte kbd/text del motor.
- ✅ Texto con antialiasing propio (cobertura por supersampling) y zoom global.
- ✅ Multi-sesión: cada visitante tiene su propio navegador.
- ❌ Sin JPEG/GIF (placeholder honesto), CSS/JS parciales, una ventana por sesión.

## Estructura

| Carpeta/archivo | Qué es |
|---|---|
| `nuby_engine/` | El navegador (motor C++20 + shell + servidor). Ver su README. |
| `spider.py`, `main.py` | Crawler y buscador FTS5 de la etapa Python (datos reales origen del índice). |
| `index.html` | Página del proyecto (información + enlaces de arranque). |
| `web/` | Build de navegador: el motor compilado a wasm (`web/nuby.wasm`), su runtime WASI en JS y la página que lo ejecuta. |
| `nuby_engine/wasm/` | Fuentes del puente a WebAssembly, el script de compilación y las comprobaciones automáticas (Node y Chrome). |

## Las dos builds

| | Nativa (`nuby_engine`) | Navegador (`web/`, WebAssembly) |
|---|---|---|
| HTML/CSS/layout/rasterizado/fuentes/BM25 | sí | sí (el mismo código) |
| Red (HTTP/HTTPS) | sockets + TLS propios | solo mismo origen (CORS del navegador) |
| JavaScript de las páginas | sí, intérprete propio | no (C++ para wasm sin excepciones) |
| Escritura en disco | sí | no (el navegador no lo permite) |
| Pruebas | `make test` (20/20) | `node nuby_engine/wasm/node_check.mjs` + Chrome en CI |

La versión de navegador no es un recorte del motor: es el mismo código fuente
compilado a wasm, con dos funciones de plataforma (mutex y ficheros) resueltas
de otra forma porque un navegador no las da.

La antigua app Android era una interfaz GPU separada sin navegación web
integrada y se retiró junto con los restos del juego SOMBRA, que no pertenecía
a este proyecto.
