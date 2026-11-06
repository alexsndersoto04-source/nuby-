#!/data/data/com.termux/files/usr/bin/bash
# Instala las credenciales de Kaggle dentro de Debian.
#
# Kaggle CLI 2.2.5 cambio la forma de autenticarse. Ahora hay dos formatos:
#
#   NUEVO: un unico texto que empieza por KGAT_  ->  ~/.kaggle/access_token
#   VIEJO: un JSON {"username":..,"key":..}     ->  ~/.kaggle/kaggle.json
#
# Este instalador acepta cualquiera de los dos y lo coloca donde toca, asi que
# da igual cual tenga el usuario en la nota.
#
# Uso:  bash termux_token.sh
set -u

say() { printf '\n\033[1m==> %s\033[0m\n' "$*"; }
die() { printf '\n\033[31mERROR: %s\033[0m\n' "$*" >&2; exit 1; }

command -v proot-distro >/dev/null || die "falta proot-distro.
Primero:  pkg install -y proot-distro curl"

cat <<'EXPLICA'

Vamos a instalar tus credenciales de Kaggle.

Necesitas UNO de estos dos datos, el que tengas en la nota:

  1) Un texto largo que empieza por  KGAT_     <- el moderno
  2) Un JSON con "username" y "key"            <- el clasico

Se puede pegar cualquiera de los dos y el script decide solo donde va.

Para pegar en Termux: manten pulsada la pantalla y elige "Pegar".

EXPLICA

# ---------------------------------------------------------------- entrada
DATO=""
INTENTOS=0
while [ -z "$DATO" ]; do
    INTENTOS=$((INTENTOS + 1))
    if [ "$INTENTOS" -gt 5 ]; then
        die "no recibi ningun token. Vuelve a ejecutarlo cuando lo tengas a mano."
    fi
    printf 'Pega aqui tu token (o el JSON) y pulsa Enter: '
    # read devuelve fallo al llegar al final de la entrada (Ctrl+D). Sin esta
    # comprobacion el bucle imprimia el mensaje sin parar.
    if ! read -r DATO; then
        printf '\n'
        die "entrada cerrada sin token. Vuelve a ejecutarlo y pega el token."
    fi
    DATO=$(printf '%s' "$DATO" | tr -d '\r')
done

# ---------------------------------------------------------------- clasificar
MODO=""
case "$DATO" in
    KGAT_*|kgat_*)
        MODO="token"
        ;;
    *'{'*'"key"'*)
        MODO="json"
        # extraer usuario y clave del JSON pegado
        KU=$(printf '%s' "$DATO" | tr -d '\n' | sed -n 's/.*"username"[[:space:]]*:[[:space:]]*"\([^"]*\)".*/\1/p')
        KK=$(printf '%s' "$DATO" | tr -d '\n' | sed -n 's/.*"key"[[:space:]]*:[[:space:]]*"\([^"]*\)".*/\1/p')
        [ -n "$KK" ] || die "el JSON no tiene el campo key"
        ;;
    *)
        # Si es un texto largo sin prefijo conocido, se trata como token nuevo.
        if [ "${#DATO}" -ge 24 ]; then
            MODO="token"
        else
            die "eso no parece un token (${#DATO} caracteres).
Copia el texto completo de la nota."
        fi
        ;;
esac

echo "     formato detectado: $MODO"

# ---------------------------------------------------------------- sanidad
case "$MODO" in
    token)
        case "$DATO" in
            *[!A-Za-z0-9_+=/-]*) die "el token tiene caracteres raros. Copialo otra vez." ;;
        esac
        if [ "${#DATO}" -lt 24 ]; then
            die "el token es demasiado corto (${#DATO} caracteres)."
        fi
        ;;
    json)
        [ -n "$KU" ] || die "el JSON pegado no tiene username"
        [ "${#KK}" -ge 16 ] || die "la clave del JSON es demasiado corta."
        ;;
esac

