/**
 * Diálogos: entrada de audio, exportación, proyectos y diagnóstico.
 * Todo lo que prometen los botones se ejecuta de verdad (grabar, renderizar,
 * codificar, guardar).
 */

import { h, clear, fillSelect, openDialog, closeDialog, toast, downloadBlob, drawMeter } from './dom.js';
import { formatDb, formatTime, formatBytes, safeFileName } from '../util/format.js';
import { projectDuration } from '../state.js';
import { computeProjectEnd, renderProject } from '../audio/render.js';
import { encodeMp3 } from '../audio/mp3.js';
import { encodeWav } from '../audio/wav.js';
import { integratedLoudness, truePeakDb } from '../audio/lufs.js';
import * as db from '../storage/db.js';

export class Dialogs {
  constructor({ app }) {
    this.app = app;
    this.lastExport = null; // {blob, filename}
    this.testRecording = null;
    this.testBuffer = null;
    this.stemBlobs = [];
    this.pendingTest = null;
    this.bindInputDialog();
    this.bindExportDialog();
    this.bindProjectDialog();
    this.bindDiagnostics();
  }

  get engine() {
    return this.app.engine;
  }

  get project() {
    return this.app.project;
  }

  // ======================================================== Entrada de audio
  bindInputDialog() {
    document.getElementById('device-select').addEventListener('change', async (event) => {
      try {
        await this.openDevice(event.target.value);
      } catch (error) {
        toast(error.message ?? 'No se pudo abrir el dispositivo', 'error');
      }
    });

    document.getElementById('channel-mode').addEventListener('change', (event) => {
      this.engine.applyChannelMode(event.target.value);
      this.app.updateInputUi();
    });

    document.getElementById('setup-gain').addEventListener('input', (event) => {
      const value = Number(event.target.value);
      this.engine.setInputGain(value);
      document.getElementById('setup-gain-value').textContent = formatDb(20 * Math.log10(Math.max(value, 1e-6)));
      const stripGain = document.getElementById('input-gain');
      if (stripGain) stripGain.value = String(value);
    });

    document.getElementById('setup-monitor').addEventListener('change', (event) => {
      this.engine.setMonitoring(event.target.checked);
      this.project.session.monitor = event.target.checked;
      this.app.syncInputButton();
    });

    document.getElementById('setup-latency').addEventListener('input', (event) => {
      const value = Number(event.target.value);
      this.project.session.latencyOffset = value;
      document.getElementById('setup-latency-value').textContent = `${value} ms`;
      this.app.markDirty();
    });

    document.getElementById('btn-open-input').addEventListener('click', async () => {
      const select = document.getElementById('device-select');
      try {
        await this.openDevice(select.value || undefined, true);
      } catch (error) {
        toast(error.message ?? 'No se pudo abrir el micrófono', 'error');
      }
    });

    document.getElementById('btn-test-record').addEventListener('click', () => this.runTestRecording());
    document.getElementById('btn-test-play').addEventListener('click', () => this.playTestRecording());
  }

  async openInputDialog() {
    openDialog('dialog-input');
    try {
      await this.app.ensureAudio();
    } catch (error) {
      toast(`No se pudo iniciar el audio: ${error.message}`, 'error', 6000);
      return;
    }
    this.refreshDevices();
    const session = this.project.session;
    document.getElementById('setup-latency').value = String(session.latencyOffset ?? 0);
    document.getElementById('setup-latency-value').textContent = `${session.latencyOffset ?? 0} ms`;
    document.getElementById('setup-monitor').checked = Boolean(session.monitor);
    const gain = this.app.inputGain ?? 1;
    document.getElementById('setup-gain').value = String(gain);
    document.getElementById('setup-gain-value').textContent = formatDb(20 * Math.log10(Math.max(gain, 1e-6)));
    if (this.engine?.input?.channelMode) {
      document.getElementById('channel-mode').value = this.engine.input.channelMode;
    }
  }

