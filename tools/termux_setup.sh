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

BRANCH="${CORTEX_BRANCH:-arena/c7deda7f-nuby}"
REPO="${CORTEX_REPO:-alexsndersoto04-source/nuby-}"
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
/root/kv/bin/pip install -q kaggle
/root/kv/bin/kaggle --version
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
if [ -z "$CREDS" ]; then
    CREDS=$(find "$HOME" -maxdepth 6 -name 'kaggle*.json' 2>/dev/null | head -1)
fi

if [ -z "$CREDS" ]; then
    cat <<'AYUDA'

No encontre kaggle.json. Hay que bajarlo (una sola vez):

  1. En el navegador entra en https://www.kaggle.com/settings
  2. Pulsa "Create New Token" (seccion API)
  3. Se descarga kaggle.json

Si no puedes descargarlo, activa primero el acceso a Descargas:
     termux-setup-storage
y acepta el permiso. Despues vuelve a ejecutar este script.

AYUDA
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
