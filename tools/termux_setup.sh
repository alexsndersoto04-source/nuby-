#!/data/data/com.termux/files/usr/bin/bash
# Instalacion unica de todo lo necesario para lanzar el banco en Kaggle
# desde el telefono, sin copiar y pegar nada.
#
# Por que hace falta esto: `pip install kaggle` NO compila en Termux con
# Python 3.14. El paquete arrastra rpds-py, que para Android solo existe como
# codigo fuente y necesita Rust. Dentro de una Debian (proot-distro) si
# funciona, porque ahi hay paquetes precompilados.
#
# Uso:  bash termux_setup.sh
set -u

BRANCH="${CORTEX_BRANCH:-arena/c7deda7f-ask}"
REPO="${CORTEX_REPO:-alexsndersoto04-source/ask-}"
RAW="https://raw.githubusercontent.com/$REPO/$BRANCH"
PY="/root/kv/bin/python"
KAG="/root/kv/bin/kaggle"

say() { printf '\n\033[1m==> %s\033[0m\n' "$*"; }
die() { printf '\n\033[31mERROR: %s\033[0m\n' "$*" >&2; exit 1; }

# ---------------------------------------------------------------- 1. paquetes
say "1/5  Instalando herramientas de Termux"
pkg install -y proot-distro curl >/dev/null 2>&1 || true
command -v proot-distro >/dev/null || die "no se pudo instalar proot-distro"
command -v curl >/dev/null || die "no se pudo instalar curl"

# ---------------------------------------------------------------- 2. debian
say "2/5  Instalando Debian dentro del telefono (una sola vez, ~400 MB)"
if proot-distro login debian -- true >/dev/null 2>&1; then
    echo "     ya estaba instalada, se reutiliza"
else
    proot-distro install debian || die "fallo al instalar Debian"
fi

# ------------------------------------------------- 3. python y kaggle dentro
say "3/5  Instalando Python y la herramienta de Kaggle dentro de Debian"
proot-distro login debian -- bash -c '
set -e
export DEBIAN_FRONTEND=noninteractive
apt-get update -qq
apt-get install -y -qq python3 python3-venv python3-pip ca-certificates curl >/dev/null
if [ ! -x /root/kv/bin/pip ]; then
    python3 -m venv /root/kv
fi
/root/kv/bin/pip install -q --upgrade pip
# `six` tiene que ir explicito: el paquete de Kaggle lo usa (kaggle/models/
# upload_file.py) pero no lo declara como dependencia, asi que en una
# instalacion limpia se rompe al arrancar con ModuleNotFoundError: six.
/root/kv/bin/pip install -q kaggle six
# comprobar que arranca de verdad, no solo que pip dijo que si
if ! /root/kv/bin/kaggle --version; then
    echo "Kaggle no arranca; faltan dependencias. Instalando las habituales..."
    /root/kv/bin/pip install -q six urllib3 certifi python-dateutil requests tqdm
    /root/kv/bin/kaggle --version
fi
' || die "fallo al instalar kaggle dentro de Debian"

# ---------------------------------------------------------------- 4. credenciales
say "4/5  Buscando tu kaggle.json"
CREDS=""
for p in \
    "$HOME/storage/downloads/kaggle.json" \
    "$HOME/storage/downloads/Download/kaggle.json" \
    "$HOME/downloads/kaggle.json" \
    "$HOME/Download/kaggle.json" \
    "$HOME/kaggle.json"
do
    [ -f "$p" ] && CREDS="$p" && break
done
# Las dos rutas mas probables: el home y la memoria del telefono.
if [ -z "$CREDS" ]; then
    CREDS=$(find "$HOME" -maxdepth 6 -name 'kaggle*.json' 2>/dev/null | head -1)
