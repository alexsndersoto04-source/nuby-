/**
 * Puerta de ruido (noise gate) para la entrada del micrófono.
 *
 * Abre cuando la señal supera el umbral y cierra suavemente cuando baja,
 * manteniendo (hold) para no cortar palabras. Con el BM800 y la V8 el ruido de
 * fondo es audible en los silencios: esto lo deja limpio sin efectos raros.
 *
 * Parámetros (k-rate): threshold (dBFS), range (dB de atenuación al cerrar),
 * attack (s), hold (s), release (s).
 */
class GateProcessor extends AudioWorkletProcessor {
  static get parameterDescriptors() {
    return [
      { name: 'threshold', defaultValue: -45, minValue: -100, maxValue: 0, automationRate: 'k-rate' },
      { name: 'range', defaultValue: -60, minValue: -90, maxValue: 0, automationRate: 'k-rate' },
      { name: 'attack', defaultValue: 0.003, minValue: 0.0001, maxValue: 0.5, automationRate: 'k-rate' },
      { name: 'hold', defaultValue: 0.05, minValue: 0, maxValue: 1, automationRate: 'k-rate' },
      { name: 'release', defaultValue: 0.12, minValue: 0.005, maxValue: 2, automationRate: 'k-rate' },
    ];
  }

  constructor() {
    super();
    this.envelope = 0;
    this.gain = 0;
    this.holdCounter = 0;
    this.open = false;
  }

  process(inputs, outputs) {
    const input = inputs[0];
    const output = outputs[0];
    if (!output || !output.length) return true;

    const frames = output[0].length;
    if (!input || !input.length) {
      for (let c = 0; c < output.length; c++) output[c].fill(0);
      this.gain = 0;
      this.open = false;
      return true;
    }

    const p = this.parameters;
    const threshold = Math.pow(10, p.threshold[0] / 20);
    const floorGain = Math.pow(10, p.range[0] / 20);
    const attackCoeff = timeToCoeff(p.attack[0], sampleRate);
    const releaseCoeff = timeToCoeff(p.release[0], sampleRate);
    const holdFrames = Math.max(0, Math.round(p.hold[0] * sampleRate));
    const envCoeff = timeToCoeff(0.01, sampleRate);

    for (let i = 0; i < frames; i++) {
      let peak = 0;
      for (let c = 0; c < input.length; c++) {
        const v = Math.abs(input[c][i]);
        if (v > peak) peak = v;
      }

      // Envolvente de detección suavizada.
      this.envelope = peak > this.envelope ? peak : this.envelope + (peak - this.envelope) * envCoeff;

      if (this.envelope >= threshold) {
        this.open = true;
        this.holdCounter = holdFrames;
      } else if (this.holdCounter > 0) {
        this.holdCounter--;
      } else {
        this.open = false;
      }

      const target = this.open ? 1 : floorGain;
      const coeff = target > this.gain ? attackCoeff : releaseCoeff;
      this.gain += (target - this.gain) * coeff;

      for (let c = 0; c < output.length; c++) {
        output[c][i] = (input[Math.min(c, input.length - 1)][i] ?? 0) * this.gain;
      }
    }
    return true;
  }
}

function timeToCoeff(seconds, rate) {
  if (seconds <= 0) return 1;
  return 1 - Math.exp(-1 / (seconds * rate));
}

registerProcessor('nuby-gate', GateProcessor);
