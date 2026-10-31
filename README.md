# Nuby — IA desde cero

Repositorio vacío a propósito, para construir una IA propia sin usar modelos ya
entrenados. Nada de pesos descargados, nada de APIs de terceros, nada de
simulaciones: la red, el entrenamiento y la evaluación son nuestros.

## Estado real

| Pieza | Estado |
|---|---|
| Red neuronal escrita a mano, sin librerías | ✅ funcionando |
| Entrenamiento con retropropagación + Adam | ✅ funcionando |
| Verificación numérica del gradiente | ✅ coincide |
| Generación de texto con lo aprendido | ✅ funcionando |
| Tokenizador propio (BPE) | ⬜ pendiente |
| Transformer propio | ⬜ pendiente |
| Entrenamiento largo en GitHub Actions con checkpoints | ⬜ pendiente |
| Evaluación y demo publicada | ⬜ pendiente |

La prueba viva está en [`experimentos/00_ia_minima/`](experimentos/00_ia_minima/):
un modelo de 10.443 parámetros que aprende español de 31 frases en menos de un
minuto. Es diminuto, pero es real: la pérdida baja, las muestras mejoran y el
gradiente está comprobado contra el cálculo numérico.

## La verdad sobre el alcance

- **Sí:** una IA real, propia, entrenada aquí, que escribe textos de un dominio
  concreto y cuyo aprendizaje se puede auditar (loss, perplejidad, muestras).
- **No:** una IA general que converse de todo. Esa escala se mide en miles de
  millones de dólares de cómputo, no en jobs de 6 horas de Actions.
- **El límite es el cómputo:** Actions da 4 vCPU y 16 GB por job, sin GPU, con
  máximo 6 horas por job. Alcanza para modelos de millones de parámetros con
  paciencia; no alcanza para modelos de miles de millones.

## Cómo está organizado

```
experimentos/
  00_ia_minima/   red de juguete, sin dependencias: prueba el concepto
```

Cada experimento es independiente y se puede correr con Python 3 sin instalar
nada.
