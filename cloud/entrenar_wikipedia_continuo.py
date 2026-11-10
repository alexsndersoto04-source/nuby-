#!/usr/bin/env python3
"""
entrenar_wikipedia_continuo.py — Entrenamiento continuo por lotes de Wikipedia ES
para CORTEX-ML Macro en Google Colab gratuito.

Guarda el progreso en tu bóveda privada de Hugging Face:
  alexsndersoto/cortex-macro-1m-backup

Uso en Colab:
    python cloud/entrenar_wikipedia_continuo.py

Requisitos:
    - Variable de entorno HF_TOKEN con tu token de Hugging Face (hf_...)
    - pip install huggingface_hub datasets
    - El binario C++ compilado: cortex-lm/bin/cerebro
"""

import json
import os
import shutil
import subprocess
import sys
import tempfile
import time
from pathlib import Path

# ── Configuración ──────────────────────────────────────────────────────────
HF_REPO = "alexsndersoto/cortex-macro-1m-backup"
HF_MODEL_FILENAME = "cerebro_macro_1m.bin"
HF_REGISTRO_FILENAME = "registro_wikipedia.json"
LOCAL_MODEL = Path("cortex-lm/data/cerebro_macro_1m.bin")
LOCAL_REGISTRO = Path("cortex-lm/data/registro_wikipedia.json")
CEREBRO_BIN = Path("cortex-lm/bin/cerebro")
BLOQUE_CHARS = 200000  # ~200 KB por lote (ajustable según RAM)
MAX_LOTES_POR_SESION = 50  # seguridad para no saturar Colab

# ── Hugging Face ───────────────────────────────────────────────────────────


def get_token():
    token = os.environ.get("HF_TOKEN", "").strip()
    if not token:
        print("ERROR: Falta HF_TOKEN. En Colab: os.environ['HF_TOKEN'] = 'tu_token'")
        sys.exit(1)
    return token


def hf_download(filename: str, destino: Path, token: str) -> bool:
    """Descarga un archivo desde Hugging Face Hub. Devuelve True si existe y se bajó."""
    from huggingface_hub import hf_hub_download, HfApi

    api = HfApi(token=token)
    try:
        # Verificar si existe
        archivos = api.list_repo_files(repo_id=HF_REPO, repo_type="dataset")
        if filename not in archivos:
            print(f"[INFO] {filename} no existe en la bóveda aún (primera vez).")
            return False
        ruta = hf_hub_download(
            repo_id=HF_REPO,
            filename=filename,
            repo_type="dataset",
            token=token,
        )
        shutil.copy2(ruta, destino)
        print(f"[OK] Descargado {filename} ({destino.stat().st_size / 1024 / 1024:.2f} MB)")
        return True
    except Exception as exc:
        print(f"[WARN] No se pudo descargar {filename}: {exc}")
        return False


def hf_upload(filename: str, origen: Path, token: str):
    """Sube un archivo a Hugging Face Hub."""
    from huggingface_hub import HfApi

    if not origen.exists():
        print(f"[WARN] No existe {origen}, no se sube.")
        return

    api = HfApi(token=token)
    api.create_repo(repo_id=HF_REPO, repo_type="dataset", exist_ok=True)
    api.upload_file(
        path_or_fileobj=str(origen),
        path_in_repo=filename,
        repo_id=HF_REPO,
        repo_type="dataset",
    )
    print(f"[OK] Subido {filename} a {HF_REPO}")


# ── Registro de artículos procesados ──────────────────────────────────────


def cargar_registro() -> dict:
    if LOCAL_REGISTRO.exists():
        with open(LOCAL_REGISTRO, "r", encoding="utf-8") as f:
            return json.load(f)
    return {"articulos_procesados": [], "lotes_completados": 0, "bytes_asimilados": 0}


def guardar_registro(reg: dict):
    LOCAL_REGISTRO.parent.mkdir(parents=True, exist_ok=True)
    with open(LOCAL_REGISTRO, "w", encoding="utf-8") as f:
        json.dump(reg, f, ensure_ascii=False, indent=2)


# ── Descarga de bloques de Wikipedia sin repetir ───────────────────────────


def descargar_bloque_wikipedia(articulos_vistos: set) -> tuple[str, set]:
    """
    Descarga un bloque de artículos de Wikipedia en español que NO estén en articulos_vistos.
    Devuelve (texto_bloque, nuevos_articulos_ids).
    """
    from datasets import load_dataset

    ds = load_dataset("wikimedia/wikipedia", "20231101.es", split="train", streaming=True)

    textos = []
    nuevos_ids = set()
    chars = 0
    articulos_revisados = 0

    for ejemplo in ds:
        titulo = ejemplo.get("title", "")
        texto = ejemplo.get("text", "")

        # Saltar si ya lo procesamos
        if titulo in articulos_vistos:
            continue

        articulos_revisados += 1

        if len(texto) < 100:  # saltar artículos muy cortos
            continue

        textos.append(texto)
        nuevos_ids.add(titulo)
        chars += len(texto)

        if chars >= BLOQUE_CHARS:
            break

        # Seguridad: no revisar miles sin encontrar nada nuevo
        if articulos_revisados > 5000:
            break

    bloque = "\n\n".join(textos)
    return bloque, nuevos_ids


