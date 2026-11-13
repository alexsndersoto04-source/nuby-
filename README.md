# Ask — App nativa conectada a CORTEX-ML

App conversacional nativa para Android que se conecta directamente a tu cerebro de 1 017 856 neuronas LIF en la nube (arquitectura **CORTEX-ML**). Sin navegador, sin web, sin intermediarios.

## Diseño

- **Interfaz pura blanca** (`#FFFFFF`) con toques púrpura casi imperceptibles.
- **Pensamiento cortical visible**: cápsula orgánica y profesional que muestra la actividad interna de las columnas TRN, certeza, acetilcolina (ACh) y compuerta Go/No-Go.
- **Barra de texto minimalista**: sin bordes, sin dibujos feos, fresca y limpia.
- **Voz humana cálida**: síntesis neuronal desde la nube (`/api/voz`, 24 kHz MP3) con opciones femenina y masculina, y motor nativo de Android como respaldo.

## Arquitectura

- **App**: `Ask` (paquete `com.ask.studio`).
- **Arquitectura subyacente**: `CORTEX-ML` (1 017 856 neuronas LIF puras, ~96,9 millones de sinapsis CSR).
- **Cerebro**: se mantiene en la nube (16 GB RAM). El teléfono es un cliente nativo ligero.
- **Comunicación**: puente HTTP nativo de Java (`HttpURLConnection`) que evita problemas CORS en WebView.

## Conexión a la nube

La app se conecta automáticamente al túnel vivo publicado por el flujo de GitHub Actions `Corteza Viva Macro 1M (16GB RAM 24-7)` a través del archivo `ENLACE_VIVO.txt` respaldado en tu bóveda privada de Hugging Face (`alexsndersoto/cortex-macro-1m-backup`).

Si el servidor está apagado, la app te avisa honestamente: *"Aún estoy reconectando con el servidor de 16 GB RAM en la nube."* Solo debes volver a lanzar el flujo desde GitHub Actions y esperar unos minutos.

## Entrenamiento continuo

El cerebro se alimenta por lotes de Wikipedia en español y libros clásicos (`--continuo`). Cada lote se guarda en tu bóveda privada de Hugging Face (`DIARIO_ASK.json` y checkpoints `.bin`) para que nada se pierda entre sesiones.

## Funcionalidades principales

- Chat en vivo directo a CORTEX-ML.
- Visualización orgánica del pensamiento cortical (pensamiento interno, certeza, columnas TRN activas).
- Síntesis de voz femenina y masculina cálida y natural.
- Dictado por micrófono manos libres.
- Enseñanza directa por texto (`/asimilar`).
- Sueño cortical (`/sonar`) para consolidar memoria.
- Historial local con respaldo privado en Hugging Face.

## Requisitos

- Android 7.0 (API 24) o superior.
- Conexión a Internet.
- Token privado de Hugging Face (`hf_...`) para descubrir el túnel vivo (se ingresa una sola vez en la app).

## Estructura del proyecto

```
.
├── android/                    # App nativa Android (Ask)
│   ├── app/
│   │   ├── build.gradle        # applicationId com.ask.studio
│   │   └── src/main/...
│   └── build.gradle
├── colab/                      # Notebooks para entrenamiento en Google Colab
├── cloud/                      # Scripts de servidor y sincronización con Hugging Face
├── cortex-lm/                  # Motor C++ de neuronas LIF puras (CORTEX-ML)
├── js/                         # Lógica cliente de la app Ask
├── css/                        # Estilos blanco + púrpura sutil
├── index.html                  # Interfaz de Ask
├── manifest.webmanifest        # PWA manifest
└── README.md                   # Este archivo
```

## Compilación

El APK se compila automáticamente en GitHub Actions en cada push. El workflow construye la misma referencia que disparó la ejecución y sube el artefacto como `Ask-APK` (`Ask.apk`).

## Notas

- El nombre de la app es **Ask**. La arquitectura subyacente se conserva como **CORTEX-ML**.
- Todo el cerebro (neuronas, sinapsis, aprendizaje) permanece en la nube; el teléfono solo es el cliente nativo.
- No hay backpropagation, ni modelos de lenguaje externos, ni simulaciones falsas. Solo neuronas LIF puras con plasticidad sináptica local.
