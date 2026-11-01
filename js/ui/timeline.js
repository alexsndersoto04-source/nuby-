/**
 * Línea de tiempo: regla, rejilla, clips con forma de onda y edición
 * (mover, recortar, fades, bucle, cabezal).
 */

import { h, clear } from './dom.js';
import { clamp } from '../util/format.js';

const LANE_HEIGHT = 78;

export class TimelineView {
  constructor({ elements, callbacks }) {
    this.els = elements;
    this.callbacks = callbacks;
    this.project = null;
    this.buffers = new Map();
    this.peaks = new Map();
    this.selectedClipId = null;
    this.selectedTrackId = null;
    this.zoom = 80;
    this.drag = null;

    // El cabezal y la región de bucle viven dentro del contenido desplazable.
    this.els.lanes.appendChild(this.els.playhead);
    this.els.lanes.appendChild(this.els.loop);

    this.els.wrap.addEventListener('scroll', () => this.syncScroll(), { passive: true });
    this.els.ruler.addEventListener('pointerdown', (event) => this.onRulerDown(event));
    this.els.lanes.addEventListener('pointerdown', (event) => this.onLanesDown(event));
    window.addEventListener('pointermove', (event) => this.onPointerMove(event));
    window.addEventListener('pointerup', (event) => this.onPointerUp(event));
    window.addEventListener('resize', () => this.renderRuler());
  }

  setData({ project, buffers, peaks, zoom, selectedClipId, selectedTrackId }) {
    this.project = project;
    this.buffers = buffers;
    this.peaks = peaks;
    this.zoom = zoom ?? this.zoom;
    this.selectedClipId = selectedClipId ?? null;
    this.selectedTrackId = selectedTrackId ?? null;
  }

  get contentWidth() {
    if (!this.project) return 0;
    let end = 0;
    for (const clip of this.project.clips) end = Math.max(end, clip.start + clip.duration);
    return Math.max(this.els.wrap.clientWidth, (end + 12) * this.zoom);
  }

  render() {
    if (!this.project) return;
    this.renderRuler();
    this.renderLanes();
    this.syncScroll();
  }

  renderLanes() {
    const { lanes } = this.els;
    const width = this.contentWidth;
    lanes.style.width = `${width}px`;
    lanes.style.backgroundImage = this.gridBackground();
    clear(lanes);
    lanes.appendChild(this.els.playhead);
    lanes.appendChild(this.els.loop);

    for (const track of this.project.tracks) {
      const lane = h('div', {
        class: `lane${track.id === this.selectedTrackId ? ' is-selected' : ''}`,
        dataset: { trackId: track.id },
        style: { height: `${LANE_HEIGHT}px`, width: `${width}px` },
      });
      for (const clip of this.project.clips.filter((c) => c.trackId === track.id)) {
        lane.appendChild(this.renderClip(clip, track));
      }
      lanes.appendChild(lane);
    }

    this.els.empty.hidden = this.project.clips.length > 0;
    this.updateLoop();
    this.updateClipTools();
  }

  gridBackground() {
    const beat = 60 / Math.max(20, this.project?.session?.bpm ?? 120);
    const beatsPerBar = this.project?.session?.timeSig?.[0] ?? 4;
    const barWidth = beat * beatsPerBar * this.zoom;
    if (!Number.isFinite(barWidth) || barWidth < 8) return 'none';
    return `repeating-linear-gradient(to right, rgba(255,255,255,0.045) 0 1px, transparent 1px ${barWidth}px)`;
  }

