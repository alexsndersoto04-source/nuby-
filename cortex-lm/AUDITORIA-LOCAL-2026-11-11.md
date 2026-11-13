# Auditoría local de CORTEX-LM — 2026-11-11

## Alcance y cautela

Esta nota registra pruebas ejecutadas en el checkout actual. No equivale a una
validación científica independiente ni certifica comprensión general. Los
resultados lingüísticos son de un solo libro y una partición cronológica; deben
repetirse en fuentes disjuntas antes de generalizar.

Se conservaron intactos los pesos/conexiones del motor. Las pruebas usan
inicializaciones deterministas por semilla y corpus local reconstruible. Los
binarios, corpus reconstruido y modelo pequeño de prueba son artefactos
ignorados o temporales; no se versionan aquí.

## Compilación y pruebas estructurales

Ejecutado:

```bash
cd cortex-lm
make clean && make -j2
make portable
./bin/cortex_lm audit
./bin/cortex_lm smoke /tmp/cortex_smoke.bin
./bin/cortex_lm bench
```

Resultados observados:

- Compilación nativa y portable correcta, sin avisos del compilador.
- `audit`: 1.024 neuronas, 52.173 sinapsis, corriente recurrente/umbral 0,30,
  81,6 disparos por símbolo aislado, actividad estable y 91,7 % en recuperación
  de 40 claves con 8 símbolos de relleno (azar 2,5 %).
- `smoke`: 1.008/1.024 neuronas usadas y actividad estable alrededor de 68,3
  disparos por símbolo.
- `bench`: 40 claves, 10 símbolos de relleno y 100 % tras 60 épocas de
  entrenamiento del lector de salida.

**Límite de interpretación:** la recuperación de claves del `audit` decodifica
el estado con un lector ridge entrenado fuera de línea. `bench` también aprende
un lector supervisado en una tarea sintética. Son pruebas útiles de dinámica y
codificabilidad de la memoria de trabajo, no pruebas de predicción lingüística.

## Escala computacional

Perfil de cinco regiones `2048,65536,131072,524288,294912`, 32 macrocolumnas,
TRN top-4, ejecutado con `bench_cortical macro`:

- 1.017.856 neuronas LIF y 96.891.043 sinapsis CSR.
- 1.742 MB de RAM reportada; lectores compactos FP16 de 37,8 MB.
- Inicialización en 7,56 s y 434 bytes/s en un ensayo de 512 bytes con dos hilos.
- Deriva RSS de +792 KB entre las mitades del ensayo.

Escala CSR de cuatro regiones `4096,98304,98304,98304`, grado 72, 4.096 bytes:

- 299.008 neuronas, 21.722.209 sinapsis, 395,1 MB, 360 bytes/s y +0 KB de
  deriva RSS observada entre mitades.

Esto comprueba creación, ejecución y consumo de recursos en las configuraciones
indicadas. El ensayo macro **no** evalúa calidad lingüística y el de 512 bytes no
es entrenamiento representativo a escala.

## Predicción de texto y ablación de plasticidad

El corpus se reconstruye con:

```bash
python3 data/build_corpus_large.py
```

La reconstrucción produjo 45.741 caracteres Unicode, 46.920 bytes UTF-8,
alfabeto de 72 símbolos y 2.096 palabras del diccionario. `bench_cortical bpc`
usa los primeros 80 % de bytes para entrenamiento y los últimos 20 % para
holdout cronológico: 37.536 train / 9.384 test. El holdout no se usa para
actualizar pesos. La configuración fue `{1024,8192,8192,6144}`, grado 52,
8 ticks por byte, 7 épocas del lector, tasa inicial 0,34 y semillas 1234, 7 y
99. Se generaron 300 bytes para el indicador secundario de palabras.

Se amplió `tools/bench_cortical bpc` para separar mecanismos sin cambiar la
semántica del argumento antiguo: al omitir el modo, `stdp=1` conserva el modo
combinado histórico y `stdp=0` conserva el modo sin plasticidad. Se puede indicar
al final `combined`, `stdp`, `homeo` o `none`.

| Modo | Seed 1234 | Seed 7 | Seed 99 | Media BPC |
|---|---:|---:|---:|---:|
| Sin STDP ni homeostasis (`none`) | 2,561 | 2,568 | 2,561 | **2,563** |
| STDP local, homeostasis apagada (`stdp`) | 2,490 | 2,483 | 2,484 | **2,486** |
| Homeostasis sola, STDP apagado (`homeo`) | 2,555 | no medido | no medido | — |
| STDP + homeostasis (`combined`) | 2,487 | 2,477 | 2,480 | **2,481** |

