#!/usr/bin/env python3
"""
CORTEX-LM en Kaggle: banco de medida sobre texto español REAL.

Qué hace
--------
1. Consigue texto español de verdad, con varias fuentes de respaldo.
2. Lo audita antes de usarlo: si detecta que el texto está repetido en bucle
   (el error que invalidó todas las medidas anteriores), se planta y lo dice.
3. Compila el motor C++ del repositorio.
4. Corre la auditoría estructural.
5. Corre los bancos de medida con y sin plasticidad, sobre texto no visto.
6. Imprime una tabla final lista para copiar y pegar.

Cómo lanzarlo: ver kaggle/README.md. No hace falta tocar este fichero.
"""

import os
import re
import shutil
import subprocess
import sys
import urllib.request

REPO_GH = os.environ.get("CORTEX_REPO", "alexsndersoto04-source/nuby-")
BRANCH = os.environ.get("CORTEX_BRANCH", "arena/c7deda7f-nuby")
WORK = os.environ.get("CORTEX_REPO_DIR", "/kaggle/working/nuby-")
# En local (pruebas) se salta la descarga y se usa el corpus de juguete.
LOCAL = os.environ.get("CORTEX_LOCAL", "") == "1"

# Tamano de la corrida. Estas cifras NO son un capricho: estan medidas en
# local con el motor real, porque la corrida anterior (60.000 caracteres y 8
# epocas) se paso de los 90 minutos y Kaggle ni siquiera llego a publicar el
# registro. Medido con un solo nucleo lento (parecido al de Kaggle):
# Medido en local: 20 corridas con 4.000/2.000 tardan 2 min. Con 20.000/5.000
# y un nucleo lento son unos 25-30 min: cabe de sobra en los 90 de Kaggle.
TRAIN_CHARS = int(os.environ.get("CORTEX_TRAIN_CHARS", "20000"))
HOLDOUT_CHARS = int(os.environ.get("CORTEX_HOLDOUT_CHARS", "5000"))
# El lector RLS converge en UNA pasada, asi que ya no hay "epocas" que elegir.
# Lo que manda es el tamano del texto: 20 corridas (4 condiciones x 5 semillas)
# tienen que caber en el tiempo de Kaggle.

SEP = "=" * 70

# Kaggle no publica la salida estandar del kernel mientras corre: solo publica
# los ficheros que van apareciendo en /kaggle/working. Para que el usuario vea
# por donde va, cada etapa apunta su marca en este fichero, que el lanzador lee
# desde el telefono. Si falla al escribirse, se sigue: nunca puede tumbar la
# corrida.
PARTE = "/kaggle/working/avance.txt" if os.path.isdir("/kaggle/working") \
    else os.path.join(os.path.dirname(os.path.abspath(__file__)), "avance.txt")


def parte(msg):
    """Apunta una linea de avance (hora + mensaje) y la publica."""
    import time as _t
    linea = f"[{_t.strftime('%H:%M:%S')}] {msg}"
    print(f">>> {linea}", flush=True)
    try:
        with open(PARTE, "a", encoding="utf-8") as f:
            f.write(linea + "\n")
            f.flush()
            os.fsync(f.fileno())
    except Exception:  # noqa: BLE001
        pass


def sh(cmd, cwd=None, check=True, env=None):
    """Ejecuta un comando y devuelve su salida."""
    print(f"$ {' '.join(cmd) if isinstance(cmd, list) else cmd}")
    r = subprocess.run(cmd, cwd=cwd, shell=isinstance(cmd, str),
                       capture_output=True, text=True, env=env)
    out = (r.stdout or "") + (r.stderr or "")
    print(out.rstrip()[-4000:])
    if check and r.returncode != 0:
        raise SystemExit(f"falló: {cmd}")
    return out


# --------------------------------------------------------------------------
# 1. Corpus
# --------------------------------------------------------------------------
def try_download(url, timeout=60):
    try:
        req = urllib.request.Request(url, headers={"User-Agent": "cortex-lm/1.0"})
        with urllib.request.urlopen(req, timeout=timeout) as f:
            raw = f.read()
        return raw.decode("utf-8", errors="ignore")
    except Exception as e:  # noqa: BLE001
        print(f"    no: {url} ({type(e).__name__}: {e})")
        return None


