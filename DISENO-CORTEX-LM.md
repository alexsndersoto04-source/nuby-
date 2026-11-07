# CORTEX-LM — Arquitectura

Red neuronal de pulsos que aprende lenguaje natural con **plasticidad local**,
sin retropropagación, y con memoria independiente de la longitud de la secuencia.

Versión 0.1 — documento de diseño. **No hay código todavía.** Esto es el plano,
no el edificio.

---

## 0. Regla de honestidad de este documento

Cada afirmación está marcada:

- **[E]** Establecido. Hay papers, matemáticas y resultados reproducibles.
- **[H]** Hipótesis. Creemos que funcionará. No está demostrado por nadie.
- **[R]** Riesgo. Si esto falla, el proyecto no funciona. Está dicho por adelantado.

Si en algún momento este proyecto se presenta como "va a funcionar", está
mintiendo. Lo que sí puedo prometer es que **cada fase tiene un criterio de
muerte escrito antes de empezar**, para que no nos engañemos.

---

## 1. Qué se construye exactamente

**Objetivo medible:** un modelo de lenguaje a nivel de carácter, en español,
entrenado con reglas de plasticidad local, que alcance **menos de 2.0 bits por
carácter (BPC)** en texto no visto.

Como referencia: el español escrito tiene ~4.7 bits/carácter de entropía cruda.
Un modelo de bigramas baja a ~3.2. Un LSTM pequeño a ~1.6. GPT-2 a ~1.0.

- Por debajo de **2.5 BPC**: aprende ortografía y palabras frecuentes.
- Por debajo de **2.0 BPC**: aprende morfología y estructura de frase. **Este es el objetivo.**
- Por debajo de **1.6 BPC**: iguala a un LSTM entrenado con backpropagation. Sería un resultado publicable.

**Lo que NO es el objetivo:** un chatbot. Un modelo de 2.5M de sinapsis no
conversa, igual que un LSTM de 2.5M no conversa. Conversar exige tres o cuatro
órdenes de magnitud más. Si alguien dice lo contrario, está vendiendo algo.

La apuesta de este proyecto no es "chat". Es: **demostrar que el aprendizaje
local, sin gradientes hacia atrás, puede modelar lenguaje a una escala donde
nadie lo ha demostrado.** Eso, si sale, es la base sobre la que se escala.

---

## 2. La tesis: qué es nuevo y qué no

**[E] Lo que existe y usamos.** No reinventamos el hormigón:

| Pieza | Origen | Uso aquí |
|---|---|---|
| Neurona LIF | dinámica de membrana estándar | unidad de cómputo |
| Traza de elegibilidad | e-prop (Bellec et al., 2020) | memoria local por sinapsis |
| Señal de aprendizaje difundida | feedback alignment (Lillicrap, 2016) | canal de enseñanza |
| Conectividad dispersa CSR | computación científica | motor de eventos |

**[H] Lo que no existe y es la apuesta.** La combinación concreta:

1. **Contexto por retardo, no por atención.** En un transformer, el contexto se
   consigue comparando todos los tokens con todos (atención). Aquí, cada sinapsis
   tiene un retardo propio de 1 a 32 ticks. Una neurona con entradas de retardo
   largo integra historia larga; una con retardos cortos, historia corta. El
   contexto emerge de la **geometría temporal del cableado**, no de un producto
   punto. Nadie ha construido un modelo de lenguaje así.

2. **Dos escalas de memoria acopladas.** Pesos rápidos (elegibilidad, viven
   segundos, se disipan) y pesos lentos (consolidados durante un ciclo de reposo
   donde la red se reproduce a sí misma). Es la teoría de sistemas
   complementarios de McClelland aplicada a un modelo de lenguaje de pulsos. No
   se ha hecho.

3. **Asambleas como vocabulario intermedio.** Una capa competitiva (k-WTA)
   obliga a que la representación intermedia sea un conjunto pequeño de neuronas
   activas —una coalición, no un vector denso—. La hipótesis es que esas
   coaliciones se estabilicen y funcionen como unidades de significado sin que
   nadie las etiquete.

**[R] El riesgo central.** Si la plasticidad local con señal difundida no puede
igualar a backpropagation en modelado de lenguaje a esta escala, el proyecto
entero se cae. Es la hipótesis a probar **primero**, en la fase 2, antes de
invertir en nada más.

---

## 3. Arquitectura