En esta configuración, **STDP aislada mejora el holdout alrededor de 0,078 BPC
frente a ninguna de las dos reglas activas** en las tres semillas probadas. La
homeostasis sola solo se midió en una semilla y su diferencia frente al control
fue pequeña. La configuración combinada mejora ligeramente la media frente a
STDP sola, pero esa diferencia es demasiado pequeña para interpretarla como un
beneficio confirmado. El modo de plasticidad también cambia la actividad: en
la corrida de seed 1234, el indicador medio de rasgos activos fue 1.129 para
STDP sola, frente a 4.623 para homeostasis sola.

Control Witten–Bell de bytes, calculado únicamente con el train y puntuado en
el mismo holdout:

```bash
python3 tools/baseline_ngram.py data/corpus.txt
```

| Orden | BPC |
|---:|---:|
| 1 | 3,2777 |
| 2 | 2,8254 |
| 3 | 2,5775 |
| 4 | 2,5645 |

La media de CORTEX-LM con STDP aislada (2,486) es menor que el baseline de orden
4 (2,565) para **esta partición y estos parámetros**. Es una señal genuinamente
alentadora y merece seguimiento. No demuestra aún generalización a otros libros:
el corpus procede de una sola obra, el holdout es una continuación del mismo
texto y la configuración se ha desarrollado sobre esta familia de pruebas.

### Qué aprende cada parte en esta prueba

Durante el período crítico, STDP modifica localmente conexiones corticales a
partir de coincidencias de actividad. Después la corteza se congela y se entrena
el lector para predecir el siguiente byte usando el objetivo supervisado. Por
tanto, el resultado muestra que **la exposición con STDP produce rasgos más
útiles para un lector entrenado**; todavía no demuestra que STDP sola aprenda la
función completa de predicción del siguiente byte.

La ablación combina varios cambios internos de actividad; se necesitan más
semillas, más corpora y controles de robustez para descartar sensibilidad a la
partición, al lector o a la selección de hiperparámetros.

## Memoria episódica y semántica: comprobación exploratoria

Con un modelo temporal pequeño de 768 neuronas, 12.063 sinapsis y solo 4.000
bytes de entrenamiento, se verificó que `recordar` guarda un hecho y que una
consulta cercana recupera su texto exacto desde CA3 (similitud observada 0,81).
El código reinyecta el texto recuperado en el flujo del generador: esto valida
recuperación asociativa del registro, no deducción semántica autónoma.

Al asimilar 12 pasajes, el índice semántico devolvió dos fragmentos con
similitudes 0,37 y 0,36 para una pregunta general; la respuesta generada siguió
siendo de baja calidad. Por el tamaño mínimo del modelo, es solo una prueba de
ruta y un recordatorio de que hay que medir por separado recuperación y
composición de respuestas.

## Reproducción de la ablación

```bash
cd cortex-lm
python3 data/build_corpus_large.py
make -j2

# Modo explícito; cambiar la última palabra por combined, homeo o none.
./bin/bench_cortical bpc data/corpus.txt data/dic.txt \
  8192 8192,6144 7 0.34 1 1234 stdp
```

Los resultados de esta nota se ejecutaron con el checkout y hardware de esta
sesión. Para uso como cifra de proyecto, repetir al menos cinco semillas y
holdouts de varias fuentes antes de cambiar la afirmación de estado.

## Artefactos remotos comunicados por el usuario (pendiente de validación)

El usuario consultó desde Colab el repositorio configurado y compartió estos
nombres y tamaños. Son resultados informados por el usuario; no se descargó ni
se abrió el repositorio remoto desde este entorno:

| Archivo | Tamaño informado |
|---|---:|
| `cerebro_macro_1m.bin` | 3.391.315.238 bytes (aprox. 3,16 GiB) |
| `cerebro_macro_1m.registro.json` | 114.371 bytes |
| `registro_wikipedia.json` | 2.841 bytes |

