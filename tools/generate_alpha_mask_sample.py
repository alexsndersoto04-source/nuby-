#!/usr/bin/env python3
"""Generate a tiny deterministic GLB used to exercise masked camera/shadow rendering."""
from __future__ import annotations

import json
import pathlib
import struct
import zlib

ROOT = pathlib.Path(__file__).resolve().parents[1]
OUTPUT = ROOT / "app/src/main/assets/engine-tests/alpha_mask_card.glb"


def png_chunk(kind: bytes, data: bytes) -> bytes:
    return (struct.pack(">I", len(data)) + kind + data
            + struct.pack(">I", zlib.crc32(kind + data) & 0xFFFFFFFF))


def make_png() -> bytes:
    width, height = 64, 96
    scanlines = bytearray()
    for y in range(height):
        scanlines.append(0)  # PNG filter: None.
        for x in range(width):
            nx = (x - (width - 1) * 0.5) / (width * 0.44)
            ny = (y - (height - 1) * 0.5) / (height * 0.48)
            inside = nx * nx + ny * ny <= 1.0
            holes = ((x - 32) ** 2 + (y - 27) ** 2 < 7 ** 2
                     or (x - 23) ** 2 + (y - 50) ** 2 < 5 ** 2
                     or (x - 41) ** 2 + (y - 50) ** 2 < 5 ** 2
                     or (x - 32) ** 2 + (y - 70) ** 2 < 6 ** 2)
            alpha = 255 if inside and not holes else 0
            edge = inside and (nx * nx + ny * ny > 0.82)
            color = (224, 178, 76) if edge else (53, 173, 104)
            scanlines.extend((*color, alpha))
    header = struct.pack(">2I5B", width, height, 8, 6, 0, 0, 0)
    return (b"\x89PNG\r\n\x1a\n"
            + png_chunk(b"IHDR", header)
            + png_chunk(b"IDAT", zlib.compress(bytes(scanlines), level=9))
            + png_chunk(b"IEND", b""))


def align4(data: bytearray) -> None:
    data.extend(b"\0" * ((-len(data)) & 3))


def main() -> None:
    binary = bytearray()
    views: list[dict[str, int]] = []

    def append_view(payload: bytes, target: int | None = None) -> int:
        align4(binary)
        index = len(views)
        entry = {"buffer": 0, "byteOffset": len(binary), "byteLength": len(payload)}
        if target is not None:
            entry["target"] = target
        views.append(entry)
        binary.extend(payload)
        return index

    positions = [(-0.65, 0.0, 0.0), (0.65, 0.0, 0.0),
                 (0.65, 2.0, 0.0), (-0.65, 2.0, 0.0)]
    normals = [(0.0, 0.0, 1.0)] * 4
    uvs = [(0.0, 0.0), (1.0, 0.0), (1.0, 1.0), (0.0, 1.0)]
    position_view = append_view(b"".join(struct.pack("<3f", *v) for v in positions), 34962)
    normal_view = append_view(b"".join(struct.pack("<3f", *v) for v in normals), 34962)
    uv_view = append_view(b"".join(struct.pack("<2f", *v) for v in uvs), 34962)
    index_view = append_view(struct.pack("<6H", 0, 1, 2, 0, 2, 3), 34963)
    image_view = append_view(make_png())
    align4(binary)

    accessors = [
        {"bufferView": position_view, "componentType": 5126, "count": 4,
         "type": "VEC3", "min": [-0.65, 0.0, 0.0], "max": [0.65, 2.0, 0.0]},
        {"bufferView": normal_view, "componentType": 5126, "count": 4, "type": "VEC3"},
        {"bufferView": uv_view, "componentType": 5126, "count": 4, "type": "VEC2"},
        {"bufferView": index_view, "componentType": 5123, "count": 6, "type": "SCALAR"},
    ]
    document = {
        "asset": {"version": "2.0", "generator": "Nuby Engine alpha-mask fixture"},
        "scene": 0,
        "scenes": [{"nodes": [0]}],
        "nodes": [{"name": "Perforated alpha-mask card", "mesh": 0}],
        "meshes": [{"primitives": [{"attributes": {
            "POSITION": 0, "NORMAL": 1, "TEXCOORD_0": 2},
            "indices": 3, "material": 0}]}],
        "materials": [{"name": "Cutout foliage", "doubleSided": True,
            "pbrMetallicRoughness": {"baseColorFactor": [1.0, 1.0, 1.0, 1.0],
                "baseColorTexture": {"index": 0}, "metallicFactor": 0.05,
                "roughnessFactor": 0.72}, "alphaMode": "MASK", "alphaCutoff": 0.5}],
        "textures": [{"source": 0}],
        "images": [{"bufferView": image_view, "mimeType": "image/png"}],
        "bufferViews": views,
        "accessors": accessors,
        "buffers": [{"byteLength": len(binary)}],
    }
    encoded_json = json.dumps(document, separators=(",", ":"), ensure_ascii=True).encode()
    encoded_json += b" " * ((-len(encoded_json)) & 3)
    total_length = 12 + 8 + len(encoded_json) + 8 + len(binary)
    glb = (struct.pack("<III", 0x46546C67, 2, total_length)
           + struct.pack("<II", len(encoded_json), 0x4E4F534A) + encoded_json
           + struct.pack("<II", len(binary), 0x004E4942) + binary)
    OUTPUT.parent.mkdir(parents=True, exist_ok=True)
    OUTPUT.write_bytes(glb)
    print(f"Generated {OUTPUT} ({len(glb)} bytes, perforated alpha-mask PNG)")


if __name__ == "__main__":
    main()
