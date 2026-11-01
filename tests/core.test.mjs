/**
 * Pruebas automáticas del núcleo (sin navegador).
 * Ejecutar: node --test tests/
 */
import test from 'node:test';
import assert from 'node:assert/strict';

import { encodeWav } from '../js/audio/wav.js';
import { encodeMp3, buildId3Tags, readId3Tags } from '../js/audio/mp3.js';
import { integratedLoudness, kWeightingCoefficients, peakDb, truePeakDb } from '../js/audio/lufs.js';
import { computePeaks } from '../js/audio/peaks.js';
import { clipEnvelope, scheduleEnvelope } from '../js/audio/transport.js';
import { faderToGain, gainToFader, formatTime, safeFileName, snapTo } from '../js/util/format.js';
import { createProject, createTrack, projectDuration, normalizeProject } from '../js/state.js';

const SR = 48000;

function sine(freq, seconds, amplitude = 0.5, rate = SR) {
  const n = Math.round(seconds * rate);
  const data = new Float32Array(n);
  for (let i = 0; i < n; i++) data[i] = Math.sin((2 * Math.PI * freq * i) / rate) * amplitude;
  return data;
}

test('WAV 24 bits: cabecera y datos correctos', () => {
  const channels = [sine(440, 0.1), sine(440, 0.1)];
  const buffer = encodeWav(channels, SR, { bitDepth: 24 });
  const view = new DataView(buffer);
  const ascii = (o, n) => String.fromCharCode(...new Uint8Array(buffer, o, n));

  assert.equal(ascii(0, 4), 'RIFF');
  assert.equal(ascii(8, 4), 'WAVE');
  assert.equal(ascii(36, 4), 'data');
  assert.equal(view.getUint16(20, true), 1, 'formato PCM');
  assert.equal(view.getUint16(22, true), 2, 'estéreo');
  assert.equal(view.getUint32(24, true), SR);
  assert.equal(view.getUint16(34, true), 24);
  const frames = channels[0].length;
  assert.equal(view.getUint32(40, true), frames * 2 * 3);
  assert.equal(buffer.byteLength, 44 + frames * 6);
  assert.equal(view.getUint32(4, true), buffer.byteLength - 8);
});

test('WAV 16 bits: cuantización con signo correcto', () => {
  const data = new Float32Array([0, 1, -1, 0.5, -0.5]);
  const buffer = encodeWav([data], 8000, { bitDepth: 16 });
  const view = new DataView(buffer);
  const samples = [];
  for (let i = 0; i < 5; i++) samples.push(view.getInt16(44 + i * 2, true));
  assert.deepEqual(samples, [0, 32767, -32768, 16384, -16384]);
});

test('MP3 320 kbps: estructura de tramas válida y duración correcta', async () => {
  const seconds = 1.5;
  const left = sine(440, seconds, 0.5);
  const right = sine(660, seconds, 0.5);
  const blob = await encodeMp3([left, right], SR, { bitrate: 320 });
  const bytes = new Uint8Array(await blob.arrayBuffer());
  assert.ok(bytes.length > 50000, 'el MP3 no puede estar vacío');

  const id3 = readId3Tags(bytes);
  assert.equal(id3, null, 'sin etiquetas si no se piden');

  const first = findFirstFrame(bytes);
  assert.ok(first, 'debe haber al menos una trama MPEG');
  assert.equal(first.sampleRate, SR);
  assert.equal(first.bitrate, 320000);
  assert.equal(first.channels, 2);

  const frames = countFrames(bytes);
  const duration = (frames * 1152) / SR;
  assert.ok(Math.abs(duration - seconds) < 0.15, `duración ${duration} cerca de ${seconds}`);
});

test('MP3 mono con etiquetas ID3', async () => {
  const mono = sine(220, 0.5, 0.4);
  const blob = await encodeMp3([mono], SR, {
    bitrate: 192,
    tags: { title: 'Prueba', artist: 'Nuby' },
  });
  const bytes = new Uint8Array(await blob.arrayBuffer());
  const id3 = readId3Tags(bytes);
  assert.ok(id3, 'debe llevar ID3');
  const header = new TextDecoder('latin1').decode(bytes.subarray(10, 60));
  assert.ok(header.includes('TIT2'), 'etiqueta de título');
  assert.ok(header.includes('TPE1'), 'etiqueta de artista');
  const frame = findFirstFrame(bytes.subarray(10 + id3.size));
  assert.equal(frame.channels, 1);
});

