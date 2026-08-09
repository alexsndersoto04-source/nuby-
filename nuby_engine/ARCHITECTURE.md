# NUBY 2.0 — Arquitectura técnica real (sin marketing)

Este documento dice la verdad. La versión anterior de este archivo inflaba
el proyecto ("listo para producción", "motor ECMAScript", "antialiasing de
subpíxeles", "reemplazo de Chromium"). Eso era falso y se ha reescrito.

## Lo que NUBY ES hoy (verificable con `make test` y en vivo)

Un navegador cuya **interfaz completa la pinta su propio motor C++20**:

```
┌──────────────────────────────────────────────────────────────┐
│  Navegador del usuario = MONITOR (canvas mudo)               │
│    • recibe píxeles RGBA por HTTP (RLE propio)               │
│    • envía clicks/teclas/rueda                               │
│    • NO pinta ni un elemento de la UI de Nuby                │
└──────────────────────────────────────────────────────────────┘
              ↑ frames RLE          eventos ↓
┌──────────────────────────────────────────────────────────────┐
│  WebServer (transporte) — src/server/web_server.cpp          │
└──────────────────────────────────────────────────────────────┘
              │
┌──────────────────────────────────────────────────────────────┐
│  BrowserShell — include/nuby/app/browser_shell.hpp           │
│    • chrome (botones, url-bar) y páginas internas en HTML    │
│    • hit-testing sobre el árbol de layout (bloque más        │
│      profundo en (x,y), subida por el DOM a <a>/data-action) │
│    • historial real (pilas back/forward), caret y select-all │
│    • composición chrome+contenido en un solo framebuffer     │
└──────────────────────────────────────────────────────────────┘
              │ render_page()
┌──────────────────────────────────────────────────────────────┐
│  NubyBrowserEngine — pipeline real medido (~1 ms página)     │
│   1. HTMLParser (tokenizer + árbol)                          │
│   2. CascadeEngine CSS (especificidad, inline, UA sheet)     │
│   3. LayoutEngine (bloques + flex 3-pasos con re-layout)     │
│   4. DisplayList                                             │
│   5. SoftwareRasterizer (RGBA, blend alfa, SDF bordes, AA)   │
└──────────────────────────────────────────────────────────────┘
```

### Módulos reales construidos en esta build

| Módulo | Archivo | Qué hace DE VERDAD |
|---|---|---|
| Fetcher | `net/fetcher.hpp` | DNS+TCP+HTTP/1.1 con socket POSIX propio; HTTPS via `openssl s_client` (TLS real del sistema); dechunking; redirects; URLs relativas (RFC 3986 simplificada) |
| Buscador | `search/search_index.hpp` | Índice invertido propio + BM25 genuino (k1=1.2, b=0.75) + snippets del texto real + persistencia TSV + indexación incremental al navegar |
| JavaScript | `js/js_interp.hpp` | Intérprete real: lexer→parser descenso recursivo→AST→evaluador tree-walk. Variables, funciones, closures, `if/while/for`, recursión, `document.getElementById`, `innerHTML`/`textContent` (mutan el DOM de verdad), `onclick`/`addEventListener` vía `.onclick=` |
| UI del navegador | `app/browser_shell.hpp` | Cada píxel de la interfaz (logo, botones, url-bar, home, resultados, errores) lo rasteriza el motor; el usuario lo comprueba mirando cualquier frame |
| Preproceso web | `app/html_preprocess.hpp` | raw-text `<style>/<script>`, entities HTML → UTF-8, `<img>/<svg>/<video>` → placeholders honestos, `<base href>`, charset latin1→UTF-8 |
| Transporte | `server/web_server.cpp` | Frameframes RLE (PackBits por píxel, formato propio) por HTTP; eventos de entrada; APIs `/api/search` y `/api/stats` con datos reales |

### Datos iniciales del índice

`data/crawl_pages.tsv`: 995 páginas web **reales** rastreadas por `spider.py`
el 8-ago-2026 (Wikipedia, Hacker News, …) y exportadas con
`tools/export_index.py`. Nada está hardcodeado en el código.

## Lo que NUBY NO hace todavía (la otra verdad)

- **Sin imágenes**: no hay decodificadores JPEG/PNG enlazados. Los `<img>`
  se muestran como placeholders con su `alt`. (La build enlaza cero
  librerías externas.)
- **Sin gzip/br**: pedimos `Accept-Encoding: identity`.
- **CSS es un subconjunto**: flujo de bloques, flex (row/column, grow,
  justify, align, gap), márgenes/padding/border, colores, radios,
  sombras, `position:absolute/fixed`. NO: grid, floats, tablas reales,
  `inline-block` real, `position:sticky`, `@media` (se eliminan de la hoja).
- **JS es un subconjunto** (documentado arriba, el error es explícito).
  Las webs con JS moderno pesado no funcionarán.
- **Fuentes**: bitmap 8×12 propio escalado (con composición de acentos para
  español). Sin TTF/hinting.
- **HTTPS** depende del binario `openssl` del sistema: sin él, solo HTTP.
- **Una sesión** por servidor (es el modelo "escritorio remoto").

## Bugs reales encontrados y corregidos en esta build

1. **Flex sin interior**: los items flex se posicionaban pero su contenido
   nunca se maquetaba (botones vacíos). Ahora: medida → flex → re-layout
   completo de cada item con su rect final (3 pasos).
2. **`align-items:center` con `height:auto`**: centraba respecto a altura 0
   (offsets negativos). Ahora la altura provisional se deriva del contenido
   antes de alinear.
3. **Métrica de texto doble**: el shaper estimaba por carácter y el
   rasterizador pintaba monoespaciado → solapes. Comparten métrica ahora.
4. **URL-bar concatenaba** en vez de reemplazar al enfocar: implementado el
   select-all real (comportamiento de los navegadores).
5. **Makefile sin dependencias de headers**: `make` no recompilaba tras
   editar `.hpp` (builds en falso verde).
6. **URLs relativas con `//` dobles** por segmentos vacíos.

## Mediciones reales (sandbox Debian, g++ -O3)

- Ciclo completo de render de página interna: **~1 ms** (medido en vivo)
- Búsqueda BM25 sobre 995 docs: **~25 μs** (medido por `/api/search`)
- Frame 1024×640 RLE: **~35-80 KB** (vs ~2 MB crudos)
- Suite: `make test` → 7/7

## Cómo correrlo

```bash
cd nuby_engine && make && ./bin/nuby_engine --port 8080
# abre http://localhost:8080 — tu navegador es solo el monitor;
# la interfaz entera la pinta el motor Nuby.
```
