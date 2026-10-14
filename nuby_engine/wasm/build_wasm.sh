#!/usr/bin/env bash
# ============================================================================
# Compila el motor Nuby a WebAssembly (wasm32-wasi) usando Zig como cadena de
# herramientas C++ (Zig trae libc++ para WASI; no hace falta emscripten).
#
#   Uso:  nuby_engine/wasm/build_wasm.sh [salida.wasm]
#
# Resultado: web/nuby.wasm — el motor REAL (parser HTML, CSS, layout,
# rasterizador, fuentes TrueType, buscador BM25 y la UI del navegador) listo
# para ejecutarse dentro de una página, sin servidor.
# ============================================================================
set -euo pipefail

ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
OUT="${1:-$ROOT/web/nuby.wasm}"

# Localiza zig: puede estar en el PATH o instalado como paquete de Python.
ZIG="${ZIG:-}"
if [ -z "$ZIG" ]; then
    if command -v zig >/dev/null 2>&1; then
        ZIG="$(command -v zig)"
    else
        ZIG="$(python3 - <<'PY'
import os, ziglang
print(os.path.join(os.path.dirname(ziglang.__file__), "zig"))
PY
)"
    fi
fi

EXPORTS=(
    nuby_alloc nuby_boot nuby_documents nuby_terms nuby_width nuby_height
    nuby_click nuby_wheel nuby_char nuby_key nuby_resize nuby_set_scale nuby_goto nuby_tick
    nuby_frame_rle nuby_state nuby_search nuby_history
)
LDFLAGS=(--no-entry --strip-all -z stack-size=4194304)
for e in "${EXPORTS[@]}"; do LDFLAGS+=(--export="$e"); done

echo "[wasm] zig:    $ZIG"
echo "[wasm] fuente: nuby_engine/wasm/nuby_wasm.cpp"
echo "[wasm] salida: $OUT"

"$ZIG" c++ \
    -target wasm32-wasi \
    -std=c++20 -O3 -fno-exceptions \
    -I "$ROOT/nuby_engine/include" \
    "$ROOT/nuby_engine/wasm/nuby_wasm.cpp" \
    "$ROOT/nuby_engine/src/core/types.cpp" \
    "$ROOT/nuby_engine/src/css/css_value.cpp" \
    "$ROOT/nuby_engine/src/css/css_rule.cpp" \
    -o "$OUT" \
    -Wl,"$(IFS=,; echo "${LDFLAGS[*]}")" 2>&1 | grep -v '^$' || true

# El motor lee su indice y sus fuentes del sistema de archivos. Para servir
# la pagina sin depender de rutas del repositorio, se copian aqui (no se
# versionan: los originales viven en nuby_engine/data).
WEB_DIR="$(dirname "$OUT")"
mkdir -p "$WEB_DIR/data/fonts"
cp "$ROOT/nuby_engine/data/crawl_pages.tsv" "$WEB_DIR/data/crawl_pages.tsv"
cp "$ROOT/nuby_engine/data/fonts/"*.ttf "$WEB_DIR/data/fonts/"

ls -la "$OUT"
echo "[wasm] hecho: motor, indice y fuentes listos en $WEB_DIR"