El resumen posterior de los registros muestra que el registro principal tiene
14 fases (`arranque_macro` y 13 fases `wiki_hf_lote_...`), un campo
`bytes_asimilados` de 3.235.273 y una lista `articulos_wiki` de 5.000 nombres.
El segundo registro informa 9 lotes, 139 artículos y 1.925.358 bytes
asimilados. Los dos JSON tienen esquemas distintos y el código del proyecto
contiene flujos de entrenamiento distintos que los generan; no se pueden sumar
como si fueran un único conteo ni se ha demostrado que ambos correspondan
exactamente al mismo estado del `.bin`. La lista de 5.000 puede ser un máximo
conservado por el programa, no necesariamente el total de artículos vistos.

El usuario informó que su runtime de Colab muestra 11,4 GB de RAM libre y
107,6 GB de disco libre. Esto permitió compilar el lector y probar el
checkpoint en modo de solo lectura.

Actualización posterior, según salida de Colab compartida por el usuario: la
copia descargada mide 3.391.315.238 bytes, el encabezado reconoce la firma
`NUB3` (`0x4E554233`), y su SHA-256 es
`65109c7b784dc5c6707ac9efed266d37a665b15b92b17c2d175d8c28339a4b98`. La firma
coincide con el formato macro FP16 que acepta el cargador de `cerebro.cpp`.
El usuario también informó que `cerebro evaluar` terminó con `returncode=0`
y produjo estas cifras: carga en 4.084 ms, 5 regiones, 1.017.856 neuronas LIF,
96.891.056 sinapsis CSR y 0 engramas CA3. El BPC fue 2,7636 sobre un texto de
solo 317 caracteres. Esto confirma carga e inferencia, pero el texto corto y
elaborado para smoke test no es una evaluación válida de calidad ni es
comparable con el benchmark anterior. `0 engramas CA3` indica que este archivo
no contiene engramas CA3 disponibles para esa lectura; no invalida su estado
cortical.

Después, el usuario evaluó una muestra de 16.348 bytes (16.000 caracteres
Python) concatenada de cuatro artículos aleatorios que no aparecen en los
registros visibles: `Domingo Eugenio Villafañe`, `Sabueso Fino Colombiano`,
`Batería de Aptitudes Diferenciales y Generales` y `La Peña`. `cerebro evaluar`
reportó 3,2207 BPC con la misma arquitectura y carga en 3.796 ms. Es una primera
medida de predicción en una muestra mayor y más diversa; el registro principal
puede estar truncado a 5.000 títulos, así que no se puede garantizar que esos
artículos nunca se vieron. Tampoco es comparable con la prueba local previa
(2,486 BPC) ni con Witten–Bell (2,565 BPC), que usaron otro corpus y otro
protocolo. Hace falta un baseline entrenado solo con datos del train comparable
antes de interpretar si 3,2207 es bueno o malo. Para facilitarlo, amplié
`tools/baseline_ngram.py` con un modo `--holdout` que ajusta el byte n-gram solo
con un archivo de train y puntúa un archivo de test independiente. Verifiqué que
el modo cronológico anterior conserva sus resultados y que el modo de holdout
separado funciona en una prueba sintética. La próxima comparación usará el
mismo archivo de 16.348 bytes y un train aproximado de igual volumen basado en
el flujo español de Wikipedia; por el límite del registro de artículos, el
control será aproximado y esa limitación se documentará. El primer intento de
preparar ese control falló antes de descargar registros porque la celda indicó
por error `alexsndersoto-source/...`; el ID correcto es
`alexsndersoto/cortex-macro-1m-backup`. No se llegó a entrenar ni a evaluar el
baseline en ese intento. En el siguiente intento, ya corregido el ID, Colab
reconstruyó un train de 3.235.273 bytes (semilla del repo más 1.234 artículos
que aparecen en el registro) y un holdout regenerado de 16.347 bytes. Witten–Bell
obtuvo 3,6200 / 3,2427 / 2,9014 / 2,7090 BPC en órdenes 1–4. El orden 4 quedó
0,5117 BPC por debajo del CORTEX-LM reportado (3,2207), señal preliminar de que
el baseline predijo mejor en estas muestras. Luego el usuario volvió a
puntuar el checkpoint sobre exactamente el mismo holdout regenerado de 16.347
bytes que el baseline: CORTEX-LM obtuvo 3,2208 BPC (carga 15.381 ms; mismas 5
regiones, 1.017.856 neuronas, 96.891.056 sinapsis y 0 CA3). Así, en este holdout
coincidente, Witten–Bell orden 4 (2,7090) queda 0,5118 BPC por debajo. La prueba
de test ahora es exacta; la limitación restante es que el train n-gram se
reconstruyó con la versión `20231101.es` y 1.234 artículos del registro, no con
un volcado byte a byte del entrenamiento original. La conclusión segura es que
el baseline superó al checkpoint en esta muestra y protocolo; no generalizar a
toda la lengua ni a la arquitectura sin más holdouts.

