#!/usr/bin/env python3
"""
servidor_vivo.py — Observatorio Cortical Web 24/7 para CORTEX-LM Macro ("Corteza Viva").

Mantiene `./bin/cerebro tubo <modelo.bin>` cargado permanentemente en RAM (0 ms de recarga)
y expone un servidor HTTP ligero (0.0.0.0, sin dependencias externas) con interfaz visual
en tiempo real para:
  - Conversar en múltiples turnos con memoria ejecutiva NMDA y plasticidad viva (ACh -> CA3).
  - Observar qué Macro-Columnas Talamocorticales (TRN) se encienden en cada pregunta.
  - Medir Acetilcolina (ACh), Certeza de Ganglios Basales (Go / No-Go) y Pensamiento LIF.
  - Alimentar textos nuevos en vivo ("Leer = Saber" en CortezaSemanticaCSR) y disparar Sueño Cortical.

Uso:
  python3 tools/servidor_vivo.py --modelo /ruta/a/cerebro.bin --puerto 8080
"""

import argparse
import json
import os
import subprocess
import sys
import threading
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from pathlib import Path

HTML_UI = r"""<!DOCTYPE html>
<html lang="es">
<head>
<meta charset="utf-8"/>
<meta name="viewport" content="width=device-width, initial-scale=1"/>
<title>CORTEX-LM Macro — Observatorio Cortical Vivo</title>
<style>
  :root {
    --bg: #090d16; --panel: #111827; --border: #1f2937;
    --text: #e5e7eb; --muted: #9ca3af; --cyan: #22d3ee;
    --emerald: #10b981; --amber: #f59e0b; --rose: #f43f5e; --violet: #a78bfa;
  }
  * { box-sizing: border-box; }
  body {
    margin: 0; font-family: system-ui, -apple-system, sans-serif;
    background: var(--bg); color: var(--text);
    display: grid; grid-template-columns: 1.55fr 1fr; height: 100vh;
  }
  @media (max-width: 920px) {
    body { grid-template-columns: 1fr; height: auto; }
  }
  .col-chat { display: flex; flex-direction: column; border-right: 1px solid var(--border); height: 100vh; }
  header {
    padding: 14px 20px; border-bottom: 1px solid var(--border);
    display: flex; justify-content: space-between; align-items: center; background: #0b1120;
  }
  header h1 { margin: 0; font-size: 1.05rem; letter-spacing: 0.02em; color: var(--cyan); }
  .badge {
    font-size: 0.76rem; padding: 4px 10px; border-radius: 999px;
    background: rgba(34,211,238,0.12); color: var(--cyan); border: 1px solid rgba(34,211,238,0.3);
  }
  #messages { flex: 1; overflow-y: auto; padding: 18px; display: flex; flex-direction: column; gap: 14px; }
  .msg { max-width: 88%; padding: 12px 15px; border-radius: 12px; line-height: 1.48; font-size: 0.94rem; }
  .msg.user { align-self: flex-end; background: #1d4ed8; color: #fff; }
  .msg.bot { align-self: flex-start; background: var(--panel); border: 1px solid var(--border); }
  .msg .thought {
    font-family: ui-monospace, monospace; font-size: 0.79rem; color: var(--violet);
    background: rgba(167,139,250,0.08); border-left: 2px solid var(--violet);
    padding: 6px 9px; margin-bottom: 8px; border-radius: 4px;
  }
  .msg .meta-tag { font-size: 0.74rem; color: var(--emerald); margin-bottom: 5px; }
  .msg .nogo-tag { font-size: 0.74rem; color: var(--amber); margin-bottom: 5px; }
  .composer { padding: 14px; border-top: 1px solid var(--border); display: flex; gap: 8px; background: #0b1120; }
  input[type="text"], textarea {
    flex: 1; background: var(--panel); color: var(--text); border: 1px solid var(--border);
    border-radius: 8px; padding: 10px 12px; font-size: 0.94rem; outline: none;
  }
  button {
    background: var(--cyan); color: #06121d; font-weight: 600; border: none;
    border-radius: 8px; padding: 10px 15px; cursor: pointer; font-size: 0.88rem;
  }
  button.sec { background: #1f2937; color: var(--text); border: 1px solid #374151; }
  .col-obs { padding: 18px; overflow-y: auto; display: flex; flex-direction: column; gap: 16px; }
  .card { background: var(--panel); border: 1px solid var(--border); border-radius: 12px; padding: 14px; }
  .card h2 { margin: 0 0 10px 0; font-size: 0.86rem; text-transform: uppercase; color: var(--muted); letter-spacing: 0.05em; }
  .meter-row { display: flex; justify-content: space-between; font-size: 0.84rem; margin-bottom: 4px; }
  .bar-bg { height: 9px; background: #1f2937; border-radius: 99px; overflow: hidden; margin-bottom: 10px; }
  .bar-fill { height: 100%; transition: width 0.25s ease; }
  .cols-grid { display: flex; flex-wrap: wrap; gap: 6px; }
  .col-pill {
    font-family: ui-monospace, monospace; font-size: 0.78rem; padding: 4px 8px;
    border-radius: 6px; background: rgba(16,185,129,0.13); border: 1px solid rgba(16,185,129,0.35); color: var(--emerald);
  }
  .premisa {
    font-size: 0.82rem; padding: 8px; border-radius: 6px; background: #0b1120;
    border: 1px solid var(--border); margin-top: 6px; color: #d1d5db;
  }
  .toolbar { display: flex; gap: 8px; flex-wrap: wrap; }
</style>
</head>
<body>
  <section class="col-chat">
    <header>
      <h1>CORTEX-LM Macro · Corteza Spiking LIF Viva</h1>
      <span class="badge" id="brainStats">Conectando corteza...</span>
    </header>
    <div id="messages"></div>
    <form class="composer" id="chatForm">
      <input type="text" id="promptInput" placeholder="Conversa, pregunta o enseña un dato declarativo en vivo..." autocomplete="off"/>
      <button type="submit">Enviar</button>
    </form>
  </section>

  <aside class="col-obs">
    <div class="card">
      <h2>Fisiología Neuromoduladora en Vivo</h2>
      <div class="meter-row"><span>Sorpresa Acetilcolina (ACh)</span><span id="achVal">0%</span></div>
      <div class="bar-bg"><div class="bar-fill" id="achBar" style="width:0%;background:var(--amber)"></div></div>
      <div class="meter-row"><span>Certeza Ganglios Basales (<span id="goState">Go</span>)</span><span id="certVal">100%</span></div>
      <div class="bar-bg"><div class="bar-fill" id="certBar" style="width:100%;background:var(--emerald)"></div></div>
      <div class="toolbar">
        <button class="sec" type="button" onclick="accion('/sonar')">Sueño Cortical (Consolidar)</button>
        <button class="sec" type="button" onclick="accion('/nuevo')">Nuevo Hilo</button>
      </div>
    </div>

    <div class="card">
      <h2>Macro-Columnas Talamocorticales (TRN)</h2>
      <div class="cols-grid" id="trnCols"><span style="color:var(--muted);font-size:0.84rem">Esperando espigas...</span></div>
    </div>

    <div class="card">
      <h2> Engramas Recuperados (CA3 + Semántica CSR)</h2>
      <div id="engramBox" style="color:var(--muted);font-size:0.84rem">Ningún engrama consultado aún.</div>
    </div>

    <div class="card">
      <h2>Leer = Saber (Asimilar Conocimiento Continuo)</h2>
      <textarea id="docInput" rows="4" style="width:100%;margin-bottom:8px" placeholder="Pega párrafos científicos, técnicos o notas para indexarlos en CortezaSemanticaCSR y consolidarlos por sueño cortical..."></textarea>
      <button type="button" onclick="asimilarDoc()">Asimilar en Corteza Semántica</button>
    </div>
  </aside>

<script>
const msgs = document.getElementById('messages');
function addMsg(role, text, data) {
  const d = document.createElement('div');
  d.className = 'msg ' + role;
  if (data) {
    if (data.aprendio_vivo) {
      const t = document.createElement('div');
      t.className = 'meta-tag';
      t.textContent = 'Plasticidad Viva ACh -> Consolidado automáticamente en Hipocampo CA3';
      d.appendChild(t);
    } else if (data.go === false) {
      const t = document.createElement('div');
      t.className = 'nogo-tag';
      t.textContent = 'Ganglios Basales No-Go -> Abstención honesta (evitando alucinación)';
      d.appendChild(t);
    }
    if (data.pensamiento) {
      const th = document.createElement('div');
      th.className = 'thought';
      th.textContent = '~ ' + data.pensamiento;
      d.appendChild(th);
    }
  }
  const p = document.createElement('div');
  p.textContent = text;
  d.appendChild(p);
  msgs.appendChild(d);
  msgs.scrollTop = msgs.scrollHeight;
}

function updateObs(d) {
  if (typeof d.ach === 'number') {
    const a = Math.round(d.ach * 100);
    document.getElementById('achVal').textContent = a + '%';
    document.getElementById('achBar').style.width = a + '%';
  }
  if (typeof d.certeza === 'number') {
    const c = Math.round(d.certeza * 100);
    document.getElementById('certVal').textContent = c + '%';
    const bar = document.getElementById('certBar');
    bar.style.width = c + '%';
    bar.style.background = d.go === false ? 'var(--rose)' : 'var(--emerald)';
    document.getElementById('goState').textContent = d.go === false ? 'No-Go' : 'Go';
  }
  if (Array.isArray(d.columnas) && d.columnas.length > 0) {
    const box = document.getElementById('trnCols');
    box.innerHTML = '';
    d.columnas.forEach(c => {
      const s = document.createElement('span');
      s.className = 'col-pill';
      s.textContent = `Col #${c.id} · ${c.energia.toFixed(1)}`;
      box.appendChild(s);
    });
  }
  const eb = document.getElementById('engramBox');
  let html = '';
  if (d.recuerdo_ca3) {
    html += `<div class="premisa"><strong>Hipocampo CA3 (sim=${(d.sim_ca3||0).toFixed(2)}):</strong> ${d.recuerdo_ca3}</div>`;
  }
  if (Array.isArray(d.premisas)) {
    d.premisas.forEach((p, idx) => {
      html += `<div class="premisa"><strong>Semántica CSR #${idx+1} (sim=${p.sim.toFixed(2)}):</strong> ${p.texto}</div>`;
    });
  }
  if (html) eb.innerHTML = html;
  refreshStats(d);
}

function refreshStats(d) {
  if (d.neuronas) {
    document.getElementById('brainStats').textContent =
      `${d.regiones} regiones · ${(d.neuronas).toLocaleString()} LIF · CA3=${d.ca3} · CSR=${d.semantica}`;
  } else if (typeof d.ca3 === 'number') {
    const cur = document.getElementById('brainStats').textContent.split('· CA3=')[0];
    document.getElementById('brainStats').textContent = `${cur}· CA3=${d.ca3} · CSR=${d.semantica}`;
  }
}

async function loadState() {
  const r = await fetch('/api/estado');
  const d = await r.json();
  refreshStats(d);
  addMsg('bot', `Corteza spiking activa (${(d.neuronas||0).toLocaleString()} neuronas LIF puras, ${(d.sinapsis||0).toLocaleString()} sinapsis CSR). Habla conmigo o enséñame hechos nuevos en vivo.`);
}

document.getElementById('chatForm').addEventListener('submit', async (e) => {
  e.preventDefault();
  const inp = document.getElementById('promptInput');
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
  addMsg('bot', d.respuesta || '', d);
});

async function accion(cmd) {
  const r = await fetch('/api/chat', {
    method: 'POST', headers: {'Content-Type': 'application/json'},
    body: JSON.stringify({mensaje: cmd})
  });
  const d = await r.json();
  updateObs(d);
  addMsg('bot', d.respuesta || 'Listo.');
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
  updateObs(d);
  addMsg('bot', d.respuesta || 'Conocimiento asimilado.');
}

loadState();
</script>
</body>
</html>
"""


