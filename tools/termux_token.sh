#!/data/data/com.termux/files/usr/bin/bash
# Crea kaggle.json a partir del token que ya tienes guardado.
#
# Sirve para cuando NO se descargo el fichero pero si se copio la clave (por
# ejemplo, a una nota). Se pide el usuario y la clave, se escribe el fichero
# con el formato correcto y se deja dentro de Debian, listo para usar.
#
# Uso:  bash termux_token.sh
set -u

say() { printf '\n\033[1m==> %s\033[0m\n' "$*"; }
die() { printf '\n\033[31mERROR: %s\033[0m\n' "$*" >&2; exit 1; }

TOKEN="${HOME}/kaggle.json"

command -v proot-distro >/dev/null || die "falta proot-distro. Ejecuta antes: bash setup.sh"

cat <<'EXPLICA'

Vamos a crear el fichero de credenciales a mano, con lo que ya tienes.

Necesitas dos datos (los dos estan en la nota que guardaste):

  - usuario de Kaggle  (el nombre de tu cuenta, no el correo)
  - clave              (un texto largo de letras y numeros)

Cuando el script pida la clave, puedes PEGARLA: manten pulsada la pantalla
en Termux y elige "Pegar".

EXPLICA

# ---------------------------------------------------------------- usuario
KU=""
while [ -z "$KU" ]; do
    printf 'Usuario de Kaggle: '
    read -r KU
    KU=$(printf '%s' "$KU" | tr -d ' \t\r')
done

# ---------------------------------------------------------------- clave
KK=""
while [ -z "$KK" ]; do
    printf 'Clave (pega aqui y pulsa Enter): '
    read -r KK
    KK=$(printf '%s' "$KK" | tr -d ' \t\r')
done

# Si el usuario pego el token completo con formato JSON, se extrae.
case "$KK" in
    *'{'*)
        KK=$(printf '%s' "$KK" | tr -d '\n' | sed -n 's/.*"key"[[:space:]]*:[[:space:]]*"\([^"]*\)".*/\1/p')
        [ -n "$KK" ] || die "parecia un JSON pero no encontre la clave dentro"
        ;;
esac

# ---------------------------------------------------------------- validar
if [ "${#KK}" -lt 16 ]; then
    die "la clave es demasiado corta (${#KK} caracteres). Revisa la nota."
fi
case "$KK" in
    *[!A-Za-z0-9+/=_-]*) die "la clave tiene caracteres raros. Copiala otra vez." ;;
esac

# ---------------------------------------------------------------- escribir
say "Escribiendo el fichero de credenciales"
umask 077
printf '{"username":"%s","key":"%s"}\n' "$KU" "$KK" > "$TOKEN" || die "no pude escribir $TOKEN"
chmod 600 "$TOKEN"
echo "     $TOKEN"

# Comprobar que es JSON valido antes de seguir.
if command -v python3 >/dev/null 2>&1; then
    python3 - "$TOKEN" "$KU" <<'PY' || die "el fichero generado no es valido"
import json, sys
d = json.load(open(sys.argv[1]))
assert d["username"] == sys.argv[2], "el usuario no coincide"
assert len(d["key"]) >= 16, "clave corta"
print("     JSON valido, usuario:", d["username"])
PY
fi

# ---------------------------------------------------------------- copiar a Debian
say "Copiando dentro de Debian"
# Se copia DIRECTAMENTE al sistema de ficheros de Debian. Pasar el contenido
# por tuberia a traves de `proot-distro login` es fragil (proot no siempre
# reenvia la entrada estandar bien); una copia de fichero no falla.
ROOTFS="${PREFIX:-/data/data/com.termux/files/usr}/var/lib/proot-distro/installed-rootfs/debian"

COPIADO=0
if [ -d "$ROOTFS/root" ]; then
    mkdir -p "$ROOTFS/root/.kaggle" 2>/dev/null || true
    if cp "$TOKEN" "$ROOTFS/root/.kaggle/kaggle.json" 2>/dev/null; then
        chmod 600 "$ROOTFS/root/.kaggle/kaggle.json"
        COPIADO=1
        echo "     copiado a $ROOTFS/root/.kaggle/"
    fi
fi

if [ "$COPIADO" -eq 0 ]; then
    echo "     copia directa no disponible, probando por proot..."
    proot-distro login debian -- bash -c "mkdir -p /root/.kaggle" \
        || die "no pude preparar /root/.kaggle dentro de Debian"
    cp "$TOKEN" "$ROOTFS/root/.kaggle/kaggle.json" 2>/dev/null \
        || proot-distro login debian -- cp "/root/kaggle_import.json" "/root/.kaggle/kaggle.json" 2>/dev/null \
        || true
fi

# Comprobacion final sobre el fichero REAL, sin depender de que proot
# responda: si el fichero tiene contenido en su ruta, esta ahi.
DESTINO="$ROOTFS/root/.kaggle/kaggle.json"
if [ ! -s "$DESTINO" ]; then
    die "el fichero no llego a Debian.
Prueba a mano:
    mkdir -p \$PREFIX/var/lib/proot-distro/installed-rootfs/debian/root/.kaggle
    cp $TOKEN \$PREFIX/var/lib/proot-distro/installed-rootfs/debian/root/.kaggle/kaggle.json"
fi
echo "     verificado: $DESTINO"

# Y si proot responde, comprobarlo tambien desde dentro (aviso, no error).
if ! proot-distro login debian -- test -s /root/.kaggle/kaggle.json 2>/dev/null; then
    echo "     (aviso: proot no confirmo la lectura; el fichero esta en su ruta)"
fi

cat <<LISTO

Listo. Ya hay credenciales dentro de Debian.

Ahora lanza la medida con:

    bash run.sh

LISTO
