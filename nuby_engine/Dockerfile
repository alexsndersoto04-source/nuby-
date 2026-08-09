# ============================================================================
#  NUBY 2.0 — Dockerfile para hosting real (Render, Fly, Koyeb, Oracle, etc.)
#
#  Aquí NO hay trampa: esta imagen contiene el binario C++20 de Nuby y el
#  binario de OpenSSL (que el fetcher usa de puente TLS para páginas HTTPS).
#  Nada de Chrome, nada de Electron: el motor pinta y sirve sus píxeles.
# ============================================================================

# ---------- Etapa 1: compilación (con tests — si fallan, falla el build) ----------
FROM debian:bookworm-slim AS build
RUN apt-get update && \
    apt-get install -y --no-install-recommends g++ make && \
    rm -rf /var/lib/apt/lists/*
WORKDIR /src
COPY . .
RUN make -j"$(nproc)" && make test

# ---------- Etapa 2: runtime mínimo ----------
FROM debian:bookworm-slim
RUN apt-get update && \
    apt-get install -y --no-install-recommends openssl ca-certificates && \
    rm -rf /var/lib/apt/lists/*
WORKDIR /app
COPY --from=build /src/bin/nuby_engine   /app/nuby_engine
COPY --from=build /src/data/crawl_pages.tsv /app/data/crawl_pages.tsv

# La plataforma de hosting inyecta PORT; main.cpp lo respeta (ver src/main.cpp)
ENV PORT=8080
EXPOSE 8080

# Ejecutar desde /app para que el índice se encuentre en data/crawl_pages.tsv
CMD ["./nuby_engine"]
