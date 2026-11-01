/**
 * Inspector: rack de efectos de la pista seleccionada, máster, sesión y guía.
 * Todos los controles escriben en el modelo y el motor de audio se actualiza al
 * instante (no hay botón "aplicar").
 */

import { h, clear, sliderControl, switchControl, fxSection, fillSelect } from './dom.js';
import { formatDb } from '../util/format.js';
import { REVERB_PRESETS } from '../audio/impulse.js';
import { projectDuration, hasSolo } from '../state.js';

const dbFormat = (value) => formatDb(value, 1);
const hzFormat = (value) => (value >= 1000 ? `${(value / 1000).toFixed(1)} kHz` : `${Math.round(value)} Hz`);
const pctFormat = (value) => `${Math.round(value * 100)} %`;
const msFormat = (value) => `${Math.round(value * 1000)} ms`;
const ratioFormat = (value) => `${value.toFixed(1)}:1`;

/** Slider de frecuencia con escala logarítmica (20 Hz – 20 kHz). */
function frequencyControl({ label, value, onChange, min = 20, max = 20000 }) {
  const toSlider = (hz) => (Math.log(hz / min) / Math.log(max / min)) * 100;
  const fromSlider = (pos) => min * Math.pow(max / min, pos / 100);
  const valueLabel = h('span', { class: 'control__value', text: hzFormat(value) });
  const input = h('input', {
    type: 'range',
    min: 0,
    max: 100,
    step: 0.1,
    value: String(toSlider(value)),
    attrs: { 'aria-label': label },
    oninput: (event) => {
      const hz = Math.round(fromSlider(Number(event.target.value)));
      valueLabel.textContent = hzFormat(hz);
      onChange(hz);
    },
  });
  return h(
    'div',
    { class: 'control' },
    h('div', { class: 'control__head' }, h('span', { class: 'control__label', text: label }), valueLabel),
    input
  );
}

export class Inspector {
  constructor({ panelTrack, panelMaster, panelSession, panelHelp, callbacks }) {
    this.panelTrack = panelTrack;
    this.panelMaster = panelMaster;
    this.panelSession = panelSession;
    this.panelHelp = panelHelp;
    this.callbacks = callbacks;
    this.project = null;
    this.selectedTrackId = null;
    this.liveRefs = {};
    this.renderHelp();
  }

  setProject(project) {
    this.project = project;
  }

  setSelectedTrack(id) {
    this.selectedTrackId = id;
  }

  render() {
    if (!this.project) return;
    this.renderTrack();
    this.renderMaster();
    this.renderSession();
  }

