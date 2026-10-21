#pragma once

#include <string>
#include <string_view>
#include <vector>

namespace nuby {

// Interleaved vertex format shared by importers and the GLES renderer.
struct MeshVertex {
    float position[3]{};
    float normal[3]{};
    float uv[2]{};
    float uv1[2]{};
};

struct ObjSubmesh {
    std::string objectName;
    std::string groupName;
    std::string materialName;
    std::vector<MeshVertex> vertices;
};

struct ObjModel {
    std::vector<std::string> materialLibraries;
    std::vector<ObjSubmesh> submeshes;
};

// Parses the supported OBJ geometry subset and preserves contiguous object,
// group and usemtl ranges plus mtllib references. Ear clipping triangulates
// simple polygons; missing normals honor OBJ smoothing groups. Material library
// paths are checked as project-relative paths.
bool parseObjModel(std::string_view source, ObjModel& model, std::string& error);

// Compatibility convenience API for callers that only need concatenated geometry.
bool parseObj(std::string_view source, std::vector<MeshVertex>& vertices,
              std::string& error);

}  // namespace nuby
