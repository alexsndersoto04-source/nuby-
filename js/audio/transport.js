/**
 * Transporte multitrack.
 *
 * Responsabilidades:
 *  - Reproducir los clips con el reloj del AudioContext (no con setTimeout).
 *  - Programar con anticipación (lookahead) para que nada llegue tarde.
 *  - Soportar bucle de compases y cuenta de entrada para grabar.
 *  - Colocar la toma grabada exactamente en su posición, compensando la latencia.
 *
 * Las "entradas" de la línea de tiempo (entries) son tramos contiguos:
 * {ctxStart, pos, duration}. Cada vuelta de un bucle crea una entrada nueva, de
 * forma que la posición se puede calcular con exactitud en cualquier momento.
 */

export class Transport {
  /**
   * @param {import('./engine.js').StudioEngine} engine
   * @param {{getProject: Function, getBuffer: Function, onStateChange?: Function, onRecordingComplete?: Function}} options
   */
  constructor(engine, options) {
    this.engine = engine;
    this.ctx = engine.ctx;
    this.getProject = options.getProject;
    this.getBuffer = options.getBuffer;
    this.onStateChange = options.onStateChange ?? (() => {});
    this.onRecordingComplete = options.onRecordingComplete ?? (() => {});
    this.onCountIn = options.onCountIn ?? (() => {});

    this.state = 'stopped'; // stopped | playing | recording | count-in
    this.timeline = [];
    this.sources = [];
    this.timer = null;
    this.lookahead = 0.3;
    this.loop = { on: false, start: 0, end: 0 };
    this.countInBeats = 0;
    this.recordingInfo = null;
    this.lastPosition = 0;
    this.pendingStopTimeout = null;
    this._endScheduled = false;
  }

  get sampleRate() {
    return this.ctx.sampleRate;
  }

  // ------------------------------------------------------------- Consultas
  /** Posición actual del cabezal en segundos de proyecto. */
  getPosition() {
    if (this.state === 'stopped' || !this.timeline.length) return this.lastPosition;
    const now = this.ctx.currentTime;
    const entry = this.entryAt(now);
    if (!entry) return this.timeline[0]?.pos ?? 0;
    const offset = Math.max(0, Math.min(entry.duration, now - entry.ctxStart));
    return entry.pos + offset;
  }

  isPlaying() {
    return this.state !== 'stopped';
  }

  entryAt(ctxTime) {
    let found = null;
    for (const entry of this.timeline) {
      if (entry.ctxStart <= ctxTime) found = entry;
      else break;
    }
    return found;
  }

  // ------------------------------------------------------------- Controles
  setLoop(loop) {
    this.loop = {
      on: Boolean(loop?.on),
      start: Math.max(0, loop?.start ?? 0),
      end: Math.max(0, loop?.end ?? 0),
    };
    if (this.loop.on && this.loop.end <= this.loop.start) this.loop.on = false;
  }

  setBpm(bpm) {
    this.bpm = bpm;
  }

  seek(position) {
    this.lastPosition = Math.max(0, position);
    if (this.state === 'stopped') return;
    if (this.state === 'recording') {
      // No se puede mover el cabezal mientras se graba: se ignora.
      return;
    }
    const wasPlaying = this.state === 'playing';
    this.stopSources();
    this.state = 'stopped';
    clearInterval(this.timer);
    if (wasPlaying) this.play(this.lastPosition);
  }

  /** Inicia la reproducción desde una posición. */
  play(fromPosition = this.lastPosition) {
    this.ctx.resume?.();
    this.cleanup();
    let start = Math.max(0, fromPosition);
    if (this.loop.on && start >= this.loop.end - 0.001) start = this.loop.start;
    const ctxStart = this.ctx.currentTime + 0.08;

    this.state = 'playing';
    this.lastPosition = start;
    this.timeline = [this.makeEntry(ctxStart, start)];
    this.scheduler = { entryIndex: 0, ctx: ctxStart, pos: start };
    this.startTimer();
    this.onStateChange(this.state);
    this._tick();
  }

