# Estado del motor CORTEX-LM

Acta de auditoría. Lo que se midió, lo que se encontró y lo que sigue abierto.

---

## Qué se hizo

Auditoría a fondo del motor, y ante la evidencia de que la dinámica no
controlaba la entrada, **reescritura completa del núcleo** (`cortex_lm.hpp` y
`cortex_lm.cpp`). El motor anterior se conserva en el historial de git.

Se añadió el comando `audit`, que verifica cinco propiedades estructurales y
falla ruidosamente si alguna deja de cumplirse. Es la red de seguridad que
faltaba: los errores de esta sesión habrían salido en segundos.

---

## Los hallazgos de la auditoría

### 1. La red era autónoma: la entrada no la gobernaba

Instrumentando el motor anterior:

| sonda | resultado |
|---|---|
| disparos de un símbolo aislado | **0** |
| disparos por símbolo en secuencia | 0,7 al principio → **162** al final |
| neuronas activas del total | **168 de 256**, permanentemente |
| variar el historial vs variar el símbolo | **0,081 vs 0,089** (la entrada apenas manda) |

La actividad crecía sin punto fijo y la respuesta dependía del historial tanto
como del símbolo. La red era un oscilador al que se le susurraba.

### 2. Causa: pesos sin escala y una compuerta que imponía el régimen

- Los pesos entrantes de una neurona sumaban **0,80 con un umbral de 0,60**.
- La fuga de membrana era 1,7 % por tick: nunca frenaba nada.
- La única limitación era un k-WTA, un número fijo de ganadores por tick.

Y un hallazgo operativo: **la red tarda ~200 símbolos en alcanzar su régimen**.
Todas las mediciones anteriores usaban los primeros 400 símbolos, es decir,
medían el arranque.

### 3. Bugs de memoria

- **Desbordamiento del readout**: el array se dimensionaba con la vista de
  historial y se leía con la vista de la red (más ancha). Producía NaN desde el
  segundo símbolo. Detectado con `-fsanitize=address`.
- **Filas solapadas** en el readout por usar tamaños distintos al leer y al
  escribir.
- **La fatiga solo decaía en las neuronas de la lista activa**: al vaciarse
  quedaba congelada en su valor alto y la red no podía recuperarse nunca.
- **El sesgo homeostático se aplicaba después de la integración**, con lo que
  solo se sumaba una vez y permanecía para siempre.
- **La homeostasis apagaba la entrada**: con objetivo de tasa fuerte castigaba
  a las neuronas sensoriales hasta dejar 2 dimensiones vivas de 1025.

### 4. El readout divergía

Adam con paso unitario sobre 1024 dimensiones y sin recorte: los logits
llegaron a **±455** en 100 pasos, el softmax se saturó y el BPC se disparó a 27.
Corregido con acotado por elemento, sesgo acotado y suelo de desviación.

---

## La dinámica nueva

```
v(t) = v_reset + (v(t-1) - v_reset)·α + I_sens + I_rec − I_inh + sesgo
si v ≥ v_th + θ :  dispara,  θ += theta_step
θ *= theta_decay   en cada tick
```

Tres decisiones, cada una corrigiendo un fallo medido:

**Pesos derivados, no elegidos.** Se fijan para que la corriente recurrente
total, con la fracción activa típica, valga `w_scale · v_th`. Antes se elegían a
mano y sumaban más que el umbral.

**La entrada manda.** `input_gain > v_th`: un símbolo solo hace disparar a su
población. Verificado en `audit` (test 2).

**Freno proporcional a la actividad**, no un número fijo de ganadores. Cada
disparo resta a una muestra al azar de la capa. Cuanto más activa, más freno:
eso da un punto fijo.

**Memoria = fatiga.** La neurona que dispara sube su umbral y tarda en
recuperarse. El patrón de fatiga codifica lo que acaba de ocurrir. Es local, no
necesita gradientes, y es el mecanismo de memoria de trabajo de la corteza.

---

## Resultados medidos

### Auditoría estructural: todo OK

```
[1] corriente recurrente 0.35 x umbral   ok
[2] símbolo aislado: 89 disparos         ok
[3] régimen estable (deriva x0.75)       ok
[4] fatiga distingue símbolos            ok
[5] memoria de trabajo 82.5 %            ok
```

### Memoria de trabajo