  async refreshDevices() {
    const select = document.getElementById('device-select');
    if (!select) return;
    try {
      const devices = await this.engine.listInputDevices();
      const current = this.engine?.input?.deviceId ?? '';
      if (!devices.length) {
        fillSelect(select, [{ value: '', label: 'No se detectan entradas — da permiso al micrófono' }], '');
        return;
      }
      fillSelect(
        select,
        devices.map((d, index) => ({
          value: d.deviceId,
          label: d.label && d.label !== 'Entrada sin nombre' ? d.label : `Entrada ${index + 1}`,
        })),
        current || devices[0].deviceId
      );
    } catch (error) {
      toast(`No se pudieron listar los dispositivos: ${error.message}`, 'error');
    }
  }

  async openDevice(deviceId, requestPermission = false) {
    await this.app.ensureAudio();
    const info = await this.engine.openInput(deviceId || undefined);
    this.app.inputGain = 1;
    this.engine.setInputGain(1);
    document.getElementById('setup-monitor').checked = Boolean(this.project.session.monitor);
    this.engine.setMonitoring(Boolean(this.project.session.monitor));
    this.app.updateInputUi();
    await this.refreshDevices();
    const label = info.deviceLabel || 'entrada';
    toast(`Entrada abierta: ${label}`, 'ok');
    if (requestPermission) closeDialog('dialog-input');
    return info;
  }

  async runTestRecording() {
    const button = document.getElementById('btn-test-record');
    const playButton = document.getElementById('btn-test-play');
    if (this.app.transport.isPlaying()) {
      toast('Detén la reproducción antes de probar la grabación', 'warn');
      return;
    }
    if (!this.engine.input.stream) {
      toast('Primero abre la entrada de audio', 'warn');
      return;
    }
    button.disabled = true;
    button.textContent = 'Grabando 3 s…';
    try {
      const startFrame = Math.round(this.engine.ctx.currentTime * this.engine.ctx.sampleRate);
      const promise = this.engine.recorder.start({ startFrame, channels: 1 });
      await new Promise((resolve) => setTimeout(resolve, 3050));
      const result = await this.engine.recorder.stop();
      const recording = result ?? (await promise);
      if (!recording || !recording.frames) {
        toast('No se capturó audio. Revisa el dispositivo y el permiso.', 'error');
        return;
      }
      const buffer = this.engine.ctx.createBuffer(
        1,
        recording.frames,
        recording.sampleRate
      );
      buffer.copyToChannel(recording.channels[0], 0);
      this.testBuffer = buffer;

      let peak = 0;
      const data = recording.channels[0];
      for (let i = 0; i < data.length; i++) peak = Math.max(peak, Math.abs(data[i]));
      const peakDb = peak > 0 ? 20 * Math.log10(peak) : -Infinity;
      playButton.disabled = false;
      button.textContent = 'Grabar 3 s';
      if (peakDb < -45) {
        toast('Se grabó, pero casi en silencio: sube el gain de la V8 o revisa +48V.', 'warn', 6000);
      } else {
        toast(`Prueba grabada. Pico: ${formatDb(peakDb)}. Pulsa "Escuchar".`, 'ok', 6000);
      }
    } catch (error) {
      toast(`Error en la prueba: ${error.message}`, 'error');
    } finally {
      button.disabled = false;
      button.textContent = 'Grabar 3 s';
    }
  }

  playTestRecording() {
    if (!this.testBuffer) return;
    const source = this.engine.ctx.createBufferSource();
    source.buffer = this.testBuffer;
    source.connect(this.engine.master.input);
    source.start();
    toast('Reproduciendo la prueba…', 'info', 2000);
  }

  updateSetupMeters(level) {
    const dialog = document.getElementById('dialog-input');
    if (!dialog?.open) return;
    const canvas = document.getElementById('setup-meter');
    if (!canvas) return;
    drawMeter(canvas, level.peak, { hold: level.peak });
    const peakLabel = document.getElementById('setup-peak-label');
    if (peakLabel) peakLabel.textContent = Number.isFinite(level.peak) ? formatDb(level.peak) : '−∞ dBFS';
    const clipLabel = document.getElementById('setup-clip-label');
    if (clipLabel) clipLabel.textContent = level.clipping ? '¡SATURACIÓN!' : '';
  }

  // ============================================================= Exportación
  bindExportDialog() {
    const formatSelect = document.getElementById('export-format');
    formatSelect.addEventListener('change', () => this.syncExportFields());

    document.getElementById('btn-do-export').addEventListener('click', () => this.runExport());
    document.getElementById('btn-download').addEventListener('click', () => {
      if (!this.lastExport) return;
      downloadBlob(this.lastExport.blob, this.lastExport.filename);
    });
  }

