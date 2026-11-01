/**
 * Elección de la entrada de audio.
 *
 * Android abre por defecto el micrófono del teléfono. Si el usuario tiene una
 * tarjeta conectada (V8 y compañía) y la app abre el micrófono interno, la toma
 * suena con eco (el altavoz entra por el micro) y con ruido, aunque el hardware
 * esté perfecto. Este módulo decide qué dispositivo abrir y avisa cuando la
 * única entrada disponible es la del teléfono.
 */

/** Entradas que casi seguro son una tarjeta/interfaz externa. */
const EXTERNAL_RE =
  /(v8|usb|interfaz|interface|tarjeta|audio[\s_-]?interface|line[\s_-]?in|xlr|behringer|focusrite|yamaha|roland|zoom|shure|rode|samson|steinberg|presonus|motu|audient|universal audio|scarlett|ur\d|ag0\d|headset|diadema|lavalier)/i;

/** Entradas integradas en el teléfono (origen del eco). */
const BUILT_IN_RE =
  /(built[\s_-]?in|interno|interna|integrado|integrada|internal|tel[eé]fono|phone|handset|altavoz|speaker|auricular del|default|predeterminad)/i;

export function isBuiltInLabel(label = '') {
  return BUILT_IN_RE.test(String(label));
}

export function isExternalLabel(label = '') {
  const text = String(label);
  return EXTERNAL_RE.test(text) && !BUILT_IN_RE.test(text);
}

/** Puntuación: más alto = mejor candidato. */
export function scoreInputDevice(device, preferredId = '') {
  const label = device?.label ?? '';
  let score = 0;
  if (preferredId && device?.deviceId === preferredId) score += 1000;
  if (isExternalLabel(label)) score += 120;
  if (isBuiltInLabel(label)) score -= 60;
  if (!label) score -= 5;
  return score;
}

/**
 * Ordena las entradas de mejor a peor.
 * @param {MediaDeviceInfo[]} devices
 * @param {string} [preferredId]
 */
export function rankInputDevices(devices = [], preferredId = '') {
  return devices
    .filter((device) => (device.kind ? device.kind === 'audioinput' : true))
    .map((device, index) => ({
      deviceId: device.deviceId ?? '',
      label: device.label || `Entrada ${index + 1}`,
      score: scoreInputDevice(device, preferredId),
      builtIn: isBuiltInLabel(device.label),
      external: isExternalLabel(device.label),
      index,
    }))
    .sort((a, b) => b.score - a.score || a.index - b.index);
}

/**
 * Devuelve la entrada recomendada, o null si no hay ninguna utilizable.
 * @param {MediaDeviceInfo[]} devices
 * @param {string} [preferredId] última entrada usada (se respeta si sigue existiendo)
 */
export function pickInputDevice(devices = [], preferredId = '') {
  const ranked = rankInputDevices(devices, preferredId);
  const usable = ranked.filter((device) => device.deviceId && device.deviceId !== 'default');
  if (!usable.length) return ranked[0] ?? null;
  return usable[0];
}

/**
 * Texto corto para la interfaz: nombre + si es del teléfono.
 */
export function describeInputDevice(device) {
  if (!device) return 'Sin entrada';
  const name = device.label || 'Entrada';
  if (device.builtIn) return `${name} (del teléfono)`;
  if (device.external) return `${name} (externa)`;
  return name;
}
