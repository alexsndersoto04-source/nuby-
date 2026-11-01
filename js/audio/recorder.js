/**
 * Captura de la entrada a PCM flotante.
 *
 * Se graba por **dos caminos a la vez** y al terminar se queda la toma con señal:
 *
 *  1. `ScriptProcessorNode` (API antigua): en cualquier WebView de Android
 *     recibe siempre la señal conectada, aunque el nodo procese en el hilo
 *     principal.
 *  2. `AudioWorklet`: captura en el hilo de audio, con menos fluctuación, pero
 *     depende de que el navegador entregue bien los canales de entrada (en
 *     algunos WebView llegan vacíos y la toma saldría en silencio).
 *
 * Grabar por los dos y comprobar cuál trajo audio convierte un fallo silencioso
 * en algo imposible: si un camino falla, el otro entrega la toma.
 */

/** Pico absoluto de un conjunto de canales. */
function peakOf(channels) {
  let peak = 0;
  for (const channel of channels) {
    for (let i = 0; i < channel.length; i++) {
      const value = channel[i] < 0 ? -channel[i] : channel[i];
      if (value > peak) peak = value;
    }
  }
  return peak;
}

export class InputRecorder {
  /**
   * @param {BaseAudioContext} ctx
   * @param {{worklets?: boolean}} options
   */
  constructor(ctx, options = {}) {
    this.ctx = ctx;
    this.recording = false;
    this.channels = 1;
    this.startFrameTarget = -1;
    this.resolveStop = null;
    this.pendingStop = null;
    this.useWorklet = false;
    this.workletError = null;
    /** Qué camino entregó la última toma: 'worklet' | 'script'. */
    this.lastSource = null;
    this.lastPeak = 0;

    this.scriptPath = this.createScriptPath();

    if (options.worklets) {
      try {
        this.workletPath = this.createWorkletPath();
        this.useWorklet = true;
      } catch (error) {
        this.workletPath = null;
        this.useWorklet = false;
        this.workletError = error?.message ?? String(error);
      }
    } else {
      this.workletPath = null;
    }

    // Compatibilidad: el motor conecta una salida muda para que los nodos corran.
    this.node = this.workletPath?.node ?? this.scriptPath.node;
  }

  /** Nodos que hay que mantener conectados a la salida (aunque sea en silencio). */
  get nodes() {
    return [this.workletPath?.node, this.scriptPath.node].filter(Boolean);
  }

  /** Conecta todas las salidas de captura al nodo indicado (normalmente un gain a 0). */
  connectOutput(destination) {
    for (const node of this.nodes) {
      try {
        node.connect(destination);
      } catch {
        /* nodo ya conectado */
      }
    }
  }

  // -------------------------------------------------------------- Construcción
  createScriptPath() {
    const node = this.ctx.createScriptProcessor(4096, 2, 1);
    const path = {
      kind: 'script',
      node,
      chunks: [],
      frames: 0,
      startFrame: -1,
      skip: 0,
      blockFrame: 0,
      ready: false,
    };
    node.onaudioprocess = (event) => {
      if (!this.recording) return;
      const input = event.inputBuffer;
      if (!input) return;
      const frames = input.length;
      const rate = this.ctx.sampleRate;

      if (!path.ready) {
        // Los bloques del ScriptProcessor llegan seguidos: basta saber dónde
        // empieza el primero y descartar los fotogramas de la cuenta de entrada
        // (o del pre-roll) hasta el punto exacto de inicio.
        const nowFrame = Math.round(this.ctx.currentTime * rate);
        path.blockFrame = Math.max(0, nowFrame - frames);
        path.skip = Math.max(0, this.startFrameTarget - path.blockFrame);
        path.ready = true;
      }

      const skip = Math.min(path.skip, frames);
      path.skip -= skip;
      if (path.startFrame < 0) path.startFrame = path.blockFrame + skip;
      path.blockFrame += frames;
      const count = frames - skip;
      if (count <= 0) return;

      const block = [];
      for (let c = 0; c < this.channels; c++) {
        const source = input.getChannelData(Math.min(c, input.numberOfChannels - 1));
        block.push(source.slice(skip, skip + count));
      }
      this.collect(path, block, count);
    };
    return path;
  }

  createWorkletPath() {
    const node = new AudioWorkletNode(this.ctx, 'nuby-recorder', {
      numberOfInputs: 1,
      numberOfOutputs: 1,
      outputChannelCount: [1],
      // Sin forzar canales: el nodo se adapta a lo que llegue (mono o estéreo).
      channelCountMode: 'max',
    });
    const path = { kind: 'worklet', node, chunks: [], frames: 0, startFrame: -1, ready: false, done: null };
    node.port.onmessage = (event) => {
      const data = event.data;
      if (!data) return;
      if (data.type === 'chunk') {
        this.collect(path, data.channels, data.frames);
        if (data.startFrame >= 0 && path.startFrame < 0) path.startFrame = data.startFrame;
      } else if (data.type === 'done') {
        if (data.startFrame >= 0) path.startFrame = data.startFrame;
        path.done?.(path);
        path.done = null;
      }
    };
    return path;
  }