# ── Entrenamiento con el motor C++ ─────────────────────────────────────────


def entrenar_lote(texto_bloque: str, lote_num: int) -> bool:
    """Escribe el bloque a un archivo temporal y ejecuta el binario C++."""
    LOCAL_MODEL.parent.mkdir(parents=True, exist_ok=True)

    with tempfile.NamedTemporaryFile("w", suffix=".txt", encoding="utf-8", delete=False) as tf:
        tf.write(texto_bloque)
        tmp_bloque = tf.name

    try:
        cmd = [
            str(CEREBRO_BIN),
            "alimentar",
            str(LOCAL_MODEL),
            tmp_bloque,
            "--bloque", str(BLOQUE_CHARS),
            "--epocas", "1",
            "--lr", "0.24",
            "--ancla", "0.022",
            "--semantica",
            "--max-pasajes", "24",
        ]
        print(f"[ENTRENANDO] Lote {lote_num} ({len(texto_bloque)} caracteres)...")
        resultado = subprocess.run(cmd, capture_output=True, text=True, timeout=3600)

        if resultado.returncode != 0:
            print(f"[ERROR] El lote {lote_num} falló:")
            print(resultado.stderr[-2000:] if len(resultado.stderr) > 2000 else resultado.stderr)
            return False

        # Mostrar última línea de salida útil
        lineas = [l for l in resultado.stdout.strip().split("\n") if l.strip()]
        if lineas:
            print(f"[SALIDA] {lineas[-1]}")
        return True

    except subprocess.TimeoutExpired:
        print(f"[ERROR] El lote {lote_num} excedió el tiempo límite (1 hora).")
        return False
    finally:
        if os.path.exists(tmp_bloque):
            os.remove(tmp_bloque)


# ── Flujo principal ───────────────────────────────────────────────────────


def main():
    token = get_token()

    # 1. Descargar cerebro y registro existentes
    hf_download(HF_MODEL_FILENAME, LOCAL_MODEL, token)
    hf_download(HF_REGISTRO_FILENAME, LOCAL_REGISTRO, token)

    reg = cargar_registro()
    articulos_vistos = set(reg.get("articulos_procesados", []))
    print(f"[INFO] Artículos ya procesados: {len(articulos_vistos)}")
    print(f"[INFO] Lotes completados: {reg.get('lotes_completados', 0)}")

    if not CEREBRO_BIN.exists():
        print("[ERROR] No se encontró el binario C++ en cortex-lm/bin/cerebro")
        print("Compílalo primero con: make portable -j$(nproc) en cortex-lm/")
        sys.exit(1)

    if not LOCAL_MODEL.exists():
        print("[ERROR] No existe el archivo del cerebro. Descárgalo primero de tu bóveda.")
        sys.exit(1)

    # 2. Bucle de entrenamiento por lotes
    lote = reg.get("lotes_completados", 0) + 1
    lotes_sesion = 0

    while lotes_sesion < MAX_LOTES_POR_SESION:
        print(f"\n=== Lote {lote} ===")

        bloque, nuevos_ids = descargar_bloque_wikipedia(articulos_vistos)

        if not bloque.strip():
            print("[INFO] No se encontraron artículos nuevos de Wikipedia. Esperando...")
            time.sleep(30)
            continue

        ok = entrenar_lote(bloque, lote)

        if ok:
            # Actualizar registro
            articulos_vistos.update(nuevos_ids)
            reg["articulos_procesados"] = sorted(list(articulos_vistos))
            reg["lotes_completados"] = lote
            reg["bytes_asimilados"] = reg.get("bytes_asimilados", 0) + len(bloque.encode("utf-8"))
            guardar_registro(reg)

            # Subir después de cada lote exitoso
            hf_upload(HF_MODEL_FILENAME, LOCAL_MODEL, token)
            hf_upload(HF_REGISTRO_FILENAME, LOCAL_REGISTRO, token)

            print(f"[GUARDADO] Progreso guardado en Hugging Face tras lote {lote}")
            lote += 1
            lotes_sesion += 1
        else:
            print("[REINTENTO] Saltando este lote y pasando al siguiente...")
            lote += 1
            lotes_sesion += 1

    print(f"\n[FIN] Sesión terminada. Lotes procesados esta vez: {lotes_sesion}")
    print(f"[FIN] Total de lotes acumulados: {reg.get('lotes_completados', 0)}")
    print("[FIN] Puedes volver a ejecutar este script cuando quieras continuar.")


if __name__ == "__main__":
    main()
