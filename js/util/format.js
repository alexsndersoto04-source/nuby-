/**
 * Utilidades de formato numérico y de tiempo.
 * Módulo sin dependencias del navegador: se puede probar en Node.
 */

/** Convierte una duración en segundos a "m:ss.cc". */
export function formatTime(seconds, showCentis = true) {
  if (!Number.isFinite(seconds)) seconds = 0;
  const negative = seconds < 0;
  let s = Math.abs(seconds);
  const m = Math.floor(s / 60);
  s -= m * 60;
  const sec = Math.floor(s);
  const cs = Math.floor((s - sec) * 100);
  const base = `${negative ? '-' : ''}${m}:${String(sec).padStart(2, '0')}`;
  return showCentis ? `${base}.${String(cs).padStart(2, '0')}` : base;
}

/** Convierte un valor lineal de amplitud a decibelios (-Infinity si es 0). */
export function toDb(linear) {
  if (linear <= 0) return -Infinity;
  return 20 * Math.log10(linear);
}

/** Convierte decibelios a amplitud lineal. */
export function fromDb(db) {
  if (db === -Infinity) return 0;
  return Math.pow(10, db / 20);
}

/** Formatea decibelios para mostrar: "−12.3 dB", "−∞". */
export function formatDb(db, digits = 1) {
  if (!Number.isFinite(db)) return '-∞ dB';
  const v = db.toFixed(digits);
  return `${Number(v) > 0 ? '+' : ''}${v} dB`;
}

/** Formatea un número con separación de miles. */
export function formatNumber(n) {
  return new Intl.NumberFormat('es-VE').format(n);
}

/** Formatea bytes de forma legible. */
export function formatBytes(bytes) {
  if (bytes < 1024) return `${bytes} B`;
  const units = ['KB', 'MB', 'GB'];
  let v = bytes / 1024;
  let i = 0;
  while (v >= 1024 && i < units.length - 1) {
    v /= 1024;
    i++;
  }
  return `${v.toFixed(v >= 10 ? 0 : 1)} ${units[i]}`;
}

/**
 * Curva del fader: posición 0..1 -> ganancia lineal.
 * Igual que en una consola: 0.8 del recorrido es 0 dB (unidad), el máximo da
 * +6 dB y por debajo de 0.8 la escala es logarítmica hasta -60 dB (silencio).
 */
const FADER_UNITY = 0.8;
const FADER_MIN_DB = -60;
const FADER_MAX_DB = 6;

export function faderToGain(position) {
  const db = faderToDb(position);
  if (db === -Infinity) return 0;
  return fromDb(db);
}

/** Inversa de faderToGain. */
export function gainToFader(gain) {
  if (gain <= 0) return 0;
  const db = toDb(gain);
  if (db < 0) return Math.max(0, Math.min(1, ((db - FADER_MIN_DB) / -FADER_MIN_DB) * FADER_UNITY));
  return Math.min(1, FADER_UNITY + (db / FADER_MAX_DB) * (1 - FADER_UNITY));
}

/** dB de una posición del fader (para las etiquetas de la consola). */
export function faderToDb(position) {
  if (position <= 0) return -Infinity;
  if (position <= FADER_UNITY) {
    return FADER_MIN_DB + (position / FADER_UNITY) * -FADER_MIN_DB;
  }
  return ((position - FADER_UNITY) / (1 - FADER_UNITY)) * FADER_MAX_DB;
}

/** Redondea al múltiplo más cercano. */
export function snapTo(value, step) {
  if (!step || step <= 0) return value;
  return Math.round(value / step) * step;
}

/** Limita un número entre min y max. */
export function clamp(v, min, max) {
  return v < min ? min : v > max ? max : v;
}

/** Identificador corto único. */
export function uid(prefix = 'id') {
  const rnd = Math.random().toString(36).slice(2, 8);
  return `${prefix}_${Date.now().toString(36)}_${rnd}`;
}

/** Nombre de archivo seguro (sin acentos ni caracteres raros). */
export function safeFileName(name, fallback = 'mezcla') {
  const clean = (name || '')
    .normalize('NFD')
    .replace(/[\u0300-\u036f]/g, '')
    .replace(/[^a-zA-Z0-9\-_ ]+/g, '')
    .trim()
    .replace(/\s+/g, '-');
  return clean || fallback;
}
