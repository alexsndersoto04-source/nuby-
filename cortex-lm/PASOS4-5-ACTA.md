# Pasos 4 y 5: factor postsinaptico y senal de error local

Fecha: 2026-11-04. Criterio acordado: "si tras los Pasos 4 y 5 la plasticidad
sigue empeorando o empatando con el baseline, se documenta como resultado
negativo y se para. No se pasa al Paso 3 sin evidencia de que la regla completa
al menos no empeora."

## Lo que se implemento

**Paso 4, factor postsinaptico.** La regla pasa a ser:

    dw = -eta * s_local(post) * traza_pre(pre) * traza_post(post)

`post_trace` vale 1 en el tick en que disparo la neurona de destino y decae con
`tau_post = 12` ticks. Si el destino no disparo, la sinapsis no se toca: no
hubo causa. Antes se reforzaba "lo que estaba activo cuando hubo error".

**Paso 5, senal de error propia de la capa.** Cada neurona de la capa 0 tiene
una prediccion top-down de lo que deberia hacer (`pred`), que sigue a la
poblacion sensorial del simbolo actual. El error local es:

    s_local(j) = real(j) - pred(j)      con real normalizado a [0,1]

No usa el lector, ni R, ni ninguna matriz. `error_local()` lo calcula y
`stats_error_local()` lo publica. Medido: **error medio 0,0871, maximo 0,3740**
sobre 1024 neuronas. La senal existe y es de magnitud utilizable.

Se elimino el uso de `B_` y de `R_` en la regla: `learn()` acepta `error=nullptr`
y en ese caso usa solo la senal local.

## Medicion: 5 semillas por condicion, train 8.000, test no visto 9.780

| condicion | movimiento | media bpc | desv | rango |
|---|---:|---:|---:|---|
| sin plasticidad | 0 % | 0,941 | 0,110 | 0,802 - 1,062 |
| regla nueva, eta 3e-5 | 0,12 % | 0,956 | 0,104 | 0,793 - 1,064 |
| regla nueva, eta 3e-4 | 1,24 % | 1,113 | 0,113 | 0,956 - 1,263 |

Diferencias y regla de 2 sigma:

- eta 3e-5: **+0,015 bpc** (efecto 0,14 sigma) -> **EMPATE**
- eta 3e-4: **+0,172 bpc** (efecto 1,54 sigma) -> **EMPATE** (roza el umbral)

Control de honestidad: a eta 3e-5 la regla nueva da **exactamente** los mismos
numeros que la regla vieja (1.013 0.793 1.064 0.923 0.986). Es decir, cuando el
movimiento es minimo, cambiar la regla no cambia nada. La regla solo empieza a
notarse cuando el paso crece, y cuando crece el resultado empeora.

## Veredicto, aplicando el criterio del usuario

**La plasticidad empata con el baseline y nunca mejora.** No hay una sola
semilla ni una sola configuracion en la que la plasticidad baje el bpc respecto
a no tener plasticidad.

Por el criterio acordado, esto es un **resultado negativo** y **no se pasa al
Paso 3** (quitar la linea de retardo).

## Lo que este resultado NO dice

No dice que la plasticidad local sea imposible. Dice algo mas preciso y mas
util: **en esta arquitectura, con la linea de retardo haciendo todo el trabajo
de memoria, la capa recurrente no tiene ninguna informacion que aportar, y
cualquier cambio de sus pesos solo puede empeorar la representacion que el
lector ya sabe aprovechar.**

Es exactamente lo que el Paso 3 estaba pensado para resolver (quitar el buffer
y obligar a la red a recordar). Pero el criterio acordado prohibe dar ese paso
sin evidencia previa de mejora, y esa evidencia no existe.

## Recomendacion

Si se quiere seguir, el orden logico es el inverso al acordado: **quitar la
linea de retardo primero**, porque es la condicion en la que la plasticidad
tiene algo que ganar, y solo despues medir la regla. Medir la plasticidad
mientras un buffer hace todo el trabajo mide, por construccion, cero.
