# Despliegue 24/7 de Nuby en Servidor Gratuito (Render / Cloud)

## 1. Despliegue Automático en Render.com (Gratuito de por vida)

El repositorio de **Nuby** ya cuenta con el `Dockerfile` y `render.yaml` preconfigurados para compilar en C++20 y mantenerse activo 24/7 en la nube sin costo.

### Pasos para Activar tu Servidor Nuby en Render:
1. Ve a [Render.com](https://render.com) y crea una cuenta gratuita con tu GitHub.
2. Haz clic en **"New +"** -> **"Web Service"**.
3. Selecciona tu repositorio de GitHub: `juego-fabrica` (rama `arena/019fda5b-juego-fabrica` o `main`).
4. En **Runtime**, selecciona **Docker** (Render detectará automáticamente el `Dockerfile`).
5. En **Instance Type**, selecciona **Free ($0/month)**.
6. Haz clic en **"Create Web Service"**.

---

## 2. Lo que Obtendrás
* Tu propio dominio público con certificado SSL gratuito (ejemplo: `https://nuby-browser.onrender.com`).
* El servidor C++20 de **Nuby** ejecutándose de por vida en la nube con consumo menor a 20 MB de RAM.
* Indexación persistente por lotes y acceso global desde cualquier teléfono o computadora del mundo.
