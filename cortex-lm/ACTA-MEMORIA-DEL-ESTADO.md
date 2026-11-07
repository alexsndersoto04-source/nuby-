# Acta — cuánta memoria tiene de verdad el estado (y dos instrumentos que estaban mal)

**Fecha:** 2026-11-05 · Rama `arena/c7deda7f-nuby`

## 1. Lo que quedaba abierto

En el acta anterior la conclusión fue: *el estado de la red apenas cambia entre
caracteres*. Esa conclusión venía de una varianza, y una varianza **no dice si
el estado se acuerda de algo**. Así que lo primero fue medir memoria de verdad:

- **`tools/diag_var.cpp`** — separa la variación *entre neuronas* (patrón fijo:
  qué neurona responde a qué letra) de la variación *en el tiempo* (lo único que
  puede ser memoria). También mide la correlación del estado con el de hace 1 y
  4 caracteres.
- **`tools/diag_mem.cpp`** — la prueba directa: se calcula el estado medio de
  cada letra y se comprueba si, dado el estado, se puede adivinar **la letra
  actual**, **la anterior** o **la de cuatro antes**. Ajuste en la primera mitad
  del texto, prueba en la segunda.

## 2. Dos errores míos, encontrados y corregidos

Los apunto porque invalidaban números que ya había dicho:

1. **`diag_var` no dividía por el número de pasos**: la "varianza entre
   neuronas" que salió primero (282) era falsa; el valor real es 0,0014.
2. **`diag_mem` evaluaba con los mismos datos con los que ajustaba**. Con 2048
   dimensiones y 1200 muestras, el vecino más cercano acierta siempre: daba
   91,9 % de acierto de la letra anterior en una configuración que luego sacaba
   4,02 bits — imposible. Corregido con partición ajuste/prueba.

También apareció un tercer problema, este del banco: **el lector no normalizaba
la entrada**, así que configuraciones del reservorio con escalas distintas no se
podían comparar (con inhibición 0,005 se midieron 6,54 bits con 70 clases, y el
máximo posible son 6,13). Corregido con normalización por dimensión calibrada
una vez sobre texto y congelada.

## 3. Resultado: el estado es una foto, no una memoria

Con los instrumentos corregidos, partición ajuste/prueba, 2400 caracteres:

```
              acierto letra   letra        letra
              ACTUAL          ANTERIOR     de 4 antes
BASE              100.0 %      21.9 %       2.2 %
TICKS 24           99.8 %      22.1 %       2.3 %
RECURR 0.6         84.6 %      35.3 %      10.5 %
INH 0.005          98.8 %      64.4 %       8.5 %
```

Es el diagnóstico completo del proyecto en cuatro líneas:

- **La letra actual está perfectamente codificada (100 %).** El reservorio sí
  representa el estímulo; no está roto ni muerto.
- **La letra anterior está casi perdida (21,9 %).** La red **olvida** el
  carácter en cuanto entra el siguiente.
- Como solo ve el presente, el mejor lector posible sobre este estado es el
  **bigrama**. Y eso es exactamente lo que saca: **3,03 bits** frente al bigrama
  exacto del mismo texto, **3,245**. El motor está haciendo, con 1024 neuronas y
  8 ticks por símbolo, el trabajo de un contador de pares de letras.

Referencia del mismo corpus (60.000 caracteres): unigram 4,402 · bigrama 3,245 ·
trigrama 2,582 · tetragrama 1,899.

## 4. Lo que intenté para darle memoria, y qué pasó

### 4.1 Darle al lector los estados anteriores ("taps")

`Engine::tap_features` guarda la foto del estado al cerrar cada símbolo y
devuelve las `n` últimas. Medido, 5 épocas, semilla 1234:

```
TAPS=1 (lo de siempre)  2,86 bits   39,1 % palabras reales
TAPS=2                  2,90 bits   27,3 %
TAPS=3                  3,33 bits   14,3 %
TAPS=4                  3,34 bits   36,8 %
```

