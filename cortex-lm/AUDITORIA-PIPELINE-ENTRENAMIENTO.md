# Auditoría del pipeline de entrenamiento CORTEX-LM

Fecha: 2026-11-11. Rama: `arena/b0c825e4-nuby`.

## Objetivo y alcance

Esta auditoría sigue el flujo desde corpus y cuadernos Colab hasta las llamadas C++,
checkpoint, metadatos y evaluación. No pretende validar comprensión general ni
certificar un entrenamiento que no pueda reconstruirse desde sus datos/registros.
El checkpoint macro se audita por separado del motor local pequeño.

## Flujo efectivo encontrado

1. `cortex-lm/tools/cerebro.cpp::cmd_entrenar` crea el modelo. Con `--stdp 1`,
   aplica plasticidad local no supervisada solo a los primeros `min(train_bytes,
   65.000)` bytes; después congela Engine y ajusta el lector ventral con el byte
   siguiente como etiqueta. El holdout de `--holdout` es el sufijo del mismo
   archivo y no se usa para actualizar el lector.
2. Los flujos macro actuales usan luego `cerebro alimentar`. Esa rutina desactiva
   la plasticidad general de Engine, pero aprende el lector supervisado byte a
   byte; STDP/homeostasis local solo actúan si se pasan flags explícitos. La
   opción `--semantica` crea/actualiza un índice semántico separado y hace
   consolidación de lectores semántico/episódico; no implica que cada lote de
   Wikipedia vuelva a entrenar las sinapsis corticales con STDP.
3. `entrenar_macro_colab.py`, `motor_macro_nube_24_7.py` y
   `entrenar_wikipedia_continuo.py` son tres rutas distintas. Comparten corpus y
   formato de modelo, pero no protocolo ni formato de registro. Las dos primeras
   tienen entrenamiento de arranque, alineación y lotes continuos; el script de
   Wikipedia es otro alimentador.
4. La fábrica macro mantiene LR `0.24` por defecto para asimilación continua.
   Las comparaciones reportadas desde Colab favorecen `0.03`/`0.08` en lotes
   pequeños, pero no bastan todavía para modificar globalmente la política de
   producción.

## Hallazgos de alta prioridad

### Corregidos en esta iteración

- **Los fallos de lote podían registrarse como si se hubieran entrenado.** En
  `motor_macro_nube_24_7.py`, el worker de sombra antes devolvía `None` tanto en
  éxito como en error; el caller seguía avanzando el registro de artículos y la
  fase. Ahora devuelve éxito/fallo explícito; solo confirma lote/fase tras
  promover el checkpoint, y deja fallos pendientes de reintento. También evita
  marcar completa la fase de clásicos si algún bloque falló.
- **Tamaños de lote declarados en bytes se contaban como caracteres Unicode.**
  `entrenar_macro_colab.py` ahora suma bytes UTF-8 en el streaming y usa un
  separador que corta por bytes sin romper secuencias UTF-8; el tamaño de lotes
  clásicos utiliza el mismo helper.
- **El registro del entrenador macro podía quedar como JSON parcial.** Su
  escritura ahora usa archivo temporal y `os.replace`.
- **El alimentador alternativo `entrenar_wikipedia_continuo.py` podía destruir
  el checkpoint activo durante un fallo.** Ahora entrena en `.pending`, verifica
  firma y promueve solo si el proceso termina bien; registros y descarga local
  también se actualizan mediante reemplazo. Se añadió `--max-lotes`.
- **La celda de Colab que decía `--solo-descargar` ignoraba esa bandera** y
  realmente iniciaba el bucle de entrenamiento (hasta 50 lotes), porque el
  script no tenía parser de argumentos. La opción ya existe y tiene prueba
  regresiva que confirma que no entrena.
