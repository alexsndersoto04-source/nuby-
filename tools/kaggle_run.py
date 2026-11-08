#!/usr/bin/env python3
"""Lanza el banco de medida en Kaggle desde el telefono, sin copiar y pegar.

Se ejecuta DENTRO de la Debian, via proot-distro. Hace todo:

  1. Toma el codigo del propio repositorio y lo empaqueta como kernel de Kaggle.
  2. Lo sube y lo pone a correr.
  3. Espera, o vuelve enseguida si se le pide no esperar.
  4. Al terminar, descarga el registro y saca la tabla del resumen.

Si el usuario de Kaggle tiene que aceptar reglas o verificar el telefono, se
avisa con el enlace exacto en vez de fallar sin explicacion.

Uso:
    python3 kaggle_run.py            lanzar y esperar
    python3 kaggle_run.py --no-wait  solo lanzar
    python3 kaggle_run.py --fetch    recoger el resultado de la ultima corrida
"""

import argparse
import json
import os
import re
import subprocess
import sys
import time
import urllib.error
import urllib.request

REPO = os.environ.get("CORTEX_REPO", "alexsndersoto04-source/nuby-")
BRANCH = os.environ.get("CORTEX_BRANCH", "arena/886cbd4f-nuby")
SLUG_TITULO = os.environ.get("CORTEX_SLUG", "cortex-lm-banco-texto-real")
TIMEOUT_MIN = int(os.environ.get("CORTEX_TIMEOUT_MIN", "90"))

SEP = "=" * 62


def say(msg):
    print(f"\n==> {msg}", flush=True)


def _rutas_credenciales():
    home = os.path.expanduser("~")
    return [os.path.join(home, ".kaggle", "access_token"),
            os.path.join(home, ".kaggle", "kaggle.json")]


def hay_credenciales():
    """True si hay credenciales en cualquiera de los dos formatos."""
    if os.environ.get("KAGGLE_API_TOKEN"):
        return True
    for r in _rutas_credenciales():
        try:
            if os.path.getsize(r) > 10:
                return True
        except OSError:
            continue
    return False


def preparar_auth():
    """Deja las credenciales como las espera la version instalada.

    Kaggle CLI 2.2.5 cambio el sistema: ya no usa kaggle.json, sino un unico
    token (KGAT_...) en ~/.kaggle/access_token o en KAGGLE_API_TOKEN. Aqui se
    acepta cualquiera de las dos formas y se exporta la variable de entorno,
    que es la via mas fiable y no depende de que el CLI mire el fichero.
    """
    home = os.path.expanduser("~")
    token = None

    # 1) formato nuevo
    try:
        with open(os.path.join(home, ".kaggle", "access_token"), encoding="utf-8") as f:
            token = f.read().strip() or None
    except OSError:
        pass

    # 2) formato clasico: solo vale si el CLI es antiguo, pero se conserva
    usuario = None
    try:
        with open(os.path.join(home, ".kaggle", "kaggle.json"), encoding="utf-8") as f:
            d = json.load(f)
        usuario = (d.get("username") or "").strip()
        if not token:
            k = (d.get("key") or "").strip()
            if k.startswith("KGAT_"):
                token = k
    except Exception:
        pass

    if token:
        os.environ["KAGGLE_API_TOKEN"] = token

    if not token and not usuario:
        raise SystemExit(
            "No encuentro credenciales de Kaggle en ningun formato.\n"
            "Crealas con:\n    bash token.sh")

    # Kaggle usa identificadores en minusculas para los slugs, aunque el perfil
    # se muestre con mayusculas. Con mayusculas la subida falla sin explicar.
    if usuario:
        return usuario.lower()

    # Sin usuario en el fichero: preguntarselo a Kaggle.
    rc, out = kaggle(["config", "view"], check=False)
    m = re.search(r"username[^:]*[:=]\s*([\w.-]+)", out, re.I)
    if m:
        return m.group(1).lower()
    raise SystemExit(
        "Tengo el token pero no se tu nombre de usuario de Kaggle.\n"
        "Anadelo al fichero ~/.kaggle/kaggle.json como {\"username\":\"tu_nombre\"}")


def user_de_kaggle():
    return preparar_auth()


