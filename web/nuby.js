// ============================================================================
// NUBY EN EL NAVEGADOR — el motor C++ ejecutándose como WebAssembly.
//
// Esta página NO dibuja la interfaz: dibuja los píxeles que produce el motor.
// Tampoco inventa resultados: el buscador es el BM25 real del motor sobre el
// rastreo real (nuby_engine/data/crawl_pages.tsv).
//
// Limitaciones REALES de esta build (se dicen, no se esconden):
//   • No ejecuta JavaScript de las páginas (C++ para wasm no tiene excepciones
//     en esta cadena de herramientas; el motor nativo sí lo ejecuta).
//   • La red la pone el navegador y solo puede leer el MISMO ORIGEN, así que
//     navegar a webs externas fallará con el error real del navegador (CORS).
// ============================================================================

(function () {
  "use strict";

  const STATE = {
    exports: null,
    canvas: null,
    ctx: null,
    ready: false,
    w: 0,
    h: 0,
    lastSeq: -1,
  };

  const $ = (id) => document.getElementById(id);
  function setStatus(txt) { const el = $("nuby-stat"); if (el) el.textContent = txt; }
  function setNote(txt) { const el = $("nuby-note"); if (el) el.textContent = txt; }

  // ---------------------------------------------------------------- RLE -----
  // Formato del motor: [u32 w][u32 h][u32 len] y luego PackBits por píxel.
  function drawRle(bytes) {
    const dv = new DataView(bytes.buffer, bytes.byteOffset, bytes.byteLength);
    const w = dv.getUint32(0, true);
    const h = dv.getUint32(4, true);
    const len = dv.getUint32(8, true);
    if (!w || !h) return;

    if (STATE.w !== w || STATE.h !== h) {
      STATE.canvas.width = w;
      STATE.canvas.height = h;
      STATE.canvas.style.aspectRatio = w + " / " + h;
      STATE.w = w; STATE.h = h;
    }
    const img = STATE.ctx.createImageData(w, h);
    const px = img.data;
    let p = 12, i = 0, end = 12 + len;
    while (p < end && i < w * h) {
      const ctrl = bytes[p++];
      if (ctrl & 0x80) {
        const reps = 257 - ctrl;
        const r = bytes[p++], g = bytes[p++], b = bytes[p++];
        for (let k = 0; k < reps && i < w * h; k++, i++) {
          const o = i * 4;
          px[o] = r; px[o + 1] = g; px[o + 2] = b; px[o + 3] = 255;
        }
      } else {
        const count = ctrl + 1;
        for (let k = 0; k < count && i < w * h; k++, i++) {
          const r = bytes[p++], g = bytes[p++], b = bytes[p++];
          const o = i * 4;
          px[o] = r; px[o + 1] = g; px[o + 2] = b; px[o + 3] = 255;
        }
      }
    }
    STATE.ctx.putImageData(img, 0, 0);
  }

  function paint() {
    const ex = STATE.exports;
    if (!ex) return;
    const lenPtr = ex.nuby_alloc(4);
    const ptr = ex.nuby_frame_rle(lenPtr);
    if (!ptr) return;
    const len = new DataView(ex.memory.buffer).getUint32(lenPtr, true);
    if (!len) return;
    drawRle(new Uint8Array(ex.memory.buffer, ptr, len));
  }

  function refreshState() {
    const ex = STATE.exports;
    if (!ex) return;
    const ptr = ex.nuby_state();
    const str = readCString(ptr);
    try {
      const st = JSON.parse(str);
      setStatus("URL: " + st.url + "  ·  " + st.status + "  ·  " +
                st.docs + " documentos  ·  " + st.terms + " términos en el índice BM25");
    } catch (e) { /* estado ilegible: no se inventa nada */ }
  }

  // Escribe una cadena UTF-8 (con NUL) en la memoria del wasm y ejecuta fn
  // con el puntero. El API de WebAssembly no convierte cadenas de JS solo:
  // hay que reservar memoria dentro del módulo (nuby_alloc).
  function withCString(str, fn) {
    const ex = STATE.exports;
    const bytes = new TextEncoder().encode(str + "\u0000");
    const ptr = ex.nuby_alloc(bytes.length);
    new Uint8Array(ex.memory.buffer).set(bytes, ptr);
    return fn(ptr);
  }

  function callString(name, str) {
    const ex = STATE.exports;
    if (!ex || !ex[name]) return 0;
    return withCString(str, (ptr) => ex[name](ptr));
  }

  function readCString(ptr) {
    const mem = new Uint8Array(STATE.exports.memory.buffer);
    let end = ptr;
    while (end < mem.length && mem[end] !== 0) end++;
    return new TextDecoder().decode(mem.subarray(ptr, end));
  }

  function tick() {
    const ex = STATE.exports;
    if (!ex) return;
    if (ex.nuby_tick()) {
      paint();
      refreshState();
    }
    setTimeout(tick, 220);
  }

  // -------------------------------------------------------------- entrada ---
  function canvasCoords(ev) {
    const r = STATE.canvas.getBoundingClientRect();
    const x = Math.round((ev.clientX - r.left) * (STATE.canvas.width / r.width));
    const y = Math.round((ev.clientY - r.top) * (STATE.canvas.height / r.height));
    return [Math.max(0, Math.min(STATE.canvas.width - 1, x)),
            Math.max(0, Math.min(STATE.canvas.height - 1, y))];
  }

  function bindInput() {
    const ex = () => STATE.exports;
    STATE.canvas.addEventListener("mousedown", (ev) => {
      ev.preventDefault();
      const [x, y] = canvasCoords(ev);
      ex().nuby_click(x, y);
      paint(); refreshState();
      STATE.canvas.focus();
    });
    STATE.canvas.addEventListener("wheel", (ev) => {
      ev.preventDefault();
      ex().nuby_wheel(Math.round(ev.deltaY));
      paint(); refreshState();
    }, { passive: false });

    window.addEventListener("keydown", (ev) => {
      const e = ex();
      if (!e) return;
      const special = {
        Backspace: "Backspace", Enter: "Enter", Escape: "Escape",
        ArrowLeft: "ArrowLeft", ArrowRight: "ArrowRight",
        ArrowUp: "ArrowUp", ArrowDown: "ArrowDown",
      };
      if (special[ev.key]) {
        ev.preventDefault();
        callString("nuby_key", special[ev.key]);
        paint(); refreshState();
        return;
      }
      if (ev.key.length === 1 && !ev.ctrlKey && !ev.metaKey) {
        ev.preventDefault();
        e.nuby_char(ev.key.codePointAt(0));
        paint(); refreshState();
      }
    });

    let rt = null;
    window.addEventListener("resize", () => {
      clearTimeout(rt);
      rt = setTimeout(() => {
        resizeEngine();
        paint(); refreshState();
      }, 180);
    });
  }

  function resizeEngine() {
    const ex = STATE.exports;
    if (!ex) return;
    const box = STATE.canvas.parentElement;
    const w = Math.max(320, Math.min(1400, box.clientWidth));
    const h = Math.max(360, Math.min(900, Math.round(window.innerHeight * 0.72)));
    ex.nuby_resize(w, h);
  }

  // --------------------------------------------------------------- arranque -
  async function loadBinary(url) {
    const r = await fetch(url, { cache: "no-store" });
    if (!r.ok) throw new Error("no se pudo descargar " + url + " (" + r.status + ")");
    return new Uint8Array(await r.arrayBuffer());
  }

  async function boot() {
    const base = document.body.dataset.base || "./";
    setNote("Cargando el motor (wasm), el índice real y las fuentes…");

    const [wasmBytes, tsv, fontRegular, fontBold] = await Promise.all([
      loadBinary(base + "nuby.wasm"),
      loadBinary(base + "data/crawl_pages.tsv"),
      loadBinary(base + "data/fonts/DejaVuSans.ttf"),
      loadBinary(base + "data/fonts/DejaVuSans-Bold.ttf"),
    ]);

    // Sistema de archivos virtual: lo que el motor nativo lee del disco, aquí
    // lo lee de esta tabla (mismo contenido real, sin cambios en el motor).
    const files = {
      "data/crawl_pages.tsv": tsv,
      "data/fonts/DejaVuSans.ttf": fontRegular,
      "data/fonts/DejaVuSans-Bold.ttf": fontBold,
      "DejaVuSans.ttf": fontRegular,
      "DejaVuSans-Bold.ttf": fontBold,
      "data/fonts/DejaVuSans.ttf": fontRegular,
      "data/fonts/DejaVuSans-Bold.ttf": fontBold,
    };

    const wasiRt = NubyWasi.createWasi({
      files,
      onStdout: (s) => console.log("[motor]", s.trimEnd()),
      onExit: (code) => setNote("El motor terminó con código " + code),
      randomBytes: (buf) => {
        if (window.crypto && window.crypto.getRandomValues) window.crypto.getRandomValues(buf);
        else for (let i = 0; i < buf.length; i++) buf[i] = (Math.random() * 256) | 0;
      },
    });

    const importObject = {
      wasi_snapshot_preview1: wasiRt.wasi,
      nuby: { http_fetch: NubyWasi.createHttpFetch(() => STATE.exports.memory) },
    };

    const { instance } = await WebAssembly.instantiate(wasmBytes, importObject);
    NubyWasi.setAlloc(instance.exports.nuby_alloc);
    // setAlloc antes de instanciar no es posible (nuby_alloc nace con el
    // módulo): se conecta aquí y la primera petición de red ya lo tiene.
    STATE.exports = instance.exports;
    wasiRt.setMemory(instance.exports.memory);

    const docs = instance.exports.nuby_boot("data/crawl_pages.tsv", 1024, 640);
    if (!docs) throw new Error("el motor arrancó pero el índice cargó 0 documentos");

    resizeEngine();
    paint();
    refreshState();
    bindInput();
    STATE.ready = true;
    setNote("");
    tick();
    console.log("[nuby] motor en marcha:", docs, "documentos indexados");
  }

  function goto(url) {
    const r = callString("nuby_goto", url);
    paint(); refreshState();
    return r;
  }

  function key(name) {
    const r = callString("nuby_key", name);
    paint(); refreshState();
    return r;
  }

  function searchJson(q) {
    const ex = STATE.exports;
    if (!ex) return null;
    const ptr = withCString(q, (p) => ex.nuby_search(p));
    return { json: readCString(ptr) };
  }

  window.NubyApp = { boot, paint, refreshState, goto, key, searchJson, readCString, withCString, state: STATE };

  window.addEventListener("DOMContentLoaded", () => {
    STATE.canvas = $("nuby-screen");
    STATE.ctx = STATE.canvas.getContext("2d", { alpha: false });
    boot().catch((err) => {
      setNote("No se pudo arrancar el motor: " + err.message);
      console.error(err);
    });
  });
})();
