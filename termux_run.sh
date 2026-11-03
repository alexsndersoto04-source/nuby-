#!/data/data/com.termux/files/usr/bin/bash
# Lanza el banco de medida en Kaggle y recoge la tabla.
#
#   bash termux_run.sh             lanzar y esperar el resultado
#   bash termux_run.sh --no-wait   solo lanzar (puedes cerrar Termux)
#   bash termux_run.sh --fetch     recoger el resultado de la ultima corrida
#
# Todo el trabajo ocurre en Kaggle. El telefono solo manda y recibe.
set -u

command -v proot-distro >/dev/null || {
    echo "Falta proot-distro. Ejecuta primero:  bash termux_setup.sh"
    exit 1
}

if proot-distro login debian -- test -f /root/kaggle_run.py 2>/dev/null; then
    proot-distro login debian -- /root/kv/bin/python /root/kaggle_run.py "$@"
else
    echo "El lanzador no esta instalado. Ejecuta primero:  bash termux_setup.sh"
    exit 1
fi
