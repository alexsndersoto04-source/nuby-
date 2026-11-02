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

### Texto (entrena 80 %, evalúa 20 % no visto)

| vista | bpc |
|---|---|
| **historial de 4 símbolos** | **0,409** |
| historial, motor anterior (referencia) | 1,396 |
| tasa de disparo de la red | 4,370 |
| fatiga de la red | 5,301 |
| bigrama trivial (contexto de 1 byte) | 2,893 |

La curva de la vista de tasa baja despacio y monótona, como un unigrama: la
fatiga no aporta contexto de texto.

---

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
