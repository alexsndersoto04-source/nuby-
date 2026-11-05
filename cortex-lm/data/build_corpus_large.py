#!/usr/bin/env python3
"""Construye un corpus LARGO (~35k chars) de Don Quijote para validacion Kaggle.

Lee el texto base (build_corpus.py) y los chunks 8-11, los limpia y los
concatena. Genera corpus.txt y dic.txt.
"""
import re, os

HERE = os.path.dirname(os.path.abspath(__file__))

def lee(name):
    with open(os.path.join(HERE, name), encoding="utf-8") as f:
        return f.read()

def limpia(t):
    t = t.replace("\r\n", "\n").replace("\r", "\n")
    # colapsar lineas vacias
    t = re.sub(r"\n{3,}", "\n\n", t)
    return t

# texto base (capítulos I-II, ~14k chars)
base = limpia(lee("build_corpus.py"))
# extraer solo la parte del texto (despues del docstring y TEXT = """)
m = re.search(r'TEXT\s*=\s*"""(.+?)"""', base, re.DOTALL)
if not m:
    raise SystemExit("no se encontro TEXT en build_corpus.py")
texto = limpia(m.group(1))

# chunks adicionales
for chunk in ["chunk8.txt", "chunk9.txt", "chunk10.txt", "chunk11.txt"]:
    texto += "\n\n" + limpia(lee(chunk))

# escribir corpus
with open(os.path.join(HERE, "corpus.txt"), "w", encoding="utf-8") as f:
    f.write(texto)

# diccionario
words = re.findall(r"[a-záéíóúüñ]+", texto.lower())
seen = set(); out = []
for w in words:
    if len(w) >= 2 and w not in seen:
        seen.add(w); out.append(w)
with open(os.path.join(HERE, "dic.txt"), "w", encoding="utf-8") as f:
    f.write("\n".join(out) + "\n")

# stats
n = len(texto)
alpha = len(set(texto))
print(f"corpus: {n} caracteres | alfabeto: {alpha} simbolos")
print(f"diccionario: {len(out)} palabras")
ventana = 80
muestras = [texto[i:i+ventana] for i in range(0, max(1, n-ventana), 97)]
unicas = len(set(muestras))
ratio = unicas / max(1, len(muestras))
print(f"ventanas unicas: {unicas}/{len(muestras)} = {100*ratio:.1f}%")