# ---------------------------------------------------------------- escribir
# Se preparan LAS DOS formas. Cuesta nada y evita tener que adivinar que
# version de la herramienta hay instalada.
say "Preparando las credenciales"
TMPDIR_="${PREFIX:-/data/data/com.termux/files/usr}/tmp"
mkdir -p "$TMPDIR_" 2>/dev/null || TMPDIR_="$HOME"
ACCESO="$TMPDIR_/access_token"
JSONF="$TMPDIR_/kaggle.json"

umask 077
if [ "$MODO" = "token" ]; then
    printf '%s\n' "$DATO" > "$ACCESO"
    # Un JSON con solo la clave tampoco estorba si la herramienta es antigua.
    printf '{"username":"","key":"%s"}\n' "$DATO" > "$JSONF"
else
    printf '{"username":"%s","key":"%s"}\n' "$KU" "$KK" > "$JSONF"
    printf '%s\n' "$KK" > "$ACCESO"
fi
chmod 600 "$ACCESO" "$JSONF"

# ---------------------------------------------------------------- copiar
say "Copiando dentro de Debian"

encontrar_rootfs() {
    for base in \
        "${PREFIX:-}/var/lib/proot-distro/installed-rootfs" \
        "/data/data/com.termux/files/usr/var/lib/proot-distro/installed-rootfs" \
        "$HOME/../usr/var/lib/proot-distro/installed-rootfs" \
        "/data/data/com.termux/files/home/.proot-distro/installed-rootfs"
    do
        [ -d "$base/debian" ] && { printf '%s' "$base/debian"; return 0; }
    done
    find "${PREFIX:-/data/data/com.termux/files/usr}" "$HOME" \
         -maxdepth 6 -type d -name debian -path '*installed-rootfs*' 2>/dev/null | head -1
}

ROOTFS=$(encontrar_rootfs)
COPIADO=0
if [ -n "$ROOTFS" ] && [ -d "$ROOTFS/root" ]; then
    echo "     Debian en: $ROOTFS"
    mkdir -p "$ROOTFS/root/.kaggle" 2>/dev/null || true
    if cp "$ACCESO" "$ROOTFS/root/.kaggle/access_token" 2>/dev/null && \
       cp "$JSONF"   "$ROOTFS/root/.kaggle/kaggle.json"  2>/dev/null; then
        chmod 600 "$ROOTFS/root/.kaggle/access_token" "$ROOTFS/root/.kaggle/kaggle.json" 2>/dev/null || true
        COPIADO=1
        echo "     copiado directamente"
    fi
fi

if [ "$COPIADO" -eq 0 ]; then
    echo "     escribiendo a traves de proot..."
    proot-distro login debian -- bash -c '
        mkdir -p /root/.kaggle
        cat > /root/.kaggle/access_token
        chmod 600 /root/.kaggle/access_token
    ' < "$ACCESO" || die "no pude escribir dentro de Debian"
    proot-distro login debian -- bash -c '
        mkdir -p /root/.kaggle
        cat > /root/.kaggle/kaggle.json
        chmod 600 /root/.kaggle/kaggle.json
    ' < "$JSONF" || true
fi

# ---------------------------------------------------------------- verificar
say "Comprobando que Kaggle las acepta"
if proot-distro login debian -- test -s /root/.kaggle/access_token 2>/dev/null; then
    echo "     access_token presente"
elif proot-distro login debian -- test -s /root/.kaggle/kaggle.json 2>/dev/null; then
    echo "     kaggle.json presente"
else
    die "no hay ningun fichero de credenciales dentro de Debian"
fi

# La prueba de verdad: pedir algo a Kaggle. Si contesta, las credenciales
# valen; si no, se avisa aqui y no tres pasos mas adelante.
if proot-distro login debian -- /root/kv/bin/kaggle config view >/dev/null 2>&1; then
    echo "     Kaggle responde"
else
    echo "     (aviso: no pude confirmar con Kaggle; se intentara al lanzar)"
fi

# Limpieza: los temporales tienen la clave en claro.
rm -f "$ACCESO" "$JSONF"

cat <<LISTO

Listo. Lanza la medida con:

    bash run.sh

LISTO
