#!/usr/bin/env python3
"""Generate deterministic idle, walk, run and jump clips in the Wayfarer GLB.

The mesh and skin are preserved. Re-running this tool is safe: animation
accessors are rebuilt rather than duplicated.
"""
from __future__ import annotations

import json
import math
import struct
from pathlib import Path

ASSET = Path(__file__).resolve().parents[1] / "app/src/main/assets/characters/wayfarer_skeletal.glb"
GLB_MAGIC = 0x46546C67
JSON_CHUNK = 0x4E4F534A
BIN_CHUNK = 0x004E4942
FLOAT = 5126


def align4(data: bytearray) -> None:
    data.extend(b"\0" * ((-len(data)) % 4))


def quat_z(angle: float) -> tuple[float, float, float, float]:
    return (0.0, 0.0, math.sin(angle * 0.5), math.cos(angle * 0.5))


def quat_x(angle: float) -> tuple[float, float, float, float]:
    return (math.sin(angle * 0.5), 0.0, 0.0, math.cos(angle * 0.5))


def pack_track(values: list[tuple[float, float, float, float]]) -> bytes:
    return b"".join(struct.pack("<4f", *value) for value in values)


def track_samples(amplitude: float, phase: float = 0.0) -> list[float]:
    # Closed loop: neutral at both ends, with a smooth four-step stride.
    return [amplitude * math.sin(2.0 * math.pi * index / 4.0 + phase)
            for index in range(5)]


def read_glb(data: bytes) -> tuple[dict, bytearray]:
    if len(data) < 20:
        raise ValueError("GLB is truncated")
    magic, version, total = struct.unpack_from("<III", data)
    if magic != GLB_MAGIC or version != 2 or total != len(data):
        raise ValueError("Unsupported or malformed GLB header")
    offset = 12
    document = None
    binary = None
    while offset < len(data):
        if len(data) - offset < 8:
            raise ValueError("GLB chunk header is truncated")
        length, kind = struct.unpack_from("<II", data, offset)
        offset += 8
        if length > len(data) - offset:
            raise ValueError("GLB chunk exceeds file length")
        chunk = data[offset:offset + length]
        offset += length
        if kind == JSON_CHUNK:
            document = json.loads(chunk.rstrip(b" \t\r\n\0"))
        elif kind == BIN_CHUNK:
            binary = bytearray(chunk)
    if document is None or binary is None:
        raise ValueError("GLB must contain JSON and BIN chunks")
    return document, binary


def encode_glb(document: dict, binary: bytearray) -> bytes:
    document["buffers"][0]["byteLength"] = len(binary)
    json_bytes = json.dumps(document, separators=(",", ":"), ensure_ascii=True).encode("utf-8")
    json_bytes += b" " * ((-len(json_bytes)) % 4)
    binary.extend(b"\0" * ((-len(binary)) % 4))
    total = 12 + 8 + len(json_bytes) + 8 + len(binary)
    return (struct.pack("<III", GLB_MAGIC, 2, total)
            + struct.pack("<II", len(json_bytes), JSON_CHUNK) + json_bytes
            + struct.pack("<II", len(binary), BIN_CHUNK) + binary)


def add_clip(document: dict, binary: bytearray, name: str,
             arm_amplitude: float, leg_amplitude: float,
             jump_pose: bool = False) -> None:
    times = [0.0, 0.25, 0.5, 0.75, 1.0]
    time_bytes = struct.pack("<5f", *times)
    align4(binary)
    time_offset = len(binary)
    binary.extend(time_bytes)
    time_view = len(document["bufferViews"])
    document["bufferViews"].append({"buffer": 0, "byteOffset": time_offset,
                                    "byteLength": len(time_bytes)})
    time_accessor = len(document["accessors"])
    document["accessors"].append({"bufferView": time_view, "componentType": FLOAT,
                                  "count": 5, "type": "SCALAR", "min": [0.0], "max": [1.0]})

    samplers = []
    channels = []
    targets = [(3, arm_amplitude, 0.0, "arm"),
               (4, arm_amplitude, math.pi, "arm"),
               (5, leg_amplitude, math.pi, "leg"),
               (6, leg_amplitude, 0.0, "leg")]
    for joint, amplitude, phase, limb in targets:
        if jump_pose:
            angles = ([0.0, 0.45, 0.72, 0.72, 0.72] if limb == "arm"
                      else [0.0, -0.15, -0.38, -0.38, -0.38])
            if joint in (4, 6):
                angles = [-value for value in angles]
        else:
            angles = track_samples(amplitude, phase)
        quaternions = [quat_z(value) if limb == "arm" else quat_x(value)
                       for value in angles]
        encoded = pack_track(quaternions)
        align4(binary)
        offset = len(binary)
        binary.extend(encoded)
        view = len(document["bufferViews"])
        document["bufferViews"].append({"buffer": 0, "byteOffset": offset,
                                        "byteLength": len(encoded)})
        accessor = len(document["accessors"])
        document["accessors"].append({"bufferView": view, "componentType": FLOAT,
                                      "count": 5, "type": "VEC4"})
        sampler = len(samplers)
        samplers.append({"input": time_accessor, "output": accessor,
                         "interpolation": "LINEAR"})
        channels.append({"sampler": sampler,
                         "target": {"node": joint, "path": "rotation"}})
    document["animations"].append({"name": name, "samplers": samplers, "channels": channels})


def main() -> None:
    document, binary = read_glb(ASSET.read_bytes())
    base_accessors = document["accessors"]
    used_accessors: set[int] = {document["skins"][0]["inverseBindMatrices"]}
    for primitive in document["meshes"][0]["primitives"]:
        used_accessors.add(primitive["indices"])
        used_accessors.update(primitive["attributes"].values())
    used_views = {base_accessors[index]["bufferView"] for index in used_accessors}
    end = max(document["bufferViews"][index].get("byteOffset", 0)
              + document["bufferViews"][index]["byteLength"] for index in used_views)
    binary = binary[:end]
    base_views = document["bufferViews"]
    document["bufferViews"] = [base_views[index] for index in sorted(used_views)]
    view_remap = {old: new for new, old in enumerate(sorted(used_views))}
    document["accessors"] = [base_accessors[index] for index in sorted(used_accessors)]
    for accessor in document["accessors"]:
        accessor["bufferView"] = view_remap[accessor["bufferView"]]
    accessor_remap = {old: new for new, old in enumerate(sorted(used_accessors))}
    for primitive in document["meshes"][0]["primitives"]:
        primitive["indices"] = accessor_remap[primitive["indices"]]
        primitive["attributes"] = {key: accessor_remap[value]
                                    for key, value in primitive["attributes"].items()}
    document["skins"][0]["inverseBindMatrices"] = accessor_remap[
        document["skins"][0]["inverseBindMatrices"]]
    document["animations"] = []
    for view in document["bufferViews"]:
        view["buffer"] = 0

    add_clip(document, binary, "idle", 0.035, 0.0)
    add_clip(document, binary, "walk", 0.45, 0.52)
    add_clip(document, binary, "run", 0.9, 0.72)
    add_clip(document, binary, "jump", 0.0, 0.0, jump_pose=True)
    ASSET.write_bytes(encode_glb(document, binary))
    print(f"Updated {ASSET}: clips={','.join(a['name'] for a in document['animations'])}, "
          f"bytes={ASSET.stat().st_size}")


if __name__ == "__main__":
    main()
