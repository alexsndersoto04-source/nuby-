/**
 * Motor de audio del estudio.
 *
 * - Un único AudioContext a 48 kHz (la frecuencia nativa de la mayoría de
 *   interfaces USB como la V8) con la cadena maestra y los buses de efectos.
 * - Entrada de micrófono real con limpieza de los procesadores del navegador
 *   (cancelación de eco, supresión de ruido y AGC desactivados: si no, la voz
 *   se procesa y se pierde calidad).
 * - Medidores de pico/RMS por pista y medidor LUFS en el maestro.
 *
 * Todo lo que suena pasa por aquí; la exportación reconstruye exactamente la
 * misma topología en un OfflineAudioContext.
 */

import { buildTrackChain, buildMasterChain } from './track-chain.js';
import { createImpulseResponse, REVERB_PRESETS } from './impulse.js';
import { InputRecorder } from './recorder.js';
import { pickInputDevice, rankInputDevices, isBuiltInLabel } from './input-devices.js';
import { Metronome } from './metronome.js';
import { getSetting } from '../state/settings.js';

/** Frecuencia por defecto (la V8 y la mayoría de interfaces entregan 48 kHz). */
export const SAMPLE_RATE = 48000;

/** Frecuencia configurada en Ajustes. */
export function preferredSampleRate() {
  const value = Number(getSetting('sampleRate'));
  return Number.isFinite(value) && value >= 8000 ? value : SAMPLE_RATE;
}
const WORKLET_FILES = [
  'recorder-processor.js',
  'gate-processor.js',
  'limiter-processor.js',
  'lufs-processor.js',
];

export class StudioEngine {
  constructor() {
    this.ctx = null;
    this.tracks = new Map();
    this.workletsReady = false;
    this.workletError = null;
    /**
     * ¿Los procesadores de audio dejan pasar la señal en este navegador?
     * En algunos WebView de Android los AudioWorklet reciben la entrada vacía y
     * devolverían silencio: se comprueba al arrancar y, si fallan, se usa el
     * camino de respaldo (que suena igual, con menos precisión).
     */
    this.processingWorklets = true;
    this.processingCheck = null;
    this.input = {
      stream: null,
      source: null,
      splitter: null,
      leftGain: null,
      rightGain: null,
      gain: null,
      bus: null,
      analyser: null,
      deviceId: null,
      channelMode: 'mono-left',
      monitoring: false,
    };
    this.reverbPreset = 'vocal';
    this.meterBuffer = new Float32Array(2048);
  }

  /** Crea el contexto y toda la topología maestra. Se llama una sola vez. */
  async init() {
    if (this.ctx) return this.ctx;

    const Ctx = window.AudioContext ?? window.webkitAudioContext;
    const latencyHint = getSetting('latencyHint') ?? 'interactive';
    this.ctx = new Ctx({ latencyHint, sampleRate: preferredSampleRate() });

    await this.loadWorklets(this.ctx);
    if (this.workletsReady) {
      this.processingWorklets = await this.checkProcessing();
      if (!this.processingWorklets) {
        this.workletError =
          'Los procesadores de audio no reciben señal en este dispositivo: se usa el modo de respaldo.';
      }
    } else {
      this.processingWorklets = false;
    }

    this.buildMaster();
    this.buildBuses();

    // Codificador grabador (se mantiene vivo con una salida silenciosa).
    this.recorder = new InputRecorder(this.ctx, { worklets: this.workletsReady && this.processingWorklets });
    this.recorderSilence = this.ctx.createGain();
    this.recorderSilence.gain.value = 0;
    // Las dos salidas de captura se mantienen conectadas (aunque sea en silencio):
    // sin conexión al destino el navegador no las hace correr.
    this.recorder.connectOutput(this.recorderSilence);
    this.recorderSilence.connect(this.ctx.destination);

    this.metronome = new Metronome(this.ctx);
    return this.ctx;
  }

