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
    keys: null,       // campo oculto que da vida al teclado del móvil
    ready: false,
    w: 0,             // tamaño del búfer en px físicos
    h: 0,
    logicalW: 0,      // tamaño en px lógicos (los que usa el motor para el layout)
    logicalH: 0,
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
      // El búfer va a la resolución física (w,h) y el CSS lo muestra al tamaño
      // lógico: así un píxel del motor cae en un píxel real de la pantalla.
      STATE.canvas.width = w;
      STATE.canvas.height = h;
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
  // El motor trabaja en píxeles LÓGICOS (los mismos que el CSS de la página);
  // el canvas tiene la resolución física (x densidad). Por eso la conversión
  // usa el tamaño CSS, no el tamaño del búfer.
  function canvasCoords(ev) {
    const r = STATE.canvas.getBoundingClientRect();
    const x = Math.round((ev.clientX - r.left) * (STATE.logicalW / r.width));
    const y = Math.round((ev.clientY - r.top) * (STATE.logicalH / r.height));
    return [Math.max(0, Math.min(STATE.logicalW - 1, x)),
            Math.max(0, Math.min(STATE.logicalH - 1, y))];
  }

  // Enseña el teclado del teléfono: los navegadores solo abren el teclado
  // virtual si el foco va a un campo de texto REAL dentro de un gesto del
  // usuario (un tap). Sin esto, en un móvil no se puede escribir NADA.
  function focusKeyCatcher() {
    const el = STATE.keys;
    if (!el) return;
    try {
      el.focus({ preventScroll: true });
    } catch (e) {
      try { el.focus(); } catch (_) { /* sin teclado disponible: no se rompe */ }
    }
  }

  function bindInput() {
    const ex = () => STATE.exports;
    const canvas = STATE.canvas;

    const clickAt = (ev) => {
      const [x, y] = canvasCoords(ev);
      ex().nuby_click(x, y);
      paint(); refreshState();
      focusKeyCatcher();          // móvil: abre el teclado del sistema
    };

    canvas.addEventListener("mousedown", (ev) => { ev.preventDefault(); clickAt(ev); });

    // ---- Táctil: tap = clic, arrastre vertical = scroll del motor ----
    let touchY = null, moved = 0;
    canvas.addEventListener("touchstart", (ev) => {
      if (ev.touches.length !== 1) return;
      ev.preventDefault();
      touchY = ev.touches[0].clientY;
      moved = 0;
    }, { passive: false });

    canvas.addEventListener("touchmove", (ev) => {
      if (touchY === null || ev.touches.length !== 1) return;
      ev.preventDefault();
      const y = ev.touches[0].clientY;
      const dy = touchY - y;              // dedo hacia arriba = bajar la página
      moved += Math.abs(dy);
      touchY = y;
      if (Math.abs(dy) >= 1 && ex().nuby_wheel(Math.round(dy))) { paint(); refreshState(); }
    }, { passive: false });

    canvas.addEventListener("touchend", (ev) => {
      if (touchY !== null && moved < 8) clickAt(ev.changedTouches[0]);
      touchY = null;
    }, { passive: false });

    canvas.addEventListener("wheel", (ev) => {
      ev.preventDefault();
      ex().nuby_wheel(Math.round(ev.deltaY));
      paint(); refreshState();
    }, { passive: false });

    // ---- Teclado FÍSICO (escritorio) ----
    window.addEventListener("keydown", (ev) => {
      const e = ex();
      if (!e || !STATE.ready) return;
      // Si el usuario está escribiendo en el campo oculto, ese camino manda.
      if (document.activeElement === STATE.keys) return;
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

    // ---- Teclado TÁCTIL (móvil): campo oculto que recibe el texto real ----
    const keys = STATE.keys;
    if (keys) {
      keys.addEventListener("input", () => {
        const e = ex();
        if (!e) return;
        const value = keys.value;      // puede traer varias letras del autocorrector
        keys.value = "";
        for (const ch of value) {
          const cp = ch.codePointAt(0);
          if (cp >= 32 && cp !== 127) e.nuby_char(cp);
        }
        paint(); refreshState();
      });
      keys.addEventListener("keydown", (ev) => {
        const special = {
          Backspace: "Backspace", Enter: "Enter", Escape: "Escape",
          ArrowLeft: "ArrowLeft", ArrowRight: "ArrowRight",
          ArrowUp: "ArrowUp", ArrowDown: "ArrowDown",
        };
        if (special[ev.key]) {
          ev.preventDefault();
          callString("nuby_key", special[ev.key]);
          paint(); refreshState();
        }
      });
    }

    let rt = null;
    const relayout = () => {
      clearTimeout(rt);
      rt = setTimeout(() => { resizeEngine(); paint(); refreshState(); }, 150);
    };
    window.addEventListener("resize", relayout);
    window.addEventListener("orientationchange", relayout);
    if (window.visualViewport) window.visualViewport.addEventListener("resize", relayout);
  }

  // Tamaño de la ventana del motor = tamaño REAL del contenedor en píxeles
  // lógicos (CSS). Antes estaba limitado a 900 px de alto (72% de la pantalla)
  // y a 1400 de ancho: en un móvil la interfaz no llenaba la pantalla.
  function resizeEngine() {
    const ex = STATE.exports;
    if (!ex) return;
    const box = STATE.canvas.parentElement;
    const rect = box.getBoundingClientRect();
    const w = Math.max(240, Math.round(rect.width || box.clientWidth || window.innerWidth));
    const h = Math.max(320, Math.round(rect.height || box.clientHeight || window.innerHeight));
    // Densidad de la pantalla (2 o 3 en móviles): el motor rasteriza a esa
    // resolución y el texto deja de verse pixelado.
    const dpr = Math.max(1, Math.min(3, window.devicePixelRatio || 1));
    if (typeof ex.nuby_set_scale === "function") ex.nuby_set_scale(dpr);
    ex.nuby_resize(w, h);
    STATE.logicalW = ex.nuby_width ? ex.nuby_width() : w;
    STATE.logicalH = ex.nuby_height ? ex.nuby_height() : h;
  }

  // --------------------------------------------------------------- arranque -
  async function loadBinary(url) {
    const r = await fetch(url, { cache: "no-store" });
    if (!r.ok) throw new Error("no se pudo descargar " + url + " (" + r.status + ")");
    return new Uint8Array(await r.arrayBuffer());
  }

  // BUG REAL corregido (2026-10-13, visto en el movil del usuario): el motor
  // pedia SIEMPRE "./data/..." y eso solo existe cuando el sitio se publica
  // desde la carpeta web/ (build de Actions). Cuando GitHub Pages publica la
  // raiz del repositorio, el indice vive en "../nuby_engine/data/", y la
  // pagina mostraba "no se pudo descargar ./data/crawl_pages.tsv (404)".
  // Ahora se prueban las rutas reales del proyecto, en orden, y se usa la
  // primera que responde: la MISMA pagina funciona en los dos modos.
  async function loadFirst(candidates) {
    let lastErr = null;
    for (const url of candidates) {
      try { return await loadBinary(url); } catch (e) { lastErr = e; }
    }
    throw new Error(lastErr ? lastErr.message + " — rutas probadas: " +
                    candidates.join(", ") : "no hay rutas candidatas");
  }

  async function boot() {
    const base = document.body.dataset.base || "./";
    setNote("Cargando el motor (wasm), el índice real y las fuentes…");

    // Bases posibles para los datos del motor (índice y fuentes):
    //   1. ./data/                 -> sitio publicado desde web/ (Actions)
    //   2. ../nuby_engine/data/    -> sitio publicado desde la raiz del repo
    const dataBases = [base + "data/", base + "../nuby_engine/data/"];

    const [wasmBytes, tsv, fontRegular, fontBold] = await Promise.all([
      loadBinary(base + "nuby.wasm"),
      loadFirst(dataBases.map((b) => b + "crawl_pages.tsv")),
      loadFirst(dataBases.map((b) => b + "fonts/DejaVuSans.ttf")),
      loadFirst(dataBases.map((b) => b + "fonts/DejaVuSans-Bold.ttf")),
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
    STATE.keys = $("nuby-keys");   // campo oculto: teclado del móvil
    boot().catch((err) => {
      setNote("No se pudo arrancar el motor: " + err.message);
      console.error(err);
    });
  });
})();
