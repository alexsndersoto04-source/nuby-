# Nuby Studio

Estudio de grabación multipista que funciona en el navegador. Graba tu micrófono
de verdad (PCM sin comprimir), procesa con efectos reales, mezcla y exporta a
**WAV** o **MP3** de buena calidad. Pensado para el micrófono **BM800** con la
tarjeta de sonido **V8**, pero funciona con cualquier interfaz que el navegador
reconozca como entrada de audio.

No hay demos ni maquetas: cada botón hace lo que dice, y lo que suena es lo que
se exporta (la mezcla se recalcula con la misma cadena de efectos).

---

## Cómo usarlo

### Opción A — instalar la app en Android (APK)

1. Entra en la pestaña **Actions** del repositorio y abre el workflow
   **"Compilar APK"**.
2. Pulsa **Run workflow** (rama `main`) y espera unos minutos.
3. Al terminar, descarga **Nuby-Studio.apk** desde *Artifacts* — o, si la
   compilación vino de `main`, desde **Releases**, que no pide cuenta:
   <https://github.com/alexsndersoto04-source/nuby-/releases/download/apk/Nuby-Studio.apk>
4. Abre el archivo en el teléfono y acepta instalar desde "origen desconocido".
   Es un APK **firmado**; las versiones siguientes se instalan encima sin
   desinstalar.

La app es la misma que esta web, dentro de un contenedor nativo: el teléfono no
necesita internet ni servidor. Los archivos exportados se guardan en
**Descargas/Nuby Studio**. Al ser una pantalla pequeña, abajo aparece una barra
con **Pistas · Editor · Canal** para cambiar de panel.

En Android el audio llega por la entrada USB de la V8 (con el cable OTG
correspondiente) o por el micrófono del teléfono. Si el teléfono no detecta la
V8, revisa en el diálogo **Micrófono** que esté elegida como entrada.

### Opción B — abrir la web en el ordenador

1. Descarga `Nuby-Studio-1.0.zip` (se genera con `python3 tools/empaquetar.py`).
2. Descomprímelo y ejecuta el lanzador de tu sistema:
   - Windows: `ABRIR-EN-WINDOWS.bat`
   - macOS: `ABRIR-EN-MAC.command`
   - Linux: `ABRIR-EN-LINUX.sh`
3. Se abre el navegador con el estudio. **No cierres la ventana negra** mientras
   trabajas: es el motor local. Necesita tener instalado **Python 3** o **Node.js**
   (el lanzador usa el que encuentre).

El lanzador hace falta porque los navegadores solo permiten micrófono y
`AudioWorklet` en `localhost` o HTTPS: abriendo `index.html` con doble clic
(`file://`) el micrófono queda bloqueado.

### Opción C — servidor de desarrollo

```bash
npm install        # solo para las pruebas automáticas
npm run dev        # http://localhost:4173
```

### Opción D — publicarlo como sitio (GitHub Pages, Netlify, etc.)

Es un sitio 100 % estático: basta con subir el contenido del repositorio. En este
repositorio GitHub Pages ya está configurado para servir la rama `main` en
`https://alexsndersoto04-source.github.io/nuby-/`; al fusionar los cambios, la
app queda disponible en esa dirección y se puede **instalar como aplicación**
desde Chrome (menú > Instalar Nuby Studio), también en Android.

Abre la dirección en **Chrome, Edge o Firefox**. El micrófono exige un contexto
seguro: `localhost` o HTTPS. No hay backend, todo ocurre en el propio equipo, y
al ser una PWA se puede instalar y usar sin conexión.

### Primeros pasos en el teléfono (Redmi 9C y similares)

1. Abre la app y toca **Activar micrófono** (tarjeta amarilla en *Pistas*).
   Android pedirá permiso: acéptalo. Es lo único que hace falta para empezar.
2. Conecta la **V8** por USB-OTG y elige la entrada en **Ajustes › Sistema ›
   Probar micrófono**. Si no aparece, revisa que el cable OTG sea de datos.
