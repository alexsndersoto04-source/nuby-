/**
 * Aplicación: une motor de audio, modelo, vistas y teclado.
 */

import { StudioEngine } from '../audio/engine.js';
import { Transport } from '../audio/transport.js';
import { computePeaks, detectSignalRegion } from '../audio/peaks.js';
import {
  createProject,
  createTrack,
  createClip,
  normalizeProject,
  snapshotProject,
  findTrack,
  hasSolo,
  projectDuration,
} from '../state.js';
import * as db from '../storage/db.js';
import { getSetting, getSettings } from '../state/settings.js';
import { encodeWav } from '../audio/wav.js';
import { TrackStrips } from './strips.js';
import { TimelineView } from './timeline.js';
import { Inspector } from './inspector.js';
import { Dialogs } from './dialogs.js';
import { h, clear, toast, openDialog, drawMeter, isNativeApp } from './dom.js';
import { formatTime, formatDb, uid, clamp } from '../util/format.js';

/** Crea un AudioBuffer sin AudioContext (navegadores modernos y respaldo). */
function createDetachedBuffer(numberOfChannels, length, sampleRate) {
  try {
    return new AudioBuffer({ numberOfChannels, length, sampleRate });
  } catch {
    const Ctx = window.OfflineAudioContext ?? window.webkitOfflineAudioContext;
    return new Ctx(numberOfChannels, Math.max(1, length), sampleRate).createBuffer(
      numberOfChannels,
      Math.max(1, length),
      sampleRate
    );
  }
}

export class StudioApp {
  constructor() {
    this.project = createProject();
    this.buffers = new Map(); // bufferId -> AudioBuffer
    this.peaks = new Map(); // bufferId -> {min,max,length,peak,bucket}
    this.selectedTrackId = null;
    this.selectedClipId = null;
    this.engine = null;
    this.transport = null;
    this.dialogs = null;
    this.tool = 'select';
    this.playheadPosition = 0;
    this.undoStack = [];
    this.redoStack = [];
    this.dirty = false;
    this.saveTimer = null;
    this.recordingTrackId = null;
    this.inputGain = 1;
    this.lastMeterUpdate = 0;
    this.lastTextUpdate = 0;
    this.countInUntil = 0;
    this.autoSave = true; // se ajusta con las preferencias al arrancar
  }

  // ------------------------------------------------------------------ Arranque
  async boot() {
    this.cacheElements();
    this.buildViews();

    const lastId = localStorage.getItem('ask:lastProject');
    if (lastId) {
      try {
        const saved = await db.loadProject(lastId);
        if (saved) {
          this.project = normalizeProject(saved);
          await this.restoreBuffers(this.project);
        }
      } catch (error) {
        console.warn('No se pudo abrir el último proyecto', error);
      }
    }

    this.selectedTrackId = this.project.tracks[0]?.id ?? null;
    this.autoSave = Boolean(getSetting('autoSave'));
    this.applyPreferences();
    this.applyProjectToUi();
    this.bindEvents();
    this.bindKeyboard();
    this.registerServiceWorker();

    document.getElementById('app').setAttribute('aria-busy', 'false');
    this.startAnimationLoop();
    this.refreshStorageInfo();

    window.addEventListener('beforeunload', (event) => {
      if (!this.dirty) return;
      event.preventDefault();
      event.returnValue = '';
    });
    window.addEventListener('unhandledrejection', (event) => {
      console.error(event.reason);
      toast(`Error inesperado: ${event.reason?.message ?? event.reason}`, 'error', 6000);
    });
  }

  cacheElements() {
    this.els = {
      app: document.getElementById('app'),
      projectName: document.getElementById('project-name'),
      saveState: document.getElementById('save-state'),
      time: document.getElementById('time-display'),
      play: document.getElementById('btn-play'),
      stop: document.getElementById('btn-stop'),
      record: document.getElementById('btn-record'),
      rewind: document.getElementById('btn-rewind'),
      metronome: document.getElementById('btn-metronome'),
      loop: document.getElementById('btn-loop'),
      bpm: document.getElementById('bpm-input'),
      timeSig: document.getElementById('timesig-select'),
      masterMeter: document.getElementById('master-meter'),
      masterPeak: document.getElementById('master-peak-label'),
      lufs: document.getElementById('lufs-label'),
      trackList: document.getElementById('track-list'),
      lanes: document.getElementById('lanes'),
      ruler: document.getElementById('ruler'),
      lanesWrap: document.getElementById('lanes-wrap'),
      playhead: document.getElementById('playhead'),
      loopRegion: document.getElementById('loop-region'),
      emptyHint: document.getElementById('empty-hint'),
      snap: document.getElementById('snap-select'),
      statusDevice: document.getElementById('status-device'),
      statusSr: document.getElementById('status-sr'),
      statusLatency: document.getElementById('status-latency'),
      statusStorage: document.getElementById('status-storage'),
      statusEngine: document.getElementById('status-engine'),
      inputStrip: document.getElementById('input-strip'),
      inputStripLabel: document.getElementById('input-strip-label'),
      inputMeter: document.getElementById('input-meter'),
      inputPeak: document.getElementById('input-peak-label'),
      inputGain: document.getElementById('input-gain'),
      monitor: document.getElementById('btn-monitor'),
    };
  }

  buildViews() {
    this.strips = new TrackStrips({
      container: this.els.trackList,
      engine: null,
      callbacks: {
        onSelect: (id) => this.selectTrack(id),
        onArm: (id, armed) => this.setArmed(id, armed),
        onMute: (id, muted) => this.patchTrack(id, { muted }),
        onSolo: (id, solo) => this.patchTrack(id, { solo }),
        onFader: (id, value) => this.patchTrack(id, { fader: value }, { silent: true }),
        onPan: (id, value) => this.patchTrack(id, { pan: value }, { silent: true }),
        onRename: (id, name) => this.patchTrack(id, { name }, { rebuildInspector: true }),
        onRemove: (id) => this.removeTrack(id),
      },
    });

    this.timeline = new TimelineView({
      elements: {
        lanes: this.els.lanes,
        ruler: this.els.ruler,
        wrap: this.els.lanesWrap,
        playhead: this.els.playhead,
        loop: this.els.loopRegion,
        empty: this.els.emptyHint,
      },
      callbacks: {
        onSeek: (position) => this.seek(position),
        onSelectTrack: (id) => this.selectTrack(id),
        onSelectClip: (id) => this.selectClip(id),
        onLoopChange: (loop) => this.setLoop(loop),
        onClipChange: (clip) => this.onClipDragged(clip),
        onEditStart: (label) => this.pushUndo(label),
        onEditEnd: () => this.afterEdit('Editar clip'),
      },
    });

    this.inspector = new Inspector({
      panelTrack: document.getElementById('panel-track'),
      panelMaster: document.getElementById('panel-master'),
      panelSession: document.getElementById('panel-session'),
      panelHelp: document.getElementById('panel-help'),
      callbacks: {
        onFxChange: (trackId) => this.onFxChange(trackId),
        onMasterChange: () => this.onMasterChange(),
        onSessionChange: () => this.onSessionChange(),
        onInputChannel: (trackId, value) => this.patchTrack(trackId, { inputChannel: value }),
        onReverbPreset: (preset) => this.setReverbPreset(preset),
        onFxBusChange: (patch) => this.onFxBusChange(patch),
        onZoomChange: (zoom) => this.setZoom(zoom),
        onClearSession: () => this.clearSession(),
      },
    });

    this.dialogs = new Dialogs({ app: this });
  }

