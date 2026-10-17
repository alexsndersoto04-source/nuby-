#include "nuby/mtl_loader.hpp"

#include <cmath>
#include <cstdlib>
#include <fstream>
#include <iostream>
#include <iterator>
#include <string>
#include <vector>

namespace {
int failures = 0;

void expect(bool condition, const char* message) {
    if (!condition) {
        std::cerr << "FAIL: " << message << '\n';
        ++failures;
    }
}

bool near(float actual, float expected, float epsilon = 0.0001f) {
    return std::fabs(actual - expected) <= epsilon;
}

void testPackagedSampleMaterialLibrary() {
    std::ifstream file("app/src/main/assets/meshes/orbital_shard.mtl");
    expect(file.good(), "packaged sample MTL asset exists");
    if (!file.good()) return;
    const std::string source((std::istreambuf_iterator<char>(file)),
                             std::istreambuf_iterator<char>());
    std::vector<nuby::WavefrontMaterial> materials;
    std::string error;
    expect(nuby::parseMtl(source, "meshes/orbital_shard.mtl", materials, error),
           "packaged sample MTL parses");
    expect(materials.size() == 1 && materials[0].name == "ShardMetal",
           "sample MTL material name is preserved");
    if (!materials.empty()) {
        const nuby::Material& material = materials[0].material;
        expect(near(material.metallic, 0.68f) && near(material.roughness, 0.26f)
                       && near(material.emission[0], 0.018f),
               "sample MTL PBR factors parse as expected");
        expect(material.albedoTexture == "meshes/orbital_shard_albedo.tga"
                       && material.normalTexture == "meshes/orbital_shard_normal.tga",
               "sample MTL image paths resolve to packaged assets");
    }
}

void testPbrFactorsAndRelativeMapsParse() {
    constexpr char source[] = R"MTL(
# Simple hand-authored PBR material
newmtl Sun Relic
Kd 0.8 0.55 0.18
Ke 0.02 0.01 0.0
Ns 198.0
Pm 0.72
Pr 0.31
map_Kd -s 0.5 0.75 1 -o 0.1 0.2 0 textures/sun albedo.tga
norm -s 2 2 textures/sun normal.tga
map_Pr -o -0.2 0.25 textures/roughness.tga
map_Pm textures/metallic.tga
map_AO textures/occlusion.tga

newmtl Stone
Kd 0.34 0.31 0.27
Ns 18
illum 2
)MTL";
    std::vector<nuby::WavefrontMaterial> materials;
    std::string error;
    expect(nuby::parseMtl(source, "characters/wayfarer/materials.mtl", materials, error),
           "PBR Wavefront material library parses");
    expect(materials.size() == 2 && materials[0].name == "Sun Relic"
                   && materials[1].name == "Stone",
           "multiple named materials are retained in source order");
    if (materials.size() == 2) {
        const nuby::Material& relic = materials[0].material;
        expect(near(relic.color[0], 0.8f) && near(relic.color[1], 0.55f)
                       && near(relic.emission[0], 0.02f)
                       && near(relic.metallic, 0.72f) && near(relic.roughness, 0.31f),
               "diffuse, emissive, metallic and explicit roughness factors are parsed");
        expect(relic.albedoTexture
                       == "characters/wayfarer/textures/sun albedo.tga"
                       && relic.normalTexture
                       == "characters/wayfarer/textures/sun normal.tga"
                       && relic.roughnessTexture
                       == "characters/wayfarer/textures/roughness.tga"
                       && relic.metallicTexture
                       == "characters/wayfarer/textures/metallic.tga"
                       && relic.occlusionTexture
                       == "characters/wayfarer/textures/occlusion.tga",
               "all supported PBR texture maps resolve relative to the MTL file");
        expect(near(relic.albedoMapTransform.uvScale[0], 0.5f)
                       && near(relic.albedoMapTransform.uvScale[1], 0.75f)
                       && near(relic.albedoMapTransform.uvOffset[0], 0.1f)
                       && near(relic.albedoMapTransform.uvOffset[1], 0.2f)
                       && near(relic.normalMapTransform.uvScale[0], 2.0f)
                       && near(relic.roughnessMapTransform.uvOffset[0], -0.2f)
                       && near(relic.roughnessMapTransform.uvOffset[1], 0.25f),
               "common per-map UV scale and offset options are parsed independently");
        expect(near(materials[1].material.roughness,
                    std::sqrt(2.0f / 20.0f)),
               "classic specular exponent maps to a PBR roughness fallback");
    }
}

void testInvalidMaterialsAndTraversalAreRejectedTransactionally() {
    std::vector<nuby::WavefrontMaterial> materials(1);
    materials[0].name = "keep";
    std::string error;
    const std::string invalidLibraries[] = {
        "newmtl Bad\nmap_Kd ../outside.tga\n",
        "newmtl Bad\nPr nan\n",
        "newmtl Duplicate\nnewmtl Duplicate\n",
        "newmtl Bad\nmap_Kd -clamp on texture.tga\n",
        "newmtl Bad\nmap_Kd -s 2000 2000 texture.tga\n"
    };
    for (const std::string& source : invalidLibraries) {
        expect(!nuby::parseMtl(source, "materials/model.mtl", materials, error),
               "unsafe paths and invalid or unsupported MTL data are rejected");
        expect(!error.empty() && materials.size() == 1 && materials[0].name == "keep",
               "failed MTL parse reports an error and preserves prior materials");
    }
    expect(!nuby::parseMtl("newmtl x\n", "../materials.mtl", materials, error),
           "MTL library paths cannot escape the project assets");
}

void testMalformedAndMissingMaterialRecordsAreRejected() {
    std::vector<nuby::WavefrontMaterial> materials;
    std::string error;
    expect(!nuby::parseMtl("Kd 1 1 1\n", "materials/file.mtl", materials, error),
           "material properties before the first newmtl are not accepted as a library");
    expect(!nuby::parseMtl("", "materials/file.mtl", materials, error),
           "empty MTL files are rejected");
    expect(!nuby::parseMtl("newmtl Color\nKd 0 1 1 0.5\n",
                   "materials/file.mtl", materials, error),
           "MTL color records must have exactly three finite values");
}
}  // namespace

int main() {
    testPackagedSampleMaterialLibrary();
    testPbrFactorsAndRelativeMapsParse();
    testInvalidMaterialsAndTraversalAreRejectedTransactionally();
    testMalformedAndMissingMaterialRecordsAreRejected();
    if (failures != 0) {
        std::cerr << failures << " MTL loader test(s) failed\n";
        return EXIT_FAILURE;
    }
    std::cout << "All MTL loader tests passed\n";
    return EXIT_SUCCESS;
}
