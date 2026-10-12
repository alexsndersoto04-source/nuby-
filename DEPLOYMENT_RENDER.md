# Despliegue de Nuby

Este documento describe solo lo que está comprobado. Nada de promesas de
servicio permanente ni de repositorios que no son este.

## 1. Lo que ya está publicado (automático)

**<https://alexsndersoto04-source.github.io/nuby-/>** — el motor Nuby compilado a
WebAssembly ejecutándose en el navegador del visitante, sin servidor.

- Se publica desde `.github/workflows/deploy.yml` en cada push a `main`.
- Antes de publicar, el workflow **debe** pasar:
  1. `native`: compila el motor C++ y ejecuta `make test` (20/20 suites).
  2. `browser-build`: compila a wasm, ejecuta `nuby_engine/wasm/node_check.mjs`
     y `nuby_engine/wasm/browser_check.mjs` en un Chrome real (con capturas
     guardadas como artefacto). Si no hay navegador disponible, el job falla.
  3. `deploy`: sube el directorio `web/` a GitHub Pages.
- Si cualquiera de esos pasos falla, **no** se publica nada nuevo: la versión
  anterior sigue en línea.

Límites conocidos de esa versión (CORS, sin JS de páginas, sin escritura en
disco): ver `about.html` en el propio sitio.

## 2. Servidor propio (opcional, no verificado por el proyecto)

El repositorio incluye `Dockerfile` y `render.yaml` para quien quiera levantar
el servidor nativo (motor + red + JavaScript reales) en un servicio que ejecute
Docker. Pasos generales:

1. Crea una cuenta en el proveedor y conecta este repositorio.
2. Crea un **Web Service** con runtime **Docker** apuntando a `Dockerfile`.
3. El motor escucha en el puerto que indique la variable `PORT` (por defecto 8080).

Advertencias honestas:

- **No hay ningún despliegue de este tipo hecho ni verificado por el proyecto**;
  la URL de ejemplo que aparecía antes en este documento no existe como servicio
  de Nuby.
- Muchos planes gratuitos **suspenden** el servicio cuando no recibe tráfico y lo
  despiertan con lentitud la primera vez. No es un servicio "24/7 de por vida":
  eso depende del proveedor que elijas, no de este repositorio.
- El servidor nativo guarda el historial y las páginas indexadas en la memoria
  del proceso; en un plan pequeño conviene limitar las sesiones (ya hay un tope
  de 24 sesiones simultáneas y 90 minutos de inactividad por sesión).

## 3. Ejecutarlo en tu propia máquina

```bash
cd nuby_engine
make
make test
./bin/nuby_engine --port 8080   # abre http://localhost:8080
```

Esta es la única forma que el proyecto puede comprobar de verdad sin depender de
un tercero, y es la que se usa en las pruebas del repositorio.