  get selectedTrack() {
    return findTrack(this.project, this.selectedTrackId);
  }

  // ------------------------------------------------------------------ Motor
  async ensureAudio() {
    if (this.engine?.ctx) {
      await this.engine.resume();
      return this.engine;
    }
    this.els.statusEngine.textContent = 'Iniciando motor…';
    this.engine = new StudioEngine();
    await this.engine.init();
    this.engine.setMaster(this.project.master);
    this.engine.setReverbPreset(this.project.fxBuses.reverbPreset);
    this.engine.setDelayTime(this.project.fxBuses.delayTime);
    this.engine.setDelayFeedback(this.project.fxBuses.delayFeedback);
    if (this.engine.reverbReturn) {
      this.engine.reverbReturn.gain.value = this.project.fxBuses.reverbReturn ?? 0.9;
    }
    if (this.engine.delayReturn) {
      this.engine.delayReturn.gain.value = this.project.fxBuses.delayReturn ?? 0.8;
    }

    this.transport = new Transport(this.engine, {
      getProject: () => this.project,
      getBuffer: (id) => this.buffers.get(id),
      onStateChange: (state) => this.onTransportState(state),
    });
    this.transport.setBpm(this.project.session.bpm);
    this.transport.setLoop(this.project.session.loop);

    this.strips.engine = this.engine;
    this.syncEngineTracks();
    this.updateStatusBar();
    if (this.engine.workletError) {
      toast(this.engine.workletError, 'warn', 7000);
    }
    return this.engine;
  }

  syncEngineTracks() {
    if (!this.engine) return;
    const ids = new Set(this.project.tracks.map((t) => t.id));
    for (const id of [...this.engine.tracks.keys()]) {
      if (!ids.has(id)) this.engine.removeTrack(id);
    }
    const soloActive = hasSolo(this.project);
    for (const track of this.project.tracks) {
      if (!this.engine.tracks.has(track.id)) this.engine.addTrack(track);
      else this.engine.updateTrack(track, soloActive);
    }
    const armed = this.project.tracks.filter((t) => t.armed).map((t) => t.id);
    this.engine.setArmedTracks(armed);
  }

  // ------------------------------------------------------------------ Proyecto
  applyProjectToUi() {
    this.els.projectName.value = this.project.name;
    this.els.bpm.value = String(this.project.session.bpm);
    this.els.timeSig.value = `${this.project.session.timeSig[0]}/${this.project.session.timeSig[1]}`;
    this.els.metronome.setAttribute('aria-pressed', String(Boolean(this.project.session.metronome?.on)));
    this.els.loop.setAttribute('aria-pressed', String(Boolean(this.project.session.loop?.on)));
    const snapValue = String(this.project.session.snap ?? 0);
    if (![...this.els.snap.options].some((o) => o.value === snapValue)) {
      this.els.snap.value = '0';
    } else {
      this.els.snap.value = snapValue;
    }
    this.updateSaveState();
    this.renderAll();
    this.updateLoopUi();
  }

  renderAll() {
    this.strips.setProject(this.project);
    this.strips.setSelected(this.selectedTrackId);
    this.strips.render();
    this.inspector.setProject(this.project);
    this.inspector.setSelectedTrack(this.selectedTrackId);
    this.inspector.render();
    this.timeline.setData({
      project: this.project,
      buffers: this.buffers,
      peaks: this.peaks,
      zoom: this.project.session.zoom,
      selectedClipId: this.selectedClipId,
      selectedTrackId: this.selectedTrackId,
    });
    this.timeline.render();
    this.timeline.updatePlayhead(this.playheadPosition);
    this.updateClipTools();
    this.updateArrangeInfo();
    this.syncEngineTracks();
  }

  renderTimelineOnly() {
    this.timeline.setData({
      project: this.project,
      buffers: this.buffers,
      peaks: this.peaks,
      zoom: this.project.session.zoom,
      selectedClipId: this.selectedClipId,
      selectedTrackId: this.selectedTrackId,
    });
    this.timeline.render();
    this.timeline.updatePlayhead(this.playheadPosition);
  }

  /** Aplica las preferencias guardadas (densidad, seguimiento, ganancia…). */
  applyPreferences() {
    const prefs = getSettings();
    document.body.dataset.density = String(prefs.density ?? 'comoda');
    this.inputGain = Number(prefs.inputGain ?? 1);
    if (this.els?.inputGain) this.els.inputGain.value = String(this.inputGain);
    if (this.timeline) this.timeline.follow = Boolean(prefs.autoScroll);
    this.dialogs?.applyAllSettings?.();
  }

  /** Ganancia de entrada desde cualquier sitio (ajustes, pista, diálogo). */
  setInputGain(value) {
    const gain = Math.max(0.05, Math.min(8, Number(value) || 1));
    this.inputGain = gain;
    if (this.els?.inputGain) this.els.inputGain.value = String(gain);
    const label = document.getElementById('input-gain-label');
    if (label) label.textContent = `Ganancia ${formatDb(20 * Math.log10(gain))}`;
    this.engine?.setInputGain(gain);
    return gain;
  }

  markDirty() {
    if (!this.autoSave) {
      this.dirty = true;
      this.updateSaveState();
      return;
    }
    this.dirty = true;
    this.updateSaveState();
    clearTimeout(this.saveTimer);
    this.saveTimer = setTimeout(() => this.save(), 1400);
  }

  updateSaveState() {
    if (!this.els.saveState) return;
    this.els.saveState.textContent = this.dirty ? 'Cambios sin guardar' : 'Guardado';
    this.els.saveState.classList.toggle('is-dirty', this.dirty);
    if (!this.dirty && this.project.updatedAt) {
      const date = new Date(this.project.updatedAt);
      this.els.saveState.textContent = `Guardado ${date.getHours()}:${String(date.getMinutes()).padStart(2, '0')}`;
    }
  }

  async save(immediate = false) {
    clearTimeout(this.saveTimer);
    try {
      this.project.updatedAt = Date.now();
      this.project.duration = projectDuration(this.project);
      await db.saveProject(JSON.parse(JSON.stringify(this.project)));
      localStorage.setItem('ask:lastProject', this.project.id);
      this.dirty = false;
      this.updateSaveState();
      return true;
    } catch (error) {
      console.error(error);
      if (immediate) toast(`No se pudo guardar: ${error.message}`, 'error', 6000);
      return false;
    }
  }