class PuenteCerebroVivo:
    def __init__(self, bin_cerebro: str, ruta_modelo: str, temp: float = 0.08, honesto: bool = True):
        cmd = [bin_cerebro, "tubo", ruta_modelo, "--temp", str(temp)]
        if honesto:
            cmd.append("--honesto")
        self.proc = subprocess.Popen(
            cmd,
            stdin=subprocess.PIPE,
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
            text=True,
            bufsize=1,
        )
        self.lock = threading.Lock()
        first = self.proc.stdout.readline().strip()
        if not first:
            err = self.proc.stderr.read()
            raise RuntimeError(f"No se pudo iniciar cerebro tubo: {err}")
        self.info = json.loads(first)

    def enviar(self, mensaje: str) -> dict:
        limpio = mensaje.replace("\r", " ").replace("\n", " ").strip()
        with self.lock:
            assert self.proc.stdin and self.proc.stdout
            self.proc.stdin.write(limpio + "\n")
            self.proc.stdin.flush()
            linea = self.proc.stdout.readline().strip()
            if not linea:
                return {"evento": "error", "respuesta": "El proceso cortical se cerró."}
            datos = json.loads(linea)
            if "ca3" in datos:
                self.info["ca3"] = datos["ca3"]
            if "semantica" in datos:
                self.info["semantica"] = datos["semantica"]
            return datos