Desglose del mismo holdout por artículo, con orden 4:

| Artículo | CORTEX-LM BPC | Witten–Bell BPC | Diferencia (CORTEX − WB) |
|---|---:|---:|---:|
| Domingo Eugenio Villafañe | 3,1092 | 2,4956 | +0,6136 |
| Sabueso Fino Colombiano | 3,2335 | 2,7811 | +0,4524 |
| Batería de Aptitudes Diferenciales y Generales | 3,3622 | 2,9122 | +0,4500 |
| La Peña | 3,1376 | 2,6247 | +0,5129 |
| Media simple por artículo | 3,2106 | 2,7034 | +0,5072 |

El n-grama obtuvo menor BPC en los cuatro textos, así que la diferencia no
provino de un solo artículo difícil. Siguiente trabajo: auditar la etapa de
aprendizaje del lector de salida y diseñar una prueba pequeña en una copia del
checkpoint; no actualizar ni sobrescribir el original.

Hallazgo de código relevante para interpretar el resultado: en el flujo de
`motor_macro_nube_24_7.py`, los lotes `wiki_hf_lote_...` se procesan mediante
`cerebro alimentar --semantica`. Dentro de `cmd_alimentar`, `st.eng.set_plasticity(false)`
se mantiene mientras `rd_ventral.step(..., true, ...)` actualiza el lector
supervisado. Así, esos lotes no aplican STDP cortical; actualizan el lector y,
con `--semantica`, el índice semántico. El STDP del flujo de creación ocurre
solo durante el período crítico inicial de hasta 65.000 bytes en `cmd_entrenar`.
Los nombres de fase son compatibles con este flujo, aunque no prueban por sí
solos qué script exacto produjo el checkpoint. Para probar esta hipótesis sin
cambiar el comportamiento anterior, añadí la opción explícita
`cerebro alimentar --plasticidad-local`: activa la plasticidad de Engine solo si
se solicita; por defecto sigue apagada. `make -j2` y una prueba de humo con un
modelo temporal confirmaron que el modo predeterminado informa
`plasticidad-local=NO` y la opción nueva `plasticidad-local=SI`. El siguiente
experimento debe usar dos copias idénticas del macro checkpoint, con la misma
muestra corta de entrenamiento y evaluación, una con la opción y otra sin ella;
el checkpoint original permanece intacto.

Resultado de ese primer A/B en Colab, informado por el usuario: dos copias del
mismo checkpoint recibieron una sola época sobre el mismo lote nuevo de 16.384
bytes (artículos `Acacia`, `Azur`, `Aldea`, `Anatomía`, `Anatomía humana`) y se
puntuaron con el holdout común. Control sin plasticidad local: train 2,675,
eval 3,2295 BPC. STDP+homeostasis local: train 2,720, eval 3,2753 BPC. La opción
local combinada quedó 0,0458 BPC peor que el control y 0,0545 peor que el
checkpoint antes de la actualización (3,2208). Es un piloto de una muestra, no
una conclusión general, pero no justifica activar plasticidad local combinada
por defecto.

Para diagnosticar el efecto, añadí compuertas independientes
`--stdp-local` y `--homeostasis-local`; `--plasticidad-local` conserva la opción
combinada y el modo predeterminado sigue apagado. `set_plasticity(bool)` sigue
siendo retrocompatible y controla ambas. Compilación, auditoría estructural,
resultados del baseline previo y smoke tests de los cuatro modos pasan. El
A/B separado ya se ejecutó sobre el mismo lote de 16.384 bytes y holdout, cada
modo desde una copia idéntica del original:

| Modo | Train BPC | Eval BPC |
|---|---:|---:|
| Sin plasticidad | 2,675 | 3,2295 |
| STDP sola | 2,687 | 3,2438 |
| Homeostasis sola | 2,709 | 3,2606 |
| STDP + homeostasis | 2,720 | 3,2753 |