  async restoreBuffers(project) {
    const ids = [...new Set(project.clips.map((clip) => clip.bufferId))];
    let missing = 0;
    for (const id of ids) {
      if (this.buffers.has(id)) continue;
      const record = await db.getBuffer(id);
      if (!record) {
        missing++;
        continue;
      }
      try {
        const ctx = this.engine?.ctx ?? null;
        const buffer = ctx
          ? ctx.createBuffer(record.channels.length, record.channels[0].length, record.sampleRate)
          : createDetachedBuffer(record.channels.length, record.channels[0].length, record.sampleRate);
        record.channels.forEach((channel, index) => buffer.copyToChannel(channel, index));
        this.buffers.set(id, buffer);
        this.peaks.set(id, computePeaks(record.channels, 256));
      } catch (error) {
        missing++;
        console.warn('Buffer dañado', id, error);
      }
    }
    if (missing) {
      toast(
        `${missing} audio(s) del proyecto no están en este equipo. Los clips aparecerán vacíos.`,
        'warn',
        8000
      );
    }
  }

  async newProject() {
    if (this.dirty && !confirm('Hay cambios sin guardar. ¿Crear un proyecto nuevo igualmente?')) return;
    this.project = createProject();
    this.buffers.clear();
    this.peaks.clear();
    this.selectedTrackId = this.project.tracks[0].id;
    this.selectedClipId = null;
    this.playheadPosition = 0;
    this.undoStack = [];
    this.redoStack = [];
    this.applyProjectToUi();
    this.updateUndoButtons();
    await this.save(true);
    toast('Proyecto nuevo creado', 'ok');
  }

  async loadProjectById(id) {
    const saved = await db.loadProject(id);
    if (!saved) {
      toast('No se encontró el proyecto', 'error');
      return;
    }
    this.project = normalizeProject(saved);
    this.buffers.clear();
    this.peaks.clear();
    await this.restoreBuffers(this.project);
    this.selectedTrackId = this.project.tracks[0]?.id ?? null;
    this.selectedClipId = null;
    this.playheadPosition = 0;
    this.undoStack = [];
    this.redoStack = [];
    this.applyProjectToUi();
    this.updateUndoButtons();
    localStorage.setItem('ask:lastProject', this.project.id);
    if (this.engine) {
      this.transport.setLoop(this.project.session.loop);
      this.transport.setBpm(this.project.session.bpm);
    }
    toast(`Proyecto abierto: ${this.project.name}`, 'ok');
  }

  // ------------------------------------------------------------------ Edición
  pushUndo(label = 'Cambio') {
    this.undoStack.push({ project: snapshotProject(this.project), label });
    if (this.undoStack.length > 60) this.undoStack.shift();
    this.redoStack = [];
    this.updateUndoButtons();
  }

  undo() {
    const entry = this.undoStack.pop();
    if (!entry) return;
    this.redoStack.push(snapshotProject(this.project));
    this.project = normalizeProject(entry.project);
    this.afterHistoryRestore();
    toast(`Deshecho: ${entry.label}`, 'info', 1800);
  }

  redo() {
    const entry = this.redoStack.pop();
    if (!entry) return;
    this.undoStack.push(snapshotProject(this.project));
    this.project = normalizeProject(entry);
    this.afterHistoryRestore();
    toast('Rehecho', 'info', 1500);
  }

  afterHistoryRestore() {
    if (!findTrack(this.project, this.selectedTrackId)) {
      this.selectedTrackId = this.project.tracks[0]?.id ?? null;
    }
    if (!this.project.clips.some((c) => c.id === this.selectedClipId)) this.selectedClipId = null;
    this.applyProjectToUi();
    this.updateUndoButtons();
    this.markDirty();
  }

  updateUndoButtons() {
    const undoBtn = document.getElementById('btn-undo');
    const redoBtn = document.getElementById('btn-redo');
    if (undoBtn) undoBtn.disabled = this.undoStack.length === 0;
    if (redoBtn) redoBtn.disabled = this.redoStack.length === 0;
  }

  selectTrack(id) {
    if (!id || id === this.selectedTrackId) return;
    this.selectedTrackId = id;
    this.strips.setSelected(id);
    this.inspector.setSelectedTrack(id);
    this.inspector.renderTrack();
    this.timeline.setData({
      project: this.project,
      buffers: this.buffers,
      peaks: this.peaks,
      zoom: this.project.session.zoom,
      selectedClipId: this.selectedClipId,
      selectedTrackId: id,
    });
    // Solo se resalta la pista: reconstruir las pistas destruiría el clip que
    // se está arrastrando en ese instante.
    for (const lane of document.querySelectorAll('.lane')) {
      lane.classList.toggle('is-selected', lane.dataset.trackId === id);
    }
    this.updateArrangeInfo();
  }

  selectClip(id) {
    this.selectedClipId = id;
    for (const node of document.querySelectorAll('.clip')) {
      node.classList.toggle('is-selected', node.dataset.clipId === id);
    }
    this.updateClipTools();
    const clip = this.project.clips.find((c) => c.id === id);
    if (clip) this.selectTrack(clip.trackId);
  }

  updateClipTools() {
    const tools = document.getElementById('clip-tools');
    if (tools) tools.hidden = !this.selectedClipId;
    const name = document.getElementById('clip-tools-name');
    const clip = this.project.clips.find((c) => c.id === this.selectedClipId);
    if (name && clip) name.textContent = clip.name || 'Toma';
  }

  updateArrangeInfo() {
    const node = document.getElementById('arrange-info');
    if (!node) return;
    const duration = projectDuration(this.project);
    const zoom = Math.round(this.project.session.zoom);
    const snapBeats = this.project.session.snap ?? 0;
    node.textContent = `${formatTime(duration, false)} · ${this.project.clips.length} clips · ${zoom} px/s · ajuste ${
      snapBeats ? `${snapBeats} tiempo(s)` : 'libre'
    }`;
  }

  patchTrack(id, patch, options = {}) {
    const track = findTrack(this.project, id);
    if (!track) return;
    const label = options.label ?? (patch.name ? 'Renombrar pista' : 'Ajustar pista');
    if (!options.silent) this.pushUndo(label);
    Object.assign(track, patch);
    this.syncEngineTracks();
    this.strips.syncTrack(track);
    if (options.rebuildInspector || patch.name) {
      this.inspector.renderTrack();
      this.strips.render();
      this.strips.setSelected(this.selectedTrackId);
    }
    this.markDirty();
  }

  addTrack() {
    this.pushUndo('Añadir pista');
    const index = this.project.tracks.length;
    const track = createTrack({ name: `Pista ${index + 1}`, colorIndex: index });
    this.project.tracks.push(track);
    this.selectedTrackId = track.id;
    this.renderAll();
    this.markDirty();
    toast(`Pista "${track.name}" añadida`, 'ok', 1800);
  }

