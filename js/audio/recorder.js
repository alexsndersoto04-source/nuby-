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
      // Anclas: (fotograma estimado al abrir el bloque − fotogramas ya
      // capturados). El bloque se procesa en el hilo principal un poco después
      // de sonar, así que cada ancla adelanta el inicio real; la más pequeña es
      // la más fiable. Sin esto la toma entraba con retardo (el "atraso").
      anchors: [],
    };
    node.onaudioprocess = (event) => {
      if (!this.recording) return;
      const input = event.inputBuffer;
      if (!input) return;
      const frames = input.length;
      const rate = this.ctx.sampleRate;

      if (path.frames === 0 || path.chunks.length % 24 === 0) {
        const nowFrame = Math.round(this.ctx.currentTime * rate);
        path.anchors.push({ index: path.frames, observed: nowFrame - frames });
      }

      // Se captura todo el bloque: el recorte exacto (pre-roll incluido) se
      // hace al parar, con el reloj de audio como referencia.
      const block = [];
      for (let c = 0; c < this.channels; c++) {
        const source = input.getChannelData(Math.min(c, input.numberOfChannels - 1));
        block.push(source.slice(0));
      }
      this.collect(path, block, frames);
    };
    return path;
  }

  createWorkletPath() {
    const node = new AudioWorkletNode(this.ctx, 'ask-recorder', {
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
    const rate = this.ctx.sampleRate;
    this.channels = options.channels === 2 ? 2 : 1;
    this.startFrameTarget =
      options.startFrame ?? Math.round(this.ctx.currentTime * rate);
    // Ventana real de la toma: desde el fotograma pedido hasta el de parada.
    this.captureStartFrame = this.startFrameTarget;
    this.captureStopFrame = -1;
    this.recording = true;
    this.lastSource = null;
    this.lastPeak = 0;
    this.lastReport = null;

    for (const path of [this.scriptPath, this.workletPath]) {
      if (!path) continue;
      path.chunks = [];
      path.frames = 0;
      path.startFrame = -1;
      path.anchors = [];
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
    // Fotograma exacto de parada: la toma nunca puede ser más larga que esto.
    this.captureStopFrame = Math.round(this.ctx.currentTime * this.ctx.sampleRate);

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
    this.lastReport = best.report ?? null;
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

  /**
   * Fotograma en el que empieza de verdad lo capturado por este camino.
   * El worklet lo informa exacto; el ScriptProcessor se estima con las anclas.
   */
  resolveStartFrame(path) {
    if (!path) return -1;
    if (path.kind !== 'script' || !path.anchors?.length) return path.startFrame;
    let best = Infinity;
    for (const anchor of path.anchors) {
      const start = anchor.observed - anchor.index;
      if (Number.isFinite(start) && start < best) best = start;
    }
    return Number.isFinite(best) ? Math.max(0, Math.round(best)) : path.startFrame;
  }

  /**
   * Une los bloques de un camino en canales continuos y recorta la toma a la
   * ventana real de grabación (desde el fotograma pedido hasta la parada).
   * Así la toma dura exactamente lo que se grabó y empieza en su sitio.
   */
  finishPath(path) {
    if (!path || !path.frames) {
      if (path) {
        path.chunks = [];
        path.frames = 0;
      }
      return null;
    }
    const capturedStart = this.resolveStartFrame(path);
    const captured = path.frames;
    if (path.startFrame < 0 && capturedStart >= 0) path.startFrame = capturedStart;

    const windowStart = Number.isFinite(this.captureStartFrame) ? this.captureStartFrame : capturedStart;
    const windowEnd = this.captureStopFrame > 0 ? this.captureStopFrame : -1;

    let head = 0;
    if (capturedStart >= 0 && windowStart >= capturedStart) head = windowStart - capturedStart;
    head = Math.max(0, Math.min(captured, head));

    let end = captured;
    if (windowEnd > 0 && capturedStart >= 0) {
      const endInPath = windowEnd - capturedStart;
      if (endInPath > head) end = Math.min(captured, endInPath);
    }
    // Red de seguridad: ninguna toma puede ser mucho más larga que lo grabado.
    const windowFrames = windowEnd > windowStart ? windowEnd - windowStart : captured;
    if (windowFrames > 0 && captured - head > windowFrames * 2 + this.ctx.sampleRate) {
      end = Math.min(end, head + windowFrames);
    }

    const length = Math.max(0, Math.floor(end - head));
    const frames = length > 0 ? length : captured;
    const start = length > 0 ? Math.round(capturedStart + head) : capturedStart;

    const channels = [];
    for (let c = 0; c < this.channels; c++) channels.push(new Float32Array(frames));

    let offset = 0;
    for (const block of path.chunks) {
      const blockLength = block[0]?.length ?? 0;
      if (!blockLength) continue;
      const blockStart = offset;
      const blockEnd = offset + blockLength;
      // Qué parte de este bloque cae dentro de [head, head+frames).
      const from = Math.max(blockStart, head);
      const to = Math.min(blockEnd, head + frames);
      if (to > from) {
        const destOffset = from - head;
        const sourceOffset = from - blockStart;
        const count = to - from;
        for (let c = 0; c < this.channels; c++) {
          const source = block[Math.min(c, block.length - 1)];
          if (source) channels[c].set(source.subarray(sourceOffset, sourceOffset + count), destOffset);
        }
      }
      offset = blockEnd;
      if (offset >= head + frames) break;
    }

    const report = {
      source: path.kind,
      capturedFrames: captured,
      frames,
      headTrim: head,
      tailTrim: Math.max(0, captured - head - frames),
      startFrame: start,
      windowFrames: windowEnd > windowStart ? windowEnd - windowStart : captured,
      sampleRate: this.ctx.sampleRate,
    };

    path.chunks = [];
    path.frames = 0;
    path.anchors = [];

    return {
      channels,
      frames,
      startFrame: start,
      capturedFrames: captured,
      sampleRate: this.ctx.sampleRate,
      source: path.kind,
      peak: peakOf(channels),
      report,
    };
  }
}
