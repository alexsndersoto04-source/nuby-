# IA mínima — prueba de que se puede, a pequeña escala

Esto no es una demo falsa ni una simulación: es una red neuronal de verdad,
escrita a mano en Python puro (cero librerías, cero pesos descargados de
internet). Se entrena aquí mismo, en tu ordenador o en GitHub Actions, en menos
de un minuto.

## Verlo funcionando

```bash
python3 ia_minima.py
```

Salida real de una ejecución:

```
Corpus: 901 caracteres | vocabulario: 27 | parámetros: 10,443
Pérdida inicial (sin entrenar): 3.3004
Muestra inicial: 'el gato cytfmlrua vjt lseíz  níiej djmeeelf voqdñybhssíjvrg...'

paso  500 | pérdida 0.5022 | perplejidad   1.65
paso 2500 | pérdida 0.3584 | perplejidad   1.43

Pérdida final (todo el corpus): 0.1320
empezó en 27.1 opciones de duda, acabó en 1.14

Muestra entrenada:
  el gato mira la lluvia cae sobre el pueblo. el río sigue corriendo. el tejado.
  la mañana es clara y fría. la luna sale despacio...
```

Eso es aprendizaje real: antes escribía basura, después escribe español.

## Qué demuestra

1. **Se puede escribir una IA desde cero**, sin modelos ya hechos. El archivo
   tiene la red, el cálculo del error, la retropropagación y el optimizador
   escritos a mano.
2. **El gradiente está bien calculado.** Con `--verificar-gradiente` se compara
   el gradiente analítico con el numérico: si la matemática estuviera mal, no
   coincidirían. Coinciden.
3. **El cómputo es el límite, no el código.** Este modelo tiene 10.443 números
   ajustables y leyó 901 caracteres. Un chat competente tiene del orden de
   100.000.000.000 de números y leyó billones de palabras.

## Qué NO demuestra

- **No conversa.** Aprende el idioma de 31 frases sobre un gato. Fuera de eso,
  no sabe nada.
- **No generaliza todavía.** Con 10.000 parámetros y 900 caracteres, memoriza.
  Para generalizar hay que darle mucho más texto y más capacidad.
- **No es un asistente.** Es la prueba de que la maquinaria funciona.

## Decisiones que se pueden tocar

| Opción | Qué hace |
|---|---|
| `--pasos 2500` | cuántas veces repasa el texto (más = mejor, y más lento) |
| `--lr 0.004` | tamaño del paso al corregir. Muy alto = se vuelve loco |
| `--lote 16` | cuántos ejemplos mira a la vez |
| `--verificar-gradiente` | comprueba la matemática contra el cálculo numérico |
| `--cada 500` | cada cuántos pasos imprime una muestra |

## El siguiente paso, en orden de dificultad

1. **Cambiar el corpus** por uno tuyo (poesía, código, recetas, leyes...).
   Mismo código, otro idioma que aprende. Gratis y ya.
2. **Aumentar el modelo** a 1–5 millones de parámetros y pasar de mirar
   8 caracteres a mirar 256 (eso ya pide PyTorch y una GPU o mucha paciencia).
3. **Escribir un transformer** (la arquitectura de los chats actuales) en vez
   de esta red simple. Es la diferencia entre una calculadora lista y un motor.
