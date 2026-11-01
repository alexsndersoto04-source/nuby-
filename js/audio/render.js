/**
 * Mezcla final fuera de línea.
 *
 * Reconstruye la MISMA topología del estudio (pistas → envíos → buses →
 * máster) en un OfflineAudioContext, renderiza más rápido que en tiempo real y
 * devuelve el audio definitivo. No hay simulación: los nodos son los mismos que
 * se usan al escuchar, con los mismos parámetros.
 */

import { buildTrackChain, buildMasterChain, normalizeMaster } from './track-chain.js';
import { createImpulseResponse, REVERB_PRESETS } from './impulse.js';
import { integratedLoudness, truePeakDb, peakDb } from './lufs.js';
import { scheduleEnvelope } from './transport.js';

const WORKLET_FILES = [
  'gate-processor.js',
  'limiter-processor.js',
];

/**
 * @param {object} project
 * @param {object} options
 * @param {Map<string, AudioBuffer>} options.buffers
 * @param {number} [options.start] inicio del rango a exportar
 * @param {number} [options.end] final del rango
 * @param {number} [options.sampleRate]
 * @param {boolean} [options.includeMasterFx]
 * @param {string|null} [options.soloTrackId] renderiza solo esa pista (stems)
 * @param {boolean} [options.normalize] normaliza el pico a -1 dBFS
 * @param {Function} [options.onProgress]
 * @returns {Promise<{buffer: AudioBuffer, lufs: number, peak: number, truePeak: number, start: number, end: number}>}
 */
