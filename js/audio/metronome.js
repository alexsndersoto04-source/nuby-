/**
 * Metrónomo con clic sintetizado y programación anticipada.
 *
 * Los clics se agendan en el reloj del AudioContext (no con setTimeout), así que
 * caen al sample y sirven de referencia fiable para grabar.
 */

export class Metronome {
  constructor(ctx) {
    this.ctx = ctx;
    this.output = ctx.createGain();
    this.output.gain.value = 0.55;
    this.output.connect(ctx.destination);
    this.enabled = false;
  }

  setEnabled(enabled) {
    this.enabled = Boolean(enabled);
  }

  setVolume(value) {
    this.output.gain.setTargetAtTime(Math.max(0, Math.min(1, value)), this.ctx.currentTime, 0.02);
  }

  /**
   * Agenda un clic.
   * @param {number} when tiempo del AudioContext
   * @param {boolean} accent primer tiempo del compás
   */
  click(when, accent = false) {
    if (when < this.ctx.currentTime - 0.01) return;
    const osc = this.ctx.createOscillator();
    const gain = this.ctx.createGain();
    osc.type = 'square';
    osc.frequency.setValueAtTime(accent ? 1760 : 1174, when);

    gain.gain.setValueAtTime(0.0001, when);
    gain.gain.exponentialRampToValueAtTime(accent ? 0.9 : 0.5, when + 0.001);
    gain.gain.exponentialRampToValueAtTime(0.0001, when + 0.045);

    osc.connect(gain);
    gain.connect(this.output);
    osc.start(when);
    osc.stop(when + 0.06);
    osc.onended = () => {
      try {
        osc.disconnect();
        gain.disconnect();
      } catch {
        /* ya desconectado */
      }
    };
  }
}
