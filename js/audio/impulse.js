/**
 * Generación de respuestas al impulso (IR) para la reverberación por convolución.
 *
 * No son samples de reverberación descargados: se sintetizan en tiempo real con
 * ruido decorrelacionado, envolvente exponencial, reflexiones tempranas y
 * amortiguación de agudos progresiva, que es como funcionan los generadores de
 * IR de las DAW. El resultado se convoluciona de verdad con el audio.
 */

/**
 * @typedef {Object} ImpulseOptions
 * @property {number} [seconds] duración de la cola
 * @property {number} [decay] 0..1; más alto = caída más lenta
 * @property {number} [preDelay] segundos antes de la primera reflexión
 * @property {number} [brightness] 0..1; amortiguación de agudos
 * @property {number} [width] 0..1; decorrelación entre canales
 * @property {number} [earlyReflections] cantidad de reflexiones tempranas
 */

/**
 * Crea un AudioBuffer estéreo con la IR.
 * @param {BaseAudioContext} ctx
 * @param {ImpulseOptions} options
 */
export function createImpulseResponse(ctx, options = {}) {
  const seconds = options.seconds ?? 2.2;
  const decay = options.decay ?? 0.6;
  const preDelay = options.preDelay ?? 0.02;
  const brightness = options.brightness ?? 0.55;
  const width = options.width ?? 0.85;
  const earlyCount = options.earlyReflections ?? 8;

  const sampleRate = ctx.sampleRate;
  const length = Math.max(1, Math.floor(seconds * sampleRate));
  const buffer = ctx.createBuffer(2, length, sampleRate);

  // Semilla determinista para que la mezcla exportada suene igual que en vivo.
  let seed = 1234567;
  const random = () => {
    seed = (seed * 1103515245 + 12345) & 0x7fffffff;
    return (seed / 0x7fffffff) * 2 - 1;
  };

  const preDelayFrames = Math.floor(preDelay * sampleRate);
  const decayPower = 1.6 + decay * 4.4;

  for (let channel = 0; channel < 2; channel++) {
    const data = buffer.getChannelData(channel);
    // Filtro paso bajo de una muesca para oscurecer la cola progresivamente.
    let lp = 0;
    const lpCoeff = 0.15 + (1 - brightness) * 0.7;

    for (let i = preDelayFrames; i < length; i++) {
      const t = (i - preDelayFrames) / (length - preDelayFrames);
      const envelope = Math.pow(1 - t, decayPower);
      const noise = random();
      lp = lp + lpCoeff * (noise - lp);
      // Poca correlación entre canales: la cola debe envolver, no venir de un punto.
      const channelTilt = channel === 0 ? 1 : 1 - width * 0.35;
      data[i] = lp * envelope * channelTilt * 0.6;
    }

    // Reflexiones tempranas discretas: dan sensación de tamaño y "paredes".
    for (let e = 0; e < earlyCount; e++) {
      const time = preDelay + ((e + 1) / earlyCount) * Math.min(0.09, seconds * 0.3);
      const index = Math.floor((time + Math.abs(random()) * 0.004) * sampleRate);
      if (index < length) {
        const amplitude = (0.55 / (e + 1)) * (channel === 0 ? 1 : 0.92);
        data[index] += amplitude * (random() < 0 ? -1 : 1);
      }
    }
  }

  // Normalizar la IR para que el nivel de la cola dependa del envío, no de la IR.
  let max = 0;
  for (let c = 0; c < 2; c++) {
    const data = buffer.getChannelData(c);
    for (let i = 0; i < length; i++) max = Math.max(max, Math.abs(data[i]));
  }
  if (max > 0) {
    const norm = 0.85 / max;
    for (let c = 0; c < 2; c++) {
      const data = buffer.getChannelData(c);
      for (let i = 0; i < length; i++) data[i] *= norm;
    }
  }

  return buffer;
}

/** Ajustes con nombre para el selector de sala. */
export const REVERB_PRESETS = {
  vocal: { label: 'Voz natural', seconds: 1.5, decay: 0.45, preDelay: 0.018, brightness: 0.62, width: 0.9, earlyReflections: 6 },
  room: { label: 'Sala pequeña', seconds: 1.1, decay: 0.35, preDelay: 0.008, brightness: 0.55, width: 0.7, earlyReflections: 10 },
  hall: { label: 'Sala grande', seconds: 3.4, decay: 0.72, preDelay: 0.035, brightness: 0.42, width: 1, earlyReflections: 12 },
  plate: { label: 'Placa', seconds: 2.4, decay: 0.6, preDelay: 0.004, brightness: 0.8, width: 0.55, earlyReflections: 4 },
  ambient: { label: 'Ambiente', seconds: 5.2, decay: 0.85, preDelay: 0.05, brightness: 0.3, width: 1, earlyReflections: 3 },
};