3. Abajo en la barra hay tres pestañas: **Pistas · Editor · Canal**. En el
   teléfono se ve un panel a la vez; el editor ocupa toda la pantalla.
4. Al grabar, la pantalla se queda encendida y el botón *atrás* solo cierra el
   diálogo abierto (no cierra la app ni pierde la sesión).

### Primeros pasos con el BM800 + V8

1. Conecta el BM800 a la entrada **XLR** de la V8 con cable XLR.
2. Activa **+48V** (alimentación fantasma) en la V8. Sin esto, el BM800 no suena.
3. Conecta la V8 por USB y abre **Micrófono** en la app; elige `V8` en la lista.
4. Apaga el **ECO/REVERB de la V8**: se graba en seco y la reverb se añade aquí,
   con control real y sin quedar pegada a la grabación.
5. Sube el **gain** de la V8 hasta que la voz llegue a **−18 / −12 dBFS**
   (el medidor de entrada no debe llegar al rojo).
6. Arma la pista (botón rojo), pulsa **Grabar**. Si activas el metrónomo, la app
   hace la cuenta de entrada antes de empezar.
7. Graba tomas, elige la buena, aplica efectos y exporta.

Usa **auriculares** si vas a monitorizar: con altavoces el micrófono acopla.

---

## Qué incluye (todo funcional)

### Grabación
- Captura **PCM float de 32 bits a 48 kHz** mediante `AudioWorklet`, sin la
  compresión y la latencia variable de `MediaRecorder`.
- Inicio de grabación alineado a un **fotograma exacto** del reloj de audio, de
  modo que las tomas sucesivas (overdubs) quedan sincronizadas entre sí.
- **Compensación de latencia** automática (la informada por el dispositivo) más
  un ajuste manual en milisegundos.
- Selección de dispositivo, modo de canal (mono izquierdo/derecho/suma o
  estéreo), ganancia de entrada y monitorización con aviso de acople.
- **Cuenta de entrada** de 0 a 2 compases y metrónomo sintetizado y agendado en
  el reloj de audio.
- Limpieza de los procesadores del navegador (cancelación de eco, supresión de
  ruido y AGC desactivados) para no degradar la voz.

### Edición
- Línea de tiempo con regla, rejilla por compases, zoom y ajuste (snap) opcional.
- Mover, recortar por los extremos, **fundidos de entrada y salida** arrastrables.
- Cortar en el cabezal, duplicar, eliminar, **normalizar**, **invertir** y
  **recortar silencios**.
- Bucle de reproducción (Mayús + arrastre sobre la regla).
- Importar pistas de audio (WAV, MP3, M4A…) para grabar encima.
- Deshacer y rehacer (60 pasos).

### Mezcla y efectos (por pista)
- **Puerta de ruido** con umbral, atenuación, ataque, mantenimiento y liberación.
- **Paso alto** (20 Hz – 20 kHz, escala logarítmica).
- **Ecualizador de 4 bandas** (estantes de graves/agudos y dos campanas).
- **Compresor** (umbral, proporción, ataque, liberación, codo y compensación).
- **Saturación** con drive y mezcla seco/húmedo.
- **Envíos** a reverb por convolución (5 salas sintetizadas) y delay con
  realimentación.
- Fader con escala de consola (0,8 = 0 dB, máximo +6 dB), panorama y mute/solo.
- Vúmetro de pico en tiempo real por pista.

### Máster
- EQ de 3 bandas, compresor de bus y **limitador con lookahead** de 5 ms que
  informa la reducción real aplicada.
- Medición de **sonoridad integrada LUFS** según ITU-R BS.1770-4 (ponderación K,
  bloques de 400 ms con solape del 75 % y puertas absoluta y relativa).

### Exportación
- Mezcla fuera de línea en `OfflineAudioContext` con **la misma topología** que
  la escucha (pistas → envíos → buses → máster).
