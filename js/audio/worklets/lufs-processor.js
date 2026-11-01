/**
 * Medidor de sonoridad BS.1770-4 dentro del hilo de audio.
 *
 * Filtra con la ponderación K, integra bloques de 400 ms con solapamiento del
 * 75 % y aplica las puertas absoluta y relativa. Emite momentary (400 ms),
 * short-term (3 s) e integrado (toda la reproducción), igual que un medidor
 * de estudio.
 */
class LufsProcessor extends AudioWorkletProcessor {
  constructor() {
    super();
    const coeffs = kWeighting(sampleRate);
    this.shelf = coeffs.shelf;
    this.highpass = coeffs.highpass;
    this.state = [
      { s1: 0, s2: 0, h1: 0, h2: 0 },
      { s1: 0, s2: 0, h1: 0, h2: 0 },
    ];

    this.hop = Math.round(0.1 * sampleRate); // 100 ms
    this.hopEnergy = 0; // suma de cuadrados del hop actual (todos los canales)
    this.hopCount = 0;
    this.history = []; // energía media por hop (normalizada)
    this.maxHistory = Math.round(4 * 10); // 4 s de hops
    this.blockLoudness = []; // para el integrado
    this.integrated = -Infinity;
    this.samplePeak = 0;
    this.reportCounter = 0;

    this.port.onmessage = (event) => {
      if (event.data?.type === 'reset') {
        this.history = [];
        this.blockLoudness = [];
        this.integrated = -Infinity;
        this.hopEnergy = 0;
        this.hopCount = 0;
        this.samplePeak = 0;
      }
    };
  }

  process(inputs, outputs) {
    const input = inputs[0];
    const output = outputs[0];
    if (output && output.length) {
      // Passthrough nulo: el medidor no altera el audio.
      for (let c = 0; c < output.length; c++) output[c].fill(0);
    }
    if (!input || !input.length) return true;

    const frames = input[0].length;
    const channelCount = Math.min(input.length, 2);

    for (let i = 0; i < frames; i++) {
      let energy = 0;
      for (let c = 0; c < channelCount; c++) {
        const x = input[c][i];
        const abs = Math.abs(x);
        if (abs > this.samplePeak) this.samplePeak = abs;
        const filtered = this.filterSample(this.state[c], x);
        energy += filtered * filtered;
      }
      this.hopEnergy += energy;
      this.hopCount++;

      if (this.hopCount >= this.hop) {
        const mean = this.hopEnergy / (this.hop * channelCount);
        this.history.push(mean);
        if (this.history.length > this.maxHistory) this.history.shift();
        this.updateIntegrated(mean);
        this.hopEnergy = 0;
        this.hopCount = 0;
      }
    }

    this.reportCounter += frames;
    if (this.reportCounter >= sampleRate / 10) {
      this.reportCounter = 0;
      this.port.postMessage({
        type: 'loudness',
        momentary: this.blockLoudnessOf(4),
        shortTerm: this.blockLoudnessOf(30),
        integrated: this.integrated,
        peak: this.samplePeak,
      });
      this.samplePeak = 0;
    }
    return true;
  }

  blockLoudnessOf(hops) {
    if (this.history.length < 2) return -Infinity;
    const slice = this.history.slice(-hops);
    const mean = slice.reduce((a, b) => a + b, 0) / slice.length;
    return mean > 0 ? -0.691 + 10 * Math.log10(mean) : -Infinity;
  }

  updateIntegrated(hopMean) {
    if (this.history.length < 4) return;
    // Bloque de 400 ms = 4 hops consecutivos, evaluado cada 100 ms.
    const lastFour = this.history.slice(-4);
    const blockMean = lastFour.reduce((a, b) => a + b, 0) / 4;
    const loudness = blockMean > 0 ? -0.691 + 10 * Math.log10(blockMean) : -Infinity;
    this.blockLoudness.push(loudness);
    if (this.blockLoudness.length > 36000) this.blockLoudness.shift(); // ~1 h

    const aboveAbsolute = this.blockLoudness.filter((v) => v > -70);
    if (!aboveAbsolute.length) {
      this.integrated = -Infinity;
      return;
    }
    const meanAbove =
      aboveAbsolute.reduce((a, b) => a + Math.pow(10, b / 10), 0) / aboveAbsolute.length;
    const relativeThreshold = -0.691 + 10 * Math.log10(meanAbove) - 10;
    const threshold = Math.max(-70, relativeThreshold);
    const gated = this.blockLoudness.filter((v) => v > threshold);
    if (!gated.length) {
      this.integrated = -Infinity;
      return;
    }
    const mean = gated.reduce((a, b) => a + Math.pow(10, b / 10), 0) / gated.length;
    this.integrated = -0.691 + 10 * Math.log10(mean);
  }

  filterSample(state, x) {
    const { b: sb, a: sa } = this.shelf;
    const y1 = sb[0] * x + state.s1;
    state.s1 = sb[1] * x - sa[1] * y1 + state.s2;
    state.s2 = sb[2] * x - sa[2] * y1;

    const { b: hb, a: ha } = this.highpass;
    const y2 = hb[0] * y1 + state.h1;
    state.h1 = hb[1] * y1 - ha[1] * y2 + state.h2;
    state.h2 = hb[2] * y1 - ha[2] * y2;
    return y2;
  }
}

function kWeighting(fs) {
  const shelfF0 = 1681.974450955533;
  const shelfGain = 3.999843853973347;
  const shelfQ = 0.7071752369554196;
  const hpF0 = 38.13547087602444;
  const hpQ = 0.5003270373238773;

  const K1 = Math.tan((Math.PI * shelfF0) / fs);
  const Vh = Math.pow(10, shelfGain / 20);
  const Vb = Math.pow(Vh, 0.4996667741545416);
  const a0 = 1 + K1 / shelfQ + K1 * K1;
  const shelf = {
    b: [
      (Vh + (Vb * K1) / shelfQ + K1 * K1) / a0,
      (2 * (K1 * K1 - Vh)) / a0,
      (Vh - (Vb * K1) / shelfQ + K1 * K1) / a0,
    ],
    a: [1, (2 * (K1 * K1 - 1)) / a0, (1 - K1 / shelfQ + K1 * K1) / a0],
  };

  const K2 = Math.tan((Math.PI * hpF0) / fs);
  const a0h = 1 + K2 / hpQ + K2 * K2;
  const highpass = {
    b: [1 / a0h, -2 / a0h, 1 / a0h],
    a: [1, (2 * (K2 * K2 - 1)) / a0h, (1 - K2 / hpQ + K2 * K2) / a0h],
  };

  return { shelf, highpass };
}

registerProcessor('nuby-lufs', LufsProcessor);
