/**
 * Captura de la entrada a PCM flotante.
 *
 * Camino principal: AudioWorklet, que entrega muestras exactas y permite
 * empezar a grabar en un fotograma concreto del reloj de audio (alineación
 * entre tomas). Camino de respaldo: ScriptProcessorNode para navegadores sin
 * AudioWorklet, con una precisión menor que se avisa en el diagnóstico.
 */

export class InputRecorder {
  /**
   * @param {BaseAudioContext} ctx
   * @param {{worklets?: boolean}} options
   */
  constructor(ctx, options = {}) {
    this.ctx = ctx;
    this.recording = false;
    this.chunks = [];
    this.frames = 0;
    this.startFrame = -1;
    this.channels = 1;
    this.pendingStop = null;
    this.usingWorklet = false;
    this.usingFallback = false;

    if (options.worklets) {
      try {
        this.node = new AudioWorkletNode(ctx, 'nuby-recorder', {
          numberOfInputs: 1,
          numberOfOutputs: 1,
          outputChannelCount: [1],
          channelCount: 2,
          channelCountMode: 'explicit',
        });
        this.usingWorklet = true;
        this.node.port.onmessage = (event) => this.handleMessage(event.data);
      } catch {
        this.node = null;
      }
    }

    if (!this.node) {
      // Respaldo: ScriptProcessorNode (obsoleto pero funcional en todos los navegadores).
      this.node = ctx.createScriptProcessor(4096, 2, 1);
      this.node.onaudioprocess = (event) => {
        if (!this.recording) return;
        const input = event.inputBuffer;
        const count = input.length;
        const block = [];
        for (let c = 0; c < this.channels; c++) {
          block.push(new Float32Array(input.getChannelData(Math.min(c, input.numberOfChannels - 1))));
        }
        this.collect(block, count);
      };
      this.usingFallback = true;
    }
  }

  /** Conecta la fuente de entrada (la señal limpia del micrófono). */
  connectSource(sourceNode) {
    try {
      this.sourceNode?.disconnect();
    } catch {
      /* nada */
    }
    this.sourceNode = sourceNode;
    sourceNode.connect(this.node);
    if (this.usingFallback) {
      // El procesador de respaldo necesita estar conectado a la salida para correr,
      // pero su salida es silencio porque nunca escribimos en ella.
      const mute = this.ctx.createGain();
      mute.gain.value = 0;
      this.node.connect(mute);
      mute.connect(this.ctx.destination);
      this.fallbackMute = mute;
    }
  }

  handleMessage(data) {
    if (!data) return;
    if (data.type === 'chunk') {
      this.collect(data.channels, data.frames);
      if (data.startFrame >= 0 && this.startFrame < 0) this.startFrame = data.startFrame;
    } else if (data.type === 'started') {
      this.startFrame = data.targetFrame;
    } else if (data.type === 'done') {
      this.startFrame = data.startFrame >= 0 ? data.startFrame : this.startFrame;
      this.finish();
    }
  }

  collect(channels, frames) {
    this.chunks.push(channels);
    this.frames += frames;
  }

  /**
   * Comienza a capturar en un fotograma del reloj de audio.
   * @param {{startFrame?: number, channels?: 1|2}} options
   */
  start(options = {}) {
    this.chunks = [];
    this.frames = 0;
    this.startFrame = -1;
    this.channels = options.channels === 2 ? 2 : 1;
    this.recording = true;
    this.pendingStop = new Promise((resolve) => {
      this.resolveStop = resolve;
    });

    const startFrame =
      options.startFrame ?? Math.round(this.ctx.currentTime * this.ctx.sampleRate);

    if (this.usingWorklet) {
      this.node.port.postMessage({ type: 'start', startFrame, channels: this.channels });
    } else {
      this.startFrame = startFrame;
    }
    return this.pendingStop;
  }

  /** Detiene la captura y devuelve el audio resultante. */
  stop() {
    if (!this.recording) return Promise.resolve(null);
    if (this.usingWorklet) {
      this.node.port.postMessage({ type: 'stop' });
      return this.pendingStop;
    }
    // Se guarda la promesa antes de que finish() la limpie.
    const pending = this.pendingStop;
    this.recording = false;
    this.finish();
    return pending;
  }

  cancel() {
    this.recording = false;
    this.chunks = [];
    this.frames = 0;
    if (this.usingWorklet) this.node.port.postMessage({ type: 'cancel' });
    this.resolveStop?.(null);
    this.resolveStop = null;
    this.pendingStop = null;
  }

  finish() {
    this.recording = false;
    const result = this.assemble();
    this.resolveStop?.(result);
    this.resolveStop = null;
    this.pendingStop = null;
    return result;
  }

  /** Une los bloques en canales continuos. */
  assemble() {
    const frames = this.frames;
    if (!frames) return null;
    const channels = [];
    for (let c = 0; c < this.channels; c++) channels.push(new Float32Array(frames));

    let offset = 0;
    for (const block of this.chunks) {
      const length = block[0].length;
      for (let c = 0; c < this.channels; c++) {
        const source = block[Math.min(c, block.length - 1)];
        if (source) channels[c].set(source.subarray(0, length), offset);
      }
      offset += length;
    }
    this.chunks = [];
    this.frames = 0;

    return {
      channels,
      frames,
      startFrame: this.startFrame,
      sampleRate: this.ctx.sampleRate,
    };
  }
}
