# Excepcion acordada: quitar la linea de retardo SOLO para medir (Paso 3)

Fecha: 2026-11-04. Decision del usuario: opcion B, una unica excepcion explicita
al criterio de parada, solo para medir y sin comprometerse a nada mas.

## Condiciones

- LH = 1: el lector ve UN solo estado. La unica memoria posible es la capa
  recurrente.
- Todo lo demas igual: train 8.000, test no visto 9.780, K = 64, RLS con
  temperatura elegida en texto no entrenado, regla de tres factores
  (Pasos 4 y 5), eprop_scale 0,5.
- 5 semillas por condicion.

## Resultado

| condicion | media | desv | rango | diferencia |
|---|---:|---:|---|---:|
| LH=1 sin plasticidad | 3,460 | 0,058 | 3,382 - 3,530 | - |
| LH=1 plasticidad eta 3e-5 | 3,476 | 0,046 | 3,403 - 3,523 | +0,016 |
| LH=1 plasticidad eta 3e-4 | 3,524 | 0,027 | 3,484 - 3,557 | +0,064 |
| LH=8 sin plasticidad | 0,941 | 0,110 | 0,802 - 1,062 | referencia |
| LH=8 plasticidad eta 3e-5 | 0,956 | 0,104 | 0,793 - 1,064 | +0,015 |

Todas las diferencias caen dentro de 2 sigma: **empate**.

## Control decisivo: la recurrencia, sin plasticidad, tampoco aporta

Con LH=1, la unica memoria es la recurrencia. Comparando el peso recurrente:

| w_scale | bpc (3 semillas) | media |
|---|---|---:|
| 0,0 (sin recurrencia) | 3,587 / 3,440 / 3,556 | 3,528 |
| 0,3 | 3,437 / 3,382 / 3,530 | 3,450 |
| 0,6 | 4,416 / 4,443 / 4,413 | 4,424 |

La recurrencia aporta **0,078 bpc** (de 3,528 a 3,450). El bigrama de
referencia es ~3,00. Es decir: **incluso con toda la memoria cargada sobre la
capa recurrente, esa capa no llega a igualar al bigrama, y con w_scale=0,6
empeora medio punto.**

## Veredicto

La hipotesis del Paso 3 era: "quitando el buffer, la plasticidad tendra un
trabajo real y mejorara". **La medicion la refuta.** Con el buffer quitado:

1. La plasticidad sigue empatando (y con eta alta empeora).
2. La capa recurrente, sin plasticidad, aporta 0,078 bpc de 3,528 posibles:
   casi nada.
3. Con mas recurrencia (w_scale 0,6) la red se degrada.

La conclusion no es "la plasticidad fallo". Es mas concreta y mas grave para la
arquitectura: **la capa de pulsos no tiene, hoy, capacidad de memoria util.**
Ni con plasticidad ni sin ella. El buffer de 8 estados no estaba tapando el
talento de la capa: estaba tapando su ausencia.

## Por que esto importa para cualquier continuacion

Cualquier regla de aprendizaje local que se pruebe sobre esta capa va a dar
cero, porque la capa no produce una representacion del pasado. El problema esta
una capa mas abajo: en la dinamica (fatiga, inhibicion, umbral), que olvida en
2-3 simbolos.

## Resultado global del proyecto

| hito | bpc | estado |
|---|---:|---|
| objetivo < 2,0 en espanol | **0,941** (media de 5 semillas, test no visto) | CUMPLIDO con holgura |
| aprendizaje con plasticidad local | empata con no tener plasticidad | NO DEMOSTRADO |
| capa de pulsos como memoria | 0,078 bpc de aportacion | INSUFICIENTE |

El objetivo numerico que se acordo al principio esta cumplido y superado. Lo que
no esta demostrado es que la plasticidad local sea lo que lo consigue: lo
consigue el lector RLS sobre las features de la capa, y el estado de la capa.