test('ID3 sincsafe en lugar de bytes grandes', () => {
  const tags = buildId3Tags({ title: 'x'.repeat(300) });
  assert.ok(tags);
  for (let i = 6; i < 10; i++) assert.ok(tags[i] < 0x80, 'tamaño synchsafe');
});

test('LUFS: seno de -20 dBFS estéreo ≈ -20 LUFS', () => {
  const amp = Math.pow(10, -20 / 20);
  const seconds = 3;
  const left = sine(997, seconds, amp);
  const right = sine(997, seconds, amp);
  const lufs = integratedLoudness([left, right], SR);
  assert.ok(Math.abs(lufs - -20) < 1.0, `LUFS = ${lufs}`);
});

test('LUFS: el silencio no devuelve un número inventado', () => {
  const silence = new Float32Array(SR);
  const lufs = integratedLoudness([silence, silence], SR);
  assert.equal(lufs, -Infinity);
});

test('LUFS: -6 dB más de nivel = +6 LU', () => {
  const a = integratedLoudness([sine(997, 2, 0.1), sine(997, 2, 0.1)], SR);
  const b = integratedLoudness([sine(997, 2, 0.2), sine(997, 2, 0.2)], SR);
  assert.ok(Math.abs((b - a) - 6) < 0.4, `diferencia ${b - a}`);
});

test('Coeficientes de ponderación K para varias frecuencias', () => {
  for (const rate of [44100, 48000, 96000]) {
    const { shelf, highpass } = kWeightingCoefficients(rate);
    assert.equal(shelf.b.length, 3);
    assert.equal(shelf.a.length, 3);
    assert.ok(Math.abs(shelf.a[0] - 1) < 1e-9, 'a0 normalizado a 1');
    assert.ok(highpass.b[0] > 0);
  }
});

test('Picos y True Peak detectan el nivel', () => {
  const data = new Float32Array([0.5, -0.9, 0.1]);
  assert.ok(Math.abs(peakDb([data]) - 20 * Math.log10(0.9)) < 1e-6);
  assert.ok(truePeakDb([data]) >= peakDb([data]));
});

test('Picos por bloque para dibujar la onda', () => {
  const data = new Float32Array(1000);
  data[10] = 0.8;
  data[11] = -0.6;
  data[500] = 0.4;
  const peaks = computePeaks([data], 100);
  assert.equal(peaks.length, 10);
  assert.ok(Math.abs(peaks.max[0] - 0.8) < 1e-6);
  assert.ok(Math.abs(peaks.min[0] - -0.6) < 1e-6);
  assert.ok(Math.abs(peaks.max[5] - 0.4) < 1e-6);
  assert.ok(Math.abs(peaks.peak - 0.8) < 1e-6);
});

test('Envolvente del clip: fades lineales exactos', () => {
  const clip = { duration: 10, gain: 1, fadeIn: 1, fadeOut: 2 };
  assert.equal(clipEnvelope(clip, 0), 0);
  assert.ok(Math.abs(clipEnvelope(clip, 0.5) - 0.5) < 1e-6);
  assert.equal(clipEnvelope(clip, 5), 1);
  assert.ok(Math.abs(clipEnvelope(clip, 9) - 0.5) < 1e-6);
  assert.equal(clipEnvelope(clip, 10), 0);
});

test('scheduleEnvelope escribe rampas en el AudioParam', () => {
  const calls = [];
  const param = {
    setValueAtTime: (v, t) => calls.push(['set', v, t]),
    linearRampToValueAtTime: (v, t) => calls.push(['ramp', v, t]),
  };
  const clip = { duration: 4, gain: 1, fadeIn: 1, fadeOut: 1 };
  scheduleEnvelope(param, clip, 0, 4, 10);
  assert.equal(calls[0][0], 'set');
  assert.equal(calls[0][1], 0);
  const last = calls[calls.length - 1];
  assert.equal(last[0], 'ramp');
  assert.ok(Math.abs(last[2] - 14) < 1e-6, 'termina en el tiempo esperado');
});

