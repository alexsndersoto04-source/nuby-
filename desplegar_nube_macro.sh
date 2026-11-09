#!/usr/bin/env bash
# desplegar_nube_macro.sh — Visor de Enlace Vivo y Estado de Bóveda Gratuita (16 GB RAM GitHub Actions + HF Dataset)
# Uso:
#   ./desplegar_nube_macro.sh          # Consultar tu enlace web vivo actual (https://...trycloudflare.com)

set -euo pipefail

RAIZ="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"

if [ -x "$RAIZ/entrenar_noche_termux.sh" ]; then
    "$RAIZ/entrenar_noche_termux.sh" detener >/dev/null 2>&1 || true
fi

HF_TOKEN="${1:-}"
if [ -z "$HF_TOKEN" ]; then
    printf "Pega tu token nuevo de Hugging Face (hf_...) para consultar tu enlace web vivo: "
    read -r HF_TOKEN
fi
HF_TOKEN="$(printf '%s' "$HF_TOKEN" | tr -d '[:space:]')"

python3 - <<'PY' "$HF_TOKEN"
import sys
from huggingface_hub import HfApi, hf_hub_download

token = sys.argv[1]
api = HfApi(token=token)
user = api.whoami()["name"]
repo_id = f"{user}/cortex-macro-1m-backup"
print(f"\nBóveda conectada: https://huggingface.co/datasets/{repo_id}")
try:
    p = hf_hub_download(repo_id=repo_id, filename="ENLACE_VIVO.txt", repo_type="dataset", token=token, force_download=True)
    print("\n======================================================================")
    print(" TU ENLACE WEB EN VIVO (16 GB RAM, 1,017,856 NEURONAS LIF):")
    print(open(p, encoding="utf-8").read().strip())
    print("======================================================================")
except Exception:
    print("Aún no se ha publicado ENLACE_VIVO.txt (espera ~2 minutos tras pulsar 'Run workflow' en GitHub Actions).")
PY
