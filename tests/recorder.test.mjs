/**
 * Pruebas del grabador con un reloj de audio realista.
 *
 * El fallo reportado en el teléfono ("grabo unos segundos y el audio sale de
 * 1:46, y todo con atraso") solo aparece cuando el tiempo del contexto avanza
 * mientras llegan bloques. El entorno falso de la app no mueve el reloj, así que
 * estas pruebas lo simulan aparte: es el único sitio donde el caso se reproduce.
 */

import { test } from 'node:test';
import assert from 'node:assert/strict';
import { InputRecorder } from '../js/audio/recorder.js';

const RATE = 48000;
const BLOCK = 4096;

class FakeCtx {
  constructor() {
    this.sampleRate = RATE;
    this._t = 0;
    this._nodes = [];
  }
  get currentTime() {
    return this._t;
  }
  createScriptProcessor() {
    const node = { onaudioprocess: null };
    this._nodes.push(node);
    return node;
  }
  createGain() {
    return { connect() {}, disconnect() {} };
  }
}

/** Señal que codifica el número de fotograma: permite comprobar la alineación. */
function makeBuffer(frames, amplitude = 0.5) {
  const data = new Float32Array(frames);
  for (let i = 0; i < frames; i++) data[i] = Math.sin(i / 12) * amplitude;
  return data;
}

/**
 * Entrega bloques como si el hilo de audio los fuese produciendo.
 * @param {object} options.delay retardo del hilo principal (s) al entregar el bloque
 * @param {object} options.advance si el reloj avanza con los bloques
 */
function feed(ctx, node, seconds, { advanceClock = true, delay = 0.02 } = {}) {
  const blocks = Math.max(1, Math.floor((seconds * RATE) / BLOCK));
  let produced = 0;
  for (let block = 0; block < blocks; block++) {
    // El bloque cubre [produced, produced + BLOCK).
    const blockStart = produced;
    if (advanceClock) ctx._t = (blockStart + BLOCK) / RATE + delay;
    const data = makeBuffer(BLOCK);
    node.onaudioprocess({
      inputBuffer: {
        length: BLOCK,
        numberOfChannels: 2,
        getChannelData: () => data,
      },
    });
    produced += BLOCK;
  }
  return produced;
}

test('la toma dura lo que se grabó, no lo que se acumuló', async () => {
  const ctx = new FakeCtx();
  const recorder = new InputRecorder(ctx, { worklets: false });
  const node = ctx._nodes[0];

  const startFrame = Math.round(ctx.currentTime * RATE);
  recorder.start({ startFrame, channels: 1 });
  feed(ctx, node, 2);
  ctx._t = node ? ctx._t : 0;
  const take = await recorder.stop();

  assert.ok(take, 'debe devolver una toma');
  const seconds = take.frames / RATE;
  assert.ok(seconds > 1.9 && seconds < 2.15, `la toma debe durar ≈2 s (duró ${seconds.toFixed(2)} s)`);
  assert.ok(take.report, 'debe traer el informe de la toma');
  assert.ok(
    take.report.windowFrames / RATE < 2.2,
    'la ventana grabada debe ser la real'
  );
});

test('si llegan más bloques que tiempo grabado, la toma se recorta', async () => {
  const ctx = new FakeCtx();
  const recorder = new InputRecorder(ctx, { worklets: false });
  const node = ctx._nodes[0];

  recorder.start({ startFrame: 0, channels: 1 });
  // 10 s de bloques entregados, pero el reloj de audio solo avanza 2 s:
  // es exactamente el síntoma de "grabo 3 segundos y salen 1:46".
  feed(ctx, node, 10, { advanceClock: false });
  ctx._t = 2;
  const take = await recorder.stop();

  assert.ok(take, 'debe devolver una toma');
  const seconds = take.frames / RATE;
  assert.ok(seconds <= 2.05, `la toma no puede pasar de lo grabado (duró ${seconds.toFixed(2)} s)`);
  assert.ok(seconds > 1.5, `la toma debe conservar el audio real (duró ${seconds.toFixed(2)} s)`);
});

test('la toma empieza en el fotograma pedido (sin atraso)', async () => {
  const ctx = new FakeCtx();
  const recorder = new InputRecorder(ctx, { worklets: false });
  const node = ctx._nodes[0];

  // Se pide empezar en 1 s; los bloques anteriores son cuenta de entrada.
  const startFrame = RATE;
  recorder.start({ startFrame, channels: 1 });
  feed(ctx, node, 3);
  ctx._t = 3.05;
  const take = await recorder.stop();

  assert.ok(take, 'debe devolver una toma');
  const seconds = take.frames / RATE;
  assert.ok(seconds > 1.9 && seconds < 2.15, `debe recortar el pre-roll (duró ${seconds.toFixed(2)} s)`);
  assert.ok(
    take.startFrame >= startFrame - BLOCK && take.startFrame <= startFrame + BLOCK,
    `el inicio debe caer en el fotograma pedido (${take.startFrame} vs ${startFrame})`
  );
});

test('sin bloques no hay toma y no se rompe', async () => {
  const ctx = new FakeCtx();
  const recorder = new InputRecorder(ctx, { worklets: false });
  recorder.start({ startFrame: 0, channels: 1 });
  const take = await recorder.stop();
  assert.equal(take, null);
  assert.equal(await recorder.stop(), null, 'parar dos veces no duplica la toma');
});

test('grabar en estéreo devuelve dos canales con la misma duración', async () => {
  const ctx = new FakeCtx();
  const recorder = new InputRecorder(ctx, { worklets: false });
  const node = ctx._nodes[0];
  recorder.start({ startFrame: 0, channels: 2 });
  feed(ctx, node, 1);
  ctx._t = 1.05;
  const take = await recorder.stop();
  assert.ok(take);
  assert.equal(take.channels.length, 2);
  assert.equal(take.channels[0].length, take.frames);
  assert.equal(take.channels[1].length, take.frames);
});