def kaggle(args, check=False):
    cmd = ["/root/kv/bin/kaggle"] + args if os.path.exists("/root/kv/bin/kaggle") else ["kaggle"] + args
    r = subprocess.run(cmd, capture_output=True, text=True)
    out = (r.stdout or "") + (r.stderr or "")
    if check and r.returncode != 0:
        raise SystemExit(f"kaggle {' '.join(args)} fallo:\n{out}")
    return r.returncode, out


# ---------------------------------------------------------------- codigo
def descargar(remoto, destino, local_base=None):
    """Baja un fichero del repo. `local_base` solo se usa en pruebas."""
    if local_base:
        origen = os.path.join(local_base, remoto)
        os.makedirs(os.path.dirname(destino) or ".", exist_ok=True)
        with open(origen, "rb") as f, open(destino, "wb") as g:
            g.write(f.read())
        return
    url = f"https://raw.githubusercontent.com/{REPO}/{BRANCH}/{remoto}"
    req = urllib.request.Request(url, headers={"User-Agent": "cortex-lm/1.0"})
    with urllib.request.urlopen(req, timeout=90) as f:
        datos = f.read()
    os.makedirs(os.path.dirname(destino) or ".", exist_ok=True)
    with open(destino, "wb") as f:
        f.write(datos)


# Ficheros del motor que viajan dentro del cuaderno.
MOTOR = [
    "cortex-lm/Makefile",
    "cortex-lm/engine/cortex_lm.cpp",
    "cortex-lm/engine/cortex_lm.hpp",
    "cortex-lm/engine/cortex_lm_main.cpp",
    "cortex-lm/tools/bench_cortical.cpp",
    "cortex-lm/tools/cerebro.cpp",
    "cortex-lm/tools/generar_alineacion.py",
]


def descargar_texto(remoto, local_base=None):
    """Baja un fichero del repo y lo devuelve como texto."""
    import tempfile
    with tempfile.TemporaryDirectory() as td:
        destino = os.path.join(td, os.path.basename(remoto))
        descargar(remoto, destino, local_base)
        with open(destino, encoding="utf-8", errors="replace") as f:
            return f.read()


def empaquetar_motor(motor, cuaderno):
    """Devuelve el cuaderno con el motor metido como cadena comprimida."""
    import base64
    import zlib

    crudo = json.dumps(motor, ensure_ascii=False).encode("utf-8")
    b64 = base64.b64encode(zlib.compress(crudo, 9)).decode("ascii")
    trozos = [b64[i:i + 76] for i in range(0, len(b64), 76)]
    bloque = "\n".join(f'    "{t}"' for t in trozos)

    cabecera = (
        "# ---------------------------------------------------------------------------\n"
        "# MOTOR PEGADO.\n"
        "# Va dentro del cuaderno, no en el dataset: asi el kernel no depende de que\n"
        "# el dataset adjunto este al dia. Se genero automaticamente desde el\n"
        "# repositorio; no editar a mano.\n"
        "# ---------------------------------------------------------------------------\n"
        "import base64 as _b64\n"
        "import json as _json\n"
        "import zlib as _zlib\n"
        "\n"
        "_MOTOR_B64 = (\n"
        f"{bloque}\n"
        ")\n"
        "\n"
        "\n"
        "def motor_pegado():\n"
        "    return _json.loads(_zlib.decompress(_b64.b64decode(_MOTOR_B64))\n"
        "                       .decode('utf-8'))\n"
        "\n"
        "\n"
    )
    return cabecera + cuaderno


def preparar_carpeta(user, dataset=None):
    d = os.path.expanduser("~/cortex_kaggle")
    os.makedirs(d, exist_ok=True)

    say("Preparando el paquete del kernel")
    # El CUADERNO viaja con su codigo pegado. Asi el kernel no depende ni de
    # internet ni de que el dataset este al dia: cuando el dataset era antiguo,
    # el kernel se paraba por un fichero que faltaba y habia que regenerarlo a
    # mano. Empaquetando el motor como texto, ese paso desaparece.
    nb = descargar_texto("kaggle/run_cortex_lm.py",
                         os.environ.get("CORTEX_LOCAL_BASE"))
    motor = {}
    for ruta in MOTOR:
        try:
            motor[ruta] = descargar_texto(ruta, os.environ.get("CORTEX_LOCAL_BASE"))
        except Exception as e:  # noqa: BLE001
            raise SystemExit(f"No pude bajar {ruta} del repositorio ({e}).")
    pegado = empaquetar_motor(motor, nb)
    with open(os.path.join(d, "run_cortex_lm.py"), "w", encoding="utf-8") as f:
        f.write(pegado)
    print(f"     cuaderno + motor pegado ({len(motor)} ficheros, "
          f"{len(pegado) // 1024} KB)")

    meta = {
        "id": f"{user}/{SLUG_TITULO}",
        "title": "CORTEX-LM banco texto real",
        "code_file": "run_cortex_lm.py",
        "language": "python",
        "kernel_type": "script",
        "is_private": True,
        "enable_gpu": False,
        # Sin internet si el dataset trae todo: asi el kernel funciona aunque
        # Kaggle niegue la red a la cuenta.
        "enable_internet": dataset is None,
        "dataset_sources": [dataset] if dataset else [],
        "competition_sources": [],
        "kernel_sources": [],
        "model_sources": [],
    }
    with open(os.path.join(d, "kernel-metadata.json"), "w", encoding="utf-8") as f:
        json.dump(meta, f, indent=2)
    return d, meta["id"]