  async loadWorklets(ctx) {
    if (!ctx.audioWorklet) {
      this.workletError = 'El navegador no soporta AudioWorklet (procesadores avanzados).';
      return false;
    }
    try {
      for (const file of WORKLET_FILES) {
        const url = new URL(`./worklets/${file}`, import.meta.url);
        await ctx.audioWorklet.addModule(url.href);
      }
      this.workletsReady = true;
      return true;
    } catch (error) {
      this.workletsReady = false;
      this.workletError = `No se pudieron cargar los procesadores de audio: ${error?.message ?? error}`;
      return false;
    }
  }

  /**
   * Comprueba de verdad que un AudioWorklet deja pasar el audio en este equipo:
   * renderiza un tono a través de la cadena maestra en un contexto offline y
   * mide la salida. Si sale en silencio, el dispositivo no sirve para los
   * procesadores y hay que usar el respaldo.
   * @returns {Promise<boolean>}
   */
  async checkProcessing() {
    const OfflineCtx = window.OfflineAudioContext ?? window.webkitOfflineAudioContext;
    if (!OfflineCtx) return false;
    const rate = this.ctx?.sampleRate ?? 48000;
    try {
      const offline = new OfflineCtx(2, Math.round(rate * 0.2), rate);
      for (const file of WORKLET_FILES) {
        await offline.audioWorklet.addModule(new URL(`./worklets/${file}`, import.meta.url).href);
      }
      const master = buildMasterChain(offline, {}, { worklets: true, safe: true });
      master.output.connect(offline.destination);
      const osc = offline.createOscillator();
      osc.frequency.value = 440;
      const level = offline.createGain();
      level.gain.value = 0.5;
      osc.connect(level);
      level.connect(master.input);
      osc.start(0);
      osc.stop(0.19);
      const rendered = await offline.startRendering();
      let peak = 0;
      const from = Math.round(rate * 0.02);
      for (let c = 0; c < rendered.numberOfChannels; c++) {
        const data = rendered.getChannelData(c);
        for (let i = from; i < data.length; i++) {
          const value = data[i] < 0 ? -data[i] : data[i];
          if (value > peak) peak = value;
        }
      }
      this.processingCheck = { peak, ok: peak > 0.01 };
      return this.processingCheck.ok;
    } catch (error) {
      this.processingCheck = { peak: 0, ok: false, error: String(error?.message ?? error) };
      return false;
    }
  }

  buildMaster() {
    const ctx = this.ctx;
    this.master = buildMasterChain(ctx, {}, { worklets: this.processingWorklets });
    // buildMasterChain ya conecta entrada → EQ → compresor → limitador → salida.
    // (Una conexión directa entrada→salida duplicaría la señal y saltaría el máster.)

    this.masterGain = this.master.output;
    this.masterAnalyser = ctx.createAnalyser();
    this.masterAnalyser.fftSize = 2048;
    this.masterGain.connect(this.masterAnalyser);

    // Medidor LUFS real (BS.1770) en paralelo, sin alterar el audio.
    if (this.workletsReady) {
      try {
        this.lufsNode = new AudioWorkletNode(ctx, 'nuby-lufs', {
          numberOfInputs: 1,
          numberOfOutputs: 1,
          outputChannelCount: [2],
          channelCount: 2,
          channelCountMode: 'explicit',
        });
        this.lufsValues = { momentary: -Infinity, shortTerm: -Infinity, integrated: -Infinity, peak: 0 };
        this.lufsNode.port.onmessage = (event) => {
          if (event.data?.type === 'loudness') this.lufsValues = event.data;
        };
        this.masterGain.connect(this.lufsNode);
        const silent = ctx.createGain();
        silent.gain.value = 0;
        this.lufsNode.connect(silent);
        silent.connect(ctx.destination);
      } catch {
        this.lufsNode = null;
      }
    }

    // Reducción real del limitador (GR) para el medidor del máster.
    this.limiterReduction = 0;
    const limiter = this.master.nodes?.limiter;
    if (limiter?.port) {
      limiter.port.onmessage = (event) => {
        if (event.data?.type === 'reduction') this.limiterReduction = event.data.db;
      };
    }

    this.masterGain.connect(ctx.destination);
  }