  renderClip(clip, track) {
    const node = h('div', {
      class: `clip${clip.id === this.selectedClipId ? ' is-selected' : ''}`,
      dataset: { clipId: clip.id, trackId: track.id },
      style: {
        left: `${clip.start * this.zoom}px`,
        width: `${Math.max(6, clip.duration * this.zoom)}px`,
      },
    });

    const canvas = h('canvas', { class: 'clip__wave' });
    node.appendChild(canvas);

    const label = h('span', { class: 'clip__label', text: clip.name || 'Toma' });
    node.appendChild(label);

    const handleL = h('div', { class: 'clip__handle clip__handle--l', dataset: { mode: 'trim-l' } });
    const handleR = h('div', { class: 'clip__handle clip__handle--r', dataset: { mode: 'trim-r' } });
    const fadeIn = h('div', { class: 'clip__fade clip__fade--in', dataset: { mode: 'fade-in' }, title: 'Fundido de entrada' });
    const fadeOut = h('div', { class: 'clip__fade clip__fade--out', dataset: { mode: 'fade-out' }, title: 'Fundido de salida' });
    node.append(handleL, handleR, fadeIn, fadeOut);

    node.addEventListener('pointerdown', (event) => {
      if (event.button !== 0) return;
      event.stopPropagation();
      const mode = event.target.dataset.mode ?? 'move';
      this.beginClipDrag(event, clip, track, mode);
    });

    // Posicionar los mangos de fundido y dibujar la onda (necesita medidas).
    requestAnimationFrame(() => {
      this.positionClip(clip);
    });
    return node;
  }

  drawClipWave(node, clip, track) {
    const canvas = node.querySelector('canvas');
    if (!canvas) return;
    const width = Math.max(6, Math.round(clip.duration * this.zoom));
    const height = Math.max(20, LANE_HEIGHT - 10);
    const dpr = window.devicePixelRatio || 1;
    // El navegador limita el tamaño de un canvas: se dibuja a menor resolución
    // y el CSS lo estira, manteniendo la posición temporal exacta.
    const drawWidth = Math.min(width, 8000);
    canvas.width = Math.round(drawWidth * dpr);
    canvas.height = Math.round(height * dpr);
    canvas.style.width = `${width}px`;
    canvas.style.height = `${height}px`;

    const ctx = canvas.getContext('2d');
    ctx.clearRect(0, 0, canvas.width, canvas.height);

    const peaks = this.peaks.get(clip.bufferId);
    const buffer = this.buffers.get(clip.bufferId);
    const mid = canvas.height / 2;

    // Fondo del clip con el color de la pista.
    ctx.fillStyle = hexToRgba(track.color, 0.14);
    ctx.fillRect(0, 0, canvas.width, canvas.height);

    if (!peaks) return;

    const sampleRate = buffer?.sampleRate ?? 48000;
    const framesPerBucket = peaks.bucket; // muestras por bloque de dibujo
    const startFrame = (clip.offset ?? 0) * sampleRate;
    const endFrame = startFrame + clip.duration * sampleRate;

    ctx.fillStyle = hexToRgba(track.color, 0.85);
    const step = Math.max(1, Math.floor(canvas.width / 1600));
    for (let x = 0; x < canvas.width; x += step) {
      const frame = startFrame + ((x / canvas.width) * (endFrame - startFrame));
      const bucket = Math.floor(frame / framesPerBucket);
      if (bucket < 0 || bucket >= peaks.length) continue;
      const lo = peaks.min[bucket];
      const hi = peaks.max[bucket];
      const y1 = mid - clamp(hi, -1, 1) * (mid - 3);
      const y2 = mid - clamp(lo, -1, 1) * (mid - 3);
      ctx.fillRect(x, y1, Math.max(1, step), Math.max(1.4, y2 - y1));
    }

    // Línea central
    ctx.fillStyle = 'rgba(255,255,255,0.12)';
    ctx.fillRect(0, mid, canvas.width, Math.max(1, dpr));

    // Rampas de fundido visibles
    ctx.strokeStyle = 'rgba(230, 240, 248, 0.5)';
    ctx.lineWidth = Math.max(1, dpr);
    const fadeInPx = Math.max(0, (clip.fadeIn ?? 0) * this.zoom * dpr);
    const fadeOutPx = Math.max(0, (clip.fadeOut ?? 0) * this.zoom * dpr);
    if (fadeInPx > 1) {
      ctx.beginPath();
      ctx.moveTo(0, 0);
      ctx.lineTo(fadeInPx, mid);
      ctx.stroke();
    }
    if (fadeOutPx > 1) {
      ctx.beginPath();
      ctx.moveTo(canvas.width - fadeOutPx, mid);
      ctx.lineTo(canvas.width, 0);
      ctx.stroke();
    }
  }

