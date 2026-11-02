# Estado del motor CORTEX-LM

Acta honesta. Lo que funciona, lo que no, y qué se midió para saberlo.

---

## Resumen

El sistema **aprende lenguaje y generaliza** (0,24 bpc en texto no visto con
8 símbolos de historial). La red de pulsos **todavía no aporta** ese
rendimiento, y hoy se encontró la razón mecánica: **satura**.

---

## Los dos bugs que invalidaban todas las mediciones anteriores

**1. Cada símbolo avanzaba 10 ticks, no 5, y perdía la mitad de su corriente.**
`inject_only()` contenía un bucle de ticks que `feed()` volvía a ejecutar
después. Consecuencia: la corriente sensorial se aplicaba una vez y se dejaba
evolucionar el doble de tiempo, con lo que un símbolo aislado **no llegaba a
hacer disparar a nada** (0 disparos con ganancia 0,62 e incluso con 3,0).

**2. El contador de disparos tapaba el problema.** Todos los barridos de
parámetros de las sesiones anteriores se hicieron con el motor en ese estado:
medían una red medio apagada. Los números de esas tablas ya no valen.

Con el motor corregido, un símbolo aislado produce ~26 disparos y la red
responde a la ganancia como es debido.

## Lo que se midió después, ya con el motor correcto

**La red identifica el símbolo actual** si el estado se limpia entre símbolos:
acierto 1,000 (azar 0,062).

**En secuencia continua no identifica nada**: el estado queda saturado en
~168 de 256 neuronas permanentemente activas, y la entrada nueva modula apenas
un océano de actividad previa.

**El pasado no está en el estado**. Decodificar el símbolo de hace 1 a 4 pasos
desde el estado de la red da valores de azar:

| lag | `rate` | `trace` |
|---|---|---|
| 1 | 0,14 | 0,23 |
| 2 | 0,04 | 0,15 |
| 3 | 0,12 | 0,14 |
| 4 | 0,05 | 0,09 |

(azar 0,062). Hay una pizca de información de un paso atrás en `trace`, y nada
más. Con ganancia de 0,40 desaparece por completo.

**Y no mejora al crecer**: 256, 512, 1024 y 2048 neuronas dan el mismo
resultado. **La premisa del diseño —más grande, más capacidad— es falsa en esta
red, porque el régimen es saturación, no estado disperso.**

## Lo que se intentó para arreglar la saturación

| mecanismo | efecto |
|---|---|
| Bajar `w_init_scale` de 0,12 a 0,005 | más saturado todavía (236 neuronas) |
| Inhibición de retroalimentación divergente y retardada (nueva, `fi_gain`) | reduce activas de 167 a 82, pero **no crea memoria** |
| Quitar la compuerta k-WTA | sin efecto (nunca había exceso de candidatos) |
| Subir el peso recurrente a 1,0 | sin efecto |

Ninguno produjo memoria. El freno cambia cuántas neuronas están activas, no
qué información guardan.

## Lo que sí está resuelto y verificado

- **Memoria de trabajo**: 100 % en el banco de 40 claves con 10 de relleno.
- **Aprendizaje de texto con generalización perfecta** (entrenamiento y test
  coinciden al tercer decimal).
- **La plasticidad local por fin se ejecuta**: 0 → ~125 marcas de elegibilidad
  por símbolo.
- **Vocabulario honesto**: 256 bytes reales en vez de 120 con mapeo que
  corrompía el texto.

## El problema abierto, en una frase

Conseguir un régimen de actividad **dispersa y persistente** —pocas neuronas
activas a la vez, capaces de seguir activas sin entrada— en vez de saturación
o silencio. Es el requisito para que el estado guarde algo, y sin él ninguna
regla de lectura puede sacar contexto.

## Reproducir

```
make
./bin/cortex_lm eval x                      # historial de 8 símbolos: ~0,24 bpc
CORTEX_NCTX=4 ./bin/cortex_lm eval x        # ~1,40 bpc
CORTEX_VIEW=trace ./bin/cortex_lm eval x    # solo estado de la red: ~4,40 bpc
./bin/cortex_lm bench /tmp/b.bin            # memoria de trabajo: 100 %
```

## Diseño aprobado

`DISENO-CORTEX-LM.md`: 2 capas LIF × 4096 neuronas, 6 % dispersión, retardos
1..32, ~2,5 M sinapsis, ~30 MB. Objetivo de fase 2: BPC < 2,0.

## Infraestructura

Kaggle (P100 16 GB, `kaggle kernels push` desde Termux) para el cómputo pesado.
El Redmi 9C es terminal de control, no máquina de entrenamiento.
