#!/usr/bin/env python3
"""
ejecutar_fabrica_github_16gb.py — Orquestador Gratuito a Gran Escala (4 vCPUs, 16 GB RAM)
para GitHub Actions + Bóveda Gratuita Hugging Face Datasets + Túnel Web Público en Vivo.

Corre `cloud/motor_macro_nube_24_7.py` (`macro-1m`: 1,017,856 neuronas LIF puras, ~96.9M sinapsis)
durante hasta 5h 20m por ciclo en los servidores gratuitos de GitHub Actions, respalda el cerebro
`.bin` continuamente en tu Dataset privado de Hugging Face (`<usuario>/cortex-macro-1m-backup`,
100% gratis sin plan PRO), y publica un enlace web HTTPS (`https://...trycloudflare.com`)
en `ENLACE_VIVO.txt` para que puedas abrir el Observatorio Cortical Web desde tu celular.
"""

import os
import re
import signal
import stat
import subprocess
import sys
import time
import urllib.request
from pathlib import Path

RAIZ = Path(__file__).resolve().parent.parent
DIR_CORTEX = RAIZ / "cortex-lm"


def preparar_boveda_hf(token: str) -> str:
    if not token:
        print("[AVISO] HF_TOKEN vacío: el progreso solo vivirá durante este ciclo.", flush=True)
        return ""
    from huggingface_hub import HfApi

    api = HfApi(token=token)
    user = api.whoami()["name"]
    repo_id = os.environ.get("HF_REPO_ID", "").strip() or f"{user}/cortex-macro-1m-backup"
    api.create_repo(repo_id=repo_id, repo_type="dataset", private=True, exist_ok=True)
    print(f"[BÓVEDA HF LISTA] Repositorio gratuito de respaldo continuo: {repo_id}", flush=True)
    return repo_id


def subir_enlace_vivo(token: str, repo_id: str, url: str):
    if not token or not repo_id or not url:
        return
    try:
        from huggingface_hub import HfApi

        api = HfApi(token=token)
        contenido = (
            f"{url}\n"
            f"Actualizado UTC: {time.strftime('%Y-%m-%d %H:%M:%S', time.gmtime())}\n"
        )
        api.upload_file(
            path_or_fileobj=contenido.encode("utf-8"),
            path_in_repo="ENLACE_VIVO.txt",
            repo_id=repo_id,
            repo_type="dataset",
        )
    except Exception as exc:
        print(f"[AVISO] No se pudo subir ENLACE_VIVO.txt ({exc})", flush=True)


def iniciar_tunel_cloudflare(puerto: int, token: str, repo_id: str):
    bin_cf = Path("/tmp/cloudflared")
    if not bin_cf.exists():
        url_bin = "https://github.com/cloudflare/cloudflared/releases/latest/download/cloudflared-linux-amd64"
        print("[TÚNEL] Descargando cliente cloudflared...", flush=True)
        urllib.request.urlretrieve(url_bin, str(bin_cf))
        bin_cf.chmod(bin_cf.stat().st_mode | stat.S_IEXEC)

    proc = subprocess.Popen(
        [str(bin_cf), "tunnel", "--no-autoupdate", "--url", f"http://127.0.0.1:{puerto}"],
        stdout=subprocess.PIPE,
        stderr=subprocess.STDOUT,
        text=True,
        bufsize=1,
    )

    def lector():
        assert proc.stdout
        patron = re.compile(r"https://[a-zA-Z0-9-]+\.trycloudflare\.com")
        encontrado = False
        for ln in proc.stdout:
            m = patron.search(ln)
            if m and not encontrado:
                encontrado = True
                url = m.group(0)
                print("\n" + "=" * 72, flush=True)
                print(f" OBSERVATORIO WEB MACRO 1M EN VIVO: {url}", flush=True)
                print("=" * 72 + "\n", flush=True)
                subir_enlace_vivo(token, repo_id, url)

    import threading

    threading.Thread(target=lector, daemon=True).start()
    return proc


def respaldo_final(token: str, repo_id: str, ruta_modelo: Path):
    if not token or not repo_id or not ruta_modelo.exists():
        return
    try:
        from huggingface_hub import HfApi

        print("[CIERRE LIMPIO] Guardando respaldo final del cerebro Macro en Hugging Face Dataset...", flush=True)
        api = HfApi(token=token)
        api.upload_file(
            path_or_fileobj=str(ruta_modelo),
            path_in_repo=ruta_modelo.name,
            repo_id=repo_id,
            repo_type="dataset",
        )
        reg = ruta_modelo.with_suffix(".registro.json")
        if reg.exists():
            api.upload_file(
                path_or_fileobj=str(reg),
                path_in_repo=reg.name,
                repo_id=repo_id,
                repo_type="dataset",
            )
        print("[CIERRE LIMPIO] Respaldo completado con éxito.", flush=True)
    except Exception as exc:
        print(f"[AVISO] Error en respaldo final: {exc}", flush=True)


def main():
    token = os.environ.get("HF_TOKEN", "").strip()
    perfil = os.environ.get("PERFIL_CORTEX", "macro-1m").strip() or "macro-1m"
    max_minutos = int(os.environ.get("MAX_MINUTOS_CICLO", "320"))  # 5h 20m (< límite 6h de GitHub)
    puerto = int(os.environ.get("PORT", "7860"))
    ruta_modelo = DIR_CORTEX / "data" / "cerebro_macro_1m.bin"

    repo_id = preparar_boveda_hf(token)
    env = os.environ.copy()
    env["PORT"] = str(puerto)
    env["PERFIL_CORTEX"] = perfil
    env["RUTA_MODELO"] = str(ruta_modelo)
    if repo_id:
        env["HF_REPO_ID"] = repo_id

    subprocess.run(["make", "portable", f"-j{os.cpu_count() or 4}"], cwd=str(DIR_CORTEX), check=True)

    proc_motor = subprocess.Popen(
        [sys.executable, str(RAIZ / "cloud" / "motor_macro_nube_24_7.py")],
        cwd=str(RAIZ),
        env=env,
    )
    time.sleep(3)
    proc_cf = iniciar_tunel_cloudflare(puerto, token, repo_id)

    inicio = time.time()
    limite_seg = max_minutos * 60
    try:
        while time.time() - inicio < limite_seg:
            if proc_motor.poll() is not None:
                break
            time.sleep(15)
    finally:
        for p in (proc_cf, proc_motor):
            if p and p.poll() is None:
                p.send_signal(signal.SIGTERM)
                try:
                    p.wait(timeout=10)
                except Exception:
                    p.kill()
        respaldo_final(token, repo_id, ruta_modelo)


if __name__ == "__main__":
    main()
