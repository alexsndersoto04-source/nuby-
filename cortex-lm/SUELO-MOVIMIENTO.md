# Suelo de movimiento: calibracion de eta y eprop_scale (Paso 1b)

Fecha: 2026-11-04. Criterio acordado con el usuario: si tras calibrar eta la
plasticidad sigue sin mover los pesos de forma medible (cambio medio < 1 % del
peso tipico), se para y se documenta como resultado negativo.

## Primero, una correccion de metodo

La medicion anterior ("media inicial 0.29374 -> final 0.29374") era **inutil**:
media el cambio de la MEDIA de 52.173 pesos. Unos suben y otros bajan, y la
media se cancela. Hay que medir el cambio absoluto sinapsis a sinapsis.

Medido bien, con eta=1e-3 (el valor de antes):

    sinapsis movidas ................ 48.926 de 52.173 (93,8 %)
    cambio absoluto medio ........... 4,73e-06
    peso tipico ..................... 0,294
    ratio ........................... 0,002 %   <- 500 veces por debajo del 1 %

Es decir: la plasticidad SI actuaba, sobre el 94 % de las sinapsis, pero con un
paso tan pequeno que no cambiaba nada. Correctamente medido, no era "cero".

## Calibracion de eprop_scale (eta = 0,3 fijo, 1 epoca, train 8.000)

Ojo con un error que cometi: el 7o argumento del banco es `w_scale` (el peso
recurrente), NO `eprop_scale`. Al principio cambie el primero creyendo que
cambiaba el segundo. Los valores de esta tabla ya usan el argumento correcto.

| eprop_scale | cambio medio | ratio | bpc |
|---:|---:|---:|---:|
| 0,125 (defecto) | 1,73e-03 | 0,59 % | 1,073 |
| 0,5 | 6,93e-03 | **2,36 %** | 1,225 |
| 1,0 | 1,38e-02 | 4,70 % | 1,241 |
| 2,0 | 2,70e-02 | 9,20 % | 1,326 |

**Criterio del 1 % cumplido** a partir de eprop_scale = 0,5.

## Pero mover mas empeora. Medido con 5 semillas

| condicion | media | desviacion | rango |
|---|---:|---:|---|
| sin plasticidad | 0,941 | 0,110 | 0,802 - 1,062 |
| plasticidad (eta 0,3 / eprop 0,5) | 1,127 | 0,081 | 1,027 - 1,225 |

Diferencia: **+0,186 bpc** (empeora), tamano del efecto 1,92 desviaciones.
Por la regla acordada (2 sigma) esto queda **casi en empate**, pero la
direccion es consistente: las cinco semillas con plasticidad estan por encima
de la media de las cinco sin plasticidad.

## Interpretacion honesta

El suelo de movimiento se cumple, pero cumplirlo no mejora nada: mover los pesos
mas deprisa solo degrada. Eso es coherente con el diagnostico de los Pasos 4 y
5 del plan: la regla actual usa (a) una traza presinaptica, (b) una senal de
error ajena a la capa, y (c) ningun factor postsinaptico. Con esa regla,
cualquier paso que se note en los pesos es un paso que se nota por el lado malo.

**Conclusion para el plan:** el suelo de movimiento queda calibrado
(eta = 0,3, eprop_scale = 0,5) y documentado, pero **no se debe esperar que
mejore nada hasta cambiar la regla** (Pasos 4 y 5). Seguir con la homeostasis
sobre una regla que empeora al moverse estabilizaria el error, no el
aprendizaje.

Recomendacion: hacer los Pasos 4 y 5 (factor postsinaptico + error local
propio) ANTES de la homeostasis, y volver a aplicar el criterio de 2 sigma
despues. La homeostasis tiene sentido sobre una regla que ya no empeora.
