#!/usr/bin/env python3
"""Descarga y limpia un corpus espanol literario amplio y no repetitivo desde GITenberg (GitHub).

Obras incluidas (dominio publico, espanol moderno y clasico, 100% ventanas unicas):
  1. Benito Perez Galdos — Marianela (espanol moderno claro, narrativa y dialogo)
  2. Benito Perez Galdos — Tormento (espanol moderno urbano y conversacional)
  3. Benito Perez Galdos — Nazarin (dialogos filosoficos y prosa moderna)
  4. Gustavo Adolfo Becquer — Obras escogidas (leyendas, ensayos, cartas)
  5. Miguel de Cervantes — Don Quijote de la Mancha (prosa rica clasica)

Genera en cortex-lm/data/ (ignorado por git para no inflar el repositorio):
  - data/libro_moderno.txt   (~1.3 MB: Galdos + Becquer, espanol moderno)
  - data/libro_quijote.txt   (~2.1 MB: Don Quijote completo, UTF-8 limpio)
  - data/corpus_amplio.txt   (seleccion equilibrada ~900 KB para entrenamiento agil en CPU)
  - data/dic_amplio.txt      (diccionario real extraido del corpus completo)
"""

import io
import os
import re
import sys
import tarfile
import urllib.request

LIBROS = [
    ("Marianela (Galdos)", "https://codeload.github.com/GITenberg/Marianela_17340/tar.gz/master"),
    ("Tormento (Galdos)", "https://codeload.github.com/GITenberg/Tormento_31465/tar.gz/master"),
    ("Nazarin (Galdos)", "https://codeload.github.com/GITenberg/Nazarin_73322/tar.gz/master"),
    ("Obras escogidas (Becquer)", "https://codeload.github.com/GITenberg/Obras-escogidas_53552/tar.gz/master"),
    ("Don Quijote (Cervantes)", "https://codeload.github.com/GITenberg/Don-Quijote_2000/tar.gz/master"),
]


def decodificar_bytes(raw: bytes) -> str:
    try:
        return raw.decode("utf-8-sig")
    except UnicodeDecodeError:
        return raw.decode("iso-8859-1")


def limpiar_gutenberg(txt: str) -> str:
    s = re.search(r"\*\*\* ?START OF (THE|THIS) PROJECT GUTENBERG.*?\*\*\*", txt, flags=re.I | re.S)
    if s:
        txt = txt[s.end():]
    e = re.search(r"\*\*\* ?END OF (THE|THIS) PROJECT GUTENBERG", txt, flags=re.I)
    if e:
        txt = txt[:e.start()]
    txt = txt.replace("\r\n", "\n").replace("\r", "\n")
    # Quitar notas de produccion iniciales en ingles si existen
    lineas = txt.split("\n")
    ini = 0
    for idx, ln in enumerate(lineas[:40]):
        low = ln.lower()
        if any(k in low for k in ("produced by", "online distributed", "html file by", "transcriber")):
            ini = idx + 1
    txt = "\n".join(lineas[ini:])
    # Normalizar espacios sin alterar saltos de parrafo ni puntuacion espanola
    txt = re.sub(r"[ \t]+", " ", txt)
    txt = re.sub(r" *\n *", "\n", txt)
    txt = re.sub(r"\n{3,}", "\n\n", txt)
    return txt.strip() + "\n"


