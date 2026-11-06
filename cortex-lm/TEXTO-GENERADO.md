# Lo que escribe el sistema (la prueba que se puede juzgar sin números)

Fecha: 2026-11-04. `tools/escribir.cpp`.

## Cómo se ha generado

Sin trampas: se entrena el reservorio + lector local con el train, se le dan los
últimos 40 caracteres como arranque y se le pide que continúe. Se muestrea de la
distribución que da el modelo (no se coge siempre lo más probable, que se queda
en bucle). Nada de copiar frases del original.

## Lo que sale

```
 o a cumato, quelaraño, que re ton, ne

etiran. corta, cudo de la frto.

 títas a Din Quijotes, se llocro y al hebajaren anfiero da
jutrte: amuno or cos tedí, piro paza lqui bendadericade da brofarro que pare. jo
vientraestea, y puda el el as entede
comello fienterapítirá dos paranda del cumotode aque er pocey coded: ues a to  o
enco do ellióna rascimatidepentel porque; mes padienteo, mijose
```

## Qué se puede ver ahí, sin saber nada de números

**Lo que sí ha aprendido:**
- Las palabras **tienen forma de palabra española**: "cumato", "quelaraño",
  "hebajaren", "fienterapítirá", "rascimatidepentel". No son letras al azar.
- Acentúa donde toca: "títas", "tedí", "pítirá".
- Usa puntuación y mayúsculas en sitios razonables: "Din Quijotes", coma, punto.
- Los espacios caen entre palabras.
- Hay trozos de palabras reales: "corta", "cudo", "piro", "paza", "pare".

**Lo que no ha aprendido:**
- **No dice nada.** Ninguna palabra inventada significa algo.
- No mantiene el hilo: puede empezar hablando del Quijote y acabar en una palabra
  inventada de 19 letras.
- No hay frases: hay trozos con forma de frase.

## La conclusión honesta, en una línea

El sistema aprendió **cómo se escriben las palabras** y **no** aprendió **qué
significan**. Sabe deletrear español inventado; no sabe hablar.

Esto es exactamente lo que se espera de un modelo que solo mira las últimas 3
letras y un resumen borroso de lo anterior: la forma de la palabra está en esas
letras, el significado no.

## Reproducir

```
cd cortex-lm
g++ -O3 -march=native -std=c++17 tools/escribir.cpp -o bin/escribir
bin/escribir 1234 400 1.0
```
