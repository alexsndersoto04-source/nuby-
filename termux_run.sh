#!/data/data/com.termux/files/usr/bin/bash
# Lanza el banco de medida en Kaggle y recoge la tabla.
#
#   bash termux_run.sh             lanzar y esperar el resultado
#   bash termux_run.sh --no-wait   solo lanzar (puedes cerrar Termux)
#   bash termux_run.sh --fetch     recoger el resultado de la ultima corrida
#
# Todo el trabajo ocurre en Kaggle. El telefono solo manda y recibe.
set -u

# Comprobacion barata antes de arrancar Debian: si no hay credenciales, mejor
# decirlo aqui que esperar a que falle dentro.
if ! proot-distro login debian -- test -f /root/.kaggle/kaggle.json 2>/dev/null; then
    echo "Faltan las credenciales de Kaggle."
    echo "Ejecuta primero:  bash setup.sh"
    exit 1
fi

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
