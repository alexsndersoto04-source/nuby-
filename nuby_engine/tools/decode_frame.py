#!/usr/bin/env python3
"""Decodifica un frame RLE de Nuby (/frame.raw) a PNG.

Formato RLE (ver encode_rle en src/server/web_server.cpp):
  cabecera: w, h, len (3 × u32 little-endian); luego `len` bytes:
   - ctrl > 128: run de (257-ctrl) píxeles, seguido de R,G,B
   - ctrl <= 128: literal de (ctrl+1) píxeles, seguido de (ctrl+1) × R,G,B

Uso:
  python3 decode_frame.py frame.dat salida.png      # desde archivo
  curl -b cookies http://host/frame.raw > f.dat
"""
import sys, struct, zlib

def decode(data):
    w, h, ln = struct.unpack('<III', data[:12])
    body = data[12:12+ln]
    px = bytearray()
    i = 0
    while i < len(body):
        c = body[i]; i += 1
        if c > 128:
            run = 257 - c
            r, g, b = body[i], body[i+1], body[i+2]; i += 3
            px += bytes((r, g, b)) * run
        else:
            n = c + 1
            px += body[i:i+3*n]; i += 3*n
    if len(px) != w*h*3:
        raise ValueError(f"pixeles decodificados {len(px)} != esperados {w*h*3}")
    return w, h, bytes(px)

def save_png(w, h, rgb, path):
    def chunk(t, d):
        c = struct.pack('>I', len(d)) + t + d
        return c + struct.pack('>I', zlib.crc32(t + d) & 0xffffffff)
    raw = b''.join(b'\x00' + rgb[y*w*3:(y+1)*w*3] for y in range(h))
    out = b'\x89PNG\r\n\x1a\n'
    out += chunk(b'IHDR', struct.pack('>IIBBBBB', w, h, 8, 2, 0, 0, 0))
    out += chunk(b'IDAT', zlib.compress(raw, 6))
    out += chunk(b'IEND', b'')
    open(path, 'wb').write(out)

if __name__ == '__main__':
    src = sys.argv[1]
    dst = sys.argv[2]
    data = open(src, 'rb').read()
    w, h, rgb = decode(data)
    save_png(w, h, rgb, dst)
    print(f"OK {w}x{h} -> {dst}")