  openExportDialog() {
    openDialog('dialog-export');
    document.getElementById('export-result').hidden = true;
    document.getElementById('export-progress').hidden = true;
    document.getElementById('btn-download').hidden = true;
    document.getElementById('export-status').textContent =
      'La mezcla se calcula en tu equipo; no se sube nada a internet.';
    this.syncExportFields();
    this.updateExportSummary();
  }

  syncExportFields() {
    const format = document.getElementById('export-format').value;
    document.getElementById('export-bitrate-field').hidden = format !== 'mp3';
    document.getElementById('export-depth-field').hidden = format !== 'wav';
    this.updateExportSummary();
  }

  updateExportSummary() {
    const project = this.project;
    const duration = projectDuration(project);
    const tracksWithAudio = new Set(project.clips.map((c) => c.trackId)).size;
    document.getElementById('export-summary-duration').textContent = formatTime(duration, false);
    document.getElementById('export-summary-tracks').textContent = String(tracksWithAudio);

    const format = document.getElementById('export-format').value;
    const summary = document.getElementById('export-summary-format');
    if (format === 'mp3') {
      const bitrate = document.getElementById('export-bitrate').value;
      summary.textContent = `MP3 ${bitrate} kbps · 48 kHz · estéreo`;
    } else {
      const depth = document.getElementById('export-depth').value;
      summary.textContent = `WAV ${depth} bits · 48 kHz · estéreo`;
    }
    document.getElementById('export-summary-lufs').textContent = 'se calcula al exportar';
    document.getElementById('export-range').disabled = !project.session.loop?.on;
  }

  async runExport() {
    const project = this.project;
    if (!project.clips.length) {
      toast('No hay audio para exportar', 'warn');
      return;
    }

    const format = document.getElementById('export-format').value;
    const bitrate = Number(document.getElementById('export-bitrate').value);
    const depth = Number(document.getElementById('export-depth').value);
    const range = document.getElementById('export-range').value;
    const applyMaster = document.getElementById('export-master').checked;
    const normalize = document.getElementById('export-normalize').checked;
    const withStems = document.getElementById('export-stems').checked;

    const start = range === 'loop' && project.session.loop?.on ? project.session.loop.start : 0;
    const end =
      range === 'loop' && project.session.loop?.on ? project.session.loop.end : computeProjectEnd(project);

    const progress = document.getElementById('export-progress');
    const bar = document.getElementById('export-progress-bar');
    const status = document.getElementById('export-status');
    const button = document.getElementById('btn-do-export');
    const result = document.getElementById('export-result');

    progress.hidden = false;
    result.hidden = true;
    button.disabled = true;
    bar.style.width = '4%';
    status.textContent = 'Mezclando…';

    try {
      const rendered = await renderProject(project, {
        buffers: this.app.buffers,
        start,
        end,
        includeMasterFx: applyMaster,
        normalize,
        onProgress: (value) => {
          bar.style.width = `${Math.round(4 + value * 60)}%`;
        },
      });

      const channels = [
        rendered.buffer.getChannelData(0),
        rendered.buffer.getChannelData(1),
      ];

      status.textContent = format === 'mp3' ? 'Codificando MP3…' : 'Escribiendo WAV…';
      bar.style.width = '68%';

      let blob;
      let filename;
      const name = safeFileName(project.name, 'mezcla');
      if (format === 'mp3') {
        blob = await encodeMp3(channels, rendered.sampleRate, {
          bitrate,
          tags: {
            title: project.name,
            artist: 'Nuby Studio',
            album: 'Sesión',
            year: String(new Date().getFullYear()),
            comment: 'Creado con Nuby Studio',
          },
          onProgress: (value) => {
            bar.style.width = `${Math.round(68 + value * 28)}%`;
          },
        });
        filename = `${name}.mp3`;
      } else {
        blob = new Blob([encodeWav(channels, rendered.sampleRate, { bitDepth: depth })], {
          type: 'audio/wav',
        });
        filename = `${name}-${depth}bit.wav`;
      }

      bar.style.width = '100%';
      this.lastExport = { blob, filename };

      const lufs = integratedLoudness(channels, rendered.sampleRate);
      const peak = truePeakDb(channels);

      const downloadButton = document.getElementById('btn-download');
      downloadButton.hidden = false;

      clear(result);
      result.append(
        h('strong', { text: 'Mezcla lista' }),
        h('div', {
          text: `${filename} · ${formatBytes(blob.size)} · ${formatTime(rendered.end - rendered.start, false)}`,
        }),
        h('div', {
          text: `Sonoridad integrada: ${Number.isFinite(lufs) ? `${lufs.toFixed(1)} LUFS` : 'silencio'} · True Peak: ${
            Number.isFinite(peak) ? formatDb(peak) : '−∞'
          }`,
        }),
        h('div', { class: 'hint', text: 'Pulsa "Descargar archivo" para guardarlo en tu equipo.' })
      );
      result.hidden = false;
      status.textContent = 'Listo.';

      // Descarga automática del archivo principal.
      downloadBlob(blob, filename);
      toast('Exportación completada', 'ok');

      if (withStems) await this.exportStems({ start, end, format, bitrate, depth, result });
    } catch (error) {
      console.error(error);
      status.textContent = `Error: ${error.message}`;
      toast(`No se pudo exportar: ${error.message}`, 'error', 7000);
    } finally {
      button.disabled = false;
    }
  }

