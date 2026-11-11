#!/usr/bin/env python3
"""
CORTEX-LM en Kaggle: jerarquia cortical LIF pura + CSR escalable + aprendizaje continuo.

Que hace
--------
1. Obtiene un corpus amplio y limpio en espanol (dataset adjunto, GITenberg en GitHub,
   Hugging Face o Project Gutenberg) con decodificacion UTF-8 / ISO-8859-1 intacta.
2. Audita el corpus antes de usarlo (rechaza texto repetido en bucle).
3. Compila los binarios C++ puros (cortex_lm, bench_cortical, cerebro) sin dependencias.
4. Ejecuta la auditoria estructural (5/5 pruebas del motor LIF).
5. Mide la escalabilidad CSR y memoria O(1) en secuencia (hasta >130,000 neuronas LIF).
6. Corre el banco de 5 semillas sobre holdout cronologico jamas visto (STDP=0 vs STDP=1).
7. Entrena un cerebro persistente en espanol, aplica aprendizaje continuo (--continuo)
   sobre un segundo dominio literario/expositivo y lo alinea conversacionalmente,
   dejando el modelo listo en /kaggle/working/cerebro_es.bin.
"""

import io
import os
import re
import shutil
import statistics
import subprocess
import sys
import tarfile
import urllib.request

REPO_GH = os.environ.get("CORTEX_REPO", "alexsndersoto04-source/ask-")
BRANCH = os.environ.get("CORTEX_BRANCH", "arena/886cbd4f-ask")
WORK = os.environ.get("CORTEX_REPO_DIR", "/kaggle/working/ask-")
LOCAL = os.environ.get("CORTEX_LOCAL", "") == "1"

TRAIN_CHARS = int(os.environ.get("CORTEX_TRAIN_CHARS", "300000"))
HOLDOUT_CHARS = int(os.environ.get("CORTEX_HOLDOUT_CHARS", "40000"))

SEP = "=" * 70

PARTE = "/kaggle/working/avance.txt" if os.path.isdir("/kaggle/working") \
    else os.path.join(os.path.dirname(os.path.abspath(__file__)), "avance.txt")


def parte(msg):
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
    print(f"$ {' '.join(cmd) if isinstance(cmd, list) else cmd}")
    r = subprocess.run(cmd, cwd=cwd, shell=isinstance(cmd, str),
                       capture_output=True, text=True, errors="replace", env=env)
    out = (r.stdout or "") + (r.stderr or "")
    print(out.rstrip()[-4000:])
    if check and r.returncode != 0:
        raise SystemExit(f"falló: {cmd}")
    return out


def _decodificar(raw: bytes) -> str:
    try:
        return raw.decode("utf-8-sig")
    except UnicodeDecodeError:
        return raw.decode("iso-8859-1")


def strip_gutenberg(text: str) -> str:
    start = re.search(r"\*\*\* ?START OF (THE|THIS) PROJECT GUTENBERG.*?\*\*\*", text, flags=re.I | re.S)
    if start:
        text = text[start.end():]
    end = re.search(r"\*\*\* ?END OF (THE|THIS) PROJECT GUTENBERG", text, flags=re.I)
    if end:
        text = text[:end.start()]
    text = text.replace("\r\n", "\n").replace("\r", "\n")
    text = re.sub(r"\n{3,}", "\n\n", text)
    return text.strip() + "\n"


def _alcanzable(host, timeout=6):
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
    if LOCAL:
        return
    if os.path.isdir("/kaggle/input"):
        for root, _d, files in os.walk("/kaggle/input"):
            for name in files:
                if name.endswith((".txt", ".csv", ".tsv", ".zip")):
                    if os.path.getsize(os.path.join(root, name)) > 50_000:
                        print("hay un dataset adjunto: no hace falta internet")
                        return

    if any(_alcanzable(h) for h in ("codeload.github.com", "raw.githubusercontent.com", "www.gutenberg.org")):
        return

    raise SystemExit(
        "\n" + "=" * 62 + "\n"
        "ESTE KERNEL NO TIENE SALIDA A INTERNET NI DATASET ADJUNTO.\n"
        "Verifica el telefono en https://www.kaggle.com/settings o adjunta un dataset.\n"
        + "=" * 62)