```
                    texto  "la casa es"
                       │
                       ▼
            ┌──────────────────────┐
            │  Codificador de bytes │   un carácter = 8 neuronas
            │  (sin aprendizaje)    │   (código binario, disperso)
            └──────────┬───────────┘
                       ▼
   ┌─────────────────────────────────────────┐
   │  Capa 1 · recurrente LIF                 │  N = 4096
   │  dispersión 6 %  ·  retardos 1..32      │  1.0 M sinapsis
   │  inhibición local k-WTA (k = 41 ≈ 1 %)  │
   └──────────────────┬──────────────────────┘
                      ▼
   ┌─────────────────────────────────────────┐
   │  Capa 2 · recurrente LIF                 │  N = 4096
   │  dispersión 6 %  ·  retardos 1..32      │  1.0 M sinapsis
   │  inhibición local k-WTA                 │
   └──────────────────┬──────────────────────┘
                      ▼
            ┌──────────────────────┐
            │  Readout  120 → vocab │  0.5 M parámetros
            │  (carácter siguiente) │
            └──────────────────────┘
```

**Total: ~2.5 M sinapsis entrenables.**

### 3.1 Ciclo de tiempo

**[E]** El tiempo es virtual. Un `tick` no es un milisegundo de pared: es un paso
de simulación. Se corre tan rápido como la máquina permita.

- `dt = 1` (unidad de simulación)
- `tau_m = 20` (constante de membrana, en ticks)
- `v_rest = 0.0`, `v_reset = 0.0`, `v_th = 1.0` (normalizado, sin milivoltios)
- refractario: 2 ticks

**[H] Compresión temporal.** Un carácter no ocupa un tick, ocupa **5 ticks**. El
texto entra como una ráfaga corta y luego hay silencio. Esto separa los símbolos
en el tiempo y permite que la recurrencia procese entre símbolo y símbolo. Si en
la fase 3 resulta que 5 es demasiado poco, se sube a 8 o 12; es un parámetro de
la búsqueda, no del diseño.

### 3.2 Codificación de entrada

**[E]** Un carácter = su código, 8 bits, cada bit a una neurona sensorial fija.
Se activan los bits a 1 durante los primeros 2 de los 5 ticks del símbolo.

Por qué así y no con "una neurona por carácter": un código de 8 bits compartido
significa que la letra `a` y la letra `e` comparten neuronas y difieren en una o
dos. La red debe aprender a separarlas, y ese esfuerzo es el que crea estructura.
Con una neurona por carácter, no hay nada que aprender: es una tabla.

**[R]** Riesgo conocido: el código binario no tiene nada que ver con la
fonología. `a` y `á` quedan en posiciones arbitrarias. Es aceptable en fase 2;
si resulta limitante, se sustituye por un código aprendido —pero eso es una capa
más, y primero hay que demostrar que el mecanismo base aprende.

### 3.3 Dinámica de la neurona

```
v_j(t) = α · v_j(t-1) + Σ_i w_ji · z_i(t - d_ji) - v_th · z_j(t-1)

z_j(t) = 1  si  v_j(t) ≥ v_th  y  t - t_last(j) ≥ refractory
         0  en otro caso

α = exp(-dt / tau_m)
```

`d_ji` es el retardo de esa sinapsis concreta, en ticks. Es el corazón de la
tesis 1: el retardo no es un detalle de implementación, es el mecanismo de
contexto.

### 3.4 Inhibición k-WTA

**[H]** Tras la integración, en cada capa solo sobreviven las `k` neuronas con
mayor potencial; el resto se fuerza a cero ese tick.

- Capa 1 y 2: `k = 41` (1 % de 4096). Una coalición de ~40 neuronas por símbolo.

Esto es lo que crea las asambleas: la competición fuerza a repartir la
representación en vez de que la actividad se difunda. Y es lo que hace que la
red sea **dispersa de verdad**, que es lo que abarata el cómputo.

**[R]** Un `k` demasiado pequeño mata neuronas (colapso de representación); uno
demasiado grande produce avalancha. Se barre en fase 2 sobre {21, 41, 82, 164}.
Hay una métrica objetiva para esto: el histograma de uso de neuronas debe ser
plano, no una ley de potencias extrema.

---

## 4. La regla de aprendizaje

Esta es la parte que define el proyecto. **[E]** en su forma matemática,
**[H]** en que funcione para lenguaje.

### 4.1 Traza de elegibilidad (local, por sinapsis)

Cada sinapsis guarda **un solo número**: su elegibilidad. Es lo único que
recuerda del pasado, y vive segundos.

```
ê_ji(t) = α_e · ê_ji(t-1) + ψ_j(t) · z_i(t - d_ji)

α_e = exp(-dt / tau_e),   tau_e = 50 ticks
```