  buildBuses() {
    const ctx = this.ctx;

    // --- Reverb por convolución ------------------------------------------
    this.reverbInput = ctx.createGain();
    this.reverbConvolver = ctx.createConvolver();
    this.reverbConvolver.normalize = true;
    this.reverbReturn = ctx.createGain();
    this.reverbReturn.gain.value = 0.9;
    this.reverbInput.connect(this.reverbConvolver);
    this.reverbConvolver.connect(this.reverbReturn);
    this.reverbReturn.connect(this.master.input);
    this.setReverbPreset(this.reverbPreset);

    // --- Delay con realimentación ----------------------------------------
    this.delayInput = ctx.createGain();
    this.delayNode = ctx.createDelay(2.0);
    this.delayNode.delayTime.value = 0.375; // corchea con puntillo a 120 BPM
    this.delayFeedback = ctx.createGain();
    this.delayFeedback.gain.value = 0.3;
    this.delayDamping = ctx.createBiquadFilter();
    this.delayDamping.type = 'lowpass';
    this.delayDamping.frequency.value = 4200;
    this.delayReturn = ctx.createGain();
    this.delayReturn.gain.value = 0.8;
    this.delayInput.connect(this.delayNode);
    this.delayNode.connect(this.delayDamping);
    this.delayDamping.connect(this.delayFeedback);
    this.delayFeedback.connect(this.delayNode);
    this.delayNode.connect(this.delayReturn);
    this.delayReturn.connect(this.master.input);
  }

  setReverbPreset(name) {
    const preset = REVERB_PRESETS[name] ?? REVERB_PRESETS.vocal;
    this.reverbPreset = REVERB_PRESETS[name] ? name : 'vocal';
    if (!this.ctx) return;
    // Regenerar la IR es costoso: se hace solo al cambiar de sala.
    this.reverbConvolver.buffer = createImpulseResponse(this.ctx, preset);
    return preset;
  }

  setDelayTime(seconds) {
    if (!this.delayNode) return;
    const now = this.ctx.currentTime;
    this.delayNode.delayTime.setTargetAtTime(Math.max(0.02, seconds), now, 0.05);
  }

  setDelayFeedback(value) {
    if (!this.delayFeedback) return;
    this.delayFeedback.gain.setTargetAtTime(Math.max(0, Math.min(0.85, value)), this.ctx.currentTime, 0.05);
  }

  // ---------------------------------------------------------------- Pistas
  addTrack(track) {
    if (this.tracks.has(track.id)) return this.tracks.get(track.id);
    const runtime = this.createTrackRuntime(track);
    runtime.signature = fxSignature(track.fx);
    this.tracks.set(track.id, runtime);
    this.syncInputRouting();
    return runtime;
  }

  createTrackRuntime(track) {
    const ctx = this.ctx;
    const chain = buildTrackChain(
      ctx,
      { ...track.fx, fader: track.fader, pan: track.pan, muted: track.muted },
      { worklets: this.processingWorklets }
    );
    chain.sends.reverb.connect(this.reverbInput);
    chain.sends.delay.connect(this.delayInput);
    chain.output.connect(this.master.input);

    const analyser = ctx.createAnalyser();
    analyser.fftSize = 1024;
    chain.output.connect(analyser);

    return { id: track.id, chain, analyser, data: new Float32Array(analyser.fftSize) };
  }

