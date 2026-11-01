/**
 * Entorno de pruebas: DOM real (jsdom) + Web Audio simulado + IndexedDB real
 * (fake-indexeddb). Permite arrancar la app y ejercitar el flujo completo
 * (grabar, editar, exportar) sin navegador.
 *
 * El Web Audio simulado respeta la forma de la API (nodos, parámetros,
 * conexiones y buffers reales en memoria), de modo que los errores de cableado
 * y de lógica sí se detectan.
 */

import fs from 'node:fs';
import path from 'node:path';
import { JSDOM } from 'jsdom';
import { indexedDB, IDBKeyRange } from 'fake-indexeddb';

const ROOT = path.resolve(import.meta.dirname, '../..');

// ------------------------------------------------------------------ Web Audio
class FakeAudioParam {
  constructor(value = 0, name = 'param') {
    this.value = value;
    this.name = name;
    this.events = [];
  }
  setValueAtTime(value, time) {
    this.value = value;
    this.events.push(['set', value, time]);
    return this;
  }
  linearRampToValueAtTime(value, time) {
    this.value = value;
    this.events.push(['ramp', value, time]);
    return this;
  }
  exponentialRampToValueAtTime(value, time) {
    if (value <= 0) throw new RangeError('exponentialRampToValueAtTime con valor <= 0');
    this.value = value;
    this.events.push(['exp', value, time]);
    return this;
  }
  setTargetAtTime(value, time) {
    this.value = value;
    this.events.push(['target', value, time]);
    return this;
  }
  cancelScheduledValues() {
    return this;
  }
}

class FakeNode {
  constructor(ctx, type) {
    this.context = ctx;
    this.type = type;
    this.connections = [];
    this.channelCount = 2;
    this.channelCountMode = 'max';
    this.channelInterpretation = 'speakers';
  }
  connect(destination) {
    this.connections.push(destination);
    return destination;
  }
  disconnect() {
    this.connections = [];
  }
}

class FakeAudioBuffer {
  constructor({ numberOfChannels = 2, length = 0, sampleRate = 48000 }) {
    this.numberOfChannels = numberOfChannels;
    this.length = length;
    this.sampleRate = sampleRate;
    this.channels = [];
    for (let i = 0; i < numberOfChannels; i++) this.channels.push(new Float32Array(length));
  }
  get duration() {
    return this.length / this.sampleRate;
  }
  getChannelData(index) {
    if (index >= this.numberOfChannels) throw new Error('canal fuera de rango');
    return this.channels[index];
  }
  copyToChannel(source, index) {
    this.channels[index].set(source.subarray(0, this.length));
  }
}

class FakeAudioContext {
  constructor(options = {}) {
    this.sampleRate = options.sampleRate ?? 48000;
    this.state = 'running';
    this._time = 0;
    this.baseLatency = 0.005;
    this.outputLatency = 0.011;
    this.destination = new FakeNode(this, 'destination');
    this.created = [];
    this._scriptProcessors = [];
  }
  get currentTime() {
    return this._time;
  }
  advance(seconds) {
    this._time += seconds;
  }
  async resume() {
    this.state = 'running';
  }
  async suspend() {
    this.state = 'suspended';
  }
  async close() {
    this.state = 'closed';
  }
  _register(node) {
    this.created.push(node);
    return node;
  }
  createGain() {
    const node = new FakeNode(this, 'gain');
    node.gain = new FakeAudioParam(1, 'gain');
    return this._register(node);
  }
  createBiquadFilter() {
    const node = new FakeNode(this, 'biquad');
    node.type = 'lowpass';
    node.frequency = new FakeAudioParam(350, 'frequency');
    node.Q = new FakeAudioParam(1, 'Q');
    node.gain = new FakeAudioParam(0, 'gain');
    return this._register(node);
  }
  createDynamicsCompressor() {
    const node = new FakeNode(this, 'compressor');
    node.threshold = new FakeAudioParam(-24, 'threshold');
    node.knee = new FakeAudioParam(30, 'knee');
    node.ratio = new FakeAudioParam(12, 'ratio');
    node.attack = new FakeAudioParam(0.003, 'attack');
    node.release = new FakeAudioParam(0.25, 'release');
    node.reduction = 0;
    return this._register(node);
  }
  createWaveShaper() {
    const node = new FakeNode(this, 'waveshaper');
    node.curve = null;
    node.oversample = 'none';
    return this._register(node);
  }
  createStereoPanner() {
    const node = new FakeNode(this, 'panner');
    node.pan = new FakeAudioParam(0, 'pan');
    return this._register(node);
  }
  createAnalyser() {
    const node = new FakeNode(this, 'analyser');
    node.fftSize = 2048;
    node.getFloatTimeDomainData = (array) => {
      const level = this.testLevel ?? 0;
      for (let i = 0; i < array.length; i++) {
        array[i] = Math.sin((2 * Math.PI * 440 * i) / this.sampleRate) * level;
      }
    };
    return this._register(node);
  }
  createDelay(maxDelay = 1) {
    const node = new FakeNode(this, 'delay');
    node.delayTime = new FakeAudioParam(0, 'delayTime');
    node.maxDelayTime = maxDelay;
    return this._register(node);
  }
  createConvolver() {
    const node = new FakeNode(this, 'convolver');
    node.buffer = null;
    node.normalize = true;
    return this._register(node);
  }
  createChannelSplitter(channels = 2) {
    const node = new FakeNode(this, 'splitter');
    node.numberOfOutputs = channels;
    return this._register(node);
  }
  createChannelMerger(channels = 2) {
    const node = new FakeNode(this, 'merger');
    node.numberOfInputs = channels;
    return this._register(node);
  }
  createOscillator() {
    const node = new FakeNode(this, 'oscillator');
    node.type = 'sine';
    node.frequency = new FakeAudioParam(440, 'frequency');
    node.start = (when = 0) => {
      this.clickTimes = this.clickTimes ?? [];
      this.clickTimes.push(when);
    };
    node.stop = () => {};
    return this._register(node);
  }
  createBuffer(numberOfChannels, length, sampleRate) {
    return new FakeAudioBuffer({ numberOfChannels, length, sampleRate });
  }
  createBufferSource() {
    const node = new FakeNode(this, 'buffersource');
    node.buffer = null;
    node.playbackRate = new FakeAudioParam(1, 'playbackRate');
    node.started = [];
    node.start = (when = 0, offset = 0, duration) => {
      node.started.push({ when, offset, duration });
      node.onended?.();
    };
    node.stop = () => {};
    return this._register(node);
  }
  createMediaStreamSource(stream) {
    const node = new FakeNode(this, 'mediastream');
    node.stream = stream;
    return this._register(node);
  }
  createScriptProcessor(bufferSize = 4096, inputs = 2, outputs = 1) {
    const node = new FakeNode(this, 'scriptprocessor');
    node.bufferSize = bufferSize;
    node.onaudioprocess = null;
    this._scriptProcessors.push(node);
    return this._register(node);
  }
  createMediaStreamDestination() {
    return this._register(new FakeNode(this, 'mediastreamdestination'));
  }
}

