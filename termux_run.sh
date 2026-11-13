#!/data/data/com.termux/files/usr/bin/bash
# Lanza el banco de medida en Kaggle y recoge la tabla.
#
#   bash termux_run.sh             lanzar y esperar el resultado
#   bash termux_run.sh --no-wait   solo lanzar (puedes cerrar Termux)
#   bash termux_run.sh --fetch     recoger el resultado de la ultima corrida
#
# Todo el trabajo ocurre en Kaggle. El telefono solo manda y recibe.
set -u

BRANCH="${CORTEX_BRANCH:-main}"
REPO="${CORTEX_REPO:-alexsndersoto04-source/nuby-}"
RAW="https://raw.githubusercontent.com/$REPO/$BRANCH"

die() { printf '\n\033[31m%s\033[0m\n' "$*" >&2; exit 1; }
ok()  { printf '\n\033[1m==> %s\033[0m\n' "$*"; }

command -v proot-distro >/dev/null || die "Falta proot-distro.
Instala todo con:
  curl -fsSL $RAW/termux_setup.sh -o setup.sh && bash setup.sh"

# --- credenciales -------------------------------------------------------
if ! proot-distro login debian -- test -s /root/.kaggle/kaggle.json 2>/dev/null; then
    die "Faltan las credenciales de Kaggle.
Crealas con:
  curl -fsSL $RAW/tools/termux_token.sh -o token.sh && bash token.sh"
fi

# --- Python dentro de Debian -------------------------------------------
if ! proot-distro login debian -- test -x /root/kv/bin/python 2>/dev/null; then
    die "Falta Python dentro de Debian.
Instalalo con:
  curl -fsSL $RAW/termux_setup.sh -o setup.sh && bash setup.sh"
fi

# --- lanzador: se instala solo si falta --------------------------------
# El setup original solo lo instalaba al final, y si se paraba antes (por
# ejemplo buscando las credenciales) nunca llegaba a instalarse. Aqui se
# comprueba y se pone al dia sin obligar a repetir toda la instalacion.
if ! proot-distro login debian -- test -s /root/kaggle_run.py 2>/dev/null; then
    ok "Instalando el lanzador (solo la primera vez)"
    proot-distro login debian -- bash -c \
        "curl -fsSL '$RAW/tools/kaggle_run.py' -o /root/kaggle_run.py" \
        || die "No pude descargar el lanzador.
Comprueba que hay conexion y vuelve a intentarlo."
    proot-distro login debian -- test -s /root/kaggle_run.py 2>/dev/null \
        || die "El lanzador no llego a Debian."
    echo "     lanzador instalado"
fi

# --- arrancar -----------------------------------------------------------
proot-distro login debian -- /root/kv/bin/python /root/kaggle_run.py "$@"
