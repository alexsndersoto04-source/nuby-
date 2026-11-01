/**
 * Pruebas de la elección de entrada de audio.
 *
 * El eco y el ruido de las tomas venían de que la app abría el micrófono del
 * teléfono cuando no se elegía dispositivo. Estas pruebas fijan la regla:
 * con una tarjeta externa disponible, se usa esa.
 */

import { test } from 'node:test';
import assert from 'node:assert/strict';
import {
  rankInputDevices,
  pickInputDevice,
  isBuiltInLabel,
  isExternalLabel,
  describeInputDevice,
} from '../js/audio/input-devices.js';

const devices = [
  { kind: 'audioinput', deviceId: 'default', label: 'Micrófono predeterminado' },
  { kind: 'audioinput', deviceId: 'mic-telefono', label: 'Micrófono del teléfono' },
  { kind: 'audioinput', deviceId: 'v8-usb', label: 'V8' },
  { kind: 'audiooutput', deviceId: 'spk', label: 'Altavoz' },
];

test('detecta el micrófono del teléfono y las entradas externas', () => {
  assert.equal(isBuiltInLabel('Micrófono del teléfono'), true);
  assert.equal(isBuiltInLabel('Built-in Microphone'), true);
  assert.equal(isBuiltInLabel('V8'), false);
  assert.equal(isExternalLabel('V8'), true);
  assert.equal(isExternalLabel('USB Audio Device'), true);
  assert.equal(isExternalLabel('Micrófono del teléfono'), false);
});

test('la V8 se elige por delante del micrófono del teléfono', () => {
  const best = pickInputDevice(devices, '');
  assert.equal(best.deviceId, 'v8-usb', `se eligió ${best.deviceId}`);
  assert.equal(best.external, true);
  assert.equal(best.builtIn, false);
});

test('sin tarjeta externa se usa la entrada del sistema', () => {
  const onlyPhone = [
    { kind: 'audioinput', deviceId: 'default', label: 'Micrófono predeterminado' },
    { kind: 'audioinput', deviceId: 'mic-telefono', label: 'Micrófono del teléfono' },
  ];
  const best = pickInputDevice(onlyPhone, '');
  assert.ok(best, 'debe devolver algo');
  assert.notEqual(best.deviceId, 'default', 'no se elige el alias "predeterminado"');
  assert.equal(best.builtIn, true);
});

test('la última entrada usada se respeta si sigue disponible', () => {
  const best = pickInputDevice(devices, 'mic-telefono');
  assert.equal(best.deviceId, 'mic-telefono');
  assert.ok(best.score >= 900, 'la preferencia pesa más que ser externa');
});

test('solo se listan entradas de audio y el orden es estable', () => {
  const ranked = rankInputDevices(devices, '');
  assert.equal(ranked.length, 3);
  assert.ok(ranked.every((d) => d.deviceId !== 'spk'));
  assert.equal(ranked[0].deviceId, 'v8-usb');
});

test('describe la entrada con un aviso cuando es la del teléfono', () => {
  const ranked = rankInputDevices(devices, '');
  const phone = ranked.find((d) => d.deviceId === 'mic-telefono');
  const v8 = ranked.find((d) => d.deviceId === 'v8-usb');
  assert.match(describeInputDevice(phone), /teléfono/);
  assert.match(describeInputDevice(v8), /externa/);
});
