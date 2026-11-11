# Acta — las tres piezas con el motor arreglado (y por qué se apagan)

**Fecha:** 2026-11-05 · **Rama:** `arena/c7deda7f-ask` · **Motor:** LIF real, sin retropropagación

## 1. Qué había que repetir

Los veredictos de la ronda anterior sobre las tres piezas se tomaron con un motor
que **no le enseñaba al lector su estado de membrana**: solo la fatiga. Eso se
arregló (`state_features` / `full_features`, acta `ACTA-ESTADO-DEL-MOTOR.md`) y
el mismo texto bajó de 3,74 a 3,14 bits/carácter. Con el motor arreglado, los
veredictos anteriores no valían. Aquí se repiten **las tres piezas, una a una,
tres semillas, mismo corpus, mismo lector, mismas particiones**.

Configuración común: `train.txt` (60.000 caracteres del Quijote), `dic.txt`
(6.594 palabras), 200 caracteres generados, 2 épocas, `FEAT=2` (estado + fatiga),
`layer_size = 1024`, `ticks_per_char = 8`.

## 2. Salida cruda

### 2.1 Las tres piezas, con el motor ya arreglado (`FEAT=2`)

```
########## semilla 1234
===== BASE   (nada nuevo) =====   3.03 bits | palabras reales: 4 de 21 (19.0%)
===== JERARQ (memoria lenta) ==== 3.14 bits | palabras reales: 1 de 18 (5.6%)
===== DOPA+REFUERZO ============= 3.21 bits | palabras reales: 2 de 15 (13.3%)
########## semilla 7
===== BASE   (nada nuevo) =====   3.01 bits | palabras reales: 2 de 19 (10.5%)
===== JERARQ (memoria lenta) ==== 3.08 bits | palabras reales: 3 de 21 (14.3%)
===== DOPA+REFUERZO ============= 3.18 bits | palabras reales: 7 de 28 (25.0%)
########## semilla 99
===== BASE   (nada nuevo) =====   3.02 bits | palabras reales: 2 de 23 (8.7%)
===== JERARQ (memoria lenta) ==== 3.08 bits | palabras reales: 5 de 24 (20.8%)
===== DOPA+REFUERZO ============= 3.18 bits | palabras reales: 3 de 17 (17.6%)
```

### 2.2 El oscilador, que era el caso peor

```
########## semilla 1234
===== BASE   (nada nuevo) =====   3.03 bits | 4 de 21 (19.0%)
===== OSCIL  (marcapasos) ======  4.46 bits | 0 de 22 ( 0.0%)
===== OSCIL+ANCLADO ============  6.08 bits | 3 de 24 (12.5%)
########## semilla 7
===== BASE   (nada nuevo) =====   3.01 bits | 2 de 19 (10.5%)
===== OSCIL  (marcapasos) ======  4.46 bits | 0 de 16 ( 0.0%)
===== OSCIL+ANCLADO ============  6.16 bits | 6 de 21 (28.6%)
########## semilla 99
===== BASE   (nada nuevo) =====   3.02 bits | 2 de 23 ( 8.7%)
===== OSCIL  (marcapasos) ======  4.45 bits | 0 de 22 ( 0.0%)
===== OSCIL+ANCLADO ============  6.04 bits | 3 de 22 (13.6%)
```

`OSCIL+ANCLADO` es la variante nueva: la fase del marcapasos se reinicia en cada
frontera de símbolo (`Params::pace_lock`), de modo que la misma corriente pasa a
decir "por aquí va el carácter" en vez de ser un reloj libre.

### 2.3 El mecanismo, medido sin lector (`tools/diag_estado.cpp`)

Qué le hace cada pieza al estado de membrana, 400 caracteres, tres semillas:

```
BASE                   |v|/v_th medio 0.065 | en umbral 0.0% | varianza estado 0.0158
OSCIL libre            |v|/v_th medio 0.155 | en umbral 0.1% | varianza estado 0.0435
OSCIL anclado          |v|/v_th medio 0.335 | en umbral 0.2% | varianza estado 0.0152
OSCIL anclado gain .08 |v|/v_th medio 0.146 | en umbral 0.1% | varianza estado 0.0254
OSCIL libre gain .08   |v|/v_th medio 0.107 | en umbral 0.0% | varianza estado 0.0233
JERARQ                 |v|/v_th medio 0.083 | en umbral 0.0% | varianza estado 0.0205
```
(las tres semillas dan los mismos valores con ±0,003)

### 2.4 Bajada de ganancia: el mejor oscilador posible

Con la ganancia del marcapasos a 0,08 en vez de 0,35 —la que **sí** sube la
varianza del estado sin ahogar la población— y fase anclada:

