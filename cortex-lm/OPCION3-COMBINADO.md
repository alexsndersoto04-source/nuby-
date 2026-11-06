# ACTA — Red + contador: el mejor sistema hasta ahora

Fecha: 2026-11-04. Rama `arena/c7deda7f-nuby`.

## Por qué se cambió de estrategia

Antes de esta acta se atacó el muro de la salida (que el lector lineal sea peor
que contar) con tres variantes más. **Todas empeoran**:

| variante | bpc | frente a 3,008 (el reservorio sin tocar) |
|---|---|---|
| segunda etapa recurrente de 300 | 3,026 | +0,018 |
| segunda etapa de 400 | 3,037 | +0,029 |
| segunda etapa de 800 | 3,058 | +0,050 |
| velocidad del estado (diferencia) | 3,115 | +0,107 |
| normalizar el estado por neurona | 27,416 | roto, descartado |

Diagnóstico que se saca: **el límite no está en la salida**. Añadir capacidad al
lector o cambiar los rasgos no mueve nada. El reservorio con lector local se queda
en 2,94–3,01 con este texto, y ahí se acaba lo que da de sí.

Nota sobre la normalización: salió 27 bpc porque dividir por una desviación casi
cero hace explotar las neuronas mudas. Se ha **quitado del código** para no dejar
una trampa; no cuenta como resultado negativo válido.

## El cambio de estrategia

En vez de seguir persiguiendo un número con la red sola, se construye el mejor
sistema posible **combinando** las dos cosas que sabemos que funcionan por
separado, y se mide lo único que demuestra si la red aporta algo:

> el mismo sistema con red y **sin** red, contra el mismo objetivo.

Combinación: media de probabilidades con **peso adaptable aprendido en línea**
(cada modelo se lleva el peso según quién acierta). Nada ajustado sobre el test.

Los dos predicen **exactamente el mismo objetivo**: el siguiente carácter.

## Resultado (Quijote, train 254 k, test 20 k, 5 semillas)

| sistema | bpc |
|---|---|
| contador de 1 carácter (bigrama) | 3,292 |
| reservorio solo | 2,942 ± 0,002 |
| **contador de 3 caracteres solo** | **2,442** |
| **contador de 3 + reservorio** | **2,361 ± 0,001** |
| contador de 4 caracteres | 2,397 |
| contador de 4 + reservorio | 2,194 |
| contador de 5 caracteres | 2,570 |
| contador de 5 + reservorio | 2,175 |

Las 5 semillas del combinado con contador de 3: 2,361 · 2,363 · 2,363 · 2,362 ·
2,361. Desviación 0,001.

**Números redondos de la mejora:** −0,081 bpc frente al contador de 3 solo, y
−0,581 bpc frente al reservorio solo. La escalera que antes era
3,292 → 2,942 → 2,442 ahora acaba en **2,361**.

## Atribución honesta del mérito

Un −0,081 tan pequeño hay que partirlo. Se midió el combinado **sin reservorio**
(red sustituida por nada): da 2,397.

| de dónde viene la mejora | bpc |
|---|---|
| contador de 3 solo | 2,442 |
| + combinar con un modelo débil (sin red) | 2,397 (−0,045) |
| + combinar con el reservorio | 2,361 (−0,081) |

Es decir: **0,045 vienen de combinar** (un truco de mezcla) y **0,036 del
reservorio**. Los dos son reales y están medidos, pero no hay que vender el total
como si fuera todo de la red de pulsos.

**Lo que sí es un hallazgo fuerte:** el reservorio aporta información que el
contador de 3 caracteres **no tiene**. Si el contador lo supiera todo, el peso
adaptable habría puesto el peso al contador y la mejora habría sido cero. No lo
hizo. Eso es exactamente lo que hay que demostrar de una red de pulsos.

## Lo que esto significa para el proyecto

1. **La línea local funciona como componente.** No es una curiosidad: mejora a un
   modelo de contexto de 3 caracteres, con 5 semillas y desviación de 0,001.
2. **No funciona sola.** Como único modelo se queda en 2,94, por debajo de un
   contador de 3. La idea de "una SNN que aprende lenguaje sola" no está
   demostrada y, con estos datos, no se sostiene.
3. **El camino que queda abierto** es híbrido: la parte local aporta lo que el
   contador no puede (generalización entre contextos parecidos) y el contador
   aporta la memoria exacta. Eso no viola la regla de pureza: los contadores no
   son backprop, son conteo.
4. **El techo sigue estando en los datos.** Con 4,5 M de caracteres, un contexto
   exacto de 4 tiene 0,02 ejemplos por caso. El combinado de 4 caracteres ya
   llega a 2,194 con este texto; con más texto bajaría. Pero bajar bits por
   carácter no es hablar.

## Reproducir

```
cd cortex-lm
g++ -O3 -march=native -std=c++17 tools/bench_res.cpp -o bin/bench_res

# mejor sistema: contador de 3 + reservorio
bin/bench_res 254096 20000 1500 0.35 0.9 1234 1.0 10 0 0 0.05 4 0 8 0 0 1 0 0.8 1.0 0 3

# la misma medida SIN reservorio (para atribuir el merito)
bin/bench_res 254096 20000 0 0.35 0.9 1234 1.0 10 0 0 0.05 1 0 8 0 0 1 0 0.8 1.0 0 3
```

Uso: `bench_res NTR NTE N ALFA RHO [SEED] [LAMBDA] [P0] [C] [METODO] [TASA]
[PASADAS] [DECAE] [GRADO] [BETA] [ADAGRAD] [ESCALAS] [N2] [RHO2] [BETA2] [VEL] [CP]`.
