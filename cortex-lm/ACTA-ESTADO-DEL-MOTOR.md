# ACTA — El fallo que tapaba todo: el motor no le enseñaba su estado

Fecha: 2026-11-05. Rama `arena/c7deda7f-ask`.

## El hallazgo

Todos los bancos que usaban el motor le daban al lector **solo la fatiga**
(`theta_features`): cuánto había disparado cada neurona hace poco. No le daban
**el estado de membrana**, que es lo que la red *está* pensando en ese momento.

Comparación con el mismo texto (60 k del Quijote) y el mismo lector local:

| qué se le da al lector | bits/carácter |
|---|---|
| solo fatiga (lo que hacía el motor) | 3,74 |
| **solo estado de membrana** | **3,19** |
| estado + fatiga | 3,16 |

Y en palabras reales sobre el texto generado: de **4 % a 9-11 %**.

Es el mayor salto obtenido en todo el proyecto con un cambio de una línea, y no
es una idea nueva: es que el motor **estaba tirando su propia salida**. La red
calculaba un estado y luego el lector tenía que adivinar el texto a partir de
cuánto se había fatigado cada neurona.

Código: `Engine::state_features()` y `Engine::full_features()` en
`engine/cortex_lm.hpp` / `.cpp`.

## Consecuencia incómoda: la dopamina ya no ayuda

Con el estado real delante, el tercer factor **deja de aportar**:

| configuración (3 semillas) | bits/carácter |
|---|---|
| estado + fatiga, sin dopamina | **3,14 / 3,15 / 3,16** |
| estado + fatiga, con dopamina | 3,30 / 3,31 / 3,33 |
| estado + fatiga, dopamina + refuerzo sobre lo que escribe | 3,30 / 3,31 / 3,33 |

Las palabras reales oscilan entre el 4 % y el 19 % en todas las configuraciones,
sin dirección clara. Con 20 palabras por muestra, esa métrica **no distingue**:
hacen falta muchas más para separar 9 % de 17 %.

**Por qué pasaba antes:** cuando el lector sólo veía la fatiga, la red no tenía
información suficiente y la dopamina aportaba algo (0,07 bpc). Al darle el estado
verdadero, el lector ya no necesita esa ayuda y la dopamina se convierte en
ruido: 0,16 bpc **peor**. El efecto que se midió ayer era real, pero su causa no
era la dopamina: era la falta de estado.

## Lo que se arregla y lo que no

**Arreglado:** el motor expone su estado. Es el cambio que más mejora la salida
de texto en todo el proyecto (3,74 → 3,14).

**No arreglado:** sigue inventando palabras. El texto ahora tiene más palabras
reales y más forma de frase:

```
encueniento? henó dijorenentúngosentrueneMvanijotrobrosen entosporaventos y encuentos pena
```

```
e otrecingo illos cuyenados, yoseros pos lday afues; enfestrgas eueriseros, y
fuermutiemiana dos, y es llenuros, y goberlladirmeríarnande cos, Sa; y eros
dero Eevenaídombásel más de a curen pesMa
```

Se lee "encuento", "encuentos", "ventos", "llenos", "gobierna". No significa nada,
pero ya no es balbuceo fonético suelto: son palabras españolas deformadas.

## Estado del motor

- `state_features()` / `full_features()`: **usar siempre**. Es lo correcto.
- Dopamina: implementada y medida, **no mejora** con el lector completo.
- Jerarquía lenta: implementada y medida, no mejora.
- Oscilador: implementado y medido, empeora mucho (4,45).

De las tres piezas que se pidieron, ninguna mejora el motor cuando el lector ve
su estado real. Lo que sí mejora el motor es enseñarle su propio estado.

## Reproducir

```
cd cortex-lm
g++ -std=c++20 -O3 -march=native -Iengine engine/cortex_lm.cpp \
    tools/bench_bio.cpp -o bin/bench_bio

# JER,DOP,OSC,EPOCAS   y el ultimo numero: 0 fatiga, 1 estado, 2 ambas
bin/bench_bio data/train.txt data/dic.txt 200 1234 "0,0,0,1" 1 2
bin/bench_bio data/train.txt data/dic.txt 200 1234 "0,0,0,1" 1 0
```