- `z_i` es la actividad de la neurona presináptica: disponible **en la sinapsis**.
- `ψ_j` es la derivada sustituta del umbral de la postsináptica: disponible
  **en la neurona**.
- No hay historia guardada. No hay desenrollado. **Memoria constante en la
  longitud de la secuencia.** Este es el punto entero.

### 4.2 Señal de aprendizaje (global, difundida)

Al final de cada símbolo, la red produce 120 logits (uno por carácter posible).

```
error_k    = softmax(logits)_k - objetivo_k        (en el readout)
señal_j(t) = Σ_k B_jk · error_k                     (B: retroalimentación fija aleatoria)
```

`B` es una matriz de 4096 × 120 **que no se entrena**. Es aleatoria y se queda
fija. Esto es feedback alignment: **[E]** funciona aunque los pesos de vuelta no
sean la transpuesta de los de ida, porque basta con que apunten en la dirección
correcta en promedio.

Cada neurona `j` recibe **un escalar** por tick. Eso es todo lo que necesita
saber del mundo exterior. No ve el error de las demás, no ve la arquitectura, no
necesita saber en qué capa está.

### 4.3 Actualización

```
Δw_ji = - η · Σ_t señal_j(t) · ê_ji(t)
```

Cada sinapsis calcula su cambio **con tres datos**: su traza, la señal de su
neurona postsináptica, y nada más. No hay cadena de regla de la cadena. No hay
paso hacia atrás por las capas.

**Esto es lo que hace legítimo llamarlo local.** No es backpropagation
disfrazada: no se propaga ningún gradiente entre capas, solo se difunde un
escalar por neurona.

### 4.4 Lo que hay que decir sin adornos

**[R] Este diseño NO es dopamina pura.** El plan original (recompensa escalar
global, tipo dopamina) es demasiado débil para aprender lenguaje: con una sola
señal para toda la red, todas las sinapsis reciben el mismo mensaje y no se puede
distinguir cuál fue la culpable. Ya lo probé empíricamente en la versión
anterior: la red aprendía a encender todo, que es lo mismo que no aprender.

La diferencia entre "local" y "sin ninguna señal externa" es esta:

- **Sin señal externa** (Hebb puro, STDP puro): no hay forma de aprender una
  tarea. [E]
- **Señal escalar global** (dopamina pura): aprende condicionamiento simple, no
  lenguaje. [R] por evidencia propia.
- **Señal por neurona difundida** (este diseño): es lo que la biología sugiere
  que existe (retroalimentación cortical + neuromoduladores específicos por
  población). [E] como mecanismo plausible.

Este es el compromiso central. Escogemos el tercero porque es el único que tiene
una posibilidad real de modelar lenguaje sin gradientes.

---

## 5. Las dos escalas de memoria (tesis 2)

**[H]** Dos matrices de pesos, misma forma:

- **W_lento**: los pesos que persisten. Es el modelo final.
- **W_rápido**: ajustes acumulados en la elegibilidad durante el texto.

Cada `P` símbolos (P ≈ 500), se entra en **ciclo de reposo**:

1. Se congela la entrada.
2. La red se reproduce sola durante ~200 ticks (repetición interna).
3. La elegibilidad acumulada se suma a `W_lento` con un factor de consolidación.
4. Se renormaliza la suma de pesos entrantes por neurona (homeostasis sináptica).
5. Se poda: las sinapsis con peso < 0.05 se eliminan.

Esto es lo que evita la deriva que ya observé: sin ciclo de reposo, la
plasticidad tiene sesgo sistemático y los pesos crecen hasta que la red se
enciende entera.

---

## 6. Parámetros concretos

Tabla completa. Estos son los valores de partida; los marcados con ↻ se barren.

### Red

| Parámetro | Valor | Nota |
|---|---|---|
| Neuronas por capa | 4096 | ↻ {2048, 4096} |
| Capas recurrentes | 2 | ↻ {1, 2, 3} |
| Dispersión recurrente | 6 % | ↻ {4, 6, 10} % |
| Retardos | 1..32 ticks, distribución log-normal | ↻ |
| Sinapsis totales | ~2.5 M | |
| Vocabulario | 120 caracteres | español + puntuación |
| k-WTA | 41 por capa | ↻ {21, 41, 82, 164} |

### Dinámica

| Parámetro | Valor |
|---|---|
| `dt` | 1 |
| `tau_m` | 20 ticks |
| `tau_e` (elegibilidad) | 50 ticks |
| `v_th` | 1.0 |
| `v_reset` | 0.0 |
| Refractario | 2 ticks |
| Ticks por carácter | 5 |