  // ------------------------------------------------------------------ Pista
  renderTrack() {
    const panel = this.panelTrack;
    clear(panel);
    const track = this.project.tracks.find((t) => t.id === this.selectedTrackId);
    if (!track) {
      panel.appendChild(
        h('p', { class: 'hint', text: 'Selecciona una pista para ver y ajustar sus efectos.' })
      );
      return;
    }
    const fx = track.fx;
    const update = (mutate) => {
      mutate(fx);
      this.callbacks.onFxChange(track.id);
    };

    panel.append(
      h('h3', { class: 'panel-title', text: track.name }),
      h(
        'label',
        { class: 'field field--block' },
        h('span', { class: 'field__label', text: 'Canal de entrada' }),
        (() => {
          const select = h('select', {
            class: 'field__input',
            onchange: (event) => this.callbacks.onInputChannel(track.id, event.target.value),
          });
          fillSelect(
            select,
            [
              { value: 'mono', label: 'Mono (recomendado para voz)' },
              { value: 'stereo', label: 'Estéreo' },
            ],
            track.inputChannel ?? 'mono'
          );
          return select;
        })()
      )
    );

    // --- Puerta de ruido
    panel.appendChild(
      fxSection({
        title: 'Puerta de ruido',
        enabled: fx.gate.on,
        onToggle: (on) => {
          update((f) => (f.gate.on = on));
          this.renderTrack();
        },
        open: fx.gate.on,
        children: [
          sliderControl({
            label: 'Umbral',
            min: -80,
            max: 0,
            step: 0.5,
            value: fx.gate.threshold,
            format: dbFormat,
            onChange: (v) => update((f) => (f.gate.threshold = v)),
          }),
          sliderControl({
            label: 'Atenuación al cerrar',
            min: -80,
            max: 0,
            step: 1,
            value: fx.gate.range,
            format: dbFormat,
            onChange: (v) => update((f) => (f.gate.range = v)),
          }),
          sliderControl({
            label: 'Ataque',
            min: 0.0005,
            max: 0.05,
            step: 0.0005,
            value: fx.gate.attack,
            format: msFormat,
            onChange: (v) => update((f) => (f.gate.attack = v)),
          }),
          sliderControl({
            label: 'Mantenimiento',
            min: 0,
            max: 0.4,
            step: 0.005,
            value: fx.gate.hold,
            format: msFormat,
            onChange: (v) => update((f) => (f.gate.hold = v)),
          }),
          sliderControl({
            label: 'Liberación',
            min: 0.01,
            max: 1,
            step: 0.005,
            value: fx.gate.release,
            format: msFormat,
            onChange: (v) => update((f) => (f.gate.release = v)),
          }),
        ],
      })
    );

    // --- Paso alto
    panel.appendChild(
      fxSection({
        title: 'Filtro paso alto',
        enabled: fx.hp.on,
        onToggle: (on) => {
          update((f) => (f.hp.on = on));
          this.renderTrack();
        },
        open: fx.hp.on,
        children: [
          frequencyControl({
            label: 'Frecuencia de corte',
            value: fx.hp.freq,
            onChange: (v) => update((f) => (f.hp.freq = v)),
          }),
          h('p', {
            class: 'hint',
            text: 'Quita los ruidos graves y el retumbe de la habitación. Para voz, entre 70 y 110 Hz.',
          }),
        ],
      })
    );

    // --- Ecualizador
    const eqChildren = fx.eq.bands.map((band, index) => {
      const names = ['Graves (estante)', 'Medios-bajos', 'Medios-altos', 'Agudos (estante)'];
      return h(
        'div',
        { class: 'control' },
        h('div', { class: 'control__head' }, h('span', { class: 'control__label', text: names[index] })),
        frequencyControl({
          label: `${names[index]} — frecuencia`,
          value: band.freq,
          onChange: (v) => {
            band.freq = v;
            this.callbacks.onFxChange(track.id);
          },
        }),
        h(
          'div',
          { class: 'eq-band' },
          h('span', { class: 'eq-band__label', text: 'Ganancia' }),
          h('input', {
            type: 'range',
            min: -15,
            max: 15,
            step: 0.5,
            value: String(band.gain),
            attrs: { 'aria-label': `Ganancia ${names[index]}` },
            oninput: (event) => {
              band.gain = Number(event.target.value);
              const out = event.target.parentElement.querySelector('.eq-band__value');
              if (out) out.textContent = `${band.gain > 0 ? '+' : ''}${band.gain} dB`;
              this.callbacks.onFxChange(track.id);
            },
          }),
          h('span', { class: 'eq-band__value', text: `${band.gain > 0 ? '+' : ''}${band.gain} dB` })
        )
      );
    });
    panel.appendChild(
      fxSection({
        title: 'Ecualizador (4 bandas)',
        enabled: fx.eq.on,
        onToggle: (on) => {
          update((f) => (f.eq.on = on));
          this.renderTrack();
        },
        open: fx.eq.on,
        children: [...eqChildren, h('p', { class: 'hint', text: 'Corta antes de subir: un pequeño corte suena más natural que un gran refuerzo.' })],
      })
    );

    // --- Compresor
    panel.appendChild(
      fxSection({
        title: 'Compresor',
        enabled: fx.comp.on,
        onToggle: (on) => {
          update((f) => (f.comp.on = on));
          this.renderTrack();
        },
        open: fx.comp.on,
        children: [
          sliderControl({
            label: 'Umbral',
            min: -60,
            max: 0,
            step: 0.5,
            value: fx.comp.threshold,
            format: dbFormat,
            onChange: (v) => update((f) => (f.comp.threshold = v)),
          }),
          sliderControl({
            label: 'Proporción',
            min: 1,
            max: 20,
            step: 0.1,
            value: fx.comp.ratio,
            format: ratioFormat,
            onChange: (v) => update((f) => (f.comp.ratio = v)),
          }),
          sliderControl({
            label: 'Ataque',
            min: 0.001,
            max: 0.2,
            step: 0.001,
            value: fx.comp.attack,
            format: msFormat,
            onChange: (v) => update((f) => (f.comp.attack = v)),
          }),
          sliderControl({
            label: 'Liberación',
            min: 0.02,
            max: 1,
            step: 0.01,
            value: fx.comp.release,
            format: msFormat,
            onChange: (v) => update((f) => (f.comp.release = v)),
          }),
          sliderControl({
            label: 'Codo (knee)',
            min: 0,
            max: 40,
            step: 1,
            value: fx.comp.knee,
            format: (v) => `${v} dB`,
            onChange: (v) => update((f) => (f.comp.knee = v)),
          }),
          sliderControl({
            label: 'Ganancia de compensación',
            min: -12,
            max: 24,
            step: 0.5,
            value: fx.comp.makeup,
            format: dbFormat,
            onChange: (v) => update((f) => (f.comp.makeup = v)),
          }),
        ],
      })
    );

    // --- Saturación
    panel.appendChild(
      fxSection({
        title: 'Saturación',
        enabled: fx.sat.on,
        onToggle: (on) => {
          update((f) => (f.sat.on = on));
          this.renderTrack();
        },
        open: fx.sat.on,
        children: [
          sliderControl({
            label: 'Drive',
            min: 0,
            max: 1,
            step: 0.01,
            value: fx.sat.drive,
            format: pctFormat,
            onChange: (v) => update((f) => (f.sat.drive = v)),
          }),
          sliderControl({
            label: 'Mezcla',
            min: 0,
            max: 1,
            step: 0.01,
            value: fx.sat.mix,
            format: pctFormat,
            onChange: (v) => update((f) => (f.sat.mix = v)),
          }),
          h('p', { class: 'hint', text: 'Añade cuerpo y armónicos. Con voz, mantén la mezcla por debajo del 30 %.' }),
        ],
      })
    );

    // --- Envíos
    panel.appendChild(
      fxSection({
        title: 'Envíos de efectos',
        open: true,
        children: [
          sliderControl({
            label: 'Reverb',
            min: 0,
            max: 1,
            step: 0.01,
            value: fx.sendReverb,
            format: pctFormat,
            onChange: (v) => update((f) => (f.sendReverb = v)),
          }),
          sliderControl({
            label: 'Delay',
            min: 0,
            max: 1,
            step: 0.01,
            value: fx.sendDelay,
            format: pctFormat,
            onChange: (v) => update((f) => (f.sendDelay = v)),
          }),
        ],
      })
    );
  }

