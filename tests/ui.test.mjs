/**
 * Pruebas de integración de la interfaz y del flujo real de trabajo:
 * arranque, pistas, entrada de audio, grabación, edición, deshacer y exportación.
 */
import test from 'node:test';
import assert from 'node:assert/strict';
import { setupEnvironment, feedAudio } from './helpers/env.mjs';

const env = setupEnvironment();
const { window } = env;

// Se importa después de montar el entorno (main.js crea la app y arranca).
await import('../js/main.js');
const app = window.nuby;
await app.bootPromise;
window.flushFrames(2);

test('la app arranca con el proyecto por defecto y dos pistas', () => {
  assert.equal(app.project.tracks.length, 2);
  assert.equal(window.document.querySelectorAll('#track-list .strip').length, 2);
  assert.match(window.document.getElementById('panel-track').textContent, /Puerta de ruido/);
  assert.equal(window.document.getElementById('app').getAttribute('aria-busy'), 'false');
});

test('añadir y eliminar pistas actualiza modelo e interfaz', () => {
  const before = app.project.tracks.length;
  app.addTrack();
  assert.equal(app.project.tracks.length, before + 1);
  assert.equal(window.document.querySelectorAll('#track-list .strip').length, before + 1);

  app.undo();
  assert.equal(app.project.tracks.length, before);

  app.redo();
  assert.equal(app.project.tracks.length, before + 1);
  const added = app.project.tracks[before];
  app.removeTrack(added.id);
  assert.equal(app.project.tracks.length, before);
});

test('el motor de audio se inicializa con la entrada simulada (V8)', async () => {
  await app.ensureAudio();
  await app.dialogs.openDevice('v8-id');
  assert.ok(app.engine.ctx, 'debe existir un AudioContext');
  assert.equal(app.engine.ctx.sampleRate, 48000);
  assert.equal(app.engine.input.stream.getAudioTracks()[0].label, 'V8 Audio Device');
  assert.equal(window.document.getElementById('input-strip').hidden, false);
  assert.match(window.document.getElementById('status-device').textContent, /V8/);
});

test('grabar una toma crea un clip alineado y guarda el audio', async () => {
  const trackId = app.project.tracks[0].id;
  app.selectTrack(trackId);
  // Sin cuenta de entrada: esta prueba mide la alineación de la toma.
  app.project.session.metronome.on = false;
  app.seek(2);

  await app.toggleRecord();
  assert.ok(['recording', 'count-in'].includes(app.transport.state), 'debe entrar en grabación');

  // El grabador descarta los 100 ms de pre-roll previos al inicio exacto.
  feedAudio(app.engine.ctx, 1.1);

  await app.stopTransport();

  assert.equal(app.project.clips.length, 1, 'debe existir un clip');
  const clip = app.project.clips[0];
  assert.equal(clip.trackId, trackId);
  assert.ok(clip.duration > 0.85 && clip.duration < 1.2, `duración ${clip.duration}`);
  assert.ok(app.buffers.has(clip.bufferId), 'el buffer debe estar en memoria');
  assert.ok(app.peaks.has(clip.bufferId), 'deben calcularse los picos');
  assert.ok(clip.start <= 2.01, 'la toma se coloca en el cabezal con compensación de latencia');
  // La señal de prueba entra a 0,3 (−10 dBFS): se nivela hasta ≈ −6 dBFS.
  assert.ok(clip.gain > 1, `la toma baja debe nivelarse (ganancia ${clip.gain})`);
  assert.ok(clip.gain <= 8, 'la nivelación no debe pasarse de +18 dB');
});

