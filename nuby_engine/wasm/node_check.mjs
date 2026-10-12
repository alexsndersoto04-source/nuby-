// ============================================================================
// COMPROBACIÓN DEL MOTOR EN NODE (sin navegador)
//
// Ejecuta la build de WebAssembly del motor con el runtime WASI del proyecto y
// comprueba, con datos reales, que: el índice carga, el buscador BM25 devuelve
// resultados, el frame se decodifica y la navegación (misma-origen) funciona.
//
//   Uso: node nuby_engine/wasm/node_check.mjs
// ============================================================================

import fs from "node:fs";
import path from "node:path";
import url from "node:url";

const ROOT = path.resolve(path.dirname(url.fileURLToPath(import.meta.url)), "../..");
const WEB = path.join(ROOT, "web");
let failures = 0;
const check = (ok, what) => {
  console.log((ok ? "OK    " : "FALLA ") + what);
  if (!ok) failures++;
};

// ---- entorno mínimo de navegador para el shim WASI -------------------------
globalThis.window = globalThis;
const STATIC_ROOT = WEB;
globalThis.XMLHttpRequest = class {
  constructor() { this.status = 0; this.responseText = ""; this.responseURL = ""; this.statusText = ""; }
  open(method, target) { this.method = method; this.url = target; }
  setRequestHeader() {}
  send() {
    // mismo origen simulado: se sirven ficheros de web/ (como haría el sitio)
    const rel = this.url.replace(/^https?:\/\/[^/]+/, "");
    const file = path.join(STATIC_ROOT, decodeURIComponent(rel).replace(/^\//, ""));
    try {
      const data = fs.readFileSync(file);
      let s = "";
      for (const b of data) s += String.fromCharCode(b);
      this.status = 200; this.statusText = "OK"; this.responseText = s; this.responseURL = this.url;
    } catch {
      this.status = 404; this.statusText = "Not Found"; this.responseText = "<h1>404</h1>";
    }
  }
};
new Function(fs.readFileSync(path.join(WEB, "nuby-wasi.js"), "utf8"))();

// ---- carga del motor -------------------------------------------------------
const wasmBytes = fs.readFileSync(path.join(WEB, "nuby.wasm"));
const read = (p) => new Uint8Array(fs.readFileSync(p));
const reg = read(path.join(WEB, "data/fonts/DejaVuSans.ttf"));
const bold = read(path.join(WEB, "data/fonts/DejaVuSans-Bold.ttf"));
const files = {
  "data/crawl_pages.tsv": read(path.join(WEB, "data/crawl_pages.tsv")),
  "data/fonts/DejaVuSans.ttf": reg,
  "data/fonts/DejaVuSans-Bold.ttf": bold,
  "DejaVuSans.ttf": reg,
  "DejaVuSans-Bold.ttf": bold,
};

const wasi = globalThis.NubyWasi.createWasi({ files, onStdout: () => {} });
let exportsRef = null;
const { instance } = await WebAssembly.instantiate(wasmBytes, {
  wasi_snapshot_preview1: wasi.wasi,
  nuby: { http_fetch: globalThis.NubyWasi.createHttpFetch(() => exportsRef.memory) },
});
exportsRef = instance.exports;
globalThis.NubyWasi.setAlloc(instance.exports.nuby_alloc);
wasi.setMemory(instance.exports.memory);
const ex = instance.exports;

const cstr = (ptr) => {
  const mem = new Uint8Array(ex.memory.buffer);
  let end = ptr; while (mem[end] !== 0) end++;
  return new TextDecoder().decode(mem.subarray(ptr, end));
};
const withCString = (str, fn) => {
  const bytes = new TextEncoder().encode(str + "\u0000");
  const ptr = ex.nuby_alloc(bytes.length);
  new Uint8Array(ex.memory.buffer).set(bytes, ptr);
  return fn(ptr);
};

function frame() {
  const lenPtr = ex.nuby_alloc(4);
  const ptr = ex.nuby_frame_rle(lenPtr);
  const len = new DataView(ex.memory.buffer).getUint32(lenPtr, true);
  if (!ptr || !len) return null;
  const bytes = new Uint8Array(ex.memory.buffer, ptr, len);
  const dv = new DataView(bytes.buffer, bytes.byteOffset, bytes.byteLength);
  const w = dv.getUint32(0, true), h = dv.getUint32(4, true), dl = dv.getUint32(8, true);
  const px = new Uint8Array(w * h * 3);
  let p = 12, i = 0;
  const colors = new Set();
  while (p < 12 + dl && i < w * h) {
    const ctrl = bytes[p++];
    if (ctrl & 0x80) {
      const reps = 257 - ctrl, r = bytes[p++], g = bytes[p++], b = bytes[p++];
      colors.add((r << 16) | (g << 8) | b);
      for (let k = 0; k < reps && i < w * h; k++, i++) { px[i * 3] = r; px[i * 3 + 1] = g; px[i * 3 + 2] = b; }
    } else {
      const count = ctrl + 1;
      for (let k = 0; k < count && i < w * h; k++, i++) {
        const r = bytes[p++], g = bytes[p++], b = bytes[p++];
        colors.add((r << 16) | (g << 8) | b);
        px[i * 3] = r; px[i * 3 + 1] = g; px[i * 3 + 2] = b;
      }
    }
  }
  return { w, h, px, colors: colors.size, rle: len };
}

// ---- pruebas ---------------------------------------------------------------
const t0 = Date.now();
const docs = ex.nuby_boot("data/crawl_pages.tsv", 1024, 640);
console.log(`índice: ${docs} documentos en ${Date.now() - t0} ms, ${ex.nuby_terms()} términos`);
check(docs >= 900, "el índice real se cargó en la build de navegador");

const home = frame();
check(home && home.w === 1024 && home.h === 640, "el motor produjo un frame 1024x640");
check(home && home.colors > 20, `el frame tiene contenido real (${home ? home.colors : 0} colores distintos)`);
check(home && home.rle > 1000, `el frame viene comprimido con el RLE propio (${home ? home.rle : 0} bytes)`);

const found = JSON.parse(cstr(withCString("ciencia", (p) => ex.nuby_search(p))));
check(found.results.length > 0, `BM25 devuelve resultados para "ciencia" (${found.results.length})`);
check(!!(found.results[0] && found.results[0].url.startsWith("http")),
      "los resultados traen URL real del rastreo");

// Interacción real: clic en la caja de búsqueda de la home (y=261) y teclear
ex.nuby_click(512, 261);
for (const ch of "ciencia") ex.nuby_char(ch.codePointAt(0));
ex.nuby_key(withCString("Enter", (p) => p)); // handle_key espera una clave ("Enter")
const results = frame();
check(results && results.colors > 20, "la vista de resultados se pintó tras teclear y pulsar Enter");

withCString("http://local/corpus/ejemplo.html", (p) => ex.nuby_goto(p));
const demo = frame();
const state = JSON.parse(cstr(ex.nuby_state()));
check(demo && demo.colors > 100, `la página de ejemplo se descargó y pintó (${demo ? demo.colors : 0} colores)`);
check(state.docs >= 996, "la página visitada se indexó en vivo (996 documentos)");
check(state.status.includes("HTTP 200"), `el estado reporta la descarga real: "${state.status}"`);

withCString("https://es.wikipedia.org/wiki/Ciencia", (p) => ex.nuby_goto(p));
const ext = JSON.parse(cstr(ex.nuby_state()));
check(ext.status.toLowerCase().includes("fallo") || ext.status.includes("bloqueo"),
      `la web externa falla de forma honesta (CORS): "${ext.status}"`);

console.log(failures === 0 ? "\nTODO PASA en la build de navegador" : `\n${failures} comprobaciones fallaron`);
process.exit(failures === 0 ? 0 : 1);