  removeTrack(id) {
    const track = findTrack(this.project, id);
    if (!track) return;
    const clips = this.project.clips.filter((c) => c.trackId === id);
    if (clips.length && !confirm(`La pista "${track.name}" tiene ${clips.length} clip(s). ¿Eliminarla?`)) return;
    this.pushUndo('Eliminar pista');
    this.project.tracks = this.project.tracks.filter((t) => t.id !== id);
    this.project.clips = this.project.clips.filter((c) => c.trackId !== id);
    if (this.selectedTrackId === id) this.selectedTrackId = this.project.tracks[0]?.id ?? null;
    if (this.project.tracks.length === 0) this.addTrackInternal();
    this.renderAll();
    this.markDirty();
  }

  addTrackInternal() {
    const track = createTrack({ name: 'Pista 1', colorIndex: 0 });
    this.project.tracks.push(track);
    this.selectedTrackId = track.id;
  }

  setArmed(id, armed) {
    if (armed) {
      // Solo se arma la pista pulsada: evita grabar en dos pistas por error.
      for (const track of this.project.tracks) track.armed = track.id === id;
    } else {
      const track = findTrack(this.project, id);
      if (track) track.armed = false;
    }
    this.syncEngineTracks();
    this.strips.render();
    this.strips.setSelected(this.selectedTrackId);
    this.markDirty();
  }

  onFxChange(trackId) {
    const track = findTrack(this.project, trackId);
    if (!track) return;
    this.engine?.updateTrack(track, hasSolo(this.project));
    this.markDirty();
  }

  onMasterChange() {
    this.engine?.setMaster(this.project.master);
    this.markDirty();
  }

  onSessionChange() {
    this.els.bpm.value = String(this.project.session.bpm);
    this.transport?.setBpm(this.project.session.bpm);
    this.transport?.setLoop(this.project.session.loop);
    this.engine?.metronome.setVolume(this.project.session.metronome?.volume ?? 0.55);
    this.renderTimelineOnly();
    this.updateArrangeInfo();
    this.markDirty();
    // Ojo: aquí NO se re-renderiza el panel de sesión; hacerlo destruiría el
    // control que el usuario está arrastrando en ese momento.
  }

  onFxBusChange(patch) {
    Object.assign(this.project.fxBuses, patch);
    this.engine?.setDelayTime(this.project.fxBuses.delayTime);
    this.engine?.setDelayFeedback(this.project.fxBuses.delayFeedback);
    if (this.engine?.reverbReturn) this.engine.reverbReturn.gain.value = this.project.fxBuses.reverbReturn;
    if (this.engine?.delayReturn) this.engine.delayReturn.gain.value = this.project.fxBuses.delayReturn;
    this.markDirty();
  }

  setReverbPreset(preset) {
    this.project.fxBuses.reverbPreset = preset;
    this.engine?.setReverbPreset(preset);
    this.markDirty();
  }

  onClipDragged(clip) {
    this.timeline.updateClipTools?.();
    this.updateArrangeInfo();
  }

  afterEdit() {
    this.markDirty();
    this.renderTimelineOnly();
  }

  setZoom(zoom) {
    this.project.session.zoom = clamp(zoom, 15, 600);
    this.timeline.setData({
      project: this.project,
      buffers: this.buffers,
      peaks: this.peaks,
      zoom: this.project.session.zoom,
      selectedClipId: this.selectedClipId,
      selectedTrackId: this.selectedTrackId,
    });
    this.timeline.render();
    this.updateArrangeInfo();
    this.markDirty();
  }

  seek(position) {
    if (this.transport?.state === 'recording' || this.transport?.state === 'count-in') return;
    const target = Math.max(0, position);
    this.playheadPosition = target;
    if (this.transport?.isPlaying()) {
      this.transport.seek(target);
    }
    this.timeline.updatePlayhead(target, this.transport?.state === 'recording');
  }

  setLoop(loop) {
    this.project.session.loop = loop;
    this.transport?.setLoop(loop);
    this.els.loop.setAttribute('aria-pressed', String(Boolean(loop.on)));
    this.timeline.setData({
      project: this.project,
      buffers: this.buffers,
      peaks: this.peaks,
      zoom: this.project.session.zoom,
      selectedClipId: this.selectedClipId,
      selectedTrackId: this.selectedTrackId,
    });
    this.timeline.updateLoop();
    this.markDirty();
  }

  toggleLoop() {
    const loop = { ...this.project.session.loop };
    if (!loop.on) {
      // Al activarlo sin región definida, se usa un compás desde el cabezal.
      if (!loop.end || loop.end <= loop.start) {
        const beat = 60 / this.project.session.bpm;
        const bar = beat * (this.project.session.timeSig?.[0] ?? 4);
        loop.start = Math.floor(this.playheadPosition / bar) * bar;
        loop.end = loop.start + bar;
      }
      loop.on = true;
    } else {
      loop.on = false;
    }
    this.setLoop(loop);
  }

  updateLoopUi() {
    const loop = this.project.session.loop;
    this.els.loop.setAttribute('aria-pressed', String(Boolean(loop.on)));
  }

  toggleMetronome() {
    const on = !this.project.session.metronome?.on;
    this.project.session.metronome = { ...(this.project.session.metronome ?? {}), on };
    this.els.metronome.setAttribute('aria-pressed', String(on));
    this.engine?.metronome.setEnabled(on);
    this.markDirty();
  }

  // ------------------------------------------------------------------ Transporte
  async togglePlay() {
    await this.ensureAudio();
    if (this.transport.isPlaying()) {
      await this.stopTransport();
      return;
    }
    this.engine.setArmedTracks(this.project.tracks.filter((t) => t.armed).map((t) => t.id));
    this.engine.resetLufs();
    this.transport.play(this.playheadPosition);
    this.els.statusEngine.textContent = 'Reproduciendo';
  }

  async toggleRecord() {
    await this.ensureAudio();
    if (this.transport.isPlaying()) {
      await this.stopTransport();
      return;
    }
    if (!this.engine.input.stream) {
      toast('Primero abre tu micrófono (BM800 + V8)', 'warn', 5000);
      this.dialogs.openInputDialog();
      return;
    }
    if (this.engine.input.isBuiltIn && !this.builtInWarningShown) {
      this.builtInWarningShown = true;
      toast(
        'Estás grabando con el micrófono del teléfono: de ahí vienen el eco y el ruido. ' +
          'Conecta la V8 por USB y elígela en Pistas › Micrófono.',
        'error',
        10000
      );
    }
    let track = this.project.tracks.find((t) => t.armed) ?? this.selectedTrack ?? this.project.tracks[0];
    if (!track) {
      this.addTrackInternal();
      track = this.project.tracks[0];
    }
    if (!track.armed) this.setArmed(track.id, true);
    this.recordingTrackId = track.id;
    this.engine.setArmedTracks([track.id]);

    const countInBeats = this.project.session.metronome?.on ? this.project.session.countInBeats ?? 0 : 0;
    await this.transport.record({
      position: this.playheadPosition,
      countInBeats,
      bpm: this.project.session.bpm,
      channels: this.project.session.recordChannels ?? 1,
    });
    this.seek(this.playheadPosition);
  }

