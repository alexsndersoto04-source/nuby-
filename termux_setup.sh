#!/data/data/com.termux/files/usr/bin/bash
# Instalacion en Termux. Se ejecuta con:  bash termux_setup.sh
set -u
URL="https://raw.githubusercontent.com/alexsndersoto04-source/ask-/arena/c7deda7f-ask/tools/termux_setup.sh"
curl -fsSL "$URL" -o "$PREFIX/tmp/cortex_setup.sh" || {
    echo "No hay conexion o la URL no responde."; exit 1; }
bash "$PREFIX/tmp/cortex_setup.sh" "$@"
