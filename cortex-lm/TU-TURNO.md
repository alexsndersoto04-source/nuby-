# TU TURNO — estado real del proyecto

No es una lista de tareas. Es dónde estamos, qué se provó y qué queda.

---

## 1. EL PROBLEMA QUE TU TRAJISTE: RESUELTO

Tú dijiste: el estado ve el presente al 100 % pero olvida la letra anterior.

Lo que se hizo: una poblacion lenta de neuronas dentro de tu red (sin cambiar
la arquitectura, sin gradientes). Es la que conserva el pasado.

Lo que mide `diag_mem`, en el corpus grande:

```
BASE     lag 0: 100.0%   lag 1:  22.8%    <- olvida la anterior el 77%
JER 300  lag 0: 100.0%   lag 1:  44.0%    <- olvida la anterior el 56%
```

**Criterio pactado: PASA.** lag1 >= 35 % (44 %), lag0 >= 95 % (100 %), y el
texto que escribe no se empeora (2.87 = 2.87 en BASE). Eso esta hecho.

---

## 2. LA SORPRESA: ESA MEMORIA NO SIRVE PARA ESCRIBIR

Se metio tres veces, igualito:

```
con memoria  (slow_in 0.8):  2.87 bits/caracter
sin memoria  (slow_in 0.0):  2.87 bits/caracter
BASE:                        2.87 bits/caracter
```

Quitar la memoria del todo no cambia NI UNA CENTESIMA. Y se comprobo que la
memoria llega al lector: la poblacion lenta tiene 722/724 dimensiones vivas
tras la normalizacion (99.7 %), asi que no la mata nadie.

Conclusión: la memoria esta en el estado, se mide, llega al lector, y el motor
no la consume para escribir. Es informacion que se almacena y nadie la lee.

---

## 3. LO QUE SÍ FUNCIONA: EL ESTADO ANTERIOR

Tu lector (`tap_features`) puede recibir varias fotos del estado pasado.
Antes se le daba solo una. Se metio a dar dos:

```
g_taps=1 (lo que se usaba):  BASE 2.87   JER300 2.87
g_taps=2 (ve el anterior):   BASE 2.83   JER300 2.83
g_taps=3:                    BASE 2.83   JER300 2.85
g_taps=4:                    BASE 2.85
g_taps=5:                    BASE 2.87
g_taps=6:                    BASE 2.89   <- ya peor que la base
```

Hay un plato en 2 y 3. Se confirmo en **5 semillas**, con el extraction corregido (bench_bio corre
BASE siempre como referencia, y antes se tomaban las dos lineas de holdout
juntas; ya se filtra por nombre de config):

```
BASE:      2.87  2.86  2.87  2.87  2.88   (promedio 2.870)
BASE t2:   2.83  2.83  2.83  2.84  2.85   (promedio 2.836)
JER300:    2.87  2.85  2.87  2.87  2.88   (promedio 2.868)
```

**RESULTADO: PASA** en los dos criterios:
- memoria: lag1 44 % >= 35 %, lag0 100 % >= 95 %, holdout JER300 2.868 <= BASE 2.870.
- funcional: BASE t2 2.836 <= BASE 2.870.

**Eso es una mejora real en tu arquitectura: 2.870 -> 2.836 bits/caracter.**
Y la memoria, bien medida, queda en 2.868: ni ayuda ni perjudica.

### 3b. LAS PASADAS DE ENTRENAMIENTO: EL EMPULSION MAS GRANDE

`EP` (epocas) estaba fijo en 10. Se barrero con g_taps=2:

```
EP=10: 2.83    EP=20: 2.73    EP=40: 2.66
EP=15: 2.77    EP=30: 2.69    EP=50: 2.65    EP=60: 2.64
```

Baja cada vez menos: se esta plantando. El holdout es el tramo NO VISTO
(ultimo 20 %, en orden), asi que no es sobreentreno.

**De 2.87 a 2.64 con dos cambios: que el lector vea el estado anterior y
que se entrene mas tiempo.** Y la memoria interna sigue sin mover nada.

Lo que ya se descartó, porque se midio:
- `g_feat=1` (solo los 2 estados, sin fatiga): 2.85 — la fatiga vale.
- `g_feat=0` (solo fatiga): 2.98.
- `g_hidden=8` / `16` (lector no lineal): 4.45 / 4.40 — lo destroza.
- La longitud de calibracion (1500 a 8000): 2.83 siempre, no mueve nada.

---

## 4. QUÉ HICEMOS AHORA

Se esta barrendo `ticks_por_caracter` con `g_taps=2` (8 ya da 2.83; se prueban
12, 16 y 24). Porque `ticks_per_char` era la palanca mas fuerte que ya se
habia encontrado, y ahora el lector ve mas, asi que puede valer mas.

## 5. TU COMPRO BUENO, EN UN SOLO COMANDO

```bash
cd cortex-lm

# La prueba completa: memoria + banco en 5 semillas + veredicto calculado.
./tools/verifica.sh

# Tu palanca: metes los 4 numeros de la poblacion lenta y cuantas fotos
# del pasado le das al lector.
./tools/experimenta.sh 300 6 200 0.8
./tools/experimenta.sh 300 6 200 0.8 data/corpus.txt 2400 "1234 7 99" 2
```

## 6. QUE NO HACER

- No cambies el criterio porque el resultado no te gusta. Está pactado.
- No mezcles particiones: el holdout es el último 20 % del texto, en orden.
- No digas "funciona" sin el banco. `diag_mem` es un termometro; el bits es
  la enfermedad.
- No subas corpus a GitHub. Para tenerlo otra vez:
  `python3 data/build_corpus_large.py`
