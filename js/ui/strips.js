/**
 * Tiras de canal (lista de pistas) con fader, panorama, armado, mute/solo y
 * vúmetro en tiempo real.
 */

import { h, clear, drawVerticalMeter } from './dom.js';
import { faderToGain, gainToFader, faderToDb, formatDb } from '../util/format.js';

export class TrackStrips {
  /**
   * @param {{container: HTMLElement, engine: object, callbacks: object}} options
   */
  constructor({ container, engine, callbacks }) {
    this.container = container;
    this.engine = engine;
    this.callbacks = callbacks;
    this.project = null;
    this.selectedId = null;
    this.meters = new Map(); // trackId -> {canvas, hold, holdTime}
  }

  setProject(project) {
    this.project = project;
  }

  setSelected(id) {
    this.selectedId = id;
    for (const strip of this.container.querySelectorAll('.strip')) {
      strip.classList.toggle('is-selected', strip.dataset.trackId === id);
    }
  }

  render() {
    if (!this.project) return;
    clear(this.container);
    this.meters.clear();

    for (const track of this.project.tracks) {
      this.container.appendChild(this.renderStrip(track));
    }

    if (!this.project.tracks.length) {
      this.container.appendChild(
        h('p', { class: 'hint', text: 'No hay pistas. Añade una con el botón +.' })
      );
    }
  }

  renderStrip(track) {
    const selected = track.id === this.selectedId;
    const strip = h('div', {
      class: `strip${selected ? ' is-selected' : ''}${track.armed ? ' is-armed' : ''}`,
      dataset: { trackId: track.id },
      onpointerdown: (event) => {
        if (event.target.closest('input, button')) return;
        this.callbacks.onSelect(track.id);
      },
    });

    const nameInput = h('input', {
      class: 'strip__name',
      value: track.name,
      attrs: { 'aria-label': 'Nombre de la pista', spellcheck: 'false' },
      onfocus: () => this.callbacks.onSelect(track.id),
      onchange: (event) => this.callbacks.onRename(track.id, event.target.value.trim() || 'Pista'),
    });

    const arm = h('button', {
      class: 'arm-btn',
      type: 'button',
      attrs: {
        'aria-pressed': String(track.armed),
        'aria-label': 'Armar para grabar',
        title: 'Armar para grabar',
      },
      onclick: (event) => {
        event.stopPropagation();
        this.callbacks.onArm(track.id, !track.armed);
      },
    });

    const remove = h('button', {
      class: 'mini',
      type: 'button',
      title: 'Eliminar pista',
      attrs: { 'aria-label': 'Eliminar pista' },
      text: '×',
      style: { width: '18px', height: '18px' },
      onclick: (event) => {
        event.stopPropagation();
        this.callbacks.onRemove(track.id);
      },
    });

    const head = h(
      'div',
      { class: 'strip__head' },
      h('span', { class: 'strip__color', style: { background: track.color } }),
      nameInput,
      arm,
      remove
    );

    const fader = h('input', {
      class: 'fader-v',
      type: 'range',
      min: 0,
      max: 1,
      step: 0.001,
      value: String(track.fader),
      attrs: { 'aria-label': `Volumen de ${track.name}`, title: formatDb(faderToDb(track.fader)) },
      oninput: (event) => {
        const value = Number(event.target.value);
        event.target.title = formatDb(faderToDb(value));
        this.callbacks.onFader(track.id, value);
      },
    });

    const meterCanvas = h('canvas', { width: 10, height: 74 });
    this.meters.set(track.id, { canvas: meterCanvas, hold: -Infinity, holdTime: 0 });

    const body = h(
      'div',
      { class: 'strip__body' },
      h('div', { class: 'strip__fader' }, fader),
      h('div', { class: 'strip__meter' }, meterCanvas)
    );

    const mute = h('button', {
      class: 'mini mini--mute',
      type: 'button',
      text: 'M',
      title: 'Silenciar',
      attrs: { 'aria-pressed': String(track.muted), 'aria-label': 'Silenciar pista' },
      onclick: (event) => {
        event.stopPropagation();
        this.callbacks.onMute(track.id, !track.muted);
      },
    });

    const solo = h('button', {
      class: 'mini mini--solo',
      type: 'button',
      text: 'S',
      title: 'Solo',
      attrs: { 'aria-pressed': String(track.solo), 'aria-label': 'Solo de la pista' },
      onclick: (event) => {
        event.stopPropagation();
        this.callbacks.onSolo(track.id, !track.solo);
      },
    });

    const pan = h('input', {
      class: 'pan-slider',
      type: 'range',
      min: -1,
      max: 1,
      step: 0.01,
      value: String(track.pan),
      attrs: { 'aria-label': `Panorama de ${track.name}`, title: 'Panorama' },
      oninput: (event) => this.callbacks.onPan(track.id, Number(event.target.value)),
    });

    const panLabel = h('span', {
      class: 'strip__pan-label',
      text: panText(track.pan),
    });
    pan.addEventListener('input', () => {
      panLabel.textContent = panText(Number(pan.value));
    });

    const row = h('div', { class: 'strip__row' }, mute, solo, pan, panLabel);

    strip.append(head, body, row);
    return strip;
  }

  /** Actualiza los vúmetros (se llama desde el bucle de animación). */
  updateMeters(now = performance.now()) {
    if (!this.engine?.ctx) return;
    for (const [trackId, meter] of this.meters) {
      const level = this.engine.readTrackLevel(trackId);
      const db = level.peak;
      if (db > meter.hold) {
        meter.hold = db;
        meter.holdTime = now;
      } else if (now - meter.holdTime > 900) {
        meter.hold = Math.max(-Infinity, meter.hold - 0.6);
      }
      drawVerticalMeter(meter.canvas, db, meter.hold);
    }
  }

  /** Actualiza un valor sin re-renderizar toda la lista (por ejemplo, al mover el fader). */
  syncTrack(track) {
    const strip = this.container.querySelector(`.strip[data-track-id="${track.id}"]`);
    if (!strip) return;
    strip.classList.toggle('is-armed', track.armed);
    const arm = strip.querySelector('.arm-btn');
    if (arm) arm.setAttribute('aria-pressed', String(track.armed));
    const mute = strip.querySelector('.mini--mute');
    if (mute) mute.setAttribute('aria-pressed', String(track.muted));
    const solo = strip.querySelector('.mini--solo');
    if (solo) solo.setAttribute('aria-pressed', String(track.solo));
  }
}

function panText(value) {
  if (Math.abs(value) < 0.01) return 'C';
  const amount = Math.round(Math.abs(value) * 100);
  return `${value < 0 ? 'I' : 'D'}${amount}`;
}
