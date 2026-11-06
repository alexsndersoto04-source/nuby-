# Por qué todas las comparaciones de plasticidad estaban mal hechas

Fecha: 2026-11-04. Todas las cifras son medidas en este repositorio, no estimadas.

## El hallazgo

Nunca se dejó entrenar al sistema. Todas las comparaciones "sin plasticidad vs
con plasticidad" se hicieron con 2 u 8 épocas, y en ese punto el lector todavía
no había aprendido ni lo básico.

Curva medida (`bench_joint`, train 8.000, K=64, LH=8, LAG=1, sin plasticidad):

| épocas | bpc   | acierto |
|-------:|------:|--------:|
|      2 | 3.196 |   37.9 % |
|      4 | 2.750 |   47.2 % |
|      8 | 2.296 |   58.4 % |
|     16 | 1.895 |   67.7 % |
|     24 | 1.724 |   72.4 % |
|     32 | 1.640 |   75.4 % |

Monótona y SIN saturar en 32 épocas. Las 2 épocas de la corrida de Kaggle
(4,300 bpc, 22,4 % de acierto) no miden la arquitectura: miden que no había
entrenado. Cualquier diferencia de 0,003 bpc medida ahí es ruido sobre ruido.

## Lo que NO era el problema

Hipótesis descartada: que la normalización de features congelada antes de la
plasticidad castigara a la condición "con". Se añadió `bench_norm` (recalcula
media y desviación después de la plasticidad) y los números salen idénticos
hasta el tercer decimal: 1,100 / 1,103 con y sin recalcular. Descartada.

## El problema de fondo, medido

Con LH=1 (sin línea de retardo) y 16 épocas, train 8.000:

| condición | bpc | acierto |
|---|---:|---:|
| sin plasticidad | 3.397 | 31.4 % |
| con plasticidad (eta 3e-3) | 3.402 | 31.7 % |

El bigrama de referencia es 3,0. Es decir: **la capa de pulsos recurrente no
aporta nada a la predicción**. Todo el rendimiento sale de la línea de retardo
de 8 estados y del lector. La plasticidad mueve el número 0,005 bpc, que es
ruido.

Esto no es un fallo de la plasticidad: es que la capa recurrente no tiene, hoy,
ninguna información que ofrecer que el lector no tenga ya.

## Consecuencia para el plan

El cuello de botella no es la regla de plasticidad. Es el lector: un softmax de
256 salidas entrenado con descenso de gradiente a paso fijo. Ese lector es el
que hay que arreglar primero, y hay una forma de hacerlo que sigue siendo una
regla LOCAL, sin retropropagación: mínimos cuadrados recursivos (RLS), la regla
que usa FORCE learning. Cada neurona de salida actualiza sus propios pesos con
su propio error y la actividad que recibe. Nada viaja hacia atrás.