# ---------------------------------------------------------------- lanzar
def interpretar_error(salida, user):
    """Convierte los fallos tipicos de Kaggle en instrucciones claras."""
    bajo = salida.lower()

    # Verificacion de telefono PRIMERO: un 403 generico la tapaba y el mensaje
    # mandaba al usuario a un enlace inutil.
    m2 = re.search(r"(https?://\S*?verify\S*)", salida)
    if "verify" in bajo or "phone" in bajo or m2:
        enlace = m2.group(1) if m2 else "https://www.kaggle.com/settings"
        return (f"Hay que verificar el telefono en Kaggle (una sola vez):\n\n"
                f"    {enlace}\n\n"
                f"Despues vuelve a ejecutar:  bash termux_run.sh\n")

    m = re.search(r"(https?://\S*?(?:rules|accept)\S*)", salida)
    if "403" in salida or "forbidden" in bajo or "must accept" in bajo or "rules" in bajo:
        enlace = m.group(1) if m else f"https://www.kaggle.com/{user}"
        return (f"Kaggle te pide aceptar las reglas de la cuenta. Abre esto en\n"
                f"el navegador, aceptalo y vuelve a ejecutar:\n\n    {enlace}\n")

    if "401" in salida or "unauthorized" in bajo or "credentials" in bajo:
        return ("Las credenciales de Kaggle no valen. Vuelve a bajarlas de\n"
                "https://www.kaggle.com/settings y repite bash termux_setup.sh")

    if "internet" in bajo:
        return ("Kaggle necesita el interruptor 'Internet' activado. El lanzador\n"
                "ya lo pide; si persiste, activalo a mano en la web del kernel.")

    return None


def lanzar(user, dataset=None):
    d, kid = preparar_carpeta(user, dataset)
    say(f"Subiendo a Kaggle como {kid}")
    rc, out = kaggle(["kernels", "push", "-p", d], check=False)
    print(out.strip()[-2500:])
    if rc != 0:
        pista = interpretar_error(out, user)
        raise SystemExit("\n" + (pista or "Kaggle rechazo la subida."))
    return kid


def esperar(kid):
    say(f"Esperando a que Kaggle lo ejecute (maximo {TIMEOUT_MIN} minutos)")
    print("    Kaggle no deja ver la salida del kernel en vivo; si se ve el")
    print("    parte de avance que el cuaderno va dejando escrito.")
    t0 = time.time()
    visto_running = False
    ultimo_parte_min = -1
    while True:
        rc, out = kaggle(["kernels", "status", kid], check=False)
        estado = out.strip().splitlines()[-1] if out.strip() else "?"
        mins = (time.time() - t0) / 60
        print(f"    [{mins:5.1f} min] {estado}", flush=True)
        bajo = estado.lower()
        if "complete" in bajo or "succeeded" in bajo:
            return True
        if "error" in bajo or "failed" in bajo or "cancel" in bajo:
            print("\nKaggle reporta un fallo en la ejecucion.")
            pista = interpretar_error(out, kid.split("/")[0])
            if pista:
                print(pista)
            # Sin esto el usuario solo sabe QUE fallo, nunca POR QUE. El
            # registro del kernel es donde esta la causa concreta.
            mostrar_registro(kid, lineas=60)
            return False
        if "running" in bajo:
            visto_running = True
            # Cada 5 minutos se intenta enseñar el avance. Kaggle no publica la
            # salida hasta el final, asi que muchas veces no habra nada; por eso
            # no se avisa de nada si viene vacio.
            if int(mins) != ultimo_parte_min:
                ultimo_parte_min = int(mins)
                avance = leer_parte(kid)
                if avance:
                    print("     por donde va:")
                    for ln in avance[-4:]:
                        print("       " + ln[:160])
                else:
                    print("     (todavia sin parte de avance de ESTA corrida)")
        if mins > TIMEOUT_MIN:
            print(f"\nSe acabo el tiempo de espera ({TIMEOUT_MIN} min).")
            print("No pasa nada: la corrida sigue en Kaggle. Para recogerla:")
            print("    python3 /root/kaggle_run.py --fetch")
            return None
        time.sleep(30)




