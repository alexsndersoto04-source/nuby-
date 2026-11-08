#!/usr/bin/env bash
# desplegar_nube_macro.sh — Lanza tu CORTEX-LM Macro de >1,017,856 neuronas LIF
# a un servidor gratuito de 16 GB RAM 24/7 en Hugging Face Spaces (Docker C++17),
# deteniendo cualquier proceso local en tu teléfono.
#
# Uso desde Termux:
#   ./desplegar_nube_macro.sh <TU_USUARIO_HF> <TU_TOKEN_HF> [nombre-space]

set -euo pipefail

RAIZ="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"

# 1. Liberar el teléfono si había un entrenamiento local corriendo
if [ -x "$RAIZ/entrenar_noche_termux.sh" ]; then
    "$RAIZ/entrenar_noche_termux.sh" detener >/dev/null 2>&1 || true
fi

if [ "$#" -lt 2 ]; then
    echo "======================================================================"
    echo " DESPLIEGUE EN LA NUBE A GRAN ESCALA (1,017,856 NEURONAS LIF, 16 GB RAM)"
    echo "======================================================================"
    echo "Tu teléfono quedó libre (0% CPU). Para subir el servidor Docker 24/7:"
    echo ""
    echo "  ./desplegar_nube_macro.sh TU_USUARIO_HF hf_TU_TOKEN_AQUI cortex-macro-1m"
    echo ""
    echo "¿Dónde sacas esos 2 datos gratis en 1 minuto?"
    echo "  1. Crea cuenta o entra en https://huggingface.co"
    echo "  2. Ve a https://huggingface.co/settings/tokens -> 'Create new token' -> tipo 'Write'"
    echo "  3. Pega el comando de arriba en Termux. Nuestro script creará automáticamente:"
    echo "     - Tu servidor Space Docker de 16 GB RAM 24/7 (1,017,856 neuronas LIF)"
    echo "     - Tu repositorio de respaldo automático en la nube (--continuo)"
    echo "     - Y te dará el link web directo https://<usuario>-cortex-macro-1m.hf.space"
    exit 0
fi

HF_USER="$1"
HF_TOKEN="$2"
SPACE_NAME="${3:-cortex-macro-1m}"
SPACE_ID="${HF_USER}/${SPACE_NAME}"
BACKUP_ID="${HF_USER}/${SPACE_NAME}-backup"

python3 - <<'PY' "$RAIZ" "$SPACE_ID" "$BACKUP_ID" "$HF_TOKEN"
import subprocess, sys
raiz, space_id, backup_id, token = sys.argv[1:5]
try:
    import huggingface_hub  # noqa
except ImportError:
    subprocess.check_call([sys.executable, "-m", "pip", "install", "-q", "huggingface_hub"])

from huggingface_hub import HfApi

api = HfApi(token=token)
print(f"[1/3] Creando repositorio de respaldo continuo: {backup_id} ...")
api.create_repo(repo_id=backup_id, repo_type="dataset", private=True, exist_ok=True)

print(f"[2/3] Creando servidor Docker de 16 GB RAM 24/7: {space_id} ...")
api.create_repo(repo_id=space_id, repo_type="space", space_sdk="docker", private=False, exist_ok=True)

api.add_space_secret(repo_id=space_id, key="HF_TOKEN", value=token)
api.add_space_variable(repo_id=space_id, key="HF_REPO_ID", value=backup_id)
api.add_space_variable(repo_id=space_id, key="PERFIL_CORTEX", value="macro-1m")

print("[3/3] Subiendo motor C++17 CORTEX-LM Macro a la nube...")
api.upload_folder(
    folder_path=raiz,
    repo_id=space_id,
    repo_type="space",
    ignore_patterns=[".git/*", "cortex-lm/bin/*", "*.bin", "*.log", "*.pid", "__pycache__/*"],
)
readme_hf = """---
title: CORTEX-LM Macro 1M Viva
emoji: 🧠
colorFrom: cyan
colorTo: indigo
sdk: docker
app_port: 7860
---

# CORTEX-LM Macro 1M — Corteza Viva 24/7
1,017,856 neuronas spiking LIF puras en C++17, 5 regiones corticales, 32 Macro-Columnas Talámicas (TRN), Corteza Semántica CSR, Acetilcolina (ACh), Ganglios Basales (Go/No-Go) y Sueño Cortical en aprendizaje `--continuo` 24/7.
"""
api.upload_file(
    path_or_fileobj=readme_hf.encode("utf-8"),
    path_in_repo="README.md",
    repo_id=space_id,
    repo_type="space",
)

slug = space_id.replace("/", "-").lower()
print("\n======================================================================")
print(" ¡SERVIDOR MACRO 1M DESPLEGADO EN LA NUBE 24/7!")
print(f" Panel de construcción : https://huggingface.co/spaces/{space_id}")
print(f" Enlace directo Web    : https://{slug}.hf.space")
print(f" Respaldo --continuo   : https://huggingface.co/datasets/{backup_id}")
print(" -> Ya puedes CERRAR todo y apagar la pantalla: corre 100% en 16 GB RAM de la nube.")
print("======================================================================")
PY
