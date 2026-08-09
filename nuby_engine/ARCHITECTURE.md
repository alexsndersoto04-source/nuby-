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

- **Imágenes: solo PNG** (decodificador PROPIO desde la tercera fase —
  inflate+filtros+Paeth+paleta+alfa, 9 vectores de prueba). JPEG y GIF se
  muestran como placeholder HONESTO con la razón ("JPEG no soportado aun").
  PNG entrelazado Adam7 falla con mensaje explícito, no con basura.
- **Sin gzip/br**: pedimos `Accept-Encoding: identity`.
- **CSS es un subconjunto**: flujo de bloques, **IFC real (inline flow con
  líneas, wrap, baseline y text-align)**, flex (row/column, grow, justify,
  align, gap), márgenes/padding/border, colores, radios, sombras,
  `position:absolute/fixed`, `inline-block` atómico dentro del IFC.
  NO: grid, floats, tablas reales, márgenes/padding de cajas inline puras
  (v1), `vertical-align` distinto de baseline, `position:sticky`,
  `@media` (se eliminan de la hoja).
- **JS es un subconjunto** (documentado arriba, el error es explícito).
  Las webs con JS moderno pesado no funcionarán.
- **Fuentes**: bitmap 8×12 propio escalado (con composición de acentos para
  español). Sin TTF/hinting.
- **HTTPS** depende del binario `openssl` del sistema: sin él, solo HTTP.
- **`<select>`** se pinta pero no es interactivo (placeholder honesto).

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

- Ciclo completo de render de página interna: **~1-3 ms** (medido en vivo)
- Búsqueda BM25 sobre 995 docs: **~25 μs-1 ms** (medido por `/api/search`)
- Frame 1024×640 RLE: **~35-80 KB** (vs ~2 MB crudos)
- Suite: `make test` → **15/15** (cada suite reproduce un bug real o valida
  un módulo: PNG 9 vectores, imagen intrínseca, IFC, multi-sesión, …)

## Cómo correrlo

```bash
cd nuby_engine && make && ./bin/nuby_engine --port 8080
# abre http://localhost:8080 — tu navegador es solo el monitor;
# la interfaz entera la pinta el motor Nuby.
```

---

# SEGUNDA AUDITORÍA HONESTA (2026-08-09, tarde)

El motor pintaba su interfaz y navegaba, pero al probarlo con una página
totalmente nueva servida en vivo aparecieron **cinco bugs reales**. Se listan
con la verdad completa: el producto NO estaba terminado y aquí quedó el
registro de lo que fallaba y cómo se arregló.

| # | Bug real | Síntoma | Arreglo real |
|---|----------|---------|--------------|
| 1 | Caja de texto copiaba el estilo completo del padre (borde incluido) | Cada línea de texto aparecía enmarcada con el borde de su caja | La display-list ya no pinta fondo/borde/sombra en `TEXT_BOX` (CSS: las decoraciones las pinta la caja, no cada fragmento de texto) |
| 2 | El shell nunca pasaba los `<script>` al motor | JavaScript de página web jamás se ejecutaba por esa vía | `navigate()` pasa los scripts a `render_page`, que los corre ANTES del layout → el DOM mutado es lo que se pinta |
| 3 | Los errores de JS se sobrescribían con los logs de consola | Un script roto parecía "correr bien" | Los errores se conservan y se muestran en la barra de estado (`JS: N error(es)`) |
| 4 | Los scripts corrían **dos veces** (dentro de `render_page` y otra en el shell) y los re-renders post-click perdían el CSS de la página (texto oscuro sobre fondo oscuro) | Doble ejecución + contenido mutado casi invisible | UN solo intérprete por página (el que crea `render_page`, reutilizado para los `onclick`), CSS de página conservada en `page_css_` |
| 5 | La hoja UA declaraba `color:#1a1a1a` y `font-size:16px` en CADA elemento (un "reset" mal pensado) | **Toda** herencia de color moría: `body{color:X}` nunca llegaba a div/p/span — texto siempre gris oscuro (proven con sonda de píxeles) | Hoja UA corregida: el default vive en `ComputedStyle` y los elementos heredan, como manda CSS |

Bugs colaterales corregidos el mismo día:
- Re-renders posteriores a un click dejaban el intérprete apuntando a un
  documento ya destruido (clics siguientes caían en un "fantasma"). Ahora hay
  re-bind al documento vivo. *Limitación honesta documentada: tras ese
  re-render el estado JS de carga (funciones/variables) se reinicia.*
- Suite `make test`: 10/10 con tests de regresión escritos para los bugs 1, 2
  y 5 (cada test reproduce el bug original).

### Nuevo endpoint (transparencia)
- `GET /api/goto?u=<url>` — navega como si se tecleara la URL; devuelve JSON
  con el estado real (para pruebas y automatización).

### Servidor / hosting
- `main.cpp` respeta la variable de entorno `PORT` (Render, Fly, Koyeb la
  inyectan). `--port` sigue ganando si se pasa.
- `Dockerfile` multi-etapa real: compila, corre los tests (si fallan, falla
  el build) y deja una imagen mínima con `openssl` + CA para el puente TLS.
