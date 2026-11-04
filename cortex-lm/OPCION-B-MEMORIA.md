# Opcion B: atacar la memoria de la capa (sin tocar la plasticidad)

Fecha: 2026-11-04. Todas las cifras medidas aqui, 5 semillas por condicion cuando
la decision depende de la diferencia.

## La pregunta

En el paso anterior se midio que, quitando el buffer (LH=1), la capa recurrente
solo aporta 0,078 bpc y no llega al bigrama. La hipotesis: la capa olvida en 2-3
simbolos. Si se le da memoria, deberia servir para algo.

## Palancas y resultado (LH=1, sin plasticidad, 1 semilla de sondeo)

Referencia: 3,437 bpc. Bigrama: ~3,00.

| palanca | valores | bpc |
|---|---|---|
| tau_m (fuga de membrana) | 10 / 30 / 60 / 150 | 3,437 / 3,495 / 3,464 / 3,462 |
| theta_decay (fatiga) | 0,80 / 0,90 / 0,95 / 0,98 | 3,437 / 3,512 / 3,703 / 3,961 |
| theta_step | 0,28 / 0,15 / 0,08 / 0,03 | 3,437 / 3,452 / 3,506 / 3,522 |
| ticks/simbolo | 4 / 8 / 16 | 3,437 / **3,327** / 3,509 |
| inh_tau | 3 / 8 / 20 | 3,437 / 3,702 / 3,553 |

Solo `ticks/simbolo = 8` mejora por si solo. La fatiga mas lenta **empeora**:
cuando la fatiga es lo unico que codifica el pasado, frenarla borra la memoria en
vez de conservarla.

## La combinacion que funciona

`ticks/simbolo = 8` + `theta_decay = 0,95`:

| condicion | media | desv |
|---|---:|---:|
| capa de origen (tpc 4, theta 0,80) | 3,446 | 0,057 |
| **capa nueva (tpc 8, theta 0,95)** | **3,217** | **0,034** |

Mejora de 0,229 bpc (4 desviaciones). Pero sigue por encima del bigrama (3,00).

Combinaciones descartadas: theta_decay 0,98 (3,497), tau_m 30 con tpc 8 (3,393),
inh_tau 20 con tpc 8 (3,601), tpc 12 (3,353), theta_step 0,15 (3,238).

## La plasticidad, con la capa que ya recuerda

| condicion | media | desv |
|---|---:|---:|
| capa nueva, sin plasticidad | 3,217 | 0,034 |
| capa nueva, con plasticidad (eta 3e-4) | 3,247 | 0,029 |

Diferencia +0,030 bpc (0,9 desviaciones): **empate**. Ni con una capa que
recuerda el doble la plasticidad aporta nada. Es la cuarta formulacion distinta
que da el mismo resultado.

## El hallazgo importante: la capa nueva mejora el SISTEMA COMPLETO

Con el buffer de 8 estados y el lector RLS, que es la configuracion que cumple
el objetivo:

| capa | bpc (5 semillas) | media | desv |
|---|---|---:|---:|
| de origen (tpc 4, theta 0,80) | 1,062 0,802 1,014 0,850 0,975 | 0,941 | 0,110 |
| **nueva (tpc 8, theta 0,95)** | 0,530 0,561 0,572 0,507 0,492 | **0,532** | **0,034** |

**0,408 bpc menos: 43 % de reduccion. Efecto de 5,6 desviaciones.** Y ademas la
dispersion entre semillas baja de 0,110 a 0,034 (tres veces mas estable).

Este es el primer resultado del proyecto, aparte del lector, que mejora algo de
forma inequivoca.

## Veredicto

1. **La memoria de la capa se puede mejorar**, y mejora el sistema entero: 0,941
   -> 0,532 bpc.
2. **La plasticidad local sigue sin aportar nada**, ahora sobre cuatro
   formulaciones distintas (B aleatoria, e-prop con R, tres factores, tres
   factores sobre capa con memoria). Todas empatan.
3. El cuello de botella de la plasticidad ya no es la memoria de la capa. Es la
   propia regla, o la hipotesis de que una capa recurrente de este tipo pueda
   aportar algo que el lector no tenga ya.