def _descargar_gitenberg(url: str):
    try:
        req = urllib.request.Request(url, headers={"User-Agent": "cortex-lm/2.0"})
        with urllib.request.urlopen(req, timeout=60) as resp:
            raw_tar = resp.read()
        tf = tarfile.open(fileobj=io.BytesIO(raw_tar), mode="r:gz")
        cands = [m for m in tf.getmembers()
                 if m.isfile() and m.name.endswith(".txt") and "/old/" not in m.name and m.size > 50_000]
        if not cands:
            return None
        cands.sort(key=lambda m: ("-0.txt" in m.name, m.size), reverse=True)
        return strip_gutenberg(_decodificar(tf.extractfile(cands[0]).read()))
    except Exception as e:  # noqa: BLE001
        print(f"    no: {url} ({type(e).__name__}: {e})")
        return None


def extraer_zips(base):
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
    base = "/kaggle/input"
    if not os.path.isdir(base):
        return None, None
    extraido = extraer_zips(base)
    raices = [base] + ([extraido] if extraido else [])
    texto, motor, mejor = None, None, 0
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
            cpps = [f for f in files if f.endswith((".cpp", ".hpp"))]
            if len(cpps) >= 3 and motor is None:
                motor = root
    return texto, motor


def get_corpus():
    """Devuelve (texto_moderno, texto_clasico) en espanol limpio."""
    if LOCAL:
        proj = os.path.join(WORK, "cortex-lm")
        sh(["python3", "data/build_corpus_large.py"], cwd=proj)
        with open(os.path.join(proj, "data", "corpus.txt"), encoding="utf-8") as f:
            t = f.read()
        mitad = len(t) // 2
        return t[:mitad], t[mitad:]

    print("\n--- buscando obras literarias en espanol ---")
    ruta_txt, _ = buscar_en_dataset()
    if ruta_txt:
        with open(ruta_txt, "rb") as f:
            t = strip_gutenberg(_decodificar(f.read()))
        mitad = (len(t) * 2) // 3
        return t[:mitad], t[mitad:]

    if _alcanzable("codeload.github.com"):
        obras_mod = []
        for url in (
            "https://codeload.github.com/GITenberg/Marianela_17340/tar.gz/master",
            "https://codeload.github.com/GITenberg/Tormento_31465/tar.gz/master",
            "https://codeload.github.com/GITenberg/Obras-escogidas_53552/tar.gz/master",
        ):
            t = _descargar_gitenberg(url)
            if t:
                obras_mod.append(t)
        quijote = _descargar_gitenberg("https://codeload.github.com/GITenberg/Don-Quijote_2000/tar.gz/master")
        if obras_mod and quijote:
            return "\n\n".join(obras_mod), quijote

    for book in ("2000", "17073", "36064", "46022"):
        url = f"https://www.gutenberg.org/cache/epub/{book}/pg{book}.txt"
        try:
            req = urllib.request.Request(url, headers={"User-Agent": "cortex-lm/2.0"})
            with urllib.request.urlopen(req, timeout=90) as f:
                t = strip_gutenberg(_decodificar(f.read()))
            if len(t) > 200_000:
                mitad = (len(t) * 2) // 3
                return t[:mitad], t[mitad:]
        except Exception as e:  # noqa: BLE001
            print(f"    no: {url} ({e})")

    raise SystemExit("No se pudo obtener corpus espanol.")