# --------------------------------------------------------------------------
#  Avance en vivo
# --------------------------------------------------------------------------
# Kaggle NO publica, mientras el kernel corre, lo que este escribe en su salida
# estandar. Lo unico que publica en vivo son los ficheros de /kaggle/working.
# Por eso el cuaderno deja ahi "avance.txt" con una linea por etapa y aqui se
# lee ese fichero. Antes esta funcion leia el registro completo y, si habia
# restos de una corrida anterior, los ensenaba como si fueran el avance de
# ahora: eso fue lo que le mostro al usuario un trozo de novela.
PARTE_NOMBRE = "avance.txt"


def _marcas(posibles):
    """Se queda con las lineas que parecen un parte de avance del cuaderno."""
    buenas = []
    for ln in posibles:
        ln = ln.strip().strip(",").strip('"').strip()
        if not ln:
            continue
        if ln.startswith(("[", ">>>", "==>")) or "bpc" in ln \
                or "caracteres" in ln or "banco" in ln:
            buenas.append(ln[:200])
    return buenas


def leer_parte(kid):
    """Baja /kaggle/working/avance.txt del kernel en marcha. None si no hay."""
    import shutil
    d = os.path.expanduser("~/cortex_kaggle/log")
    os.makedirs(d, exist_ok=True)
    # Limpiar el parte anterior: si no, se ensena el de la corrida pasada.
    for root, _dirs, files in os.walk(d):
        for n in files:
            if n.startswith(PARTE_NOMBRE):
                try:
                    os.remove(os.path.join(root, n))
                except OSError:
                    pass
    kaggle(["kernels", "output", kid, "-p", d, "-f", PARTE_NOMBRE], check=False)
    texto = ""
    for root, _dirs, files in os.walk(d):
        for n in files:
            if not n.startswith(PARTE_NOMBRE):
                continue
            p = os.path.join(root, n)
            try:
                with open(p, encoding="utf-8", errors="ignore") as f:
                    c = f.read()
            except OSError:
                continue
            if len(c) > len(texto):
                texto = c
    if not texto.strip():
        return None
    # Puede llegar tal cual (una linea por marca) o envuelto por Kaggle en la
    # lista JSON de objetos {"data": ...}. Ojo: una marca empieza por "[" y NO
    # es JSON, por eso se mira si detras viene una llave.
    pelado = texto.lstrip()
    es_json = pelado.startswith("[{") or pelado.startswith('{"') \
        or pelado.startswith("[\n")
    crudo = cocinar(texto) if es_json else texto.splitlines()
    return _marcas(crudo) or None


def lineas_del_registro(kid, solo_ultimas=40):
    """Devuelve las lineas del registro ya ejecutado, o None si no se puede.

    Kaggle solo publica la salida al terminar, asi que durante la corrida esto
    devuelve lo que haya (normalmente nada). Se usa solo para dar compania al
    usuario, nunca para decidir nada.
    """
    d = os.path.expanduser("~/cortex_kaggle/log")
    os.makedirs(d, exist_ok=True)
    rc, _out = kaggle(["kernels", "output", kid, "-p", d], check=False)
    if rc != 0:
        return None
    texto = ""
    for root, _dirs, files in os.walk(d):
        for n in files:
            if not n.endswith(".log"):
                continue
            p = os.path.join(root, n)
            try:
                with open(p, encoding="utf-8", errors="ignore") as f:
                    c = f.read()
            except Exception:  # noqa: BLE001
                continue
            # el registro de verdad es el que tiene las marcas del cuaderno
            if "CORTEX-LM" in c or "bpc" in c:
                if len(c) > len(texto):
                    texto = c
    if not texto:
        return None
    return texto.strip().splitlines()[-solo_ultimas:]