- **WAV** de 16, 24 o 32 bits float y **MP3** de 128 a 320 kbps (codificador LAME
  incluido, con etiquetas ID3).
- Opciones de normalizar a −1 dBFS, exportar solo el bucle o toda la sesión,
  aplicar o no el máster y exportar **cada pista por separado (stems)**.
- Informe de sonoridad y pico real del archivo generado.

### Proyectos
- Guardado automático en **IndexedDB** (audio incluido, sin base64 ni pérdida).
- Lista de proyectos, apertura, borrado y limpieza de audios huérfanos.
- Funcionamiento sin conexión e instalación como aplicación (PWA).

---

## La app Android (carpeta `android/`)

El APK no es otra aplicación: es este mismo estudio dentro de un `WebView`. El
contenedor nativo (`android/app/src/main/java/com/nuby/studio/MainActivity.java`)
solo añade lo que el navegador no puede dar:

| Necesidad | Cómo se resuelve |
| --- | --- |
| Micrófono y `AudioWorklet` | Los archivos se sirven como `https://localhost/...` interceptando las peticiones: es un *origen seguro*, así que `getUserMedia` y los worklets funcionan. Con `file://` no funcionarían. |
| Permiso de micrófono | `RECORD_AUDIO` al arrancar y concesión del permiso al `WebView` en `onPermissionRequest`. |
| Exportar MP3/WAV | Los archivos no se pueden descargar desde un `WebView`: JavaScript envía el `Blob` por el puente `window.NubyNative` y Java lo escribe en **Descargas/Nuby Studio** (`MediaStore` en Android 10+, carpeta pública en versiones anteriores). |
| Importar audio | Selector de archivos nativo (`onShowFileChooser`). |
| Grabaciones largas | Pantalla siempre encendida (`FLAG_KEEP_SCREEN_ON`) y `largeHeap`. |
| Pantalla pequeña | Barra **Pistas · Editor · Canal** para ver un panel a la vez. |
| Botón atrás | Cierra el diálogo abierto; si no hay ninguno, deja la app en segundo plano sin perder la sesión. |

El `Gradle` copia `index.html`, `css/`, `js/` y `assets/` dentro de
`assets/www` en cada compilación, así que el APK siempre lleva la última versión
del estudio sin duplicar código en el repositorio.

**Audio que se comprueba solo.** Al arrancar, el estudio renderiza un tono de
prueba a través de la cadena maestra en un contexto offline. Si el dispositivo
no deja pasar la señal por los `AudioWorklet` (pasa en algunos WebView de
Android, donde la entrada llega vacía y todo sonaría en silencio), la app pasa
automáticamente al camino de respaldo —limitador suave, grabación por
`ScriptProcessorNode`— y lo avisa en **Diagnóstico**. La grabación, además,
captura por los dos caminos a la vez y se queda con el que trae señal, así que
una toma no puede quedar muda sin avisar.

El workflow **"Compilar APK"** (`.github/workflows/android.yml`) compila, firma y
comprueba el APK en cada envío de código, e incluye una prueba opcional que
instala el APK en un emulador de Android para verificar que la interfaz arranca
(se lanza a mano desde *Actions › Compilar APK › Run workflow*; no bloquea la
entrega porque depende de la virtualización del servidor).

**Firma.** `android/nuby-release.p12` es la clave con la que se firma el APK
(contraseña `nuby-studio-apk`, alias `nuby`). Al ser siempre la misma, las
actualizaciones se instalan encima de la versión anterior. Está en el repositorio
a propósito para que GitHub Actions pueda firmar sin configurar secretos; si
alguna vez publicas la app en Google Play, genera tu propia clave y no compartas
esta.

## Cómo está construido