### Aprendizaje

| Parámetro | Valor | Nota |
|---|---|---|
| Optimizador | Adam sobre Δw | sí, Adam sobre reglas locales es válido |
| `η` inicial | 3e-4 | ↻ {1e-4, 3e-4, 1e-3} |
| Recorte de elegibilidad | ±3.0 | |
| Consolidación cada | 500 símbolos | ↻ |
| Factor de consolidación | 0.35 | ↻ |
| Poda | peso < 0.05 | |

### Entrenamiento

| Parámetro | Valor | Nota |
|---|---|---|
| Longitud de secuencia | 128 caracteres | |
| Batch | 8 | limitado por memoria, no por calidad |
| Pasos | 20.000–40.000 | ~2.5 M caracteres vistos |
| Semilla | fija, 1234 | reproducibilidad obligatoria |

---

## 7. Presupuesto de memoria y cómputo

Esta es la sección que responde a "que no gasten tanta RAM". Con números.

### 7.1 Memoria del modelo (fase 2)

| Componente | Cálculo | Tamaño |
|---|---|---|
| Pesos capa 1 | 1.0 M × 4 B | 4.0 MB |
| Pesos capa 2 | 1.0 M × 4 B | 4.0 MB |
| Readout | 0.5 M × 4 B | 2.0 MB |
| Trazas de elegibilidad | 2.5 M × 4 B | 10.0 MB |
| Pesos rápidos | 2.5 M × 4 B | 10.0 MB |
| Estado de neuronas | 8192 × ~24 B | 0.2 MB |
| **Total** | | **~30 MB** |

**30 MB.** Eso es el punto: el modelo entero cabe en memoria L3 de un procesador
moderno. Un transformer de parámetros equivalentes necesita además almacenar
activaciones de toda la secuencia para el backward pass, lo que multiplica la
memoria por la longitud de la secuencia.

**[E] Comparación:** e-prop tiene memoria constante respecto a la longitud de la
secuencia. BPTT (el entrenamiento de transformers) crece linealmente. Con
secuencias de 4096 tokens, esa diferencia es de 4096×.

### 7.2 Cómputo por paso de entrenamiento

Con 3 % de neuronas activas por tick:

```
eventos por tick por capa = 4096 × 0.03 activas × 246 salientes = 30.000
eventos por carácter      = 30.000 × 5 ticks = 150.000
ambas capas               = 300.000
por secuencia (128)       = 38.4 M
por batch (8)             = 307 M eventos
```

A ~100 M eventos/s en CPU optimizada (C++ con CSR disperso):

**≈ 3 segundos por paso.** 20.000 pasos ≈ **17 horas**.

Con una GPU (T4/P100) y kernels de eventos agrupados: **~2–4 horas**.

### 7.3 Escalado

| Escala | Sinapsis | Memoria | Tiempo (GPU) |
|---|---|---|---|
| Fase 1 (prueba) | 0.2 M | 3 MB | minutos |
| **Fase 2 (objetivo)** | **2.5 M** | **30 MB** | **~4 h** |
| Fase 3 (ambicioso) | 40 M | 480 MB | ~5 días |
| Fase 4 (publicable) | 400 M | 4.8 GB | semanas |

La fase 4 es donde empezaría a parecerse a algo. Requiere financiación de
cómputo. No es un paso, es un proyecto.

---

## 8. Dónde se ejecuta y dónde se entrena

### 8.1 Lenguaje

**[E] Decisión:** núcleo en **C++** (motor de eventos + regla de aprendizaje),
expuesto a **Python** por pybind11 para la experimentación.

Por qué no Python puro: el bucle de eventos se ejecuta cientos de millones de
veces por paso. Python no llega. Ya lo medí: 5.000–20.000 ticks/s en Python
frente a los millones que hacen falta.

Por qué no PyTorch: usaríamos torch solo como biblioteca de arrays, nunca
autograd. Es más limpio escribir el kernel de eventos a mano porque ninguna
operación estándar corresponde a "sumar eventos dispersos con retardos
heterogéneos".

### 8.2 Hardware

| Fase | Dónde | Coste |
|---|---|---|
| Fase 1 (motor + prueba sintética) | **tu máquina, CPU** | 0 |
| Fase 2 (entrenamiento del modelo) | **Kaggle, GPU P100 16 GB** | 0 — 30 h/semana gratis |
| Fase 2 (alternativa) | Google Colab, T4 | 0 (con límites) |
| Fase 3 | **RunPod / Vast.ai**, RTX 4090 | ~0,30–0,50 USD/h ≈ 40 USD el ciclo |
| Fase 4 | alquiler multi-GPU | cientos de USD |

