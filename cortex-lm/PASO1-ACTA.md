# Paso 1 del plan acordado: lector con RLS / FORCE

Fecha: 2026-11-04. Todas las cifras medidas en este repositorio.

## Criterio de parada (acordado)

"Si RLS no baja el bpc a ~2.0 en una sola pasada, lo cerramos. Si baja, el
sistema es entrenable y pasamos al Paso 2."

## Resultado: CRITERIO CUMPLIDO

Protocolo: train 8.000 simbolos (`data/spanish_small.txt`), test no visto
9.780 simbolos (segunda mitad de `spanish_holdout.txt`), K=64, LH=8, LAG=1.
La temperatura se elige sobre la PRIMERA mitad del holdout, nunca sobre el texto
donde se mide. Bigrama de referencia: 3,060 bpc.

| metodo del lector | bpc | acierto |
|---|---:|---:|
| descenso de gradiente, 1 epoca | 3.491 | 31.6 % |
| RLS (regla local), 1 epoca | **1.062** | **81.3 %** |
| RLS (regla local), 3 epocas | 1.064 | 81.4 % (ya convergio) |

RLS baja de 2,0 en UNA pasada. El sistema es entrenable.

## Por que RLS cumple la restriccion del proyecto

Cada neurona de salida c se ajusta asi:

    yhat_c = w_c . z + b_c
    e_c    = objetivo_c - yhat_c        <- SU error, el de ella sola
    w_c   <- w_c + g * e_c

donde `g` es una ganancia comun. Ninguna neurona necesita el error de las demas
ni el gradiente de la red. No hay retropropagacion: la senal no viaja hacia
atras, se queda en la sinapsis. Es el mismo tipo de regla local que STDP.

Dato honesto: `g` necesita los productos internos `z . P z`, que se pueden
calcular de forma distribuida (cada sinapsis suma su parte) pero son un paso
mas de lo estrictamente sinaptico. Lo apunto para no vender humo.

## Trampas encontradas por el camino (quedan documentadas)

1. **Una matriz P por clase**: 4,998 bpc. FORCE usa UNA P compartida por todas
   las salidas, porque todas ven la misma entrada. Con una por clase el coste se
   multiplicaba y no convergia.
2. **RLS hace regresion, no clasificacion**: 98,5 % de acierto con 4,05 bpc.
   Separa la clase correcta por ~1 nat; un softmax sobre 37 clases necesita ~4.
   Se resuelve con temperatura, elegida en texto no entrenado.
3. **Predecir el simbolo actual en vez del siguiente** (LAG=0): daba 0,086 bpc,
   una cifra imposible que delataba el error. Un BPC por debajo de ~0,5 en texto
   natural a nivel de caracter es senal de fuga de informacion.

## Hallazgo del Paso 2 (ruido), ya medido

Cinco semillas de la red (1234, 7, 99, 2024, 555), misma condicion:

    bpc: 1.062  0.802  1.014  0.850  0.975
    media 0,941 | desviacion 0,107 | rango 0,802 - 1,062

**La variacion entre inicializaciones (0,26 bpc de rango) es 85 veces mayor que
las diferencias de 0,003 bpc que se venian discutiendo.** Cualquier comparacion
de plasticidad hecha con una sola semilla y diferencias de milesimas era ruido
de semilla, no efecto. Esto valida el Paso 2 del plan y lo cierra con datos.

## Estado de la plasticidad (aviso)

Con el lector bueno, la plasticidad actual **no mueve los pesos**:
`media inicial 0.29374 -> final 0.29374 (cambio -2.13e-07)` con eta=1e-3.
No es que empeore: es que no hace nada. Eso es lo que hay que arreglar en los
Pasos 4, 5 y 6 (factor postsinaptico, error local propio, homeostasis).
