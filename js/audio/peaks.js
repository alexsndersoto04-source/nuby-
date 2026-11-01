/**
 * Cálculo de picos (mín./máx. por bloque) para dibujar formas de onda.
 * Se calcula una sola vez por clip y se guarda en caché: dibujar miles de
 * muestras por fotograma sería inviable.
 */

/**
 * @param {Float32Array[]} channels
 * @param {number} bucketFrames muestras por bloque de dibujo
 * @returns {{min: Float32Array, max: Float32Array, length: number, peak: number}}
 */
export function computePeaks(channels, bucketFrames = 512) {
  const numFrames = channels[0]?.length ?? 0;
  const bucket = Math.max(1, Math.floor(bucketFrames));
  const length = Math.max(1, Math.ceil(numFrames / bucket));
  const min = new Float32Array(length);
  const max = new Float32Array(length);
  let peak = 0;

  for (let b = 0; b < length; b++) {
    const start = b * bucket;
    const end = Math.min(start + bucket, numFrames);
    let lo = 0;
    let hi = 0;
    if (numFrames) {
      lo = Infinity;
      hi = -Infinity;
      for (const ch of channels) {
        for (let i = start; i < end; i++) {
          const v = ch[i];
          if (v < lo) lo = v;
          if (v > hi) hi = v;
        }
      }
      if (!Number.isFinite(lo)) lo = 0;
      if (!Number.isFinite(hi)) hi = 0;
    }
    min[b] = lo;
    max[b] = hi;
    const abs = Math.max(Math.abs(lo), Math.abs(hi));
    if (abs > peak) peak = abs;
  }

  return { min, max, length, peak, bucket };
}

/**
 * Nivel de pico de un AudioBuffer.
 * @param {AudioBuffer} buffer
 */
export function bufferPeak(buffer) {
  let peak = 0;
  for (let c = 0; c < buffer.numberOfChannels; c++) {
    const data = buffer.getChannelData(c);
    for (let i = 0; i < data.length; i++) {
      const v = Math.abs(data[i]);
      if (v > peak) peak = v;
    }
  }
  return peak;
}

/**
 * Detecta regiones con señal por encima de un umbral (para recortar silencios).
 * @param {AudioBuffer} buffer
 * @param {number} thresholdDb
 * @returns {{start: number, end: number}} en segundos
 */
export function detectSignalRegion(buffer, thresholdDb = -50) {
  const threshold = Math.pow(10, thresholdDb / 20);
  const length = buffer.length;
  const channels = [];
  for (let c = 0; c < buffer.numberOfChannels; c++) channels.push(buffer.getChannelData(c));
  let first = -1;
  let last = -1;
  for (let i = 0; i < length; i++) {
    let v = 0;
    for (let c = 0; c < channels.length; c++) {
      const s = Math.abs(channels[c][i]);
      if (s > v) v = s;
    }
    if (v > threshold) {
      if (first < 0) first = i;
      last = i;
    }
  }
  if (first < 0) return { start: 0, end: buffer.duration };
  // Un pequeño margen para no cortar el ataque ni la cola.
  const margin = Math.round(0.02 * buffer.sampleRate);
  return {
    start: Math.max(0, (first - margin) / buffer.sampleRate),
    end: Math.min(buffer.duration, (last + margin) / buffer.sampleRate),
  };
}