def crear_handler(puente: PuenteCerebroVivo):
    class Handler(BaseHTTPRequestHandler):
        def _cors(self):
            self.send_header("Access-Control-Allow-Origin", "*")
            self.send_header("Access-Control-Allow-Headers", "Content-Type")
            self.send_header("Access-Control-Allow-Methods", "GET, POST, OPTIONS")

        def do_OPTIONS(self):
            self.send_response(204)
            self._cors()
            self.end_headers()

        def do_GET(self):
            if self.path == "/api/estado":
                body = json.dumps(puente.info, ensure_ascii=False).encode("utf-8")
                self.send_response(200)
                self.send_header("Content-Type", "application/json; charset=utf-8")
                self._cors()
                self.end_headers()
                self.wfile.write(body)
                return
            body = HTML_UI.encode("utf-8")
            self.send_response(200)
            self.send_header("Content-Type", "text/html; charset=utf-8")
            self._cors()
            self.end_headers()
            self.wfile.write(body)

        def do_POST(self):
            if self.path != "/api/chat":
                self.send_response(404)
                self.end_headers()
                return
            n = int(self.headers.get("Content-Length", "0"))
            raw = self.rfile.read(n).decode("utf-8", errors="replace")
            payload = json.loads(raw) if raw else {}
            msg = str(payload.get("mensaje", "")).strip()
            resp = puente.enviar(msg) if msg else {"respuesta": ""}
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
    ap = argparse.ArgumentParser(description="Observatorio Cortical Web para CORTEX-LM Macro")
    ap.add_argument("--modelo", required=True, help="Ruta al archivo .bin del cerebro")
    ap.add_argument("--host", default="0.0.0.0", help="Host de escucha (por defecto 0.0.0.0)")
    ap.add_argument("--puerto", type=int, default=8080, help="Puerto HTTP")
    ap.add_argument("--temp", type=float, default=0.08, help="Temperatura cortical")
    ap.add_argument("--sin-honestidad", action="store_true", help="Desactivar compuerta Go/No-Go")
    args = ap.parse_args()

    raiz = Path(__file__).resolve().parent.parent
    bin_cerebro = str(raiz / "bin" / "cerebro")
    if not os.path.exists(bin_cerebro):
        print(f"Error: no existe {bin_cerebro}. Ejecuta 'make' primero.", file=sys.stderr)
        sys.exit(1)

    puente = PuenteCerebroVivo(bin_cerebro, args.modelo, args.temp, not args.sin_honestidad)
    srv = ThreadingHTTPServer((args.host, args.puerto), crear_handler(puente))
    print(
        f"[OBSERVATORIO CORTICAL VIVO] Escuchando en http://{args.host}:{args.puerto} | "
        f"{puente.info.get('regiones')} regiones | {puente.info.get('neuronas')} neuronas LIF | "
        f"{puente.info.get('sinapsis')} sinapsis CSR",
        flush=True,
    )
    try:
        srv.serve_forever()
    except KeyboardInterrupt:
        pass


if __name__ == "__main__":
    main()
