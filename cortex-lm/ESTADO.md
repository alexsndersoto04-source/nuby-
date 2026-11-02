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
