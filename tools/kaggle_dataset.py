#!/usr/bin/env python3
"""Prepara un DATASET de Kaggle con el texto y el motor, para kernels SIN internet.

Por que existe
--------------
Kernels sin salida a red (cuenta sin verificar, o Kaggle que la niega aunque lo
pida) no pueden bajar ni el texto ni el codigo. Los datasets, en cambio, se
montan dentro del kernel en /kaggle/input sin necesitar red.

Este script corre en el TELEFONO, que si tiene internet:
  1. Baja varias obras espanolas del Proyecto Gutenberg.
  2. Las limpia y audita (rechaza corpus repetido o sin variedad).
  3. Baja el codigo del motor.
  4. Lo empaqueta todo y lo sube como dataset privado.
  5. Imprime la referencia que hay que pasar al lanzador.

Uso:  python3 kaggle_dataset.py
"""

import json
import os
import re
import subprocess
import sys
import urllib.request

REPO = os.environ.get("CORTEX_REPO", "alexsndersoto04-source/nuby-")
BRANCH = os.environ.get("CORTEX_BRANCH", "main")
SLUG = "corpus-espanol-cortex"
WORK = os.path.expanduser("~/cortex_dataset")

# Obras de dominio publico en espanol. Cuantas mas, mas variado el texto.
LIBROS = {
    "2000": "Don Quijote de la Mancha",
    "17073": "La vida de Lazarillo de Tormes",
    "36064": "Niebla",
    "46022": "Novelas ejemplares",
}

MOTOR = [
    "cortex-lm/Makefile",
    "cortex-lm/engine/cortex_lm.cpp",
    "cortex-lm/engine/cortex_lm.hpp",
    "cortex-lm/engine/cortex_lm_main.cpp",
    "cortex-lm/tools/bench_cortical.cpp",
    "cortex-lm/tools/cerebro.cpp",
    "cortex-lm/tools/generar_alineacion.py",
]

# Commit fijo de reserva: si la rama da una copia cacheada o falla, se
# pide por hash, que no se puede cachear.
PIN = "c92698e"

SEP = "=" * 62


def say(m):
    print(f"\n==> {m}", flush=True)


def die(m):
    print(f"\nERROR: {m}", file=sys.stderr)
    sys.exit(1)


def binario_kaggle():
    """Localiza el ejecutable de kaggle.

    Esta instalado en un entorno virtual (/root/kv/bin/kaggle), que no esta en
    el PATH. Dar por hecho que se llama "kaggle" a secas rompe con
    FileNotFoundError. Ha pasado dos veces.
    """
    import shutil as _sh
    candidatos = [
        "/root/kv/bin/kaggle",
        os.path.expanduser("~/kv/bin/kaggle"),
        "/usr/local/bin/kaggle",
    ]
    for c in candidatos:
        if os.path.exists(c) and os.access(c, os.X_OK):
            return c
    encontrado = _sh.which("kaggle")
    if encontrado:
        return encontrado
    die("no encuentro el ejecutable de kaggle. Reinstala con: bash setup.sh")


def bajar(url, timeout=90):
    req = urllib.request.Request(url, headers={"User-Agent": "cortex-lm/1.0"})
    with urllib.request.urlopen(req, timeout=timeout) as f:
        return f.read()


def limpiar_gutenberg(t):
    i = re.search(r"\*\*\* ?START OF (THE|THIS) PROJECT GUTENBERG.*?\*\*\*", t)
    if i:
        t = t[i.end():]
    j = re.search(r"\*\*\* ?END OF (THE|THIS) PROJECT GUTENBERG", t)
    if j:
        t = t[:j.start()]
    return t


def auditar(texto, etiqueta):
    n = len(texto)
    alfa = len(set(texto))
    ventana = 80
    muestras = [texto[i:i + ventana] for i in range(0, max(1, n - ventana), 97)]
    unicas = len(set(muestras))
    ratio = unicas / max(1, len(muestras))
    print(f"  [{etiqueta}] {n:,} caracteres | alfabeto {alfa} | "
          f"unicas {100*ratio:.1f} %")
    if ratio < 0.50:
        die(f"corpus repetido ({100*ratio:.1f} % de ventanas unicas)")
    if alfa < 60:
        die(f"alfabeto de solo {alfa} simbolos: no parece texto natural")
    return ratio


