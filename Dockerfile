FROM debian:bookworm-slim

RUN apt-get update && apt-get install -y --no-install-recommends \
    build-essential \
    g++ \
    make \
    libgomp1 \
    python3 \
    python3-pip \
    ca-certificates \
    git \
    && pip3 install --no-cache-dir --break-system-packages huggingface_hub datasets \
    && rm -rf /var/lib/apt/lists/*

# Crear usuario no-root compatible con Hugging Face Spaces (uid 1000) y nubes Docker
RUN useradd -m -u 1000 cortex
WORKDIR /home/cortex/app

COPY --chown=cortex:cortex . /home/cortex/app/

USER cortex

# Compilar con optimización portable multi-arquitectura + OpenMP multinúcleo
RUN cd cortex-lm && make portable -j"$(nproc)"

ENV PORT=7860
ENV PERFIL_CORTEX=macro-1m
ENV LOTES_LIBROS=12
ENV LOTES_WIKI=250
ENV BYTES_LOTE=240000

EXPOSE 7860

CMD ["python3", "cloud/motor_macro_nube_24_7.py"]