  /**
   * Inicia una grabación.
   * @param {{position:number, countInBeats?:number, bpm?:number, channels?:1|2}} options
   */
  async record(options = {}) {
    await this.engine.resume();
    this.cleanup();

    const position = Math.max(0, options.position ?? this.lastPosition);
    const bpm = options.bpm ?? 120;
    const beat = 60 / Math.max(20, bpm);
    const countInBeats = Math.max(0, Math.round(options.countInBeats ?? 0));
    const lead = 0.1;

    const ctxCountInStart = this.ctx.currentTime + lead;
    const ctxRecordStart = ctxCountInStart + countInBeats * beat;

    this.state = countInBeats > 0 ? 'count-in' : 'recording';
    this.lastPosition = position;
    // Grabando no hay final de material: el tramo es infinito (o el bucle).
    const recordDuration = this.loop.on
      ? Math.max(0, this.loop.end - position)
      : Infinity;
    this.timeline = [{ ctxStart: ctxRecordStart, pos: position, duration: recordDuration }];
    this.scheduler = { entryIndex: 0, ctx: ctxRecordStart, pos: position };

    // Cuenta de entrada audible (con acento en el primer tiempo).
    if (countInBeats > 0) {
      for (let i = 0; i < countInBeats; i++) {
        this.engine.metronome.click(ctxCountInStart + i * beat, i % 4 === 0);
      }
      this.onCountIn(countInBeats);
    }

    const startFrame = Math.round(ctxRecordStart * this.sampleRate);
    this.recordingPromise = this._startRecorder(startFrame, options.channels ?? 1);
    this.recordingInfo = { position, startFrame, sampleRate: this.sampleRate };

    this.startTimer();
    this.onStateChange(this.state);
    this._tick();

    setTimeout(() => {
      if (this.state === 'count-in') {
        this.state = 'recording';
        this.onStateChange(this.state);
      }
    }, Math.max(0, (ctxRecordStart - this.ctx.currentTime) * 1000));
  }

  async _startRecorder(startFrame, channels) {
    const recorder = this.engine.recorder;
    const promise = recorder.start({ startFrame, channels });
    return promise;
  }

  makeEntry(ctxStart, pos) {
    return { ctxStart, pos, duration: this.passDuration(pos) };
  }

  passDuration(fromPos) {
    if (this.loop.on) return Math.max(0, this.loop.end - fromPos);
    const end = this.projectEnd();
    // Sin material el transporte sigue corriendo (útil para practicar con el metrónomo).
    if (end <= fromPos) return 3600;
    return end - fromPos;
  }

  projectEnd() {
    const project = this.getProject();
    let end = 0;
    for (const clip of project.clips) {
      end = Math.max(end, clip.start + clip.duration);
    }
    return end;
  }

  /**
   * Detiene todo. Si se estaba grabando, devuelve la toma capturada.
   * @returns {Promise<{channels: Float32Array[], frames: number, startFrame: number, sampleRate: number}|null>}
   */
  async stop() {
    const wasRecording = this.state === 'recording' || this.state === 'count-in';
    this.lastPosition = this.getPosition();
    this.state = 'stopped';
    this.cleanup();

    if (!wasRecording) {
      this.onStateChange(this.state);
      return null;
    }

    this.state = 'stopped';
    this.onStateChange(this.state);
    const result = await this.engine.recorder.stop();
    const info = this.recordingInfo;
    this.recordingInfo = null;
    return result ? { ...result, info } : null;
  }

  /** Detiene sin devolver la toma (descartar). */
  cancelRecording() {
    this.engine.recorder.cancel();
    this.state = 'stopped';
    this.cleanup();
    this.onStateChange(this.state);
  }

  cleanup() {
    clearInterval(this.timer);
    this.timer = null;
    this.stopSources();
    clearTimeout(this.pendingStopTimeout);
    this.pendingStopTimeout = null;
    this.timeline = [];
    this._endScheduled = false;
  }

  stopSources() {
    for (const item of this.sources) {
      try {
        item.source.onended = null;
        item.source.stop();
      } catch {
        /* ya terminó */
      }
      try {
        item.source.disconnect();
        item.gain.disconnect();
      } catch {
        /* ya desconectado */
      }
    }
    this.sources = [];
  }

