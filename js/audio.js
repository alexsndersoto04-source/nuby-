// ============================================================
//  audio.js — Sonido atmosférico generado con Web Audio API
//  No requiere archivos externos: todo se sintetiza en tiempo real.
// ============================================================

const Audio = (() => {
  let ctx = null;
  let masterGain = null;
  let ambienceNodes = [];
  let started = false;

  function init() {
    if (started) return;
    started = true;
    const AC = window.AudioContext || window.webkitAudioContext;
    ctx = new AC();
    masterGain = ctx.createGain();
    masterGain.gain.value = 0.6;
    masterGain.connect(ctx.destination);

    startAmbience();
  }

  // Paisaje sonoro de fondo: viento bajo + zumbido industrial lejano
  function startAmbience() {
    // Viento: ruido filtrado
    const bufferSize = 2 * ctx.sampleRate;
    const noiseBuffer = ctx.createBuffer(1, bufferSize, ctx.sampleRate);
    const output = noiseBuffer.getChannelData(0);
    for (let i = 0; i < bufferSize; i++) output[i] = Math.random() * 2 - 1;

    const wind = ctx.createBufferSource();
    wind.buffer = noiseBuffer;
    wind.loop = true;
    const windFilter = ctx.createBiquadFilter();
    windFilter.type = "lowpass";
    windFilter.frequency.value = 380;
    const windGain = ctx.createGain();
    windGain.gain.value = 0.05;

    // LFO para que el viento respire
    const lfo = ctx.createOscillator();
    lfo.frequency.value = 0.08;
    const lfoGain = ctx.createGain();
    lfoGain.gain.value = 0.04;
    lfo.connect(lfoGain);
    lfoGain.connect(windGain.gain);
    lfo.start();

    wind.connect(windFilter);
    windFilter.connect(windGain);
    windGain.connect(masterGain);
    wind.start();
    ambienceNodes.push(wind, lfo);

    // Zumbido industrial grave
    const drone = ctx.createOscillator();
    drone.type = "sine";
    drone.frequency.value = 55;
    const droneGain = ctx.createGain();
    droneGain.gain.value = 0.035;
    const droneFilter = ctx.createBiquadFilter();
    droneFilter.type = "lowpass";
    droneFilter.frequency.value = 120;
    drone.connect(droneFilter);
    droneFilter.connect(droneGain);
    droneGain.connect(masterGain);
    drone.start();
    ambienceNodes.push(drone);
  }

  function resume() {
    if (ctx && ctx.state === "suspended") ctx.resume();
  }

  // Un paso (golpe seco y apagado)
  function footstep() {
    if (!ctx) return;
    const t = ctx.currentTime;
    const noise = ctx.createBufferSource();
    const buf = ctx.createBuffer(1, ctx.sampleRate * 0.1, ctx.sampleRate);
    const data = buf.getChannelData(0);
    for (let i = 0; i < data.length; i++) data[i] = (Math.random() * 2 - 1) * (1 - i / data.length);
    noise.buffer = buf;
    const filter = ctx.createBiquadFilter();
    filter.type = "lowpass";
    filter.frequency.value = 220;
    const gain = ctx.createGain();
    gain.gain.setValueAtTime(0.18, t);
    gain.gain.exponentialRampToValueAtTime(0.001, t + 0.09);
    noise.connect(filter); filter.connect(gain); gain.connect(masterGain);
    noise.start(t);
    noise.stop(t + 0.1);
  }

  // Salto: barrido ascendente suave
  function jump() {
    if (!ctx) return;
    const t = ctx.currentTime;
    const osc = ctx.createOscillator();
    osc.type = "sine";
    osc.frequency.setValueAtTime(180, t);
    osc.frequency.exponentialRampToValueAtTime(420, t + 0.18);
    const gain = ctx.createGain();
    gain.gain.setValueAtTime(0.12, t);
    gain.gain.exponentialRampToValueAtTime(0.001, t + 0.25);
    osc.connect(gain); gain.connect(masterGain);
    osc.start(t); osc.stop(t + 0.26);
  }

  // Aterrizaje
  function land() {
    if (!ctx) return;
    const t = ctx.currentTime;
    const noise = ctx.createBufferSource();
    const buf = ctx.createBuffer(1, ctx.sampleRate * 0.15, ctx.sampleRate);
    const data = buf.getChannelData(0);
    for (let i = 0; i < data.length; i++) data[i] = (Math.random() * 2 - 1) * (1 - i / data.length);
    noise.buffer = buf;
    const filter = ctx.createBiquadFilter();
    filter.type = "lowpass";
    filter.frequency.value = 160;
    const gain = ctx.createGain();
    gain.gain.setValueAtTime(0.2, t);
    gain.gain.exponentialRampToValueAtTime(0.001, t + 0.14);
    noise.connect(filter); filter.connect(gain); gain.connect(masterGain);
    noise.start(t); noise.stop(t + 0.15);
  }

  // Dron: zumbido eléctrico agudo que sube al detectarte
  function droneAlert(intensity) {
    if (!ctx) return;
    const t = ctx.currentTime;
    const osc = ctx.createOscillator();
    osc.type = "sawtooth";
    osc.frequency.setValueAtTime(140 + intensity * 200, t);
    const filter = ctx.createBiquadFilter();
    filter.type = "bandpass";
    filter.frequency.value = 600;
    const gain = ctx.createGain();
    gain.gain.setValueAtTime(0.0001, t);
    gain.gain.exponentialRampToValueAtTime(0.08, t + 0.05);
    gain.gain.exponentialRampToValueAtTime(0.0001, t + 0.5);
    osc.connect(filter); filter.connect(gain); gain.connect(masterGain);
    osc.start(t); osc.stop(t + 0.55);
  }

  // Muerte: ruido blanco + tono grave descendente
  function death() {
    if (!ctx) return;
    const t = ctx.currentTime;
    const osc = ctx.createOscillator();
    osc.type = "sawtooth";
    osc.frequency.setValueAtTime(220, t);
    osc.frequency.exponentialRampToValueAtTime(40, t + 1.2);
    const gain = ctx.createGain();
    gain.gain.setValueAtTime(0.25, t);
    gain.gain.exponentialRampToValueAtTime(0.001, t + 1.3);
    const filter = ctx.createBiquadFilter();
    filter.type = "lowpass";
    filter.frequency.value = 800;
    osc.connect(filter); filter.connect(gain); gain.connect(masterGain);
    osc.start(t); osc.stop(t + 1.4);

    const noise = ctx.createBufferSource();
    const buf = ctx.createBuffer(1, ctx.sampleRate * 0.5, ctx.sampleRate);
    const data = buf.getChannelData(0);
    for (let i = 0; i < data.length; i++) data[i] = (Math.random() * 2 - 1) * (1 - i / data.length);
    noise.buffer = buf;
    const ng = ctx.createGain();
    ng.gain.setValueAtTime(0.3, t);
    ng.gain.exponentialRampToValueAtTime(0.001, t + 0.5);
    noise.connect(ng); ng.connect(masterGain);
    noise.start(t); noise.stop(t + 0.5);
  }

  // Checkpoint: dos notas suaves y cristalinas
  function checkpoint() {
    if (!ctx) return;
    const notes = [523.25, 783.99];
    notes.forEach((freq, i) => {
      const t = ctx.currentTime + i * 0.12;
      const osc = ctx.createOscillator();
      osc.type = "triangle";
      osc.frequency.value = freq;
      const gain = ctx.createGain();
      gain.gain.setValueAtTime(0.0001, t);
      gain.gain.exponentialRampToValueAtTime(0.12, t + 0.03);
      gain.gain.exponentialRampToValueAtTime(0.0001, t + 0.5);
      osc.connect(gain); gain.connect(masterGain);
      osc.start(t); osc.stop(t + 0.55);
    });
  }

  // Victoria: notas esperanzadoras
  function win() {
    if (!ctx) return;
    const notes = [392, 523.25, 659.25, 783.99];
    notes.forEach((freq, i) => {
      const t = ctx.currentTime + i * 0.25;
      const osc = ctx.createOscillator();
      osc.type = "sine";
      osc.frequency.value = freq;
      const gain = ctx.createGain();
      gain.gain.setValueAtTime(0.0001, t);
      gain.gain.exponentialRampToValueAtTime(0.15, t + 0.05);
      gain.gain.exponentialRampToValueAtTime(0.0001, t + 0.8);
      osc.connect(gain); gain.connect(masterGain);
      osc.start(t); osc.stop(t + 0.9);
    });
  }

  // Silenciar/activar todo el audio
  let muted = false;
  function setMuted(m) {
    muted = m;
    if (masterGain) masterGain.gain.value = m ? 0 : 0.6;
  }
  function isMuted() { return muted; }

  return { init, resume, footstep, jump, land, droneAlert, death, win, checkpoint, setMuted, isMuted };
})();
