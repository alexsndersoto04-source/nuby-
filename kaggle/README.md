# Lanzar el banco de medida en Kaggle

Todo el cálculo pesado ocurre en Kaggle. El teléfono solo manda el trabajo y
recibe la tabla.

---

## Ruta Termux (la que funciona hoy)

`pip install kaggle` **no compila en Termux**: arrastra `rpds-py`, que para
Android solo existe como código fuente y necesita Rust. Por eso todo se
instala dentro de una Debian mínima, donde sí hay paquetes precompilados.

**Una sola vez.** En Termux, pega estas dos líneas:

```bash
pkg install -y curl
curl -fsSL https://raw.githubusercontent.com/alexsndersoto04-source/nuby-/main/termux_setup.sh -o setup.sh && bash setup.sh
```

Instala Debian (~400 MB, tarda un rato), Python, la herramienta de Kaggle y
busca tu `kaggle.json` **solo**. Si no lo encuentra, te dice exactamente dónde
ponerlo.

**Para lanzarlo:**

```bash
curl -fsSL https://raw.githubusercontent.com/alexsndersoto04-source/nuby-/main/termux_run.sh -o run.sh && bash run.sh
```

Eso sube el trabajo, espera y al final imprime la tabla. Si quieres lanzarlo y
cerrar Termux:

```bash
bash run.sh --no-wait
```

y más tarde, para recoger el resultado:

```bash
bash run.sh --fetch
```

### Si te sale "hay que verificar el teléfono"

Es de Kaggle, no del script. Abre el enlace que te da en el navegador, verifica
el número una vez y vuelve a lanzar. El script te da el enlace exacto.

---

## Ruta navegador (alternativa)

1. Entra en <https://www.kaggle.com/code> y pulsa **New Notebook**.
2. Abre en otra pestaña:
   `https://raw.githubusercontent.com/alexsndersoto04-source/nuby-/main/kaggle/run_cortex_lm.py`
   y copia todo el texto.
3. En el cuaderno, borra lo que trae y pega lo copiado.
4. **Settings**: Internet **ON**, Accelerator **None**.
5. **Run All**.

---

## Ruta sin credenciales (si el kaggle.json da guerra)

No necesita `kaggle.json`, ni `pip`, ni Debian. Se hace todo desde el navegador
del teléfono.

1. Entra en <https://www.kaggle.com> y accede a tu cuenta.
2. Arriba a la derecha: **Create → New Notebook**.
3. En **Settings** (panel derecho):
   - **Internet → ON** (obligatorio)
   - **Accelerator → None**
4. Pega el código del cuaderno:
   abre en otra pestaña

   ```
   https://raw.githubusercontent.com/alexsndersoto04-source/nuby-/main/kaggle/run_cortex_lm.py
   ```

   y si no puedes copiar, basta con ejecutarlo sin pegarlo: crea una celda y
   escribe estas cuatro líneas

   ```python
   import urllib.request
   url = "https://raw.githubusercontent.com/alexsndersoto04-source/nuby-/main/kaggle/run_cortex_lm.py"
   code = urllib.request.urlopen(url).read().decode()
   exec(compile(code, "run_cortex_lm.py", "exec"), {"__name__": "__main__"})
   ```

   Esa celda son cuatro líneas cortas, se pueden escribir a mano.
5. Pulsa **Run All**.

Al terminar, la tabla sale al final del registro del cuaderno (sección
**Log**), empezando por la palabra `RESUMEN`.

### Por qué esto es más fiable

`kaggle.json` es una llave privada de tu cuenta. Si se pierde, hay que ir a
Settings y regenerarla. La ruta del navegador no maneja la llave en ningún
momento: tú ya has iniciado sesión, y el cuaderno corre en tu cuenta.

## Qué va a pasar

1. **Baja texto español de verdad.** Prueba un dataset adjunto, Wikipedia en
   español vía Hugging Face, y si falla, el Quijote y otras novelas de
   Gutenberg.
2. **Audita el corpus antes de usarlo.** Aquí está la lección de esta sesión:
   el corpus anterior era el mismo trozo repetido 40 veces, así que el 98 % del
   test estaba en el entrenamiento y todas las cifras eran falsas. Ahora mide
   cuántas ventanas de 80 caracteres son únicas. Si salen repetidas, **se
   planta y no mide nada**.
3. **Compila el motor** C++ y corre la auditoría de cinco propiedades.
4. **Corre los dos bancos**, con y sin plasticidad, sobre texto no visto.
5. Imprime la tabla.

## Cómo leer el resultado

```
prueba               bpc   acierto   bigrama    mejora
A sin               ...        ...       ...       ...
```

- **bpc** = preguntas que necesita por letra. **Más bajo es mejor.**
- **mejora** = cuánto se gana sobre el bigrama (la referencia tonta). Positivo
  es bueno.

La comparación que decide todo es **`B sin` contra `B con_*`**: mismo texto,
misma red, lo único que cambia es si la plasticidad está encendida. Si `B con`
baja de `B sin`, la plasticidad aporta. Si no, hay que cambiar la regla de
aprendizaje.

`A` es un control: mide el motor con el lector entrenado aparte, para
distinguir "el motor no sirve" de "la regla de aprendizaje no sirve".

## Ajustes opcionales

| variable | por defecto | para qué |
|---|---|---|
| `CORTEX_TRAIN_CHARS` | `60000` | caracteres de entrenamiento |
| `CORTEX_HOLDOUT_CHARS` | `12000` | caracteres de test |
| `CORTEX_EPOCHS_B` | `8` | épocas del banco conjunto |
| `CORTEX_TIMEOUT_MIN` | `90` | minutos máximos de espera |

## Si algo falla

- **"No se pudo bajar texto español"** → no hay Internet en el kernel.
- **"CORPUS REPETIDO"** → la fuente era basura repetida; el script hace bien
  en negarse.
- **"auditoría FALLÓ"** → pégame la salida; el motor no debería fallar.