  async exportStems({ start, end, format, bitrate, depth, result }) {
    const project = this.project;
    const tracks = project.tracks.filter((track) =>
      project.clips.some((clip) => clip.trackId === track.id)
    );
    if (!tracks.length) return;

    const list = h('div', { class: 'hint', text: 'Preparando stems…' });
    result.appendChild(list);

    for (let index = 0; index < tracks.length; index++) {
      const track = tracks[index];
      try {
        const rendered = await renderProject(project, {
          buffers: this.app.buffers,
          start,
          end,
          includeMasterFx: false,
          soloTrackId: track.id,
        });
        const channels = [rendered.buffer.getChannelData(0), rendered.buffer.getChannelData(1)];
        const name = safeFileName(`${project.name}-${track.name}`, 'stem');
        const blob =
          format === 'mp3'
            ? await encodeMp3(channels, rendered.sampleRate, { bitrate, tags: { title: track.name, artist: 'Nuby Studio' } })
            : new Blob([encodeWav(channels, rendered.sampleRate, { bitDepth: depth })], { type: 'audio/wav' });
        const filename = `${name}.${format === 'mp3' ? 'mp3' : 'wav'}`;
        this.stemBlobs.push({ blob, filename });
        list.appendChild(
          h(
            'div',
            { style: { marginTop: '4px' } },
            h('a', {
              href: URL.createObjectURL(blob),
              download: filename,
              text: `Descargar ${filename}`,
              style: { color: 'var(--accent-strong)' },
            })
          )
        );
      } catch (error) {
        list.appendChild(h('div', { text: `${track.name}: error (${error.message})` }));
      }
      if (index === tracks.length - 1 && list.firstChild) list.firstChild.remove();
    }
  }

  // ================================================================ Proyectos
  bindProjectDialog() {
    document.getElementById('btn-new-project').addEventListener('click', async () => {
      await this.app.newProject();
      closeDialog('dialog-projects');
    });
    document.getElementById('btn-save-project').addEventListener('click', async () => {
      await this.app.save(true);
      toast('Proyecto guardado', 'ok');
      this.refreshProjects();
    });
    document.getElementById('btn-clean-storage').addEventListener('click', async () => {
      try {
        // Se guarda primero: si no, los audios del proyecto abierto parecerían huérfanos.
        await this.app.save(true);
        const removed = await db.collectGarbage();
        toast(`Se eliminaron ${removed} audios sin uso`, 'ok');
        this.refreshProjects();
      } catch (error) {
        toast(`No se pudo limpiar: ${error.message}`, 'error');
      }
    });
  }

  async openProjectsDialog() {
    openDialog('dialog-projects');
    await this.refreshProjects();
  }

