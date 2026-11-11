# ACTA — Las tres piezas biológicas: implementadas y medidas una a una

Fecha: 2026-11-05. Rama `arena/c7deda7f-ask`.

## Qué se ha implementado (código real en el motor)

Todo en `engine/cortex_lm.hpp` + `engine/cortex_lm.cpp`, dentro del bucle de
ticks del motor LIF. No es un banco aparte ni un ejemplo: modifica el ciclo de
ejecución.

### 1. Regla de tres factores (modulación dopaminérgica)

- `Engine::cargar_diccionario()` y `Engine::cargar_bigramas()`: cargan las
  palabras válidas y la estadística de bigramas del corpus.
- `Engine::juzgar_palabra()`: con la palabra que se está formando emite
  **+1** (existe en el diccionario), **−1** (ya tiene ≥3 letras y no existe) o
  **−0,5** inmediato si aparece un bigrama imposible ("qzz").
- `feed()` convierte ese evento en un **nivel de dopamina** con subida rápida y
  caída lenta (`dop_tau`): el refuerzo tiene cola y alcanza a las sinapsis que
  participaron, no solo a la última.
- `learn()` multiplica la actualización por `(1 + dop_gain·D)`. La dirección la
  sigue poniendo la STDP local (traza pre × traza post); la dopamina **solo
  decide si se consolida**. Eso es literalmente `Δw = η·STDP·D(t)`.
- La dopamina entra además como corriente difusa en la membrana.
- Se añadió también como tercer factor al lector (`mod` en `Lector::paso`).

### 2. Jerarquía temporal (rápida + lenta con retraso axonal)

- La capa se parte: `n_fast` neuronas con `tau_m_fast` (8 ticks) y el resto con
  `tau_m_slow` (80 ticks). La fuga se aplica **por neurona**, no por capa.
- La población lenta recibe la entrada sensorial atenuada (`slow_in_gain`).
  Sin esto quedaba **sorda** y su estado no codificaba nada.
- Vía lenta → rápida: matriz fija dispersa (`slow_fanout` destinos por neurona)
  y **buffer circular por neurona** de `slow_delay` ticks. Es el retraso axonal.
- Traza presináptica lenta aparte (`pre_trace_slow`, 6× más duradera).

### 3. Oscilador interno de control

- `n_pace` marcapasos con periodos no múltiplos (11, 29, 29,7, 49,3 ticks) y
  fases repartidas. Disparan solos, sin entrada.
- Su corriente entra como bias en la capa, más fuerte sobre la población rápida
  que sobre la lenta (`pace_bias_fast` / `pace_bias_slow`).
- La fase relativa entre marcapasos recorre muchos valores dentro de un bloque:
  esa es la "brújula" que dice en qué punto va la generación.

## Resultado: una pieza funciona, dos no

Texto de entrenamiento: 60.000 caracteres del Quijote. 200 caracteres generados,
muestreo a temperatura 1. 1 pasada de entrenamiento (para que la comparación sea
justa entre configuraciones). **Tres semillas.** La métrica que se puede juzgar
sin saber números: **qué porcentaje de las palabras generadas existe en español**.

| configuración | bits/carác. (3 semillas) | palabras reales |
|---|---|---|
| BASE (nada nuevo) | 3,73 / 3,74 / 3,74 | 0 / 1 / 1 de ~24 (≈2 %) |
| **DOPAMINA (tercer factor)** | **3,67 / 3,67 / 3,67** | 0 / 1 / 2 de ~24 (≈4 %) |
| JERARQUÍA (memoria lenta) | 3,76 / 3,77 / 3,77 | 0 / 1 / 3 de ~24 (≈6 %) |
| JERARQUÍA + DOPAMINA | 3,69 / 3,70 / 3,70 | 0 / 0 / 2 (≈3 %) |
| **OSCILADOR** | **4,45 / 4,45 / 4,45** | 0 / 0 / 0 |
| TODO (las tres) | 4,46 / 4,46 / 4,46 | 0 / 0 / 0 |

### Lo que funciona

**La modulación dopaminérgica, y de forma inequívoca:** 3,67 frente a 3,73–3,74
en las **tres semillas, con el mismo valor**. Es 0,07 bits por carácter, pequeño
pero reproducible. Y el juez funciona: durante el entrenamiento emite **7.359
refuerzos y 108 castigos** — casi todo lo que lee es palabra válida, lo cual es
correcto porque entrena con texto real.

### Lo que no funciona, y por qué

**El oscilador es un desastre: 4,45 frente a 3,74.** El motivo es claro y no hace
falta suponerlo: una corriente periódica **con el mismo periodo que el símbolo**
(`ticks_per_char = 8`, y los marcapasos de 11 y 29 ticks no son múltiplos de 8)
mete en la membrana una señal que depende del *tick* y no del *texto*. El lector
acaba leyendo el reloj en vez del contenido, y las palabras reales caen a cero.
Con la red actual, la "brújula" es ruido periódico.

**La jerarquía temporal no mejora las palabras y empeora un poco los bits**
(3,76–3,77). Se probó lo obvio por si era un fallo de implementación y no lo era:
la población lenta quedaba sorda y se le dio entrada (3,76 → 3,77, sin cambio).
El estado lento existe y se actualiza, pero el lector no saca provecho de él con
este texto y este tamaño.

## Lección que deja esto

La pieza que **sí** funciona es la que actúa sobre **el aprendizaje** (la
dopamina). Las dos que actúan sobre **la dinámica** (membrana lenta y oscilador)
no aportan, y el oscilador además destruye.

Eso encaja con todo lo medido antes en este proyecto: cambiar la dinámica de la
capa ya se intentó por cuatro vías (fatiga, ticks, radio espectral, escalas de
memoria) sin ganancia. Lo que ha movido la aguja siempre fue **el lector y la
regla de aprendizaje**.

## Recomendación concreta

1. **Dejar la dopamina activada.** Es la única pieza con efecto medido y
   reproducible. Ya está en el motor.
2. **Dejar el oscilador desactivado por defecto** (`n_pace = 0`). Está
   implementado y disponible, pero medido empeora 0,7 bits.
3. **Dejar la jerarquía desactivada por defecto** (`n_fast = 0`) hasta que haya
   más texto: su estado no se aprovecha con 60 k caracteres.

## Reproducir

```
cd cortex-lm
g++ -std=c++20 -O3 -march=native -Iengine engine/cortex_lm.cpp \
    tools/bench_bio.cpp -o bin/bench_bio

# cada pieza por separado: JER,DOP,OSC,EPOCAS
bin/bench_bio data/train.txt data/dic.txt 200 1234 "0,1,0,1" 1
bin/bench_bio data/train.txt data/dic.txt 200 1234 "1,0,0,1" 1
bin/bench_bio data/train.txt data/dic.txt 200 1234 "0,0,1,1" 1
```

El último argumento (`1`/`0`) imprime o no el texto generado.
