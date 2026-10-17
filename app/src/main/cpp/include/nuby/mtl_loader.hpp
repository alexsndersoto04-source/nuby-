#pragma once

#include "nuby/scene.hpp"

#include <string>
#include <string_view>
#include <vector>

namespace nuby {

struct WavefrontMaterial {
    std::string name;
    Material material;
};

// Reads the Wavefront MTL material subset used by the engine's PBR renderer.
// Scalar dissolve/transparency and per-map -s/-o/-clamp settings are preserved.
// Texture paths resolve relative to the MTL asset and traversal is rejected.
bool parseMtl(std::string_view source, const std::string& libraryAssetPath,
              std::vector<WavefrontMaterial>& materials, std::string& error);

}  // namespace nuby