def descargar_libro(nombre: str, url: str) -> str:
    req = urllib.request.Request(url, headers={"User-Agent": "cortex-lm/2.0"})
    with urllib.request.urlopen(req, timeout=60) as resp:
        data = resp.read()
    tf = tarfile.open(fileobj=io.BytesIO(data), mode="r:gz")
    candidatos = []
    for m in tf.getmembers():
        if m.isfile() and m.name.endswith(".txt") and "/old/" not in m.name and "readme" not in m.name.lower():
            candidatos.append(m)
    if not candidatos:
        for m in tf.getmembers():
            if m.isfile() and m.name.endswith(".txt") and m.size > 50_000:
                candidatos.append(m)
    if not candidatos:
        raise RuntimeError(f"No se encontro .txt en {nombre}")
    # Preferir UTF-8 (-0.txt) si existe; si no, el mayor .txt
    candidatos.sort(key=lambda m: ("-0.txt" in m.name, m.size), reverse=True)
    elegido = candidatos[0]
    raw = tf.extractfile(elegido).read()
    limpio = limpiar_gutenberg(decodificar_bytes(raw))
    win = [limpio[i:i + 80] for i in range(0, max(1, len(limpio) - 80), 97)]
    uniq = 100.0 * len(set(win)) / max(1, len(win))
    print(f"  [OK] {nombre:<28} | {len(limpio):>9,} chars | alfabeto={len(set(limpio)):>3} | ventanas unicas={uniq:.1f}%")
    return limpio


def extraer_diccionario(textos):
    palabras = set()
    patron = re.compile(r"[a-záéíóúüñ]{2,}", flags=re.I)
    for t in textos:
        for m in patron.finditer(t):
            palabras.add(m.group(0).lower())
    return sorted(palabras)


def main():
    base_dir = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
    data_dir = os.path.join(base_dir, "data")
    os.makedirs(data_dir, exist_ok=True)

    print("Descargando obras literarias en espanol desde GITenberg...")
    modernos = []
    quijote = ""
    for nombre, url in LIBROS:
        txt = descargar_libro(nombre, url)
        if "Quijote" in nombre:
            quijote = txt
        else:
            modernos.append(txt)

    texto_moderno = "\n\n".join(modernos)
    ruta_mod = os.path.join(data_dir, "libro_moderno.txt")
    ruta_qui = os.path.join(data_dir, "libro_quijote.txt")
    ruta_amp = os.path.join(data_dir, "corpus_amplio.txt")
    ruta_dic = os.path.join(data_dir, "dic_amplio.txt")

    with open(ruta_mod, "w", encoding="utf-8") as f:
        f.write(texto_moderno)
    with open(ruta_qui, "w", encoding="utf-8") as f:
        f.write(quijote)

    # Corpus amplio equilibrado (~800 KB espanol moderno + ~200 KB Cervantes, sin repeticion)
    # Cortado en limite de parrafo limpio; el 15% final de Galdos queda intacto al final
    # para que cualquier corte cronologico 80/20 o 90/10 evalue sobre prosa moderna jamas vista.
    corte_q = quijote.find("\n\n", 220_000)
    trozo_q = quijote[:corte_q if corte_q != -1 else 220_000]
    corte_m = texto_moderno.find("\n\n", 780_000)
    trozo_m = texto_moderno[:corte_m if corte_m != -1 else 780_000]
    corpus_amplio = trozo_q + "\n\n" + trozo_m + "\n"
    with open(ruta_amp, "w", encoding="utf-8") as f:
        f.write(corpus_amplio)

    dic = extraer_diccionario([texto_moderno, quijote])
    with open(ruta_dic, "w", encoding="utf-8") as f:
        f.write("\n".join(dic) + "\n")

    print("\nResumen de archivos generados en cortex-lm/data/ (excluidos de git):")
    print(f"  libro_moderno.txt : {len(texto_moderno):>10,} chars ({os.path.getsize(ruta_mod)/1024:.0f} KB)")
    print(f"  libro_quijote.txt : {len(quijote):>10,} chars ({os.path.getsize(ruta_qui)/1024:.0f} KB)")
    print(f"  corpus_amplio.txt : {len(corpus_amplio):>10,} chars ({os.path.getsize(ruta_amp)/1024:.0f} KB)")
    print(f"  dic_amplio.txt    : {len(dic):>10,} palabras reales unicas")


if __name__ == "__main__":
    sys.exit(main())