```
index.html                 interfaz (una sola página, sin framework)
css/app.css                diseño de consola (grafito, sin neón)
js/main.js                 punto de entrada
js/state.js                modelo del proyecto
js/audio/engine.js         AudioContext, entrada, buses, medidores
js/audio/track-chain.js    cadena de efectos (compartida en vivo y en render)
js/audio/transport.js      reproducción, bucle, metrónomo y agendado
js/audio/recorder.js       captura PCM (AudioWorklet + respaldo)
js/audio/render.js         mezcla fuera de línea
js/audio/mp3.js            codificador MP3 e ID3
js/audio/wav.js            codificador WAV
js/audio/lufs.js           sonoridad BS.1770
js/audio/impulse.js        generación de respuestas al impulso
js/audio/peaks.js          picos para dibujar formas de onda
js/audio/worklets/*.js     gate, limitador, grabador y medidor LUFS
js/ui/*.js                 pistas, línea de tiempo, inspector y diálogos
js/storage/db.js           IndexedDB
tests/                     pruebas del núcleo e integración
```

Decisiones técnicas relevantes:

- **AudioWorklet** para todo lo que necesita muestra a muestra (grabación,
  puerta, limitador, LUFS). Si el navegador no lo soporta, hay respaldo con
  `ScriptProcessorNode` y un limitador de saturación suave, avisado en el
  diagnóstico.
- **Una sola función construye la cadena de efectos**, y se usa tanto en el
  `AudioContext` como en el `OfflineAudioContext`. Así no hay diferencias entre
  lo que se escucha y lo que se exporta.
- **Agendado con anticipación** de 300 ms sobre el reloj de audio: nada depende
  de `setTimeout` para sonar a tiempo.
- Los clips se agendan **completos** (no fragmentados por la ventana de
  anticipación), para que una toma larga no se corte.
- La reverb usa **IR sintetizadas** con semilla determinista: la mezcla suena
  igual que en la escucha.

---

## Pruebas

```bash
npm test
```

32 pruebas automáticas, sin navegador:

- **Núcleo** (16): cabeceras WAV de 16 y 24 bits, MP3 real (tramas MPEG, bitrate,
  duración, ID3), LUFS con señales de referencia, picos, envolventes y modelo.
- **Interfaz** (10): arranque, pistas, apertura de la entrada V8 simulada,
  grabación completa, exportación MP3 y WAV verificadas byte a byte, edición,
  guardado y recuperación en IndexedDB, diagnóstico.
- **Transporte** (6): agendado en su hora exacta, bucle encadenado, mute/solo,
  recableado al activar efectos, compensación de latencia y metrónomo.

Las pruebas de interfaz usan jsdom, `fake-indexeddb` y un Web Audio simulado que
respeta la forma de la API (nodos, parámetros, conexiones y buffers reales).

---

## Limitaciones (para no vender humo)

- **No hay corrección de afinación automática** (autotune) ni soporte de
  complementos **VST/AU**; no hay instrumentos **MIDI** ni secuenciador de notas.
- La grabación es de **una entrada a la vez** (la que elijas). La V8 expone una
  sola señal de micrófono; para grabar dos fuentes simultáneas hace falta una
  interfaz con más entradas.
- La calidad final depende del micrófono, la sala y la técnica. El software no
  compensa una mala captura.
- El audio de los proyectos vive en **IndexedDB del navegador**: si borras los
  datos del sitio, se pierden. Exporta tus mezclas y no dependas de una sola copia.
- En Safari algunas funciones (AudioWorklet en contextos offline, códecs) pueden
  faltar; la app lo detecta y avisa en el **Diagnóstico**. Recomendado: Chrome,
  Edge o Firefox actualizados.
- El tamaño del proyecto en disco crece rápido: 1 minuto de audio mono a 48 kHz
  en float 32 ocupa ≈ 11 MB en memoria (y otro tanto en IndexedDB).

---

## Licencia y créditos

Código propio del proyecto. El codificador MP3 usa **lamejs**
(@breezystack/lamejs), licencia **LGPL-3.0**; el aviso está en
[`LICENSE-lamejs.txt`](LICENSE-lamejs.txt).