  async stopTransport() {
    if (!this.transport.isPlaying()) return;
    const result = await this.transport.stop();
    if (result) await this.finalizeRecording(result);
    this.engine?.setArmedTracks(this.project.tracks.filter((t) => t.armed).map((t) => t.id));
    this.updateStatusBar();
  }

  onTransportState(state) {
    const playing = state === 'playing' || state === 'recording';
    this.els.play.classList.toggle('is-active', playing);
    this.els.play.setAttribute('aria-pressed', String(playing));
    this.els.record.classList.toggle('is-active', state === 'recording' || state === 'count-in');
    this.els.record.setAttribute('aria-pressed', String(state === 'recording' || state === 'count-in'));
    this.els.playhead.classList.toggle('is-recording', state === 'recording' || state === 'count-in');
    this.els.time.classList.toggle('is-countin', state === 'count-in');
    this.els.statusEngine.textContent =
      state === 'recording' ? 'Grabando' : state === 'count-in' ? 'Cuenta de entrada…' : state === 'playing' ? 'Reproduciendo' : 'Motor detenido';
    if (state === 'stopped') {
      this.els.play.classList.remove('is-active');
      this.els.play.setAttribute('aria-pressed', 'false');
      this.els.record.classList.remove('is-active');
      this.els.record.setAttribute('aria-pressed', 'false');
      this.els.playhead.classList.remove('is-recording');
      this.els.time.classList.remove('is-countin');
      this.playheadPosition = this.transport.getPosition();
      this.timeline.updatePlayhead(this.playheadPosition);
    }
  }

  async finalizeRecording(result) {
    const { channels, frames, sampleRate, info } = result;
    if (!frames || !channels?.length) {
      toast('La toma quedó vacía: revisa el micrófono', 'warn');
      return;
    }
    const trackId = this.recordingTrackId ?? this.selectedTrackId;
    if (!trackId) return;

    const ctx = this.engine.ctx;
    const buffer = ctx.createBuffer(channels.length, frames, sampleRate);
    channels.forEach((channel, index) => buffer.copyToChannel(channel, index));

    // Comprobación real de la toma: si llegó todo a cero, hay que avisar en vez
    // de dejar un clip vacío que parece que grabó.
    let peak = 0;
    for (const channel of channels) {
      for (let i = 0; i < channel.length; i++) {
        const value = channel[i] < 0 ? -channel[i] : channel[i];
        if (value > peak) peak = value;
      }
    }
    if (peak < 1e-5) {
      toast(
        'La toma llegó en silencio: el micrófono está abierto pero no entrega señal al grabador. ' +
          'Pulsa Grabar otra vez o revisa Ajustes › Sistema.',
        'error',
        9000
      );
      console.warn('Toma silenciosa', { source: result.source, frames });
    }
    this.lastRecordingPeak = peak;
    const peakDb = peak > 0 ? 20 * Math.log10(peak) : -Infinity;

    const bufferId = uid('buf');
    this.buffers.set(bufferId, buffer);
    this.peaks.set(bufferId, computePeaks(channels, 256));

    try {
      await db.putBuffer({ id: bufferId, sampleRate, channels, length: frames });
    } catch (error) {
      toast(`No se pudo guardar el audio en disco: ${error.message}`, 'error', 6000);
    }

    const latencyMs =
      (this.project.session.latencyOffset ?? 0) + (this.engine.input.latencyEstimate ?? 0) * 1000;
    const start = Math.max(0, (info?.position ?? this.playheadPosition) - latencyMs / 1000);

    this.pushUndo('Grabar toma');
    const clip = createClip({
      trackId,
      bufferId,
      name: `Toma ${this.project.clips.filter((c) => c.trackId === trackId).length + 1}`,
      start,
      duration: frames / sampleRate,
      fadeIn: 0.005,
      fadeOut: 0.01,
    });
    // La V8 entrega la señal baja: si la toma quedó floja se le da ganancia
    // (un solo ajuste lineal, sin comprimir ni recortar) hasta −6 dBFS de pico.
    const targetPeak = 0.5; // ≈ −6 dBFS
    if (getSetting('autoGainTakes') !== false && peak > 1e-5 && peak < targetPeak) {
      clip.gain = Math.min(8, targetPeak / peak);
      const applied = 20 * Math.log10(clip.gain);
      toast(
        `Nivel de la toma ajustado ${applied >= 0 ? '+' : ''}${applied.toFixed(1)} dB (entró a ${formatDb(peakDb)})`,
        'ok',
        6000
      );
    } else if (peak > 1e-5) {
      toast(`Toma grabada · pico ${formatDb(peakDb)}`, 'ok', 4000);
    }

    if (peak > 1e-5 && peakDb < -30) {
      toast(
        'La V8 está entregando muy poca señal. Sube el gain de la tarjeta (y revisa +48V), ' +
          'o sube «Ganancia» en la tira de entrada.',
        'warn',
        9000
      );
    }

    this.project.clips.push(clip);
    this.selectedClipId = clip.id;
    this.seek(start);
    this.renderAll();
    this.markDirty();
    toast(`Toma grabada: ${(frames / sampleRate).toFixed(1)} s`, 'ok');
  }

  // ------------------------------------------------------------------ Tipos de clip
  splitAtPlayhead() {
    const position = this.transport?.isPlaying() ? this.transport.getPosition() : this.playheadPosition;
    const candidates = this.project.clips.filter(
      (clip) => position > clip.start + 0.01 && position < clip.start + clip.duration - 0.01
    );
    if (!candidates.length) {
      toast('El cabezal no está sobre ningún clip', 'warn', 2500);
      return;
    }
    this.pushUndo('Cortar clip');
    for (const clip of candidates) {
      const offsetInClip = position - clip.start;
      const right = {
        ...clip,
        id: uid('clip'),
        start: position,
        offset: clip.offset + offsetInClip,
        duration: clip.duration - offsetInClip,
        fadeIn: 0.005,
      };
      clip.duration = offsetInClip;
      clip.fadeOut = Math.min(clip.fadeOut, offsetInClip / 2);
      this.project.clips.push(right);
    }
    this.renderTimelineOnly();
    this.markDirty();
    toast('Clip cortado', 'ok', 1600);
  }

  /** Elimina varios clips por id (p. ej. las tomas que quedaron en silencio). */
  removeClips(ids = [], label = 'Eliminar clips') {
    const remove = new Set(ids);
    if (!remove.size) return 0;
    const before = this.project.clips.length;
    this.pushUndo(label);
    this.project.clips = this.project.clips.filter((clip) => !remove.has(clip.id));
    if (this.selectedClipId && remove.has(this.selectedClipId)) this.selectedClipId = null;
    const removed = before - this.project.clips.length;
    if (removed) {
      this.renderAll();
      this.markDirty();
    }
    return removed;
  }