**No ayuda.** No añade información (el estado del carácter anterior es casi el
mismo que el del actual, porque el anterior ya casi no está) y multiplica las
dimensiones del lector por 2 o 4.

### 4.2 Subir la recurrencia (`w_scale` 0,6 y 0,9)

En la prueba de la sección 3, subir la recurrencia **sí** conserva más el pasado
(35 % y 64 % en lugar de 22 %). Pero al medirlo con el lector, con
normalización para que sea comparación justa:

```
                semilla 1234   7      99
BASE               3,47      3,46   3,46
RECURR 0.6         4,18      4,18   4,20
RECURR 0.9         4,44      4,44   4,44
INH 0.005          3,87      3,87   3,87
RECURR 0.6 + INH   4,44      4,44   4,44
```

**Más memoria y peor texto.** Aquí está el nudo del problema y conviene decirlo
sin rodeos: **una red que recuerda más es una red que se mueve menos**. Con
recurrencia fuerte el estado se queda prendido de su propio pasado, deja de
seguir el texto, y el lector acaba viendo siempre lo mismo (4,44 bits es casi el
valor de "no distingo nada": log2(70) = 6,13 y el unigram es 4,40). La memoria
que gana en la prueba de centroides es memoria de *quedarse quieto*, no de
*recordar la frase*.

Es decir: **no basta con añadir memoria; hay que añadir memoria que no ahogue el
presente.** Este reservorio, tal como está, no tiene esa separación: el mismo
canal que integra el pasado tapa la entrada.

## 5. Estado del código

- `Engine::tap_features` y `Engine::tap_hist_reserve` (nuevos, apagados por
  defecto: el lector usa 1 foto).
- `Params::pace_lock` / `pace_phase_reset` (acta anterior), documentados como
  medidos y descartados.
- `tools/diag_var.cpp` y `tools/diag_mem.cpp` (nuevos): miden el estado **sin
  lector**. Son ahora los instrumentos de referencia para cualquier cambio del
  reservorio.
- `tools/bench_bio.cpp`: lector con **normalización opcional** (arg 12, apagada
  por defecto para no romper la serie histórica), `w_scale` (arg 10), `inh_gain`
  (arg 11), taps (arg 9), ganancia del marcapasos (arg 8).
- Las tres piezas biológicas siguen implementadas y apagadas (acta anterior).

## 6. Qué significa para el objetivo

El motor no está "balbuceando por falta de modulación": está **repitiendo el
presente**. Con 3,03 bits ya supera al bigrama exacto y ronda el trigrama. Lo que
falta para escribir frases es que el estado **dure** sin dejar de **seguir** al
texto. Eso es un problema concreto y medible — las dos cifras de la sección 3,
"letra actual" y "letra anterior", son la prueba de si se ha conseguido — y es lo
que hay que atacar. Ninguna de las tres piezas propuestas lo toca, y por eso
ninguna ayuda.

## Reproducir

```bash
cd /home/user/nuby-/cortex-lm
g++ -std=c++20 -O3 -march=native -Iengine engine/cortex_lm.cpp tools/diag_mem.cpp -o /tmp/dmem
g++ -std=c++20 -O3 -march=native -Iengine engine/cortex_lm.cpp tools/diag_var.cpp -o /tmp/dvar
g++ -std=c++20 -O3 -march=native -Iengine engine/cortex_lm.cpp tools/bench_bio.cpp -o /tmp/bbio
cd /home/user/wk2
/tmp/dmem data/train.txt 2400 1234          # memoria del estado, sin lector
/tmp/dvar  data/train.txt 400 1234
# banco: NGEN SEED "JER,DOP,OSC,EP" mostrar FEAT PACE_GAIN TAPS W_SCALE INH NORM
/tmp/bbio data/train.txt data/dic.txt 200 1234 "0,0,0,3" 0 2 0.0 1 0.6 0 1
```