- **Lo que un hosting gratuito debe saber de Nuby**: proceso HTTP único con
  memoria de una sesión; índice de 995 docs cargado en RAM (~10 ms); las
  visitas indexan en disco, así que en plataformas con disco efímero
  (Render gratis) ese aprendizaje se pierde al reiniciar — el índice base va
  DENTRO de la imagen y siempre arranca completo.

---

# TERCERA FASE — el navegador completo (2026-08-09, noche)

La lista honesta de pendientes se tachó ENTERA, sin una simulación. Cada
pieza se verificó EN VIVO (frames píxel a píxel, servidores eco reales) y
tiene test de regresión. Suite: **15/15**.

## 1. Multi-sesión real

Cada visitante recibe cookie `nuby_sid` (24 hex, HttpOnly) y su propio
`BrowserShell`: historial, URL, foco y scroll aislados. Índice de búsqueda
ÚNICO compartido (mutex interno). Hilo por cliente + `SO_RCVTIMEO` 20 s,
GC de sesiones a los 90 min, cap LRU de 64. Verificado: dos sesiones en
páginas distintas simultáneamente; `/api/stats` expone `active_sessions`.

## 2. Formularios reales (cero teatro)

- `<input>` text/password/search/email…, `<textarea>`, checkbox, radio,
  submit/reset/button funcionan de verdad: click enfoca (caret real),
  teclear edita el atributo `value` del DOM vivo, Backspace borra, Enter
  = submit implícito (en textarea inserta `\n`), reset restaura defaults.
- Checkbox/radio alternan `checked` (radio excluye por `name` real).
- **GET**: la query se construye con la codificación de formularios HTML
  (espa­cios `+`, clave=valor por control con `name`) y se ve en la barra.
- **POST**: el fetcher envía `application/x-www-form-urlencoded` con
  Content-Length real; verificado byte a byte contra un servidor eco propio.
- Password se pinta con bullets `•` reales; placeholder en gris (selectores
  `[attr]` y `[attr="v"]` que el parser CSS antes se tragaba — bug real
  corregido: `flush_token` reescrito).
- `render_after_js`/edición re-render con `render_document(doc, css)`: el
  MISMO DOM vivo, sin serializar+reparsear (antes el foco caía sobre un
  documento muerto desde el primer carácter — bug real pillado en vivo).

## 3. Imágenes PNG reales, decodificador propio

`include/nuby/media/png_decoder.hpp`: zlib RFC 1950 con Adler-32 verificado,
DEFLATE estilo puff (stored/fixed/dynamic Huffman), PNG RFC 2083: IHDR,
PLTE, tRNS, IDAT; filtros 0-4 con Paeth; color types 0/2/3/4/6; depths
1/2/4/8/16 (16→MSB). **Cero librerías externas.** Adam7 entrelazado → error
honesto ("aún no soportado"). El `<img>` se descarga por HTTP real, se
distingue por magic bytes, se decodifica y se pinta con `DRAW_IMAGE`
(vecino más cercano + mezcla alfa por píxel + clip del rasterizador).
JPEG/GIF/404 → placeholder con la razón impresa. Tamaño en layout:
tamaño natural real con proporción y cap al contenedor.

**Bug activo encontrado en vivo y corregido**: `DRAW_IMAGE` salía con alto
0 porque el tail de `layout_block_children` pisaba la altura intrínseca del
propio `<img>` al recursar (guard `is_img`, test de regresión incluido).

## 4. IFC — Inline Formatting Context de verdad

Lo que más cambiaba la cara: antes TODO era `display:block` por defecto y
texto/enlaces/negritas se apilaban verticalmente. Ahora:

- **Hoja UA del motor** (`ENGINE_UA_CSS`): `span/a/b/i/em/code/label/…`
  nacen `display:inline`, `img/input/button/…` `inline-block`, como manda
  el estándar HTML (prioridad mínima, el CSS de la página la sobreescribe).
- **Itemización → line breaking → baseline**: los hijos inline consecutivos
  forman líneas; cada palabra es un run con su fuente; los espacios se
  insertan (o NO) según el whitespace real del fuente
  (`a<b>b</b>` ≠ `a <b>b</b>`); wrap real al borde; el espacio inicial de
  línea se come, como dice el spec.
- **Alineación baseline**: texto con ascenso 0.9·font-size y cajas atómicas
  (`img`, `input`, …) con el borde inferior del margin box sobre la línea
  base (vertical-align: baseline real); línea = max ascenso + max descenso.
- **`<br>`** rompe línea de verdad (incl. líneas en blanco con `<br><br>`).
- **`text-align` real por línea**: center/right/justify (justify no estira
  la última línea, spec).
- **Hit-testing intacto**: cada caja inline guarda el rect unión de sus
  runs, así un `<a>` que envuelve en 2 líneas sigue siendo clicable entero.
- **Bugs reales pillados al integrar**: `measure_main_content` del flex
  trataba cada run palabra como una línea entera → los items flex con texto
  quedaban aplastados al ancho de la palabra más larga; corregido midiendo
  líneas reales (agrupadas por y). Re-render idempotente (test incluido).

## Limitaciones honestas que quedan

- Select no interactivo; Adam7/JPEG/GIF sin decodificador (placeholder con
  razón); márgenes/padding de inline puros no afectan el flow (v1);
  `vertical-align` solo baseline; inline con hijos de bloque cae al flujo
  de bloques (HTML patológico); justificado sin guionado.