  deleteSelectedClip() {
    if (!this.selectedClipId) return;
    this.pushUndo('Eliminar clip');
    this.project.clips = this.project.clips.filter((c) => c.id !== this.selectedClipId);
    this.selectedClipId = null;
    this.renderAll();
    this.markDirty();
  }

  duplicateSelectedClip() {
    const clip = this.project.clips.find((c) => c.id === this.selectedClipId);
    if (!clip) return;
    this.pushUndo('Duplicar clip');
    const copy = { ...clip, id: uid('clip'), start: clip.start + clip.duration };
    this.project.clips.push(copy);
    this.selectedClipId = copy.id;
    this.renderTimelineOnly();
    this.updateClipTools();
    this.markDirty();
  }

  normalizeSelectedClip() {
    const clip = this.project.clips.find((c) => c.id === this.selectedClipId);
    if (!clip) return;
    const buffer = this.buffers.get(clip.bufferId);
    if (!buffer) return;
    const peaks = this.peaks.get(clip.bufferId) ?? computePeaks([buffer.getChannelData(0)], 256);
    const peak = peaks.peak || 0.0001;
    this.pushUndo('Normalizar clip');
    clip.gain = clamp(1 / peak, 0.01, 16);
    this.renderTimelineOnly();
    this.markDirty();
    toast(`Normalizado (+${(20 * Math.log10(clip.gain)).toFixed(1)} dB)`, 'ok', 2000);
  }

  async reverseSelectedClip() {
    const clip = this.project.clips.find((c) => c.id === this.selectedClipId);
    if (!clip) return;
    const buffer = this.buffers.get(clip.bufferId);
    if (!buffer || clip.reversed) {
      toast('Este clip ya está invertido', 'info', 2000);
      return;
    }
    const ctx = this.engine.ctx;
    const reversed = ctx.createBuffer(buffer.numberOfChannels, buffer.length, buffer.sampleRate);
    const channels = [];
    for (let c = 0; c < buffer.numberOfChannels; c++) {
      const source = buffer.getChannelData(c);
      const target = reversed.getChannelData(c);
      for (let i = 0; i < source.length; i++) target[i] = source[source.length - 1 - i];
      channels.push(target);
    }
    const bufferId = uid('buf');
    this.buffers.set(bufferId, reversed);
    this.peaks.set(bufferId, computePeaks(channels, 256));
    await db.putBuffer({ id: bufferId, sampleRate: reversed.sampleRate, channels, length: reversed.length });

    this.pushUndo('Invertir clip');
    clip.bufferId = bufferId;
    clip.reversed = true;
    this.renderTimelineOnly();
    this.markDirty();
  }

  trimSelectedClip() {
    const clip = this.project.clips.find((c) => c.id === this.selectedClipId);
    if (!clip) return;
    const buffer = this.buffers.get(clip.bufferId);
    if (!buffer) return;
    const region = detectSignalRegion(buffer, -50);
    const end = Math.min(region.end, buffer.duration);
    this.pushUndo('Recortar silencios');
    clip.offset = Math.max(0, region.start);
    clip.duration = Math.max(0.05, end - region.start);
    this.renderTimelineOnly();
    this.markDirty();
    toast(`Recortado: ${clip.duration.toFixed(2)} s`, 'ok', 2000);
  }

  // ------------------------------------------------------------------ Importar
  async importAudioFiles(files) {
    await this.ensureAudio();
    for (const file of files) {
      try {
        const arrayBuffer = await file.arrayBuffer();
        const buffer = await this.engine.ctx.decodeAudioData(arrayBuffer);
        const bufferId = uid('buf');
        const channels = [];
        for (let c = 0; c < buffer.numberOfChannels; c++) channels.push(buffer.getChannelData(c).slice());
        this.buffers.set(bufferId, buffer);
        this.peaks.set(bufferId, computePeaks(channels, 256));
        await db.putBuffer({ id: bufferId, sampleRate: buffer.sampleRate, channels, length: buffer.length });

        this.pushUndo('Importar audio');
        let track = this.selectedTrack;
        if (!track) {
          track = createTrack({ name: file.name.replace(/\.[^.]+$/, ''), colorIndex: this.project.tracks.length });
          this.project.tracks.push(track);
          this.selectedTrackId = track.id;
        }
        const clip = createClip({
          trackId: track.id,
          bufferId,
          name: file.name.replace(/\.[^.]+$/, ''),
          start: this.playheadPosition,
          duration: buffer.duration,
        });
        this.project.clips.push(clip);
        this.selectedClipId = clip.id;
        this.renderAll();
        this.markDirty();
        toast(`Audio importado: ${file.name}`, 'ok');
      } catch (error) {
        toast(`No se pudo importar ${file.name}: ${error.message}`, 'error', 6000);
      }
    }
  }

  // ------------------------------------------------------------------ Sesión
  clearSession() {
    if (!confirm('¿Vaciar la sesión? Se eliminarán los clips y las pistas de este proyecto.')) return;
    this.pushUndo('Vaciar sesión');
    this.project.clips = [];
    this.project.tracks = [createTrack({ name: 'Pista 1', colorIndex: 0 })];
    this.selectedTrackId = this.project.tracks[0].id;
    this.selectedClipId = null;
    this.playheadPosition = 0;
    this.renderAll();
    this.markDirty();
    toast('Sesión vaciada', 'ok');
  }

  updateInputUi() {
    const input = this.engine?.input;
    const visible = Boolean(input?.stream);
    this.els.inputStrip.hidden = !visible;
    const cta = document.getElementById('input-cta');
    if (cta) cta.hidden = visible;
    if (visible) {
      const name = input.deviceLabel || 'Entrada activa';
      const phone = input.isBuiltIn ? ' · del teléfono' : '';
      this.els.inputStripLabel.textContent = `${name}${phone}`;
      this.els.statusDevice.textContent = `${name}${phone}`;
      this.els.inputStripLabel.title = input.isBuiltIn
        ? 'Estás grabando con el micrófono del teléfono: produce eco y ruido. Conecta la V8 por USB.'
        : name;
      this.els.statusDevice.classList.remove('status-item--muted');
      this.els.statusDevice.classList.add('is-ok');
      const buttonLabel = document.getElementById('input-btn-label');
      if (buttonLabel) buttonLabel.textContent = 'Cambiar';
    }
    this.syncInputButton();
    this.updateStatusBar();
    if (this.project.session.monitor !== undefined) {
      this.engine?.setMonitoring(Boolean(this.project.session.monitor));
    }
  }

  syncInputButton() {
    const button = this.els.monitor;
    if (!button) return;
    const on = Boolean(this.project.session.monitor);
    button.setAttribute('aria-pressed', String(on));
    button.textContent = on ? 'Escuchando' : 'Escucha';
  }

  updateStatusBar() {
    if (!this.els.statusSr) return;
    const ctx = this.engine?.ctx;
    this.els.statusSr.textContent = ctx ? `${(ctx.sampleRate / 1000).toFixed(1)} kHz · 32 bits` : '48 kHz · 32 bits';
    if (ctx) {
      const latency = this.engine.getLatencyInfo();
      this.els.statusLatency.textContent = `Latencia ${Math.round(latency.total * 1000)} ms`;
    }
    if (!this.engine?.input?.stream) {
      this.els.statusDevice.textContent = 'Entrada no configurada';
      this.els.statusDevice.classList.add('status-item--muted');
    }
  }