  updateTrack(track, soloActive) {
    const runtime = this.tracks.get(track.id);
    if (!runtime) return;

    // Activar/desactivar un módulo cambia la topología: hay que recablear.
    // Los parámetros (ganancia, frecuencia, mezcla…) se actualizan en caliente.
    if (runtime.signature !== fxSignature(track.fx)) {
      this.removeTrack(track.id);
      this.addTrack(track);
      return;
    }

    const muted = track.muted || (soloActive && !track.solo);
    runtime.chain.setFx({ ...track.fx, fader: track.fader, pan: track.pan, muted }, {});
  }

  removeTrack(id) {
    const runtime = this.tracks.get(id);
    if (!runtime) return;
    try {
      runtime.chain.output.disconnect();
      runtime.chain.input.disconnect();
      runtime.analyser.disconnect();
      runtime.chain.sends.reverb.disconnect();
      runtime.chain.sends.delay.disconnect();
    } catch {
      /* nodos ya desconectados */
    }
    this.tracks.delete(id);
    this.syncInputRouting();
  }

  getTrackInput(id) {
    return this.tracks.get(id)?.chain.input ?? null;
  }

  setMaster(master) {
    this.master.setMaster(master);
  }

  // --------------------------------------------------------------- Entrada
  async listInputDevices() {
    if (!navigator.mediaDevices?.enumerateDevices) return [];
    const devices = await navigator.mediaDevices.enumerateDevices();
    const inputs = devices.filter((d) => d.kind === 'audioinput');
    // Ordenadas de mejor a peor: primero las tarjetas externas (V8), al final
    // el micrófono del teléfono, que es el que provoca eco al grabar.
    return rankInputDevices(inputs, this.input?.deviceId ?? this.lastDeviceId ?? '').map((d) => ({
      deviceId: d.deviceId,
      label: d.label,
      builtIn: d.builtIn,
      external: d.external,
      score: d.score,
    }));
  }

  /** La entrada recomendada ahora mismo (sin abrirla). */
  async recommendInput() {
    const devices = await navigator.mediaDevices?.enumerateDevices?.();
    return pickInputDevice(devices ?? [], this.input?.deviceId ?? this.lastDeviceId ?? '');
  }