def _alcanzable(host, timeout=6):
    """True si el dominio resuelve y acepta conexion. Rapido y sin reintentos."""
    import socket
    try:
        socket.setdefaulttimeout(timeout)
        socket.getaddrinfo(host, 443)
        return True
    except Exception:  # noqa: BLE001
        return False
    finally:
        socket.setdefaulttimeout(None)


def comprobar_internet():
    """Falla rapido si el kernel no tiene salida a red.

    Kaggle desactiva internet en silencio si la cuenta no ha verificado el
    telefono: se pide en el kernel, se ignora, y el cuaderno se pasa ocho
    minutos reintentando dominios que no resuelven. Mejor decirlo en dos
    segundos y con el enlace que lo arregla.
    """
    if LOCAL:
        return
    # Un dataset adjunto no necesita red, asi que si hay uno, no hace falta.
    if os.path.isdir("/kaggle/input"):
        for root, _d, files in os.walk("/kaggle/input"):
            for name in files:
                if name.endswith((".txt", ".csv", ".tsv")):
                    if os.path.getsize(os.path.join(root, name)) > 200_000:
                        print("hay un dataset adjunto: no hace falta internet")
                        return

    if _alcanzable("raw.githubusercontent.com") or _alcanzable("www.gutenberg.org"):
        return

    raise SystemExit(
        "\n" + "=" * 62 + "\n"
        "ESTE KERNEL NO TIENE SALIDA A INTERNET.\n\n"
        "Por eso fallan todos los dominios (Errno -3). Kaggle desactiva\n"
        "internet aunque se le pida, si la cuenta no ha verificado el\n"
        "telefono.\n\n"
        "Solucion (una sola vez, 2 minutos):\n"
        "  1. Abre en el navegador:  https://www.kaggle.com/settings\n"
        "  2. Busca 'Phone verification' y verifica tu numero.\n"
        "  3. Vuelve a lanzar:  bash run.sh\n\n"
        "Alternativa sin verificar el telefono: sube un .txt grande en\n"
        "espanol como dataset y adjuntalo al kernel.\n"
        + "=" * 62)


def get_corpus():
    """Devuelve el texto completo, de la primera fuente que funcione."""
    if LOCAL:
        print("MODO LOCAL: usando el corpus de juguete del repositorio.")
        proj = os.path.join(WORK, "cortex-lm")
        # Se REGENERA antes de leer: el propio script escribe luego en data/,
        # y leer el corpus que uno mismo sobrescribe deja el fichero vacío en
        # la segunda pasada.
        sh(["python3", "tools/make_test_corpus.py"], cwd=proj, check=False)
        with open(os.path.join(proj, "data", "spanish_small.txt"), encoding="utf-8") as f:
            return f.read()

    print("\n--- buscando texto español real ---")

    # Fuente 1: dataset adjunto. Es la unica que no necesita red.
    ruta_txt, _ = buscar_en_dataset()
    if ruta_txt:
        with open(ruta_txt, encoding="utf-8", errors="ignore") as f:
            return f.read()

    # Fuente 2: Hugging Face, Wikipedia en español (streaming, solo una parte).
    # Antes de intentarlo se prueba si el dominio resuelve. Sin esta prueba, el
    # cliente reintenta cinco veces con espera creciente y se pierden cinco
    # minutos en algo que se sabe fallido desde el primer segundo: en Kaggle
    # huggingface.co da Errno -3 (fallo de resolucion de nombres).
    if _alcanzable("huggingface.co"):
        try:
            from datasets import load_dataset  # noqa: PLC0415
            print("    probando Hugging Face: wikimedia/wikipedia (es)")
            ds = load_dataset("wikimedia/wikipedia", "20231101.es",
                              split="train[:2000]", streaming=True)
            parts = [row["text"] for row in ds]
            text = "\n\n".join(parts)
            if len(text) > 500_000:
                return text
        except Exception as e:  # noqa: BLE001
            print(f"    no: Hugging Face ({type(e).__name__}: {e})")
    else:
        print("    Hugging Face no resuelve desde aqui, se salta")

    # Fuente 3: Project Gutenberg, obras en español de dominio público.
    # 2000 = Don Quijote. Los demás son novelas españolas del catálogo.
    for book in ("2000", "17073", "36064", "46022"):
        url = f"https://www.gutenberg.org/cache/epub/{book}/pg{book}.txt"
        print(f"    probando Gutenberg {book}")
        t = try_download(url, timeout=90)
        if t and len(t) > 200_000:
            return t

    raise SystemExit(
        "No se pudo bajar texto español. Solución: activa 'Internet' en el "
        "notebook de Kaggle, o adjunta un dataset con un .txt grande en español.")