test('exportar MP3 produce un archivo real y descargable', async () => {
  const format = window.document.getElementById('export-format');
  format.value = 'mp3';
  window.document.getElementById('export-bitrate').value = '320';
  window.document.getElementById('export-master').checked = true;
  window.document.getElementById('export-normalize').checked = false;
  window.document.getElementById('export-stems').checked = false;

  await app.dialogs.runExport();

  assert.ok(app.dialogs.lastExport, 'debe haber un archivo exportado');
  const { blob, filename } = app.dialogs.lastExport;
  assert.match(filename, /\.mp3$/);
  assert.equal(blob.type, 'audio/mpeg');
  assert.ok(blob.size > 5000, `el MP3 pesa ${blob.size} bytes`);

  const bytes = new Uint8Array(await blob.arrayBuffer());
  assert.equal(bytes[0], 0x49, 'etiqueta ID3');
  assert.equal(bytes[1], 0x44);
  assert.equal(bytes[2], 0x33);
  const result = window.document.getElementById('export-result');
  assert.equal(result.hidden, false);
  assert.match(result.textContent, /Mezcla lista/);
  assert.ok((window.__downloads ?? []).length >= 1, 'debe lanzarse la descarga');
});

test('exportar WAV respeta formato y profundidad', async () => {
  window.document.getElementById('export-format').value = 'wav';
  window.document.getElementById('export-depth').value = '24';
  app.dialogs.syncExportFields();
  await app.dialogs.runExport();

  const { blob, filename } = app.dialogs.lastExport;
  assert.match(filename, /\.wav$/);
  assert.equal(blob.type, 'audio/wav');
  const bytes = new Uint8Array(await blob.arrayBuffer());
  const ascii = (offset, length) => String.fromCharCode(...bytes.subarray(offset, offset + length));
  assert.equal(ascii(0, 4), 'RIFF');
  assert.equal(ascii(8, 4), 'WAVE');
  assert.equal(bytes[34], 24, 'profundidad de 24 bits');
});

test('las operaciones de clip: cortar, duplicar, normalizar y borrar', () => {
  const clip = app.project.clips[0];
  app.seek(clip.start + clip.duration / 2);
  app.splitAtPlayhead();
  assert.equal(app.project.clips.length, 2, 'el corte crea dos clips');

  app.selectClip(app.project.clips[0].id);
  app.duplicateSelectedClip();
  assert.equal(app.project.clips.length, 3);

  const before = app.project.clips[0].gain;
  app.selectClip(app.project.clips[0].id);
  app.normalizeSelectedClip();
  assert.ok(app.project.clips[0].gain >= before, 'normalizar sube la ganancia');

  app.deleteSelectedClip();
  assert.equal(app.project.clips.length, 2);
});

test('el proyecto se guarda y se recupera de IndexedDB', async () => {
  await app.save(true);
  assert.equal(app.dirty, false);
  const list = await (await import('../js/storage/db.js')).listProjects();
  assert.ok(list.some((p) => p.id === app.project.id), 'el proyecto debe estar guardado');

  const original = app.project.clips.length;
  await app.newProject();
  assert.equal(app.project.clips.length, 0);

  await app.loadProjectById(list.find((p) => p.id !== app.project.id)?.id ?? list[0].id);
  const restored = app.project.clips.length;
  assert.ok(restored >= 1 || original === 0, 'debe recuperar los clips guardados');
});

test('el diagnóstico informa del entorno y del codificador', async () => {
  await app.dialogs.openDiagnostics();
  const rows = window.document.querySelectorAll('#diag-checks .diag__row');
  assert.ok(rows.length >= 8, `filas de diagnóstico: ${rows.length}`);
  const text = window.document.getElementById('diag-checks').textContent;
  assert.match(text, /Contexto de audio/);
  assert.match(text, /Almacenamiento/);

  await app.dialogs.testEncoder();
  assert.match(window.document.getElementById('diag-log').textContent, /MP3 320 kbps/);
});

test('los medidores se dibujan sin errores y el transporte informa del estado', () => {
  app.engine.ctx.testLevel = 0.4;
  window.flushFrames(3);
  assert.match(window.document.getElementById('status-engine').textContent, /detenido|Reproduciendo|Grabando/i);
  assert.ok(window.document.getElementById('master-peak-label').textContent.length > 0);
});