  /**
   * Abre el micrófono. Se prueban primero las restricciones "de estudio"
   * (sin procesamiento del navegador) y, si el dispositivo las rechaza, se
   * reintenta con lo mínimo necesario.
   */
  async openInput(deviceId) {
    if (!navigator.mediaDevices?.getUserMedia) {
      throw new Error(
        window.isSecureContext
          ? 'Este navegador no permite acceder al micrófono.'
          : 'El micrófono requiere HTTPS o localhost. Abre la app desde una dirección segura.'
      );
    }
    this.closeInput();

    // 1) ¿Qué dispositivo nos piden? Si no indican ninguno se usa la entrada
    //    recomendada (tarjeta externa tipo V8); nunca el micrófono del teléfono
    //    por descarte, porque es el que mete eco y ruido en la toma.
    let devices = [];
    try {
      devices = await navigator.mediaDevices.enumerateDevices();
    } catch {
      devices = [];
    }
    const hasLabels = devices.some((d) => d.kind === 'audioinput' && d.label);
    let targetId = deviceId || '';
    if (!targetId && hasLabels) {
      const preferred = pickInputDevice(devices, this.lastDeviceId ?? '');
      if (preferred?.deviceId && preferred.deviceId !== 'default') targetId = preferred.deviceId;
    }
    const targetInfo = devices.find((d) => d.deviceId === targetId);
    const builtIn = isBuiltInLabel(targetInfo?.label ?? '');
    const phoneMicCleanup = builtIn || (!hasLabels && !targetId);
    const echoSetting = getSetting('phoneMicProcessing') !== false;

    const base = {
      // En una tarjeta externa no queremos procesado del navegador: resta
      // calidad. En el micrófono del teléfono sí, o el altavoz entra en la toma.
      echoCancellation: Boolean(echoSetting && phoneMicCleanup),
      noiseSuppression: Boolean(echoSetting && phoneMicCleanup),
      autoGainControl: false,
      channelCount: { ideal: 2 },
      sampleRate: { ideal: this.ctx?.sampleRate ?? preferredSampleRate() },
      latency: { ideal: 0 },
    };
    const preferred = {
      audio: targetId ? { ...base, deviceId: { exact: targetId } } : base,
      video: false,
    };

    let stream;
    try {
      stream = await navigator.mediaDevices.getUserMedia(preferred);
    } catch (error) {
      if (error?.name === 'NotAllowedError' || error?.name === 'SecurityError') {
        throw new Error(
          'Permiso de micrófono denegado. En el candado de la barra de direcciones, permite el micrófono y vuelve a intentar.'
        );
      }
      if (error?.name === 'OverconstrainedError' || error?.name === 'NotFoundError') {
        // El dispositivo pedido ya no existe: se abre la recomendada y, si
        // tampoco, la del sistema. Se avisa en la interfaz de cuál se abrió.
        const fallbackId = pickInputDevice(devices, '')?.deviceId;
        try {
          stream = await navigator.mediaDevices.getUserMedia({
            audio: fallbackId ? { ...base, deviceId: { exact: fallbackId } } : base,
            video: false,
          });
        } catch {
          stream = await navigator.mediaDevices.getUserMedia({ audio: true, video: false });
        }
      } else {
        throw error;
      }
    }

    // Con el permiso ya concedido aparecen las etiquetas: si existe una tarjeta
    // externa (V8) y se abrió el micrófono del teléfono, se cambia sola. Es la
    // diferencia entre grabar la V8 y grabar el altavoz de vuelta (eco).
    if (!deviceId) {
      try {
        const after = await navigator.mediaDevices.enumerateDevices();
        const best = pickInputDevice(after, this.lastDeviceId ?? '');
        const currentId = stream.getAudioTracks?.()[0]?.getSettings?.().deviceId ?? '';
        if (best?.deviceId && best.external && best.deviceId !== currentId) {
          const better = await navigator.mediaDevices.getUserMedia({
            audio: {
              ...base,
              echoCancellation: false,
              noiseSuppression: false,
              deviceId: { exact: best.deviceId },
            },
            video: false,
          });
          try {
            stream.getTracks().forEach((track) => track.stop());
          } catch {
            /* nada */
          }
          stream = better;
          targetId = best.deviceId;
        }
      } catch {
        /* se queda la entrada que ya estaba abierta */
      }
    }

    if (this.ctx.state === 'suspended') await this.ctx.resume();

    const ctx = this.ctx;
    const source = ctx.createMediaStreamSource(stream);
    const splitter = ctx.createChannelSplitter(2);
    const leftGain = ctx.createGain();
    const rightGain = ctx.createGain();
    const gain = ctx.createGain();
    const bus = ctx.createGain();
    const analyser = ctx.createAnalyser();
    analyser.fftSize = 2048;

    // Modo mono-L por defecto: la V8 entrega el micrófono en el canal izquierdo.
    leftGain.gain.value = 1;
    rightGain.gain.value = 0;

    source.connect(splitter);
    splitter.connect(leftGain, 0);
    splitter.connect(rightGain, 1);
    leftGain.connect(bus);
    rightGain.connect(bus);
    bus.connect(gain);
    gain.connect(analyser);

    const track = stream.getAudioTracks()[0];
    const settings = track?.getSettings?.() ?? {};
    const previousMode = this.input?.channelMode ?? 'mono-left';
    const deviceLabel = track?.label ?? '';
    const usingBuiltIn = isBuiltInLabel(deviceLabel) || (!deviceLabel && !targetId);

    // Si acabó abriéndose el micrófono del teléfono, se activa la limpieza del
    // navegador (eco + ruido). Mejor una voz algo procesada que una toma con el
    // altavoz de vuelta, que es lo que se oye como "eco".
    if (usingBuiltIn && echoSetting) {
      try {
        await track?.applyConstraints?.({ echoCancellation: true, noiseSuppression: true });
      } catch {
        /* el dispositivo no admite cambios en caliente */
      }
    }

    this.lastDeviceId = settings.deviceId ?? targetId ?? '';
    this.input = {
      stream,
      source,
      splitter,
      leftGain,
      rightGain,
      gain,
      bus,
      analyser,
      deviceId: this.lastDeviceId || null,
      channelMode: previousMode,
      latencyEstimate: Number(settings.latency) || 0,
      monitoring: false,
      sampleRate: settings.sampleRate ?? null,
      channelCount: settings.channelCount ?? 1,
      deviceLabel,
      isBuiltIn: usingBuiltIn,
      echoCancellation: Boolean(settings.echoCancellation ?? usingBuiltIn),
      noiseSuppression: Boolean(settings.noiseSuppression ?? usingBuiltIn),
    };

    this.applyChannelMode(this.input.channelMode);

    // El grabador escucha siempre la señal limpia (post ganancia de entrada).
    this.recorder.connectSource(gain);
    this.syncInputRouting();
    return this.input;
  }

