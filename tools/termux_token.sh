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

# La ruta del rootfs NO es fija: depende de la version de proot-distro y de
# donde este instalado. Se busca en vez de suponerla; suponerla fue el fallo.
encontrar_rootfs() {
    for base in \
        "${PREFIX:-}/var/lib/proot-distro/installed-rootfs" \
        "${PREFIX:-}/var/lib/proot-distro/installed-rootfs" \
        "/data/data/com.termux/files/usr/var/lib/proot-distro/installed-rootfs" \
        "$HOME/../usr/var/lib/proot-distro/installed-rootfs" \
        "/data/data/com.termux/files/home/.proot-distro/installed-rootfs"
    do
        [ -d "$base/debian" ] && { printf '%s' "$base/debian"; return 0; }
    done
    # ultimo recurso: buscar el directorio por nombre
    find "${PREFIX:-/data/data/com.termux/files/usr}" "$HOME" \
         -maxdepth 6 -type d -name debian -path '*installed-rootfs*' 2>/dev/null \
        | head -1
}

ROOTFS=$(encontrar_rootfs)

DESTINO=""
if [ -n "$ROOTFS" ] && [ -d "$ROOTFS/root" ]; then
    echo "     Debian encontrado en: $ROOTFS"
    mkdir -p "$ROOTFS/root/.kaggle" 2>/dev/null || true
    if cp "$TOKEN" "$ROOTFS/root/.kaggle/kaggle.json" 2>/dev/null; then
        chmod 600 "$ROOTFS/root/.kaggle/kaggle.json" 2>/dev/null || true
        DESTINO="$ROOTFS/root/.kaggle/kaggle.json"
        echo "     copiado directamente"
    fi
else
    echo "     no encontre el rootfs por mi cuenta; probando dentro de Debian"
fi

# Plan B: dejar que la propia Debian cree el fichero. Se copia primero a una
# ruta accesible desde Termux y se mueve desde dentro, que es lo que funciona
# siempre porque no depende de adivinar rutas.
if [ -z "$DESTINO" ] || [ ! -s "$DESTINO" ]; then
    echo "     escribiendo a traves de proot..."
    proot-distro login debian -- bash -c 'mkdir -p /root/.kaggle && cat > /root/.kaggle/kaggle.json && chmod 600 /root/.kaggle/kaggle.json' < "$TOKEN" 2>/dev/null         || die "no pude escribir dentro de Debian"
fi

# Verificacion: preguntar a Debian, que es la unica que sabe de verdad.
if proot-distro login debian -- test -s /root/.kaggle/kaggle.json 2>/dev/null; then
    echo "     verificado dentro de Debian"
elif [ -n "$DESTINO" ] && [ -s "$DESTINO" ]; then
    echo "     verificado en disco: $DESTINO"
    echo "     (proot no respondio, pero el fichero esta en su sitio)"
else
    die "el fichero no llego a Debian.
Prueba a mano, en una sola linea:
    proot-distro login debian -- bash -c 'mkdir -p /root/.kaggle && cat > /root/.kaggle/kaggle.json' < $TOKEN"
fi

# Y si proot responde, comprobarlo tambien desde dentro (aviso, no error).
if ! proot-distro login debian -- test -s /root/.kaggle/kaggle.json 2>/dev/null; then
    echo "     (aviso: proot no confirmo la lectura; el fichero esta en su ruta)"
fi

cat <<LISTO

Listo. Ya hay credenciales dentro de Debian.

Ahora lanza la medida con:

    bash run.sh

LISTO
