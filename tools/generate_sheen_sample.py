#!/usr/bin/env python3
"""Generate a deterministic GLB that exercises sheen textures and clearcoat layering."""
from __future__ import annotations

import json
import math
import pathlib
import struct
import zlib

ROOT = pathlib.Path(__file__).resolve().parents[1]
OUTPUT = ROOT / "app/src/main/assets/engine-tests/sheen_clearcoat_sphere.glb"


def png_chunk(kind: bytes, data: bytes) -> bytes:
    checksum = zlib.crc32(kind + data) & 0xFFFFFFFF
    return struct.pack(">I", len(data)) + kind + data + struct.pack(">I", checksum)


def make_png(width: int, height: int, color_map: bool) -> bytes:
    rows = bytearray()
    for y in range(height):
        rows.append(0)
        for x in range(width):
            u = x / max(width - 1, 1)
            v = y / max(height - 1, 1)
            if color_map:
                stripe = 0.5 + 0.5 * math.sin((u * 5.0 + v * 0.7) * math.tau)
                red = int(72 + 104 * stripe)
                green = int(35 + 55 * (1.0 - stripe))
                blue = int(108 + 106 * (0.35 + 0.65 * math.cos(v * math.tau) ** 2))
                alpha = 255
            else:
                # Sheen roughness is sampled from alpha in glTF; keep RGB neutral.
                red = green = blue = 255
                alpha = int(72 + 155 * (0.5 + 0.5 * math.sin((u * 2.0 - v) * math.tau)))
            rows.extend((red, green, blue, alpha))
    header = struct.pack(">2I5B", width, height, 8, 6, 0, 0, 0)
    return (b"\x89PNG\r\n\x1a\n" + png_chunk(b"IHDR", header)
            + png_chunk(b"IDAT", zlib.compress(bytes(rows), level=9))
            + png_chunk(b"IEND", b""))


def align4(data: bytearray) -> None:
    data.extend(b"\0" * ((-len(data)) & 3))


def main() -> None:
    binary = bytearray()
    views: list[dict[str, int]] = []

    def append_view(payload: bytes, target: int | None = None) -> int:
        align4(binary)
        index = len(views)
        view = {"buffer": 0, "byteOffset": len(binary), "byteLength": len(payload)}
        if target is not None:
            view["target"] = target
        views.append(view)
        binary.extend(payload)
        return index

    latitude_segments, longitude_segments = 24, 48
    positions: list[tuple[float, float, float]] = []
    normals: list[tuple[float, float, float]] = []
    uvs0: list[tuple[float, float]] = []
    uvs1: list[tuple[float, float]] = []
    for row in range(latitude_segments + 1):
        latitude = math.pi * row / latitude_segments
        for column in range(longitude_segments + 1):
            longitude = math.tau * column / longitude_segments
            normal = (math.sin(latitude) * math.cos(longitude),
                      math.cos(latitude),
                      math.sin(latitude) * math.sin(longitude))
            positions.append(tuple(component * 0.72 for component in normal))
            normals.append(normal)
            uvs0.append((column / longitude_segments, row / latitude_segments))
            # Slightly offset and rescaled UV1 makes the second map's independent route visible.
            uvs1.append((0.08 + 0.84 * column / longitude_segments,
                         0.04 + 0.92 * row / latitude_segments))

    indices: list[int] = []
    row_width = longitude_segments + 1
    for row in range(latitude_segments):
        for column in range(longitude_segments):
            top_left = row * row_width + column
            top_right = top_left + 1
            bottom_left = top_left + row_width
            bottom_right = bottom_left + 1
            indices.extend((top_left, bottom_right, bottom_left,
                            top_left, top_right, bottom_right))

    position_view = append_view(b"".join(struct.pack("<3f", *v) for v in positions), 34962)
    normal_view = append_view(b"".join(struct.pack("<3f", *v) for v in normals), 34962)
    uv0_view = append_view(b"".join(struct.pack("<2f", *v) for v in uvs0), 34962)
    uv1_view = append_view(b"".join(struct.pack("<2f", *v) for v in uvs1), 34962)
    index_view = append_view(struct.pack(f"<{len(indices)}H", *indices), 34963)
    color_image_view = append_view(make_png(48, 48, color_map=True))
    roughness_image_view = append_view(make_png(48, 48, color_map=False))
    align4(binary)

    accessors = [
        {"bufferView": position_view, "componentType": 5126, "count": len(positions),
         "type": "VEC3", "min": [-0.72, -0.72, -0.72], "max": [0.72, 0.72, 0.72]},
        {"bufferView": normal_view, "componentType": 5126, "count": len(normals), "type": "VEC3"},
        {"bufferView": uv0_view, "componentType": 5126, "count": len(uvs0), "type": "VEC2"},
        {"bufferView": uv1_view, "componentType": 5126, "count": len(uvs1), "type": "VEC2"},
        {"bufferView": index_view, "componentType": 5123, "count": len(indices), "type": "SCALAR"},
    ]
    document = {
        "asset": {"version": "2.0", "generator": "Nuby Engine sheen/clearcoat fixture"},
        "scene": 0,
        "scenes": [{"nodes": [0]}],
        "nodes": [{"name": "Sheen over velvet; clearcoat above", "mesh": 0}],
        "meshes": [{"primitives": [{"attributes": {
            "POSITION": 0, "NORMAL": 1, "TEXCOORD_0": 2, "TEXCOORD_1": 3},
            "indices": 4, "material": 0}]}],
        "materials": [{"name": "Indigo velvet with glossy clearcoat",
            "pbrMetallicRoughness": {"baseColorFactor": [0.48, 0.54, 0.72, 1.0],
                "metallicFactor": 0.12, "roughnessFactor": 0.56},
            "extensions": {
                "KHR_materials_sheen": {
                    "sheenColorFactor": [0.85, 0.38, 0.22],
                    "sheenColorTexture": {"index": 0, "texCoord": 0,
                        "extensions": {"KHR_texture_transform": {
                            "offset": [0.03, 0.02], "scale": [1.1, 0.95], "rotation": 0.12}}},
                    "sheenRoughnessFactor": 0.62,
                    "sheenRoughnessTexture": {"index": 1, "texCoord": 1,
                        "extensions": {"KHR_texture_transform": {
                            "offset": [0.04, 0.07], "scale": [0.9, 1.05], "rotation": -0.08}}}},
                "KHR_materials_clearcoat": {"clearcoatFactor": 0.38,
                    "clearcoatRoughnessFactor": 0.16}},
        }],
        "textures": [{"source": 0}, {"source": 1}],
        "images": [
            {"bufferView": color_image_view, "mimeType": "image/png"},
            {"bufferView": roughness_image_view, "mimeType": "image/png"},
        ],
        "extensionsUsed": ["KHR_materials_sheen", "KHR_materials_clearcoat",
                           "KHR_texture_transform"],
        "extensionsRequired": ["KHR_materials_sheen", "KHR_materials_clearcoat",
                               "KHR_texture_transform"],
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
    print(f"Generated {OUTPUT} ({len(glb)} bytes, sheen color + alpha roughness PNGs)")


if __name__ == "__main__":
    main()