  /** 'mono-left' | 'mono-right' | 'mono-sum' | 'stereo' */
  applyChannelMode(mode) {
    const input = this.input;
    if (!input?.bus) return;
    input.channelMode = mode;
    try {
      input.source.disconnect();
    } catch {
      /* ya desconectado */
    }
    try {
      input.splitter.disconnect();
    } catch {
      /* ya desconectado */
    }
    input.leftGain.gain.value = 0;
    input.rightGain.gain.value = 0;

    if (mode === 'stereo') {
      input.source.connect(input.bus);
      input.bus.channelCount = 2;
      input.bus.channelCountMode = 'max';
      return;
    }

    input.bus.channelCount = 1;
    input.bus.channelCountMode = 'explicit';
    input.source.connect(input.splitter);
    input.splitter.connect(input.leftGain, 0);
    input.splitter.connect(input.rightGain, 1);
    if (mode === 'mono-right') {
      input.rightGain.gain.value = 1;
    } else if (mode === 'mono-sum') {
      input.leftGain.gain.value = 0.5;
      input.rightGain.gain.value = 0.5;
    } else {
      input.leftGain.gain.value = 1;
    }
  }

  setInputGain(linear) {
    if (!this.input.gain) return;
    this.input.gain.gain.setTargetAtTime(Math.max(0, linear), this.ctx.currentTime, 0.02);
  }

  setMonitoring(enabled) {
    this.input.monitoring = enabled;
    this.syncInputRouting();
  }

  /** Enruta la entrada solo a las pistas armadas (y solo si la escucha está activa). */
  syncInputRouting() {
    const input = this.input;
    // Se desconectan las conexiones previas de la entrada hacia las cadenas.
    if (this.monitorTap) {
      try {
        this.monitorTap.disconnect();
      } catch {
        /* nada */
      }
      this.monitorTap = null;
    }
    if (!input.bus || !input.monitoring) return;

    const tap = this.ctx.createGain();
    tap.gain.value = 1;
    input.gain.connect(tap);
    this.monitorTap = tap;

    for (const [id, runtime] of this.tracks) {
      const armedIds = this.armedTrackIds ?? new Set();
      if (armedIds.has(id)) tap.connect(runtime.chain.input);
    }
  }

  setArmedTracks(ids) {
    this.armedTrackIds = new Set(ids);
    for (const [id, runtime] of this.tracks) runtime.armed = this.armedTrackIds.has(id);
    this.syncInputRouting();
  }

  closeInput() {
    if (this.monitorTap) {
      try {
        this.monitorTap.disconnect();
      } catch {
        /* nada */
      }
      this.monitorTap = null;
    }
    if (this.input.stream) {
      for (const track of this.input.stream.getTracks()) track.stop();
    }
    try {
      this.input.source?.disconnect();
      this.input.bus?.disconnect();
      this.input.gain?.disconnect();
    } catch {
      /* nada */
    }
    this.input.stream = null;
    this.input.source = null;
    this.input.bus = null;
    this.input.gain = null;
    this.input.analyser = null;
  }

