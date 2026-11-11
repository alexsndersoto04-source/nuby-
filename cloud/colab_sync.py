#!/usr/bin/env python3
"""
colab_sync.py — Sincronización del cerebro CORTEX-ML con Hugging Face Hub
para entrenar por partes en Google Colab gratuito.

Uso dentro de Colab:
    python colab_sync.py download  # baja el último cerebro desde HF
    python colab_sync.py upload    # sube el cerebro actualizado a HF

Requisitos:
    - Variable de entorno HF_TOKEN con tu token de Hugging Face (hf_...)
    - pip install huggingface_hub
"""

import os
import sys
from pathlib import Path

HF_REPO = "alexsndersoto/cortex-macro-1m-backup"
HF_MODEL_FILENAME = "cerebro_macro_1m.bin"
LOCAL_MODEL = Path("cortex-lm/data/cerebro_macro_1m.bin")


def get_token():
    token = os.environ.get("HF_TOKEN", "").strip()
    if not token:
        print("ERROR: Falta HF_TOKEN. En Colab: os.environ['HF_TOKEN'] = 'tu_token'")
        sys.exit(1)
    return token


def cmd_download(token):
    from huggingface_hub import hf_hub_download

    LOCAL_MODEL.parent.mkdir(parents=True, exist_ok=True)
    print(f"Descargando {HF_MODEL_FILENAME} desde {HF_REPO}...")
    path = hf_hub_download(
        repo_id=HF_REPO,
        filename=HF_MODEL_FILENAME,
        repo_type="dataset",
        token=token,
    )
    # Copiar a la ruta local esperada por el motor C++
    import shutil
    shutil.copy2(path, LOCAL_MODEL)
    print(f"OK: modelo listo en {LOCAL_MODEL} ({LOCAL_MODEL.stat().st_size / 1024 / 1024:.1f} MB)")


def cmd_upload(token):
    from huggingface_hub import HfApi

    if not LOCAL_MODEL.exists():
        print(f"ERROR: No existe {LOCAL_MODEL}. No hay nada que subir.")
        sys.exit(1)

    print(f"Subiendo {LOCAL_MODEL} a {HF_REPO}...")
    api = HfApi(token=token)
    api.create_repo(repo_id=HF_REPO, repo_type="dataset", exist_ok=True)
    api.upload_file(
        path_or_fileobj=str(LOCAL_MODEL),
        path_in_repo=HF_MODEL_FILENAME,
        repo_id=HF_REPO,
        repo_type="dataset",
    )
    print("OK: modelo subido a Hugging Face Hub.")


if __name__ == "__main__":
    action = sys.argv[1] if len(sys.argv) > 1 else "help"
    token = get_token()

    if action == "download":
        cmd_download(token)
    elif action == "upload":
        cmd_upload(token)
    else:
        print("Uso: python colab_sync.py [download|upload]")