  // ------------------------------------------------------------------ Máster
  renderMaster() {
    const panel = this.panelMaster;
    clear(panel);
    const master = this.project.master;
    const update = (mutate, rebuild = false) => {
      mutate(master);
      this.callbacks.onMasterChange();
      if (rebuild) this.renderMaster();
    };

    const lufsValue = h('dd', { text: '−∞ LUFS' });
    const peakValue = h('dd', { text: '−∞ dBFS' });
    this.liveRefs.masterLufs = lufsValue;
    this.liveRefs.masterPeak = peakValue;

    panel.append(
      h('h3', { class: 'panel-title', text: 'Bus maestro' }),
      h(
        'dl',
        { class: 'summary' },
        h('div', {}, h('dt', { text: 'Sonoridad integrada' }), lufsValue),
        h('div', {}, h('dt', { text: 'Pico de salida' }), peakValue)
      ),
      sliderControl({
        label: 'Volumen maestro',
        min: 0,
        max: 1,
        step: 0.005,
        value: master.volume,
        format: (v) => formatDb(20 * Math.log10(Math.max(v, 1e-6))),
        onChange: (v) => update((m) => (m.volume = v)),
      })
    );

    panel.appendChild(
      fxSection({
        title: 'Ecualizador del máster',
        open: true,
        children: [
          sliderControl({
            label: 'Graves',
            min: -10,
            max: 10,
            step: 0.5,
            value: master.eq.low,
            format: dbFormat,
            onChange: (v) => update((m) => (m.eq.low = v)),
          }),
          sliderControl({
            label: 'Medios',
            min: -10,
            max: 10,
            step: 0.5,
            value: master.eq.mid,
            format: dbFormat,
            onChange: (v) => update((m) => (m.eq.mid = v)),
          }),
          sliderControl({
            label: 'Agudos',
            min: -10,
            max: 10,
            step: 0.5,
            value: master.eq.high,
            format: dbFormat,
            onChange: (v) => update((m) => (m.eq.high = v)),
          }),
        ],
      })
    );

    panel.appendChild(
      fxSection({
        title: 'Compresor de bus',
        enabled: master.comp.on,
        onToggle: (on) => update((m) => (m.comp.on = on), true),
        open: master.comp.on,
        children: [
          sliderControl({
            label: 'Umbral',
            min: -40,
            max: 0,
            step: 0.5,
            value: master.comp.threshold,
            format: dbFormat,
            onChange: (v) => update((m) => (m.comp.threshold = v)),
          }),
          sliderControl({
            label: 'Proporción',
            min: 1,
            max: 10,
            step: 0.1,
            value: master.comp.ratio,
            format: ratioFormat,
            onChange: (v) => update((m) => (m.comp.ratio = v)),
          }),
          sliderControl({
            label: 'Ataque',
            min: 0.001,
            max: 0.1,
            step: 0.001,
            value: master.comp.attack,
            format: msFormat,
            onChange: (v) => update((m) => (m.comp.attack = v)),
          }),
          sliderControl({
            label: 'Liberación',
            min: 0.05,
            max: 1,
            step: 0.01,
            value: master.comp.release,
            format: msFormat,
            onChange: (v) => update((m) => (m.comp.release = v)),
          }),
        ],
      })
    );

    panel.appendChild(
      fxSection({
        title: 'Limitador de picos',
        enabled: master.limiter.on,
        onToggle: (on) => update((m) => (m.limiter.on = on), true),
        open: true,
        children: [
          sliderControl({
            label: 'Techo',
            min: -6,
            max: 0,
            step: 0.1,
            value: master.limiter.ceiling,
            format: dbFormat,
            onChange: (v) => update((m) => (m.limiter.ceiling = v)),
          }),
          sliderControl({
            label: 'Liberación',
            min: 0.01,
            max: 0.5,
            step: 0.005,
            value: master.limiter.release,
            format: msFormat,
            onChange: (v) => update((m) => (m.limiter.release = v)),
          }),
          h('p', { class: 'hint', text: 'Con el techo en −1 dBFS evitas distorsión al convertir a MP3.' }),
        ],
      })
    );

    // Buses de efectos
    const presetSelect = h('select', {
      class: 'field__input',
      onchange: (event) => this.callbacks.onReverbPreset(event.target.value),
    });
    fillSelect(
      presetSelect,
      Object.entries(REVERB_PRESETS).map(([value, preset]) => ({ value, label: preset.label })),
      this.project.fxBuses.reverbPreset
    );

    panel.appendChild(
      fxSection({
        title: 'Reverb (bus)',
        open: false,
        children: [
          h('label', { class: 'field field--block' }, h('span', { class: 'field__label', text: 'Tipo de sala' }), presetSelect),
          sliderControl({
            label: 'Nivel de retorno',
            min: 0,
            max: 1.5,
            step: 0.01,
            value: this.project.fxBuses.reverbReturn,
            format: pctFormat,
            onChange: (v) => this.callbacks.onFxBusChange({ reverbReturn: v }),
          }),
        ],
      })
    );

    panel.appendChild(
      fxSection({
        title: 'Delay (bus)',
        open: false,
        children: [
          sliderControl({
            label: 'Tiempo',
            min: 0.05,
            max: 1.2,
            step: 0.005,
            value: this.project.fxBuses.delayTime,
            format: msFormat,
            onChange: (v) => this.callbacks.onFxBusChange({ delayTime: v }),
          }),
          sliderControl({
            label: 'Realimentación',
            min: 0,
            max: 0.85,
            step: 0.01,
            value: this.project.fxBuses.delayFeedback,
            format: pctFormat,
            onChange: (v) => this.callbacks.onFxBusChange({ delayFeedback: v }),
          }),
          sliderControl({
            label: 'Nivel de retorno',
            min: 0,
            max: 1.5,
            step: 0.01,
            value: this.project.fxBuses.delayReturn,
            format: pctFormat,
            onChange: (v) => this.callbacks.onFxBusChange({ delayReturn: v }),
          }),
        ],
      })
    );
  }

