/**
 * Utilidades de interfaz: creación de elementos, avisos, medidores y controles.
 * Sin dependencias externas: todo DOM nativo.
 */

/**
 * Crea un elemento.
 * @param {string} tag
 * @param {object} [props] class, text, dataset, style, on* handlers, atributos
 * @param {...(Node|string|null)} children
 */
export function h(tag, props = {}, ...children) {
  const node = document.createElement(tag);
  for (const [key, value] of Object.entries(props ?? {})) {
    if (value === null || value === undefined) continue;
    if (key === 'class') node.className = value;
    else if (key === 'text') node.textContent = value;
    else if (key === 'dataset') Object.assign(node.dataset, value);
    else if (key === 'style') Object.assign(node.style, value);
    else if (key === 'value') node.value = value;
    else if (key === 'checked') node.checked = Boolean(value);
    else if (key.startsWith('on') && typeof value === 'function') {
      node.addEventListener(key.slice(2).toLowerCase(), value);
    } else if (key === 'attrs') {
      for (const [a, v] of Object.entries(value)) {
        if (v !== null && v !== undefined) node.setAttribute(a, String(v));
      }
    } else if (value === true) node.setAttribute(key, '');
    else if (value !== false) node.setAttribute(key, String(value));
  }
  append(node, children);
  return node;
}

export function append(node, children) {
  for (const child of children.flat(4)) {
    if (child === null || child === undefined || child === false) continue;
    node.appendChild(child instanceof Node ? child : document.createTextNode(String(child)));
  }
  return node;
}

export function clear(node) {
  while (node.firstChild) node.removeChild(node.firstChild);
  return node;
}

export function svg(path, size = 16, strokeWidth = 1.6) {
  const ns = 'http://www.w3.org/2000/svg';
  const el = document.createElementNS(ns, 'svg');
  el.setAttribute('viewBox', '0 0 24 24');
  el.setAttribute('width', String(size));
  el.setAttribute('height', String(size));
  const p = document.createElementNS(ns, 'path');
  p.setAttribute('d', path);
  p.setAttribute('fill', 'none');
  p.setAttribute('stroke', 'currentColor');
  p.setAttribute('stroke-width', String(strokeWidth));
  p.setAttribute('stroke-linecap', 'round');
  p.setAttribute('stroke-linejoin', 'round');
  el.appendChild(p);
  return el;
}

/** Aviso flotante. */
export function toast(message, type = 'info', timeout = 3600) {
  const container = document.getElementById('toasts');
  if (!container) {
    console.warn(message);
    return;
  }
  const node = h('div', { class: `toast toast--${type}`, text: message });
  container.appendChild(node);
  setTimeout(() => {
    node.style.opacity = '0';
    node.style.transition = 'opacity 0.2s';
    setTimeout(() => node.remove(), 220);
  }, timeout);
  return node;
}

/** Abre un <dialog> de forma segura. */
export function openDialog(id) {
  const dialog = document.getElementById(id);
  if (!dialog) return null;
  if (typeof dialog.showModal === 'function') {
    if (!dialog.open) dialog.showModal();
  } else {
    dialog.setAttribute('open', '');
  }
  return dialog;
}

export function closeDialog(id) {
  const dialog = document.getElementById(id);
  if (!dialog) return;
  if (typeof dialog.close === 'function' && dialog.open) dialog.close();
  else dialog.removeAttribute('open');
}

/** Limita un valor y lo redondea a pasos. */
export function clampStep(value, min, max, step = 0) {
  let v = Math.min(max, Math.max(min, value));
  if (step > 0) v = Math.round(v / step) * step;
  return v;
}

/**
 * Dibuja un medidor horizontal con memoria de pico.
 * @param {HTMLCanvasElement} canvas
 * @param {number} db nivel en dBFS
 * @param {{hold?: number, clip?: boolean, segments?: boolean}} [options]
 */