def audit_corpus(text, label):
    n = len(text)
    alphabet = len(set(text))
    ventana = 80
    muestras = [text[i:i + ventana] for i in range(0, max(1, n - ventana), 97)]
    unicas = len(set(muestras))
    ratio = unicas / max(1, len(muestras))
    print(f"  [{label}] {n:,} chars | alfabeto={alphabet} | ventanas unicas={100*ratio:.1f}%")
    if ratio < 0.50 and not LOCAL:
        raise SystemExit(f"Corpus repetido ({100*ratio:.1f}% ventanas unicas)")
    return ratio


FICHEROS = [
    "cortex-lm/Makefile",
    "cortex-lm/engine/cortex_lm.cpp",
    "cortex-lm/engine/cortex_lm.hpp",
    "cortex-lm/engine/cortex_lm_main.cpp",
    "cortex-lm/tools/bench_cortical.cpp",
    "cortex-lm/tools/cerebro.cpp",
    "cortex-lm/tools/generar_alineacion.py",
]


def _descarga(url, destino):
    req = urllib.request.Request(url, headers={"User-Agent": "cortex-lm/2.0"})
    with urllib.request.urlopen(req, timeout=60) as f:
        datos = f.read()
    os.makedirs(os.path.dirname(destino), exist_ok=True)
    with open(destino, "wb") as f:
        f.write(datos)


def escribir_motor_pegado():
    try:
        motor = motor_pegado()  # noqa: F821
    except Exception:  # noqa: BLE001
        return None
    if not motor:
        return None
    destino = os.path.join(WORK, "cortex-lm")
    for ruta, texto in motor.items():
        rel = ruta.split("cortex-lm/", 1)[-1]
        salida = os.path.join(destino, rel)
        os.makedirs(os.path.dirname(salida), exist_ok=True)
        with open(salida, "w", encoding="utf-8") as f:
            f.write(texto)
    print(f"  motor pegado en el cuaderno ({len(motor)} ficheros)")
    return destino


def get_repo():
    pegado = escribir_motor_pegado()
    if pegado:
        return pegado
    if LOCAL:
        return os.path.join(WORK, "cortex-lm")

    _txt, ruta_motor = buscar_en_dataset()
    if ruta_motor:
        destino = os.path.join(WORK, "cortex-lm")
        for sub in ("engine", "tools"):
            os.makedirs(os.path.join(destino, sub), exist_ok=True)
        for ruta in FICHEROS:
            nombre = os.path.basename(ruta)
            sub = "engine" if "/engine/" in ruta else ("tools" if "/tools/" in ruta else "")
            origen = os.path.join(ruta_motor, nombre)
            if os.path.exists(origen):
                shutil.copy(origen, os.path.join(destino, sub, nombre) if sub else os.path.join(destino, nombre))
        if os.path.exists(os.path.join(destino, "engine", "cortex_lm.cpp")):
            return destino

    for rama in (BRANCH, "main"):
        fallos = []
        for ruta in FICHEROS:
            url = f"https://raw.githubusercontent.com/{REPO_GH}/{rama}/{ruta}"
            try:
                _descarga(url, os.path.join(WORK, ruta))
            except Exception as e:  # noqa: BLE001
                fallos.append(f"{ruta}: {type(e).__name__}")
        if not fallos:
            return os.path.join(WORK, "cortex-lm")
        print(f"  rama {rama}: fallaron {fallos}")
    raise SystemExit("No se pudo obtener el motor C++.")


def build_all(proj):
    bin_dir = os.path.join(proj, "bin")
    os.makedirs(bin_dir, exist_ok=True)
    cxx = next((c for c in ("g++", "c++", "clang++") if shutil.which(c)), None)
    if not cxx:
        raise SystemExit("No hay compilador C++ disponible.")
    flags = [cxx, "-O2", "-std=c++17", "-fno-math-errno", "-funroll-loops", "-Iengine"]
    sh(flags + ["engine/cortex_lm.cpp", "engine/cortex_lm_main.cpp", "-o", "bin/cortex_lm"], cwd=proj)
    sh(flags + ["engine/cortex_lm.cpp", "tools/bench_cortical.cpp", "-o", "bin/bench_cortical"], cwd=proj)
    sh(flags + ["engine/cortex_lm.cpp", "tools/cerebro.cpp", "-o", "bin/cerebro"], cwd=proj)
    return os.path.join(bin_dir, "cortex_lm"), os.path.join(bin_dir, "bench_cortical"), os.path.join(bin_dir, "cerebro")


