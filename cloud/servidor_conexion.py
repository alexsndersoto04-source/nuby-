#!/usr/bin/env python3
"""
servidor_conexion.py — Servidor ligero de conexión para Ask.
NO entrena nada. Solo carga el modelo desde Hugging Face y expone el túnel
para que la app Ask hable con tu modelo real guardado en la bóveda.
"""

import json
import os
import shutil
import subprocess
import sys
import tempfile
import threading
import time
import urllib.parse
from collections import deque
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from pathlib import Path

RAIZ_REPO = Path(__file__).resolve().parent.parent
DIR_CORTEX = RAIZ_REPO / "cortex-lm"
sys.path.insert(0, str(DIR_CORTEX / "tools"))

HTML_CONEXION = r"""<!DOCTYPE html>
<html lang="es">
<head>
<meta charset="utf-8"/><meta name="viewport" content="width=device-width, initial-scale=1"/>
<title>Ask — Servidor de Conexión</title>
<style>
  body { font-family: system-ui, -apple-system, sans-serif; background:#ffffff; color:#111; padding:24px; }
  h1 { color:#7C3AED; }
  .card { border:1px solid #EDE9FE; border-radius:12px; padding:16px; margin-top:16px; background:#F3F0FF; }
  .ok { color:#10b981; } .warn { color:#f59e0b; } .bad { color:#ef4444; }
</style>
</head>
<body>
  <h1>Ask · Servidor de Conexión</h1>
  <p>Arquitectura: <b>CORTEX-ML</b> | Modo: <b>Conexión directa</b></p>
  <div class="card">
    <p>Este servidor solo carga tu modelo desde Hugging Face y permite hablar con él desde la app <b>Ask</b>.</p>
    <p>No entrena nada. Solo conecta.</p>
  </div>
</body>
</html>"""

class ServidorConexion:
    def __init__(self):
        self.proc = None
        self.info_cerebro = {}
        self.listo = False
        self.logs = deque(maxlen=40)
        self.lock = threading.Lock()

    def log(self, msg):
        ts = time.strftime("%H:%M:%S")
        linea = f"[{ts}] {msg}"
        print(linea, flush=True)
        self.logs.append(linea)

    def cargar_modelo(self):
        """Descarga el modelo desde Hugging Face y lo pone en línea."""
        token = os.environ.get("HF_TOKEN", "").strip()
        if not token:
            self.log("ERROR: Falta HF_TOKEN")
            return False

        try:
            from huggingface_hub import hf_hub_download
        except Exception:
            subprocess.run([sys.executable, "-m", "pip", "install", "--quiet", "huggingface_hub"], check=False)
            from huggingface_hub import hf_hub_download

        ruta_modelo = DIR_CORTEX / "data" / "cerebro_macro_1m.bin"
        ruta_modelo.parent.mkdir(parents=True, exist_ok=True)

        self.log("Descargando modelo desde Hugging Face (alexsndersoto/cortex-macro-1m-backup)...")
        try:
            p_bin = hf_hub_download(
                repo_id="alexsndersoto/cortex-macro-1m-backup",
                filename="cerebro_macro_1m.bin",
                repo_type="dataset",
                token=token,
            )
            shutil.copy2(p_bin, ruta_modelo)
            self.log(f"Modelo descargado: {ruta_modelo.stat().st_size / 1024 / 1024:.1f} MB")
        except Exception as exc:
            self.log(f"Error descargando modelo: {exc}")
            return False

        # Compilar motor si no existe
        bin_cerebro = DIR_CORTEX / "bin" / "cerebro"
        if not bin_cerebro.exists():
            self.log("Compilando motor C++...")
            try:
                subprocess.run(
                    ["make", "portable", f"-j{os.cpu_count() or 2}"],
                    cwd=str(DIR_CORTEX), check=True, timeout=600
                )
            except Exception as exc:
                self.log(f"Error compilando: {exc}")
                return False

        # Lanzar tubo del cerebro
        cmd = [str(bin_cerebro), "tubo", str(ruta_modelo), "--temp", "0.08", "--honesto"]
        self.log("Iniciando cerebro en modo tubo...")
        try:
            self.proc = subprocess.Popen(
                cmd, stdin=subprocess.PIPE, stdout=subprocess.PIPE, stderr=subprocess.PIPE,
                text=True, bufsize=1,
            )
        except Exception as exc:
            self.log(f"Error iniciando tubo: {exc}")
            return False

        # Leer primera línea (info del cerebro)
        try:
            primera = self.proc.stdout.readline().strip()
            if primera:
                self.info_cerebro = json.loads(primera)
                self.listo = True
                self.log(
                    f"Cerebro EN LÍNEA: {self.info_cerebro.get('neuronas')} neuronas | "
                    f"{self.info_cerebro.get('sinapsis')} sinapsis"
                )
                return True
        except Exception as exc:
            self.log(f"Error leyendo info del cerebro: {exc}")

        return False

    def enviar_chat(self, mensaje):
        with self.lock:
            if not self.listo or self.proc is None or self.proc.poll() is not None:
                return {"respuesta": "El servidor no está listo. Intenta de nuevo en un momento.", "abrio_go": False, "certeza": 0.0}
            try:
                assert self.proc.stdin and self.proc.stdout
                self.proc.stdin.write(mensaje.replace("\r", " ").replace("\n", " ").strip() + "\n")
                self.proc.stdin.flush()
                linea = self.proc.stdout.readline().strip()
                if not linea:
                    return {"respuesta": "El cerebro no respondió. Intenta de nuevo.", "abrio_go": False, "certeza": 0.0}
                return json.loads(linea)
            except Exception as exc:
                return {"respuesta": f"Error comunicando con el cerebro: {exc}", "abrio_go": False, "certeza": 0.0}

    def estado(self):
        return {
            "listo": self.listo,
            "nombre": "Ask",
            "arquitectura": "CORTEX-ML",
            "neuronas": self.info_cerebro.get("neuronas", 0),
            "sinapsis": self.info_cerebro.get("sinapsis", 0),
            "modo": "conexion_solo",
        }

