FROM gcc:12-bookworm AS builder
WORKDIR /app
COPY . .
RUN cd nuby_engine && make clean && make all

FROM debian:bookworm-slim
# FIX REAL (2026-08-10): la imagen slim NO trae el CLI 'openssl' y el motor
# hace el HTTPS ejecutando `openssl s_client` -> TODO enlace https fallaba
# en Render ("selecciono un enlace y no arroja ningun resultado", dijo el
# usuario). Sin openssl no hay TLS; sin ca-certificates no hay verificación.
# FIX REAL (2026-08-11): JPEG/GIF reales vía `convert` (ImageMagick/libjpeg)
# y gzip/deflate vía `gzip -dc` — sin ellos, 80% de imágenes y muchos CSS
# salían como placeholders/basura.
RUN apt-get update \
 && apt-get install -y --no-install-recommends openssl ca-certificates imagemagick gzip \
 && rm -rf /var/lib/apt/lists/*
WORKDIR /app
COPY --from=builder /app/nuby_engine/bin/nuby_engine /app/nuby_engine
# FIX REAL (2026-08-10): el índice del buscador NO viajaba en la imagen.
# El motor lo busca en data/crawl_pages.tsv (relativo al WORKDIR); sin este
# COPY, el buscador de Render arrancaba con 0 documentos y las búsquedas
# salían vacías. Ahora viajan las 995 páginas rastreadas de verdad.
COPY --from=builder /app/nuby_engine/data/crawl_pages.tsv /app/data/crawl_pages.tsv
# Fuentes TrueType REALES (texto suave; sin ellas el motor usa su bitmap
# interno de respaldo — pixelado "años 80" que el usuario no quiere).
COPY --from=builder /app/nuby_engine/data/fonts /app/data/fonts
EXPOSE 8080
ENV PORT=8080
CMD ["/app/nuby_engine", "--port", "8080"]