  updateMasterLive(levels) {
    if (this.liveRefs.masterLufs) {
      this.liveRefs.masterLufs.textContent = Number.isFinite(levels.integrated)
        ? `${levels.integrated.toFixed(1)} LUFS`
        : '−∞ LUFS';
    }
    if (this.liveRefs.masterPeak) {
      this.liveRefs.masterPeak.textContent = Number.isFinite(levels.peakDb)
        ? formatDb(levels.peakDb)
        : '−∞ dBFS';
    }
  }

  // ------------------------------------------------------------------ Sesión
  renderSession() {
    const panel = this.panelSession;
    clear(panel);
    const session = this.project.session;
    const update = (mutate) => {
      mutate(session);
      this.callbacks.onSessionChange();
    };

    panel.append(
      h('h3', { class: 'panel-title', text: 'Ajustes de la sesión' }),
      sliderControl({
        label: 'Tempo (BPM)',
        min: 40,
        max: 240,
        step: 1,
        value: session.bpm,
        format: (v) => `${Math.round(v)}`,
        onChange: (v) => update((s) => (s.bpm = Math.round(v))),
      }),
      sliderControl({
        label: 'Cuenta de entrada al grabar',
        min: 0,
        max: 2,
        step: 1,
        value: Math.round((session.countInBeats ?? 4) / 4),
        format: (v) => (v === 0 ? 'sin cuenta' : `${Math.round(v)} compás`),
        onChange: (v) => update((s) => (s.countInBeats = Math.round(v) * 4)),
      }),
      sliderControl({
        label: 'Volumen del metrónomo',
        min: 0,
        max: 1,
        step: 0.01,
        value: session.metronome?.volume ?? 0.55,
        format: pctFormat,
        onChange: (v) => update((s) => (s.metronome = { ...s.metronome, volume: v })),
      }),
      h(
        'label',
        { class: 'field field--block' },
        h('span', { class: 'field__label', text: 'Canales al grabar' }),
        (() => {
          const select = h('select', {
            class: 'field__input',
            onchange: (event) => update((s) => (s.recordChannels = Number(event.target.value))),
          });
          fillSelect(
            select,
            [
              { value: 1, label: 'Mono (voz, guitarra, micrófono)' },
              { value: 2, label: 'Estéreo (2 canales de la interfaz)' },
            ],
            session.recordChannels ?? 1
          );
          return select;
        })()
      ),
      sliderControl({
        label: 'Compensación de latencia',
        min: -200,
        max: 200,
        step: 1,
        value: session.latencyOffset ?? 0,
        format: (v) => `${Math.round(v)} ms`,
        onChange: (v) => update((s) => (s.latencyOffset = Math.round(v))),
      }),
      sliderControl({
        label: 'Zoom de la línea de tiempo',
        min: 20,
        max: 400,
        step: 1,
        value: session.zoom ?? 80,
        format: (v) => `${Math.round(v)} px/s`,
        onChange: (v) => this.callbacks.onZoomChange(Math.round(v)),
      })
    );

    const stats = h('dl', { class: 'summary' });
    const duration = projectDuration(this.project);
    const totals = [
      ['Duración', formatDuration(duration)],
      ['Pistas', String(this.project.tracks.length)],
      ['Clips', String(this.project.clips.length)],
      ['Frecuencia', '48 kHz / 32 bits float'],
      ['Solo activo', hasSolo(this.project) ? 'sí' : 'no'],
    ];
    for (const [label, value] of totals) {
      stats.append(h('div', {}, h('dt', { text: label }), h('dd', { text: value })));
    }

    panel.append(
      h('h3', { class: 'panel-title', text: 'Sesión' }),
      stats,
      h(
        'div',
        { class: 'callout' },
        h('strong', { text: 'Espacio de almacenamiento' }),
        h('p', { id: 'session-storage', text: 'Calculando…' })
      ),
      h(
        'div',
        { class: 'callout' },
        h('strong', { text: 'Bucle' }),
        h('p', { text: 'Mantén pulsada la tecla Mayús y arrastra sobre la regla de tiempo para marcar la región de bucle. Con L se activa o desactiva.' })
      ),
      h(
        'div',
        { class: 'callout' },
        h('strong', { text: 'Zona de riesgo' }),
        h('p', { text: 'Vaciar la sesión borra todas las pistas y audios de este proyecto.' }),
        h(
          'div',
          { class: 'callout__actions' },
          h('button', {
            type: 'button',
            class: 'btn btn--danger-ghost btn--sm',
            text: 'Vaciar sesión',
            onclick: () => this.callbacks.onClearSession(),
          })
        )
      )
    );
  }