- **Las instrucciones Colab pedían pegar el token en una celda.** Se sustituyeron
  por lectura desde Colab Secrets; ya no hay placeholders de token. Las celdas
  de clonación dejaron de ocultar errores con `|| true`, apuntan a la rama
  activa configurable y usan una sola copia fuente.
- **Las celdas de Colab indicaban usar T4**, aunque el motor es C++/OpenMP en CPU.
  Se corrigió esa instrucción.
- **Los workflows macro y de conexión descargaban ramas Arena obsoletas.** Ahora
  usan la referencia que disparó el workflow, en lugar de clonar una rama fija.
- **Los lanzadores Kaggle/Termux apuntaban a repositorio y ramas que ya no
  existen.** Confirmé por GitHub API que `alexsndersoto04-source/ask-` responde
  404, mientras que `nuby-` es el repositorio activo con `main` como default.
  Actualicé defaults Kaggle/Termux a `nuby-`/`main` y dejé variables
  `CORTEX_REPO` / `CORTEX_BRANCH` para seleccionar otra fuente.
- En `cloud/colab_sync.py`, la descarga ahora se copia a `.download`, valida
  firma NUBY/NUB3 y SHA-256, y solo entonces reemplaza el checkpoint local. Una
  descarga corrupta no pisa el archivo anterior; la subida también rechaza una
  firma desconocida. El mensaje de error ya no invita a pegar tokens. Tras el
  primer uso en Colab corregí un detalle de endianness: el `uint32_t` NUB3 de C++
  aparece como `3BUN` en disco little-endian; ambas representaciones se aceptan.
- `colab_train_cortex.ipynb` quedó en **etapa de auditoría de solo lectura**:
  prepara artículos train/holdout disjuntos, guarda títulos y parámetros de split,
  y genera manifiesto comparando CORTEX-LM con baseline; no ejecuta `alimentar`
  ni sube el modelo. Se reserva el entrenamiento para después de revisar métricas.

### Pendientes antes de entrenamiento amplio

- **Todavía hay varias rutas de entrenamiento duplicadas.** Aún falta decidir
  cuál queda como ruta canónica y migrar las demás para que llamen a la misma
  lógica de checkpoint, registro, split y evaluación.
- `entrenar_wikipedia_continuo.py` ya protege el checkpoint, pero sigue usando
  LR `0.24`, no evalúa un holdout antes de promover y su registro no contiene
  huellas por byte/configuración de cada lote. No debería ser la ruta de
  entrenamiento recomendado todavía.
- `entrenar_macro_colab.py` ya usa copias seguras en cambios continuos, pero la
  tasa y la falta de evaluación por lote siguen pendientes. El arranque crea el
  lector sobre corpus semilla y hace alineación/retención, por lo que no es
  equivalente al experimento de una sola llamada `alimentar`.
- Parte del código registra títulos y total de bytes, pero no un manifiesto
  reproducible con SHA-256 de cada archivo, orden, versión del dataset, flags,
  tasa, semillas, hash del binario y métricas de validación. Los dos registros
  remotos comunicados por el usuario tienen esquemas distintos y no se deben
  sumar/interpretar como un solo historial.
- No se puede garantizar que los holdouts aleatorios recientes fueran nuevos
  para el checkpoint: los registros históricos no permiten verificarlo por
  contenido. Son comparaciones reportadas por Colab, no reproducciones locales.
  La nueva etapa Colab crea un split identificable y con títulos disjuntos desde
  ahora, pero tampoco puede certificar que sus artículos no aparecieron en el
  entrenamiento histórico del checkpoint.
- Los BPC históricos de Witten–Bell compartidos previamente requieren repetición:
  encontré que el baseline suavizaba unigramas solo sobre bytes vistos en train.
  Ahora usa el alfabeto completo de 256 bytes para dar probabilidad finita a
  bytes no vistos. El orden 4 reportado antes no debe mezclarse con cifras de
  esta versión corregida sin recalcularlo.