  // ------------------------------------------------------------- Medidores
  /** Nivel de entrada en dBFS (para el vúmetro de grabación). */
  readInputLevel() {
    if (!this.input.analyser) return { peak: -Infinity, rms: -Infinity, clipping: false };
    const data = this.meterBuffer.subarray(0, this.input.analyser.fftSize);
    this.input.analyser.getFloatTimeDomainData(data);
    return analyseSamples(data);
  }

  readTrackLevel(id) {
    const runtime = this.tracks.get(id);
    if (!runtime) return { peak: -Infinity, rms: -Infinity, clipping: false };
    runtime.analyser.getFloatTimeDomainData(runtime.data);
    return analyseSamples(runtime.data);
  }

  readMasterLevel() {
    if (!this.masterAnalyser) return { peak: -Infinity, rms: -Infinity, clipping: false };
    const data = this.meterBuffer.subarray(0, this.masterAnalyser.fftSize);
    this.masterAnalyser.getFloatTimeDomainData(data);
    return analyseSamples(data);
  }

  /** Reinicia la integración del medidor LUFS (al empezar una pasada). */
  resetLufs() {
    try {
      this.lufsNode?.port.postMessage({ type: 'reset' });
    } catch {
      /* sin medidor */
    }
  }

  readLimiterReduction() {
    return this.limiterReduction ?? 0;
  }

  /** Latencia declarada por el navegador (salida + base). */
  getLatencyInfo() {
    if (!this.ctx) return { base: 0, output: 0, total: 0, sampleRate: preferredSampleRate() };
    const base = this.ctx.baseLatency ?? 0;
    const output = this.ctx.outputLatency ?? 0;
    return { base, output, total: base + output, sampleRate: this.ctx.sampleRate };
  }

  /** Tono corto a través del máster, para comprobar que se oye. */
  async playTestTone(seconds = 1) {
    await this.resume();
    const ctx = this.ctx;
    if (!ctx || !this.master) return false;
    const osc = ctx.createOscillator();
    osc.type = 'sine';
    osc.frequency.value = 440;
    const gain = ctx.createGain();
    gain.gain.value = 0.2;
    const now = ctx.currentTime + 0.02;
    gain.gain.setValueAtTime(0.0001, now);
    gain.gain.exponentialRampToValueAtTime(0.2, now + 0.02);
    gain.gain.setValueAtTime(0.2, now + Math.max(0.1, seconds - 0.08));
    gain.gain.exponentialRampToValueAtTime(0.0001, now + seconds);
    osc.connect(gain);
    gain.connect(this.master.input);
    osc.start(now);
    osc.stop(now + seconds + 0.02);
    return true;
  }

  async resume() {
    if (this.ctx?.state !== 'running') await this.ctx?.resume();
  }

  async suspend() {
    if (this.ctx?.state === 'running') await this.ctx.suspend();
  }
}

/** Firma de la topología activa: si cambia, la cadena debe reconstruirse. */
function fxSignature(fx = {}) {
  return [
    Boolean(fx.gate?.on),
    Boolean(fx.hp?.on),
    Boolean(fx.eq?.on),
    Boolean(fx.comp?.on),
    Boolean(fx.sat?.on),
  ].join('|');
}

function analyseSamples(data) {
  let peak = 0;
  let sum = 0;
  let clipping = false;
  for (let i = 0; i < data.length; i++) {
    const abs = Math.abs(data[i]);
    if (abs > peak) peak = abs;
    if (abs >= 0.999) clipping = true;
    sum += data[i] * data[i];
  }
  return {
    peak: peak > 0 ? 20 * Math.log10(peak) : -Infinity,
    rms: sum > 0 ? 10 * Math.log10(sum / data.length) : -Infinity,
    clipping,
  };
}