  /** Conecta la fuente de entrada (la señal limpia del micrófono). */
  connectSource(sourceNode) {
    try {
      this.sourceNode?.disconnect();
    } catch {
      /* nada */
    }
    this.sourceNode = sourceNode;
    for (const node of this.nodes) {
      try {
        sourceNode.connect(node);
      } catch {
        /* nodo no disponible */
      }
    }
  }

  // ----------------------------------------------------------------- Captura
  collect(path, channels, frames) {
    if (!frames || !channels?.length) return;
    path.chunks.push(channels);
    path.frames += frames;
  }

  /**
   * Comienza a capturar en un fotograma del reloj de audio.
   * @param {{startFrame?: number, channels?: 1|2}} options
   */
  start(options = {}) {
    this.channels = options.channels === 2 ? 2 : 1;
    this.startFrameTarget =
      options.startFrame ?? Math.round(this.ctx.currentTime * this.ctx.sampleRate);
    this.recording = true;
    this.lastSource = null;
    this.lastPeak = 0;

    for (const path of [this.scriptPath, this.workletPath]) {
      if (!path) continue;
      path.chunks = [];
      path.frames = 0;
      path.startFrame = -1;
      path.skip = 0;
      path.blockFrame = 0;
      path.ready = false;
      path.done = null;
    }

    if (this.workletPath) {
      this.workletPath.node.port.postMessage({
        type: 'start',
        startFrame: this.startFrameTarget,
        channels: this.channels,
      });
    }

    this.pendingStop = new Promise((resolve) => {
      this.resolveStop = resolve;
    });
    return this.pendingStop;
  }

  /** Detiene la captura y devuelve el mejor audio de los dos caminos. */
  async stop() {
    if (!this.recording) return null;
    this.recording = false;

    const workletResult = await this.stopWorklet();
    const scriptResult = this.finishPath(this.scriptPath);

    const chosen = this.pickBest(workletResult, scriptResult);
    this.workletPath && (this.workletPath.done = null);
    this.resolveStop?.(chosen);
    this.resolveStop = null;
    this.pendingStop = null;
    return chosen;
  }

  stopWorklet() {
    const path = this.workletPath;
    if (!path) return Promise.resolve(null);
    return new Promise((resolve) => {
      let settled = false;
      const finish = () => {
        if (settled) return;
        settled = true;
        resolve(this.finishPath(path));
      };
      path.done = finish;
      try {
        path.node.port.postMessage({ type: 'stop' });
      } catch {
        finish();
        return;
      }
      // Si el hilo de audio no responde (por ejemplo, contexto suspendido),
      // no se pierde la toma del otro camino.
      setTimeout(finish, 400);
    });
  }

  pickBest(workletResult, scriptResult) {
    const candidates = [workletResult, scriptResult].filter((c) => c && c.frames > 0);
    if (!candidates.length) return null;
    let best = candidates[0];
    let bestPeak = -1;
    for (const candidate of candidates) {
      const peak = peakOf(candidate.channels);
      candidate.peak = peak;
      if (peak > bestPeak) {
        bestPeak = peak;
        best = candidate;
      }
    }
    // Si los dos traen la misma señal, se prefiere el worklet (menos fluctuación).
    if (workletResult && scriptResult) {
      const workletPeak = peakOf(workletResult.channels);
      if (workletPeak > 0 && workletPeak >= bestPeak * 0.98) best = workletResult;
    }
    this.lastSource = best.source;
    this.lastPeak = peakOf(best.channels);
    return best;
  }

  cancel() {
    this.recording = false;
    for (const path of [this.scriptPath, this.workletPath]) {
      if (!path) continue;
      path.chunks = [];
      path.frames = 0;
    }
    if (this.workletPath) {
      try {
        this.workletPath.node.port.postMessage({ type: 'cancel' });
      } catch {
        /* nodo cerrado */
      }
      this.workletPath.done = null;
    }
    this.resolveStop?.(null);
    this.resolveStop = null;
    this.pendingStop = null;
  }

  /** Une los bloques de un camino en canales continuos. */
  finishPath(path) {
    if (!path || !path.frames) return null;
    const frames = path.frames;
    const channels = [];
    for (let c = 0; c < this.channels; c++) channels.push(new Float32Array(frames));

    let offset = 0;
    for (const block of path.chunks) {
      const length = block[0]?.length ?? 0;
      if (!length) continue;
      for (let c = 0; c < this.channels; c++) {
        const source = block[Math.min(c, block.length - 1)];
        if (source) channels[c].set(source.subarray(0, Math.min(length, frames - offset)), offset);
      }
      offset += length;
      if (offset >= frames) break;
    }

    path.chunks = [];
    path.frames = 0;

    return {
      channels,
      frames,
      startFrame: path.startFrame,
      sampleRate: this.ctx.sampleRate,
      source: path.kind,
      peak: peakOf(channels),
    };
  }
}
