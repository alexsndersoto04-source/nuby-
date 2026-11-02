# Estado del motor CORTEX-LM

Documento honesto. Lo que funciona, lo que no, y lo que no sabemos.

---

## Resumen en una línea

El motor **aprende a predecir texto y generaliza**, pero la red de pulsos
**todavía no aporta nada medible**; el rendimiento viene del clasificador
lineal. El objetivo de BPC se cumple por debajo del listón teórico, y eso es
precisamente lo que hay que arreglar.

---

## Resultado medido hoy

Entrenamiento en las primeras 80 % del corpus, evaluación en el 20 % restante
que el modelo nunca vio:

| configuración | bpc en texto no visto |
|---|---|
| azar (uniforme) | 6,91 |
| estado de la red (trazas de actividad) | 4,36 |
| **historial sensorial de 8 símbolos** | **0,289** |
| historial sensorial de 4 símbolos | 1,396 |
| historial sensorial de 2 símbolos | 2,288 |

**La generalización es perfecta**: la cifra de entrenamiento y la de texto no
visto coinciden al tercer decimal. No hay sobreajuste.

## Pero hay un problema, y es serio

El control que separa lo que aporta la red de lo que aporta el clasificador:

| configuración | bpc en texto no visto |
|---|---|
| historial de 4 símbolos + red **aprendiendo** | 1,396 |
| historial de 4 símbolos + red **congelada** | 1,396 |

**Idénticos hasta el último dígito.** La plasticidad local no contribuye en
absoluto. Todo el rendimiento sale del clasificador lineal leyendo un one-hot
del historial de entrada; la red de pulsos es decoración.

Y el listón que hay que batir:

| modelo trivial | bpc en el mismo texto |
|---|---|
| bigrama (contexto de 1 byte) | 2,893 |
| trigrama (contexto de 2) | 1,700 |
| 4-grama (contexto de 3) | **0,855** |
| 5-grama (contexto de 4) | **0,579** |

Nuestro modelo con 4 símbolos de historial (1,396) **pierde contra un 4-grama
del montón** (0,855), que es una tabla de contar y cabe en un teléfono.

## Conclusión operativa

El sistema **ya aprende lenguaje**. Cumple el objetivo de BPC < 2,0. Pero lo
cumple por la vía trivial, no por la vía interesante. Ir a Kaggle a escalar esto
sería escalar un contador de n-gramas con una red de pulsos de adorno.

**El problema abierto, en una frase:** conseguir que el estado recurrente
aportes información que el historial de entrada no contiene.

## Lo que sí quedó arreglado (bugs reales, todos verificados)

1. **La plasticidad local valía cero.** 0 de 4887 sinapsis marcadas. La marca
   se escribía en la llegada sináptica, que exige coincidencia de pre y post en
   pocos ticks; a densidad 0,05 eso casi nunca pasa. Ahora se marca al disparo
   presináptico sobre todas sus salientes (forma de e-prop): ~125 marcas por
   símbolo.
2. **La codificación de entrada colisionaba.** Los 8 bits del carácter
   proyectaban siempre sobre las mismas ~24 neuronas, y el código 0 no encendía
   nada. Ahora cada símbolo tiene parche propio + textura propia.
3. **`consolidate()` borraba el contexto** cada 500 símbolos llamando a
   `reset_state()`. Era una operación sobre pesos destruyendo actividad.
4. **El banco de pruebas arrastraba estado entre ensayos**, lo que producía un
   0 % falso. Con el estado limpio: 100 % en memoria de trabajo (40 claves,
   10 de relleno).

## Reproducir

```
make
./bin/cortex_lm eval x                      # historial de 8: ~0,29 bpc
CORTEX_NCTX=4 ./bin/cortex_lm eval x        # historial de 4: ~1,40 bpc
CORTEX_VIEW=trace ./bin/cortex_lm eval x    # solo estado de la red: ~4,36 bpc
CORTEX_READOUT_ONLY=1 ... eval x            # control: red congelada
./bin/cortex_lm bench /tmp/b.bin            # memoria de trabajo: 100 %
```

## Lo que NO está resuelto

- La red de pulsos no aporta. Es el problema central y está abierto.
- El corpus de prueba tiene 195 valores de byte, pero el motor usa 120 como
  vocabulario: parte del texto se mapea mal y las cifras absolutas están
  infladas de forma artificial. Hay que fijar el vocabulario al rango real de
  bytes (256) y volver a medir.
- No se ha probado con corpus real grande.

## Diseño aprobado

`DISENO-CORTEX-LM.md`: 2 capas LIF × 4096 neuronas, 6 % dispersión, retardos
1..32, ~2,5 M sinapsis, ~30 MB. Objetivo de fase 2: BPC < 2,0.

## Infraestructura

Kaggle (P100 16 GB, `kaggle kernels push` desde Termux) para el cómputo pesado.
El Redmi 9C es terminal de control, no máquina de entrenamiento.
