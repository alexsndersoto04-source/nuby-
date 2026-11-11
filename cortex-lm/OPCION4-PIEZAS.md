# ACTA — Piezas (BPE) en vez de letras: resultado negativo

Fecha: 2026-11-04. Rama `arena/c7deda7f-ask`.

## La hipótesis

Con letras, el contador de 3 caracteres es un rival durísimo y la red solo aporta
0,036 bpc. Prediciendo piezas (" que", "cion", "mente") el contexto útil se
reparte en más símbolos y la red debería tener sitio para aportar algo que el
contador no tenga.

## Qué se construyó

- `tools/bpe.cpp` — BPE de verdad: se parte de bytes y se fusionan los pares más
  frecuentes. Tres tamaños probados: 700, 1.500 y 2.500 fusiones (2,98 / 3,60 /
  4,10 bytes por pieza).
- `tools/bench_bpe.cpp` — reservorio + lector local sobre piezas, y contadores de
  1, 2 y 3 piezas como suelo. **Todo en bits por BYTE de texto original** para
  que sea comparable con lo de antes (que era por carácter = por byte).

Particiones: mismas que siempre (80 % inicial para train, tramo siguiente para
test, en orden).

## Resultado: las piezas pierden contra las letras

| sistema | bits/byte |
|---|---|
| **letras: red + contador de 3** (lo de antes) | **2,361** |
| piezas 1.500 fusiones: red sola, train 70 k | 2,709 |
| piezas 1.500 fusiones: contador de 1 pieza | 2,712 |
| piezas 700 fusiones: red sola, train 85 k | 2,842 |
| piezas 700 fusiones: contador de 1 pieza | 2,597 |
| piezas 2.500 fusiones: red sola, train 62 k | 2,603 |

El mejor número de piezas en el banco de red es 2,603–2,709, claramente por
encima de 2,361. **No hay mejora: hay empeoramiento de entre 0,24 y 0,35 bits
por byte.**

## Por qué falla (medido, no supuesto)

**1. No es falta de datos.** Comparación con los MISMOS 60 k de texto:

| mismas 60 k de texto | bits/byte |
|---|---|
| letras: red + contador | 2,551 |
| piezas: red sola | 2,940 |

Con la misma cantidad de texto, las letras son mucho mejores. Así que comprimir
el texto en piezas no compensa lo que se pierde.

**2. Es que la red se atasca con muchas clases.** Con 2.256 clases y 1.500
neuronas, el lector tiene 3,4 M de parámetros para 20–60 k muestras: no converge.
Se probó muestreo de negativos para abaratarlo y sale **5,004 bits/byte**, mucho
peor. Descartado.

**3. Los contadores sobre piezas memorizan.** Con 2.256 símbolos, un contexto de
2–3 piezas se ve una sola vez y se queda con esa única salida: subir el orden
empeora (2,712 → 2,820 → 2,836). Se probó exigir un mínimo de observaciones para
fiarse del contexto: sube el orden 3 de 2,877 a 2,816, sigue peor que orden 1.

## Veredicto

**La línea de piezas (BPE) no funciona con este corpus y esta arquitectura.** No
se adopta. Se queda documentada para no repetirla. El mejor sistema del proyecto
sigue siendo letras + reservorio + contador de 3 caracteres: **2,361 bits/byte**.

## Lo que esto dice del proyecto

Juntando las dos actas anteriores con esta, el mapa está claro:

- La red local **sí aporta** algo que un contador no tiene (demostrado con el
  peso adaptable, 5 semillas, desviación 0,001).
- Ese algo es **pequeño**: 0,036 bpc de un total de 0,93 de mejora.
- El techo no está en la representación ni en la unidad (letra o pieza): está en
  que **una red de conectividad fija y un lector lineal tienen un límite** que ya
  se ha tocado por los tres lados (más neuronas, más etapas, más símbolos).

## Reproducir

```
cd cortex-lm
g++ -O2 -std=c++17 tools/bpe.cpp -o bin/bpe
g++ -O3 -march=native -std=c++17 tools/bench_bpe.cpp -o bin/bench_bpe

bin/bpe 1500 3          # parte el texto en piezas
bin/bench_bpe 70000 1500 0.35 0.9 1234 0.05 2 3
```
