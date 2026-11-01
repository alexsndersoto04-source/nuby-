/**
 * Preferencias del estudio: lo que se configura una vez y vale para todas las
 * sesiones (entrada, grabación, exportación, interfaz).
 *
 * Se guardan en `localStorage` y se aplican al arrancar. Cada preferencia tiene
 * un efecto real: ninguna es decorativa.
 */

const STORAGE_KEY = 'nuby:settings';

/** Valores de fábrica. */
export const SETTINGS_DEFAULTS = Object.freeze({
  // Captura
  sampleRate: 48000,
  latencyHint: 'interactive',
  channelMode: 'mono-left',
  inputGain: 1,
  monitor: false,
  autoGainTakes: true,
  // Grabación
  countInBeats: 4,
  metronomeOn: true,
  metronomeVolume: 0.55,
  autoScroll: true,
  autoSave: true,
  keepScreenOn: true,
  // Sesión nueva
  bpm: 120,
  timeSig: '4/4',
  // Exportación
  exportFormat: 'mp3',
  exportBitrate: 320,
  exportDepth: 24,
  exportNormalize: false,
  exportMaster: true,
  // Interfaz
  density: 'comoda',
});

/**
 * Definición de la pantalla de configuración. La interfaz se genera a partir de
 * esta lista, así que añadir una opción es añadir una entrada aquí.
 */
export const SETTINGS_SECTIONS = [
  {
    id: 'captura',
    title: 'Entrada y captura',
    hint: 'Cómo se abre el micrófono (BM800 + V8) y qué se hace con la señal.',
    items: [
      {
        key: 'channelMode',
        label: 'Canal de la entrada',
        hint: 'La V8 entrega la voz en el canal izquierdo cuando se usa en mono.',
        type: 'select',
        options: [
          { value: 'mono-left', label: 'Mono · canal izquierdo' },
          { value: 'mono-right', label: 'Mono · canal derecho' },
          { value: 'mono-mix', label: 'Mono · mezcla L+R' },
          { value: 'stereo', label: 'Estéreo' },
        ],
      },
      {
        key: 'inputGain',
        label: 'Ganancia de entrada',
        hint: 'La V8 suele entregar señal baja: +12 dB (4×) es normal. 1× es neutro.',
        type: 'number',
        min: 0.1,
        max: 8,
        step: 0.05,
        unit: '×',
      },
      {
        key: 'autoGainTakes',
        label: 'Nivelar las tomas al grabar',
        hint: 'Si la toma llega baja, se le aplica ganancia hasta dejarla audible (sin comprimir ni recortar).',
        type: 'toggle',
      },
      {
        key: 'monitor',
        label: 'Escucha directa al abrir el micrófono',
        hint: 'Oír la entrada en vivo. Usa auriculares para evitar acople.',
        type: 'toggle',
      },
      {
        key: 'sampleRate',
        label: 'Frecuencia de muestreo',
        hint: '48 kHz es lo estándar para vídeo y música. Se aplica al recargar.',
        type: 'select',
        options: [
          { value: 48000, label: '48 000 Hz — recomendado' },
          { value: 44100, label: '44 100 Hz — CD' },
        ],
      },
      {
        key: 'latencyHint',
        label: 'Modo de latencia del motor',
        hint: 'Interactivo prioriza respuesta; reproducción prioriza estabilidad.',
        type: 'select',
        options: [
          { value: 'interactive', label: 'Interactivo — grabación' },
          { value: 'balanced', label: 'Equilibrado' },
          { value: 'playback', label: 'Reproducción — mezcla estable' },
        ],
      },
    ],
  },
  {
    id: 'grabacion',
    title: 'Grabación',
    hint: 'Comportamiento al pulsar grabar.',
    items: [
      {
        key: 'countInBeats',
        label: 'Cuenta de entrada',
        hint: 'Pulsos de metrónomo antes de empezar a grabar.',
        type: 'select',
        options: [
          { value: 0, label: 'Sin cuenta' },
          { value: 4, label: '1 compás (4 pulsos)' },
          { value: 8, label: '2 compases (8 pulsos)' },
        ],
      },
      {
        key: 'autoScroll',
        label: 'Seguir el cabezal al reproducir',
        type: 'toggle',
      },
      {
        key: 'autoSave',
        label: 'Guardar automáticamente',
        hint: 'Guarda el proyecto en el teléfono unos segundos después de cada cambio.',
        type: 'toggle',
      },
      {
        key: 'keepScreenOn',
        label: 'Mantener la pantalla encendida',
        hint: 'Solo en la app Android.',
        type: 'toggle',
      },
    ],
  },
  {
    id: 'metronomo',
    title: 'Metrónomo y sesión nueva',
    hint: 'Valores con los que empieza un proyecto nuevo.',
    items: [
      {
        key: 'metronomeOn',
        label: 'Metrónomo activado al empezar',
        type: 'toggle',
      },
      {
        key: 'metronomeVolume',
        label: 'Volumen del metrónomo',
        type: 'number',
        min: 0,
        max: 1,
        step: 0.05,
        unit: '',
      },
      {
        key: 'bpm',
        label: 'Tempo por defecto',
        type: 'number',
        min: 20,
        max: 300,
        step: 1,
        unit: 'BPM',
      },
      {
        key: 'timeSig',
        label: 'Compás por defecto',
        type: 'select',
        options: [
          { value: '4/4', label: '4/4' },
          { value: '3/4', label: '3/4' },
          { value: '6/8', label: '6/8' },
          { value: '2/4', label: '2/4' },
        ],
      },
    ],
  },
  {
    id: 'exportacion',
    title: 'Exportación',
    hint: 'Lo que aparece ya elegido al abrir el diálogo de exportar.',
    items: [
      {
        key: 'exportFormat',
        label: 'Formato',
        type: 'select',
        options: [
          { value: 'mp3', label: 'MP3 — para compartir' },
          { value: 'wav', label: 'WAV — sin pérdida' },
        ],
      },
      {
        key: 'exportBitrate',
        label: 'Calidad MP3',
        type: 'select',
        options: [
          { value: 320, label: '320 kbps' },
          { value: 256, label: '256 kbps' },
          { value: 192, label: '192 kbps' },
          { value: 128, label: '128 kbps' },
        ],
      },
      {
        key: 'exportDepth',
        label: 'Profundidad WAV',
        type: 'select',
        options: [
          { value: 24, label: '24 bits' },
          { value: 16, label: '16 bits' },
          { value: 32, label: '32 bits float' },
        ],
      },
      {
        key: 'exportNormalize',
        label: 'Normalizar a −1 dBFS por defecto',
        type: 'toggle',
      },
      {
        key: 'exportMaster',
        label: 'Aplicar el máster por defecto',
        type: 'toggle',
      },
    ],
  },
  {
    id: 'interfaz',
    title: 'Interfaz',
    hint: 'Cómo se ve el estudio en esta pantalla.',
    items: [
      {
        key: 'density',
        label: 'Densidad',
        hint: 'Compacta muestra más controles; cómoda deja más aire para el dedo.',
        type: 'select',
        options: [
          { value: 'comoda', label: 'Cómoda' },
          { value: 'compacta', label: 'Compacta' },
        ],
      },
    ],
  },
];

