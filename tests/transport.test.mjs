/**
 * Pruebas del transporte: programación con anticipación, bucle, mute/solo,
 * compensación de latencia al grabar y recableado de efectos.
 */
import test from 'node:test';
import assert from 'node:assert/strict';
import { setupEnvironment, feedAudio } from './helpers/env.mjs';

const env = setupEnvironment();
const { window } = env;

await import('../js/main.js');
const app = window.nuby;
await app.bootPromise;

await app.ensureAudio();
await app.dialogs.openDevice('v8-id');

const ctx = app.engine.ctx;
const trackId = app.project.tracks[0].id;
app.selectTrack(trackId);

// Un clip sintético de 2 s que empieza en el segundo 1.
const bufferId = 'buf-test';
const buffer = ctx.createBuffer(1, ctx.sampleRate * 2, ctx.sampleRate);
const samples = buffer.getChannelData(0);
for (let i = 0; i < samples.length; i++) samples[i] = Math.sin((2 * Math.PI * 440 * i) / ctx.sampleRate) * 0.3;
app.buffers.set(bufferId, buffer);
app.project.clips = [
  {
    id: 'clip-test',
    trackId,
    bufferId,
    name: 'Toma de prueba',
    start: 1,
    offset: 0,
    duration: 2,
    gain: 1,
    fadeIn: 0,
    fadeOut: 0,
  },
];

test('el transporte programa los clips con anticipación y en su hora exacta', () => {
  app.transport.play(0);
  assert.equal(app.transport.state, 'playing');
  assert.equal(app.transport.sources.length, 0, 'todavía no debe sonar: el clip empieza en 1 s');

  ctx.advance(1.0);
  app.transport._tick();

  assert.equal(app.transport.sources.length, 1, 'el clip debe estar programado');
  const { source } = app.transport.sources[0];
  assert.equal(source.started.length, 1);
  // ctxStart de la reproducción = currentTime + 0.08 y el clip empieza en 1 s.
  assert.ok(Math.abs(source.started[0].when - 1.08) < 0.02, `when=${source.started[0].when}`);
  assert.equal(source.started[0].offset, 0);
  assert.ok(Math.abs(source.started[0].duration - 2) < 0.02);

  app.transport.stop();
  assert.equal(app.transport.state, 'stopped');
});

test('el bucle encadena vueltas y mantiene la posición', () => {
  app.transport.setLoop({ on: true, start: 0, end: 2 });
  app.transport.play(0);
  const startEntry = app.transport.timeline[0];
  assert.equal(startEntry.duration, 2);

  ctx.advance(2.2);
  app.transport._tick();

  assert.ok(app.transport.timeline.length >= 2, 'debe haber una vuelta nueva');
  const next = app.transport.timeline[app.transport.timeline.length - 1];
  assert.equal(next.pos, 0, 'la vuelta vuelve al inicio del bucle');
  assert.ok(Math.abs(next.ctxStart - (startEntry.ctxStart + 2)) < 0.02, 'encadenada sin huecos');

  const position = app.transport.getPosition();
  assert.ok(position >= 0 && position <= 2, `posición dentro del bucle (${position})`);

  app.transport.setLoop({ on: false, start: 0, end: 0 });
  app.transport.stop();
});

test('las pistas silenciadas no se programan', () => {
  app.project.clips[0].duration = 2;
  app.project.clips[0].start = 0;
  app.patchTrack(trackId, { muted: true }, { silent: true });

  app.transport.play(0);
  ctx.advance(0.5);
  app.transport._tick();
  assert.equal(app.transport.sources.length, 0, 'nada que programar en una pista muda');

  app.transport.stop();
  app.patchTrack(trackId, { muted: false }, { silent: true });
});

test('activar o desactivar un efecto recablea la cadena de la pista', () => {
  const runtimeBefore = app.engine.tracks.get(trackId);
  const chainBefore = runtimeBefore.chain;

  const track = app.project.tracks.find((t) => t.id === trackId);
  track.fx.comp.on = true;
  app.engine.updateTrack(track, false);

  const runtimeAfter = app.engine.tracks.get(trackId);
  assert.notEqual(runtimeAfter.chain, chainBefore, 'la cadena se reconstruye');
  assert.ok(runtimeAfter.chain.nodes.comp, 'el compresor debe existir en la nueva cadena');
  assert.ok(track.fx.comp.on);

  // Cambiar solo un parámetro no debe reconstruir la cadena.
  const chainStable = runtimeAfter.chain;
  track.fx.comp.threshold = -12;
  app.engine.updateTrack(track, false);
  assert.equal(app.engine.tracks.get(trackId).chain, chainStable, 'los parámetros se aplican en caliente');
  assert.equal(chainStable.nodes.comp.threshold.value, -12);
});

test('la latencia de entrada desplaza la toma para que quede alineada', async () => {
  app.project.clips = [];
  app.project.session.latencyOffset = 100; // 100 ms manuales + 12 ms del dispositivo simulado
  app.seek(2);

  await app.toggleRecord();
  feedAudio(ctx, 0.6);
  await app.stopTransport();

  assert.equal(app.project.clips.length, 1);
  const clip = app.project.clips[0];
  const expected = 2 - 0.112;
  assert.ok(Math.abs(clip.start - expected) < 0.06, `inicio=${clip.start} esperado≈${expected}`);
});

test('el metrónomo agenda un clic por pulso y solo si está activo', () => {
  app.project.session.metronome.on = false;
  app.project.session.bpm = 120; // un pulso cada 0,5 s
  app.project.clips = [];
  ctx.clickTimes = [];

  app.transport.play(0);
  ctx.advance(0.4);
  app.transport._tick();
  app.transport.stop();
  assert.equal(ctx.clickTimes.length, 0, 'apagado no suena');

  app.project.session.metronome.on = true;
  ctx.clickTimes = [];
  app.transport.play(0);
  ctx.advance(0.4);
  app.transport._tick();
  app.transport.stop();

  // Clics en 0 s y 0,5 s (desplazados por el inicio del transporte a +0,08 s).
  assert.ok(ctx.clickTimes.length >= 2, `clics programados: ${ctx.clickTimes.length}`);
  const deltas = ctx.clickTimes.slice(1).map((t, i) => t - ctx.clickTimes[i]);
  assert.ok(deltas.every((d) => Math.abs(d - 0.5) < 0.01), 'separación de un pulso');
  app.project.session.metronome.on = false;
});