class FakeOfflineAudioContext extends FakeAudioContext {
  constructor(channels, length, sampleRate) {
    super({ sampleRate });
    this.length = length;
    this.numberOfChannels = channels;
    this.state = 'suspended';
  }
  async startRendering() {
    const buffer = new FakeAudioBuffer({
      numberOfChannels: this.numberOfChannels,
      length: this.length,
      sampleRate: this.sampleRate,
    });
    // Señal de prueba: 220 Hz a -12 dBFS, para que la exportación tenga audio real.
    const amplitude = 0.25;
    for (let c = 0; c < this.numberOfChannels; c++) {
      const data = buffer.getChannelData(c);
      for (let i = 0; i < data.length; i++) {
        data[i] = Math.sin((2 * Math.PI * 220 * i) / this.sampleRate) * amplitude;
      }
    }
    this.state = 'running';
    return buffer;
  }
}

// ------------------------------------------------------------------ Canvas
function fakeCanvasContext() {
  const gradient = { addColorStop() {} };
  return {
    canvas: null,
    font: '10px sans-serif',
    textBaseline: 'alphabetic',
    fillStyle: '#000',
    strokeStyle: '#000',
    lineWidth: 1,
    clearRect() {},
    fillRect() {},
    strokeRect() {},
    fillText() {},
    beginPath() {},
    moveTo() {},
    lineTo() {},
    stroke() {},
    closePath() {},
    save() {},
    restore() {},
    setTransform() {},
    translate() {},
    createLinearGradient: () => gradient,
    measureText: () => ({ width: 10 }),
    drawImage() {},
    putImageData() {},
    getImageData: () => ({ data: new Uint8ClampedArray(4) }),
  };
}