  setStorageText(text) {
    const node = document.getElementById('session-storage');
    if (node) node.textContent = text;
  }

  // ------------------------------------------------------------------ Guía
  renderHelp() {
    const panel = this.panelHelp;
    clear(panel);
    panel.appendChild(
      h(
        'div',
        { class: 'prose' },
        h('h3', { text: 'Cómo grabar con el BM800 y la V8' }),
        h(
          'ol',
          {},
          h('li', { text: 'Conecta el BM800 a la entrada XLR de la V8 y activa +48V (alimentación fantasma).' }),
          h('li', { text: 'Conecta la V8 por USB. En "Micrófono", elige la V8 como dispositivo.' }),
          h('li', { text: 'Apaga el ECO/REVERB de la V8: aquí añadimos reverb con control real.' }),
          h('li', { text: 'Sube el gain de la V8 hasta que la voz llegue a −18/−12 dBFS, sin tocar el rojo.' }),
          h('li', { text: 'Arma la pista, pon la cuenta de entrada y pulsa Grabar.' }),
          h('li', { text: 'Graba en seco, varias tomas si quieres. Luego elige la buena y aplícale efectos.' })
        ),
        h('h3', { text: 'Qué hace este estudio (de verdad)' }),
        h(
          'ul',
          {},
          h('li', { text: 'Grabación PCM sin compresión, alineada al sample, con compensación de latencia.' }),
          h('li', { text: 'Multipista ilimitado con overdubs, bucle, metrónomo y cuenta de entrada.' }),
          h('li', { text: 'Por pista: puerta de ruido, paso alto, EQ de 4 bandas, compresor, saturación y envíos a reverb y delay.' }),
          h('li', { text: 'Máster: EQ, compresor de bus, limitador con lookahead y medición LUFS (BS.1770).' }),
          h('li', { text: 'Mezcla final idéntica a lo escuchado y exportación a WAV 16/24/32 bits y MP3 de 128 a 320 kbps.' }),
          h('li', { text: 'Proyectos guardados en el equipo (IndexedDB) y funcionamiento sin conexión.' })
        ),
        h('h3', { text: 'Lo que no incluye (para no engañarte)' }),
        h(
          'ul',
          {},
          h('li', { text: 'No hay corrección de afinidad automática (autotune) ni plugin VST/AU.' }),
          h('li', { text: 'No hay instrumentos MIDI ni secuenciador de notas.' }),
          h('li', { text: 'La calidad final depende del micrófono, la sala y la técnica: el software no sustituye eso.' })
        ),
        h('h3', { text: 'Consejos de calidad' }),
        h(
          'ul',
          {},
          h('li', { text: 'Usa auriculares cerrados para monitorizar: evita acoples y sangrado.' }),
          h('li', { text: 'Graba a 48 kHz (lo hace por defecto) y exporta WAV si vas a seguir mezclando.' }),
          h('li', { text: 'Deja aire: el pico de la voz entre −18 y −12 dBFS, no cerca de 0.' }),
          h('li', { text: 'Aplica el limitador solo al final: si lo usas durante la mezcla, te engaña el volumen.' })
        )
      )
    );
  }
}

function formatDuration(seconds) {
  if (!Number.isFinite(seconds) || seconds <= 0) return '0:00';
  const m = Math.floor(seconds / 60);
  const s = Math.floor(seconds % 60);
  return `${m}:${String(s).padStart(2, '0')}`;
}
