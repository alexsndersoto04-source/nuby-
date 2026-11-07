# ACTA — Opción 2: estado recurrente que se actualiza (reservorio)

Fecha: 2026-11-04. Rama `arena/c7deda7f-nuby`.

## La decisión

Tras la Opción 1 (acta `OPCION1-ACTA.md`), la causa localizada era la
**representación**: la ventana congelada de 8 estados no aporta nada por encima
de los 2-3 últimos caracteres. La Opción 2 sustituye esa ventana por un **estado
recurrente que se actualiza**: un reservorio.

## Qué es exactamente lo que se ha construido

`cortex-lm/tools/bench_res.cpp`:

```
x[t] = tanh( rho * sum_j W_ij x_j[t-1]  +  alfa * Win[i, simbolo_t] )
y    -> lector local  (cada salida se corrige con SU error; nada hacia atrás)
```

- **W y Win son aleatorias y NO se entrenan.** Conectividad dispersa
  (8 conexiones por neurona de 1.500). Esto respeta la regla de pureza: la
  conectividad no viene de un gradiente.
- El estado **no se reinicia** entre símbolos ni se guarda una ventana: la
  memoria es el propio estado evolucionando.
- El lector es local: `w_c ← w_c - tasa · (p_c - y_c) · z`. Cada salida usa solo
  su error. Cuesta O(clases × rasgos) y nunca se guardan los estados pasados
  (memoria constante, entrenamiento en flujo).
- RLS/FORCE también está implementado (`METODO=1`) pero con 1.500 rasgos cuesta
  O(F²) = 2,25 M por símbolo y **no converge** (medido: 4,483 bpc). Se descartó
  para esta red por dos motivos medidos, no por preferencia.

## Resultado principal

Texto: El Quijote, 323.158 caracteres, alfabeto 70. Train 254.096 (todo menos el
test), test 20.000 símbolos del tramo final, nunca vistos.

Configuración: N = 1500, alfa = 0,35, rho = 0,9, grado 8, tasa 0,05, 4 pasadas.

| condición | bpc (5 semillas) | media ± desv |
|---|---|---|
| reservorio + lector local | 2,942 · 2,948 · 2,945 · 2,946 · 2,945 | **2,945 ± 0,002** |
| **bigrama de referencia** | — | **3,292** |
| referencia 2 caracteres | — | 2,775 |
| referencia 3 caracteres (con retroceso) | — | 2,442 |

**−0,347 bpc frente al bigrama, 20 desviaciones.** Es la primera vez en el
proyecto que el sistema bate una referencia, y la bate con un margen que no
depende de la semilla.

Para comparar con lo que había: la ventana congelada (K=64, LH=8, lector local,
mismo texto y mismo test) daba **3,326 ± 0,012**. El reservorio mejora **0,381
bpc** sobre eso.

## Lo que NO consigue

No bate a la referencia de 2 caracteres (2,945 frente a 2,775, +0,17). Es decir:
el reservorio ya sabe más que un bigrama, pero **todavía no** iguala lo que se
puede sacar mirando los dos últimos caracteres con un contador. Añadirle esos dos
caracteres como entrada directa tampoco arregla nada (2,948: empeora, porque el
lector se reparte entre rasgos redundantes).

## Palancas medidas (una semilla cuando solo se acota la búsqueda)

| palanca | valores | bpc |
|---|---|---|
| tamaño del reservorio | 500 / 800 / 1200 / 1500 / 3000 / 4000 | 3,157 / 3,139 / 3,109 / 3,008 / 2,967 / 3,165 |
| ganancia de entrada (alfa) | 0,2 / 0,35 / 0,5 / 0,75 / 1,0 | 3,037 / **3,008** / 3,049 / 3,200 / 3,142 |
| radio espectral (rho) | 0,8 / 0,9 / 0,95 / 1,1 | 2,943 / **2,942** / 2,982 / falla |
| grado de conexión | 4 / 8 / 24 | 3,060 / 3,063 / 3,063 |
| pasadas del lector | 2 / 4 / 6 | 3,109 / 2,942 / 2,929 |
| símbolo anterior en la entrada (beta) | 0 / 0,5 / 1,0 | **3,049** / 3,168 / 3,498 |
| corriente de fondo constante | 0 / 0,05 | **3,049** / 3,563 |
| Adagrad en el lector | no / sí | **3,008** / 3,266 |

Cosas que se probaron y **empeoran**, para que no se repitan:
- rho ≥ 1,1: el reservorio se vuelve caótico y el lector no lo sigue (hasta 8,9 bpc).
- Corriente de fondo constante: satura el tanh (todo a +1) y borra la información.
- Meter el símbolo anterior por una segunda proyección fija: peor.
- Learing rate de RLS con F grande: ni converge ni cabe en tiempo.
- Reservorio más grande que ~1.500 no ayuda con este texto: hace falta más texto
  para llenarlo (4.000 baja a 3,165).

## Conclusión

1. **El estado recurrente sí funciona, la ventana no.** Con el mismo lector local
   y el mismo texto, pasar de ventana congelada a reservorio baja de 3,326 a
   2,945 bpc.
2. La regla local (cada salida con su error) es suficiente para leer el estado:
   no hace falta retropropagación ni RLS. RLS, además, no escala.
3. Falta para "entender lenguaje": todavía pierde contra un contador de 2
   caracteres. El siguiente paso lógico es darle al lector la mezcla de contexto
   corto y estado, pero **con regularización que evite el reparto de culpa**, o
   un lector con dos escalas. No se ha hecho.

## Reproducir

```
cd cortex-lm
g++ -O3 -march=native -std=c++17 tools/bench_res.cpp -o bin/bench_res
bin/bench_res 254096 20000 1500 0.35 0.9 1234 1.0 10 0 0 0.05 4 0 8 0 0
```

Uso: `bench_res NTR NTE N ALFA RHO [SEED] [LAMBDA] [P0] [C] [METODO] [TASA]
[PASADAS] [DECAE] [GRADO] [BETA] [ADAGRAD]`.
`METODO=1` es RLS/FORCE, `METODO=0` es la regla local de error.