def strip_gutenberg(text):
    """Quita la cabecera y el pie legales de Gutenberg."""
    start = re.search(r"\*\*\* ?START OF (THE|THIS) PROJECT GUTENBERG.*?\*\*\*", text)
    if start:
        text = text[start.end():]
    end = re.search(r"\*\*\* ?END OF (THE|THIS) PROJECT GUTENBERG", text)
    if end:
        text = text[:end.start()]
    return text


def audit_corpus(text, label):
    """Comprueba que el texto NO está repetido en bucle.

    Esto es lo que faltaba antes: el corpus anterior era el mismo fragmento
    del Quijote 40 veces, así que el 98 % del "test" estaba en el
    entrenamiento y las cifras no valían nada. Aquí se mide y se avisa.
    """
    n = len(text)
    alphabet = len(set(text))
    print(f"  [{label}] {n:,} caracteres | alfabeto de {alphabet} símbolos")

    ventana = 80
    muestras = [text[i:i + ventana] for i in range(0, max(1, n - ventana), 97)]
    unicas = len(set(muestras))
    ratio = unicas / max(1, len(muestras))
    print(f"  [{label}] ventanas de {ventana} caracteres únicas: "
          f"{unicas:,}/{len(muestras):,} = {100*ratio:.1f} %")

    problemas = []
    if ratio < 0.50:
        problemas.append(f"corpus repetido: solo {100*ratio:.1f} % de ventanas únicas")

    # El alfabeto minimo depende del TAMANO del texto. Una particion de 12.000
    # caracteres de novela tiene unos 60 simbolos distintos: es normal, no es
    # sospechoso. Exigir 60 a cualquier trozo abortaba corridas buenas, como la
    # del 4 de noviembre, donde el corpus completo tenia 121.
    # Se calcula lo esperable: cuanto mas texto, mas simbolos, con saturacion.
    # Calibrado con el corpus real de cuatro novelas espanolas:
    #   completo 4.487.742 -> 121 simbolos
    #   train      60.000 ->  82
    #   test       12.000 ->  59
    # Una particion de 12.000 caracteres con 59 simbolos es normal. El umbral
    # es deliberadamente bajo: aqui solo se descartan unigramas y texto de
    # juguete; que el texto sea natural lo decide la variedad de ventanas.
    esperado = 25 if n < 20_000 else (35 if n < 100_000 else 45)
    if alphabet < esperado:
        problemas.append(
            f"alfabeto de solo {alphabet} símbolos para {n:,} caracteres "
            f"(esperaba al menos {esperado})")
    if problemas:
        for x in problemas:
            print(f"  *** AVISO: {x}")
        if not LOCAL:
            raise SystemExit(
                "\n*** CORPUS NO APTO: " + "; ".join(problemas) +
                "\n*** Cualquier cifra medida sobre él sería memorización.")
        print("  (modo local: se continúa solo para probar el flujo)")
    print("  -> corpus aceptable: texto variado y no repetido.")
    return ratio


# --------------------------------------------------------------------------
# 2. Repositorio y compilación
# --------------------------------------------------------------------------
# Se escribe tal cual cuando el motor viene de un dataset, para no depender de
# bajar el Makefile.
Makefile_ = """CXX      ?= g++
CXXFLAGS ?= -O2 -std=c++17
INC      := -Iengine
SRC      := engine/cortex_lm.cpp engine/cortex_lm_main.cpp
BIN      := bin/cortex_lm

all: $(BIN)

$(BIN): $(SRC) engine/cortex_lm.hpp
\t@mkdir -p bin
\t$(CXX) $(CXXFLAGS) $(INC) $(SRC) -o $(BIN)

portable: $(BIN)

clean:
\trm -rf bin
"""