En este piloto, añadir cualquiera de las dos reglas empeoró train y holdout; la
combinación fue la peor. Frente al control, las diferencias de evaluación fueron
+0,0143, +0,0311 y +0,0458 BPC respectivamente. Una sola muestra no prueba que
las reglas siempre perjudiquen: sí descarta activar ninguna automáticamente
como respuesta a la comparación anterior.

Hipótesis probada: durante `alimentar`, una representación cortical que cambia
mientras el lector supervisado aprende podría ser no estacionaria. Se añadió
`--congelar-lector` para separar la exposición cortical del ajuste del lector.

Resultado de Colab con dos fases sobre el mismo lote de 16.384 bytes y holdout:
control (pasada congelada sin plasticidad, luego lector entrenable) obtuvo 2,676
train / 3,2311 eval BPC; STDP sola con lector congelado en la primera pasada y
lector entrenable después obtuvo 2,683 / 3,2457 BPC. La plasticidad antes del
ajuste del lector quedó 0,0146 BPC peor que el control. La separación de fases
no rescató STDP en este piloto. Combinado con los cuatro modos anteriores, no
hay evidencia favorable para activar STDP/homeostasis durante la asimilación de
este checkpoint; los efectos son pequeños y solo se probó un lote.

Las compuertas independientes y `--congelar-lector` quedaron compiladas y
validadas con `make portable`, `./bin/cortex_lm audit` y pruebas de humo
pequeñas. Resultado de Colab en la prueba por fases: control con lector
congelado y luego entrenado obtuvo 2,676 train / 3,2311 eval BPC; STDP con
lector congelado en fase 1 y lector entrenado después dio 2,683 / 3,2457 BPC.
STDP por fases quedó +0,0146 BPC peor en el mismo holdout, así que la hipótesis
de que solo el entrenamiento simultáneo causaba el daño no queda respaldada en
este lote. El checkpoint original permaneció intacto. Los cambios de código se
guardaron en los commits `0020a15` y `631bf83` de la rama activa
`arena/b0c825e4-nuby` y se subieron a esa misma rama para que Colab pueda clonar
la versión correcta. El informe de auditoría permanece local y no se incluyó
en esos pushes.

## Continuación de auditoría — 2026-11-11

Se auditó el lector compacto FP16 de macro y su evaluación prequential. La métrica
`train=` de `cerebro alimentar` se obtiene mientras los pesos se actualizan
byte a byte, por lo que no debe compararse como si fuera una evaluación fija; la
métrica `--eval` sí recorre el holdout con `learn=false`. En los pilotos Colab ya
compartidos, las reglas locales y el ajuste del lector no mostraron mejora
reproducible del holdout. Esto respalda mantener desactivadas STDP/homeostasis
locales por defecto y no escalar con LR por ensayo.

Añadí `tools/evaluar_reproducible.py`, que mide CORTEX-LM y Witten–Bell sobre el
mismo archivo holdout, registra SHA-256 de modelo, binario, train, holdout y
metadatos, y comprueba que las entradas no cambiaron durante la evaluación. El
smoke con modelo sintético funciona. La función baseline se corrigió para asignar
probabilidad suavizada a los 256 posibles bytes, también a símbolos no vistos en
train. **Por ello, los BPC Witten–Bell anteriores (incluido 2,7090) deben
recalcularse con el código corregido** antes de usarlos en conclusiones nuevas.

`cloud/colab_sync.py` ahora conserva el checkpoint local hasta copiar y validar
firma NUBY/NUB3 del archivo remoto; reporta su SHA-256 y la subida rechaza firmas
inválidas. Añadí dos pruebas: descarga válida promueve y descarga corrupta
conserva el original.

`colab_train_cortex.ipynb` se convirtió en una auditoría de solo lectura: prepara
un train y holdout por artículos distintos, guarda títulos/configuración del
split y genera el manifiesto comparativo. Se quitó de esta etapa la llamada a
`alimentar` y la subida automática. **Requiere ejecución del usuario en Colab**
para obtener los hashes y métricas del checkpoint remoto; esta sesión no tiene
acceso a Hugging Face.

### Manifiesto recibido de Colab (solo informado por el usuario)

