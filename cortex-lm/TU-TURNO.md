# TU TURNO — lo que tú vas a hacer

No te expliquen cosas. Te dejan una máquina. Esta es la máquina.

---

## 1. Lo que tienes que hacer (tres comandos, en orden)

En la carpeta `cortex-lm/`:

```bash
# A. La prueba completa: compila, mide memoria, mide el banco en 5 semillas.
#    tarda ~20 min. Lanzala y espera.
./tools/verifica.sh

# B. Tu palanca: metes tus números y te devuelve la memoria y el bits.
#    Los 4 números son la poblacion lenta, la que guarda el pasado.
./tools/experimenta.sh 300 6 200 0.8

# C. Cualquier otro numero que se te ocurra, igual de facil:
./tools/experimenta.sh 288 6 200 0.8
./tools/experimenta.sh 320 6 190 0.75
./tools/experimenta.sh 256 6 220 0.85
```

Si quieres una prueba mas corta: `./tools/verifica.sh 2400 "1234 7"` (2 semillas, ~8 min).

Nada de eso es teoria. Los numeros que te devuelve son los mismos que yo he medido.

---

## 2. Como lees lo que te devuelve

Hay dos cosas distintas, y no se confunden:

**A. Memoria del estado** (`diag_mem`, seccion 1 y el bloque de `experimenta.sh`):

```
BASE     lag 0: 100.0%   lag 1: 22.8%
JER 300  lag 0: 100.0%   lag 1: 44.0%
```

- `lag 0` = ¿el estado sabe que letra hay **ahora**? Siempre ~100 %. Es el presente.
- `lag 1` = ¿el estado sabe que letra havia **justo antes**? Esa es la memoria.
- BASE marca 22.8 %. JER 300 marca 44.0 %. El estado ve el presente al 100 % y, sin memoria, **olvida** la letra anterior. Con memoria, la recuerda el doble de veces.

**B. Banco normalizado** (`bench_bio`, seccion 2):

```
seed 1234  BASE 2.87   JER300 2.87
seed 7     BASE 2.86   JER300 2.85
...
```

Es **bits por caracter** en texto que el motor **nunca vi**. Mas abajo = mejor. Es la unica medida que importa al final.

---

## 3. El criterio (lo pactado, sin torcerlo)

Tres cosas, todas a la vez:

| Regla | Lo que significa |
|---|---|
| `lag1 >= 35 %` | La memoria existe y es real |
| `lag0 >= 95 %` | El presente no se sacrifica por ella |
| `holdout <= BASE` | Esa memoria **paga su alquiler**: no sube el bits/caracter |

**JER 300 PASA las tres.** Eso esta hecho y verificado, en el corpus grande (45.7 k chars) y en 5 semillas.

---

## 4. El problema que si esta abierto (este es el que te queda)

Mira los numeros de arriba juntos:

- JER 300 tiene **1,7x mas memoria** que BASE (lag1 44 % vs 23 %).
- Pero en holdout son **identicos** (2.87 vs 2.87).

Es decir: **el motor recuerda la letra anterior el doble de veces y no gana ni un milesimo de bits.** La memoria esta ahi, mide muchisimo, y el banco no se entera.

Eso tiene una causa hipotetica y una manera de comprobarla sin specular. Yo ya medi la palanca, no la estoy suponiendo: `slow_in_gain` es la corriente que entra al canal lento, y al variarla la memoria se mueve **continuamente**, sin saltos:

```
slow_in  0.8   -> lag1 44.0 %     (la memoria, como en JER 300)
slow_in  0.4   -> lag1 27.2 %
slow_in  0.2   -> lag1 23.1 %
slow_in  0.0   -> lag1 22.4 %     (se fue; BASE es 22.8 %)
```

Con `slow_in = 0` la memoria desaparece y el estado vuelve a ser BASE. Entonces puedes hacer la prueba **exacta**: misma red, mismo aprendizaje, unica diferencia la memoria.

```bash
./tools/experimenta.sh 300 6 200 0.8 data/corpus.txt 2400 "1234"   # con memoria
./tools/experimenta.sh 300 6 200 0.0 data/corpus.txt 2400 "1234"   # sin memoria
```

Compara los dos bits de holdout con BASE (2.87 a esa semilla):

- **si el bits sin memoria sube** -> la memoria estorba. Esta pisando el presente.
- **si no se mueve un milesimo** -> la memoria no entra por ningun lado que el banco mire. Esta en el estado, mide muchisimo, y nadie la lee para predecir.

En ambos casos no se trata de "faltan datos" ni de "otro alfabeto": se trata de **un cable que no llega**. Esa es la siguiente medida. Dos comandos, un numero, una conclusión.

---

## 5. Que no hacer

- No cambies los numeros del criterio porque el resultado no te gusta. El criterio esta pactado.
- No mezcles las particiones. El holdout es el último 20 % del texto, **en orden**, siempre.
- No digas "funciona" o "no funciona" sin el banco. El `diag_mem` es un termometro; el bits es la enfermedad.
- No subas corpus a GitHub (viene en `.gitignore`). Para tenerlo otra vez: `python3 data/build_corpus_large.py`.