def extraer_diccionario(textos):
    palabras = set()
    for t in textos:
        for m in re.finditer(r"[a-záéíóúüñ]{2,}", t, flags=re.I):
            palabras.add(m.group(0).lower())
    return sorted(palabras)


def main():
    print(SEP)
    print("CORTEX-LM — Jerarquia Cortical LIF Pura + CSR + Aprendizaje Continuo")
    print(SEP)

    parte("arranca corrida cortical")
    comprobar_internet()
    proj = get_repo()
    bin_main, bin_bench, bin_brain = build_all(proj)
    parte("binarios C++ compilados")

    audit_out = sh([bin_main, "audit"], cwd=proj)
    audit_ok = "TODO OK" in audit_out
    parte(f"auditoria estructural: {'TODO OK' if audit_ok else 'FALLO'}")

    print("\n--- Escalabilidad CSR y memoria constante O(1) ---")
    for cfg, deg, nch in (("1024", "52", 3000),
                          ("1024,8192,8192,6144", "52", 2500),
                          ("4096,98304,98304,98304", "16", 1500)):
        sh([bin_bench, "scale", cfg, deg, str(nch)], cwd=proj)

    txt_mod, txt_cla = get_corpus()
    audit_corpus(txt_mod, "moderno")
    audit_corpus(txt_cla, "clasico")

    data_dir = os.path.join(proj, "data")
    os.makedirs(data_dir, exist_ok=True)
    dic_path = os.path.join(data_dir, "dic_kaggle.txt")
    with open(dic_path, "w", encoding="utf-8") as f:
        f.write("\n".join(extraer_diccionario([txt_mod, txt_cla])) + "\n")

    # 1. Banco riguroso 5 semillas sobre tramo cronologico (45 KB, comparable con historico)
    n_bench = min(len(txt_cla), 46000)
    bench_corpus = os.path.join(data_dir, "bench_46k.txt")
    with open(bench_corpus, "w", encoding="utf-8") as f:
        f.write(txt_cla[:n_bench])

    semillas = [1234] if LOCAL else [1234, 7, 99, 2024, 555]
    ep_bench = "2" if LOCAL else "7"
    l1_b = "4096" if LOCAL else "8192"
    l23_b = "4096,3072" if LOCAL else "8192,6144"
    bpcs = {"0": [], "1": []}
    for stdp in ("0", "1"):
        for s in semillas:
            parte(f"bench_cortical stdp={stdp} seed={s}")
            out = sh([bin_bench, "bpc", bench_corpus, dic_path, l1_b, l23_b, ep_bench, "0.45", stdp, str(s)], cwd=proj)
            m = re.search(r"HOLDOUT=([0-9.]+)", out)
            if m:
                bpcs[stdp].append(float(m.group(1)))

    # 2. Entrenamiento amplio + aprendizaje continuo + alineacion conversacional
    gen_ali = os.path.join(proj, "tools", "generar_alineacion.py")
    if os.path.exists(gen_ali):
        sh(["python3", gen_ali], cwd=proj)

    n_tr_mod = min(TRAIN_CHARS, max(4000, len(txt_mod) - HOLDOUT_CHARS - 100))
    tr_mod_p = os.path.join(data_dir, "tr_mod.txt")
    ho_mod_p = os.path.join(data_dir, "ho_mod.txt")
    with open(tr_mod_p, "w", encoding="utf-8") as f:
        f.write(txt_mod[:n_tr_mod])
    with open(ho_mod_p, "w", encoding="utf-8") as f:
        f.write(txt_mod[n_tr_mod:n_tr_mod + min(HOLDOUT_CHARS, len(txt_mod) - n_tr_mod)])

    n_tr_cla = min(TRAIN_CHARS // 2, max(4000, len(txt_cla) - HOLDOUT_CHARS - 100))
    tr_cla_p = os.path.join(data_dir, "tr_cla.txt")
    ho_cla_p = os.path.join(data_dir, "ho_cla.txt")
    con_extra = ""
    con_p = os.path.join(data_dir, "conocimiento_es.txt")
    if os.path.exists(con_p):
        with open(con_p, encoding="utf-8") as f:
            con_extra = "\n\n" + f.read()
    with open(tr_cla_p, "w", encoding="utf-8") as f:
        f.write(txt_cla[:n_tr_cla] + con_extra)
    with open(ho_cla_p, "w", encoding="utf-8") as f:
        f.write(txt_cla[n_tr_cla:n_tr_cla + min(HOLDOUT_CHARS, len(txt_cla) - n_tr_cla)])

    out_model = "/kaggle/working/cerebro_es.bin" if os.path.isdir("/kaggle/working") \
        else os.path.join(data_dir, "cerebro_kaggle_test.bin")
    capas_brain = "1024,4096,4096,3072" if LOCAL else "1024,10240,10240,8192"
    ep_brain = "1" if LOCAL else "3"

    parte("entrenando cerebro fase 1 (literatura moderna)")
    sh([bin_brain, "entrenar", out_model, tr_mod_p, "--capas", capas_brain, "--grado", "48",
        "--epocas", ep_brain, "--lr", "0.45", "--stdp", "1", "--holdout", "0.0"], cwd=proj)

    parte("aprendizaje continuo fase 2 (--continuo)")
    sh([bin_brain, "entrenar", out_model, tr_cla_p, "--continuo",
        "--epocas", ep_brain, "--lr", "0.30", "--ancla", "0.025", "--holdout", "0.0"], cwd=proj)

    ali_p = os.path.join(data_dir, "alineacion_es.txt")
    if os.path.exists(ali_p):
        parte("alineacion deliberativa dorsal fase 3")
        sh([bin_brain, "alinear", out_model, ali_p,
            "--epocas", "6" if LOCAL else "28", "--lr", "0.50", "--ancla", "0.001"], cwd=proj)

    ev_mod = sh([bin_brain, "evaluar", out_model, ho_mod_p, dic_path], cwd=proj)
    ev_cla = sh([bin_brain, "evaluar", out_model, ho_cla_p, dic_path], cwd=proj)

    print("\n" + SEP)
    print("RESUMEN FINAL CORTEX-LM (100% SPIKING LIF PURO, CERO BACKPROP)")
    print(SEP)
    print(f"Auditoria estructural : {'TODO OK' if audit_ok else 'FALLO'}")
    for k, label in (("0", "Cortical LIF sin STDP"), ("1", "Cortical LIF con STDP")):
        if bpcs[k]:
            mu = statistics.mean(bpcs[k])
            sd = statistics.stdev(bpcs[k]) if len(bpcs[k]) > 1 else 0.0
            print(f"{label:<24}: {mu:.4f} ± {sd:.4f} bpc ({len(bpcs[k])} semillas)")
    for label, txt_ev in (("Holdout Moderno (retenido)", ev_mod), ("Holdout Clasico (continuo)", ev_cla)):
        m = re.search(r"HOLDOUT BPC[^:]*:\s*([0-9.]+)", txt_ev)
        if m:
            print(f"{label:<26}: {m.group(1)} bpc")
    print(f"Modelo persistido en  : {out_model}")
    print(SEP)
    parte("TODO TERMINADO")
    return 0


if __name__ == "__main__":
    sys.exit(main())
