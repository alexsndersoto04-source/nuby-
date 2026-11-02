# CORTEX

Red neuronal de pulsos escrita desde cero, sin numpy, sin backpropagation y sin
matrices densas. El objetivo no es imitar un transformer con otro nombre: es
construir un sistema donde **el léxico sea una salida y no una entrada**.

```
python3 snn/experiments/01_latido.py        # ¿late? ¿enruta?          ~3 s
python3 snn/experiments/02_aprendizaje.py   # ¿aprende?               ~110 s
python3 snn/experiments/03_compuerta.py     # compuerta + persistencia ~190 s
```

Python 3.11, sin dependencias. `snn/cortex/` es el motor; `snn/experiments/`
son las actas.

---

## El resultado, sin adornos

**Lo que funciona y está verificado:**

| Pieza | Estado | Medición |
|---|---|---|
| Motor de pulsos, eventos con retardo | OK | 10.000–20.000 ticks/s en un núcleo |
| Competición entre grupos (inhibición cruzada) | OK | concentración 0,94; enruta 10/10 |
| Línea base en silencio | OK | 0,06 disparos/tick (2 % de la red) |
| Aprendizaje de secuencia con dopamina | OK | 0 % → 94 % de anticipación, época 13 |
| Recuperación en frío (solo se inyecta A) | OK | la red sigue el ciclo sin estímulo externo |
| Compuerta de fase por ganancia sensorial | OK | escucha 285 disparos / emisión 0 |
| Persistencia del grafo | OK | 110 KB, recargable e inspeccionable con `zcat` |
| Reposo que reconstruye el estado sin perder pesos | OK | elimina el sesgo de saturación |

**Lo que no funciona, y por qué.** Esto es la parte importante:

> Cuando la red aprende *mucho*, deja de ser selectiva. Cualquier estímulo
> enciende todos los grupos. La potenciación sináptica no distingue "esta
> sinapsis causó la respuesta" de "esta sinapsis estaba cerca cuando hubo
> recompensa". Sin esa distinción, el crédito se reparte por igual y la red
> termina prediciendo todo, que equivale a no predecir nada.

El acierto del 94 % del experimento 2 es **parcialmente falso**: se midió sobre
una red que ya casi no se callaba. Ese número está en el experimento porque es
lo que salió, no porque sea un éxito limpio.

Con normalización sináptica la red se mantiene selectiva pero aprende poco. Con
aprendizaje fuerte aprende mucho pero pierde selectividad. **Ese compromiso no
está resuelto en este código.** Es el problema abierto del proyecto.

---

## Las decisiones de diseño que sí están resueltas

**1. Topología modular, no aleatoria.** Un grafo aleatorio sobre N neuronas no
funciona: dos neuronas que reciben del mismo grupo nunca coinciden y la señal
muere en el primer salto. La única topología que produce asambleas de verdad
tiene tres piezas simultáneas: bucle recurrente denso *dentro* del grupo
(bistable: la asamblea se sostiene sola), inhibición ancha *entre* grupos
(competición: el ganador apaga al resto) y interneuronas *dentro* del grupo
(freno, para que el bucle no explote). Quitar una de las tres rompe el sistema.

**2. La entrada debe ser subumbral.** Si el estímulo por sí solo hace disparar,
la red es un espejo: repite lo que entra. La decisión de disparar tiene que
tomarla la recurrencia. Esta sola línea de diseño separa un eco de una asamblea.

**3. Inhibición con retardo mayor que la excitación.** La excitación llega en
1 tick y la inhibición en 2-3. Esa asimetría es lo que permite que un patrón se
imponga *antes* de que llegue el freno. Si llegan juntas, todo se cancela.

**4. Plasticidad de tres factores, no STDP de pares.** El STDP clásico solo
asocia disparos separados por pocos milisegundos. Aquí la asociación cruza
decenas de ticks, así que la marca vive en una traza de elegibilidad que dura
cientos de ms, y solo se convierte en peso cuando llega dopamina.

**5. La compuerta de fase es una ganancia, no un interruptor.** En emisión la
entrada no se bloquea: se atenúa al 12 %. Sigue llegando y sigue siendo
insuficiente. Es la versión eléctrica de cerrar los ojos para hablar.

**6. El crítico no castiga el silencio.** Castigar el silencio enseña a
responder siempre, y una red que siempre responde no puede aprender nada
nuevo. Silencio es ignorancia, no error.

---

## Arquitectura del código

```
snn/cortex/brain.py
    Params            constantes, todas en un sitio, todas con su razón
    BucketQueue       cola de eventos por instante de llegada (buffer circular)
    Brain             estado en arrays planos, sinapsis en CSR
      build_clustered()      topología modular (la que funciona)
      inject()               energía en un conjunto de neuronas
      step()                 un tick: llegadas → disparo → propagación → trazas
      reward() / consolidate()  dopamina → elegibilidad → peso
      synaptic_scaling()     homeostasis: la neurona defiende su suma de entrada
      rest_cycle()           reposo: reconstruye el estado, conserva los pesos
      save() / load()        gzip, texto plano, inspeccionable
snn/cortex/assembly.py
    AssemblyDetector  agrupa neuronas que disparan juntas (Jaccard temporal)
    Readout           asocia asamblea → símbolo por votación, sin gradientes
```

Nada en el motor sabe qué es un símbolo. El detector solo agrupa. El readout
solo cuenta. La etiqueta es una estadística sobre la coalición, no una
propiedad de la neurona: si la red cambia, la etiqueta se muda sola.

---

## Lo que este sistema no es

No conversa. No escribe texto coherente. No compite con un LLM y no va a
hacerlo: un LLM es una máquina de imitar texto a escala, y esto es una máquina
de formar memoria causal en el tiempo. Son cosas distintas.

Lo que sí es: un motor de pulsos real, sin matrices densas, donde el símbolo es
una coalición que se estabiliza por competición y donde el aprendizaje no usa
gradientes. La parte que falla está medida y localizada, no escondida.

---

## El siguiente paso, si se retoma

El problema abierto es **asignación de crédito con inhibición específica**:
interneuronas que frenen solo a los competidores de su propia asamblea y no al
vecindario entero, para que el crítico tenga un campo receptivo y pueda decir
"esta sinapsis fue la culpable". Eso exige aprender el mapa de competencia, y
no es un parámetro que falte.

Antes de escribir una línea más de eso, la métrica que decide si vale la pena
es una sola: **selectividad después del aprendizaje**, medida desde el silencio,
con la red inyectada con un solo token. Si un token enciende un grupo y su
sucesor, y no los otros seis, el sistema está vivo. Si enciende los ocho, no
importa cuánto "acierte": no aprendió, se saturó.
