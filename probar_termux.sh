#!/usr/bin/env bash
# probar_termux.sh — Instalador y lanzador directo de CORTEX-LM en Termux (Android) o Linux.
# Compila el motor 100% C++ spiking LIF en el telefono, descarga el cerebro pre-entrenado
# (si aun no esta en disco) y abre la sesion interactiva en vivo.
set -euo pipefail

REPO="alexsndersoto04-source/nuby-"
BRANCH="arena/886cbd4f-nuby"
REL_TAG="cortex-lm-termux-v1"
MODEL_URL="https://github.com/${REPO}/releases/download/${REL_TAG}/cerebro_es.bin.gz"

echo "=========================================================="
echo "  CORTEX-LM (100% Spiking LIF en C++) — Modo Termux/Local"
echo "=========================================================="

# 1. En Termux, instalar compilador C++ y utilidades solo si faltan
if command -v pkg >/dev/null 2>&1; then
    FALTAN=()
    command -v clang++ >/dev/null 2>&1 || FALTAN+=("clang")
    command -v make    >/dev/null 2>&1 || FALTAN+=("make")
    command -v git     >/dev/null 2>&1 || FALTAN+=("git")
    command -v curl    >/dev/null 2>&1 || FALTAN+=("curl")
    command -v gzip    >/dev/null 2>&1 || FALTAN+=("gzip")
    command -v python3 >/dev/null 2>&1 || FALTAN+=("python")
    if [ "${#FALTAN[@]}" -gt 0 ]; then
        echo "[1/4] Instalando herramientas en Termux: ${FALTAN[*]} ..."
        pkg update -y
        pkg install -y "${FALTAN[@]}"
    fi
fi

# 2. Si este script se ejecuto suelto (fuera del clon), clonar o actualizar la rama
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
if [ -d "${SCRIPT_DIR}/cortex-lm" ]; then
    ROOT_DIR="${SCRIPT_DIR}"
else
    ROOT_DIR="${HOME}/nuby-cortex"
    if [ ! -d "${ROOT_DIR}/.git" ]; then
        echo "[1/4] Descargando repositorio CORTEX-LM (${BRANCH})..."
        git clone --depth 1 -b "${BRANCH}" "https://github.com/${REPO}.git" "${ROOT_DIR}"
    else
        echo "[1/4] Actualizando codigo en ${ROOT_DIR}..."
        git -C "${ROOT_DIR}" pull --ff-only origin "${BRANCH}" || true
    fi
fi

cd "${ROOT_DIR}/cortex-lm"
mkdir -p data

# 3. Compilar binarios C++ nativos para el procesador del dispositivo
if [ ! -x "./bin/cerebro" ] || [ ! -x "./bin/cortex_lm" ]; then
    echo "[2/4] Compilando motor neuronal C++ nativo (-O3)..."
    make portable
else
    echo "[2/4] Motor C++ ya compilado en ./bin/cerebro"
fi

echo "[3/4] Ejecutando auditoria biofisica estructural..."
./bin/cortex_lm audit

# 4. Obtener cerebro_es.bin (sin sobrescribir si ya existe con tus recuerdos/libros)
ENTRENAR_LOCAL=0
if [ "${1:-}" = "--entrenar-local" ]; then
    ENTRENAR_LOCAL=1
fi

if [ "${ENTRENAR_LOCAL}" -eq 1 ]; then
    echo "[4/4] Entrenando cerebro localmente desde cero..."
    bash tools/entrenar_completo.sh
elif [ ! -s "data/cerebro_es.bin" ]; then
    echo "[4/4] Descargando cerebro pre-entrenado (cerebro_es.bin.gz)..."
    if curl -fL "${MODEL_URL}" -o data/cerebro_es.bin.gz; then
        echo "      Descomprimiendo data/cerebro_es.bin..."
        gzip -df data/cerebro_es.bin.gz
    else
        echo "      No se pudo descargar el release; entrenando localmente..."
        bash tools/entrenar_completo.sh
    fi
else
    echo "[4/4] Usando tu cerebro existente en data/cerebro_es.bin (conserva tus recuerdos)."
fi

echo ""
echo "=========================================================="
echo "  LISTO — Entrando al chat en vivo con CORTEX-LM"
echo "  Comandos utiles dentro del chat:"
echo "    /recordar <dato>   -> memoriza un hecho nuevo al instante"
echo "    /continuar <texto> -> genera prosa literaria libre"
echo "    /pensar            -> muestra/oculta el pensamiento LIF"
echo "    /nuevo             -> limpia el hilo actual"
echo "    /salir             -> cierra la sesion"
echo "=========================================================="
echo ""

exec ./bin/cerebro interactivo data/cerebro_es.bin --pensar
