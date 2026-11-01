/**
 * Cadena de efectos por pista.
 *
 * La misma función construye la cadena en el AudioContext (escucha en vivo) y
 * en el OfflineAudioContext (mezcla final), con los mismos nodos y los mismos
 * parámetros. Por eso la mezcla exportada suena como lo que se escucha en el
 * estudio.
 *
 * Orden de la cadena (el de una consola real):
 *   entrada → puerta → paso alto → EQ 4 bandas → compresor → saturación
 *   → fader → panorama → mute/solo → salidas y envíos (reverb / delay)
 */

export const FX_DEFAULTS = () => ({
  gate: { on: false, threshold: -45, range: -60, attack: 0.003, hold: 0.05, release: 0.12 },
  hp: { on: true, freq: 75 },
  eq: {
    on: true,
    bands: [
      { type: 'lowshelf', freq: 120, gain: 0, q: 0.7 },
      { type: 'peaking', freq: 400, gain: 0, q: 1.0 },
      { type: 'peaking', freq: 3000, gain: 0, q: 1.0 },
      { type: 'highshelf', freq: 8000, gain: 0, q: 0.7 },
    ],
  },
  comp: { on: false, threshold: -20, ratio: 3, attack: 0.005, release: 0.18, knee: 6, makeup: 0 },
  sat: { on: false, drive: 0.25, mix: 0.5 },
  sendReverb: 0,
  sendDelay: 0,
});

const MAX_EQ_BANDS = 4;

/**
 * @param {BaseAudioContext} ctx
 * @param {object} fx configuración de efectos de la pista
 * @param {{worklets?: boolean, safe?: boolean}} [options] safe = sin suavizado (render offline)
 * @returns {{input: GainNode, output: GainNode, sends: {reverb: GainNode, delay: GainNode}, nodes: object, setFx: Function}}
 */