Checkpoint SHA-256 `65109c7b784dc5c6707ac9efed266d37a665b15b92b17c2d175d8c28339a4b98`
(3.391.315.238 bytes); binario evaluador SHA-256
`c072dd3771a00e118a0222aea76a9e84e72562b5cfacce5f7158b45fefa04abc`.
El holdout de 16.384 bytes contiene cinco títulos distintos de los 68 títulos
registrados en el train nuevo de 200.000 bytes. Métricas comunicadas: CORTEX-LM
3,4479 BPC; Witten–Bell órdenes 1–4: 3,8008 / 3,4067 / 3,1519 / 3,0913 BPC.
En esta muestra el baseline orden 4 aventaja en 0,3566 BPC. El train del baseline
es una muestra nueva, no el corpus histórico exacto del checkpoint; por ello es
una comparación emparejada en el test, pero no una reproducción del entrenamiento
histórico. El texto generado que aparece en el log es incoherente. La salida
informa 0 engramas CA3.

**Hallazgo posterior que obliga a recalcular solo la puntuación CORTEX:**
`CompactCorticalReadout::step` incorporaba el byte objetivo al vocabulario antes
de medir incluso con `learn=false`. Esto filtraba la identidad del target durante
la evaluación cuando aparecía un byte no visto en el vocabulario guardado y podía
hacer su BPC artificialmente bajo. Corregí el lector para que la inferencia no
muta el vocabulario, asigne un piso fijo a objetivos ausentes y registre
`lector_activo/256` y los conteos de objetivos OOV; añadí una prueba de regresión
al `audit` C++ y un smoke con símbolos UTF-8 ausentes confirmó que el hash del
modelo queda intacto. El usuario repitió con el binario corregido y los mismos
hashes de checkpoint/holdout/train: el vocabulario tiene 193/256 slots, pero hay
**0 objetivos OOV de 16.351**. Por tanto, el defecto no afectó esta medición:
CORTEX-LM confirma **3,4479 BPC** frente a **3,0913** del baseline Witten–Bell
orden 4, diferencia **+0,3566 BPC**. El baseline entrenó con solo 200.000 bytes
nuevos, pero no reproduce el corpus histórico del checkpoint. Se conserva como
resultado negativo para este holdout, no como veredicto global. El lector solo
tiene 193/256 slots activos y el holdout no contiene objetivos fuera de esos
slots. El BPC de CORTEX se normaliza sobre esos 193 bytes, mientras que Witten–Bell
lo hace sobre 256; el baseline reportado ya gana pese a esa ventaja de soporte
para CORTEX. Un baseline condicionado a los mismos 193 símbolos tendría BPC igual
o menor, así que el sentido de la diferencia no favorece al baseline por el
vocabulario. La generación muestra texto incoherente; esto es una señal
cualitativa concordante, no una métrica independiente. El manifiesto también
declara `created_utc=2026-11-13`,
fecha futura frente al reloj de esta auditoría (2026-11-11); la hora del runtime
no se usará como evidencia de procedencia.

Nueva prueba comunicada por el usuario: sensibilidad a la tasa de aprendizaje
del lector, una pasada sobre el mismo lote de 16.384 bytes y holdout, desde
copias del checkpoint original y sin plasticidad local. Con `--lr 0.03` obtuvo
2,723 train / 3,2039 eval BPC; con `--lr 0.08`, 2,701 / 3,2124 BPC. El
resultado previo con `--lr 0.24` y sin plasticidad fue 2,675 / 3,2295 BPC.
En esta muestra, tasas menores ajustaron menos el train y mejoraron el holdout
respecto a 0,24; `0.03` fue el mejor de los tres en eval, por 0,0256 BPC frente
a 0,24. Es una señal prometedora sobre sensibilidad del lector a la tasa, no
una conclusión general: falta repetir con varios lotes/holdouts y comprobar que
los protocolos y copias iniciales sean idénticos. Todos los resultados de esta
sección provienen de ejecuciones reportadas desde Colab, no de una reproducción
local sobre el checkpoint.

Repetición comunicada por el usuario: dos lotes nuevos de 16.384 bytes, un
holdout común de 16.384 bytes, artículos distintos entre los tres archivos y
copias nuevas del checkpoint para cada corrida. Los resultados fueron:

| Lote | LR | Train BPC | Eval BPC |
|---|---:|---:|---:|
| A | 0,03 | 3,785 | **3,0968** |
| A | 0,08 | 3,671 | 3,1025 |
| A | 0,24 | 3,522 | 3,1184 |
| B | 0,03 | 3,318 | **3,0920** |
| B | 0,08 | 3,292 | 3,0935 |
| B | 0,24 | 3,257 | 3,1172 |

