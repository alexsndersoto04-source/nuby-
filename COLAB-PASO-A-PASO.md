# Qué hacer ahora: auditoría del checkpoint (Colab, sin entrenar)

## Abre el cuaderno

Haz clic en este enlace:

[**Abrir auditoría CORTEX-LM en Google Colab**](https://colab.research.google.com/github/alexsndersoto04-source/nuby-/blob/arena/b0c825e4-nuby/colab_train_cortex.ipynb)

Si el enlace no abre, descarga `colab_train_cortex.ipynb` desde este repositorio/espacio de trabajo y en Colab elige **Archivo → Subir cuaderno**.

## Antes de ejecutar

1. En Colab elige **Entorno de ejecución → Cambiar tipo de entorno de ejecución → CPU**. No selecciones GPU: este motor C++ no la usa.
2. En la barra izquierda abre **Secrets** (icono de llave). Si ya tienes el secreto `HF_TOKEN`, reutilízalo solo si no fue expuesto. Si aquel token apareció en un error o fue compartido, revócalo primero en Hugging Face y crea uno nuevo.
3. En Secrets crea `HF_TOKEN` con permiso de **lectura** al dataset privado del checkpoint y activa **Notebook access**. No pegues el token en una celda ni lo envíes por el chat.

## Ejecuta la auditoría

4. En el menú, elige **Entorno de ejecución → Ejecutar todas**. Ejecuta las celdas en orden y acepta la autorización de Secrets si Colab la solicita. El cuaderno compila el motor, descarga una copia y arma train/holdout con títulos de Wikipedia distintos.
5. Espera a la última celda. **Esta versión no entrena, no promueve candidatos y no sube archivos a Hugging Face.** Solo evalúa el checkpoint y un baseline en los mismos bytes del holdout.
6. La última celda descargará `evaluacion_checkpoint.json` a tu dispositivo. Adjunta ese JSON aquí. No adjuntes ni compartas el token.

Si aparece un error, copia aquí únicamente el texto del error **después de borrar cualquier token, URL privada o dato secreto**. No ejecutes pasos de entrenamiento por ahora.