  // ------------------------------------------------------------ Cabecera/regla
  /**
   * La regla se dibuja solo del tamaño visible y se redibuja al desplazar.
   * (Un lienzo de 80 000 px de ancho supera el límite del navegador y dejaría
   * la regla en blanco en sesiones largas.)
   */
  renderRuler() {
    const canvas = this.els.ruler;
    const viewWidth = Math.max(200, Math.round(this.els.wrap.clientWidth || 600));
    const dpr = window.devicePixelRatio || 1;
    canvas.width = Math.round(viewWidth * dpr);
    canvas.height = Math.round(26 * dpr);
    canvas.style.width = `${viewWidth}px`;
    canvas.style.height = '26px';

    const ctx = canvas.getContext('2d');
    ctx.setTransform(dpr, 0, 0, dpr, 0, 0);
    ctx.clearRect(0, 0, viewWidth, 26);
    ctx.fillStyle = '#15191d';
    ctx.fillRect(0, 0, viewWidth, 26);

    if (!this.project) return;
    const scroll = this.els.wrap.scrollLeft;
    const bpm = this.project.session?.bpm ?? 120;
    const beat = 60 / Math.max(20, bpm);
    const beatsPerBar = this.project.session?.timeSig?.[0] ?? 4;
    const bar = beat * beatsPerBar;
    const barPx = bar * this.zoom;

    const startTime = scroll / this.zoom;
    const endTime = (scroll + viewWidth) / this.zoom;
    const firstBar = Math.max(0, Math.floor(startTime / bar) * bar);

    ctx.font = '10px ui-monospace, monospace';
    ctx.textBaseline = 'alphabetic';

    for (let time = firstBar; time <= endTime + bar; time += bar) {
      const x = Math.round(time * this.zoom - scroll) + 0.5;
      if (x < -40) continue;
      ctx.strokeStyle = 'rgba(255,255,255,0.1)';
      ctx.beginPath();
      ctx.moveTo(x, 16);
      ctx.lineTo(x, 26);
      ctx.stroke();

      ctx.fillStyle = '#7f8a93';
      ctx.fillText(formatRulerTime(time), x + 4, 12);

      if (barPx > 90) {
        for (let b = 1; b < beatsPerBar; b++) {
          const bx = Math.round((time + b * beat) * this.zoom - scroll) + 0.5;
          ctx.strokeStyle = 'rgba(255,255,255,0.055)';
          ctx.beginPath();
          ctx.moveTo(bx, 21);
          ctx.lineTo(bx, 26);
          ctx.stroke();
        }
      }
    }

    if (barPx > 40) {
      ctx.strokeStyle = 'rgba(255,255,255,0.06)';
      const firstBeat = Math.max(0, Math.floor(startTime / beat) * beat);
      for (let time = firstBeat; time <= endTime + beat; time += beat) {
        const x = Math.round(time * this.zoom - scroll) + 0.5;
        ctx.beginPath();
        ctx.moveTo(x, 23);
        ctx.lineTo(x, 26);
        ctx.stroke();
      }
    }
  }

  syncScroll() {
    if (this.rulerPending) return;
    this.rulerPending = true;
    requestAnimationFrame(() => {
      this.rulerPending = false;
      this.renderRuler();
    });
  }

  // ------------------------------------------------------------ Cabezal
  updatePlayhead(position, recording = false) {
    const x = position * this.zoom;
    this.els.playhead.hidden = false;
    this.els.playhead.style.transform = `translateX(${x}px)`;
    this.els.playhead.classList.toggle('is-recording', recording);

    // Desplazamiento automático para seguir al cabezal.
    const wrap = this.els.wrap;
    const margin = 60;
    if (x > wrap.scrollLeft + wrap.clientWidth - margin) {
      wrap.scrollLeft = x - wrap.clientWidth + margin;
    } else if (x < wrap.scrollLeft + 8) {
      wrap.scrollLeft = Math.max(0, x - margin);
    }
  }