let cache = null;
const listeners = new Set();

function read() {
  if (cache) return cache;
  const values = { ...SETTINGS_DEFAULTS };
  try {
    const raw = localStorage.getItem(STORAGE_KEY);
    if (raw) {
      const parsed = JSON.parse(raw);
      for (const [key, value] of Object.entries(parsed ?? {})) {
        if (key in SETTINGS_DEFAULTS && value !== null && value !== undefined) {
          values[key] = typeof SETTINGS_DEFAULTS[key] === 'number' ? Number(value) : value;
        }
      }
    }
  } catch (error) {
    console.warn('No se pudieron leer las preferencias', error);
  }
  cache = values;
  return cache;
}

/** Todas las preferencias actuales. */
export function getSettings() {
  return { ...read() };
}

/** Una preferencia concreta. */
export function getSetting(key) {
  return read()[key];
}

/**
 * Cambia una preferencia, la guarda y avisa a quien escuche.
 * @returns {object} preferencias completas actualizadas
 */
export function updateSetting(key, value) {
  if (!(key in SETTINGS_DEFAULTS)) throw new Error(`Preferencia desconocida: ${key}`);
  const values = { ...read(), [key]: value };
  cache = values;
  try {
    localStorage.setItem(STORAGE_KEY, JSON.stringify(values));
  } catch (error) {
    console.warn('No se pudieron guardar las preferencias', error);
  }
  for (const listener of listeners) {
    try {
      listener(key, value, values);
    } catch (error) {
      console.error(error);
    }
  }
  return { ...values };
}

/** Vuelve a los valores de fábrica. */
export function resetSettings() {
  cache = { ...SETTINGS_DEFAULTS };
  try {
    localStorage.removeItem(STORAGE_KEY);
  } catch {
    // sin almacenamiento disponible: la sesión sigue con los valores de fábrica
  }
  for (const listener of listeners) {
    try {
      listener('*', null, cache);
    } catch (error) {
      console.error(error);
    }
  }
  return { ...cache };
}

/** Escucha cambios (devuelve la función para dejar de escuchar). */
export function onSettingsChange(listener) {
  listeners.add(listener);
  return () => listeners.delete(listener);
}

/** Compás guardado como "4/4" → [4, 4]. */
export function parseTimeSig(value) {
  const parts = String(value ?? '4/4').split('/');
  const beats = Number(parts[0]) || 4;
  const unit = Number(parts[1]) || 4;
  return [beats, unit];
}
