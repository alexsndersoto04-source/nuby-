# Lanzar el banco de medida en Kaggle

Todo el cálculo pesado ocurre en Kaggle. El teléfono solo manda el trabajo y
recibe el resultado.

---

## Lo único que hay que hacer: una vez

Necesitas el token de tu cuenta de Kaggle. Se baja **una sola vez**:

1. Entra en <https://www.kaggle.com/settings> y busca **"Create New Token"**.
2. Se descarga un fichero llamado `kaggle.json`. **No me lo pegues en el chat.**
3. En Termux:

```bash
pkg install python git -y
pip install kaggle
mkdir -p ~/.kaggle
# mueve aquí el kaggle.json que descargaste (por ejemplo, desde Descargas)
mv ~/storage/downloads/kaggle.json ~/.kaggle/kaggle.json
chmod 600 ~/.kaggle/kaggle.json
kaggle --version          # si responde, ya está
```

---

## Ruta A — desde Termux (recomendada)

```bash
pkg install git -y
cd ~
git clone https://github.com/alexsndersoto04-source/nuby-.git
cd nuby-
git checkout arena/c7deda7f-nuby

cd kaggle
kaggle kernels push
```

Eso es todo. Tarda un rato en arrancar.

**Para ver cómo va o recoger el resultado:**

```bash
kaggle kernels status   alexsndersoto04/cortex-lm-banco-texto-real
kaggle kernels output   alexsndersoto04/cortex-lm-banco-texto-real -p ~/resultado
```

El resultado queda en `~/resultado/cortex-lm-banco-texto-real.log`. Lo que me
interesa es el bloque final que empieza por `RESUMEN`. **Pégame ese bloque en
el chat** (es la tabla, no contiene nada privado).

---

## Ruta B — desde el navegador del teléfono, sin termux

1. Entra en <https://www.kaggle.com/code> y pulsa **New Notebook**.
2. Arriba a la derecha: **File → Import Notebook**, y elige… nada. Mejor:
   en el panel derecho busca **"Upload"** y sube `kaggle/run_cortex_lm.py`.
   Si no te deja, crea un cuaderno vacío, borra su contenido y pega dentro el
   contenido completo de `kaggle/run_cortex_lm.py`.
3. En el panel derecho, en **Settings**:
   - **Internet: ON** (imprescindible, si no no puede bajar el texto ni el repo)
   - **Accelerator: None** (no hace falta GPU; es C++ en un núcleo)
4. Pulsa **Run All**.

Si te pide verificar el teléfono para activar Internet, hazlo una vez y ya.

---

## Qué va a pasar

El cuaderno hace esto, en orden:

1. **Baja texto español de verdad.** Prueba varias fuentes: un dataset que
   tengas adjunto, Wikipedia en español vía Hugging Face, y si falla, el
   Quijote y otras novelas del Proyecto Gutenberg.
2. **Audita el corpus antes de usarlo.** Aquí está la lección de esta sesión:
   el corpus anterior era el mismo trozo repetido 40 veces, así que el 98 % del
   test estaba en el entrenamiento y todas las cifras eran falsas. Ahora el
   script mide cuántas ventanas de 80 caracteres son únicas. Si salen repetidas,
   **se planta y no mide nada**. Si sale un alfabeto de 36 símbolos, también.
3. **Compila el motor** C++ del repositorio.
4. **Corre la auditoría** de las cinco propiedades estructurales.
5. **Corre los dos bancos**, con y sin plasticidad, sobre texto que la red
   nunca vio.
6. Imprime la tabla final.

## Cómo leer el resultado

```
prueba               bpc   acierto   bigrama    mejora
A sin               ...        ...       ...       ...
```

- **bpc** = preguntas que necesita por letra. **Más bajo es mejor.**
- **acierto** = porcentaje de letras siguientes adivinadas.
- **bigrama** = referencia tonta (solo mira la letra anterior). Hay que batirla.
- **mejora** = cuánto se gana sobre esa referencia. Positivo es bueno.

La comparación que decide todo es **`B sin` contra `B con_*`**: mismo
presupuesto, mismo corpus, lo único que cambia es si la plasticidad local está
encendida. Si `B con` baja de `B sin`, la plasticidad aporta y el proyecto va
por buen camino. Si no, hay que cambiar la regla de aprendizaje, y eso ya lo
decido yo con los números delante.

`A` es un control: mide el motor con el lector entrenado aparte, para saber si
el motor en sí guarda información. Sirve para distinguir "el motor no sirve" de
"la regla de aprendizaje no sirve".

---

## Ajustes opcionales

Se pueden cambiar como variables de entorno en una celda antes de ejecutar:

| variable | por defecto | para qué |
|---|---|---|
| `CORTEX_TRAIN_CHARS` | `60000` | caracteres de entrenamiento |
| `CORTEX_HOLDOUT_CHARS` | `12000` | caracteres de test |
| `CORTEX_EPOCHS_B` | `8` | épocas del banco conjunto |
| `CORTEX_BRANCH` | `arena/c7deda7f-nuby` | rama del repo |

Si una corrida tarda demasiado, baja `CORTEX_TRAIN_CHARS` a `30000`. Si quieres
más fiabilidad, súbelo a `200000` (tardará bastante más).

---

## Si algo falla

- **"No se pudo bajar texto español"** → activa Internet en Settings, o adjunta
  un dataset con un `.txt` grande en español (se detecta solo).
- **"CORPUS REPETIDO"** → la fuente era basura repetida. Cambia de fuente; el
  script hace bien en negarse.
- **"auditoría FALLÓ"** → pégame la salida y lo miro; el motor no debería fallar.
- **Se queda sin memoria** → no debería; baja `CORTEX_TRAIN_CHARS`.
