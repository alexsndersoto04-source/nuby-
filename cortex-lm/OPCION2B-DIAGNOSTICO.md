# ACTA — Qué intenté para batir los 2 caracteres (y qué falló)

Fecha: 2026-11-04. Rama `arena/c7deda7f-ask`.

## De dónde venimos

`OPCION2-RESERVORIO.md`: reservorio de 1.500 neuronas + lector local =
**2,945 bpc**, bigrama 3,292. Gana al bigrama, pero pierde contra un contador de
**2 caracteres** (2,775) por 0,17. Esta acta es el intento de cerrar esa brecha.
Las cuatro cosas que probé **empeoran**, y se documentan para no repetirlas.

## 1. Peso de mezcla aprendido (¿el lector estaba infrautilizando el contexto?)

Primer sospechoso: al entrenar estado y contexto juntos en un solo lector, el
contexto corto queda dominado por el reservorio. Se separaron en **dos lectores
independientes** (misma regla local) y la mezcla se aprendió aparte, con señal
local.

Resultado, medido en el test, con las tres cifras a la vista:

```
solo reservorio   2,942 bpc
solo contexto 2   3,076 bpc
mezcla (2,939)    peso dado al contexto: 0,072
```

El peso aprendido converge a 7 %: **la mezcla no gana nada**. Y el diagnóstico que
importa es el de en medio: un lector sobre los dos últimos caracteres da 3,076,
es decir **0,30 peor que un contador con esos mismos dos caracteres**. No era un
problema de mezcla: es que el lector lineal es peor que contar.

Comprobado que no era falta de entrenamiento: con 12 pasadas da 3,067 (0,009
mejor que con 4). Convergido.

## 2. Escalas de memoria (unas neuronas rápidas, otras lentas)

Idea biológica razonable: no todas las neuronas olvidan igual. Se le dio a cada
neurona su propia constante de fuga, repartidas en escala logarítmica. Con
train 100 k:

| escalas | bpc |
|---|---|
| 1 (todas iguales) | **3,008** |
| 4 | 3,124 |
| 16 | 3,244 |

Empeora. Con un reservorio grande las escalas lentas solo añaden estados que
apenas cambian y el lector gasta capacidad en ellos.

## 3. Contexto en la entrada del reservorio (ya medido antes)

Símbolo anterior por una segunda proyección fija: 3,049 → 3,168 (beta 0,5) →
3,498 (beta 1,0). El reservorio ya tiene esa memoria; repetirla en la entrada solo
crea estados redundantes.

## 4. Tasas de aprendizaje

| tasa | bpc |
|---|---|
| 0,02 | 2,942 |
| **0,05** | **2,939** |
| 0,10 | 3,001 |

## Veredicto

- **Lo que funciona y está medido**: reservorio + lector local = **2,939 bpc**
  frente a 3,292 del bigrama (−0,353) y 3,326 de la ventana congelada de antes
  (−0,387). Esa parte es sólida.
- **Lo que no funciona**: batir a un contador de 2 caracteres. El límite no está
  en el peso de mezcla, ni en las escalas de memoria, ni en la tasa. Está en que
  **(a)** el lector lineal es peor que contar cuando la entrada es one-hot de
  contexto, y **(b)** el estado del reservorio resume el pasado en un vector de
  1.500 números que el lector lineal tiene que convertir en 70 probabilidades.

- **Lo siguiente que sí tendría sentido** (no hecho, requiere decisión): apilar
  una segunda etapa — que las 1.500 neuronas de estado alimenten una capa
  recurrente más pequeña antes del lector. Eso son dos etapas de memoria y es lo
  único que ataca el punto (b). No es rápido ni seguro: hay que medirlo con las
  mismas 5 semillas y contra la misma referencia, y no se ha empezado.

## Reproducir

```
cd cortex-lm
g++ -O3 -march=native -std=c++17 tools/bench_res.cpp -o bin/bench_res

# lo que funciona
bin/bench_res 254096 20000 1500 0.35 0.9 1234 1.0 10 0 0 0.05 4 0 8 0 0

# las tres cifras del diagnostico de mezcla
bin/bench_res 254096 20000 1500 0.35 0.9 1234 1.0 10 2 0 0.05 4 0 8 0 0

# solo contexto, sin reservorio, para ver que el lector es peor que contar
bin/bench_res 254096 20000 0 0.35 0.9 1234 1.0 10 2 0 0.05 12 0 8 0 0
```

Uso completo: `bench_res NTR NTE N ALFA RHO [SEED] [LAMBDA] [P0] [C] [METODO]
[TASA] [PASADAS] [DECAE] [GRADO] [BETA] [ADAGRAD] [ESCALAS]`.