export async function renderProject(project, options) {
  const buffers = options.buffers;
  const start = Math.max(0, options.start ?? 0);
  const projectEnd = options.end ?? computeProjectEnd(project);
  const end = Math.max(start + 0.05, projectEnd);
  const duration = end - start;
  const sampleRate = options.sampleRate ?? 48000;
  const includeMasterFx = options.includeMasterFx !== false;
  const renderSampleRate = [44100, 48000].includes(sampleRate) ? sampleRate : 48000;

  const masterSettings = normalizeMaster(project.master);
  const limiterOn = includeMasterFx && masterSettings.limiter.on;
  const lookahead = limiterOn ? masterSettings.limiter.lookahead : 0;
  const tail = 0.15 + (project.fxBuses?.reverbReturn ? 0.4 : 0);

  const frames = Math.ceil((duration + lookahead + tail) * renderSampleRate);
  const OfflineCtx = window.OfflineAudioContext ?? window.webkitOfflineAudioContext;
  const offline = new OfflineCtx(2, frames, renderSampleRate);

  // Procesadores avanzados en el render (si el navegador los admite offline).
  let worklets = false;
  try {
    for (const file of WORKLET_FILES) {
      const url = new URL(`./worklets/${file}`, import.meta.url);
      await offline.audioWorklet.addModule(url.href);
    }
    worklets = true;
  } catch {
    worklets = false;
  }

  const masterFx = includeMasterFx
    ? masterSettings
    : { ...masterSettings, comp: { ...masterSettings.comp, on: false }, limiter: { ...masterSettings.limiter, on: false }, eq: { low: 0, mid: 0, high: 0 } };

  const master = buildMasterChain(offline, masterFx, { worklets, safe: true });
  master.output.connect(offline.destination);

  // Buses de efectos
  const fx = project.fxBuses ?? {};
  const reverbInput = offline.createGain();
  const convolver = offline.createConvolver();
  const reverbReturn = offline.createGain();
  reverbReturn.gain.value = fx.reverbReturn ?? 0.9;
  const preset = REVERB_PRESETS[fx.reverbPreset] ?? REVERB_PRESETS.vocal;
  convolver.buffer = createImpulseResponse(offline, preset);
  reverbInput.connect(convolver);
  convolver.connect(reverbReturn);
  reverbReturn.connect(master.input);

  const delayInput = offline.createGain();
  const delayNode = offline.createDelay(2);
  delayNode.delayTime.value = fx.delayTime ?? 0.375;
  const feedback = offline.createGain();
  feedback.gain.value = Math.min(0.85, fx.delayFeedback ?? 0.3);
  const damping = offline.createBiquadFilter();
  damping.type = 'lowpass';
  damping.frequency.value = 4200;
  const delayReturn = offline.createGain();
  delayReturn.gain.value = fx.delayReturn ?? 0.8;
  delayInput.connect(delayNode);
  delayNode.connect(damping);
  damping.connect(feedback);
  feedback.connect(delayNode);
  delayNode.connect(delayReturn);
  delayReturn.connect(master.input);

  const soloActive = project.tracks.some((t) => t.solo);
  const chains = new Map();

  for (const track of project.tracks) {
    const isSolo = options.soloTrackId ? track.id === options.soloTrackId : null;
    const muted =
      options.soloTrackId != null
        ? !isSolo
        : track.muted || (soloActive && !track.solo);
    const chain = buildTrackChain(
      offline,
      { ...track.fx, fader: track.fader, pan: track.pan, muted },
      { worklets, safe: true }
    );
    chain.output.connect(master.input);
    chain.sends.reverb.connect(reverbInput);
    chain.sends.delay.connect(delayInput);
    chains.set(track.id, chain);
  }

  // Colocación de los clips
  for (const clip of project.clips) {
    const track = project.tracks.find((t) => t.id === clip.trackId);
    if (!track) continue;
    if (track.muted) continue;
    if (soloActive && !track.solo && !options.soloTrackId) continue;
    if (options.soloTrackId && clip.trackId !== options.soloTrackId) continue;

    const clipEnd = clip.start + clip.duration;
    const segStart = Math.max(start, clip.start);
    const segEnd = Math.min(end, clipEnd);
    if (segEnd - segStart <= 1e-6) continue;

    const buffer = buffers.get(clip.bufferId);
    if (!buffer) continue;

    const when = segStart - start;
    const startInClip = segStart - clip.start;
    const offset = (clip.offset ?? 0) + startInClip;
    if (offset >= buffer.duration) continue;
    const available = Math.min(segEnd - segStart, buffer.duration - offset);
    if (available <= 0) continue;

    const source = offline.createBufferSource();
    source.buffer = buffer;
    const gain = offline.createGain();
    source.connect(gain);
    gain.connect(chains.get(track.id).input);
    scheduleEnvelope(gain.gain, clip, startInClip, startInClip + available, when);
    source.start(when, offset, available);
  }

  options.onProgress?.(0.05);
  const rendered = await offline.startRendering();
  options.onProgress?.(0.8);

  // El limitador introduce un retardo de "lookahead": se recorta para que la
  // exportación empiece exactamente en el compás 1.
  // Se conserva un poco de cola para que la reverb/delay del final no se corte.
  const tailSeconds = fx.reverbReturn || fx.delayReturn ? 0.35 : 0;
  const skip = Math.round(lookahead * renderSampleRate);
  const wanted = Math.ceil((duration + tailSeconds) * renderSampleRate);
  const length = Math.min(rendered.length - skip, wanted);
  const finalBuffer = makeBuffer(rendered, skip, length);

  let channels = [finalBuffer.getChannelData(0), finalBuffer.getChannelData(1)];

  if (options.normalize) {
    const peak = peakDb(channels);
    if (Number.isFinite(peak)) {
      const gain = Math.pow(10, (-1 - peak) / 20);
      if (gain !== 1) {
        for (const ch of channels) {
          for (let i = 0; i < ch.length; i++) ch[i] *= gain;
        }
      }
    }
  }

  options.onProgress?.(0.9);
  await nextTick();

  const lufs = integratedLoudness(channels, renderSampleRate);
  const truePeak = truePeakDb(channels);

  return {
    buffer: finalBuffer,
    lufs,
    peak: peakDb(channels),
    truePeak,
    start,
    end,
    sampleRate: renderSampleRate,
  };
}

function makeBuffer(source, skip, length) {
  const frames = Math.max(1, length);
  let buffer;
  try {
    buffer = new AudioBuffer({ numberOfChannels: 2, length: frames, sampleRate: source.sampleRate });
  } catch {
    const Ctx = window.OfflineAudioContext ?? window.webkitOfflineAudioContext;
    buffer = new Ctx(2, frames, source.sampleRate).createBuffer(2, frames, source.sampleRate);
  }
  for (let c = 0; c < 2; c++) {
    const from = source.getChannelData(Math.min(c, source.numberOfChannels - 1));
    const to = buffer.getChannelData(c);
    to.set(from.subarray(skip, Math.min(from.length, skip + frames)));
  }
  return buffer;
}

/** Final del proyecto según el último clip. */
export function computeProjectEnd(project) {
  let end = 0;
  for (const clip of project.clips) end = Math.max(end, clip.start + clip.duration);
  return end;
}

function nextTick() {
  return new Promise((resolve) => setTimeout(resolve, 0));
}