  async refreshStorageInfo() {
    const estimate = await db.storageEstimate();
    if (!this.els.statusStorage) return;
    if (!estimate) {
      this.els.statusStorage.textContent = 'Almacenamiento —';
      return;
    }
    const mega = (bytes) => `${(bytes / (1024 * 1024)).toFixed(0)} MB`;
    this.els.statusStorage.textContent = `Disco ${mega(estimate.usage)} / ${mega(estimate.quota)}`;
    this.inspector?.setStorageText(
      `Usado: ${(estimate.usage / (1024 * 1024)).toFixed(1)} MB de ${(estimate.quota / (1024 * 1024)).toFixed(0)} MB.`
    );
  }

  // ------------------------------------------------------------------ Bucle visual
  startAnimationLoop() {
    const frame = (timestamp) => {
      this.updateMeters(timestamp);
      requestAnimationFrame(frame);
    };
    requestAnimationFrame(frame);
  }

  updateMeters(timestamp) {
    // Posición del cabezal
    if (this.transport?.isPlaying()) {
      const position = this.transport.getPosition();
      this.playheadPosition = position;
      this.timeline.updatePlayhead(position, this.transport.state === 'recording');
      this.els.time.textContent = formatTime(position);
      this.els.time.classList.toggle('is-countin', this.transport.state === 'count-in');
    } else if (timestamp - this.lastTextUpdate > 120) {
      this.els.time.textContent = formatTime(this.playheadPosition);
    }

    if (timestamp - this.lastMeterUpdate < 33) return;
    this.lastMeterUpdate = timestamp;

    // Medidores de pistas
    this.strips.updateMeters(timestamp);

    // Entrada
    if (this.engine?.input?.stream) {
      const level = this.engine.readInputLevel();
      drawMeter(this.els.inputMeter, level.peak, { hold: level.peak });
      this.els.inputPeak.textContent = Number.isFinite(level.peak) ? formatDb(level.peak) : '−∞ dBFS';
      this.els.inputPeak.style.color = level.clipping ? 'var(--red-strong)' : '';
      this.dialogs?.updateSetupMeters(level);
    }

    // Máster
    if (this.engine?.ctx) {
      const master = this.engine.readMasterLevel();
      drawMeter(this.els.masterMeter, master.peak, { hold: master.peak });
      this.els.masterPeak.textContent = Number.isFinite(master.peak) ? formatDb(master.peak) : '−∞';
      const lufs = this.engine.lufsValues?.integrated;
      this.els.lufs.textContent = `LUFS ${Number.isFinite(lufs) ? lufs.toFixed(1) : '−∞'}`;
      this.inspector.updateMasterLive({
        integrated: lufs,
        peakDb: master.peak,
      });
    }

    if (timestamp - this.lastTextUpdate > 4000) {
      this.lastTextUpdate = timestamp;
      this.refreshStorageInfo();
    }
  }

  // ------------------------------------------------------------------ Eventos
  bindEvents() {
    this.els.play.addEventListener('click', () => this.togglePlay());
    this.els.stop.addEventListener('click', () => this.stopTransport());
    this.els.record.addEventListener('click', () => this.toggleRecord());
    this.els.rewind.addEventListener('click', () => {
      this.stopTransport().then(() => this.seek(0));
    });
    this.els.metronome.addEventListener('click', () => this.toggleMetronome());
    this.els.loop.addEventListener('click', () => this.toggleLoop());

    this.els.bpm.addEventListener('change', (event) => {
      this.project.session.bpm = clamp(Number(event.target.value) || 120, 20, 300);
      this.onSessionChange();
      this.inspector.renderSession();
    });
    this.els.timeSig.addEventListener('change', (event) => {
      const [beats, unit] = event.target.value.split('/').map(Number);
      this.project.session.timeSig = [beats, unit];
      this.onSessionChange();
    });
    this.els.snap.addEventListener('change', (event) => {
      this.project.session.snap = Number(event.target.value);
      this.updateArrangeInfo();
      this.markDirty();
    });
    this.els.projectName.addEventListener('change', (event) => {
      this.project.name = event.target.value.trim() || 'Sesión sin título';
      this.markDirty();
    });

    document.getElementById('btn-add-track').addEventListener('click', () => this.addTrack());
    document.getElementById('btn-input').addEventListener('click', () => this.dialogs.openInputDialog());
    document.getElementById('btn-activate-input').addEventListener('click', async (event) => {
      const button = event.currentTarget;
      const label = button.textContent;
      button.disabled = true;
      button.textContent = 'Activando…';
      try {
        await this.dialogs.openDevice(undefined, true);
        toast('Micrófono listo. Elige la pista y pulsa Grabar.', 'ok', 6000);
      } catch (error) {
        // Si el dispositivo falla, se abre el diálogo para elegir otro.
        toast(error.message ?? 'No se pudo abrir el micrófono', 'error', 7000);
        this.dialogs.openInputDialog();
      } finally {
        button.disabled = false;
        button.textContent = label;
      }
    });
    document.getElementById('btn-empty-setup').addEventListener('click', () => this.dialogs.openInputDialog());
    document.getElementById('btn-export').addEventListener('click', () => this.dialogs.openExportDialog());
    document.getElementById('btn-projects').addEventListener('click', () => this.dialogs.openProjectsDialog());
    document.getElementById('btn-diagnostics').addEventListener('click', () => this.dialogs.openDiagnostics());
    document.getElementById('btn-settings').addEventListener('click', () => this.dialogs.openSettings());
    document.getElementById('btn-mix-view').addEventListener('click', (event) => {
      const on = this.els.app.classList.toggle('app--mix');
      event.currentTarget.setAttribute('aria-pressed', String(on));
      setTimeout(() => this.timeline.render(), 60);
    });

    document.getElementById('input-gain').addEventListener('input', (event) => {
      this.setInputGain(Number(event.target.value));
      const setup = document.getElementById('setup-gain');
      if (setup) setup.value = event.target.value;
    });

    this.els.monitor.addEventListener('click', () => {
      this.project.session.monitor = !this.project.session.monitor;
      this.engine?.setMonitoring(Boolean(this.project.session.monitor));
      this.syncInputButton();
      this.markDirty();
      if (this.project.session.monitor && !this.monitorWarningShown) {
        this.monitorWarningShown = true;
        const latency = this.engine?.getLatencyInfo?.();
        const ms = latency ? Math.round((latency.total ?? 0) * 1000) : 0;
        toast(
          `Escucha activada${ms ? ` (${ms} ms de retardo)` : ''}. Usa cascos y, si grabas con la V8, ` +
            'apaga esta escucha y usa el monitor directo de la tarjeta: es instantáneo y no acopla.',
          'warn',
          9000
        );
      }
    });

    document.getElementById('tool-select').addEventListener('click', () => this.setTool('select'));
    document.getElementById('tool-cut').addEventListener('click', () => {
      this.setTool('cut');
      this.splitAtPlayhead();
      this.setTool('select');
    });

    document.getElementById('btn-undo').addEventListener('click', () => this.undo());
    document.getElementById('btn-redo').addEventListener('click', () => this.redo());
    document.getElementById('btn-zoom-in').addEventListener('click', () =>
      this.setZoom(this.project.session.zoom * 1.4)
    );
    document.getElementById('btn-zoom-out').addEventListener('click', () =>
      this.setZoom(this.project.session.zoom / 1.4)
    );
    document.getElementById('btn-zoom-fit').addEventListener('click', () => this.fitToSession());

    document.getElementById('clip-split').addEventListener('click', () => this.splitAtPlayhead());
    document.getElementById('clip-delete').addEventListener('click', () => this.deleteSelectedClip());
    document.getElementById('clip-duplicate').addEventListener('click', () => this.duplicateSelectedClip());
    document.getElementById('clip-normalize').addEventListener('click', () => this.normalizeSelectedClip());
    document.getElementById('clip-reverse').addEventListener('click', () => this.reverseSelectedClip());
    document.getElementById('clip-trim').addEventListener('click', () => this.trimSelectedClip());

    document.querySelectorAll('.tab').forEach((tab) => {
      tab.addEventListener('click', () => {
        document.querySelectorAll('.tab').forEach((t) => {
          t.classList.toggle('is-active', t === tab);
          t.setAttribute('aria-selected', String(t === tab));
        });
        document.querySelectorAll('.tabpanel').forEach((panel) => {
          panel.classList.toggle('is-active', panel.id === `panel-${tab.dataset.tab}`);
        });
      });
    });

    this.bindMobileNav();
    this.addImportButton();
  }

