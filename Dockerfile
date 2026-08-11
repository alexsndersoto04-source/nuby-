# ============================================================================
#  NUBY 2.0 — Dockerfile para hosting real (Render, Fly, Koyeb, Oracle, etc.)
#
#  Aquí NO hay trampa: esta imagen contiene el binario C++20 de Nuby y los
#  binarios que el motor usa como puentes REALES:
#    - openssl s_client para TLS (HTTPS)
#    - convert (ImageMagick) para JPEG/GIF (libjpeg-turbo/libgif reales)
#    - gzip para Content-Encoding: gzip/deflate
#  Nada de simulación: todo es decodificación/descompresión real.
# ============================================================================

# ---------- Etapa 1: compilación (con tests — si fallan, falla el build) ----------
FROM debian:bookworm-slim AS build
RUN apt-get update && \
    apt-get install -y --no-install-recommends g++ make && \
    rm -rf /var/lib/apt/lists/*
WORKDIR /src
COPY . .
RUN cd nuby_engine && make -j"$(nproc)" && make test

# ---------- Etapa 2: runtime mínimo ----------
FROM debian:bookworm-slim
RUN apt-get update && \
    apt-get install -y --no-install-recommends openssl ca-certificates imagemagick gzip && \
    rm -rf /var/lib/apt/lists/*
WORKDIR /app
COPY --from=build /src/nuby_engine/bin/nuby_engine   /app/nuby_engine
COPY --from=build /src/nuby_engine/data/crawl_pages.tsv /app/data/crawl_pages.tsv
COPY --from=build /src/nuby_engine/data/fonts /app/data/fonts

# La plataforma de hosting inyecta PORT; main.cpp lo respeta (ver src/main.cpp)
ENV PORT=8080
EXPOSE 8080

# Ejecutar desde /app para que el índice se encuentre en data/crawl_pages.tsv
CMD ["./nuby_engine"]
