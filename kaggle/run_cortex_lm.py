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
import subprocess
import sys
import urllib.request

REPO_GH = os.environ.get("CORTEX_REPO", "alexsndersoto04-source/nuby-")
BRANCH = os.environ.get("CORTEX_BRANCH", "arena/c7deda7f-nuby")
WORK = os.environ.get("CORTEX_REPO_DIR", "/kaggle/working/nuby-")
# En local (pruebas) se salta la descarga y se usa el corpus de juguete.
LOCAL = os.environ.get("CORTEX_LOCAL", "") == "1"

# Cuánto texto usar para entrenar. 60k símbolos ya dan una medida estable y
# mantienen cada corrida en minutos, no horas.
TRAIN_CHARS = int(os.environ.get("CORTEX_TRAIN_CHARS", "60000"))
HOLDOUT_CHARS = int(os.environ.get("CORTEX_HOLDOUT_CHARS", "12000"))
EPOCHS_B = int(os.environ.get("CORTEX_EPOCHS_B", "8"))

SEP = "=" * 70


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
    if alphabet < 60:
        problemas.append(f"alfabeto de solo {alphabet} símbolos, no parece texto natural")
    if problemas:
        for x in problemas:
            print(f"  *** AVISO: {x}")
        if not LOCAL:
            raise SystemExit(
                "\n*** CORPUS NO APTO. Cualquier cifra medida sobre él sería"
                "\n*** memorización. Abortando para no repetir el error anterior.")
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
    "cortex-lm/tools/bench.cpp",
    "cortex-lm/tools/bench_joint.cpp",
    "cortex-lm/tools/make_test_corpus.py",
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


def get_repo():
    """Trae solo los ficheros necesarios, sin git.

    Con `git clone` esto dependia de que hubiera git instalado y de la URL.
    Bajando los ficheros con urllib el cuaderno funciona pegando codigo a mano
    en Kaggle, sin clonar nada.
    """
    if LOCAL:
        print("\n--- MODO LOCAL: usando el codigo ya presente ---")
        return os.path.join(WORK, "cortex-lm")

    # Si hay motor en un dataset adjunto, se usa y no hace falta red.
    _txt, ruta_motor = buscar_en_dataset()
    if ruta_motor:
        destino = os.path.join(WORK, "cortex-lm")
        os.makedirs(os.path.join(destino, "engine"), exist_ok=True)
        os.makedirs(os.path.join(destino, "tools"), exist_ok=True)
        nombres = {"cortex_lm.cpp": "engine", "cortex_lm.hpp": "engine",
                   "cortex_lm_main.cpp": "engine",
                   "bench.cpp": "tools", "bench_joint.cpp": "tools"}
        copiados = 0
        for nombre, sub in nombres.items():
            origen = os.path.join(ruta_motor, nombre)
            if os.path.exists(origen):
                import shutil
                shutil.copy(origen, os.path.join(destino, sub, nombre))
                copiados += 1
        if copiados >= 5:
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
    print("\n--- compilando el motor ---")
    sh(["make", "clean"], cwd=proj, check=False)
    # 'portable' no usa -march=native: las máquinas de Kaggle cambian.
    sh(["make", "portable"], cwd=proj)
    return os.path.join(proj, "bin", "cortex_lm")


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

    comprobar_internet()
    text = get_corpus()
    text = strip_gutenberg(text)
    text = text.replace("\r\n", "\n")
    print(f"\ntexto obtenido: {len(text):,} caracteres")
    audit_corpus(text, "todo")

    proj = get_repo()
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
    audit_out = sh([binary, "audit"], cwd=proj, check=False)
    audit_ok = "TODO OK" in audit_out
    print(f"auditoría: {'TODO OK' if audit_ok else 'FALLÓ'}")

    # --- bancos ---
    N = min(TRAIN_CHARS, len(train) - 2)
    runs = []
    print("\n--- banco A: lector entrenado sobre features congeladas ---")
    for tag, argv in (
        ("A_sin", [N, 10, 64, 8, 0, 0, 0.30, 0]),
        ("A_con", [N, 10, 64, 8, 1, 1e-3, 0.30, 0]),
    ):
        out = run_bench(proj, {"src": "bench.cpp", "tag": tag, "argv": argv})
        r = parse(out)
        if r:
            runs.append((f"A {tag.split('_')[1]}", r))

    print("\n--- banco B: entrenamiento conjunto en línea, símbolo siguiente ---")
    for tag, argv in (
        ("B_sin",     [EPOCHS_B, 64, 8, 0, 0,    0.30, 1, N]),
        ("B_con_bajo",[EPOCHS_B, 64, 8, 1, 1e-3, 0.30, 1, N]),
        ("B_con_medio",[EPOCHS_B, 64, 8, 1, 1e-2, 0.30, 1, N]),
        ("B_con_alto",[EPOCHS_B, 64, 8, 1, 3e-2, 0.30, 1, N]),
    ):
        out = run_bench(proj, {"src": "bench_joint.cpp", "tag": tag, "argv": argv})
        r = parse(out)
        if r:
            runs.append((f"B {tag.split('_', 1)[1]}", r))

    # --- resumen ---
    print("\n" + SEP)
    print("RESUMEN (bpc menor es mejor; 'bigrama' es la referencia a batir)")
    print(SEP)
    print(f"{'prueba':<16}{'bpc':>8}{'acierto':>10}{'bigrama':>10}{'mejora':>10}")
    for name, r in runs:
        mej = r["bigram"] - r["bpc"]
        print(f"{name:<16}{r['bpc']:>8.3f}{r['acc']:>9.1f}%{r['bigram']:>10.3f}{mej:>+10.3f}")
    print(SEP)
    print(f"auditoría del motor: {'TODO OK' if audit_ok else 'FALLÓ'}")
    print("Copia esta tabla y pégala en el chat.")
    return 0


if __name__ == "__main__":
    sys.exit(main())