Tarea: 40 claves, 8 símbolos de relleno, recordar la clave. Azar 2,5 %.

| memoria | acierto |
|---|---|
| **motor nuevo, banco de 40 claves** | **82,5 %** |
| motor nuevo, protocolo de reposo | 93,5 % |
| `bench` integrado (100 % en 60 épocas) | 100 % |

Es memoria de trabajo real, sostenida por fatiga local.

### Texto: la cifra de 0,409 bpc era un artefacto del corpus

Antes de celebrar cualquier número hay que mirar el corpus. `data/spanish_small.txt`
se generaba repitiendo el mismo fragmento del Quijote 40 veces con cuatro
variantes de superficie. Medido:

```
subcadenas de 80 caracteres: 347 únicas de 15.592 = 2,2 %
```

Es decir, el 98 % del "test" ya estaba en el entrenamiento. Cualquier BPC
medido así es memorización, no predicción. **La tabla siguiente queda
invalidada como medida de calidad** (el 0,409 de la vista de historial sobre
todo):

| vista | bpc | estado |
|---|---|---|
| historial de 4 símbolos | 0,409 | **inválido, fuga de test** |
| tasa de disparo de la red | 4,370 | inválido además por el bug de inhibición |
| fatiga de la red | 5,301 | inválido además por el bug de inhibición |
| bigrama trivial | 2,893 | referencia, estimado con fuga |

Se corrigió el generador: ahora produce dos ficheros disjuntos,
`spanish_small.txt` (entrenamiento) y `spanish_holdout.txt` (test, variantes de
superficie que no aparecen en entrenamiento). Se añadió `tools/bench.cpp` con
ese protocolo.

---

## Ronda de corrección: dos bugs estructurales más

### Bug 3. La cola de eventos no se vaciaba al drenarla

`DelayQueue::drain()` devolvía el cubo de ese tick **sin vaciarlo**. Como los
cubos se reutilizan en anillo, los eventos de un tick se reentregaban cada vez
que el cabezal volvía a ese cubo, para siempre. Consecuencias:

- Con `w_scale = 0` (ver bug 4) esos eventos sumaban un peso de cero, así que
  el fallo era **invisible**: los cinco tests de `audit` pasaban.
- Con recurrencia real, la capa se saturaba a **1024 de 1024 neuronas
  disparando en cada tick** (4096 disparos por símbolo) y la cola crecía hasta
  agotar la memoria del proceso.
- Explica también el OOM que mató las primeras pruebas con recurrencia.

Arreglo: vaciar el cubo tras procesarlo (`arrived.clear()`).

### Bug 4. La recurrencia estaba apagada: `w_scale = 0`

`Params::w_scale` valía **0,0** por defecto, con el comentario "0 = sin
recurrencia (línea base sensorial)". Las 52.173 sinapsis recurrentes se creaban
con **peso exactamente cero**. La red no era una red recurrente de pulsos: era
1024 integradores independientes, cada uno con su parche sensorial fijo y su
fatiga.

Esto explica el techo de bigrama medido antes: el estado no podía contener
historia porque nada conectaba unas neuronas con otras. Y explica por qué la
plasticidad nunca cambiaba nada: con `w_init = 0`, el límite
`w0 * (1 ± w_frac_max)` la dejaba clavada en cero.

Medido con el motor arreglado y el protocolo honesto (8000 símbolos de
entrenamiento, lector softmax, K=64 dimensiones por retardo × 8 retardos):

| `w_scale` | bpc | acierto |
|---|---|---|
| 0,00 (sin recurrencia) | 1,939 | 62,5 % |
| 0,05 | 1,942 | 62,1 % |
| 0,20 | 1,886 | 65,9 % |
| **0,30** | **1,831** | **68,5 %** |
| 0,40 | 1,925 | 63,5 % |
| 0,50 | 2,651 | 45,0 % |

La recurrencia aporta **0,11 bpc** con un óptimo claro en 0,30, que es el nuevo
valor por defecto. Por encima de 0,4 el régimen dispara demasiado (87
disparos/símbolo) y el lector pierde toda la ventaja.

### Bug 5. La plasticidad nunca se ejecutó

La elegibilidad por sinapsis (`elig`) se leía y se decaía, pero **nunca se
escribía**: no había ninguna línea que la calculara. El término de tres
factores valía siempre cero, así que `PLASTIC=1` y `PLASTIC=0` daban resultados
idénticos hasta el último decimal.