export function drawMeter(canvas, db, options = {}) {
  const ctx = canvas.getContext('2d');
  const dpr = window.devicePixelRatio || 1;
  // El tamaño en CSS se fija una vez: si solo se cambia canvas.width, el
  // elemento crece en cada fotograma (el atributo define el tamaño sin CSS).
  const cssWidth = canvas.dataset.cssWidth ? Number(canvas.dataset.cssWidth) : canvas.clientWidth || canvas.width || 220;
  const cssHeight = canvas.dataset.cssHeight ? Number(canvas.dataset.cssHeight) : canvas.clientHeight || canvas.height || 16;
  canvas.dataset.cssWidth = String(cssWidth);
  canvas.dataset.cssHeight = String(cssHeight);
  canvas.style.width = `${cssWidth}px`;
  canvas.style.height = `${cssHeight}px`;
  if (canvas.width !== Math.round(cssWidth * dpr) || canvas.height !== Math.round(cssHeight * dpr)) {
    canvas.width = Math.round(cssWidth * dpr);
    canvas.height = Math.round(cssHeight * dpr);
  }
  const w = canvas.width;
  const h = canvas.height;
  ctx.clearRect(0, 0, w, h);

  ctx.fillStyle = '#0b0e10';
  ctx.fillRect(0, 0, w, h);

  // Escala: de -60 dB a 0 dB (más un pequeño tramo para el recorte).
  const minDb = -60;
  const toX = (value) => Math.max(0, Math.min(1, (value - minDb) / (0 - minDb))) * w;
  const level = Number.isFinite(db) ? Math.max(minDb, Math.min(6, db)) : minDb;
  const x = toX(level);

  const gradient = ctx.createLinearGradient(0, 0, w, 0);
  gradient.addColorStop(0, '#3f6b4a');
  gradient.addColorStop(0.62, '#6f9c74');
  gradient.addColorStop(0.8, '#c2a45a');
  gradient.addColorStop(1, '#c05a4c');
  ctx.fillStyle = gradient;
  ctx.fillRect(0, 0, x, h);

  // Marcas de referencia cada 12 dB.
  ctx.fillStyle = 'rgba(255,255,255,0.08)';
  for (let d = -48; d <= 0; d += 12) {
    const markX = Math.round(toX(d));
    ctx.fillRect(markX, 0, Math.max(1, dpr), h);
  }

  if (options.hold !== undefined && Number.isFinite(options.hold)) {
    const hx = toX(options.hold);
    ctx.fillStyle = options.hold > -1 ? '#d96a58' : 'rgba(220,230,238,0.75)';
    ctx.fillRect(Math.min(w - 2 * dpr, hx), 0, 2 * dpr, h);
  }

  if (options.clip || level >= -0.05) {
    ctx.fillStyle = '#d96a58';
    ctx.fillRect(w - 6 * dpr, 0, 6 * dpr, h);
  }
}

/** Dibuja un medidor vertical estrecho (para la tira de pista). */
export function drawVerticalMeter(canvas, db, holdDb = -Infinity) {
  const ctx = canvas.getContext('2d');
  const dpr = window.devicePixelRatio || 1;
  const cssWidth = canvas.dataset.cssWidth ? Number(canvas.dataset.cssWidth) : canvas.clientWidth || 10;
  const cssHeight = canvas.dataset.cssHeight ? Number(canvas.dataset.cssHeight) : canvas.clientHeight || 60;
  canvas.dataset.cssWidth = String(cssWidth);
  canvas.dataset.cssHeight = String(cssHeight);
  canvas.style.width = `${cssWidth}px`;
  canvas.style.height = `${cssHeight}px`;
  if (canvas.width !== Math.round(cssWidth * dpr) || canvas.height !== Math.round(cssHeight * dpr)) {
    canvas.width = Math.round(cssWidth * dpr);
    canvas.height = Math.round(cssHeight * dpr);
  }
  const w = canvas.width;
  const ht = canvas.height;
  ctx.clearRect(0, 0, w, ht);
  ctx.fillStyle = '#0b0e10';
  ctx.fillRect(0, 0, w, ht);

  const minDb = -60;
  const value = Number.isFinite(db) ? Math.max(minDb, Math.min(0, db)) : minDb;
  const ratio = (value - minDb) / (0 - minDb);
  const barHeight = ratio * ht;
  const gradient = ctx.createLinearGradient(0, ht, 0, 0);
  gradient.addColorStop(0, '#3f6b4a');
  gradient.addColorStop(0.72, '#6f9c74');
  gradient.addColorStop(0.9, '#c2a45a');
  gradient.addColorStop(1, '#c05a4c');
  ctx.fillStyle = gradient;
  ctx.fillRect(0, ht - barHeight, w, barHeight);

  if (Number.isFinite(holdDb) && holdDb > minDb) {
    const holdRatio = (Math.min(0, holdDb) - minDb) / (0 - minDb);
    ctx.fillStyle = holdDb > -1 ? '#d96a58' : 'rgba(220,230,238,0.7)';
    ctx.fillRect(0, ht - holdRatio * ht - 2 * dpr, w, 2 * dpr);
  }
}

/**
 * Control deslizante con etiqueta y valor formateado.
 * @param {{label:string, min:number, max:number, step?:number, value:number,
 *          format?:Function, onChange:Function, id?:string}} options
 */
export function sliderControl(options) {
  const format = options.format ?? ((v) => String(v));
  const valueLabel = h('span', { class: 'control__value', text: format(options.value) });
  const input = h('input', {
    type: 'range',
    min: options.min,
    max: options.max,
    step: options.step ?? 0.01,
    value: options.value,
    attrs: { 'aria-label': options.label },
    oninput: (event) => {
      const value = Number(event.target.value);
      valueLabel.textContent = format(value);
      options.onChange(value);
    },
  });
  return h(
    'div',
    { class: 'control' },
    h('div', { class: 'control__head' }, h('span', { class: 'control__label', text: options.label }), valueLabel),
    input
  );
}

