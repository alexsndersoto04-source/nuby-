/**
 * Medición de sonoridad según ITU-R BS.1770-4 (LUFS) y True Peak aproximado.
 *
 * Un medidor "de verdad" no es un vúmetro: aplica el filtro de ponderación K
 * (dos etapas: estante de agudos + paso alto), integra bloques de 400 ms con
 * solapamiento del 75 % y aplica las puertas absoluta (-70 LUFS) y relativa
 * (-10 LU) que define la norma.
 *
 * Módulo independiente del navegador para poder probarlo en Node.
 * Los coeficientes siguen la formulación de pyloudnorm / ITU para cualquier
 * frecuencia de muestreo.
 */

const ABSOLUTE_GATE = -70; // LUFS
const RELATIVE_GATE = -10; // LU

/**
 * Coeficientes del filtro de ponderación K para una frecuencia dada.
 * @param {number} sampleRate
 * @returns {{shelf: {b: number[], a: number[]}, highpass: {b: number[], a: number[]}}}
 */
export function kWeightingCoefficients(sampleRate) {
  if (!(sampleRate > 0)) throw new Error('sampleRate inválido');
  return {
    shelf: highShelf(1681.974450955533, 3.999843853973347, 0.7071752369554196, sampleRate),
    highpass: highPass(38.13547087602444, 0.5003270373238773, sampleRate),
  };
}

function highShelf(f0, gainDb, Q, fs) {
  const K = Math.tan((Math.PI * f0) / fs);
  const Vh = Math.pow(10, gainDb / 20);
  const Vb = Math.pow(Vh, 0.4996667741545416);
  const a0 = 1 + K / Q + K * K;
  return {
    b: [
      (Vh + (Vb * K) / Q + K * K) / a0,
      (2 * (K * K - Vh)) / a0,
      (Vh - (Vb * K) / Q + K * K) / a0,
    ],
    a: [1, (2 * (K * K - 1)) / a0, (1 - K / Q + K * K) / a0],
  };
}

function highPass(f0, Q, fs) {
  const K = Math.tan((Math.PI * f0) / fs);
  const a0 = 1 + K / Q + K * K;
  return {
    b: [1 / a0, -2 / a0, 1 / a0],
    a: [1, (2 * (K * K - 1)) / a0, (1 - K / Q + K * K) / a0],
  };
}

/** Aplica un biquad en cascada (transposed direct form II) sobre una señal. */
export function applyBiquad(input, coeffs, state) {
  const { b, a } = coeffs;
  const out = new Float32Array(input.length);
  let z1 = state?.z1 ?? 0;
  let z2 = state?.z2 ?? 0;
  for (let i = 0; i < input.length; i++) {
    const x = input[i];
    const y = b[0] * x + z1;
    z1 = b[1] * x - a[1] * y + z2;
    z2 = b[2] * x - a[2] * y;
    out[i] = y;
  }
  if (state) {
    state.z1 = z1;
    state.z2 = z2;
  }
  return out;
}

/** Aplica la ponderación K completa a un canal. */
export function kWeight(input, sampleRate) {
  const { shelf, highpass } = kWeightingCoefficients(sampleRate);
  return applyBiquad(applyBiquad(input, shelf), highpass);
}

/**
 * Calcula la sonoridad integrada (LUFS) de un conjunto de canales.
 * @param {Float32Array[]} channels
 * @param {number} sampleRate
 * @returns {number} LUFS (o -Infinity si el material está en silencio)
 */
export function integratedLoudness(channels, sampleRate) {
  if (!channels.length) return -Infinity;
  const weighted = channels.map((ch) => kWeight(ch, sampleRate));
  const blockSize = Math.round(0.4 * sampleRate);
  const hop = Math.round(blockSize / 4); // solapamiento del 75 %
  const numFrames = weighted[0].length;

  const blockLoudness = [];
  if (numFrames < blockSize) {
    // Material más corto que un bloque: se mide como un único bloque rellenado con ceros.
    const z = sumSquares(weighted, 0, numFrames, blockSize);
    blockLoudness.push(z > 0 ? -0.691 + 10 * Math.log10(z) : -Infinity);
  } else {
    for (let start = 0; start + blockSize <= numFrames; start += hop) {
      const z = sumSquares(weighted, start, blockSize, blockSize);
      blockLoudness.push(z > 0 ? -0.691 + 10 * Math.log10(z) : -Infinity);
    }
  }
  return gatedLoudness(blockLoudness);
}

/**
 * Suma de la energía media de todos los canales en una ventana.
 * Normaliza siempre por `nominalLength` (el material ausente cuenta como silencio).
 */
function sumSquares(channels, start, length, nominalLength) {
  const end = Math.min(start + length, channels[0].length);
  let total = 0;
  for (const ch of channels) {
    for (let i = start; i < end; i++) total += ch[i] * ch[i];
  }
  // BS.1770: z = Σ_canales (1/T ∫ y²). No se divide por el número de canales.
  return total / nominalLength;
}

/**
 * Aplica las puertas de BS.1770 sobre la lista de sonoridades por bloque.
 * @param {number[]} blockLoudness LUFS de cada bloque
 * @returns {number}
 */
export function gatedLoudness(blockLoudness) {
  const finite = blockLoudness.map((v) => (Number.isFinite(v) ? v : -Infinity));
  const aboveAbsolute = finite.filter((v) => v > ABSOLUTE_GATE);
  if (!aboveAbsolute.length) return -Infinity;
  const meanAbove = aboveAbsolute.reduce((a, b) => a + Math.pow(10, b / 10), 0) / aboveAbsolute.length;
  const relativeThreshold = -0.691 + 10 * Math.log10(meanAbove) + RELATIVE_GATE;
  const threshold = Math.max(ABSOLUTE_GATE, relativeThreshold);
  const gated = finite.filter((v) => v > threshold);
  if (!gated.length) return -Infinity;
  const mean = gated.reduce((a, b) => a + Math.pow(10, b / 10), 0) / gated.length;
  return -0.691 + 10 * Math.log10(mean);
}

/** Nivel de pico muestral en dBFS. */
export function peakDb(channels) {
  let peak = 0;
  for (const ch of channels) {
    for (let i = 0; i < ch.length; i++) {
      const v = Math.abs(ch[i]);
      if (v > peak) peak = v;
    }
  }
  return peak > 0 ? 20 * Math.log10(peak) : -Infinity;
}

/**
 * True Peak aproximado por sobremuestreo de 4x con interpolación lineal.
 * No sustituye a un medidor con filtro de reconstrucción, pero detecta
 * intermuestras por encima de 0 dBFS bastante mejor que el pico muestral.
 */
export function truePeakDb(channels, factor = 4) {
  let peak = 0;
  for (const ch of channels) {
    for (let i = 0; i < ch.length - 1; i++) {
      const a = ch[i];
      const b = ch[i + 1];
      if (Math.abs(a) > peak) peak = Math.abs(a);
      for (let k = 1; k < factor; k++) {
        const v = Math.abs(a + ((b - a) * k) / factor);
        if (v > peak) peak = v;
      }
    }
  }
  return peak > 0 ? 20 * Math.log10(peak) : -Infinity;
}

/** Nivel RMS en dBFS. */
export function rmsDb(channels) {
  let sum = 0;
  let count = 0;
  for (const ch of channels) {
    for (let i = 0; i < ch.length; i++) {
      sum += ch[i] * ch[i];
      count++;
    }
  }
  if (!count || sum <= 0) return -Infinity;
  return 10 * Math.log10(sum / count);
}