  async refreshProjects() {
    const list = document.getElementById('project-list');
    clear(list);
    try {
      const projects = await db.listProjects();
      if (!projects.length) {
        list.appendChild(h('p', { class: 'hint', text: 'Todavía no hay proyectos guardados.' }));
      }
      for (const project of projects) {
        const current = project.id === this.project.id;
        list.appendChild(
          h(
            'div',
            { class: `project-item${current ? ' is-current' : ''}` },
            h(
              'div',
              { class: 'project-item__info' },
              h('div', { class: 'project-item__name', text: project.name }),
              h('div', {
                class: 'project-item__meta',
                text: `${new Date(project.updatedAt).toLocaleString('es-VE')} · ${project.trackCount} pistas`,
              })
            ),
            current
              ? h('span', { class: 'hint', text: 'abierto' })
              : h('button', {
                  class: 'btn btn--ghost btn--sm',
                  type: 'button',
                  text: 'Abrir',
                  onclick: async () => {
                    await this.app.loadProjectById(project.id);
                    closeDialog('dialog-projects');
                  },
                }),
            h('button', {
              class: 'btn btn--danger-ghost btn--sm',
              type: 'button',
              text: 'Eliminar',
              onclick: async () => {
                if (!confirm(`¿Eliminar definitivamente "${project.name}"?`)) return;
                await db.deleteProject(project.id);
                if (project.id === this.project.id) await this.app.newProject();
                this.refreshProjects();
              },
            })
          )
        );
      }
    } catch (error) {
      list.appendChild(h('p', { class: 'hint', text: `Error al leer los proyectos: ${error.message}` }));
    }

    const info = document.getElementById('storage-info');
    const estimate = await db.storageEstimate();
    info.textContent = estimate
      ? `Usado: ${formatBytes(estimate.usage)} de ${formatBytes(estimate.quota)} disponibles.`
      : 'El navegador no informa del espacio de almacenamiento.';
  }

  // =============================================================== Diagnóstico
  bindDiagnostics() {
    document.getElementById('btn-run-diagnostics').addEventListener('click', () => this.runDiagnostics());
    document.getElementById('btn-test-encoder').addEventListener('click', () => this.testEncoder());
  }

  async openDiagnostics() {
    openDialog('dialog-diagnostics');
    await this.runDiagnostics();
  }

