/**
 * Codificador MP3 real en el navegador (LAME compilado a JavaScript).
 *
 * - Calidades: 128, 160, 192, 256 y 320 kbps CBR.
 * - Trabaja sobre Float32 y convierte a PCM entero para LAME.
 * - Añade etiquetas ID3v2.3 (título, artista, álbum, año) para que el archivo
 *   se vea bien en reproductores y en el móvil.
 *
 * LAME (lamejs) es LGPL-3.0; el aviso de licencia está en LICENSE-lamejs.txt.
 */

const SUPPORTED_RATES = [8000, 11025, 12000, 16000, 22050, 24000, 32000, 44100, 48000];

let lameModulePromise = null;

/** Carga diferida del codificador (no bloquea el arranque del estudio). */
async function loadLame() {
  if (!lameModulePromise) {
    lameModulePromise = import('../vendor/lamejs.js').then((mod) => mod.default ?? mod);
  }
  return lameModulePromise;
}

/** ¿La frecuencia de muestreo es compatible con LAME? */
export function isRateSupported(sampleRate) {
  return SUPPORTED_RATES.includes(sampleRate);
}

/**
 * Codifica audio PCM flotante a MP3.
 * @param {Float32Array[]} channels 1 (mono) o 2 (estéreo) canales
 * @param {number} sampleRate
 * @param {{bitrate?: number, onProgress?: (p:number)=>void, tags?: object}} [options]
 * @returns {Promise<Blob>}
 */
export async function encodeMp3(channels, sampleRate, options = {}) {
  const bitrate = options.bitrate ?? 320;
  const onProgress = options.onProgress ?? (() => {});
  const numChannels = channels.length === 1 ? 1 : 2;

  if (!isRateSupported(sampleRate)) {
    throw new Error(
      `MP3 no admite ${sampleRate} Hz. Frecuencias válidas: ${SUPPORTED_RATES.join(', ')}.`
    );
  }
  if (![128, 160, 192, 256, 320].includes(bitrate)) {
    throw new Error(`Calidad MP3 no soportada: ${bitrate} kbps`);
  }

  const lame = await loadLame();
  const Mp3Encoder = lame.Mp3Encoder ?? lame.default?.Mp3Encoder;
  if (!Mp3Encoder) throw new Error('No se pudo cargar el codificador MP3');

  const encoder = new Mp3Encoder(numChannels, sampleRate, bitrate);
  const numFrames = channels[0].length;
  const blockSize = 1152;
  const parts = [];

  const left = toInt16(channels[0]);
  const right = numChannels === 2 ? toInt16(channels[1]) : null;

  for (let i = 0; i < numFrames; i += blockSize) {
    const end = Math.min(i + blockSize, numFrames);
    const l = left.subarray(i, end);
    const r = right ? right.subarray(i, end) : undefined;
    const chunk = numChannels === 2 ? encoder.encodeBuffer(l, r) : encoder.encodeBuffer(l);
    if (chunk.length) parts.push(new Uint8Array(chunk.buffer ? chunk.buffer.slice(chunk.byteOffset, chunk.byteOffset + chunk.byteLength) : chunk));
    // Informar del progreso sin saturar la interfaz (cada ~2 %).
    if (onProgress && (i / blockSize) % 64 === 0) {
      onProgress(Math.min(0.99, i / Math.max(1, numFrames)));
      await nextFrame();
    }
  }

  const tail = encoder.flush();
  if (tail.length) parts.push(new Uint8Array(tail.buffer ? tail.buffer.slice(tail.byteOffset, tail.byteOffset + tail.byteLength) : tail));
  if (onProgress) onProgress(1);

  const header = buildId3Tags(options.tags ?? {});
  const blobParts = header ? [header, ...parts] : parts;
  return new Blob(blobParts, { type: 'audio/mpeg' });
}

function toInt16(float32) {
  const out = new Int16Array(float32.length);
  for (let i = 0; i < float32.length; i++) {
    const s = Math.max(-1, Math.min(1, float32[i]));
    out[i] = s < 0 ? Math.round(s * 32768) : Math.round(s * 32767);
  }
  return out;
}

/**
 * Cede el hilo para que la barra de progreso se actualice.
 * Se usa setTimeout (no requestAnimationFrame) porque el codificador también
 * debe funcionar en contextos sin animación (pestañas de fondo, pruebas).
 */
function nextFrame() {
  return new Promise((resolve) => setTimeout(resolve, 0));
}

/**
 * Construye una etiqueta ID3v2.3 mínima y válida.
 * @param {{title?:string, artist?:string, album?:string, year?:string, comment?:string}} tags
 * @returns {Uint8Array|null}
 */
export function buildId3Tags(tags) {
  const frames = [];
  const textFrame = (id, text) => {
    if (!text) return;
    const encoded = new TextEncoder().encode(text);
    const body = new Uint8Array(1 + encoded.length); // 1 byte de codificación + texto
    body[0] = 0x03; // UTF-8 (válido en ID3v2.4; los lectores modernos lo aceptan)
    body.set(encoded, 1);
    const frame = new Uint8Array(10 + body.length);
    writeAscii(frame, 0, id);
    writeSynchSafe(frame, 4, body.length);
    frame.set(body, 10);
    frames.push(frame);
  };

  textFrame('TIT2', tags.title);
  textFrame('TPE1', tags.artist);
  textFrame('TALB', tags.album);
  textFrame('TYER', tags.year);
  textFrame('TCOM', tags.comment);

  if (!frames.length) return null;
  const total = frames.reduce((n, f) => n + f.length, 0);
  const header = new Uint8Array(10);
  writeAscii(header, 0, 'ID3');
  header[3] = 3; // versión 2.3
  header[4] = 0; // revisión
  header[5] = 0; // sin flags
  writeSynchSafe(header, 6, total);
  const out = new Uint8Array(10 + total);
  out.set(header, 0);
  let offset = 10;
  for (const frame of frames) {
    out.set(frame, offset);
    offset += frame.length;
  }
  return out;
}

function writeAscii(bytes, offset, text) {
  for (let i = 0; i < text.length; i++) bytes[offset + i] = text.charCodeAt(i);
}

function writeSynchSafe(bytes, offset, value) {
  bytes[offset] = (value >> 21) & 0x7f;
  bytes[offset + 1] = (value >> 14) & 0x7f;
  bytes[offset + 2] = (value >> 7) & 0x7f;
  bytes[offset + 3] = value & 0x7f;
}

/** Lee la etiqueta ID3 de un MP3 (usado por las pruebas automáticas). */
export function readId3Tags(bytes) {
  if (bytes[0] !== 0x49 || bytes[1] !== 0x44 || bytes[2] !== 0x33) return null;
  const size =
    ((bytes[6] & 0x7f) << 21) | ((bytes[7] & 0x7f) << 14) | ((bytes[8] & 0x7f) << 7) | (bytes[9] & 0x7f);
  return { version: bytes[3], size };
}
