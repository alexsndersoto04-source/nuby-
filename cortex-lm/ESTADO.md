# CORTEX-LM — estado real

Actualizado tras la primera sesión de construcción. Sin adornos.

## Qué existe y funciona

| Pieza | Estado | Verificación |
|---|---|---|
| Motor C++ sin dependencias | **funciona** | compila con `make`, corre en cualquier máquina |
| Grafo CSR con retardos por sinapsis | **funciona** | 3.062 sinapsis en la prueba pequeña |
| Neurona LIF + refractario + umbral adaptativo | **funciona** | 3,2 disparos/tick, sin epilepsia |
| Competición k-WTA | **funciona** | 8 ganadores exactos por símbolo |
| Listas de activos (escalado) | **funciona** | solo se procesa lo que tiene carga |
| Campos receptivos distintos por carácter | **funciona** | Jaccard 0,023 entre caracteres |
| Persistencia binaria versionada | **funciona** | guarda y recarga idéntico |
| Homeostasis con conciencia del grado | **funciona** | evita el apagado de la red |
| Readout por código de tasa | **funciona** | 0,46 bpc en tarea trivial de 10 clases |
| Entrenamiento sobre español | **parcial** | 5,29 → 4,37 bpc y se estanca |

## Qué NO funciona todavía

**El estancamiento en ~4,4 bpc.** La causa está medida, no supuesta:

La información del símbolo actual llega al readout. La del **contexto anterior**
no llega de forma utilizable. El único canal de contexto es una media decaída de
la actividad (`ctx`), y su señal es débil: el mismo carácter en dos posiciones
distintas produce vectores con coseno 0,24–0,51, pero esa variación no codifica
*qué* vino antes, solo *cuánta actividad hubo*.

Sin contexto, el modelo no puede pasar de bigramas. Con el corpus de prueba, el
techo de un bigrama es ~4,3 bpc. Estamos exactamente en el techo.

**Y el problema de fondo:** la regla de tres factores con señal difundida no ha
demostrado todavía que ajuste la recurrencia de forma útil. Se probó con la
recurrencia congelada y el resultado es el mismo, lo que confirma que el cuello
de botella está en las features, no en la regla. Pero eso también significa que
**la regla sigue sin validarse.**

## El hallazgo de esta sesión: por qué no hay contexto

Medido, no supuesto. La red **no tiene estado entre símbolos**:

```
tras inyectar un carácter, actividad tick a tick:
  tick +1:  0 disparos
  tick +2:  0 disparos
  ... todos 0
```

Y el motivo es una transición de fase sin punto medio. Barriendo el peso inicial:

| peso | comportamiento |
|---|---|
| 0,03 | red muerta: 0 disparos por carácter |
| 0,07 | 1–3 disparos, se apaga en el acto |
| 0,085 | **saturada**: 135 neuronas a 4,8 de voltaje, permanentes |
| 0,12 | saturada, sin cambios |

Entre 0,07 y 0,085 no hay comportamiento intermedio. Una red recurrente sin
freno tiene exactamente dos estados: muerta o encendida. Cualquier intento de
"ajustar el peso" choca contra esa pared.

Lo que falta es un **mecanismo de freno que no sea un recorte duro**. El k-WTA
que había (8 ganadores) mataba la reverberación por diseño: en el mismo tick en
que la actividad intentaba sostenerse, quedaba recortada a 8 neuronas. Quitarlo
produjo la saturación. Ni una cosa ni la otra.

### Resuelto: inhibición recurrente

Se implementaron interneuronas de verdad: 15 % de cada capa, umbral más bajo
(+0,25 accesibles), proyección **divergente** (×5 el abanico de una excitatoria)
y **retardo 3 frente a 1-32 de la excitación**. Las sinapsis llevan signo, y las
inhibitorias no se aprenden: el freno sostiene la red, no se adapta.

Resultado medido, barriendo el peso inicial:

| peso | comportamiento |
|---|---|
| 0,06 | red muerta |
| 0,075 | 3 disparos, se apaga en 5 ticks |
| **0,11** | **15 disparos, reverbera 17 ticks y se apaga sola** |
| 0,13 | saturada, no se apaga nunca |

**Apareció el punto medio que antes no existía.** La transición dejó de ser una
pared: hay una ventana de ganancia donde la red reverbera y vuelve sola al
reposo. Eso es memoria de trabajo, y es la condición para que haya contexto.

Efecto colateral interesante: las sinapsis subieron de 3.062 a 4.887 porque las
interneuronas proyectan mucho más ancho. Es el coste de tener freno específico.

### Bugs encontrados en la codificación de entrada

Dos defectos reales, encontrados midiendo en lugar de suponiendo:

**1. Colisión de patrones sensoriales.** La proyección de los 8 bits del código
de carácter usaba siempre las mismas ~24 neuronas para todos los símbolos. Un
símbolo con código 0 **no encendía absolutamente nada**. Corregido: cada símbolo
tiene ahora un parche contiguo desplazado (identidad) más un conjunto
pseudoaleatorio dependiente del símbolo (textura). El uso de neuronas subió del
34 % al 72 % y el entrenamiento pasó de 10 a 19 pasos/s.

