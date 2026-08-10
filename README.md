# Nuby

**Un navegador web cuyo motor C++20 pinta su propia interfaz.**

Toda la UI (botones, barra de direcciones, resultados de búsqueda) es
rasterizada por el motor Nuby: quien lo usa recibe píxeles calculados por el
motor, no HTML pintado por Chrome. Incluye buscador propio con índice
invertido + BM25, red HTTP/HTTPS real e intérprete JavaScript propio.

## Inicio rápido

```bash
cd nuby_engine
make
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
| `index.html`, `js/`, `css/`, `android/` | Restos del juego 2D "SOMBRA" (primera etapa del repo). |