export function buildTrackChain(ctx, fx, options = {}) {
  const useWorklets = options.worklets !== false;
  const immediate = options.safe === true;
  const settings = normalizeFx(fx);

  const input = ctx.createGain();
  input.gain.value = 1;

  const nodes = {};

  // --- Puerta de ruido -----------------------------------------------------
  let gateNode = null;
  let gateFallback = null;
  if (useWorklets) {
    try {
      gateNode = new AudioWorkletNode(ctx, 'nuby-gate', {
        numberOfInputs: 1,
        numberOfOutputs: 1,
        outputChannelCount: [2],
        channelCount: 2,
        channelCountMode: 'explicit',
      });
      gateNode.parameters.get('threshold').value = settings.gate.threshold;
      gateNode.parameters.get('range').value = settings.gate.range;
      gateNode.parameters.get('attack').value = settings.gate.attack;
      gateNode.parameters.get('hold').value = settings.gate.hold;
      gateNode.parameters.get('release').value = settings.gate.release;
      nodes.gate = gateNode;
    } catch (error) {
      gateNode = null;
      nodes.gateError = String(error?.message ?? error);
    }
  } else {
    gateFallback = ctx.createGain();
  }

  // --- Filtro paso alto ----------------------------------------------------
  const hp = ctx.createBiquadFilter();
  hp.type = 'highpass';
  hp.frequency.value = settings.hp.freq;
  hp.Q.value = 0.707;
  nodes.hp = hp;

  // --- Ecualizador de 4 bandas --------------------------------------------
  const eqBands = [];
  for (let i = 0; i < MAX_EQ_BANDS; i++) {
    const biquad = ctx.createBiquadFilter();
    const band = settings.eq.bands[i] ?? { type: 'peaking', freq: 1000, gain: 0, q: 1 };
    biquad.type = band.type;
    biquad.frequency.value = band.freq;
    biquad.Q.value = band.q;
    biquad.gain.value = band.gain;
    eqBands.push(biquad);
  }
  nodes.eq = eqBands;

  // --- Compresor -----------------------------------------------------------
  const comp = ctx.createDynamicsCompressor();
  comp.threshold.value = settings.comp.threshold;
  comp.ratio.value = settings.comp.ratio;
  comp.attack.value = settings.comp.attack;
  comp.release.value = settings.comp.release;
  comp.knee.value = settings.comp.knee;
  const compMakeup = ctx.createGain();
  compMakeup.gain.value = dbToGain(settings.comp.makeup);
  nodes.comp = comp;
  nodes.compMakeup = compMakeup;

  // --- Saturación ----------------------------------------------------------
  const satIn = ctx.createGain();
  const shaper = ctx.createWaveShaper();
  shaper.curve = saturationCurve(settings.sat.drive);
  shaper.oversample = '4x';
  const satWet = ctx.createGain();
  const satDry = ctx.createGain();
  satWet.gain.value = settings.sat.mix;
  satDry.gain.value = 1 - settings.sat.mix;
  nodes.shaper = shaper;
  nodes.satWet = satWet;
  nodes.satDry = satDry;
  const satOut = ctx.createGain();

  // --- Fader, panorama, mute ----------------------------------------------
  const fader = ctx.createGain();
  fader.gain.value = settings.fader ?? 1;
  const panner = ctx.createStereoPanner();
  panner.pan.value = settings.pan ?? 0;
  const muteGain = ctx.createGain();
  muteGain.gain.value = settings.muted ? 0 : 1;

  // --- Envíos post-fader ---------------------------------------------------
  const sendReverb = ctx.createGain();
  sendReverb.gain.value = settings.sendReverb;
  const sendDelay = ctx.createGain();
  sendDelay.gain.value = settings.sendDelay;
  sendReverb.channelCount = 2;
  sendReverb.channelCountMode = 'explicit';
  sendDelay.channelCount = 2;
  sendDelay.channelCountMode = 'explicit';

  const output = muteGain;

  // --- Cableado ------------------------------------------------------------
  let current = input;

  const activeGate = settings.gate.on ? gateNode ?? gateFallback : null;
  if (activeGate) {
    current.connect(activeGate);
    current = activeGate;
  }

  if (settings.hp.on) {
    current.connect(hp);
    current = hp;
  }

  if (settings.eq.on) {
    for (const band of eqBands) {
      current.connect(band);
      current = band;
    }
  }

  if (settings.comp.on) {
    current.connect(comp);
    comp.connect(compMakeup);
    current = compMakeup;
  }

  if (settings.sat.on) {
    // División en seco/húmedo para que la saturación tenga mezcla.
    current.connect(satIn);
    satIn.connect(shaper);
    shaper.connect(satWet);
    satIn.connect(satDry);
    satWet.connect(satOut);
    satDry.connect(satOut);
    current = satOut;
  }

  current.connect(fader);
  fader.connect(panner);
  panner.connect(muteGain);
  muteGain.connect(sendReverb);
  muteGain.connect(sendDelay);

  /** Actualiza todos los parámetros sin recablear (evita cortes y chasquidos). */
  function setFx(nextFx, extra = {}) {
    const next = normalizeFx({ ...nextFx, ...extra });
    const set = (param, value) => setParam(param, value, ctx, immediate);

    if (gateNode) {
      set(gateNode.parameters.get('threshold'), next.gate.threshold);
      set(gateNode.parameters.get('range'), next.gate.range);
      set(gateNode.parameters.get('attack'), next.gate.attack);
      set(gateNode.parameters.get('hold'), next.gate.hold);
      set(gateNode.parameters.get('release'), next.gate.release);
    }
    set(hp.frequency, next.hp.freq);
    next.eq.bands.forEach((band, index) => {
      const biquad = eqBands[index];
      if (!biquad) return;
      set(biquad.frequency, band.freq);
      set(biquad.gain, band.gain);
      set(biquad.Q, band.q);
    });
    set(comp.threshold, next.comp.threshold);
    set(comp.ratio, next.comp.ratio);
    set(comp.attack, next.comp.attack);
    set(comp.release, next.comp.release);
    set(comp.knee, next.comp.knee);
    set(compMakeup.gain, dbToGain(next.comp.makeup));
    set(satWet.gain, next.sat.mix);
    set(satDry.gain, 1 - next.sat.mix);
    if (shaper.curve?.length !== 1024 || next.sat.drive !== settings.sat.drive) {
      shaper.curve = saturationCurve(next.sat.drive);
    }
    set(fader.gain, next.fader);
    set(panner.pan, next.pan);
    set(muteGain.gain, next.muted ? 0 : 1);
    set(sendReverb.gain, next.sendReverb);
    set(sendDelay.gain, next.sendDelay);
  }

  return { input, output, sends: { reverb: sendReverb, delay: sendDelay }, nodes, setFx, fader, panner, muteGain };
}