gestor = ServidorConexion()

def crear_handler():
    class Handler(BaseHTTPRequestHandler):
        def _cors(self):
            self.send_header("Access-Control-Allow-Origin", "*")
            self.send_header("Access-Control-Allow-Headers", "Content-Type, Authorization")
            self.send_header("Access-Control-Allow-Methods", "GET, POST, OPTIONS")

        def do_OPTIONS(self):
            self.send_response(204)
            self._cors()
            self.end_headers()

        def do_GET(self):
            if self.path == "/" or self.path == "":
                body = HTML_CONEXION.encode("utf-8")
                self.send_response(200)
                self.send_header("Content-Type", "text/html; charset=utf-8")
                self._cors()
                self.end_headers()
                self.wfile.write(body)
                return
            if self.path.startswith("/api/estado"):
                est = gestor.estado()
                body = json.dumps(est, ensure_ascii=False).encode("utf-8")
                self.send_response(200)
                self.send_header("Content-Type", "application/json; charset=utf-8")
                self._cors()
                self.end_headers()
                self.wfile.write(body)
                return
            self.send_response(404)
            self._cors()
            self.end_headers()

        def do_POST(self):
            n = int(self.headers.get("Content-Length", "0"))
            raw = self.rfile.read(n).decode("utf-8", errors="replace")
            try:
                payload = json.loads(raw) if raw else {}
            except Exception:
                payload = {}

            if self.path.startswith("/api/chat"):
                msg = str(payload.get("mensaje", "")).strip()
                resp = gestor.enviar_chat(msg) if msg else {"respuesta": ""}
                body = json.dumps(resp, ensure_ascii=False).encode("utf-8")
                self.send_response(200)
                self.send_header("Content-Type", "application/json; charset=utf-8")
                self._cors()
                self.end_headers()
                self.wfile.write(body)
                return

            self.send_response(404)
            self._cors()
            self.end_headers()

        def log_message(self, fmt, *args):
            return

    return Handler

def main():
    puerto = int(os.environ.get("PORT", "7860"))
    gestor.cargar_modelo()

    srv = ThreadingHTTPServer(("0.0.0.0", puerto), crear_handler())
    gestor.log(f"Servidor de conexión Ask en 0.0.0.0:{puerto}")
    srv.serve_forever()

if __name__ == "__main__":
    main()