  updateLoop() {
    const loop = this.project?.session?.loop;
    const region = this.els.loop;
    if (!loop?.on || loop.end <= loop.start) {
      region.hidden = true;
      return;
    }
    region.hidden = false;
    region.style.left = `${loop.start * this.zoom}px`;
    region.style.width = `${(loop.end - loop.start) * this.zoom}px`;
  }

  updateClipTools() {
    const clip = this.project?.clips.find((c) => c.id === this.selectedClipId);
    const tools = document.getElementById('clip-tools');
    if (tools) tools.hidden = !clip;
    const name = document.getElementById('clip-tools-name');
    if (name && clip) name.textContent = clip.name || 'Toma';
  }

  scrollToTime(seconds) {
    const x = seconds * this.zoom;
    const wrap = this.els.wrap;
    if (x < wrap.scrollLeft || x > wrap.scrollLeft + wrap.clientWidth - 40) {
      wrap.scrollLeft = Math.max(0, x - wrap.clientWidth / 2);
    }
  }

  // ------------------------------------------------------------ Interacción
  timeFromRulerEvent(event) {
    const rect = this.els.ruler.getBoundingClientRect();
    const x = event.clientX - rect.left + this.els.wrap.scrollLeft;
    return Math.max(0, x / this.zoom);
  }

  timeFromLanesEvent(event) {
    const rect = this.els.lanes.getBoundingClientRect();
    const x = event.clientX - rect.left;
    return Math.max(0, x / this.zoom);
  }

  onRulerDown(event) {
    if (event.button !== 0) return;
    const start = this.timeFromRulerEvent(event);

    if (event.shiftKey) {
      // Shift + arrastre = definir región de bucle.
      this.drag = { type: 'loop', anchor: start };
      this.els.ruler.setPointerCapture?.(event.pointerId);
      this.updateLoopPreview(start, start);
      return;
    }

    this.callbacks.onSeek(start);
    this.drag = { type: 'seek' };
  }

  updateLoopPreview(a, b) {
    const start = Math.min(a, b);
    const end = Math.max(a, b);
    const region = this.els.loop;
    region.hidden = end - start < 0.02;
    region.style.left = `${start * this.zoom}px`;
    region.style.width = `${(end - start) * this.zoom}px`;
  }

  onLanesDown(event) {
    if (event.button !== 0) return;
    if (event.target.closest('.clip')) return;
    const lane = event.target.closest('.lane');
    if (lane) this.callbacks.onSelectTrack(lane.dataset.trackId);
    this.callbacks.onSeek(this.timeFromLanesEvent(event));
    this.callbacks.onSelectClip(null);
  }

  beginClipDrag(event, clip, track, mode) {
    const snap = this.snapSeconds();
    this.callbacks.onSelectTrack(track.id);
    this.callbacks.onSelectClip(clip.id);
    this.drag = {
      type: 'clip',
      mode,
      clip,
      track,
      startX: event.clientX,
      original: { ...clip },
      moved: false,
      pushed: false,
      snap,
    };
    const node = event.currentTarget;
    node.classList.add('is-dragging');
  }

  snapSeconds() {
    const session = this.project?.session ?? {};
    const beat = 60 / Math.max(20, session.bpm ?? 120);
    return (session.snap ?? 0) * beat;
  }