/**
 * Cadena maestra: EQ → compresor de bus → limitador → salida.
 * @param {BaseAudioContext} ctx
 * @param {object} master
 * @param {{worklets?: boolean, safe?: boolean}} [options]
 */
export function buildMasterChain(ctx, master, options = {}) {
  const settings = normalizeMaster(master);
  const immediate = options.safe === true;

  const input = ctx.createGain();

  const low = ctx.createBiquadFilter();
  low.type = 'lowshelf';
  low.frequency.value = 110;
  low.gain.value = settings.eq.low;

  const mid = ctx.createBiquadFilter();
  mid.type = 'peaking';
  mid.frequency.value = 1000;
  mid.Q.value = 0.8;
  mid.gain.value = settings.eq.mid;

  const high = ctx.createBiquadFilter();
  high.type = 'highshelf';
  high.frequency.value = 7000;
  high.gain.value = settings.eq.high;

  const comp = ctx.createDynamicsCompressor();
  comp.threshold.value = settings.comp.threshold;
  comp.ratio.value = settings.comp.ratio;
  comp.attack.value = settings.comp.attack;
  comp.release.value = settings.comp.release;
  comp.knee.value = settings.comp.knee;

  let limiter = null;
  if (settings.limiter.on && options.worklets !== false) {
    try {
      limiter = new AudioWorkletNode(ctx, 'nuby-limiter', {
        numberOfInputs: 1,
        numberOfOutputs: 1,
        outputChannelCount: [2],
        channelCount: 2,
        channelCountMode: 'explicit',
      });
      limiter.parameters.get('ceiling').value = settings.limiter.ceiling;
      limiter.parameters.get('release').value = settings.limiter.release;
      limiter.parameters.get('lookahead').value = settings.limiter.lookahead;
    } catch {
      limiter = null;
    }
  }

  let softLimiter = null;
  if (settings.limiter.on && !limiter) {
    // Respaldo sin AudioWorklet: saturador suave que nunca pasa del techo.
    softLimiter = ctx.createWaveShaper();
    softLimiter.curve = softLimitCurve(Math.pow(10, settings.limiter.ceiling / 20));
    softLimiter.oversample = '4x';
  }

  const output = ctx.createGain();
  output.gain.value = settings.volume;

  let current = input;
  const connect = (node) => {
    current.connect(node);
    current = node;
  };
  connect(low);
  connect(mid);
  connect(high);
  if (settings.comp.on) connect(comp);
  if (limiter) connect(limiter);
  if (softLimiter) connect(softLimiter);
  current.connect(output);

  function setMaster(nextMaster) {
    const next = normalizeMaster(nextMaster);
    const set = (param, value) => setParam(param, value, ctx, immediate);
    set(low.gain, next.eq.low);
    set(mid.gain, next.eq.mid);
    set(high.gain, next.eq.high);
    set(comp.threshold, next.comp.threshold);
    set(comp.ratio, next.comp.ratio);
    set(comp.attack, next.comp.attack);
    set(comp.release, next.comp.release);
    set(comp.knee, next.comp.knee);
    set(output.gain, next.volume);
    if (limiter) {
      set(limiter.parameters.get('ceiling'), next.limiter.ceiling);
      set(limiter.parameters.get('release'), next.limiter.release);
      set(limiter.parameters.get('lookahead'), next.limiter.lookahead);
    }
  }

  return { input, output, nodes: { low, mid, high, comp, limiter, softLimiter }, setMaster, limiter };
}

