/**
 * Grabador PCM dentro del hilo de audio.
 *
 * Graba muestras exactas (sin compresión, sin pérdida y sin la latencia variable
 * de MediaRecorder) comenzando en un fotograma concreto del reloj de audio, que
 * es lo que permite que varias tomas queden alineadas al sample.
 */
class RecorderProcessor extends AudioWorkletProcessor {
  constructor() {
    super();
    this.recording = false;
    this.startFrameTarget = -1;
    this.actualStartFrame = -1;
    this.channels = 1;
    this.pending = []; // bloques pendientes: { channel: Float32Array[] }
    this.pendingFrames = 0;
    this.captured = 0;
    this.flushEvery = 16384;

    this.port.onmessage = (event) => {
      const data = event.data || {};
      if (data.type === 'start') {
        this.recording = true;
        this.startFrameTarget = Math.max(0, Math.round(data.startFrame ?? currentFrame));
        this.actualStartFrame = -1;
        this.pending = [];
        this.pendingFrames = 0;
        this.captured = 0;
        this.channels = data.channels === 2 ? 2 : 1;
        this.port.postMessage({ type: 'started', targetFrame: this.startFrameTarget });
      } else if (data.type === 'stop') {
        if (this.recording) {
          this.flush();
          this.port.postMessage({
            type: 'done',
            frames: this.captured,
            startFrame: this.actualStartFrame,
            channels: this.channels,
          });
        }
        this.recording = false;
      } else if (data.type === 'cancel') {
        this.recording = false;
        this.pending = [];
        this.pendingFrames = 0;
        this.captured = 0;
      }
    };
  }

  process(inputs) {
    if (!this.recording) return true;

    const input = inputs[0];
    const frames = input && input[0] ? input[0].length : 128;
    let skip = 0;

    if (this.actualStartFrame < 0) {
      if (currentFrame + frames <= this.startFrameTarget) return true;
      this.actualStartFrame = Math.max(currentFrame, this.startFrameTarget);
      skip = this.actualStartFrame - currentFrame;
    }

    const count = frames - skip;
    if (count <= 0) return true;

    const block = new Array(this.channels);
    for (let c = 0; c < this.channels; c++) {
      const source = input[Math.min(c, Math.max(0, input.length - 1))];
      const target = new Float32Array(count);
      if (source && source.length >= skip + count) {
        target.set(source.subarray(skip, skip + count));
      }
      block[c] = target;
    }

    this.pending.push(block);
    this.pendingFrames += count;
    this.captured += count;

    if (this.pendingFrames >= this.flushEvery) this.flush();
    return true;
  }

  flush() {
    if (!this.pendingFrames) return;
    const total = this.pendingFrames;
    const merged = [];
    for (let c = 0; c < this.channels; c++) merged.push(new Float32Array(total));

    let offset = 0;
    for (const block of this.pending) {
      const length = block[0].length;
      for (let c = 0; c < this.channels; c++) merged[c].set(block[c], offset);
      offset += length;
    }

    this.pending = [];
    this.pendingFrames = 0;

    this.port.postMessage({ type: 'chunk', channels: merged, frames: total }, merged.map((m) => m.buffer));
  }
}

registerProcessor('ask-recorder', RecorderProcessor);