**2. Normalización global de las features.** Con 4 bloques de traza y 1 de tasa
en un mismo vector, normalizar el conjunto dejaba la tasa con un 20 % de la
norma: la identidad del símbolo actual se volvía invisible. Corregido:
normalización **por bloque**, cada canal con su propia escala.

Queda un tercer problema, medido y no resuelto: la separabilidad entre símbolos
distintos es solo **+0,09** (coseno 0,907 entre claves distintas contra 0,87 del
mismo símbolo repetido). Las representaciones se parecen demasiado entre sí.

### RESUELTO: memoria de trabajo confirmada

**100 % de acierto.** 40 claves distintas, 10 símbolos de relleno variable,
recuperación de la clave. Azar: 0,8 %.

```
epoca  1  acierto   0.0 %
epoca 11  acierto 100.0 %
epoca 60  acierto 100.0 %
```

Para llegar aquí hicieron falta tres cosas, y las tres eran bugs de verdad:

**1. Banco de slots temporales.** Las trazas decaían con constantes de tiempo
medidas en *ticks*, y un símbolo ocupa 5. Con la clave a 50 ticks de distancia,
desaparecía. Ahora hay 8 slots explícitos: cada uno guarda el perfil de
actividad de un símbolo anterior, y el banco se desplaza. La ventana de memoria
es explícita en vez de depender de un decaimiento exponencial.

Verificado: misma clave con distinto relleno → coseno 0,57; clave distinta →
0,19. La identidad sobrevive al relleno.

**2. El bench arrastraba estado entre ensayos.** La actividad del ensayo anterior
contaminaba el actual. Se resetea el estado al empezar cada ensayo. Sin esto, el
bench daba 0 % incluso teniendo features perfectamente utilizables. Fue el
motivo real del "no recuerda ni el último símbolo": éramos nosotros midiendo mal.

**3. El entrenamiento tampoco reseteaba entre secuencias.** Corregido también.

### El test de memoria, y su resultado real (histórico)

Se añadió `cortex_lm bench`: memorizar una clave, N símbolos de relleno, recordarla.

**Versión fácil** (1 clave, 6 de relleno): 100 %. **Falso positivo.** El readout
solo tenía que leer "qué símbolo apareció", no recordarlo.

**Versión dura** (40 claves distintas, 10 de relleno):

```
epoca  1  acierto   1.2 %  (1/80)
epoca 60  acierto   0.0 %  (0/80)
sin relleno:   3.8 %
con relleno:   0.0 %
```

3,8 % sin relleno está en el nivel del azar (1/120 = 0,8 %). Es decir: **el
readout no puede ni identificar qué clave se acaba de presentar**, mucho menos
recordarla después del relleno.

Esto corrige la lectura anterior. La reverberación de 17 ticks que se midió sí
existe, pero **no transporta la identidad de la clave**. Es actividad, no
información. Son cosas distintas y la diferencia es todo el proyecto.

### Lo que esto NO arregla todavía

El corpus no mejora por sí solo: sigue en ~4,4 bpc. La reverberación existe pero
el readout todavía no la aprovecha, y la regla de aprendizaje local no ha
demostrado que ajuste la recurrencia de forma útil.

Orden correcto de aquí en adelante:

1. **Arreglar la codificación de identidad.** Que la clave deje una huella
   distinguible en el estado y que esa huella sobreviva al relleno. Hoy no
   ocurre. Candidatos: entrada más fuerte y más dispersa, trazas por neurona que
   se actualicen con el disparo (no con la tasa), o codificación por latencia.
2. Repetir el bench duro hasta que pase. Es el criterio objetivo.
3. Solo entonces, volver al corpus.

### Opciones descartadas, con el motivo

1. **Inhibición recurrente real**, como en la versión en Python: interneuronas
   con sinapsis propias, retardos y pesos. No una resta global (probada, no
   funciona porque frena a todos por igual e impide cualquier umbral).
2. **k-WTA con ventana temporal** en vez de por tick: se acumula actividad y se
   recorta cada N ticks, no cada tick. Permite reverberación dentro de la ventana.
3. **Umbral adaptativo más agresivo**: la neurona que dispara mucho sube su
   umbral lo bastante como para ceder el turno. Ya existe (`theta_step`) pero su
   escala es demasiado fina para frenar una población entera.

**Ninguna de las tres está implementada.** El motor actual funciona, aprende
bigramas y no tiene memoria. Eso está comprobado y es reproducible.

## Lo que hay que arreglar, en orden

1. **Canal de contexto.** Que la red represente "lo que vino antes" y no solo
   "lo que está ahora". Dos vías: (a) memoria de elegibilidad por neurona que
   persista entre símbolos, (b) retardos más largos que crucen la frontera entre
   símbolos. Probablemente las dos.
2. **Validar la regla local** con una tarea donde el contexto importe (memoria
   de trabajo, no texto). Si no aprende ahí, no aprenderá texto.
3. **Escalar.** 256 neuronas no es el objetivo; 4.096 sí. El motor lo soporta,
   falta medirlo.

## Cifras del motor

- 256 neuronas, 3.062 sinapsis, 1 capa: ~10 pasos/s en este contenedor
- Objetivo fase 2: 4.096 neuronas, 2 capas, ~2,5 M sinapsis
- Memoria estimada del objetivo: ~30 MB

## Regla de este proyecto

Cada afirmación de arriba tiene un número detrás. Si algún día no lo tiene, está
mal y hay que decirlo.
