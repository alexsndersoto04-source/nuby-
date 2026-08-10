FROM gcc:12-bookworm AS builder
WORKDIR /app
COPY . .
RUN cd nuby_engine && make clean && make all

FROM debian:bookworm-slim
WORKDIR /app
COPY --from=builder /app/nuby_engine/bin/nuby_engine /app/nuby_engine
# FIX REAL (2026-08-10): el índice del buscador NO viajaba en la imagen.
# El motor lo busca en data/crawl_pages.tsv (relativo al WORKDIR); sin este
# COPY, el buscador de Render arrancaba con 0 documentos y las búsquedas
# salían vacías. Ahora viajan las 995 páginas rastreadas de verdad.
COPY --from=builder /app/nuby_engine/data/crawl_pages.tsv /app/data/crawl_pages.tsv
EXPOSE 8080
ENV PORT=8080
CMD ["/app/nuby_engine", "--port", "8080"]