  /**
   * Barra de navegación del teléfono: en pantallas pequeñas solo cabe un panel.
   * En el ordenador esta barra está oculta y no cambia nada.
   */
  bindMobileNav() {
    const nav = document.querySelector('.mobile-nav');
    const workspace = document.querySelector('.workspace');
    if (!nav || !workspace) return;
    const tabs = [...nav.querySelectorAll('.mobile-tab')];
    const select = (view) => {
      workspace.dataset.view = view;
      for (const tab of tabs) {
        const active = tab.dataset.view === view;
        tab.classList.toggle('is-active', active);
        tab.setAttribute('aria-selected', String(active));
      }
      // Al cambiar de panel el lienzo necesita saber su ancho real.
      if (view === 'editor') {
        requestAnimationFrame(() => window.dispatchEvent(new Event('resize')));
      }
    };
    for (const tab of tabs) {
      tab.addEventListener('click', () => select(tab.dataset.view || 'editor'));
    }
    select(workspace.dataset.view || 'editor');
  }

  addImportButton() {
    const actions = document.querySelector('.panel__actions');
    if (!actions) return;
    const input = h('input', { type: 'file', accept: 'audio/*', multiple: true, style: { display: 'none' } });
    input.addEventListener('change', async (event) => {
      const files = [...event.target.files];
      event.target.value = '';
      if (files.length) await this.importAudioFiles(files);
    });
    const button = h(
      'button',
      {
        class: 'btn btn--ghost btn--icon',
        type: 'button',
        title: 'Importar audio (WAV, MP3, M4A…)',
        attrs: { 'aria-label': 'Importar audio' },
        onclick: () => input.click(),
      },
      h('span', { text: '⤓', style: { fontSize: '14px' } })
    );
    actions.insertBefore(button, actions.lastElementChild);
    actions.appendChild(input);
  }

  setTool(tool) {
    this.tool = tool;
    document.getElementById('tool-select').classList.toggle('is-active', tool === 'select');
    document.getElementById('tool-cut').classList.toggle('is-active', tool === 'cut');
    document.getElementById('tool-select').setAttribute('aria-pressed', String(tool === 'select'));
    document.getElementById('tool-cut').setAttribute('aria-pressed', String(tool === 'cut'));
  }

  fitToSession() {
    const duration = Math.max(projectDuration(this.project), 10);
    const width = this.els.lanesWrap.clientWidth - 30;
    this.setZoom(width / duration);
  }

  bindKeyboard() {
    document.addEventListener('keydown', (event) => {
      const target = event.target;
      const typing =
        target instanceof HTMLElement &&
        (target.tagName === 'INPUT' ||
          target.tagName === 'TEXTAREA' ||
          target.tagName === 'SELECT' ||
          target.isContentEditable);
      if (typing) return;
      if (document.querySelector('dialog[open]')) return;

      const meta = event.ctrlKey || event.metaKey;

      if (meta && event.key.toLowerCase() === 'z') {
        event.preventDefault();
        if (event.shiftKey) this.redo();
        else this.undo();
        return;
      }
      if (meta && event.key.toLowerCase() === 's') {
        event.preventDefault();
        this.save(true).then((ok) => ok && toast('Proyecto guardado', 'ok', 1600));
        return;
      }
      if (meta && event.key.toLowerCase() === 'e') {
        event.preventDefault();
        this.dialogs.openExportDialog();
        return;
      }
      if (meta && event.key.toLowerCase() === 'd') {
        event.preventDefault();
        this.duplicateSelectedClip();
        return;
      }

      switch (event.key) {
        case ' ':
          event.preventDefault();
          if (this.transport?.isPlaying()) this.stopTransport();
          else this.togglePlay();
          break;
        case 'r':
        case 'R':
          event.preventDefault();
          this.toggleRecord();
          break;
        case 'Home':
          event.preventDefault();
          this.stopTransport().then(() => this.seek(0));
          break;
        case 's':
        case 'S':
          this.splitAtPlayhead();
          break;
        case 'm':
        case 'M':
          this.toggleMetronome();
          break;
        case 'l':
        case 'L':
          this.toggleLoop();
          break;
        case 'f':
        case 'F':
          this.fitToSession();
          break;
        case 'Delete':
        case 'Backspace':
          if (this.selectedClipId) {
            event.preventDefault();
            this.deleteSelectedClip();
          }
          break;
        case '+':
        case '=':
          this.setZoom(this.project.session.zoom * 1.4);
          break;
        case '-':
        case '_':
          this.setZoom(this.project.session.zoom / 1.4);
          break;
        case '?':
          openDialog('dialog-shortcuts');
          break;
        default:
          break;
      }
    });
  }

  registerServiceWorker() {
    // En la app Android los archivos ya viven dentro del teléfono: un service
    // worker solo serviría versiones antiguas después de actualizar.
    if (isNativeApp()) return;
    if (!('serviceWorker' in navigator)) return;
    window.addEventListener('load', () => {
      navigator.serviceWorker.register('sw.js').catch((error) => {
        console.warn('Sin modo autónomo:', error.message);
      });
    });
  }
}
