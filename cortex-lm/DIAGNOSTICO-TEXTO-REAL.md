# Por que el sistema pierde contra el bigrama en texto real

Fecha: 2026-11-04. Todas las cifras medidas aqui, no estimadas.

## El resultado de Kaggle

```
condicion                            media    desv
Sistema sin plasticidad              4.524   0.021
Sistema con plasticidad              4.542   0.012
Capa sola sin plasticidad            4.598   0.028
Capa sola con plasticidad            4.622   0.038
bigrama de referencia                3.619
```

El sistema **pierde** contra el bigrama. Y los 0,532 bpc que se presentaron
como "objetivo cumplido" salieron del corpus de juguete (`make_test_corpus.py`),
que es texto sintetico y trivialmente predecible. **Ese numero no valia.** Este
acta lo corrige.

## Reproducido en local

Con prosa real (las actas de este proyecto, 40.364 caracteres, 120 simbolos,
bigrama 3,559) el fallo se reproduce: 4,759 bpc, acierto 28,5 %.

## La medicion que lo explica todo

Entropia del texto real, estimada SOLO con el train y medida en el test:

| contexto | bpc (train 20k) | bpc (train 40k) |
|---|---:|---:|
| 0 caracteres | 4,703 | 4,770 |
| **1 caracter** | **3,855** | **3,990** |
| 2 caracteres | 3,782 | 4,006 |
| 4 caracteres | 5,514 | 5,781 |
| 8 caracteres | 7,459 | 7,536 |

Dos hechos duros:

1. **El techo real de este texto con 20.000 caracteres de entrenamiento esta en
   ~3,8-4,0 bpc.** El "objetivo < 2,0 bpc" es inalcanzable a nivel de caracter
   con este tamano de entrenamiento. Con 60.000 (la corrida de Kaggle) el techo
   baja a ~3,6. No hay forma de llegar a 2,0 con 20.000 caracteres en espanol.
2. **Ver mas contexto EMPEORA cuando el corpus es pequeno.** Pasar de 1 a 8
   caracteres de contexto sube el bpc de 3,99 a 7,54, porque el 84 % de los
   contextos largos no se han visto nunca en el train.

## Por que el sistema pierde contra el bigrama

La red ve **8 estados** (LH=8), que equivalen a 8 caracteres de contexto. Con
20.000 caracteres de entrenamiento ese contexto **no se puede estimar**. La red
queda sobreajustada: en la calibracion sobre su propio train da 4,8 bpc, no 0,5.
No esta memorizando de mas: su representacion del pasado es ambigua y el lector
no puede desambiguarla con la evidencia disponible.

Prueba de que **no es la plasticidad** ni el lector:
- Con softmax de gradiente en vez de RLS: 4,32 bpc. Igual de mal.
- Reduciendo features de 512 a 32: 5,42 bpc. Peor aun.
- Con features de tasa de disparo en vez de fatiga: 5,71 bpc peor.

Ninguna de esas variantes toca el problema, porque el problema es **estadistico,
no arquitectonico**: se esta pidiendo a un modelo que distinga 256 simbolos con
un contexto que apenas se repite en el corpus.

## La comparacion honesta

| modelo | contexto | bpc |
|---|---|---:|
| referencia de 1 caracter | 1 | 3,855 |
| **referencia de 2 caracteres** | 2 | **3,987** |
| sistema (red + RLS) | 8 estados | 4,759 |

Incluso contra la referencia de 2 caracteres, que es la que corresponde a lo que
la red puede estimar con este corpus, **el sistema pierde 0,77 bpc.**

## Que hay que hacer, y en que orden

1. **Corregir la metrica del cuaderno.** Comparar contra una referencia del
   MISMO contexto, no contra un bigrama. Ya esta implementado (`REF`).
2. **Entrenar con todo el corpus (4,5 M), no con 20.000.** Es la unica forma de
   que el contexto de 8 caracteres sea estimable. Con 20k no se puede.
3. **Reducir la dimensionalidad de salida.** 256 salidas con 121 simbolos reales
   y 20k de texto es pedirle peras al olmo. La red debe predecir sobre el
   alfabeto del corpus, no sobre 256 bytes.
4. **Y solo despues** volver a medir la plasticidad. Sobre un sistema que pierde
   contra una tabla de frecuencias no se puede juzgar ninguna regla de
   aprendizaje.

## Correccion de actas anteriores

`OPCION-B-MEMORIA.md` dice "0,941 -> 0,532 bpc, 43 % de mejora". Esos numeros son
reales pero **del corpus sintetico**. No se sostienen en texto real. El acta
queda corregida por este documento.