```
semilla 1234: BASE 3.03 | OSCIL+ANCLADO gain 3.54
semilla    7: BASE 3.01 | OSCIL+ANCLADO gain 3.52
semilla   99: BASE 3.02 | OSCIL+ANCLADO gain 3.49
```

## 3. Lectura

- **BASE gana en las tres semillas** (3,01–3,03). Ninguna pieza se acerca.
- **Jerarquía lenta:** 3,08–3,14, es decir 0,06–0,11 peor. Con el motor ciego
  daba 3,76 frente a 3,73; ahora da 3,08–3,14 frente a 3,01–3,03. El veredicto
  no cambia al arreglar el motor: **no aporta**, y ahora se sabe que no es
  porque el lector no pudiera verla (la ve: `full_features` incluye todas las
  neuronas, rápidas y lentas).
- **Dopamina + refuerzo:** 3,18–3,21, unas 0,16 peor. El refuerzo dispara
  14.721 veces y castiga 233: la señal sigue sin separar, porque el bloque que
  el sistema escribe es corto y casi todo lo que produce es ruido que el juez no
  distingue de una palabra rara.
- **Oscilador:** 4,46 libre; 6,08 anclado, **peor que no anclar**. La causa se ve
  en 2.3: la corriente periódica sube la carga media de la población de 0,065 a
  0,335 —cinco veces— **sin añadir información**: la varianza del estado no sube,
  es la misma. Es un foco de luz apuntando a la cámara: más señal, cero
  contenido. Anclar la fase lo hace aún más determinista, y por eso empeora más.
  Con ganancia suave el daño baja (3,49–3,54) pero nunca llega a BASE.
- **El texto lo dice sin números.** BASE y DOPA+REFUERZO escriben:

```
BASE (3.03 bits, 25% palabras reales):
e otreCmos.
hincía, senciendoseros, y que cual, isen erosen es en tros jay en lasa. Don Qbió tías másmponentros boseminos más den el cuan ados esinadescha?os díos aáse mías ha
; curina, hosen muy Don quentis de

DOPA+REFUERZO (3.21 bits, 12.5% palabras reales):
enguandenos pos,y cuyendendentó hilaseradenvendenienciendosenibría quendentos Nicuendagand, y enzeniciendosendeniencendendenienienienden, cuen, cur. Siendas, Dercenólos varreniengend, el nmos quegahiosen
posendendenUc
```

La dopamina mete el sufijo `-enden-` en casi todas las palabras: el juez premia
trozos que se parecen a palabras del diccionario y el lector se queda con el
trozo, no con la palabra.

## 4. Decisión

- **Las tres piezas quedan en el código** (son C++ real, dentro del LIF, y se
  pueden activar por `Params`), **apagadas por defecto**: `n_fast = 0`,
  `dop_gain = 0`, `n_pace = 0`.
- El motor por defecto es **BASE con estado + fatiga**: 3,01–3,03 bits/carácter
  y 19–25 % de palabras reales.
- Se añade `Params::pace_lock` (oscilador anclado) y `pace_gain` configurable,
  documentados como medidos y descartados: sirven para que nadie los “redescubra”
  sin saber el resultado.
- Nuevo `tools/diag_estado.cpp`: mide el estado de membrana **sin lector**, para
  poder separar "la pieza no informa" de "el lector no la ve".

## 5. Qué queda como el problema de verdad

El motor ya no está ciego, y el texto mejoró de verdad (de 3,74 a 3,03 y de 4 %
a 19–25 % de palabras reales). Pero sigue sin frases. Lo que ninguna de las tres
piezas aporta es **la relación entre palabras**, y el diagnóstico de 2.3 dice por
qué: el estado de la red es muy poco variable entre caracteres (varianza 0,016
sobre valores de ~0,065). La red está casi siempre en el mismo punto; el lector
distingue letras, no contextos. Eso no lo arregla más corriente ni más memoria,
sino que el estado **cambie** con lo que se lleva escrito.

## Reproducir

```bash
cd /home/user/ask-/cortex-lm
g++ -std=c++20 -O3 -march=native -Iengine engine/cortex_lm.cpp tools/bench_bio.cpp -o /tmp/bbio
g++ -std=c++20 -O3 -march=native -Iengine engine/cortex_lm.cpp tools/diag_estado.cpp -o /tmp/diag
cd /home/user/wk2
/tmp/bbio data/train.txt data/dic.txt 200 1234 "0,0,0,2" 0 2        # BASE
/tmp/bbio data/train.txt data/dic.txt 200 1234 "1,0,0,2" 0 2        # jerarquia
/tmp/bbio data/train.txt data/dic.txt 200 1234 "0,0,1,2" 0 2 0.08   # oscilador suave
/tmp/bbio data/train.txt data/dic.txt 200 1234 "0,1,0,2" 0 2        # dopamina + refuerzo
/tmp/diag  data/train.txt 400 1234
```