fi
if [ -z "$CREDS" ]; then
    # Ojo: el navegador a veces guarda "kaggle (1).json", asi que se busca
    # cualquier json que empiece por kaggle, y con mas profundidad.
    CREDS=$(find /sdcard /storage/emulated/0 -maxdepth 6 \
             \( -name 'kaggle*.json' -o -name '*kaggle*.json' \) \
             -not -path '*/Android/data/*' -not -path '*/Android/obb/*' \
             2>/dev/null | head -1)
fi

# Si aparece, se copia al home para no depender del permiso de la memoria.
case "$CREDS" in
    /sdcard/*|/storage/*)
        mkdir -p "$HOME"
        cp "$CREDS" "$HOME/kaggle.json" && echo "     copiado desde la memoria del telefono"
        CREDS="$HOME/kaggle.json"
        ;;
esac

if [ -z "$CREDS" ]; then
    cat <<'AYUDA'

Kaggle 2.2.5 ya funciona, solo falta el fichero de credenciales
(es el mismo para siempre: se baja una vez).

  1. En el navegador abre  https://www.kaggle.com/settings
  2. Baja hasta "API" y pulsa "Create New Token"
  3. Se descargara un fichero llamado kaggle.json

Ahora hay que dejarlo en la carpeta de Descargas del telefono para que este
script lo encuentre. Lo mas facil:

       termux-setup-storage

(Acepta el permiso que saldra en pantalla. Si ya lo aceptaste antes, este paso
no hace falta.)

Y despues:

  - Si el fichero se quedo en Descargas, nada mas: vuelve a lanzar esto
       bash setup.sh
    y lo encontrara solo.

  - Si no lo encuentras, muevelo tu al home de Termux. Desde el navegador
    suele estar en "Descargas"; puedes copiarlo con:

       cp /sdcard/Download/kaggle.json ~/kaggle.json

    (o /sdcard/Descargas/kaggle.json si el telefono esta en espanol)

Comprobar que quedo bien:

       ls -l ~/kaggle.json

Y volver a lanzar:

       bash setup.sh

AYUDA
    # Diagnostico: decir que SI se ve en la carpeta de descargas. Sin esto el
    # usuario no sabe si el problema es el nombre, la carpeta o que el fichero
    # no se descargo.
    echo "--- diagnostico ---"
    for d in /sdcard/Download /sdcard/Downloads /sdcard/Descargas \
             /storage/emulated/0/Download "$HOME"; do
        [ -d "$d" ] || continue
        js=$(ls -1 "$d" 2>/dev/null | grep -i '\.json$' | head -5)
        if [ -n "$js" ]; then
            echo "  json en $d:"
            echo "$js" | sed 's/^/    /'
        fi
    done
    echo "  (si no aparece ninguno, el fichero no se ha descargado)"
    echo
    echo "Plan B sin credenciales: ver kaggle/README.md, seccion"
    echo "'Ruta sin credenciales'. No necesita kaggle.json."
    exit 1
fi
echo "     encontrado en: $CREDS"

proot-distro login debian -- bash -c "
mkdir -p /root/.kaggle
cp '$CREDS' /root/.kaggle/kaggle.json
chmod 600 /root/.kaggle/kaggle.json
" || die "no se pudo copiar kaggle.json a Debian"

# comprobar que las credenciales sirven
USERK=$(proot-distro login debian -- bash -c "
$PY - <<'EOF'
import json
print(json.load(open('/root/.kaggle/kaggle.json'))['username'])
EOF
" 2>/dev/null | tr -d '\r')
[ -n "$USERK" ] || die "kaggle.json no tiene el formato esperado"
echo "     usuario de Kaggle: $USERK"

# ---------------------------------------------------------------- 5. lanzador
say "5/5  Instalando el lanzador"
proot-distro login debian -- bash -c "
curl -fsSL '$RAW/tools/kaggle_run.py' -o /root/kaggle_run.py \
  || { echo 'no se pudo descargar kaggle_run.py'; exit 1; }
" || die "no se pudo instalar el lanzador"

cat <<LISTO

============================================================
 Listo. A partir de ahora, para lanzar la medida:

    bash termux_run.sh

============================================================
LISTO
