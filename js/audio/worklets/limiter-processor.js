/**
 * Limitador de picos con anticipación (lookahead) en el bus maestro.
 *
 * Mantiene el techo configurado sin recortar (clipping) de forma brusca:
 * detecta el pico hasta 5 ms antes de que ocurra, aplica la reducción con
 * ataque instantáneo y libera suavemente. Informa la reducción aplicada para
 * que el vúmetro de "GR" sea real y no decorativo.
 *
 * Parámetros: ceiling (dBFS), release (s), lookahead (s).
 */
class LimiterProcessor extends AudioWorkletProcessor {
  static get parameterDescriptors() {
    return [
      { name: 'ceiling', defaultValue: -1, minValue: -24, maxValue: 0, automationRate: 'k-rate' },
      { name: 'release', defaultValue: 0.08, minValue: 0.005, maxValue: 1, automationRate: 'k-rate' },
      { name: 'lookahead', defaultValue: 0.005, minValue: 0.001, maxValue: 0.02, automationRate: 'k-rate' },
    ];
  }

  constructor() {
    super();
    this.maxLookahead = Math.round(0.02 * sampleRate);
    this.delay = [new Float32Array(this.maxLookahead + 1), new Float32Array(this.maxLookahead + 1)];
    this.gainQueue = new Float32Array(this.maxLookahead + 1);
    this.writeIndex = 0;
    this.currentGain = 1;
    this.currentLookahead = -1;
    this.peaks = []; // deque monótona: { index, value }
    this.frameCounter = 0;
    this.reductionDb = 0;
    this.reportCounter = 0;
  }

  process(inputs, outputs) {
    const input = inputs[0];
    const output = outputs[0];
    if (!output || !output.length) return true;

    const frames = output[0].length;
    const lookaheadSamples = Math.max(
      1,
      Math.min(this.maxLookahead, Math.round(this.parameters.lookahead[0] * sampleRate))
    );
    const ceiling = Math.pow(10, this.parameters.ceiling[0] / 20);
    const releaseCoeff = 1 - Math.exp(-1 / (this.parameters.release[0] * sampleRate));
    const channelCount = output.length;

    if (!input || !input.length) {
      for (let c = 0; c < channelCount; c++) output[c].fill(0);
      return true;
    }

    for (let i = 0; i < frames; i++) {
      const frameIndex = this.frameCounter++;
      const ring = this.writeIndex;
      this.delay[0][ring] = input[0][i] ?? 0;
      this.delay[1][ring] = input[Math.min(1, input.length - 1)][i] ?? 0;

      let peak = 0;
      for (let c = 0; c < input.length; c++) {
        const v = Math.abs(input[c][i]);
        if (v > peak) peak = v;
      }

      // Cola monótona decreciente para el máximo deslizante de la ventana.
      while (this.peaks.length && this.peaks[this.peaks.length - 1].value <= peak) this.peaks.pop();
      this.peaks.push({ index: frameIndex, value: peak });
      while (this.peaks.length && this.peaks[0].index <= frameIndex - lookaheadSamples) this.peaks.shift();
      const windowMax = this.peaks.length ? this.peaks[0].value : 0;

      const targetGain = windowMax > ceiling ? ceiling / windowMax : 1;
      if (targetGain < this.currentGain) this.currentGain = targetGain; // ataque instantáneo
      else this.currentGain += (targetGain - this.currentGain) * releaseCoeff;
      this.gainQueue[ring] = this.currentGain;

      // La ganancia calculada hoy se aplica a la muestra que sale del retardo.
      const readIndex = (ring - lookaheadSamples + this.delay[0].length) % this.delay[0].length;
      const appliedGain = this.gainQueue[readIndex];
      for (let c = 0; c < channelCount; c++) {
        const delayed = this.delay[Math.min(c, 1)][readIndex];
        const value = delayed * appliedGain;
        // Seguridad: nunca dejar pasar nada por encima del techo.
        output[c][i] = value > ceiling ? ceiling : value < -ceiling ? -ceiling : value;
      }

      const gr = appliedGain > 0 ? -20 * Math.log10(appliedGain) : 60;
      if (gr > this.reductionDb) this.reductionDb = gr;

      this.writeIndex = (ring + 1) % this.delay[0].length;
    }

    this.reportCounter += frames;
    if (this.reportCounter >= sampleRate / 12) {
      this.port.postMessage({ type: 'reduction', db: this.reductionDb });
      this.reductionDb = 0;
      this.reportCounter = 0;
    }
    return true;
  }
}

registerProcessor('nuby-limiter', LimiterProcessor);
