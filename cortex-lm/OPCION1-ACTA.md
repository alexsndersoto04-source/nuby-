# ACTA — Opción 1: alfabeto real + texto completo

Fecha: 2026-11-04. Rama `arena/c7deda7f-nuby`.

## Qué se hizo y por qué

La crítica era justa: se había atribuido el fracaso a "el alfabeto de 256" y a
"pocos datos" sin medirlo. Aquí se miden las dos cosas, sobre **texto español
real** (El Quijote, 323.158 caracteres, alfabeto 70 símbolos). El desastre
anterior se midió con 4,5 M de caracteres; aquí hay menos texto, así que además
se mide **cómo cambia la desventaja con el tamaño del train**, que es justo la
variable que se quería explorar.

Banco nuevo: `cortex-lm/tools/bench_full.cpp`.
- Entrena **en flujo** (memoria constante): leer 4,5 M con el banco anterior
  exigía guardar 18 GB de features.
- Lector local con softmax **solo sobre las clases que existen** (no 256).
- Referencias con **retroceso a orden menor** (Jelinek-Mercer, alfa = 1). Sin
  retroceso, un contexto nunca visto se castigaba con 16 bits y la referencia
  salía peor que el bigrama: era una comparación falsa. Comprobado que sin este
  arreglo la referencia de 3 caracteres daba 4,267 bpc, es decir **peor que la de
  1** (3,386), que es imposible en un contador sano.

Particiones: train = 80 % inicial del texto, test = tramo siguiente (nunca
visto), en orden. Partición mezclada descartada (ya se demostró que invalida el
bigrama).

## Resultado 1 — el alfabeto no era el problema

Mismo texto, mismo test, 2 pasadas, 3-5 semillas. La red de pulsos sola
(ventana de 8 estados, 512 features) contra el bigrama:

```
train   120.000   3,866 ± 0,016   bigrama 3,320   +0,546
train   254.093   3,653 ± 0,015   bigrama 3,299   +0,354
```

Con **70 símbolos** en vez de 256, la desventaja sobre el bigrama sigue ahí.
El alfabeto explicaba una parte pequeña, no la causa.

## Resultado 2 — el tamaño del corpus no cierra la brecha

La brecha se estrecha al crecer el train (+0,546 → +0,354), pero en el tramo
medido se queda en ~0,35 bpc **por encima** del bigrama. El corpus de Kaggle
tiene 4,5 M: extrapolar no es medir y no se hace aquí, pero 0,35 bpc no es la
tendencia que llega a cero sola.

## Resultado 3 — la causa medida: el lector estaba sin entrenar, y aun así no gana

Con 2 pasadas el train sigue en 4,09 bpc: el lector no había convergido. Con 8
pasadas, misma red, misma ventana, 3 semillas:

```
                        train final   TEST    bigrama   diferencia
semilla 1234              3,303       3,318    3,299     +0,019
semilla 7                 3,330       3,340    3,299     +0,041
semilla 99                3,304       3,321    3,299     +0,022
                            media     3,326 ± 0,012             +0,027
```

Es decir: **la red de pulsos, ya entrenada, empata con el bigrama y no lo gana**.
Un contador de 1 carácter es la vara más baja que existe. Empatar con ella no es
aprender lenguaje.

## Resultado 4 — la ventana no aporta nada por encima del último carácter

Lector **solo** con los últimos C caracteres (sin red), 254 k de train, 5 semillas:

```
C = 2 chars (512 feat)   3,131 ± 0,000   referencia mismo contexto 2,781   +0,350
C = 3 chars (768 feat)   3,050 ± 0,000   referencia mismo contexto 2,441   +0,609
```

Y la red (memoria de 8 estados) sola: 3,653. Es decir:

- la red **no bate** a mirar los 2-3 últimos caracteres;
- un lector lineal sobre contexto directo tampoco bate al contador, y con C = 3
  pierde **más** que con C = 2 (más parámetros, mismas 254 k muestras);
- la información de contexto largo existe (la referencia de 3 chars baja a 2,441)
  pero ni la red ni el lector saben sacarla.

## Conclusión

1. Alfabeto real: **no era la causa**. Descartado con medida.
2. Corpus completo: **no se mide aquí** (el texto disponible son 323 k). Lo que sí
   se mide es que la brecha no se cierra al crecer el train en el tramo medido.
3. La causa localizada es la **representación**: la ventana congelada de 8 estados
   no contiene más información que el último carácter, y el lector local no puede
   explotar contexto largo aunque el dato esté ahí.

Por el criterio de parada acordado: la Opción 1 **no cumple** el umbral ("batir a
la referencia del mismo contexto"). Lo que sigue, si se quiere seguir, es la
Opción 2 (estado recurrente comprimido que se actualiza, en vez de ventana
congelada). No se ha hecho: necesita aprobación.

## Reproducir

```
# 1) generar particiones (cualquier .txt español en UTF-8)
python3 - <<'EOF'
d = open('quijote.txt','rb').read().replace(b'\r\n', b'\n')
n = len(d); tr = int(n*0.80); te = int(n*0.95)
open('cortex-lm/data/real_train.txt','wb').write(d[:tr])
open('cortex-lm/data/real_hold.txt','wb').write(d[tr:te])
EOF

# 2) compilar y medir
cd cortex-lm
g++ -O2 -std=c++17 -Iengine engine/cortex_lm.cpp tools/bench_full.cpp -o bin/bench_full
bin/bench_full 254000 20000 64 8 0 0.05 1234 0 8   # red sola
bin/bench_full 254000 20000  0 1 3 0.05 1234 0     # solo contexto directo
```

Uso: `bench_full NTR NTE K LH C TLR [SEED] [DECAIMIENTO] [EPOCAS]`.
`K=0` desactiva la red (mide solo contexto directo), `C=0` desactiva el contexto
directo. Imprime siempre la referencia del **mismo** número de caracteres y la
diferencia con signo.