def cocinar(texto):
    """Convierte el registro nuevo de Kaggle en texto legible.

    Kaggle 2.x devuelve una lista de objetos {"stream_name","time","data"},
    pero con los objetos partidos en VARIAS lineas (cada salto de linea dentro
    de "data" rompe la linea fisica). Por eso no vale procesar linea a linea:
    hay que recorrer el texto caracter a caracter y quedarse con cada objeto
    completo.
    """
    import json
    salida = []
    i = 0
    n = len(texto)
    while i < n:
        j = texto.find("{", i)
        if j < 0:
            resto = texto[i:].strip().strip("[],").strip()
            if resto:
                salida.append(resto)
            break
        # buscar el cierre del objeto contando llaves
        prof = 0
        k = j
        dentro_cadena = False
        escape = False
        while k < n:
            ch = texto[k]
            if dentro_cadena:
                if escape:
                    escape = False
                elif ch == "\\":
                    escape = True
                elif ch == '"':
                    dentro_cadena = False
            else:
                if ch == '"':
                    dentro_cadena = True
                elif ch == "{":
                    prof += 1
                elif ch == "}":
                    prof -= 1
                    if prof == 0:
                        k += 1
                        break
            k += 1
        trozo = texto[j:k]
        i = k
        try:
            o = json.loads(trozo)
        except Exception:  # noqa: BLE001
            continue
        d = o.get("data") or ""
        t = o.get("time")
        for sub in d.splitlines():
            if not sub.strip():
                continue
            salida.append(f"[{t:7.1f}s] {sub}" if isinstance(t, (int, float)) else sub)
    return salida


def mostrar_registro(kid, lineas=60):
    """Baja el registro del kernel y muestra el final, que es donde esta el error."""
    import tempfile
    d = os.path.expanduser("~/cortex_kaggle/log")
    os.makedirs(d, exist_ok=True)
    say("Descargando el registro para ver la causa")
    rc, out = kaggle(["kernels", "output", kid, "-p", d], check=False)
    if rc != 0:
        print("  No pude descargar el registro. Miralo en:")
        print(f"    https://www.kaggle.com/code/{kid}/log")
        return None

    # La descarga incluye TODO lo que produjo el kernel: el corpus, el codigo,
    # los .html de Kaggle y tambien los .log. Quedarse con el fichero mas
    # grande devuelve el corpus (4 MB de novela), no el registro: eso es lo que
    # le paso al usuario, que recibio un parrafo del Quijote como "error".
    # El registro se identifica por su extension y por su contenido.
    candidatos = []
    for root, _dirs, files in os.walk(d):
        for n in files:
            p = os.path.join(root, n)
            try:
                tam = os.path.getsize(p)
            except OSError:
                continue
            if n.endswith(".log"):
                candidatos.append((0, tam, p))          # maxima prioridad
            elif n.endswith(".json"):
                candidatos.append((1, tam, p))          # formato nuevo anidado
            elif n.endswith((".txt", ".md")):
                candidatos.append((2, tam, p))          # ultimo recurso

    texto = ""
    if candidatos:
        candidatos.sort()
        for _prio, _tam, p in candidatos:
            try:
                with open(p, encoding="utf-8", errors="ignore") as f:
                    c = f.read()
            except Exception:  # noqa: BLE001
                continue
            # Descartar lo que claramente no es un registro.
            if "stream_name" in c or "CORTEX-LM" in c or "Traceback" in c \
                    or "Error" in c:
                texto = c
                break

    if not texto:
        print("  No encontre el registro en la descarga. Miralo en:")
        print(f"    https://www.kaggle.com/code/{kid}/log")
        print("  Ficheros descargados:")
        for root, _dirs, files in os.walk(d):
            for n in sorted(files)[:12]:
                print(f"    {n} ({os.path.getsize(os.path.join(root, n)):,} bytes)")
        return None

    crudo = texto.strip().splitlines()[-lineas:]
    cola = cocinar("\n".join(crudo))
    print("\n" + "-" * 62)
    for ln in cola:
        print("  " + ln[:200])
    print("-" * 62)

    # Diagnostico automatico de los fallos ya conocidos.
    bajo = texto.lower()
    if "corpus repetido" in bajo or "corpus no apto" in bajo:
        print("\nCausa: el texto descargado no era apto (repetido o sin variedad).")
    elif "no se pudo bajar texto" in bajo:
        print("\nCausa: el kernel no tenia salida a internet.")
        print("Activa 'Internet' en Settings del kernel y vuelve a lanzar.")
    elif "no such file" in bajo or "not found" in bajo:
        print("\nCausa: falta un fichero del repositorio o un comando en el kernel.")
    elif "memoryerror" in bajo or "killed" in bajo:
        print("\nCausa: se quedo sin memoria. Baja CORTEX_TRAIN_CHARS.")
    elif "auditor" in bajo and "fall" in bajo:
        print("\nCausa: la auditoria del motor no paso. Es un fallo del motor.")
    return texto


