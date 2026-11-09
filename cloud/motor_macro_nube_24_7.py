#!/usr/bin/env python3
"""
motor_macro_nube_24_7.py — Servidor + Fábrica Continua 24/7 en la Nube a Gran Escala
para CORTEX-LM Macro («Corteza Viva»: 1,017,856 neuronas LIF puras y ~96.9M sinapsis CSR).

Diseñado para servidores en la nube de 16 GB RAM (Hugging Face Spaces Docker gratuito 24/7,
Railway, Koyeb, Codespaces o VPS), SIN usar recursos de tu teléfono y SIN depender de tener
ninguna pestaña del navegador abierta.

Cómo opera:
  1. Levanta de inmediato el Observatorio Cortical Web en 0.0.0.0:$PORT (7860 por defecto)
     mostrando estado en vivo, millones de caracteres asimilados y consola de conversación.
  2. Si defines HF_TOKEN y HF_REPO_ID, restaura automáticamente el último cerebro .bin
     desde tu repositorio de Hugging Face y respalda cada nuevo lote continuo en la nube.
  3. Crea el cerebro Macro de 5 regiones + 32 Macro-Columnas TRN + Lector FP16 (`macro-1m`:
     1,017,856 neuronas LIF puras; o `macro-526k`), lo pone EN LÍNEA para chatear al instante,
     y en segundo plano sigue alimentándolo 24/7 en modo `--continuo --semantica` con libros
     clásicos + cientos de lotes enciclopédicos de Wikipedia ES + Sueño Cortical (`sonar`),
     haciendo recarga atómica en caliente sin interrumpir tus conversaciones.
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
import urllib.request
from collections import deque
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from pathlib import Path

RAIZ_REPO = Path(__file__).resolve().parent.parent
DIR_CORTEX = RAIZ_REPO / "cortex-lm"
sys.path.insert(0, str(DIR_CORTEX / "tools"))

from entrenar_macro_colab import (  # noqa: E402
    CONOCIMIENTO_FUNDACIONAL_MULTIDOMINIO,
    PERFILES,
    cargar_registro,
     guardar_registro,
    iterar_lotes_wikipedia_streaming,
)

HTML_NUBE = r"""<!DOCTYPE html>
<html lang="es">
<head>
<meta charset="utf-8"/>
<meta name="viewport" content="width=device-width, initial-scale=1"/>
<title>CORTEX-LM Macro 1M — Nube Viva 24/7</title>
<style>
  :root {
    --bg: #070b14; --panel: #111827; --border: #1f2937;
    --text: #e5e7eb; --muted: #9ca3af; --cyan: #22d3ee;
    --emerald: #10b981; --amber: #f59e0b; --violet: #a78bfa;
  }
  * { box-sizing: border-box; }
  body {
    margin: 0; font-family: system-ui, -apple-system, sans-serif;
    background: var(--bg); color: var(--text);
    display: grid; grid-template-columns: 1.5fr 1fr; min-height: 100vh;
  }
  @media (max-width: 920px) { body { grid-template-columns: 1fr; } }
  .col-chat { display: flex; flex-direction: column; border-right: 1px solid var(--border); height: 100vh; }
  header {
    padding: 14px 18px; border-bottom: 1px solid var(--border);
    display: flex; justify-content: space-between; align-items: center; background: #0b1120; gap: 8px; flex-wrap: wrap;
  }
  header h1 { margin: 0; font-size: 1.02rem; color: var(--cyan); }
  .badge {
    font-size: 0.75rem; padding: 4px 10px; border-radius: 999px;
    background: rgba(34,211,238,0.12); color: var(--cyan); border: 1px solid rgba(34,211,238,0.3);
  }
  #messages { flex: 1; overflow-y: auto; padding: 16px; display: flex; flex-direction: column; gap: 12px; }
  .msg { max-width: 88%; padding: 12px 14px; border-radius: 12px; line-height: 1.48; font-size: 0.94rem; }
  .msg.user { align-self: flex-end; background: #1d4ed8; color: #fff; }
  .msg.bot { align-self: flex-start; background: var(--panel); border: 1px solid var(--border); }
  .thought {
    font-family: ui-monospace, monospace; font-size: 0.78rem; color: var(--violet);
    background: rgba(167,139,250,0.08); border-left: 2px solid var(--violet);
    padding: 6px 9px; margin-bottom: 8px; border-radius: 4px;
  }
  .meta-tag { font-size: 0.74rem; color: var(--emerald); margin-bottom: 5px; }
  .nogo-tag { font-size: 0.74rem; color: var(--amber); margin-bottom: 5px; }
  .composer { padding: 12px; border-top: 1px solid var(--border); display: flex; gap: 8px; background: #0b1120; }
  input[type="text"], textarea {
    flex: 1; background: var(--panel); color: var(--text); border: 1px solid var(--border);
    border-radius: 8px; padding: 10px 12px; font-size: 0.94rem; outline: none;
  }
  button {
    background: var(--cyan); color: #06121d; font-weight: 600; border: none;
    border-radius: 8px; padding: 10px 14px; cursor: pointer; font-size: 0.86rem;
  }
  button.sec { background: #1f2937; color: var(--text); border: 1px solid #374151; }
  .col-obs { padding: 16px; overflow-y: auto; display: flex; flex-direction: column; gap: 14px; max-height: 100vh; }
  .card { background: var(--panel); border: 1px solid var(--border); border-radius: 12px; padding: 14px; }
  .card h2 { margin: 0 0 9px 0; font-size: 0.82rem; text-transform: uppercase; color: var(--muted); letter-spacing: 0.05em; }
  .stat-grid { display: grid; grid-template-columns: 1fr 1fr; gap: 8px; font-size: 0.84rem; }
  .stat-box { background: #0b1120; border: 1px solid var(--border); border-radius: 8px; padding: 8px 10px; }
  .stat-box b { display: block; font-size: 1rem; color: var(--cyan); margin-top: 2px; }
  .meter-row { display: flex; justify-content: space-between; font-size: 0.83rem; margin-bottom: 4px; }
  .bar-bg { height: 8px; background: #1f2937; border-radius: 99px; overflow: hidden; margin-bottom: 10px; }
  .bar-fill { height: 100%; transition: width 0.25s ease; }
  .cols-grid { display: flex; flex-wrap: wrap; gap: 6px; }
  .col-pill {
    font-family: ui-monospace, monospace; font-size: 0.77rem; padding: 4px 8px;
    border-radius: 6px; background: rgba(16,185,129,0.13); border: 1px solid rgba(16,185,129,0.35); color: var(--emerald);
  }
  pre#logBox {
    margin: 0; font-size: 0.74rem; max-height: 170px; overflow-y: auto;
    background: #070b14; padding: 9px; border-radius: 8px; border: 1px solid var(--border); color: #93c5fd;
    white-space: pre-wrap;
  }
</style>
</head>
<body>
  <section class="col-chat">
    <header>
      <h1>CORTEX-LM Macro — Nube Viva 24/7</h1>
      <span class="badge" id="statsBadge">Conectando con servidor cortical...</span>
    </header>
    <div id="messages">
      <div class="msg bot">
        Servidor en la nube activo 24/7 (puedes cerrar el navegador o apagar tu teléfono en cualquier momento:
        el cerebro sigue entrenando y consolidando solo en la nube).
      </div>
    </div>
    <div class="composer">
      <input id="chatInput" type="text" placeholder="Escribe tu pregunta o enséñale un hecho nuevo..." onkeydown="if(event.key==='Enter')enviar()"/>
      <button onclick="enviar()">Enviar</button>
      <button class="sec" onclick="comando('/sonar')">Soñar</button>
      <button class="sec" onclick="comando('/nuevo')">Nuevo hilo</button>
    </div>
  </section>

  <aside class="col-obs">
    <div class="card">
      <h2>Fábrica Continua en la Nube (--continuo 24/7)</h2>
      <div class="stat-grid">
        <div class="stat-box">Perfil Macro<b id="stPerfil">—</b></div>
        <div class="stat-box">Estado Fábrica<b id="stFase">—</b></div>
        <div class="stat-box">Lotes Asimilados<b id="stLotes">0</b></div>
        <div class="stat-box">Lectura Acumulada<b id="stMB">0.00 MB</b></div>
      </div>
      <div style="margin-top:10px">
        <pre id="logBox">Iniciando registro en vivo...</pre>
      </div>
    </div>

    <div class="card">
      <h2>Neuromodulación y Ganglios Basales</h2>
      <div class="meter-row"><span>Acetilcolina (ACh — Sorpresa)</span><span id="achVal">0.00</span></div>
      <div class="bar-bg"><div id="achBar" class="bar-fill" style="width:0%;background:#a78bfa"></div></div>
      <div class="meter-row"><span>Certeza Cortical (Go / No-Go)</span><span id="certVal">0.00</span></div>
      <div class="bar-bg"><div id="certBar" class="bar-fill" style="width:0%;background:#10b981"></div></div>
      <div style="font-size:0.8rem;color:var(--muted)" id="memStats">Hipocampo CA3: 0 | Corteza Semántica CSR: 0</div>
    </div>

    <div class="card">
      <h2>Macro-Columnas Talámicas Activas (TRN)</h2>
      <div id="colsGrid" class="cols-grid"><span style="font-size:0.82rem;color:var(--muted)">Esperando estímulo...</span></div>
    </div>

    <div class="card">
      <h2>Inyectar Conocimiento Directo (Leer = Saber)</h2>
      <textarea id="docInput" rows="3" placeholder="Pega cualquier párrafo técnico o científico para indexarlo al instante en CortezaSemanticaCSR..." style="width:100%;margin-bottom:8px"></textarea>
      <button onclick="asimilarDoc()" style="width:100%">Asimilar en Corteza Semántica</button>
    </div>
  </aside>

<script>
async function refrescarEstado() {
  try {
    const r = await fetch('/api/estado');
    const d = await r.json();
    document.getElementById('stPerfil').textContent = d.perfil || '—';
    document.getElementById('stFase').textContent = d.fase_actual || '—';
    document.getElementById('stLotes').textContent = d.lotes_completados || 0;
    document.getElementById('stMB').textContent = ((d.bytes_asimilados || 0) / 1048576).toFixed(2) + ' MB';
    document.getElementById('logBox').textContent = (d.ultimos_logs || []).join('\n');
    if (d.listo_chat) {
      document.getElementById('statsBadge').textContent =
        `${Number(d.neuronas||0).toLocaleString()} neuronas LIF | ${Number(d.sinapsis||0).toLocaleString()} sinapsis | EN LÍNEA`;
      document.getElementById('memStats').textContent =
        `Hipocampo CA3: ${d.ca3 || 0} | Corteza Semántica CSR: ${d.semantica || 0}`;
    } else {
      document.getElementById('statsBadge').textContent = `Forjando cerebro Macro inicial (${d.fase_actual})...`;
    }
  } catch (e) {}
}

function addMsg(role, text, meta) {
  const box = document.getElementById('messages');
  const div = document.createElement('div');
  div.className = 'msg ' + role;
  if (meta) {
    if (meta.aberturas === false) {
      const ng = document.createElement('div');
      ng.className = 'nogo-tag';
      ng.textContent = `[No-Go Honesto | Certeza ${(meta.certeza*100).toFixed(0)}%]`;
      div.appendChild(ng);
    } else if (meta.aprendio_vivo) {
      const mt = document.createElement('div');
      mt.className = 'meta-tag';
      mt.textContent = `[ACh ${(meta.ach*100).toFixed(0)}% -> Consolidado autónomamente en CA3]`;
      div.appendChild(mt);
    }
    if (meta.pensamiento) {
      const th = document.createElement('div');
      th.className = 'thought';
      th.textContent = 'Pensamiento LIF: «' + meta.pensamiento + '»';
      div.appendChild(th);
    }
  }
  const p = document.createElement('div');
  p.textContent = text;
  div.appendChild(p);
  box.appendChild(div);
  box.scrollTop = box.scrollHeight;
}

function updateObs(d) {
  if (typeof d.ach === 'number') {
    document.getElementById('achVal').textContent = d.ach.toFixed(2);
    document.getElementById('achBar').style.width = Math.min(100, d.ach * 100) + '%';
  }
  if (typeof d.certeza === 'number') {
    document.getElementById('certVal').textContent = d.certeza.toFixed(2);
    const bar = document.getElementById('certBar');
    bar.style.width = Math.min(100, d.certeza * 100) + '%';
    bar.style.background = d.abrio_go === false ? '#f59e0b' : '#10b981';
  }
  if (Array.isArray(d.columnas_trn) && d.columnas_trn.length) {
    const g = document.getElementById('colsGrid');
    g.innerHTML = '';
    d.columnas_trn.forEach(c => {
      const s = document.createElement('span');
      s.className = 'col-pill';
      s.textContent = `Col #${c[0]} (${c[1].toFixed(2)}x)`;
      g.appendChild(s);
    });
  }
  refrescarEstado();
}

async function enviar() {
  const inp = document.getElementById('chatInput');
  const txt = inp.value.trim();
  if (!txt) return;
  inp.value = '';
  addMsg('user', txt);
  const r = await fetch('/api/chat', {
    method: 'POST', headers: {'Content-Type': 'application/json'},
    body: JSON.stringify({mensaje: txt})
  });
  const d = await r.json();
  updateObs(d);
  addMsg('bot', d.respuesta || '(sin salida)', {
    pensamiento: d.pensamiento, ach: d.ach, certeza: d.certeza,
    aberturas: d.abrio_go, aprendio_vivo: d.aprendio_vivo
  });
}

async function comando(cmd) {
  const r = await fetch('/api/chat', {
    method: 'POST', headers: {'Content-Type': 'application/json'},
    body: JSON.stringify({mensaje: cmd})
  });
  const d = await r.json();
  addMsg('bot', `[${cmd}] ` + (d.evento || d.respuesta || 'OK'));
  refrescarEstado();
}

async function asimilarDoc() {
  const box = document.getElementById('docInput');
  const txt = box.value.trim().replace(/\s+/g, ' ');
  if (!txt) return;
  box.value = '';
  addMsg('user', '[Asimilar conocimiento]: ' + txt);
  const r = await fetch('/api/chat', {
    method: 'POST', headers: {'Content-Type': 'application/json'},
    body: JSON.stringify({mensaje: '/asimilar ' + txt})
  });
  const d = await r.json();
  addMsg('bot', d.respuesta || 'Conocimiento asimilado.');
  refrescarEstado();
}

refrescarEstado();
setInterval(refrescarEstado, 5000);
</script>
</body>
</html>
"""


class GestorMacroNube:
    def __init__(self, bin_cerebro: Path, ruta_modelo: Path, perfil: str):
        self.bin_cerebro = str(bin_cerebro)
        self.ruta_modelo = ruta_modelo
        self.perfil = perfil
        self.lock = threading.Lock()
        self.proc = None
        self.info_cerebro = {}
        self.fase_actual = "Arrancando"
        self.lotes_completados = 0
        self.bytes_asimilados = 0
        self.logs = deque(maxlen=60)

    def log(self, msg: str):
        ts = time.strftime("%H:%M:%S")
        linea = f"[{ts}] {msg}"
        print(linea, flush=True)
        self.logs.append(linea)

    def recargar_tubo(self):
        with self.lock:
            if self.proc is not None:
                try:
                    self.proc.stdin.write("/salir\n")
                    self.proc.stdin.flush()
                    self.proc.wait(timeout=4)
                except Exception:
                    try:
                        self.proc.kill()
                    except Exception:
                        pass
                self.proc = None

            if not self.ruta_modelo.exists():
                return

            cmd = [self.bin_cerebro, "tubo", str(self.ruta_modelo), "--temp", "0.08", "--honesto"]
            self.proc = subprocess.Popen(
                cmd, stdin=subprocess.PIPE, stdout=subprocess.PIPE, stderr=subprocess.PIPE,
                text=True, bufsize=1,
            )
            first = self.proc.stdout.readline().strip() if self.proc.stdout else ""
            if first:
                self.info_cerebro = json.loads(first)
                self.log(
                    f"Cerebro EN LÍNEA: {self.info_cerebro.get('neuronas')} neuronas LIF | "
                    f"{self.info_cerebro.get('sinapsis')} sinapsis CSR | semántica={self.info_cerebro.get('semantica')}"
                )

    def enviar_chat(self, mensaje: str) -> dict:
        limpio = mensaje.replace("\r", " ").replace("\n", " ").strip()
        with self.lock:
            if self.proc is None or self.proc.poll() is not None:
                return {
                    "respuesta": (
                        f"El cerebro Macro ({self.perfil}) se está forjando inicialmente en la nube "
                        f"({self.fase_actual}). En unos minutos quedará en línea 24/7 y podrás hablarle "
                        "mientras sigue aprendiendo en segundo plano."
                    ),
                    "abrio_go": False,
                    "certeza": 0.0,
                }
            assert self.proc.stdin and self.proc.stdout
            self.proc.stdin.write(limpio + "\n")
            self.proc.stdin.flush()
            linea = self.proc.stdout.readline().strip()
            if not linea:
                return {"respuesta": "Recargando nuevo lote cortical en RAM, intenta en 2 segundos."}
            datos = json.loads(linea)
            if "ca3" in datos:
                self.info_cerebro["ca3"] = datos["ca3"]
            if "semantica" in datos:
                self.info_cerebro["semantica"] = datos["semantica"]
            return datos

    def estado_json(self) -> dict:
        return {
            "listo_chat": self.proc is not None and self.proc.poll() is None,
            "perfil": self.perfil,
            "fase_actual": self.fase_actual,
            "lotes_completados": self.lotes_completados,
            "bytes_asimilados": self.bytes_asimilados,
            "neuronas": self.info_cerebro.get("neuronas", 0),
            "sinapsis": self.info_cerebro.get("sinapsis", 0),
            "regiones": self.info_cerebro.get("regiones", 5),
            "ca3": self.info_cerebro.get("ca3", 0),
            "semantica": self.info_cerebro.get("semantica", 0),
            "ultimos_logs": list(self.logs),
        }


def intentar_restaurar_hf(gestor: GestorMacroNube, ruta_modelo: Path, ruta_reg: Path):
    token = os.environ.get("HF_TOKEN", "").strip()
    repo_id = os.environ.get("HF_REPO_ID", "").strip()
    if not token or not repo_id or ruta_modelo.exists():
        return
    try:
        from huggingface_hub import hf_hub_download  # type: ignore

        gestor.log(f"Buscando respaldo previo en Hugging Face Hub ({repo_id})...")
        p_bin = hf_hub_download(repo_id=repo_id, filename=ruta_modelo.name, repo_type="dataset", token=token)
        shutil.copy2(p_bin, ruta_modelo)
        try:
            p_reg = hf_hub_download(repo_id=repo_id, filename=ruta_reg.name, repo_type="dataset", token=token)
            shutil.copy2(p_reg, ruta_reg)
        except Exception:
            pass
        gestor.log("Respaldo Macro restaurado con éxito; retomando aprendizaje --continuo.")
    except Exception as exc:
        gestor.log(f"Sin respaldo previo en HF Hub ({exc}); se creará cerebro Macro desde cero.")


def intentar_subir_hf(gestor: GestorMacroNube, ruta_modelo: Path, ruta_reg: Path):
    token = os.environ.get("HF_TOKEN", "").strip()
    repo_id = os.environ.get("HF_REPO_ID", "").strip()
    if not token or not repo_id or not ruta_modelo.exists():
        return
    try:
        from huggingface_hub import HfApi  # type: ignore

        api = HfApi(token=token)
        api.create_repo(repo_id=repo_id, repo_type="dataset", exist_ok=True)
        api.upload_file(
            path_or_fileobj=str(ruta_modelo),
            path_in_repo=ruta_modelo.name,
            repo_id=repo_id,
            repo_type="dataset",
        )
        if ruta_reg.exists():
            api.upload_file(
                path_or_fileobj=str(ruta_reg),
                path_in_repo=ruta_reg.name,
                repo_id=repo_id,
                repo_type="dataset",
            )
        gestor.log(f"Checkpoint respaldado en la nube ({repo_id}).")
    except Exception as exc:
        gestor.log(f"Aviso respaldo HF ({exc}).")


def ejecutar_cmd(gestor: GestorMacroNube, cmd: list[str], cwd: Path):
    proc = subprocess.Popen(
        cmd, cwd=str(cwd), stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True, bufsize=1
    )
    assert proc.stdout
    for ln in proc.stdout:
        s = ln.strip()
        if s:
            gestor.log(s)
    rc = proc.wait()
    if rc != 0:
        raise RuntimeError(f"Comando falló ({rc}): {' '.join(cmd)}")


def hilo_fabrica_continua(gestor: GestorMacroNube):
    ruta_modelo = gestor.ruta_modelo
    ruta_modelo.parent.mkdir(parents=True, exist_ok=True)
    ruta_reg = ruta_modelo.with_suffix(".registro.json")

    intentar_restaurar_hf(gestor, ruta_modelo, ruta_reg)
    reg = cargar_registro(ruta_reg)
    gestor.lotes_completados = len(reg.get("fases_completadas", []))
    gestor.bytes_asimilados = int(reg.get("bytes_asimilados", 0))

    ejecutar_cmd(gestor, [sys.executable, "tools/generar_alineacion.py"], DIR_CORTEX)
    if not (DIR_CORTEX / "data" / "corpus.txt").exists():
        ejecutar_cmd(gestor, [sys.executable, "build_corpus.py"], DIR_CORTEX / "data")

    ruta_ali = str(DIR_CORTEX / "data" / "alineacion_es.txt")
    texto_arranque = (
        (DIR_CORTEX / "data" / "conocimiento_es.txt").read_text(encoding="utf-8")
        + "\n\n"
        + CONOCIMIENTO_FUNDACIONAL_MULTIDOMINIO
        + "\n\n"
        + (DIR_CORTEX / "data" / "corpus.txt").read_text(encoding="utf-8")
    )

    capas_csv, grado, ncols, topk = PERFILES.get(gestor.perfil, PERFILES["macro-1m"])
    bytes_lote = int(os.environ.get("BYTES_LOTE", "240000"))
    lotes_libros = int(os.environ.get("LOTES_LIBROS", "12"))
    lotes_wiki = int(os.environ.get("LOTES_WIKI", "250"))

    # 1. Forja inicial si no existe
    if not ruta_modelo.exists():
        gestor.fase_actual = f"Creando {gestor.perfil}"
        with tempfile.NamedTemporaryFile("w", suffix=".txt", encoding="utf-8", delete=False) as tf:
            tf.write(texto_arranque)
            tmp_init = tf.name
        try:
            ejecutar_cmd(
                gestor,
                [
                    gestor.bin_cerebro, "entrenar", str(ruta_modelo), tmp_init,
                    "--capas", capas_csv, "--grado", str(grado),
                    "--columnas", str(ncols), "--top-columnas", str(topk),
                    "--macro", "--epocas", "3", "--lr", "0.45", "--stdp", "1", "--holdout", "0.05",
                ],
                DIR_CORTEX,
            )
            gestor.fase_actual = "Alineando voz deliberativa"
            ejecutar_cmd(
                gestor,
                [gestor.bin_cerebro, "alinear", str(ruta_modelo), ruta_ali, "--epocas", "22", "--lr", "0.40", "--ancla", "0.01"],
                DIR_CORTEX,
            )
            ejecutar_cmd(
                gestor,
                [gestor.bin_cerebro, "asimilar", str(ruta_modelo), tmp_init, "--max-pasajes", "50", "--ciclos", "18"],
                DIR_CORTEX,
            )
            reg["fases_completadas"].append("arranque_macro")
            reg["bytes_asimilados"] = reg.get("bytes_asimilados", 0) + len(texto_arranque.encode("utf-8"))
            guardar_registro(ruta_reg, reg)
            gestor.lotes_completados = len(reg["fases_completadas"])
            gestor.bytes_asimilados = reg["bytes_asimilados"]
            intentar_subir_hf(gestor, ruta_modelo, ruta_reg)
        finally:
            if os.path.exists(tmp_init):
                os.remove(tmp_init)

    # Poner el cerebro EN LÍNEA inmediatamente mientras el aprendizaje --continuo corre en copia sombra
    gestor.recargar_tubo()

    def alimentar_en_sombra(archivo_lote: str, con_semantica: bool, id_fase: str, n_bytes: int):
        sombra = ruta_modelo.with_suffix(".sombra.bin")
        if sombra.exists():
            try:
                sombra.unlink()
            except Exception:
                pass
        try:
            shutil.copy2(ruta_modelo, sombra)
        except Exception as exc:
            gestor.log(f"Sin espacio en disco para sombra ({exc}); saltando lote {id_fase}.")
            return
        cmd = [
            gestor.bin_cerebro, "alimentar", str(sombra), archivo_lote,
            "--bloque", str(bytes_lote), "--epocas", "1", "--lr", "0.24", "--ancla", "0.022",
        ]
        if con_semantica:
            cmd += ["--semantica", "--max-pasajes", "24"]
        try:
            ejecutar_cmd(gestor, cmd, DIR_CORTEX)
        except Exception as exc:
            gestor.log(f"Lote {id_fase} falló ({exc}); se descarta y continúa con el siguiente.")
            if sombra.exists():
                try:
                    sombra.unlink()
                except Exception:
                    pass
            return
        if gestor.lotes_completados > 0 and gestor.lotes_completados % 6 == 0:
            try:
                ejecutar_cmd(gestor, [gestor.bin_cerebro, "sonar", str(sombra), "--ciclos", "16"], DIR_CORTEX)
            except Exception as exc:
                gestor.log(f"Sueño parcial omitido ({exc}).")
        try:
            os.replace(sombra, ruta_modelo)
        except Exception as exc:
            gestor.log(f"No se pudo aplicar sombra ({exc}); se conserva el modelo anterior.")
            if sombra.exists():
                try:
                    sombra.unlink()
                except Exception:
                    pass
            return
        reg["fases_completadas"].append(id_fase)
        reg["bytes_asimilados"] = int(reg.get("bytes_asimilados", 0)) + n_bytes
        guardar_registro(ruta_reg, reg)
        gestor.lotes_completados = len(reg["fases_completadas"])
        gestor.bytes_asimilados = reg["bytes_asimilados"]
        gestor.recargar_tubo()
        if gestor.lotes_completados % 4 == 0:
            intentar_subir_hf(gestor, ruta_modelo, ruta_reg)

    # 2. Clásicos públicos en español
    if lotes_libros > 0 and "libros_gitenberg_completos" not in reg["fases_completadas"]:
        with tempfile.TemporaryDirectory() as tmpdir:
            tmp_corpus = os.path.join(tmpdir, "clasicos.txt")
            try:
                gestor.fase_actual = "Descargando clásicos ES"
                ejecutar_cmd(
                    gestor,
                    [sys.executable, "tools/descargar_corpus.py", "--salida", tmp_corpus, "--max-mb", "4.0"],
                    DIR_CORTEX,
                )
                texto_lib = Path(tmp_corpus).read_text(encoding="utf-8", errors="ignore")
                os.remove(tmp_corpus)
                tramos = [texto_lib[i : i + bytes_lote] for i in range(0, len(texto_lib), bytes_lote)][:lotes_libros]
                for idx, tramo in enumerate(tramos, 1):
                    id_f = f"libro_{idx:03d}"
                    if id_f in reg["fases_completadas"]:
                        continue
                    gestor.fase_actual = f"Libro continuo {idx}/{len(tramos)}"
                    lp = os.path.join(tmpdir, f"{id_f}.txt")
                    Path(lp).write_text(tramo, encoding="utf-8")
                    alimentar_en_sombra(lp, False, id_f, len(tramo.encode("utf-8")))
                    os.remove(lp)
                reg["fases_completadas"].append("libros_gitenberg_completos")
                guardar_registro(ruta_reg, reg)
            except Exception as exc:
                gestor.log(f"Aviso libros ({exc}); continuando con Wikipedia ES.")

    # 3. Flujo continuo enciclopédico 24/7 (Wikipedia ES)
    vistos = set(reg.get("articulos_wiki", []))
    while True:
        procesados_ciclo = 0
        for nombre_lote, contenido in iterar_lotes_wikipedia_streaming(vistos, lotes_wiki, bytes_lote):
            id_unico = f"{nombre_lote}_{len(reg['fases_completadas']):04d}"
            gestor.fase_actual = f"Wikipedia ES ({id_unico})"
            with tempfile.NamedTemporaryFile("w", suffix=".txt", encoding="utf-8", delete=False) as tf:
                tf.write(contenido)
                tmp_w = tf.name
            try:
                alimentar_en_sombra(tmp_w, True, id_unico, len(contenido.encode("utf-8")))
                reg["articulos_wiki"] = sorted(list(vistos))[-5000:]
                guardar_registro(ruta_reg, reg)
                procesados_ciclo += 1
            finally:
                if os.path.exists(tmp_w):
                    os.remove(tmp_w)
        gestor.fase_actual = "Sueño Cortical Profundo"
        sombra = ruta_modelo.with_suffix(".sombra.bin")
        shutil.copy2(ruta_modelo, sombra)
        ejecutar_cmd(gestor, [gestor.bin_cerebro, "sonar", str(sombra), "--ciclos", "28"], DIR_CORTEX)
        os.replace(sombra, ruta_modelo)
        gestor.recargar_tubo()
        intentar_subir_hf(gestor, ruta_modelo, ruta_reg)
        if procesados_ciclo == 0:
            gestor.fase_actual = "En línea 24/7 (Consolidado)"
            time.sleep(120)


def sintetizar_voz_humana_mp3(texto: str, genero: str) -> bytes:
    """
    Sintetiza voz neuronal fluida, cálida y humana en español (24 kHz MP3)
    usando voces prosódicas neuronales (femenina cálida / masculina cálida).
    """
    import asyncio
    import edge_tts  # type: ignore

    limpio = " ".join(texto.replace("~", " ").replace("«", " ").replace("»", " ").split()).strip()
    if not limpio:
        return b""

    voces_fem = ["es-VE-PaolaNeural", "es-CO-SalomeNeural", "es-MX-DaliaNeural"]
    voces_masc = ["es-CO-GonzaloNeural", "es-AR-TomasNeural", "es-MX-JorgeNeural"]
    candidatas = voces_masc if str(genero).lower().startswith("m") else voces_fem

    async def _gen(voz_id: str) -> bytes:
        com = edge_tts.Communicate(limpio, voice=voz_id, rate="0%", pitch="+5Hz", volume="+20%")
        audio = bytearray()
        async for chunk in com.stream():
            if chunk.get("type") == "audio" and chunk.get("data"):
                audio.extend(chunk["data"])
        return bytes(audio)

    ultimo_err = None
    for v in candidatas:
        try:
            datos = asyncio.run(_gen(v))
            if datos:
                return datos
        except Exception as exc:
            ultimo_err = exc
            continue
    raise RuntimeError(f"No se pudo sintetizar audio neuronal ({ultimo_err})")


def crear_handler(gestor: GestorMacroNube):
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
            if self.path.startswith("/api/estado"):
                est = gestor.estado_json()
                est["nombre"] = "Ask"
                est["arquitectura"] = "CORTEX-ML"
                body = json.dumps(est, ensure_ascii=False).encode("utf-8")
                self.send_response(200)
                self.send_header("Content-Type", "application/json; charset=utf-8")
                self._cors()
                self.end_headers()
                self.wfile.write(body)
                return
            body = HTML_NUBE.encode("utf-8")
            self.send_response(200)
            self.send_header("Content-Type", "text/html; charset=utf-8")
            self._cors()
            self.end_headers()
            self.wfile.write(body)

        def do_POST(self):
            n = int(self.headers.get("Content-Length", "0"))
            raw = self.rfile.read(n).decode("utf-8", errors="replace")
            payload = json.loads(raw) if raw else {}

            if self.path.startswith("/api/voz"):
                texto = str(payload.get("texto", "")).strip()
                voz = str(payload.get("voz", "femenina")).strip()
                try:
                    mp3 = sintetizar_voz_humana_mp3(texto, voz)
                    self.send_response(200)
                    self.send_header("Content-Type", "audio/mpeg")
                    self.send_header("Content-Length", str(len(mp3)))
                    self._cors()
                    self.end_headers()
                    self.wfile.write(mp3)
                except Exception as exc:
                    err = json.dumps({"error": str(exc)}, ensure_ascii=False).encode("utf-8")
                    self.send_response(503)
                    self.send_header("Content-Type", "application/json; charset=utf-8")
                    self._cors()
                    self.end_headers()
                    self.wfile.write(err)
                return

            if not self.path.startswith("/api/chat"):
                self.send_response(404)
                self._cors()
                self.end_headers()
                return

            msg = str(payload.get("mensaje", "")).strip()
            resp = gestor.enviar_chat(msg) if msg else {"respuesta": ""}
            body = json.dumps(resp, ensure_ascii=False).encode("utf-8")
            self.send_response(200)
            self.send_header("Content-Type", "application/json; charset=utf-8")
            self._cors()
            self.end_headers()
            self.wfile.write(body)

        def log_message(self, fmt, *args):
            return

    return Handler


def main():
    puerto = int(os.environ.get("PORT", "7860"))
    perfil = os.environ.get("PERFIL_CORTEX", "macro-1m")
    ruta_modelo = Path(os.environ.get("RUTA_MODELO", str(DIR_CORTEX / "data" / "cerebro_macro_1m.bin")))
    bin_cerebro = DIR_CORTEX / "bin" / "cerebro"

    if not bin_cerebro.exists():
        subprocess.run(["make", "portable", f"-j{os.cpu_count() or 2}"], cwd=str(DIR_CORTEX), check=True)

    gestor = GestorMacroNube(bin_cerebro, ruta_modelo, perfil)
    t = threading.Thread(target=hilo_fabrica_continua, args=(gestor,), daemon=True)
    t.start()

    srv = ThreadingHTTPServer(("0.0.0.0", puerto), crear_handler(gestor))
    gestor.log(f"Observatorio Nube 24/7 abierto en 0.0.0.0:{puerto} (perfil={perfil})")
    srv.serve_forever()


if __name__ == "__main__":
    main()
