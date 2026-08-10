# Imagen mínima para publicar la interfaz estática de Nuby en Render.
FROM nginx:1.27-alpine

# La configuración por defecto de nginx sirve este archivo en la ruta /.
COPY index.html /usr/share/nginx/html/index.html

EXPOSE 80