def recoger(kid):
    say("Descargando el resultado")
    d = os.path.expanduser("~/cortex_kaggle/out")
    os.makedirs(d, exist_ok=True)
    rc, out = kaggle(["kernels", "output", kid, "-p", d], check=False)
    if rc != 0:
        pista = interpretar_error(out, kid.split("/")[0])
        raise SystemExit(out.strip()[-1500:] + "\n" + (pista or ""))

    texto = ""
    for root, _dirs, files in os.walk(d):
        for n in files:
            p = os.path.join(root, n)
            try:
                with open(p, encoding="utf-8", errors="ignore") as f:
                    c = f.read()
            except Exception:
                continue
            if "RESUMEN" in c and len(c) > len(texto):
                texto = c
    if not texto:
        raise SystemExit("Descargue la salida pero no encontre el bloque RESUMEN.")

    i = texto.find("RESUMEN")
    bloque = texto[i:].strip()
    # quedarse con la tabla y las lineas siguientes
    lineas = bloque.splitlines()
    recorte = []
    for ln in lineas:
        recorte.append(ln)
        if len(recorte) > 16:
            break
    while recorte and (not recorte[-1].strip() or set(recorte[-1].strip()) == {"="}):
        recorte.pop()
    tabla = "\n".join(recorte)

    print("\n" + SEP)
    print(tabla)
    print(SEP)
    with open(os.path.expanduser("~/cortex_kaggle/resumen.txt"), "w", encoding="utf-8") as f:
        f.write(tabla + "\n")
    print("\nGuardado tambien en ~/cortex_kaggle/resumen.txt")
    return tabla


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--no-wait", action="store_true", help="lanzar y salir")
    ap.add_argument("--fetch", action="store_true", help="recoger la ultima corrida")
    ap.add_argument("--log", action="store_true",
                    help="mostrar el registro de la ultima corrida y salir")
    ap.add_argument("--estado", action="store_true",
                    help="ver el estado actual del kernel y salir")
    ap.add_argument("--dataset", metavar="USUARIO/SLUG",
                    help="dataset con el texto y el motor; permite kernels sin internet")
    a = ap.parse_args()

    user = user_de_kaggle()
    kid = f"{user}/{SLUG_TITULO}"

    print(SEP)
    print("CORTEX-LM en Kaggle")
    print(SEP)

    if a.estado:
        rc, out = kaggle(["kernels", "status", kid], check=False)
        print(out.strip())
        if "running" in out.lower():
            avance = leer_parte(kid)
            if avance:
                print("\nPor donde va:")
                for ln in avance[-12:]:
                    print("   " + ln[:200])
            print("\nEsta corriendo. Kaggle no publica el registro hasta el final.")
            print("Para verlo en vivo, abre en el navegador:")
            print(f"  https://www.kaggle.com/code/{kid}/log")
        return 0

    if a.log:
        _rc, est = kaggle(["kernels", "status", kid], check=False)
        if "running" in est.lower():
            print("OJO: ese kernel esta corriendo AHORA MISMO, asi que lo que")
            print("sigue es el registro de una ejecucion ANTERIOR, no de esta.")
            print("Para ver por donde va la de ahora:  bash run.sh --estado")
            print()
        mostrar_registro(kid, lineas=80)
        return 0

    if a.fetch:
        recoger(kid)
        return 0

    kid = lanzar(user, a.dataset)
    if a.no_wait:
        say("Lanzado. Para recoger el resultado:")
        print("    python3 /root/kaggle_run.py --fetch")
        return 0

    estado = esperar(kid)
    if estado is False:
        return 1
    if estado is None:
        # Se agoto la espera pero la corrida sigue en Kaggle: no se puede
        # recoger todavia, seria un error confuso.
        return 0
    recoger(kid)
    print("\nPega esa tabla en el chat y te digo que significa.")
    return 0


if __name__ == "__main__":
    sys.exit(main())
