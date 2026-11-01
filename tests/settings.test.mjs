/**
 * Pruebas de la configuración del estudio: valores por defecto, guardado en el
 * equipo y efecto real sobre los proyectos nuevos.
 */
import test from 'node:test';
import assert from 'node:assert/strict';
import { setupEnvironment } from './helpers/env.mjs';

const env = setupEnvironment();
const { window } = env;
window.localStorage.clear();

const settings = await import('../js/state/settings.js');
const { createProject } = await import('../js/state.js');

test('la configuración arranca con valores de fábrica', () => {
  const values = settings.getSettings();
  assert.equal(values.sampleRate, 48000);
  assert.equal(values.countInBeats, 4);
  assert.equal(values.exportFormat, 'mp3');
  assert.equal(values.density, 'comoda');
});

test('un proyecto nuevo hereda la configuración de sesión', () => {
  settings.updateSetting('bpm', 90);
  settings.updateSetting('timeSig', '3/4');
  settings.updateSetting('countInBeats', 8);
  settings.updateSetting('metronomeVolume', 0.3);

  const project = createProject('Prueba');
  assert.equal(project.session.bpm, 90);
  assert.deepEqual(project.session.timeSig, [3, 4]);
  assert.equal(project.session.countInBeats, 8);
  assert.equal(project.session.metronome.volume, 0.3);
});

test('los cambios se guardan en el equipo y avisan a quien escucha', () => {
  const seen = [];
  const off = settings.onSettingsChange((key, value) => seen.push([key, value]));
  settings.updateSetting('exportBitrate', 256);
  off();
  settings.updateSetting('exportBitrate', 320); // ya no debe avisar

  assert.deepEqual(seen, [['exportBitrate', 256]]);
  const stored = JSON.parse(window.localStorage.getItem('nuby:settings'));
  assert.equal(stored.exportBitrate, 320);
  assert.equal(settings.getSetting('exportBitrate'), 320);
});

test('una preferencia desconocida se rechaza en lugar de guardarse en silencio', () => {
  assert.throws(() => settings.updateSetting('inventada', 1), /Preferencia desconocida/);
});

test('restaurar devuelve los valores de fábrica y borra lo guardado', () => {
  settings.updateSetting('density', 'compacta');
  const values = settings.resetSettings();
  assert.equal(values.density, 'comoda');
  assert.equal(window.localStorage.getItem('nuby:settings'), null);
  assert.equal(createProject().session.bpm, 120);
});

test('la pantalla de configuración se genera con todas las secciones', async () => {
  await import('../js/main.js');
  const app = window.nuby;
  await app.bootPromise;
  window.flushFrames(1);

  app.dialogs.openSettings();
  const container = window.document.getElementById('settings-sections');
  const sections = container.querySelectorAll('.settings-section');
  // Una sección por bloque de opciones + la de estado del sistema.
  assert.equal(sections.length, settings.SETTINGS_SECTIONS.length + 1);

  const rows = container.querySelectorAll('.settings-row');
  const expected = settings.SETTINGS_SECTIONS.reduce((total, section) => total + section.items.length, 0);
  assert.ok(rows.length >= expected, 'faltan controles en la pantalla de configuración');

  const selects = container.querySelectorAll('select');
  assert.ok(selects.length > 0);
});
