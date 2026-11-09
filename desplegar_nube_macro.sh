#!/usr/bin/env bash
# desplegar_nube_macro.sh — Despliegue Guiado de 1 Solo Paso en Hugging Face Spaces (16 GB RAM 24/7)
# Solo ejecuta: ./desplegar_nube_macro.sh
# El asistente detecta tu usuario automáticamente a partir de tu token (hf_...).

set -euo pipefail

RAIZ="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"

# 1. Liberar el teléfono si había un entrenamiento local corriendo
if [ -x "$RAIZ/entrenar_noche_termux.sh" ]; then
    "$RAIZ/entrenar_noche_termux.sh" detener >/dev/null 2>&1 || true
fi

if [ "$#" -ge 2 ] && [[ "${2:-}" == hf_* ]]; then
    HF_TOKEN="$2"
    SPACE_NAME="${3:-cortex-macro-1m}"
else
    HF_TOKEN="${1:-}"
    SPACE_NAME="${2:-cortex-macro-1m}"
fi

if [ -z "$HF_TOKEN" ]; then
    echo "======================================================================"
    echo " ASISTENTE AUTOMÁTICO — CORTEZA VIVA MACRO 1M EN LA NUBE (16 GB RAM)"
    echo "======================================================================"
    echo "No necesitas escribir tu usuario: lo detectaremos automáticamente."
    echo "(Obtén tu token tipo 'Write' en https://huggingface.co/settings/tokens)"
    echo ""
    printf "Pega aquí tu token (empieza con hf_) y presiona Enter: "
    read -r HF_TOKEN
fi

HF_TOKEN="$(printf '%s' "$HF_TOKEN" | tr -d '[:space:]')"
if [ -z "$HF_TOKEN" ] || [ "$HF_TOKEN" = "hf_TU_TOKEN" ]; then
    echo "Error: debes pegar tu token real que empieza por 'hf_'."
    exit 1
fi

python3 - <<'PY' "$RAIZ" "$HF_TOKEN" "$SPACE_NAME"
import subprocess, sys
raiz, token, space_name = sys.argv[1:4]
try:
    import huggingface_hub  # noqa
except ImportError:
    subprocess.check_call([sys.executable, "-m", "pip", "install", "-q", "huggingface_hub"])

from huggingface_hub import HfApi

api = HfApi(token=token)
try:
    info = api.whoami()
    hf_user = info["name"]
except Exception as exc:
    print("\n[ERROR] El token no es válido o no tiene permisos 'Write':", exc)
    print("Asegúrate de crear un token tipo 'Write' en https://huggingface.co/settings/tokens")
    sys.exit(1)

space_id = f"{hf_user}/{space_name}"
backup_id = f"{hf_user}/{space_name}-backup"

print(f"\n[OK] Usuario detectado automáticamente: {hf_user}")
print(f"[1/3] Creando bóveda privada de respaldo continuo: {backup_id} ...")
api.create_repo(repo_id=backup_id, repo_type="dataset", private=True, exist_ok=True)

print(f"[2/3] Creando servidor Docker de 16 GB RAM 24/7: {space_id} ...")
api.create_repo(repo_id=space_id, repo_type="space", space_sdk="docker", private=False, exist_ok=True)

api.add_space_secret(repo_id=space_id, key="HF_TOKEN", value=token)
api.add_space_variable(repo_id=space_id, key="HF_REPO_ID", value=backup_id)
api.add_space_variable(repo_id=space_id, key="PERFIL_CORTEX", value="macro-1m")

print("[3/3] Subiendo motor C++17 CORTEX-LM Macro (>1,017,856 neuronas LIF) a la nube...")
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
print(" ¡LISTO! TU SERVIDOR MACRO 1M YA ESTÁ CORRIENDO EN LA NUBE 24/7")
print(f" Ver construcción / panel : https://huggingface.co/spaces/{space_id}")
print(f" Link directo de Chat Web : https://{slug}.hf.space")
print(f" Bóveda privada --continuo: https://huggingface.co/datasets/{backup_id}")
print(" -> Ya puedes CERRAR todo y apagar la pantalla: la nube de 16 GB RAM trabaja sola.")
print("======================================================================")
PY