  onPointerMove(event) {
    if (!this.drag) return;

    if (this.drag.type === 'seek') {
      this.callbacks.onSeek(this.timeFromRulerEvent(event));
      return;
    }

    if (this.drag.type === 'loop') {
      const current = this.timeFromRulerEvent(event);
      this.drag.current = current;
      this.updateLoopPreview(this.drag.anchor, current);
      return;
    }

    if (this.drag.type !== 'clip') return;
    const { clip, original, mode, snap } = this.drag;
    const dx = (event.clientX - this.drag.startX) / this.zoom;
    if (Math.abs(event.clientX - this.drag.startX) > 2) this.drag.moved = true;
    if (!this.drag.moved) return;
    if (!this.drag.pushed) {
      // El historial se registra al primer movimiento real, no al hacer clic.
      this.drag.pushed = true;
      this.callbacks.onEditStart?.(
        mode === 'move' ? 'Mover clip' : mode.startsWith('fade') ? 'Ajustar fundido' : 'Recortar clip'
      );
    }

    if (mode === 'move') {
      clip.start = Math.max(0, snapValue(original.start + dx, snap));
    } else if (mode === 'trim-l') {
      const maxDelta = original.duration - 0.05;
      let delta = snapValue(dx, snap);
      delta = clamp(delta, -original.offset, maxDelta);
      clip.start = Math.max(0, original.start + delta);
      clip.offset = Math.max(0, original.offset + delta);
      clip.duration = Math.max(0.05, original.duration - delta);
    } else if (mode === 'fade-in') {
      clip.fadeIn = clamp(original.fadeIn + dx, 0, clip.duration);
    } else if (mode === 'fade-out') {
      clip.fadeOut = clamp(original.fadeOut - dx, 0, clip.duration);
    } else if (mode === 'trim-r') {
      let delta = snapValue(dx, snap);
      const maxDuration = (this.buffers.get(clip.bufferId)?.duration ?? Infinity) - (clip.offset ?? 0);
      delta = clamp(delta, -(original.duration - 0.05), Math.max(0, maxDuration - original.duration));
      clip.duration = Math.max(0.05, original.duration + delta);
    }

    this.positionClip(clip);
    this.callbacks.onClipChange(clip);
  }

  onPointerUp() {
    if (!this.drag) return;

    if (this.drag.type === 'loop') {
      const a = this.drag.anchor;
      const b = this.drag.current ?? a;
      const start = Math.min(a, b);
      const end = Math.max(a, b);
      this.drag = null;
      if (end - start > 0.05) {
        this.callbacks.onLoopChange({ on: true, start, end });
      } else {
        this.updateLoop();
      }
      return;
    }

    if (this.drag.type === 'clip') {
      const node = this.els.lanes.querySelector(`.clip[data-clip-id="${this.drag.clip.id}"]`);
      node?.classList.remove('is-dragging');
      const changed = this.drag.moved;
      this.drag = null;
      if (changed) this.callbacks.onEditEnd?.();
      return;
    }

    this.drag = null;
  }

  positionClip(clip) {
    const node = this.els.lanes.querySelector(`.clip[data-clip-id="${clip.id}"]`);
    if (!node) return;
    const width = Math.max(6, clip.duration * this.zoom);
    node.style.left = `${clip.start * this.zoom}px`;
    node.style.width = `${width}px`;

    const fadeIn = node.querySelector('.clip__fade--in');
    const fadeOut = node.querySelector('.clip__fade--out');
    if (fadeIn) fadeIn.style.left = `${Math.min(width - 8, (clip.fadeIn ?? 0) * this.zoom)}px`;
    if (fadeOut) fadeOut.style.left = `${Math.max(1, width - (clip.fadeOut ?? 0) * this.zoom - 7)}px`;

    const track = this.project.tracks.find((t) => t.id === clip.trackId);
    if (track) this.drawClipWave(node, clip, track);
  }

  /** Posición del clic en el clip (para cortes en el cabezal, etc.). */
  clipAt(position) {
    return this.project?.clips.filter(
      (clip) => position >= clip.start && position <= clip.start + clip.duration
    ) ?? [];
  }
}

function snapValue(value, snap) {
  if (!snap || snap <= 0) return value;
  return Math.round(value / snap) * snap;
}

function formatRulerTime(seconds) {
  const m = Math.floor(seconds / 60);
  const s = Math.floor(seconds % 60);
  return `${m}:${String(s).padStart(2, '0')}`;
}

export function hexToRgba(hex, alpha) {
  const value = hex.replace('#', '');
  const full = value.length === 3 ? value.split('').map((c) => c + c).join('') : value;
  const int = parseInt(full, 16);
  const r = (int >> 16) & 255;
  const g = (int >> 8) & 255;
  const b = int & 255;
  return `rgba(${r}, ${g}, ${b}, ${alpha})`;
}

export { LANE_HEIGHT };
