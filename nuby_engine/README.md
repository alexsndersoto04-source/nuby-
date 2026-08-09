# NUBY — el navegador que se pinta a sí mismo

Motor de navegador escrito en **C++20 desde cero** donde **toda la interfaz
(botones, barra de direcciones, páginas) la rasteriza su propio motor**, no el
navegador del usuario. Tu navegador actúa solo como monitor remoto: recibe
píxeles y devuelve clicks y teclas.

```bash
cd nuby_engine
make                 # compila (cero dependencias externas)
./bin/nuby_engine --port 8080
# Abre http://localhost:8080
```

## Qué es real aquí

- **Pipeline de render propio**: HTML → CSS (cascada+especificidad) → layout
  (bloques + flex) → display list → rasterizador software con fuente bitmap
  propia (UTF-8 y acentos incluidos).
- **La UI del navegador la dibuja el motor**: se define en HTML/CSS pero la
  parsea y pinta Nuby. Compruébalo: desconecta tu CSS — seguirá igual.
- **Red real**: DNS/TCP/HTTP propios, HTTPS mediante el OpenSSL del sistema,
  chunked, redirects, URLs relativas. Las páginas web reales se descargan y se
  renderizan con el motor (dentro del subconjunto soportado).
- **Buscador real**: índice invertido + ranking **BM25** escritos a mano;
  precargado con 995 páginas **realmente rastreadas**; cada página que visitas
  se indexa automáticamente. Sin resultados prefabricados: si algo no está,
  verás 0 resultados.
- **JavaScript real**: intérprete propio (lexer, parser, AST, closures) para
  un subconjunto documentado; los clicks disparan handlers que mutan el DOM y
  se repinta.
- **Honestidad ante todo**: los errores de red se muestran tal cual, y
  `nuby://about` lista lo soportado y lo que no (sin imágenes, sin gzip,
  subconjunto CSS/JS…).

## Historia del proyecto y corrección de rumbo

Este repo contenía un motor que compilaba pero cuyo "navegador" era una web
servida a Chrome (Chrome pintaba la interfaz), con búsqueda hardcodeada y un
falso "JS engine" basado en comparaciones de texto. La versión 2.0 eliminó ese
teatro: el índice, el intérprete, la red y **la pintura de la interfaz** son
reales y están probados (`make test`, 7/7 suites).

Detalles técnicos sin marketing: [`ARCHITECTURE.md`](ARCHITECTURE.md).

## Utilidades

- `tools/export_index.py` — exporta el rastreo real (SQLite de `spider.py`)
  al TSV del índice.
- `spider.py` / `main.py` — crawler y buscador FTS5 de la etapa Python
  (funcionaban de verdad; el índice actual parte de sus datos).