  startTimer() {
    clearInterval(this.timer);
    this.timer = setInterval(() => this._tick(), 25);
  }

  // ------------------------------------------------------------ Programación
  _tick() {
    if (this.state === 'stopped') return;
    const now = this.ctx.currentTime;

    // Se descartan entradas viejas (ya terminadas) para no crecer sin límite.
    while (this.timeline.length > 2 && this.timeline[1].ctxStart + this.timeline[1].duration < now) {
      this.timeline.shift();
      this.scheduler.entryIndex = Math.max(0, this.scheduler.entryIndex - 1);
    }

    let guard = 0;
    while (guard++ < 200) {
      const entry = this.timeline[this.scheduler.entryIndex];
      if (!entry) break;

      const entryEndCtx = entry.ctxStart + entry.duration;
      const horizon = now + this.lookahead;
      const segmentEndCtx = Math.min(horizon, entryEndCtx);
      const length = segmentEndCtx - this.scheduler.ctx;

      if (length > 1e-4) {
        const atPassStart = Math.abs(this.scheduler.pos - entry.pos) < 1e-6;
        const passEnd = this.loop.on ? this.loop.end : Infinity;
        this.scheduleRange(this.scheduler.pos, this.scheduler.pos + length, this.scheduler.ctx, {
          includeTails: atPassStart,
          passEnd,
        });
        this.scheduler.ctx += length;
        this.scheduler.pos += length;
      } else if (this.scheduler.ctx < horizon && entryEndCtx <= this.scheduler.ctx + 1e-4) {
        // Fin del tramo: si hay bucle, se encadena la vuelta siguiente.
        if (this.loop.on && this.loop.end > this.loop.start) {
          const next = {
            ctxStart: entryEndCtx,
            pos: this.loop.start,
            duration: Math.max(0, this.loop.end - this.loop.start),
          };
          this.timeline.push(next);
          this.scheduler.entryIndex = this.timeline.length - 1;
          this.scheduler.ctx = next.ctxStart;
          this.scheduler.pos = next.pos;
          continue;
        }
        // Sin bucle: al llegar al final se detiene solo.
        if (!this._endScheduled) {
          this._endScheduled = true;
          const ms = Math.max(0, (entryEndCtx - this.ctx.currentTime) * 1000) + 30;
          if (this.state === 'playing') {
            this.pendingStopTimeout = setTimeout(() => {
              if (this.state === 'playing') {
                this.state = 'stopped';
                this.lastPosition = this.projectEnd();
                this.cleanup();
                this.onStateChange(this.state);
              }
            }, ms);
          }
        }
        break;
      } else {
        break;
      }
    }
  }

