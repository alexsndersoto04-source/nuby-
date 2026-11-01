/**
 * Codificador WAV (RIFF) para exportación sin pérdida.
 * Soporta PCM de 16 y 24 bits y float de 32 bits.
 * Sin dependencias: también funciona en Node para pruebas.
 */

/**
 * @param {Float32Array[]} channels canales del audio (misma longitud)
 * @param {number} sampleRate
 * @param {{bitDepth?: 16|24|32}} [options] 32 = float de 32 bits
 * @returns {ArrayBuffer}
 */
export function encodeWav(channels, sampleRate, options = {}) {
  const bitDepth = options.bitDepth ?? 24;
  if (!channels.length) throw new Error('encodeWav: se necesita al menos un canal');
  const numChannels = channels.length;
  const numFrames = channels[0].length;
  const bytesPerSample = bitDepth / 8;
  const blockAlign = numChannels * bytesPerSample;
  const dataSize = numFrames * blockAlign;
  const buffer = new ArrayBuffer(44 + dataSize);
  const view = new DataView(buffer);

  writeAscii(view, 0, 'RIFF');
  view.setUint32(4, 36 + dataSize, true);
  writeAscii(view, 8, 'WAVE');
  writeAscii(view, 12, 'fmt ');
  view.setUint32(16, 16, true); // tamaño del bloque fmt
  view.setUint16(20, bitDepth === 32 ? 3 : 1, true); // 3 = IEEE float, 1 = PCM
  view.setUint16(22, numChannels, true);
  view.setUint32(24, sampleRate, true);
  view.setUint32(28, sampleRate * blockAlign, true);
  view.setUint16(32, blockAlign, true);
  view.setUint16(34, bitDepth, true);
  writeAscii(view, 36, 'data');
  view.setUint32(40, dataSize, true);

  let offset = 44;
  if (bitDepth === 16) {
    for (let i = 0; i < numFrames; i++) {
      for (let c = 0; c < numChannels; c++) {
        view.setInt16(offset, floatToInt16(channels[c][i]), true);
        offset += 2;
      }
    }
  } else if (bitDepth === 24) {
    for (let i = 0; i < numFrames; i++) {
      for (let c = 0; c < numChannels; c++) {
        const v = floatToInt24(channels[c][i]);
        view.setUint8(offset, v & 0xff);
        view.setUint8(offset + 1, (v >> 8) & 0xff);
        view.setUint8(offset + 2, (v >> 16) & 0xff);
        offset += 3;
      }
    }
  } else if (bitDepth === 32) {
    for (let i = 0; i < numFrames; i++) {
      for (let c = 0; c < numChannels; c++) {
        view.setFloat32(offset, channels[c][i], true);
        offset += 4;
      }
    }
  } else {
    throw new Error(`encodeWav: profundidad no soportada (${bitDepth})`);
  }

  return buffer;
}

function writeAscii(view, offset, text) {
  for (let i = 0; i < text.length; i++) view.setUint8(offset + i, text.charCodeAt(i));
}

/** Float -1..1 -> Int16 con recorte y redondeo correcto. */
export function floatToInt16(sample) {
  const s = Math.max(-1, Math.min(1, sample));
  // 32768 no cabe en int16: se usa el máximo asimétrico únicamente en el extremo negativo.
  return s < 0 ? Math.round(s * 32768) : Math.round(s * 32767);
}

/** Float -1..1 -> entero de 24 bits (devuelto como int32 con signo). */
export function floatToInt24(sample) {
  const s = Math.max(-1, Math.min(1, sample));
  const scaled = s < 0 ? Math.round(s * 8388608) : Math.round(s * 8388607);
  return scaled < 0 ? scaled + 0x1000000 : scaled; // almacenado como 3 bytes little-endian
}

/** Envuelve bytes en un Blob con el tipo MIME correcto. */
export function wavBlob(channels, sampleRate, options) {
  return new Blob([encodeWav(channels, sampleRate, options)], { type: 'audio/wav' });
}
