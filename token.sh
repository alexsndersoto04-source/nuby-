#!/data/data/com.termux/files/usr/bin/bash
# Crea el fichero de credenciales de Kaggle a partir de la clave que ya tienes.
#
# Uso:  bash token.sh
#
# Se baja UNA vez a mano y no cambia. El instalador de verdad se descarga en
# el momento, asi que este fichero no hay que volver a bajarlo.
set -u

BRANCH="arena/c7deda7f-nuby"
REPO="alexsndersoto04-source/nuby-"
RAW="https://raw.githubusercontent.com/$REPO/$BRANCH"
CURL="curl -fsSL -H Cache-Control:no-cache -H Pragma:no-cache"

step() { printf '\n\033[1m==> %s\033[0m\n' "$*"; }
die()  { printf '\n\033[31mERROR: %s\033[0m\n' "$*" >&2; exit 1; }

command -v proot-distro >/dev/null || die "Falta proot-distro.
Primero:
  pkg install -y proot-distro curl"

step "Descargando el instalador"
# Se guarda en Termux (no en Debian) porque el instalador tiene que leer lo que
# escribes por teclado, y eso solo funciona en la terminal del telefono.
TMP="${PREFIX:-/data/data/com.termux/files/usr}/tmp"
mkdir -p "$TMP" 2>/dev/null || TMP="$HOME"
INST="$TMP/cortex_token.sh"

$CURL "$RAW/tools/termux_token.sh" -o "$INST" || die "No pude descargarlo.
Comprueba que hay conexion."

[ -s "$INST" ] || die "El instalador llego vacio."

# Comprobar que es el bueno y no una copia vieja o una pagina de error.
# Las cadenas tienen que ser estables: antes se comprobaba "Usuario de Kaggle",
# que desaparecio al cambiar el mensaje del prompt, y el propio token.sh se
# quedo inservible. Ahora se busca algo que no va a cambiar: que sepa tratar
# los dos formatos de credenciales.
if ! grep -q "access_token" "$INST" 2>/dev/null || \
   ! grep -q "KGAT_" "$INST" 2>/dev/null; then
    die "El instalador descargado no es el esperado (¿copia vieja en cache?).
Mira que contiene:
    head -5 \"$INST\"
Y si es basura, vuelve a intentarlo."
fi

step "Arrancando el instalador"
exec bash "$INST"