  /**
   * Agenda todos los clips (y clics) que comienzan dentro de [fromPos, toPos).
   * Cada clip se agenda COMPLETO (no por trozos): así una toma de 5 minutos no
   * se fragmenta en nodos de 300 ms ni se corta al terminar la ventana.
   * `includeTails` permite entrar a mitad de un clip (al empezar a reproducir
   * desde el medio o al reiniciar un bucle).
   */
  scheduleRange(fromPos, toPos, ctxAtFrom, options = {}) {
    if (toPos - fromPos <= 1e-6) return;
    const includeTails = options.includeTails !== false;
    const passEnd = options.passEnd ?? Infinity;
    const project = this.getProject();
    const soloActive = project.tracks.some((t) => t.solo);

    for (const clip of project.clips) {
      const track = project.tracks.find((t) => t.id === clip.trackId);
      if (!track) continue;
      if (track.muted || (soloActive && !track.solo)) continue;

      const clipEnd = Math.min(clip.start + clip.duration, passEnd);
      const startsInWindow = clip.start >= fromPos - 1e-6 && clip.start < toPos - 1e-6;
      const continuesFromBefore = includeTails && clip.start < fromPos - 1e-6 && clipEnd > fromPos;
      if (!startsInWindow && !continuesFromBefore) continue;

      const buffer = this.getBuffer(clip.bufferId);
      if (!buffer) continue;

      const segStart = startsInWindow ? clip.start : fromPos;
      const playDuration = clipEnd - segStart;
      if (playDuration <= 1e-6) continue;

      let when = ctxAtFrom + (segStart - fromPos);
      const startInClip = segStart - clip.start;
      let offset = (clip.offset ?? 0) + startInClip;
      let duration = playDuration;

      // Si el instante ya pasó (arranque con retraso), se recorta en vez de perderlo.
      const late = this.ctx.currentTime - when;
      if (late > 0.002) {
        if (late >= duration) continue;
        offset += late;
        duration -= late;
        when = this.ctx.currentTime;
      }

      if (offset >= buffer.duration) continue;
      const available = Math.min(duration, buffer.duration - offset);

      const source = this.ctx.createBufferSource();
      source.buffer = buffer;
      const gain = this.ctx.createGain();
      source.connect(gain);
      const target = this.engine.getTrackInput(track.id);
      if (!target) continue;
      gain.connect(target);

      // Posición dentro del clip (en segundos de clip) donde empieza este fragmento.
      const envelopeStart = offset - (clip.offset ?? 0);
      scheduleEnvelope(gain.gain, clip, envelopeStart, envelopeStart + available, when);
      try {
        source.start(when, offset, available);
      } catch {
        continue;
      }
      source.onended = () => {
        try {
          source.disconnect();
          gain.disconnect();
        } catch {
          /* nada */
        }
        this.sources = this.sources.filter((s) => s.source !== source);
      };
      this.sources.push({ source, gain });
    }

    // Metrónomo alineado a la rejilla del proyecto.
    const session = project.session ?? {};
    if (session.metronome?.on && this.engine.metronome) {
      this.engine.metronome.setEnabled(true);
      this.engine.metronome.setVolume(session.metronome.volume ?? 0.55);
      const beat = 60 / Math.max(20, session.bpm ?? 120);
      const beatsPerBar = session.timeSig?.[0] ?? 4;
      const firstBeat = Math.ceil((fromPos - 1e-9) / beat);
      const lastBeat = Math.floor((toPos - 1e-9) / beat);
      for (let b = firstBeat; b <= lastBeat; b++) {
        const beatPos = b * beat;
        const when = ctxAtFrom + (beatPos - fromPos);
        if (when < this.ctx.currentTime) continue;
        this.engine.metronome.click(when, b % beatsPerBar === 0);
      }
    }
  }
}

/** Valor de la envolvente (fades incluidos) de un clip en el instante t. */
export function clipEnvelope(clip, t) {
  const base = clip.gain ?? 1;
  const duration = clip.duration;
  const fadeIn = Math.max(0, clip.fadeIn ?? 0);
  const fadeOut = Math.max(0, clip.fadeOut ?? 0);
  let value = base;
  if (fadeIn > 0 && t < fadeIn) value *= Math.max(0, t / fadeIn);
  if (fadeOut > 0 && t > duration - fadeOut) {
    value *= Math.max(0, (duration - t) / fadeOut);
  }
  return Math.max(0, value);
}

/** Programa la envolvente exacta del clip entre t0 y t1 sobre un AudioParam. */
export function scheduleEnvelope(param, clip, t0, t1, when) {
  const points = [t0];
  const fadeIn = Math.max(0, clip.fadeIn ?? 0);
  const fadeOut = Math.max(0, clip.fadeOut ?? 0);
  const fadeOutStart = clip.duration - fadeOut;
  if (fadeIn > t0 && fadeIn < t1) points.push(fadeIn);
  if (fadeOut > 0 && fadeOutStart > t0 && fadeOutStart < t1) points.push(fadeOutStart);
  points.push(t1);
  points.sort((a, b) => a - b);

  param.setValueAtTime(clipEnvelope(clip, points[0]), when);
  for (let i = 1; i < points.length; i++) {
    const offset = points[i] - points[0];
    if (offset <= 0) continue;
    param.linearRampToValueAtTime(clipEnvelope(clip, points[i]), when + offset);
  }
}
