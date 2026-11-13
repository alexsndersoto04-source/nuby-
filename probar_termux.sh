#!/usr/bin/env bash
# probar_termux.sh — Instalador y lanzador directo de CORTEX-LM en Termux (Android) o Linux.
# Compila el motor 100% C++ spiking LIF nativo en el dispositivo, crea y entrena el cerebro
# la primera vez (conservandolo intacto en ejecuciones futuras) y abre el chat en vivo.
set -euo pipefail

REPO="${CORTEX_REPO:-alexsndersoto04-source/nuby-}"
BRANCH="${CORTEX_BRANCH:-main}"

echo "=========================================================="
echo "  CORTEX-LM (100% Spiking LIF en C++) — Modo Termux/Local"
echo "=========================================================="

# 1. En Termux, instalar compilador C++ y utilidades solo si faltan y activar wake-lock
if command -v pkg >/dev/null 2>&1; then
    FALTAN=()
    command -v clang++ >/dev/null 2>&1 || FALTAN+=("clang")
    command -v make    >/dev/null 2>&1 || FALTAN+=("make")
    command -v git     >/dev/null 2>&1 || FALTAN+=("git")
    command -v curl    >/dev/null 2>&1 || FALTAN+=("curl")
    command -v python3 >/dev/null 2>&1 || FALTAN+=("python")
    if [ "${#FALTAN[@]}" -gt 0 ]; then
        echo "[1/4] Instalando herramientas en Termux: ${FALTAN[*]} ..."
        pkg update -y
        pkg install -y "${FALTAN[@]}"
    fi
    if command -v termux-wake-lock >/dev/null 2>&1; then
        termux-wake-lock || true
        trap 'termux-wake-unlock 2>/dev/null || true' EXIT
    fi
fi

# 2. Si este script se ejecuto suelto (con curl), clonar o actualizar la rama en ~/ask-cortex
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
if [ -d "${SCRIPT_DIR}/cortex-lm" ]; then
    ROOT_DIR="${SCRIPT_DIR}"
else
    ROOT_DIR="${HOME}/ask-cortex"
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

# 3. Compilar binarios C++ nativos (-O3) para el procesador del telefono/PC
echo "[2/4] Compilando motor neuronal C++ nativo (-O3)..."
make portable

echo "[3/4] Ejecutando auditoria biofisica estructural..."
./bin/cortex_lm audit

# 4. Construir cerebro_es.bin solo la primera vez (nunca sobrescribe tus recuerdos ni lecturas)
if [ "${1:-}" = "--reentrenar" ] || [ ! -s "data/cerebro_es.bin" ]; then
    echo "[4/4] Primera ejecucion en este dispositivo: creando y entrenando tu cerebro LIF"
    echo "      de 4 regiones (29,696 neuronas, 2.11M sinapsis CSR)."
    echo "      Esto ocurre UNA SOLA VEZ (~8-15 min segun tu telefono); luego abrira en 0.2 s."
    bash tools/entrenar_completo.sh
else
    echo "[4/4] Cerebro encontrado en data/cerebro_es.bin (carga instantanea, conserva tus recuerdos)."
fi

echo ""
echo "=========================================================="
echo "  LISTO — Entrando al chat en vivo con CORTEX-LM"
echo "  Comandos utiles dentro del chat:"
echo "    /recordar <dato>   -> memoriza un hecho nuevo en 1 disparo"
echo "    /continuar <texto> -> genera prosa literaria libre"
echo "    /pensar            -> muestra/oculta el pensamiento LIF"
echo "    /nuevo             -> limpia el hilo actual"
echo "    /salir             -> cierra la sesion"
echo "=========================================================="
echo ""

exec ./bin/cerebro interactivo data/cerebro_es.bin --pensar
