#!/usr/bin/env bash
# entrenar_noche_termux.sh — Entrenamiento Continuo en Segundo Plano (con pantalla apagada)
# para CORTEX-LM Macro («Corteza Viva») directo en Termux / Linux, SIN depender de Google Colab.
#
# ¿Por qué no se corta nunca al apagar la pantalla o salir de la app?
#   - Activa `termux-wake-lock` (evita que Android duerma el procesador).
#   - Lanza el proceso desacoplado con `nohup ... &` (puedes cerrar Chrome, cambiar de app o dormir).
#   - Descarga lotes pequeños en `/tmp`, consolida en modo `--continuo` y BORRA cada lote temporal
#     al instante (0 MB de datasets acumulados en tu teléfono).
#
# Uso en Termux:
#   ./entrenar_noche_termux.sh iniciar   # Compila, activa wake-lock y entrena solo en segundo plano
#   ./entrenar_noche_termux.sh estado    # Ver progreso en vivo (Ctrl+C para salir sin detenerlo)
#   ./entrenar_noche_termux.sh hablar    # Conversar en consola con Corteza Viva (--pensar --vivo --honesto)
#   ./entrenar_noche_termux.sh web       # Abrir Observatorio Cortical Web en http://127.0.0.1:8080
#   ./entrenar_noche_termux.sh detener   # Pausar limpiamente (el avance queda guardado en el .bin)

set -euo pipefail

RAIZ="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
DIR_CORTEX="$RAIZ/cortex-lm"
MODELO="$DIR_CORTEX/data/cerebro_viva_es.bin"
LOG="$DIR_CORTEX/entrenamiento_noche.log"
PIDFILE="$DIR_CORTEX/entrenamiento_noche.pid"

ACCION="${1:-iniciar}"

detectar_perfil_ram() {
    local mem_kb=4000000
    if [ -r /proc/meminfo ]; then
        mem_kb="$(awk '/MemTotal:/ {print $2}' /proc/meminfo 2>/dev/null || echo 4000000)"
    fi
    # >= 5.5 GB RAM -> equilibrio-165k (165,376 neuronas LIF, ~330 MB RAM)
    # <  5.5 GB RAM -> agil-82k        ( 80,896 neuronas LIF, ~175 MB RAM, muy rápido)
    if [ "${mem_kb:-0}" -ge 5500000 ]; then
        echo "equilibrio-165k"
    else
        echo "agil-82k"
    fi
}

compilar_si_falta() {
    if [ -n "${PREFIX:-}" ] && command -v pkg >/dev/null 2>&1; then
        for p in clang make python git; do
            command -v "$p" >/dev/null 2>&1 || pkg install -y "$p"
        done
    fi
    cd "$DIR_CORTEX"
    if [ ! -x ./bin/cerebro ]; then
        echo "[1/2] Compilando motor C++17 CORTEX-LM Macro..."
        local hilos
        hilos="$(nproc 2>/dev/null || echo 2)"
        make -j"$hilos" || make portable -j"$hilos"
    fi
}

case "$ACCION" in
    iniciar)
        if [ -f "$PIDFILE" ] && kill -0 "$(cat "$PIDFILE")" 2>/dev/null; then
            echo "Ya hay un entrenamiento nocturno corriendo en segundo plano (PID $(cat "$PIDFILE"))."
            echo "Usa './entrenar_noche_termux.sh estado' para ver su avance."
            exit 0
        fi

        compilar_si_falta

        if command -v termux-wake-lock >/dev/null 2>&1; then
            termux-wake-lock || true
            echo "[WAKE-LOCK ACTIVO] Android mantendrá el procesador despierto aunque apagues la pantalla."
        fi

        PERFIL="${PERFIL_CORTEX:-$(detectar_perfil_ram)}"
        LOTES_LIBROS="${LOTES_LIBROS:-3}"
        LOTES_WIKI="${LOTES_WIKI:-6}"

        cd "$DIR_CORTEX"
        nohup python3 tools/entrenar_macro_colab.py \
            --perfil "$PERFIL" \
            --salida "$MODELO" \
            --lotes-libros "$LOTES_LIBROS" \
            --lotes-wiki "$LOTES_WIKI" \
            --bytes-lote 180000 \
            >> "$LOG" 2>&1 &
        NEW_PID=$!
        echo "$NEW_PID" > "$PIDFILE"

        echo ""
        echo "=================================================================="
        echo " CORTEX-LM («Corteza Viva») ENTRENANDO EN SEGUNDO PLANO"
        echo "   PID           : $NEW_PID"
        echo "   Perfil        : $PERFIL (5 regiones + Tálamo TRN + FP16)"
        echo "   Cerebro (.bin): $MODELO"
        echo "   Registro      : $LOG"
        echo "=================================================================="
        echo " -> YA PUEDES SALIR DE LA APP, BLOQUEAR LA PANTALLA E IRTE A DORMIR."
        echo " -> Para mirar cómo va cuando quieras: ./entrenar_noche_termux.sh estado"
        echo " -> Para hablar con él al terminar   : ./entrenar_noche_termux.sh hablar"
        echo " -> Para abrir el panel visual web   : ./entrenar_noche_termux.sh web"
        ;;

    estado)
        if [ -f "$PIDFILE" ] && kill -0 "$(cat "$PIDFILE")" 2>/dev/null; then
            echo "[ACTIVO] Entrenando en segundo plano (PID $(cat "$PIDFILE")). Mostrando últimas líneas (Ctrl+C para salir de la vista):"
        else
            echo "[INACTIVO] El proceso en segundo plano ya terminó o no está iniciado."
        fi
        echo "------------------------------------------------------------------"
        if [ -f "$LOG" ]; then
            tail -n 30 "$LOG"
        else
            echo "Aún no existe $LOG."
        fi
        ;;

    hablar)
        compilar_si_falta
        if [ ! -f "$MODELO" ]; then
            echo "Aún no existe '$MODELO'. Ejecuta primero './entrenar_noche_termux.sh iniciar'."
            exit 1
        fi
        cd "$DIR_CORTEX"
        exec ./bin/cerebro interactivo "$MODELO" --pensar --vivo --honesto
        ;;

    web)
        compilar_si_falta
        if [ ! -f "$MODELO" ]; then
            echo "Aún no existe '$MODELO'. Ejecuta primero './entrenar_noche_termux.sh iniciar'."
            exit 1
        fi
        echo "Abriendo Observatorio Cortical Vivo en http://127.0.0.1:8080 ..."
        cd "$DIR_CORTEX"
        exec python3 tools/servidor_vivo.py --modelo "$MODELO" --puerto 8080
        ;;

    detener)
        if [ -f "$PIDFILE" ] && kill -0 "$(cat "$PIDFILE")" 2>/dev/null; then
            kill "$(cat "$PIDFILE")" || true
            rm -f "$PIDFILE"
            echo "Proceso detenido. El último lote completado quedó guardado en '$MODELO'."
        else
            echo "No hay entrenamiento corriendo."
        fi
        if command -v termux-wake-unlock >/dev/null 2>&1; then
            termux-wake-unlock || true
        fi
        ;;

    *)
        echo "Uso: $0 {iniciar|estado|hablar|web|detener}"
        exit 1
        ;;
esac