- **Persistencia C++ truncada directamente**: `guardar_cerebro_completo` ahora
  escribe a `.tmp`, valida escritura/cierre y promueve con `rename`, con respaldo
  para plataformas donde no se reemplaza un destino existente. Se verificó con
  un modelo pequeño creado, actualizado, cargado y evaluado; el destino `.tmp`
  desaparece tras el guardado.
- En `motor_macro_nube_24_7.py`, las publicaciones a Hugging Face son avisos
  best-effort; un fallo de subida deja checkpoint local más nuevo que el remoto.
  Falta una marca de respaldo pendiente/verificación de hash antes de afirmar
  que el progreso quedó respaldado.
- La indexación semántica y la alineación cambian componentes distintos del
  lector de siguiente byte. Los próximos experimentos deben registrar esos
  flags por separado y no presentar “asimilar” como una sola clase de
  aprendizaje.

## Cambios y verificación de esta iteración

- Se añadieron `cloud/test_training_safety.py`,
  `cloud/test_evaluation_manifest.py` y `cloud/test_colab_sync_safety.py`.
  Cubren seguridad de candidatos, split UTF-8, hashes/manifiesto, baseline con
  bytes no vistos y descarga HF válida/corrupta.
- `python3 -m unittest cloud.test_training_safety cloud.test_evaluation_manifest
  cloud.test_colab_sync_safety -v`: **13 pruebas pasan**.
- `python3 -m py_compile` de los entrenadores, sincronizador HF, baseline y
  evaluador reproducible; `git diff --check`: pasan.
- `tools/evaluar_reproducible.py` se probó end-to-end con un cerebro sintético
  pequeño: evaluó el mismo holdout en CORTEX-LM y Witten–Bell, produjo JSON con
  hashes y confirmó que los archivos modelo/train/holdout/binario no cambiaron.
  Este smoke valida el arnés, no el checkpoint macro.
- `make portable` y `./bin/cortex_lm audit`: pasan (red local pequeña; no evalúan
  la calidad del macro checkpoint).
- Smoke del guardado atómico: crear modelo pequeño, `alimentar`, cargar/evaluar;
  pasa y no deja `.tmp` residual.
- Tres notebooks Colab parsean como JSON; sus celdas Python parseables pasan
  AST, y los cuadernos ya no contienen instrucciones para pegar tokens.
- `npm ci --ignore-scripts` instala dependencias sin vulnerabilidades; `npm test`
  del producto completo termina con **31/35 aprobadas y 4 fallidas**. Al revisar
  la causa, encontré una incongruencia de alcance en el propio checkout: `index.html`
  presenta Ask como chat y carga `js/ask_app.js`, mientras que parte de los tests
  importa `js/main.js`, que arranca un estudio de audio (`StudioApp`/`TimelineView`)
  y espera nodos ausentes en ese HTML. Por eso fallan los tests de settings/UI/
  transport. No los maquillé ni los conté como validación de CORTEX; queda pendiente
  decidir si se corrigen los tests a la app Ask actual o se restaura el HTML del
  estudio que esos tests esperan.
- No se ejecutó entrenamiento con el checkpoint real ni se solicitaron
  credenciales. El token configurado anteriormente en Colab no debe reutilizarse
  si fue expuesto; no se incluye en código ni documentación.

## Próximos pasos técnicos (orden de trabajo)

1. Terminar y probar la seguridad de checkpoint en todos los runners y consolidar
   una única ruta de entrenamiento macro.
2. Hacer manifiesto de datos/configuración y validación independiente; definir
   criterios de mejora y retención antes del run.
3. Usar una copia del checkpoint y un piloto acumulativo con checkpoint por
   lote, registro de BPC y rollback. Solo progresar de escala si el holdout nuevo
   mejora y las métricas de retención no caen.
4. Tras confirmar el flujo, entrenar sobre el corpus completo reproducible y
   validar una sola vez en un test final sellado.