FICHEROS = [
    "cortex-lm/Makefile",
    "cortex-lm/engine/cortex_lm.cpp",
    "cortex-lm/engine/cortex_lm.hpp",
    "cortex-lm/engine/cortex_lm_main.cpp",
    "cortex-lm/tools/bench_rls.cpp",
    "cortex-lm/tools/bench_res.cpp",
]


def _descarga(url, destino):
    req = urllib.request.Request(url, headers={"User-Agent": "cortex-lm/1.0"})
    with urllib.request.urlopen(req, timeout=60) as f:
        datos = f.read()
    os.makedirs(os.path.dirname(destino), exist_ok=True)
    with open(destino, "wb") as f:
        f.write(datos)
    return len(datos)


def extraer_zips(base):
    """Descomprime cualquier .zip que haya en un dataset.

    kaggle_dataset.py empaqueta el motor en motor.zip (Kaggle lo pide asi para
    varios ficheros sueltos), pero el cuaderno espera los .cpp a la vista. Sin
    esto el motor "no aparece" y el kernel muere diciendo que el dataset solo
    traia 0 ficheros.
    """
    import zipfile
    extraidos = []
    destino = os.path.join(WORK, "de_dataset")
    for root, _dirs, files in os.walk(base):
        for name in files:
            if not name.endswith(".zip"):
                continue
            origen = os.path.join(root, name)
            try:
                with zipfile.ZipFile(origen) as z:
                    z.extractall(destino)
                extraidos.append(name)
            except Exception as e:  # noqa: BLE001
                print(f"    no pude abrir {name}: {type(e).__name__}")
    if extraidos:
        print(f"    descomprimido: {', '.join(extraidos)}")
    return destino if extraidos else None


def buscar_en_dataset():
    """Busca texto y motor en /kaggle/input (datasets adjuntos).

    Es la unica via que funciona sin internet: los datasets se montan dentro
    del kernel sin necesitar red.
    """
    base = "/kaggle/input"
    if not os.path.isdir(base):
        return None, None

    # Los .zip se descomprimen antes de buscar: el motor viene comprimido.
    extraido = extraer_zips(base)

    raices = [base]
    if extraido:
        raices.append(extraido)

    texto = None
    motor = None
    mejor = 0
    for raiz in raices:
        for root, _dirs, files in os.walk(raiz):
            for name in files:
                p = os.path.join(root, name)
                try:
                    tam = os.path.getsize(p)
                except OSError:
                    continue
                if name.endswith(".txt") and tam > mejor:
                    mejor = tam
                    texto = p
            # el motor son los .cpp/.hpp que vienen dentro de motor.zip
            cpps = [f for f in files if f.endswith((".cpp", ".hpp"))]
            if len(cpps) >= 3 and motor is None:
                motor = root

    if texto:
        print(f"    texto del dataset: {texto} ({os.path.getsize(texto)/1e6:.1f} MB)")
    if motor:
        print(f"    motor del dataset: {motor}")
    return texto, motor


def escribir_motor_pegado():
    """Escribe el motor que viene pegado en el propio cuaderno.

    Devuelve la carpeta del proyecto, o None si el cuaderno no lo trae.
    Esto va ANTES de mirar el dataset: el motor del dataset se queda viejo en
    cuanto se cambia el codigo, y regenerarlo a mano es un paso que se olvida.
    """
    try:
        motor = motor_pegado()          # noqa: F821  (lo inyecta el lanzador)
    except Exception:                                    # noqa: BLE001
        return None
    if not motor:
        return None
    destino = os.path.join(WORK, "cortex-lm")
    os.makedirs(os.path.join(destino, "engine"), exist_ok=True)
    os.makedirs(os.path.join(destino, "tools"), exist_ok=True)
    for ruta, texto in motor.items():
        rel = ruta.split("cortex-lm/", 1)[-1]
        salida = os.path.join(destino, rel)
        os.makedirs(os.path.dirname(salida), exist_ok=True)
        with open(salida, "w", encoding="utf-8") as f:
            f.write(texto)
    with open(os.path.join(destino, "Makefile"), "w", encoding="utf-8") as f:
        f.write(Makefile_)
    print(f"  motor pegado en el cuaderno ({len(motor)} ficheros)")
    return destino


