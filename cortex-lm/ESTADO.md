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

Opciones reales, en orden de preferencia:

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
