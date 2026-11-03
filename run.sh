#!/data/data/com.termux/files/usr/bin/bash
# Arranca la medida en Kaggle desde el telefono.
#
#   bash run.sh             lanzar y esperar el resultado
#   bash run.sh --no-wait   solo lanzar (puedes cerrar Termux)
#   bash run.sh --fetch     recoger el resultado de la ultima corrida
#
# Este fichero se baja UNA vez a mano y a partir de ahi no cambia. Todo lo que
# evoluciona (el lanzador, el cuaderno, el motor) se descarga en el momento,
# asi que no hace falta volver a bajarlo aunque el proyecto cambie.
set -u

BRANCH="arena/c7deda7f-nuby"
REPO="alexsndersoto04-source/nuby-"
RAW="https://raw.githubusercontent.com/$REPO/$BRANCH"

# Evitar que curl reutilice una copia vieja: se pide con cabeceras de no-cache.
CURL="curl -fsSL -H Cache-Control:no-cache -H Pragma:no-cache"

step() { printf '\n\033[1m==> %s\033[0m\n' "$*"; }
die()  { printf '\n\033[31m%s\033[0m\n' "$*" >&2; exit 1; }

step "Comprobando el entorno"
command -v proot-distro >/dev/null || die "Falta proot-distro.
Primero:
  pkg install -y proot-distro curl
Y despues el setup completo."

proot-distro login debian -- test -s /root/kv/bin/python 2>/dev/null || die "Falta Python dentro de Debian (o esta roto).
Solucion:
  bash setup.sh"

# Kaggle CLI 2.2.5 usa ~/.kaggle/access_token; las versiones antiguas usaban
# ~/.kaggle/kaggle.json. Vale cualquiera de los dos.
if proot-distro login debian -- test -s /root/.kaggle/access_token 2>/dev/null; then
    :
elif proot-distro login debian -- test -s /root/.kaggle/kaggle.json 2>/dev/null; then
    :
else
    die "Faltan las credenciales de Kaggle.
Solucion:
  bash token.sh"
fi

# --- el lanzador se descarga SIEMPRE, sin comprobar versiones ------------
step "Preparando el lanzador"
proot-distro login debian -- bash -c \
    "$CURL '$RAW/tools/kaggle_run.py' -o /root/kaggle_run.py" \
  || die "No pude descargar el lanzador. ¿Hay conexion?"

proot-distro login debian -- test -s /root/kaggle_run.py 2>/dev/null \
  || die "El lanzador llego vacio. Vuelve a intentarlo."

# Comprobar que es la version nueva: la vieja no tenia esto.
if proot-distro login debian -- grep -q "user_de_kaggle" /root/kaggle_run.py 2>/dev/null; then
    echo "     lanzador listo"
else
    die "El lanzador descargado no es el esperado.
Comprueba la conexion y vuelve a intentarlo."
fi

# --- arrancar ------------------------------------------------------------
step "Arrancando"
proot-distro login debian -- /root/kv/bin/python /root/kaggle_run.py "$@"