/** Curva de limitación suave que nunca supera `ceiling` (respaldo del limitador). */
export function softLimitCurve(ceiling, samples = 4096) {
  const curve = new Float32Array(samples);
  const limit = Math.max(0.01, Math.min(1, ceiling));
  for (let i = 0; i < samples; i++) {
    const x = (i * 2) / (samples - 1) - 1;
    // Zona lineal hasta ~70 % del techo y compresión suave por encima.
    const knee = limit * 0.7;
    const abs = Math.abs(x);
    let y;
    if (abs <= knee) y = abs;
    else {
      const over = abs - knee;
      const room = limit - knee;
      y = knee + room * Math.tanh(over / Math.max(room * 0.6, 1e-4));
    }
    curve[i] = (x < 0 ? -1 : 1) * Math.min(limit, y);
  }
  return curve;
}

/** Curvas de saturación suave (tanh aproximada) con Drive 0..1. */
export function saturationCurve(drive, samples = 1024) {
  const curve = new Float32Array(samples);
  const amount = 1 + Math.max(0, Math.min(1, drive)) * 24;
  for (let i = 0; i < samples; i++) {
    const x = (i * 2) / (samples - 1) - 1;
    curve[i] = Math.tanh(x * amount) / Math.tanh(amount);
  }
  return curve;
}

/** Completa una configuración de efectos con los valores por defecto. */
export function normalizeFx(fx = {}) {
  const defaults = FX_DEFAULTS();
  const bands = defaults.eq.bands.map((band, index) => ({
    ...band,
    ...(fx.eq?.bands?.[index] ?? {}),
  }));
  return {
    gate: { ...defaults.gate, ...(fx.gate ?? {}) },
    hp: { ...defaults.hp, ...(fx.hp ?? {}) },
    eq: { ...defaults.eq, ...(fx.eq ?? {}), bands },
    comp: { ...defaults.comp, ...(fx.comp ?? {}) },
    sat: { ...defaults.sat, ...(fx.sat ?? {}) },
    sendReverb: fx.sendReverb ?? defaults.sendReverb,
    sendDelay: fx.sendDelay ?? defaults.sendDelay,
    fader: fx.fader ?? 1,
    pan: fx.pan ?? 0,
    muted: fx.muted ?? false,
  };
}

export const MASTER_DEFAULTS = () => ({
  volume: 1,
  eq: { low: 0, mid: 0, high: 0 },
  comp: { on: false, threshold: -18, ratio: 3, attack: 0.01, release: 0.25, knee: 6 },
  limiter: { on: true, ceiling: -1, release: 0.08, lookahead: 0.005 },
});

export function normalizeMaster(master = {}) {
  const defaults = MASTER_DEFAULTS();
  return {
    volume: master.volume ?? defaults.volume,
    eq: { ...defaults.eq, ...(master.eq ?? {}) },
    comp: { ...defaults.comp, ...(master.comp ?? {}) },
    limiter: { ...defaults.limiter, ...(master.limiter ?? {}) },
  };
}

function dbToGain(db) {
  return db <= -96 ? 0 : Math.pow(10, db / 20);
}

function setParam(param, value, ctx, immediate) {
  if (!param || !Number.isFinite(value)) return;
  if (immediate || !ctx || ctx.state === 'closed') {
    param.value = value;
    return;
  }
  const now = ctx.currentTime;
  param.cancelScheduledValues(now);
  param.setTargetAtTime(value, now, 0.015);
}
