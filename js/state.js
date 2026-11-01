/**
 * Modelo de datos del proyecto y utilidades de estado.
 *
 * El proyecto es un objeto plano serializable (salvo el audio, que se guarda
 * aparte en IndexedDB y se referencia por bufferId). Así el historial de
 * deshacer y el autoguardado son baratos y consistentes.
 */

import { uid } from './util/format.js';
import { FX_DEFAULTS, MASTER_DEFAULTS } from './audio/track-chain.js';

export const TRACK_COLORS = [
  '#5b8fb9',
  '#7f9c6b',
  '#b98b5b',
  '#9b7fb0',
  '#5ba89b',
  '#b9705b',
  '#7a8fa6',
  '#a68f5b',
];

/** Crea una pista con todos sus valores por defecto. */
export function createTrack(overrides = {}) {
  const index = overrides.colorIndex ?? 0;
  return {
    id: overrides.id ?? uid('trk'),
    name: overrides.name ?? 'Pista',
    color: overrides.color ?? TRACK_COLORS[index % TRACK_COLORS.length],
    fader: overrides.fader ?? 0.78, // ≈ 0 dB en la curva del fader
    pan: overrides.pan ?? 0,
    muted: overrides.muted ?? false,
    solo: overrides.solo ?? false,
    armed: overrides.armed ?? false,
    inputChannel: overrides.inputChannel ?? 'mono',
    fx: { ...FX_DEFAULTS(), ...(overrides.fx ?? {}) },
  };
}

/** Proyecto nuevo con dos pistas listas para grabar. */
export function createProject(name = 'Sesión sin título') {
  return {
    id: uid('prj'),
    name,
    createdAt: Date.now(),
    updatedAt: Date.now(),
    sampleRate: 48000,
    tracks: [
      createTrack({ name: 'Voz principal', colorIndex: 0 }),
      createTrack({ name: 'Dobles', colorIndex: 1 }),
    ],
    clips: [],
    master: MASTER_DEFAULTS(),
    fxBuses: {
      reverbPreset: 'vocal',
      reverbReturn: 0.9,
      delayTime: 0.375,
      delayFeedback: 0.3,
      delayReturn: 0.8,
    },
    session: {
      bpm: 120,
      timeSig: [4, 4],
      metronome: { on: false, volume: 0.55 },
      loop: { on: false, start: 0, end: 8 },
      snap: 0,
      zoom: 80, // píxeles por segundo
      countInBeats: 4,
      latencyOffset: 0, // ms añadidos a la compensación automática
      recordChannels: 1,
      monitor: false,
    },
  };
}

/** Duración total del proyecto (fin del último clip). */
export function projectDuration(project) {
  let end = 0;
  for (const clip of project.clips) end = Math.max(end, clip.start + clip.duration);
  return end;
}

export function hasSolo(project) {
  return project.tracks.some((t) => t.solo);
}

export function findTrack(project, id) {
  return project.tracks.find((t) => t.id === id) ?? null;
}

export function createClip(overrides) {
  return {
    id: overrides.id ?? uid('clip'),
    trackId: overrides.trackId,
    bufferId: overrides.bufferId,
    name: overrides.name ?? 'Toma',
    start: overrides.start ?? 0,
    offset: overrides.offset ?? 0,
    duration: overrides.duration ?? 0,
    gain: overrides.gain ?? 1,
    fadeIn: overrides.fadeIn ?? 0.005,
    fadeOut: overrides.fadeOut ?? 0.005,
    reversed: overrides.reversed ?? false,
  };
}

/** Rellena campos que falten al cargar un proyecto guardado (migración suave). */
export function normalizeProject(raw) {
  if (!raw) return createProject();
  const base = createProject(raw.name ?? 'Sesión sin título');
  return {
    ...base,
    ...raw,
    master: { ...MASTER_DEFAULTS(), ...(raw.master ?? {}) },
    fxBuses: { ...base.fxBuses, ...(raw.fxBuses ?? {}) },
    session: { ...base.session, ...(raw.session ?? {}) },
    tracks: (raw.tracks ?? base.tracks).map((track, index) => ({
      ...createTrack({ colorIndex: index }),
      ...track,
      fx: { ...FX_DEFAULTS(), ...(track.fx ?? {}) },
    })),
    clips: (raw.clips ?? []).map((clip) => createClip(clip)),
  };
}

/** Copia superficial del proyecto apta para el historial de deshacer. */
export function snapshotProject(project) {
  return JSON.parse(JSON.stringify(project));
}
