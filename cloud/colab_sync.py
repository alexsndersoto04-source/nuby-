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

import hashlib
import os
import shutil
import sys
from pathlib import Path

HF_REPO = "alexsndersoto/cortex-macro-1m-backup"
HF_MODEL_FILENAME = "cerebro_macro_1m.bin"
LOCAL_MODEL = Path("cortex-lm/data/cerebro_macro_1m.bin")
# Los headers se escriben como uint32_t nativo en C++. En x86 little-endian,
# 0x4E554233 (comentado como NUB3) aparece en disco como los bytes "3BUN".
VALID_MAGIC_VALUES = {0x4E554259, 0x4E554233}  # NUBY denso, NUB3 compacto


def verify_checkpoint(path: Path) -> str:
    if not path.is_file() or path.stat().st_size < 4:
        raise ValueError(f"Checkpoint ausente o truncado: {path}")
    with path.open("rb") as stream:
        magic = stream.read(4)
    if not any(int.from_bytes(magic, order) in VALID_MAGIC_VALUES for order in ("little", "big")):
        raise ValueError(f"Firma de checkpoint no reconocida: {magic!r} (hex={magic.hex()})")
    digest = hashlib.sha256()
    with path.open("rb") as stream:
        for chunk in iter(lambda: stream.read(8 * 1024 * 1024), b""):
            digest.update(chunk)
    return digest.hexdigest()


def get_token():

    token = os.environ.get("HF_TOKEN", "").strip()
    if not token:
        print("ERROR: Falta HF_TOKEN. Configura el secreto HF_TOKEN en Colab Secrets; no pegues el token en una celda.")
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
    temporal = LOCAL_MODEL.with_name(LOCAL_MODEL.name + ".download")
    try:
        shutil.copy2(path, temporal)
        digest = verify_checkpoint(temporal)
        os.replace(temporal, LOCAL_MODEL)
    except Exception:
        temporal.unlink(missing_ok=True)
        raise
    print(f"OK: modelo listo en {LOCAL_MODEL} ({LOCAL_MODEL.stat().st_size / 1024 / 1024:.1f} MB), SHA-256={digest}")


def cmd_upload(token):
    from huggingface_hub import HfApi

    if not LOCAL_MODEL.exists():
        print(f"ERROR: No existe {LOCAL_MODEL}. No hay nada que subir.")
        sys.exit(1)
    try:
        digest = verify_checkpoint(LOCAL_MODEL)
    except ValueError as exc:
        print(f"ERROR: {exc}. Se cancela la subida.")
        sys.exit(1)

    print(f"Subiendo {LOCAL_MODEL} a {HF_REPO} (SHA-256={digest})...")
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