// ------------------------------------------------------------------ Entorno
export function setupEnvironment() {
  const html = fs.readFileSync(path.join(ROOT, 'index.html'), 'utf8');
  const dom = new JSDOM(html, {
    url: 'http://localhost:4173/',
    runScripts: 'outside-only',
    pretendToBeVisual: false,
  });

  const { window } = dom;

  // Canvas
  window.HTMLCanvasElement.prototype.getContext = function () {
    if (!this._ctx) this._ctx = fakeCanvasContext();
    return this._ctx;
  };
  Object.defineProperty(window.HTMLElement.prototype, 'clientWidth', {
    configurable: true,
    get() {
      return this.dataset?.width ? Number(this.dataset.width) : 800;
    },
  });
  Object.defineProperty(window.HTMLElement.prototype, 'clientHeight', {
    configurable: true,
    get() {
      return 400;
    },
  });

  // Animación controlada
  const frameQueue = [];
  window.requestAnimationFrame = (callback) => {
    frameQueue.push(callback);
    return frameQueue.length;
  };
  window.cancelAnimationFrame = () => {};
  window.flushFrames = (count = 1) => {
    for (let i = 0; i < count; i++) {
      const pending = frameQueue.splice(0, frameQueue.length);
      for (const callback of pending) callback(performance.now());
    }
  };

  // Diálogos y navegación
  window.confirm = () => true;
  window.alert = () => {};
  window.matchMedia = () => ({ matches: false, addEventListener() {}, removeEventListener() {} });
  window.devicePixelRatio = 2;
  window.URL.createObjectURL = () => 'blob:fake-url';
  window.URL.revokeObjectURL = () => {};
  window.HTMLAnchorElement.prototype.click = function () {
    window.__downloads = window.__downloads ?? [];
    window.__downloads.push({ href: this.href, download: this.download });
  };
  window.HTMLDialogElement.prototype.showModal = function () {
    this.setAttribute('open', '');
    this.open = true;
  };
  window.HTMLDialogElement.prototype.close = function () {
    this.removeAttribute('open');
    this.open = false;
  };

  // Micrófono simulado
  const audioTrack = {
    label: 'V8 Audio Device',
    stop() {},
    getSettings: () => ({ sampleRate: 48000, channelCount: 2, deviceId: 'v8-id', latency: 0.012 }),
  };
  const stream = {
    getAudioTracks: () => [audioTrack],
    getTracks: () => [audioTrack],
  };
  Object.defineProperty(window.navigator, 'mediaDevices', {
    configurable: true,
    value: {
      getUserMedia: async () => stream,
      enumerateDevices: async () => [
        { kind: 'audioinput', deviceId: 'v8-id', label: 'V8 Audio Device' },
        { kind: 'audioinput', deviceId: 'builtin', label: 'Micrófono interno' },
        { kind: 'audiooutput', deviceId: 'speakers', label: 'Altavoces' },
      ],
    },
  });
  Object.defineProperty(window.navigator, 'storage', {
    configurable: true,
    value: {
      estimate: async () => ({ usage: 12 * 1024 * 1024, quota: 2048 * 1024 * 1024 }),
      persist: async () => true,
    },
  });
  Object.defineProperty(window.navigator, 'permissions', {
    configurable: true,
    value: { query: async () => ({ state: 'granted' }) },
  });

  // Web Audio
  window.AudioContext = FakeAudioContext;
  window.OfflineAudioContext = FakeOfflineAudioContext;
  window.AudioBuffer = FakeAudioBuffer;

  // IndexedDB
  window.indexedDB = indexedDB;
  window.IDBKeyRange = IDBKeyRange;

  // Globals para los módulos (usan `window` y también nombres globales)
  const globals = {
    window,
    document: window.document,
    navigator: window.navigator,
    localStorage: window.localStorage,
    HTMLElement: window.HTMLElement,
    HTMLInputElement: window.HTMLInputElement,
    HTMLDialogElement: window.HTMLDialogElement,
    Element: window.Element,
    Node: window.Node,
    Event: window.Event,
    CustomEvent: window.CustomEvent,
    getComputedStyle: window.getComputedStyle.bind(window),
    matchMedia: window.matchMedia,
    requestAnimationFrame: window.requestAnimationFrame,
    cancelAnimationFrame: window.cancelAnimationFrame,
    devicePixelRatio: 2,
    AudioContext: FakeAudioContext,
    OfflineAudioContext: FakeOfflineAudioContext,
    AudioBuffer: FakeAudioBuffer,
    indexedDB,
    IDBKeyRange,
    URL: window.URL,
    Blob: window.Blob ?? Blob,
    File: window.File ?? File,
    TextEncoder,
    TextDecoder,
    performance,
    confirm: window.confirm,
  };
  for (const [key, value] of Object.entries(globals)) {
    if (value === undefined) continue;
    try {
      Object.defineProperty(globalThis, key, {
        configurable: true,
        writable: true,
        value,
      });
    } catch {
      /* propiedad no configurable en este runtime */
    }
  }

  return { dom, window, FakeAudioContext, FakeAudioBuffer };
}

/** Rellena un bloque con una onda senoidal y lo entrega al grabador de respaldo. */
export function feedAudio(ctx, seconds = 0.5, frequency = 440, amplitude = 0.3) {
  const processor = ctx._scriptProcessors[ctx._scriptProcessors.length - 1];
  if (!processor?.onaudioprocess) throw new Error('El grabador no está conectado a un procesador');
  const blockSize = processor.bufferSize;
  const blocks = Math.ceil((seconds * ctx.sampleRate) / blockSize);
  const data = new Float32Array(blockSize);
  for (let block = 0; block < blocks; block++) {
    for (let i = 0; i < blockSize; i++) {
      const n = block * blockSize + i;
      data[i] = Math.sin((2 * Math.PI * frequency * n) / ctx.sampleRate) * amplitude;
    }
    processor.onaudioprocess({
      inputBuffer: {
        length: blockSize,
        numberOfChannels: 2,
        getChannelData: () => data,
      },
      outputBuffer: {
        length: blockSize,
        numberOfChannels: 1,
        getChannelData: () => new Float32Array(blockSize),
      },
    });
  }
  return blocks * blockSize;
}