def get_repo():
    """Trae solo los ficheros necesarios, sin git.

    Con `git clone` esto dependia de que hubiera git instalado y de la URL.
    Bajando los ficheros con urllib el cuaderno funciona pegando codigo a mano
    en Kaggle, sin clonar nada.
    """
    # 1) el motor que viaja pegado en este mismo cuaderno: siempre al dia
    pegado = escribir_motor_pegado()
    if pegado:
        parte("motor del cuaderno (no hace falta dataset al dia)")
        return pegado

    if LOCAL:
        print("\n--- MODO LOCAL: usando el codigo ya presente ---")
        return os.path.join(WORK, "cortex-lm")

    # Si hay motor en un dataset adjunto, se usa y no hace falta red.
    _txt, ruta_motor = buscar_en_dataset()
    if ruta_motor:
        destino = os.path.join(WORK, "cortex-lm")
        os.makedirs(os.path.join(destino, "engine"), exist_ok=True)
        os.makedirs(os.path.join(destino, "tools"), exist_ok=True)
        # bench_rls.cpp es el banco bueno (lector RLS y capa con memoria). Si el
        # dataset es antiguo y no lo trae, se usa bench_joint.cpp como sustituto:
        # compila igual pero mide menos cosas. Antes se exigian 5 ficheros
        # EXACTOS y un dataset de 4 tumbaba la corrida entera por eso.
        nombres = {"cortex_lm.cpp": "engine", "cortex_lm.hpp": "engine",
                   "cortex_lm_main.cpp": "engine", "bench_rls.cpp": "tools"}
        copiados = 0
        for nombre, sub in nombres.items():
            origen = os.path.join(ruta_motor, nombre)
            if os.path.exists(origen):
                import shutil
                shutil.copy(origen, os.path.join(destino, sub, nombre))
                copiados += 1
        # sustituto si el dataset es antiguo
        destino_bench = os.path.join(destino, "tools", "bench_rls.cpp")
        if not os.path.exists(destino_bench):
            for alt in ("bench_joint.cpp", "bench.cpp"):
                origen_alt = os.path.join(ruta_motor, alt)
                if os.path.exists(origen_alt):
                    import shutil
                    shutil.copy(origen_alt, destino_bench)
                    print(f"  el dataset no traia bench_rls.cpp: uso {alt}")
                    copiados += 1
                    break
        if copiados >= 4:
            print(f"  motor copiado del dataset ({copiados} ficheros)")
            # el Makefile se genera aqui: no hace falta bajarlo
            with open(os.path.join(destino, "Makefile"), "w", encoding="utf-8") as f:
                f.write(Makefile_)
            return destino
        print(f"  el dataset solo traia {copiados} ficheros de motor, se ignora")

    print("\n--- descargando el motor ---")
    for rama in (BRANCH, "main"):
        fallos = []
        for ruta in FICHEROS:
            url = f"https://raw.githubusercontent.com/{REPO_GH}/{rama}/{ruta}"
            try:
                _descarga(url, os.path.join(WORK, ruta))
            except Exception as e:  # noqa: BLE001
                fallos.append(f"{ruta}: {type(e).__name__}")
        if not fallos:
            print(f"  {len(FICHEROS)} ficheros desde la rama {rama}")
            return os.path.join(WORK, "cortex-lm")
        print(f"  rama {rama}: fallaron {len(fallos)} -> {fallos[:3]}")
    raise SystemExit(
        "No se pudo descargar el codigo del repositorio. Comprueba que "
        "'Internet' esta activado en Settings.")


def build(proj):
    """Compila el motor SIN depender de make.

    La imagen de Kaggle puede no traer make, y no hay internet para instalarlo.
    Llamando a g++ directamente (que si viene) el problema desaparece.
    """
    print("\n--- compilando el motor ---")
    bin_dir = os.path.join(proj, "bin")
    os.makedirs(bin_dir, exist_ok=True)
    salida = os.path.join(bin_dir, "cortex_lm")

    compilador = None
    for cand in ("g++", "c++", "clang++"):
        if shutil.which(cand):
            compilador = cand
            break
    if not compilador:
        raise SystemExit(
            "No hay compilador de C++ en el kernel (ni g++, ni c++, ni clang++).\n"
            "Avisa de esto: significa que hay que compilar en otro sitio.")

    print(f"    compilador: {compilador}")
    parte(f"compilando el motor con {compilador}")
    cmd = [compilador, "-O2", "-std=c++17", "-Iengine",
           "engine/cortex_lm.cpp", "engine/cortex_lm_main.cpp", "-o", salida]
    sh(cmd, cwd=proj)

    if not os.path.exists(salida):
        raise SystemExit("la compilacion no produjo el binario")
    parte("motor compilado")
    return salida