test('Escala del fader: ida y vuelta consistente', () => {
  for (const position of [0, 0.25, 0.5, 0.78, 1]) {
    const gain = faderToGain(position);
    const back = gainToFader(gain);
    if (position === 0) {
      assert.equal(back, 0);
    } else {
      assert.ok(Math.abs(back - position) < 0.02, `posición ${position} → ${back}`);
    }
  }
  assert.equal(faderToGain(0), 0);
  assert.ok(faderToGain(0.8) > 0.999 && faderToGain(0.8) < 1.001, '0.8 del fader = 0 dB');
  assert.ok(faderToGain(1) > 1.9 && faderToGain(1) < 2.01, 'el máximo es +6 dB');
});

test('Formato de tiempo y nombres de archivo', () => {
  assert.equal(formatTime(0), '0:00.00');
  assert.equal(formatTime(65.5), '1:05.50');
  assert.equal(formatTime(3600), '60:00.00');
  assert.equal(safeFileName('Mi canción (versión 2)'), 'Mi-cancion-version-2');
  assert.equal(safeFileName(''), 'mezcla');
  assert.equal(snapTo(1.24, 0.25), 1.25);
});

test('Modelo del proyecto: pistas, clips y normalización', () => {
  const project = createProject('Demo');
  assert.equal(project.tracks.length, 2);
  assert.ok(project.tracks[0].fx.eq.bands.length === 4);
  project.clips.push({ trackId: project.tracks[0].id, start: 2, duration: 3 });
  assert.equal(projectDuration(project), 5);

  const track = createTrack({ name: 'Guitarra', colorIndex: 3 });
  assert.equal(track.name, 'Guitarra');
  assert.ok(track.color.startsWith('#'));

  // Cargar un proyecto incompleto no debe romper nada.
  const restored = normalizeProject({ name: 'Viejo', tracks: [{ id: 't1', name: 'X' }] });
  assert.equal(restored.master.limiter.on, true);
  assert.equal(restored.tracks[0].fx.eq.bands.length, 4);
  assert.equal(restored.tracks[0].fader, 0.78);
});

// ---------------------------------------------------------------- utilidades
function findFirstFrame(bytes) {
  for (let i = 0; i + 4 < bytes.length; i++) {
    if (bytes[i] === 0xff && (bytes[i + 1] & 0xe0) === 0xe0) {
      const version = (bytes[i + 1] >> 3) & 3;
      const bitrateIndex = (bytes[i + 2] >> 4) & 15;
      const sampleRateIndex = (bytes[i + 2] >> 2) & 3;
      const channelMode = (bytes[i + 3] >> 6) & 3;
      const bitrates = [0, 32, 40, 48, 56, 64, 80, 96, 112, 128, 160, 192, 224, 256, 320, 0];
      const rates = [44100, 48000, 32000, 0];
      if (version !== 3 || !rates[sampleRateIndex] || !bitrates[bitrateIndex]) continue;
      return {
        bitrate: bitrates[bitrateIndex] * 1000,
        sampleRate: rates[sampleRateIndex],
        channels: channelMode === 3 ? 1 : 2,
      };
    }
  }
  return null;
}

function countFrames(bytes) {
  let count = 0;
  for (let i = 0; i + 4 < bytes.length; i++) {
    if (bytes[i] === 0xff && (bytes[i + 1] & 0xe0) === 0xe0) {
      const sampleRateIndex = (bytes[i + 2] >> 2) & 3;
      const bitrateIndex = (bytes[i + 2] >> 4) & 15;
      const padding = (bytes[i + 2] >> 1) & 1;
      const bitrates = [0, 32, 40, 48, 56, 64, 80, 96, 112, 128, 160, 192, 224, 256, 320, 0];
      const rates = [44100, 48000, 32000, 0];
      if (!rates[sampleRateIndex] || !bitrates[bitrateIndex]) continue;
      const length = Math.floor((144 * bitrates[bitrateIndex] * 1000) / rates[sampleRateIndex]) + padding;
      if (length < 4) continue;
      count++;
      i += length - 1;
    }
  }
  return count;
}