/** Interruptor on/off. */
export function switchControl({ label, value, onChange, id }) {
  const button = h('button', {
    type: 'button',
    class: 'switch',
    attrs: { 'aria-pressed': String(Boolean(value)), 'aria-label': label },
    onclick: () => {
      const next = button.getAttribute('aria-pressed') !== 'true';
      button.setAttribute('aria-pressed', String(next));
      onChange(next);
    },
  });
  if (id) button.id = id;
  return button;
}

/** Sección plegable del inspector. */
export function fxSection({ title, enabled, onToggle, children, open = false }) {
  const section = h('section', { class: `fx-section${open ? ' is-open' : ''}` });
  const body = h('div', { class: 'fx-section__body' }, ...children);
  const toggle = h('button', {
    type: 'button',
    class: 'fx-section__toggle',
    attrs: { 'aria-label': 'Mostrar u ocultar' },
    text: '▾',
    onclick: () => section.classList.toggle('is-open'),
  });
  const head = h(
    'div',
    { class: 'fx-section__head' },
    h('span', { class: 'fx-section__title', text: title }),
    enabled !== undefined
      ? switchControl({ label: `Activar ${title}`, value: enabled, onChange: onToggle ?? (() => {}) })
      : null,
    toggle
  );
  section.append(head, body);
  return section;
}

/** Opciones para un <select>. */
export function fillSelect(select, options, selectedValue) {
  clear(select);
  for (const option of options) {
    const node = h('option', { value: option.value, text: option.label });
    if (String(option.value) === String(selectedValue)) node.selected = true;
    select.appendChild(node);
  }
}

/** ¿Estamos dentro de la app Android? (el contenedor nativo inyecta `AskNative`). */
export function isNativeApp() {
  return Boolean(typeof window !== 'undefined' && window.AskNative && window.AskNative.isAndroid?.());
}

/** Convierte bytes a base64 sin reventar la pila con archivos grandes. */
function bytesToBase64(bytes) {
  if (typeof btoa !== 'function') throw new Error('btoa no disponible');
  let binary = '';
  const step = 0x8000;
  for (let index = 0; index < bytes.length; index += step) {
    binary += String.fromCharCode.apply(null, bytes.subarray(index, index + step));
  }
  return btoa(binary);
}

/**
 * Guarda un Blob como archivo.
 * - En la app Android lo escribe el código nativo en Descargas/Ask,
 *   porque las descargas del navegador no existen dentro de un WebView.
 * - En un navegador usa la descarga normal.
 * Devuelve una promesa (se puede ignorar: los botones no esperan).
 */
let saveQueue = Promise.resolve();

export function downloadBlob(blob, filename) {
  if (isNativeApp()) {
    // En el teléfono los archivos se escriben de uno en uno: si se exportan
    // varios stems seguidos, no pueden pisarse entre ellos.
    saveQueue = saveQueue.then(() => saveToAndroid(blob, filename)).catch(() => {});
    return saveQueue;
  }

  const url = URL.createObjectURL(blob);
  const link = h('a', { href: url, download: filename, style: { display: 'none' } });
  document.body.appendChild(link);
  link.click();
  setTimeout(() => {
    link.remove();
    URL.revokeObjectURL(url);
  }, 4000);
  return Promise.resolve(filename);
}

/** Escribe el archivo en el almacenamiento del teléfono a través del puente nativo. */
async function saveToAndroid(blob, filename) {
  const bridge = window.AskNative;
  const safeName = String(filename || 'ask-export').replace(/[\\/:*?"<>|]/g, '-');
  const mime = blob.type || (/\.wav$/i.test(safeName) ? 'audio/wav' : 'audio/mpeg');

  if (!bridge.start(safeName, mime)) {
    toast('No se pudo crear el archivo en el teléfono', 'error', 8000);
    throw new Error('El almacenamiento del teléfono rechazó el archivo');
  }

  try {
    const CHUNK = 512 * 1024;
    for (let offset = 0; offset < blob.size; offset += CHUNK) {
      const slice = blob.slice(offset, offset + CHUNK);
      const buffer = await slice.arrayBuffer();
      bridge.chunk(bytesToBase64(new Uint8Array(buffer)));
      // Cede el hilo para que la interfaz siga respondiendo en archivos grandes.
      await new Promise((resolve) => setTimeout(resolve, 0));
    }
    const location = bridge.finish();
    toast(`Guardado en ${location}`, 'ok', 7000);
    return location;
  } catch (error) {
    bridge.abort();
    toast(`No se pudo guardar el archivo: ${error.message}`, 'error', 8000);
    throw error;
  }
}

/** Escapa texto para insertarlo en HTML (por si se usa innerHTML con contenido dinámico). */
export function escapeHtml(text) {
  return String(text).replace(/[&<>"']/g, (char) => ({
    '&': '&amp;',
    '<': '&lt;',
    '>': '&gt;',
    '"': '&quot;',
    "'": '&#39;',
  })[char]);
}