# --------------------------------------------------------------------------
# 3. Bancos de medida
# --------------------------------------------------------------------------
def run_bench(proj, args):
    """Compila y ejecuta un banco; devuelve su salida."""
    src = os.path.join(proj, "tools", args["src"])
    out = f"/tmp/bench_{args['tag']}"
    sh(["g++", "-O2", "-std=c++17", "-Iengine", "engine/cortex_lm.cpp", src, "-o", out],
       cwd=proj)
    cmd = [out] + [str(a) for a in args["argv"]]
    return sh(cmd, cwd=proj, check=False)


def parse_res(out):
    """Lee la salida de bench_res (reservorio)."""
    v = re.search(r"lector local\s*:\s*([\d.]+)\s*bpc", out)
    b = re.search(r"referencia 1 caracter\s*:\s*([\d.]+)\s*bpc", out)
    r2 = re.search(r"referencia 2 caracteres\s*:\s*([\d.]+)\s*bpc", out)
    if not v:
        return None
    return {"bpc": float(v.group(1)),
            "bigram": float(b.group(1)) if b else float("nan"),
            "ref2": float(r2.group(1)) if r2 else float("nan"),
            "acc": 0.0}


def parse(out):
    m = re.search(r"TEST[^:]*:\s*([\d.]+)\s*bpc\s*\|\s*acierto\s*([\d.]+)", out)
    b = re.search(r"bigrama[^:]*:\s*([\d.]+)\s*bpc", out)
    if not m:
        return None
    return {"bpc": float(m.group(1)), "acc": float(m.group(2)),
            "bigram": float(b.group(1)) if b else float("nan")}


