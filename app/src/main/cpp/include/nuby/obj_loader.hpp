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
};

// Parses the geometry subset of Wavefront OBJ: positions, UVs, normals and
// polygon faces. Faces are triangulated and missing normals are generated.
bool parseObj(std::string_view source, std::vector<MeshVertex>& vertices,
              std::string& error);

}  // namespace nuby