**[E] Recomendación concreta:** diseñar para Kaggle. Da 30 horas semanales de
P100 gratis, sin tarjeta. Con 4 horas por corrida, son 7 experimentos por
semana. Es exactamente lo que hace falta para iterar.

**Lo que necesito saber de ti:** qué máquina tienes (CPU, RAM, si tiene GPU y
cuál). Con eso ajusto la fase 1 y decido si el ciclo de desarrollo es local o
todo en la nube.

### 8.3 Datos

**[E]** Corpus en español, nivel de carácter:

| Opción | Tamaño | Dónde |
|---|---|---|
| Dominio público (Gutenberg ES) | 50–100 MB | gutenberg.org |
| Wikipedia ES filtrada | 500 MB | dumps.wikimedia.org |
| Mix (recomendado) | 100 MB | libros + wiki |

Con 100 MB hay 100 millones de caracteres. Con 20.000 pasos × 8 × 128 vemos
~20 M de caracteres, o sea ~5 épocas. Es suficiente para la fase 2.

División: 98 % entrenamiento, 1 % validación, 1 % prueba. La validación decide
todo: el BPC se mide **siempre** en texto no visto.

---

## 9. Fases y criterios de muerte

Escritas antes de empezar. Si una fase falla su criterio, se detiene el
proyecto o se replantea. Sin excusas.

### Fase 0 — Referencia (1 día)

Entrenar un **LSTM a nivel de carácter con BPTT**, mismo tamaño, mismos datos.
Es la vara de medir. Sin esto no sabemos si 2.0 BPC es bueno o malo.

**Criterio:** obtener BPC de referencia del LSTM.
**Si falla:** el problema es el corpus o el tokenizador, no la arquitectura.

### Fase 1 — El motor (3–5 días)

Motor C++ + prueba sintética: aprender una secuencia de 4 símbolos.

**Criterio:** la red aprende la secuencia y **se mantiene selectiva** — un
símbolo enciende su coalición y no las demás. Medido desde el silencio.
**Si falla:** el problema es la regla de aprendizaje, no el lenguaje. Se
replantea antes de seguir. *(Este es exactamente el punto donde falló el
prototipo anterior: aprendía encendiendo todo.)*

### Fase 2 — Lenguaje (2–4 semanas)

Entrenamiento real sobre español, nivel de carácter.

**Criterio de éxito:** BPC < 2.0 en validación.
**Criterio de fracaso:** BPC > 2.5 tras 40.000 pasos → la plasticidad local no
modela lenguaje a esta escala.
**Criterio de resultado publicable:** BPC ≤ LSTM de referencia + 0.15.

### Fase 3 — Escala (si la fase 2 sale bien)

40 M de sinapsis, corpus de 500 MB.

**Criterio:** BPC < 1.7. Sería el primer modelo de lenguaje entrenado sin
retropropagación que alcanza calidad de LSTM.

### Fase 4 — Financiación

Esto ya no es código, es buscar cómputo.

---

## 10. Lo que no prometo

1. **No prometo que funcione.** La hipótesis central —plasticidad local con
   señal difundida puede modelar lenguaje— no está demostrada por nadie. Si
   estuviera, no sería terreno nuevo.
2. **No prometo que hable.** Un modelo de 2.5 M de sinapsis escribe español
   localmente plausible, no conversa. Conversar es otra escala y otro presupuesto.
3. **No prometo que iguale a un transformer.** Igualar a un LSTM sería ya un
   resultado. Igualar a un transformer moderno exige cómputo que no está sobre
   la mesa.
4. **Sí prometo** que cada fase tiene un criterio de muerte escrito antes de
   empezar, y que si falla lo voy a decir con el número en la mano, como hice
   con la versión anterior.

---

## 11. Lo único que necesito de ti para empezar

1. **Tu máquina:** CPU, RAM, GPU (si tiene), sistema operativo.
2. **Confirmación de la regla de aprendizaje:** ¿aceptas señal de error
   difundida por neurona (sección 4.2), o quieres explorar dopamina escalar pura
   sabiendo que la evidencia dice que no alcanza para lenguaje?
3. **Confirmación del objetivo:** BPC < 2.0 en español a nivel de carácter como
   vara de medir. Si tu objetivo real es otro, este diseño apunta al sitio
   equivocado.

Con esas tres respuestas, la fase 0 y la fase 1 se pueden empezar.