def main():
    print(SEP)
    print("CORTEX-LM — banco de medida sobre texto español real")
    print(SEP)

    parte("arranca el cuaderno")
    comprobar_internet()
    text = get_corpus()
    text = strip_gutenberg(text)
    text = text.replace("\r\n", "\n")
    print(f"\ntexto obtenido: {len(text):,} caracteres")
    parte(f"texto conseguido: {len(text):,} caracteres")
    audit_corpus(text, "todo")

    proj = get_repo()
    parte("motor localizado en el dataset")
    binary = build(proj)

    # --- particiones: el test sale del FINAL del texto, nunca visto ---
    # Particion DISJUNTA y en orden: el test sale del tramo siguiente, que la
    # red no ha visto nunca. (Antes esto estaba invertido: entrenaba con un
    # trozo pequeño y medía sobre el resto.)
    train = text[:TRAIN_CHARS]
    holdout = text[TRAIN_CHARS:TRAIN_CHARS + HOLDOUT_CHARS]
    data_dir = os.path.join(proj, "data")
    os.makedirs(data_dir, exist_ok=True)
    with open(os.path.join(data_dir, "spanish_small.txt"), "w", encoding="utf-8") as f:
        f.write(train)
    with open(os.path.join(data_dir, "spanish_holdout.txt"), "w", encoding="utf-8") as f:
        f.write(holdout)
    print(f"\nentrenamiento: {len(train):,} | test no visto: {len(holdout):,}")
    if len(train) < 3000 or len(holdout) < 1000:
        raise SystemExit(f"particiones demasiado pequeñas (train {len(train)}, "
                         f"test {len(holdout)}): revisa CORTEX_TRAIN_CHARS")
    audit_corpus(train, "train")
    audit_corpus(holdout, "test")

    # --- auditoría estructural del motor ---
    print("\n--- auditoría estructural ---")
    parte(f"particiones: train {len(train):,} / test {len(holdout):,}; auditoria en curso")
    audit_out = sh([binary, "audit"], cwd=proj, check=False)
    audit_ok = "TODO OK" in audit_out
    print(f"auditoría: {'TODO OK' if audit_ok else 'FALLÓ'}")
    parte(f"auditoria del motor: {'TODO OK' if audit_ok else 'FALLO'}")

    # --- bancos ---
    N = min(TRAIN_CHARS, len(train) - 2)
    runs = []          # historico
    ref_bigram = float("nan")
    # ------------------------------------------------------------------
    # El sistema definitivo: lector RLS (regla local) + capa con memoria.
    # bench_rls usa el motor que se acaba de mejorar: 8 ticks por simbolo y
    # fatiga mas lenta. Cinco semillas por condicion, como se acordo.
    # ------------------------------------------------------------------
    SEMILLAS = [1234, 7, 99, 2024, 555]
    # argumentos: NTR EPOCAS K LH PL ETA WSCALE LAG NORM METODO lambda alf p0 SEED
    # Segun que banco se haya podido copiar, los argumentos son distintos.
    # bench_rls: NTR EPOCAS K LH PL ETA WSCALE LAG NORM METODO lambda alf p0 SEED
    # bench_joint (sustituto): EPOCHS K LH PL ETA WSCALE LAG [NMAX]
    # Pasarle los argumentos de uno al otro no da error: da una medida falsa.
    # Por eso se comprueba que el banco sea el bueno antes de medir.
    with open(os.path.join(proj, "tools", "bench_rls.cpp"),
              encoding="utf-8", errors="ignore") as f:
        es_rls = "rls_paso" in f.read()
    if not es_rls:
        parte("AVISO: el dataset es antiguo y no trae el banco RLS")
        print("\nEl dataset adjunto no trae bench_rls.cpp, que es el unico banco")
        print("que mide el sistema definitivo. Sin el, las cifras de abajo NO")
        print("valen: son otro banco con otros argumentos. Hay que regenerar el")
        print("dataset en el telefono con:  python3 tools/kaggle_dataset.py")

    def correr(tag, argv):
        parte(f"{tag} arrancando")
        out = run_bench(proj, {"src": "bench_rls.cpp", "tag": tag, "argv": argv})
        nonlocal ref_bigram
        r = parse(out)
        if r:
            parte(f"{tag}: {r['bpc']:.3f} bpc, acierto {r['acc']:.1f}%")
            runs.append(r)
            if ref_bigram != ref_bigram:      # primera vez (NaN)
                ref_bigram = r["bigram"]
        return r

    def serie(nombre, base_argv):
        """Corre las 5 semillas y devuelve media y desviacion."""
        vals = []
        for sem in SEMILLAS:
            r = correr(f"{nombre}_s{sem}", list(base_argv) + [sem])
            if r:
                vals.append(r["bpc"])
        if not vals:
            return None
        media = sum(vals) / len(vals)
        var = sum((v - media) ** 2 for v in vals) / max(1, len(vals) - 1)
        desv = var ** 0.5
        print(f"  {nombre}: media {media:.3f} desv {desv:.3f} "
              f"({', '.join(f'{v:.3f}' for v in vals)})")
        parte(f"{nombre}: media {media:.3f} bpc desv {desv:.3f}")
        return {"nombre": nombre, "media": media, "desv": desv,
                "vals": vals, "bpc": media, "acc": 0.0}

    if not es_rls:
        # Sin el banco bueno no se mide: mejor decirlo que publicar una cifra
        # que parece un resultado y no lo es.
        parte("PARADO: hace falta regenerar el dataset con el motor actual")
        print("\n" + SEP)
        print("NO SE MIDIO NADA. Copia este mensaje en el chat.")
        print(SEP)
        print(f"auditoría del motor: {'TODO OK' if audit_ok else 'FALLÓ'}")
        return 2

    print("\n--- sistema completo, texto no visto, 5 semillas ---")
    # LH=8: con la linea de retardo (configuracion que cumple el objetivo).
    sys_sin = serie("Sistema sin plasticidad",
                    [N, 1, 64, 8, 0, 0, 0.30, 1, 0, 1, 1.0, 1.0, 100])
    sys_con = serie("Sistema con plasticidad",
                    [N, 1, 64, 8, 1, 3e-4, 0.30, 1, 0, 1, 1.0, 1.0, 100])

    print("\n--- la capa sola, sin linea de retardo (LH=1) ---")
    # Aqui la unica memoria posible es la capa. Sirve para saber cuanto aporta.
    cap_sin = serie("Capa sola sin plasticidad",
                    [N, 1, 64, 1, 0, 0, 0.30, 1, 0, 1, 1.0, 1.0, 100])
    cap_con = serie("Capa sola con plasticidad",
                    [N, 1, 64, 1, 1, 3e-4, 0.30, 1, 0, 1, 1.0, 1.0, 100])

    # --- resumen ---
    parte("TODO TERMINADO: tabla lista")
    print("\n" + SEP)
    print("RESUMEN: bpc sobre texto NO VISTO. Cinco semillas. Menor es mejor.")
    print(SEP)
    print(f"{'condicion':<34}{'media':>8}{'desv':>8}  {'mejor':>7}{'peor':>7}")
    filas = [sys_sin, sys_con, cap_sin, cap_con]
    for f in filas:
        if not f:
            continue
        print(f"{f['nombre']:<34}{f['media']:>8.3f}{f['desv']:>8.3f}"
              f"  {min(f['vals']):>7.3f}{max(f['vals']):>7.3f}")
    print(SEP)
    if sys_sin and sys_con:
        d = sys_con["media"] - sys_sin["media"]
        pool = ((sys_sin["desv"] ** 2 + sys_con["desv"] ** 2) / 2) ** 0.5
        veredicto = "EMPATE" if abs(d) < 2 * pool else "EFECTO"
        print(f"plasticidad en el sistema completo: {d:+.3f} bpc -> {veredicto}")
    if cap_sin and cap_con:
        d = cap_con["media"] - cap_sin["media"]
        pool = ((cap_sin["desv"] ** 2 + cap_con["desv"] ** 2) / 2) ** 0.5
        veredicto = "EMPATE" if abs(d) < 2 * pool else "EFECTO"
        print(f"plasticidad en la capa sola:        {d:+.3f} bpc -> {veredicto}")
    # -------------------------------------------------------------------
    # Opcion 2: reservorio recurrente (estado que se actualiza, no ventana).
    # Aqui se usan TODOS los caracteres disponibles, que es donde el banco
    # anterior no podia llegar por memoria.
    # -------------------------------------------------------------------
    print("\n" + SEP)
    print("RESERVORIO RECURRENTE + lector local (sin ventana congelada)")
    print(SEP)
    N_RES = 1500
    res_vals, res_bigram, res_ref2 = [], float("nan"), float("nan")
    for sem in SEMILLAS:
        parte(f"reservorio semilla {sem} arrancando")
        out = run_bench(proj, {"src": "bench_res.cpp", "tag": f"res_{sem}",
                               "argv": [N, HOLDOUT_CHARS, N_RES, 0.35, 0.9, sem,
                                        1.0, 10, 0, 0, 0.05, 4, 0, 8, 0, 0]})
        r = parse_res(out)
        if r:
            res_vals.append(r["bpc"])
            res_bigram = r["bigram"]
            res_ref2 = r["ref2"]
            parte(f"reservorio semilla {sem}: {r['bpc']:.3f} bpc "
                  f"(bigrama {r['bigram']:.3f})")
    if res_vals:
        m = sum(res_vals) / len(res_vals)
        var = sum((v - m) ** 2 for v in res_vals) / max(1, len(res_vals) - 1)
        d = var ** 0.5
        print(f"{'Reservorio + lector local':<34}{m:>8.3f}{d:>8.3f}"
              f"  {min(res_vals):>7.3f}{max(res_vals):>7.3f}")
        print(f"bigrama de referencia: {res_bigram:.3f} | "
              f"referencia 2 car.: {res_ref2:.3f}")
        print(f"frente al bigrama: {m - res_bigram:+.3f} bpc "
              f"({'GANA' if m < res_bigram else 'PIERDE'})")
        parte(f"RESERVORIO: {m:.3f} bpc frente a bigrama {res_bigram:.3f}")
    print(SEP)
    print(f"auditoría del motor: {'TODO OK' if audit_ok else 'FALLÓ'}")
    print("Copia esta tabla y pégala en el chat.")
    return 0


if __name__ == "__main__":
    sys.exit(main())