def main():
    global KAG
    KAG = binario_kaggle()
    print(f"usando: {KAG}")

    os.makedirs(WORK, exist_ok=True)
    # Las subcarpetas se crean aqui, no al escribir: si no, el primer open()
    # falla con FileNotFoundError. Ya paso una vez.
    os.makedirs(os.path.join(WORK, "motor"), exist_ok=True)
    tok = os.path.expanduser("~/.kaggle/access_token")
    if not os.path.exists(tok) and not os.path.exists(
            os.path.expanduser("~/.kaggle/kaggle.json")):
        die("faltan las credenciales de Kaggle. Ejecuta antes: bash token.sh")

    # ---------------------------------------------------------------- texto
    ruta_texto = os.path.join(WORK, "espanol.txt")
    reutilizar = os.path.exists(ruta_texto) and os.path.getsize(ruta_texto) > 1_000_000

    if reutilizar:
        # Si ya se bajo antes, no se repite: son varios MB y un fallo posterior
        # (como el de la carpeta motor) no debe obligar a bajarlo otra vez.
        say("Reutilizando el texto ya bajado")
        with open(ruta_texto, encoding="utf-8") as f:
            texto = f.read()
        print(f"  {len(texto):,} caracteres")
    else:
        say("Bajando obras en espanol (en el telefono, que si tiene red)")
        partes = []
        for ident, titulo in LIBROS.items():
            url = f"https://www.gutenberg.org/cache/epub/{ident}/pg{ident}.txt"
            print(f"  {titulo}...", end=" ", flush=True)
            try:
                t = bajar(url).decode("utf-8", errors="ignore")
                t = limpiar_gutenberg(t)
                if len(t) < 50_000:
                    print(f"corto ({len(t):,}), se descarta")
                    continue
                partes.append(t)
                print(f"{len(t):,} caracteres")
            except Exception as e:  # noqa: BLE001
                print(f"no ({type(e).__name__})")

        if not partes:
            die("no se pudo bajar ninguna obra. Revisa la conexion del telefono.")

        texto = "\n\n".join(partes).replace("\r\n", "\n")
        print(f"\ntexto total: {len(texto):,} caracteres")
        with open(ruta_texto, "w", encoding="utf-8") as f:
            f.write(texto)

    auditar(texto, "corpus completo")

    # ---------------------------------------------------------------- motor
    say("Bajando el motor")
    for ruta in MOTOR:
        destino = os.path.join(WORK, "motor", os.path.basename(ruta))
        try:
            datos = bajar(f"https://raw.githubusercontent.com/{REPO}/{BRANCH}/{ruta}")
        except Exception:  # noqa: BLE001
            # La versión fijada por commit como reserva: una rama se puede
            # cachear, un hash no.
            try:
                datos = bajar(f"https://raw.githubusercontent.com/{REPO}/{PIN}/{ruta}")
            except Exception as e:  # noqa: BLE001
                die(f"no pude bajar {ruta} ({type(e).__name__})")
        with open(destino, "wb") as f:
            f.write(datos)
        print(f"  {os.path.basename(ruta)} ({len(datos):,} bytes)")

    # ---------------------------------------------------------------- ficha
    usuario = None
    try:
        with open(os.path.expanduser("~/.kaggle/kaggle.json"), encoding="utf-8") as f:
            usuario = (json.load(f).get("username") or "").strip().lower() or None
    except Exception:  # noqa: BLE001
        pass
    if not usuario:
        r = subprocess.run([KAG, "config", "view"], capture_output=True, text=True)
        m = re.search(r"username[^:]*[:=]\s*([\w.-]+)", r.stdout + r.stderr, re.I)
        usuario = m.group(1).lower() if m else None
    if not usuario:
        die("no se tu usuario de Kaggle")

    meta = {
        "title": "Corpus espanol CORTEX-LM",
        "id": f"{usuario}/{SLUG}",
        "licenses": [{"name": "CC0-1.0"}],
    }
    with open(os.path.join(WORK, "dataset-metadata.json"), "w", encoding="utf-8") as f:
        json.dump(meta, f, indent=2)

    # ---------------------------------------------------------------- subir
    say("Subiendo a Kaggle como dataset privado")
    ref = f"{usuario}/{SLUG}"
    # Primero se INTENTA una version nueva. Si el dataset no existe, version
    # falla con "not found" y entonces se crea. Antes se hacia al reves y la
    # deteccion de "ya existe" estaba escrita como "already exists", pero Kaggle
    # contesta 'The requested slug "..." is already in use by a dataset': la
    # condicion nunca se cumplia, se daba por bueno el fallo y se imprimia
    # "Dataset listo" sin haber subido NADA. Ese fue el error que hizo que la
    # corrida siguiente siguiera viendo el dataset viejo.
    print("  intentando subir VERSION nueva del dataset")
    r = subprocess.run([KAG, "datasets", "version", "-p", WORK, "-r", "zip",
                        "-m", "motor actualizado (RLS + capa con memoria)"],
                       capture_output=True, text=True)
    salida = (r.stdout or "") + (r.stderr or "")
    bajo = salida.lower()
    if r.returncode != 0 and ("not found" in bajo or "404" in bajo
                              or "does not exist" in bajo):
        print("  el dataset no existe todavia: creandolo")
        r = subprocess.run([KAG, "datasets", "create", "-p", WORK, "-r", "zip"],
                           capture_output=True, text=True)
        salida = (r.stdout or "") + (r.stderr or "")
    print(salida.strip()[-1500:])
    if r.returncode != 0:
        die("Kaggle rechazo el dataset. El texto de arriba dice por que.")

    # Comprobacion de verdad: el dataset tiene que llevar el banco RLS.
    print()
    say("Comprobando que el dataset lleva el banco RLS")
    listado = subprocess.run([KAG, "datasets", "files", ref],
                             capture_output=True, text=True)
    txt = (listado.stdout or "") + (listado.stderr or "")
    print(txt.strip()[:600])
    if listado.returncode == 0 and txt.strip() and "bench_rls" not in txt:
        print()
        print("  OJO: el listado no menciona bench_rls.cpp.")
        print("  Kaggle puede tardar unos minutos en propagar la version nueva.")
        print("  Si la corrida se para con 'dataset antiguo', espera 5 minutos")
        print("  y vuelve a lanzar la version del dataset a mano en Kaggle.")

    print("\n" + SEP)
    print("Dataset listo:  " + ref)
    print(SEP)
    print("\nAhora lanza la medida con:")
    print(f"    bash run.sh --dataset {ref}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