  async runDiagnostics() {
    const container = document.getElementById('diag-checks');
    clear(container);
    const log = document.getElementById('diag-log');
    log.textContent = '';
    const add = (title, status, detail) => {
      container.appendChild(
        h(
          'div',
          { class: 'diag__row' },
          h('span', { class: `diag__badge diag__badge--${status}`, text: status === 'ok' ? '✓' : status === 'warn' ? '!' : '×' }),
          h(
            'div',
            { class: 'diag__text' },
            h('div', { class: 'diag__title', text: title }),
            detail ? h('div', { class: 'diag__detail', text: detail }) : null
          )
        )
      );
    };

    const ctx = this.engine?.ctx;
    add(
      'Contexto de audio',
      ctx && ctx.state !== 'closed' ? 'ok' : 'warn',
      ctx ? `Estado: ${ctx.state} · ${ctx.sampleRate} Hz · base ${(ctx.baseLatency ?? 0).toFixed(3)} s` : 'Aún no se ha creado (abre la entrada o pulsa reproducir)'
    );

    add(
      'AudioWorklet (procesadores avanzados)',
      this.engine?.workletsReady ? 'ok' : 'warn',
      this.engine?.workletsReady
        ? 'Puerta de ruido, limitador con lookahead, grabador PCM y medidor LUFS activos.'
        : this.engine?.workletError ?? 'No disponible: la grabación usará el modo de respaldo (menos preciso).'
    );

    const devices = await this.engine?.listInputDevices?.().catch(() => []) ?? [];
    add(
      'Entradas de audio',
      devices.length ? 'ok' : 'warn',
      devices.length
        ? devices.map((d) => d.label).join(' · ')
        : 'No se detectan entradas o no hay permiso de micrófono.'
    );

    const input = this.engine?.input;
    add(
      'Entrada abierta',
      input?.stream ? 'ok' : 'warn',
      input?.stream
        ? `${input.deviceLabel || 'dispositivo'} · ${input.sampleRate ?? '?'} Hz · ${input.channelCount ?? '?'} canal(es) · modo ${input.channelMode}`
        : 'Todavía no se ha abierto el micrófono.'
    );

    const latency = this.engine?.getLatencyInfo?.();
    add(
      'Latencia declarada',
      latency ? 'ok' : 'warn',
      latency
        ? `Salida ${(latency.output * 1000).toFixed(1)} ms + base ${(latency.base * 1000).toFixed(1)} ms. Compensación manual: ${this.project.session.latencyOffset ?? 0} ms.`
        : 'No disponible'
    );

    let permission = 'desconocido';
    try {
      if (navigator.permissions?.query) {
        const status = await navigator.permissions.query({ name: 'microphone' });
        permission = status.state;
      }
    } catch {
      permission = 'no consultable en este navegador';
    }
    add('Permiso de micrófono', permission === 'granted' ? 'ok' : 'warn', `Estado: ${permission}`);

    add(
      'Almacenamiento (IndexedDB)',
      'indexedDB' in window ? 'ok' : 'fail',
      'indexedDB' in window ? 'Los proyectos y audios se guardan en el equipo.' : 'No disponible: no se podrán guardar proyectos.'
    );

    const estimate = await db.storageEstimate();
    if (estimate) {
      add(
        'Espacio',
        estimate.usage < estimate.quota * 0.9 ? 'ok' : 'warn',
        `${formatBytes(estimate.usage)} usados de ${formatBytes(estimate.quota)}`
      );
    }

    add(
      'Ubicación segura (HTTPS)',
      window.isSecureContext ? 'ok' : 'fail',
      window.isSecureContext
        ? 'El micrófono puede usarse.'
        : 'Abre la app por HTTPS o localhost; si no, el navegador bloquea el micrófono.'
    );

    add(
      'Funcionamiento sin conexión',
      'serviceWorker' in navigator ? 'ok' : 'warn',
      'serviceWorker' in navigator
        ? navigator.serviceWorker.controller
          ? 'Instalado y activo: la app funciona sin internet.'
          : 'Soportado; se activará al recargar una vez.'
        : 'Sin soporte: la app necesita internet en cada visita.'
    );

    log.textContent = [
      `Navegador: ${navigator.userAgent}`,
      `Plataforma: ${navigator.platform}`,
      `Pantalla: ${window.screen.width}×${window.screen.height} @${window.devicePixelRatio}x`,
      `Contexto: ${ctx ? `${ctx.sampleRate} Hz, ${ctx.state}` : 'sin crear'}`,
      `Clips: ${this.project.clips.length} · Pistas: ${this.project.tracks.length}`,
      `Audio en memoria: ${this.app.buffers.size} buffers`,
      `Sonoridad del máster: ${Number.isFinite(this.engine?.lufsValues?.integrated) ? this.engine.lufsValues.integrated.toFixed(1) : '−∞'} LUFS`,
    ].join('\n');
  }

  async testEncoder() {
    const log = document.getElementById('diag-log');
    log.textContent = 'Probando codificador MP3…\n';
    try {
      const rate = 48000;
      const seconds = 1;
      const left = new Float32Array(rate * seconds);
      const right = new Float32Array(rate * seconds);
      for (let i = 0; i < left.length; i++) {
        left[i] = Math.sin((2 * Math.PI * 440 * i) / rate) * 0.5;
        right[i] = Math.sin((2 * Math.PI * 660 * i) / rate) * 0.5;
      }
      const started = performance.now();
      const blob = await encodeMp3([left, right], rate, { bitrate: 320, tags: { title: 'Prueba técnica' } });
      const elapsed = Math.round(performance.now() - started);
      const bytes = new Uint8Array(await blob.arrayBuffer());
      const hasId3 = bytes[0] === 0x49 && bytes[1] === 0x44 && bytes[2] === 0x33;
      let frames = 0;
      for (let i = 0; i < bytes.length - 3; i++) {
        if (bytes[i] === 0xff && (bytes[i + 1] & 0xe0) === 0xe0) frames++;
      }
      log.textContent += [
        `MP3 320 kbps: ${bytes.length} bytes en ${elapsed} ms`,
        `Etiqueta ID3: ${hasId3 ? 'sí' : 'no'}`,
        `Tramas detectadas: ${frames}`,
        bytes.length > 30000 ? 'Resultado: OK — el codificador funciona.' : 'Resultado: revisar, el archivo es demasiado pequeño.',
      ].join('\n');
      toast('Codificador MP3 verificado', 'ok');
    } catch (error) {
      log.textContent += `Error: ${error.message}`;
      toast(`El codificador falló: ${error.message}`, 'error');
    }
  }
}

export { formatTime };