LR 0,03 tuvo el mejor eval en ambos lotes. Medias simples sobre los dos lotes:
0,03 = 3,0944; 0,08 = 3,0980; 0,24 = 3,1178 BPC. En ambos, LR 0,24 obtuvo
menor train BPC pero peor holdout, patrón compatible con una actualización menos
generalizable; no demuestra por sí solo sobreajuste. La dirección del efecto es
consistente en dos lotes, pero ambos usan el mismo holdout. El usuario evaluó
después el checkpoint original, sin actualizar, sobre ese holdout de 16.384
bytes: 3,1233 BPC. Comparado con este baseline exacto, los seis ajustes
reportados mejoraron la evaluación: LR 0,03 por 0,0265 / 0,0313 BPC en lotes
A/B; LR 0,08 por 0,0208 / 0,0298; LR 0,24 por 0,0049 / 0,0061. Esto aporta
evidencia directa de que el ajuste del lector con esos lotes mejora la
predicción sobre el holdout común; LR 0,03 dio la mayor mejora en ambos lotes.
El usuario repitió con otro lote y un segundo holdout independiente. El
checkpoint original obtuvo 3,0865 BPC; después de entrenar, LR 0,03 obtuvo
3,0540 (train 3,001), LR 0,08 obtuvo 3,0535 (train 2,981) y LR 0,24 obtuvo
3,0634 (train 2,957). Los tres ajustes mejoraron frente al original por 0,0325,
0,0330 y 0,0231 BPC respectivamente. En el nuevo holdout, 0,08 quedó 0,0005
BPC delante de 0,03, diferencia mínima; 0,24 volvió a ser el peor en eval aunque
el train fuera mejor. Así, la señal de que el ajuste del lector ayuda y de que
0,03/0,08 superan a 0,24 se repitió en dos holdouts distintos. El primer holdout
se usó con dos lotes de entrenamiento y el segundo con uno. Evidencia aún
preliminar —holdouts de 16 KB, mismo checkpoint y dominio Wikipedia—, no es
suficiente para iniciar entrenamiento masivo, pero respalda una prueba
confirmatoria adicional antes de escoger la tasa de producción.

Tercer holdout comunicado por el usuario, posterior a un lote de entrenamiento
de 32.768 bytes. El checkpoint original obtuvo 3,4348 BPC; LR 0,03 obtuvo
3,3985 (train 3,091), LR 0,08 obtuvo 3,3968 (train 3,060) y LR 0,24 obtuvo
3,4057 (train 3,014). Los tres mejoraron al original en 0,0363 / 0,0380 /
0,0291 BPC. LR 0,08 superó a 0,03 por solo 0,0017 BPC; ambas tasas menores
volvieron a superar a 0,24. El patrón de mejora del lector se observó en tres
holdouts; no hay ganador robusto entre 0,03 y 0,08, mientras que 0,24 mostró
menor ganancia relativa en todos. Los datos siguen siendo pilotos de una época,
Wikipedia y holdouts cortos. Se propone ahora un piloto acumulativo: dos lotes
distintos de 32 KB consecutivos por copia, tasas 0,03/0,08/0,24, holdout nuevo
reservado y evaluación solo al final, para comprobar si el beneficio persiste
cuando se asimila más de un bloque sin plasticidad local.

Resultado del piloto acumulativo comunicado por el usuario: dos lotes distintos
de 32.768 bytes fueron asimilados secuencialmente por cada copia (64 KB total),
sin plasticidad local; holdout nuevo de 16.384 bytes evaluado una sola vez al
final. El checkpoint original dio 3,4897 BPC. LR 0,03: lotes train 3,289 y
3,302, eval 3,4148 (mejora 0,0749); LR 0,08: train 3,267 y 3,269, eval 3,4086
(mejora 0,0811); LR 0,24: train 3,237 y 3,225, eval 3,4248 (mejora 0,0649).
Las tres tasas mejoraron el holdout, pero LR 0,08 obtuvo el mejor resultado,
0,0062 BPC por debajo de LR 0,03 y 0,0162 por debajo de LR 0,24. Se confirma
que el beneficio no desapareció al pasar de un solo lote a dos lotes
consecutivos de esta escala. Evidencia de pilotaje, no garantía para corpora
amplios: hace falta probar escalado progresivo con monitorización de validación,
retención y checkpoints recuperables antes de entrenamiento masivo.