Se implementó una traza de disparo presináptica real (`Layer::pre_trace`), con
decaimiento exponencial por tick (`tau_elig = 20` ticks ≈ 5 símbolos) y elevada
al retardo de la sinapsis para alinearla con la llegada del pulso. El error
difundido por la matriz fija `B` se filtra paso bajo, porque el error
instantáneo por símbolo es casi todo ruido. Los pesos se acotan a ±60 % de su
valor inicial para que no se vayan a los extremos.

---

## Estado real del aprendizaje local, sin adornos

Con la plasticidad funcionando de verdad, sobre texto no visto:

| plasticidad | `eta` | deriva de pesos | bpc | acierto |
|---|---|---|---|---|
| **no** | — | 0 % | **2,050** | **63,8 %** |
| sí | 3e-4 | 0,07 % | 2,067 | 63,7 % |
| sí | 3e-3 | 0,69 % | 2,084 | 62,9 % |
| sí | 1e-2 | 2,16 % | 1,973 → 2,084 | 64,0 % |

**La plasticidad local, tal como está implementada, no mejora nada: empeora.**
El efecto crece con `eta` y siempre en la dirección mala. Es un resultado
negativo y se queda escrito como tal.

### Intento de arreglo 1: alinear el crédito en el tiempo

La primera hipótesis fue que el crédito estaba desalineado: `feedback_[post]`
es el error del símbolo **actual**, pero el objetivo es el **siguiente**.

Se probó con el objetivo puesto en el símbolo actual (crédito perfectamente
alineado, el lector llega al 96,5 % de acierto):

| plasticidad | bpc | acierto |
|---|---|---|
| no | **0,577** | **96,5 %** |
| sí (`eta` = 1e-3, matriz `B` aleatoria) | 0,657 | 95,6 % |

Sigue empeorando. **La alineación temporal no era el problema.**

### Intento de arreglo 2: señal de e-prop en vez de matriz aleatoria

Si no era el tiempo, era la señal. Con `B` aleatoria cada neurona recibe el
error de un símbolo al azar: no hay crédito específico y la regla solo inyecta
ruido. Se sustituyó por la señal real de e-prop, difundiendo el error por la
matriz de **lectura entrenada**:

```
L_j = sum_c R[c][j] * error[c]          (escalada por eprop_scale = 0.125)
```

| plasticidad | `eta` | bpc |
|---|---|---|
| no | — | **0,577** |
| sí, e-prop | 1e-3 | 0,581 |
| sí, e-prop | 1e-2 | 0,597 |
| sí, e-prop | 3e-2 | 0,598 |

El daño baja (de 0,657 a 0,581) pero **sigue sin haber ganancia**. La razón es
de protocolo, no de regla: en `bench.cpp` la red ve el texto en **una sola
pasada** y el lector se entrena después, así que la plasticidad solo llega a
ver una `R_` aleatoria y su señal de aprendizaje vale casi cero. Hay que
entrenar los dos a la vez.

### Intento de arreglo 3: entrenamiento conjunto en línea

Se añadió `tools/bench_joint.cpp`: lector y plasticidad aprenden símbolo a
símbolo, en línea. Con presupuesto corto (3000 símbolos, 2 épocas) el lector
todavía no está entrenado y todas las variantes quedan en ~3,78 bpc, así que
esta prueba **no distingue** nada: hace falta ejecutarla en Kaggle con el
corpus completo. El régimen de disparo se comprobó estable (37,7
disparos/símbolo en ambos casos, sin fuga).

### Conclusión honesta de la ronda

La plasticidad local, ya ejecutándose de verdad, **no ha producido ninguna
mejora medible en ninguna de las tres formulaciones**. Lo que sí está
demostrado es que el motor ya funciona: guarda historia, la recurrencia aporta
0,11 bpc y el conjunto mejora al bigrama por más de un bit sobre texto no
visto. El aprendizaje local sigue siendo el problema abierto, y ahora está
acotado: no es el tiempo, no es (solo) la matriz de retroalimentación, y el
banco para decidirlo existe.

## Números honestos actuales (protocolo disjunto)

Motor arreglado, recurrencia 0,30, lector softmax K=64 × 8 retardos, 8000
símbolos de entrenamiento, test sobre `spanish_holdout.txt`:

| modelo | bpc | acierto |
|---|---|---|
| **red de pulsos (sin plasticidad)** | **2,050** | **63,8 %** |
| bigrama de referencia (estimado con train) | 3,060 | — |

Mejor que el bigrama por un margen real, sobre texto no visto. Sirve como
prueba de que el motor funciona; **no** sirve como cifra final, porque el
corpus sigue siendo el mismo fragmento repetido con variantes y solo tiene 36
símbolos distintos. La cifra buena exige el corpus real en Kaggle.

## Medición nueva: ¿qué guarda la fatiga en texto real?

Protocolo de reposo (estado vacío antes de cada ensayo) y decodificador ridge
offline con partición train/test, sobre `data/spanish_small.txt`:

| objetivo | acierto |
|---|---|
| el byte **anterior** (lag 1) | **97,7 %** |
| el byte de hace dos (lag 2) | 26,4 % |

La fatiga **sí codifica el símbolo anterior en texto real**, con casi total
fidelidad. No es un artefacto de la tarea sintética.

Pero el techo de predicción del byte **siguiente** usando el perfil completo:

| vista | techo offline (streaming) | readout en línea |
|---|---|---|
| fatiga | 4,99 bpc | 5,30 bpc |
| tasa | 5,04 bpc | 4,37 bpc |
| bigrama trivial | — | 2,89 bpc |

La deriva de las features es despreciable (0,004 frente a una escala de 0,004
salvo en las dimensiones activas), así que el problema **no** es el optimizador
ni la no estacionariedad.

**La lectura honesta:** la información sobre el símbolo anterior está en la red,
pero convertirla en predicción del siguiente exige aprender la tabla de
transiciones del lenguaje, que es un problema mucho más difícil que recuperar la
clave. Con 6.000 muestras y un decodificador lineal, el techo medido es ~5 bpc.

## Lo que sigue abierto, sin adornos

**La red no mejora la predicción de texto.** Tiene memoria de trabajo —eso está
demostrado y medido— pero esa memoria no se traduce en contexto útil para
predecir. Con texto real, el perfil completo de fatiga (1024 dimensiones) rinde
peor que un bigrama.

Intentado y descartado en esta sesión:

- Canal de memoria separado y más fuerte para marcar la clave: funciona para la
  tarea de memoria, pero usarlo para todo el texto satura la red.
- Barridos de fatiga (paso 0,4–0,6, decaimiento 0,92–0,97), ganancia de memoria
  y homeostasis: no cambian el resultado en texto.
- Subir el tamaño: comprobado antes de la reescritura, no escala.

Ese paso ya está hecho (arriba): **la fatiga sí guarda el símbolo anterior en
texto real**. Lo que falta es el paso siguiente: que esa representación se
convierta en predicción. Dos caminos medibles:

1. **Más datos para el decodificador.** El techo de 4,99 bpc se midió con 6.000
   muestras y 1.024 dimensiones. Duplicar o cuadruplicar las muestras dirá si
   el techo baja hacia el bigrama o se estanca.
2. **Un decodificador con capacidad.** Un clasificador lineal desde 1.024
   dimensiones es pobre para aprender transiciones. Es legítimo usar algo más
   potente en la lectura: la arquitectura propia está en la red, no en el
   lector.

---

## Reproducir

```
make
./bin/cortex_lm audit                          # las 5 comprobaciones
./bin/cortex_lm smoke x                        # latido y estabilidad
CORTEX_VIEW=ctx CORTEX_NCTX=4 ./bin/cortex_lm eval data/spanish_small.txt
CORTEX_VIEW=rate  ./bin/cortex_lm eval data/spanish_small.txt
CORTEX_VIEW=theta ./bin/cortex_lm eval data/spanish_small.txt
./bin/cortex_lm bench                          # memoria de trabajo
```

Variables: `CORTEX_VIEW=ctx|rate|theta`, `CORTEX_N=<neuronas>`,
`CORTEX_NCTX=1..8`.

---

## Infraestructura

Kaggle (P100 16 GB, `kaggle kernels push` desde Termux) para el cómputo pesado.
El Redmi 9C es terminal de control, no máquina de entrenamiento.

`make portable` compila sin `-march=native` para Kaggle y Termux.
