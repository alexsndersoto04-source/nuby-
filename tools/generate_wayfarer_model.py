#!/usr/bin/env python3
"""Build the engine's deterministic, skinned Wayfarer explorer model.

The mesh is assembled from low-poly ellipsoids and a simple rig so the asset is
reproducible without Blender or third-party libraries. Run this first, then run
 generate_wayfarer_animations.py to append the four locomotion clips.
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
USHORT = 5123
UBYTE = 5121

# The engine palette is deliberately small and authored as glTF PBR factors.
MATERIALS = [
    {"name": "Tunic - deep teal", "pbrMetallicRoughness": {
        "baseColorFactor": [0.10, 0.34, 0.43, 1.0], "metallicFactor": 0.04,
        "roughnessFactor": 0.84}},
    {"name": "Mantle - midnight blue", "pbrMetallicRoughness": {
        "baseColorFactor": [0.045, 0.12, 0.20, 1.0], "metallicFactor": 0.08,
        "roughnessFactor": 0.78}},
    {"name": "Trousers - slate", "pbrMetallicRoughness": {
        "baseColorFactor": [0.13, 0.19, 0.25, 1.0], "metallicFactor": 0.02,
        "roughnessFactor": 0.9}},
    {"name": "Leather - warm umber", "pbrMetallicRoughness": {
        "baseColorFactor": [0.30, 0.13, 0.055, 1.0], "metallicFactor": 0.0,
        "roughnessFactor": 0.88}},
    {"name": "Trim - antique brass", "pbrMetallicRoughness": {
        "baseColorFactor": [0.62, 0.34, 0.085, 1.0], "metallicFactor": 0.62,
        "roughnessFactor": 0.42}},
    {"name": "Gloves and boots - dark leather", "pbrMetallicRoughness": {
        "baseColorFactor": [0.12, 0.075, 0.055, 1.0], "metallicFactor": 0.0,
        "roughnessFactor": 0.94}},
]

# Joint node world-space bind locations match the original Wayfarer rig. Mesh
# positions are authored in model space and weighted to the closest body joint.
JOINT_NAMES = ["Pelvis", "Spine", "Neck", "LeftArm", "RightArm", "LeftLeg", "RightLeg"]
JOINT_WORLD = [
    (0.0, 0.55, 0.0), (0.0, 1.0, 0.0), (0.0, 1.4, 0.0),
    (-0.32, 1.25, 0.0), (0.32, 1.25, 0.0),
    (-0.15, 0.25, 0.0), (0.15, 0.25, 0.0),
]

class MeshGroup:
    def __init__(self) -> None:
        self.positions: list[tuple[float, float, float]] = []
        self.normals: list[tuple[float, float, float]] = []
        self.uvs: list[tuple[float, float]] = []
        self.joints: list[tuple[int, int, int, int]] = []
        self.weights: list[tuple[float, float, float, float]] = []
        self.indices: list[int] = []


groups = [MeshGroup() for _ in MATERIALS]


def add_ellipsoid(material: int, center: tuple[float, float, float],
                  radii: tuple[float, float, float], joint: int,
                  slices: int = 16, stacks: int = 10) -> None:
    mesh = groups[material]
    base = len(mesh.positions)
    cx, cy, cz = center
    rx, ry, rz = radii
    # One north pole, interior latitude rings, and one south pole avoid
    # degenerate pole triangles while retaining smooth normals.
    mesh.positions.append((cx, cy + ry, cz))
    mesh.normals.append((0.0, 1.0, 0.0))
    mesh.uvs.append((0.5, 0.0))
    mesh.joints.append((joint, 0, 0, 0))
    mesh.weights.append((1.0, 0.0, 0.0, 0.0))
    for ring in range(1, stacks):
        theta = math.pi * ring / stacks
        st, ct = math.sin(theta), math.cos(theta)
        for side in range(slices):
            phi = 2.0 * math.pi * side / slices
            cp, sp = math.cos(phi), math.sin(phi)
            position = (cx + rx * st * cp, cy + ry * ct, cz + rz * st * sp)
            nx, ny, nz = st * cp / rx, ct / ry, st * sp / rz
            length = math.sqrt(nx * nx + ny * ny + nz * nz)
            mesh.positions.append(position)
            mesh.normals.append((nx / length, ny / length, nz / length))
            mesh.uvs.append((side / slices, ring / stacks))
            mesh.joints.append((joint, 0, 0, 0))
            mesh.weights.append((1.0, 0.0, 0.0, 0.0))
    bottom = len(mesh.positions)
    mesh.positions.append((cx, cy - ry, cz))
    mesh.normals.append((0.0, -1.0, 0.0))
    mesh.uvs.append((0.5, 1.0))
    mesh.joints.append((joint, 0, 0, 0))
    mesh.weights.append((1.0, 0.0, 0.0, 0.0))

    first_ring = base + 1
    for side in range(slices):
        following = (side + 1) % slices
        # Winding faces outwards in the engine's right-handed model space.
        mesh.indices.extend((base, first_ring + following, first_ring + side))
    for ring in range(stacks - 2):
        upper = first_ring + ring * slices
        lower = upper + slices
        for side in range(slices):
            following = (side + 1) % slices
            mesh.indices.extend((upper + side, upper + following, lower + side,
                                 upper + following, lower + following, lower + side))
    last_ring = first_ring + (stacks - 2) * slices
    for side in range(slices):
        following = (side + 1) % slices
        mesh.indices.extend((bottom, last_ring + side, last_ring + following))


def add_horizontal_torus(material: int, center_y: float, major: float,
                         minor: float, joint: int, segments: int = 32,
                         tube_segments: int = 8) -> None:
    mesh = groups[material]
    base = len(mesh.positions)
    for ring in range(segments):
        phi = 2.0 * math.pi * ring / segments
        cp, sp = math.cos(phi), math.sin(phi)
        for tube in range(tube_segments):
            theta = 2.0 * math.pi * tube / tube_segments
            ct, st = math.cos(theta), math.sin(theta)
            radial = major + minor * ct
            mesh.positions.append((radial * cp, center_y + minor * st, radial * sp))
            mesh.normals.append((ct * cp, st, ct * sp))
            mesh.uvs.append((ring / segments, tube / tube_segments))
            mesh.joints.append((joint, 0, 0, 0))
            mesh.weights.append((1.0, 0.0, 0.0, 0.0))
    for ring in range(segments):
        next_ring = (ring + 1) % segments
        for tube in range(tube_segments):
            next_tube = (tube + 1) % tube_segments
            a = base + ring * tube_segments + tube
            b = base + next_ring * tube_segments + tube
            c = base + ring * tube_segments + next_tube
            d = base + next_ring * tube_segments + next_tube
            mesh.indices.extend((a, c, b, c, d, b))


def build_character() -> None:
    global groups
    groups = [MeshGroup() for _ in MATERIALS]
    # Tailored tunic and a split hem create a readable torso silhouette.
    add_ellipsoid(0, (0.0, 0.93, 0.0), (0.30, 0.39, 0.205), 1, 20, 12)
    add_ellipsoid(0, (0.0, 0.64, 0.0), (0.33, 0.19, 0.225), 0, 20, 10)
    add_ellipsoid(1, (0.0, 1.205, 0.0), (0.385, 0.155, 0.235), 1, 18, 9)
    # Arms swing from the rig's shoulder pivots; gloves follow those same bones.
    for side, joint in ((-1.0, 3), (1.0, 4)):
        add_ellipsoid(0, (side * 0.405, 0.955, 0.0), (0.132, 0.305, 0.135), joint)
        add_ellipsoid(5, (side * 0.425, 0.625, 0.0), (0.12, 0.125, 0.125), joint, 12, 8)
        # Raised shoulder guards, rivets and a narrow upper-arm seam.
        add_ellipsoid(1, (side * 0.325, 1.215, 0.0), (0.165, 0.115, 0.17), 1, 14, 8)
        add_ellipsoid(4, (side * 0.34, 1.225, 0.145), (0.055, 0.045, 0.026), 1, 10, 6)
    # Separate trousers and cuffed field boots keep the leg swing visible.
    for side, joint in ((-1.0, 5), (1.0, 6)):
        add_ellipsoid(2, (side * 0.15, 0.285, 0.0), (0.137, 0.295, 0.14), joint)
        add_ellipsoid(5, (side * 0.15, 0.095, 0.045), (0.16, 0.135, 0.215), joint)
        add_ellipsoid(3, (side * 0.15, 0.055, 0.06), (0.165, 0.045, 0.22), joint, 12, 6)
    # A leather belt, brass clasp and chest crest add small readable landmarks.
    add_horizontal_torus(3, 0.635, 0.277, 0.047, 0)
    add_ellipsoid(4, (0.0, 0.645, 0.229), (0.075, 0.067, 0.032), 0, 12, 8)
    add_ellipsoid(4, (0.0, 1.01, 0.201), (0.075, 0.09, 0.025), 1, 12, 8)
    # Cross-body satchel straps are weighted to the torso and sit on its back.
    for side in (-1.0, 1.0):
        add_ellipsoid(3, (side * 0.105, 0.97, -0.185), (0.037, 0.34, 0.023), 1, 10, 8)
    add_horizontal_torus(4, 1.375, 0.145, 0.028, 2, 24, 8)



def append_view(document: dict, binary: bytearray, payload: bytes) -> int:
    binary.extend(b"\0" * ((-len(binary)) % 4))
    offset = len(binary)
    binary.extend(payload)
    view = len(document["bufferViews"])
    document["bufferViews"].append({"buffer": 0, "byteOffset": offset,
                                    "byteLength": len(payload)})
    return view


def append_accessor(document: dict, binary: bytearray, values: list[tuple],
                    fmt: str, component_type: int, element_type: str,
                    minimum: list[float] | None = None,
                    maximum: list[float] | None = None) -> int:
    flat = [component for row in values for component in row]
    payload = struct.pack("<" + fmt * len(flat), *flat)
    view = append_view(document, binary, payload)
    accessor = {"bufferView": view, "componentType": component_type,
                "count": len(values), "type": element_type}
    if minimum is not None:
        accessor["min"] = minimum
    if maximum is not None:
        accessor["max"] = maximum
    document["accessors"].append(accessor)
    return len(document["accessors"]) - 1


def encode_glb(document: dict, binary: bytearray) -> bytes:
    document["buffers"] = [{"byteLength": len(binary)}]
    json_bytes = json.dumps(document, separators=(",", ":"), ensure_ascii=True).encode("utf-8")
    json_bytes += b" " * ((-len(json_bytes)) % 4)
    binary.extend(b"\0" * ((-len(binary)) % 4))
    total = 12 + 8 + len(json_bytes) + 8 + len(binary)
    return (struct.pack("<III", GLB_MAGIC, 2, total)
            + struct.pack("<II", len(json_bytes), JSON_CHUNK) + json_bytes
            + struct.pack("<II", len(binary), BIN_CHUNK) + binary)


def build_glb() -> bytes:
    build_character()
    binary = bytearray()
    document = {
        "asset": {"version": "2.0", "generator": "Nuby deterministic Wayfarer model builder"},
        "scene": 0,
        "scenes": [{"nodes": [0, 7]}],
        "nodes": [
            {"name": "Pelvis", "translation": [0.0, 0.55, 0.0], "children": [1, 5, 6]},
            {"name": "Spine", "translation": [0.0, 0.45, 0.0], "children": [2, 3, 4]},
            {"name": "Neck", "translation": [0.0, 0.40, 0.0]},
            {"name": "LeftArm", "translation": [-0.32, 0.25, 0.0]},
            {"name": "RightArm", "translation": [0.32, 0.25, 0.0]},
            {"name": "LeftLeg", "translation": [-0.15, -0.30, 0.0]},
            {"name": "RightLeg", "translation": [0.15, -0.30, 0.0]},
            {"name": "WayfarerMesh", "mesh": 0, "skin": 0},
        ],
        "skins": [{"name": "WayfarerRig", "joints": list(range(7)),
                   "inverseBindMatrices": 0, "skeleton": 0}],
        "meshes": [{"name": "WayfarerExplorer", "primitives": []}],
        "materials": MATERIALS,
        "bufferViews": [],
        "accessors": [],
        "buffers": [],
    }
    inverse_binds = []
    for x, y, z in JOINT_WORLD:
        inverse_binds.append((1.0, 0.0, 0.0, 0.0,
                              0.0, 1.0, 0.0, 0.0,
                              0.0, 0.0, 1.0, 0.0,
                              -x, -y, -z, 1.0))
    inverse_accessor = append_accessor(document, binary, inverse_binds,
                                       "f", FLOAT, "MAT4")
    assert inverse_accessor == 0

    for material, mesh in enumerate(groups):
        if not mesh.indices:
            continue
        mins = [min(point[axis] for point in mesh.positions) for axis in range(3)]
        maxs = [max(point[axis] for point in mesh.positions) for axis in range(3)]
        position = append_accessor(document, binary, mesh.positions, "f", FLOAT, "VEC3", mins, maxs)
        normal = append_accessor(document, binary, mesh.normals, "f", FLOAT, "VEC3")
        uv = append_accessor(document, binary, mesh.uvs, "f", FLOAT, "VEC2")
        joints = append_accessor(document, binary, mesh.joints, "B", UBYTE, "VEC4")
        weights = append_accessor(document, binary, mesh.weights, "f", FLOAT, "VEC4")
        indices = append_accessor(document, binary,
                                  [(index,) for index in mesh.indices], "H", USHORT, "SCALAR",
                                  [0], [max(mesh.indices)])
        document["meshes"][0]["primitives"].append({
            "attributes": {"POSITION": position, "NORMAL": normal, "TEXCOORD_0": uv,
                           "JOINTS_0": joints, "WEIGHTS_0": weights},
            "indices": indices, "material": material, "mode": 4,
        })
    # Include inverse binds before the first mesh accessor; append_accessor above
    # places this first and the skin palette order matches the joint list.
    return encode_glb(document, binary)


def main() -> None:
    ASSET.parent.mkdir(parents=True, exist_ok=True)
    data = build_glb()
    ASSET.write_bytes(data)
    print(f"Generated {ASSET} ({len(data)} bytes, {len(MATERIALS)} materials, "
          f"{sum(bool(group.indices) for group in groups)} skinned parts)")


if __name__ == "__main__":
    main()
