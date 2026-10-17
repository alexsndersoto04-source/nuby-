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
                       && material.normalTexture == "meshes/orbital_shard_normal.tga"
                       && material.opacityTexture == "meshes/orbital_shard_opacity.tga"
                       && material.opacityMapChannel == nuby::ScalarTextureChannel::Matte
                       && material.opacityMapTransform.clampToEdge
                       && near(material.opacityMapTransform.uvScale[0], 1.25f)
                       && near(material.opacityMapTransform.uvOffset[0], 0.05f)
                       && near(material.opacityMapValueRange.base, 0.02f)
                       && near(material.opacityMapValueRange.gain, 0.96f),
               "sample MTL resolves matte UV, wrap and base/gain settings");
        expect(near(material.albedoMapTransform.uvScale[0], 1.25f)
                       && near(material.albedoMapTransform.uvOffset[0], 0.05f)
                       && material.albedoMapTransform.clampToEdge
                       && near(material.normalMapTransform.uvScale[1], 1.25f)
                       && near(material.normalMapTransform.uvOffset[0], 0.05f),
               "packaged sample exercises per-map scale and offset options");
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
d 0.65
map_Kd -mm -0.1 1.3 -s 0.5 0.75 1 -o 0.1 0.2 0 textures/sun albedo.tga
norm -mm -0.05 1.1 -s 2 2 textures/sun normal.tga
map_d -imfchan m -mm 0.2 0.7 -s 0.75 0.8 1 -o 0.2 0.1 0 -clamp on textures/sun opacity.tga
map_Pr -mm 0.03 0.9 -o -0.2 0.25 textures/roughness.tga
map_Pm -mm -0.02 1.2 -clamp on textures/metallic.tga
map_AO -mm 0.1 0.8 -clamp off textures/occlusion.tga

newmtl Stone
Kd 0.34 0.31 0.27
Ns 18
Tr 0.2
map_d textures/stone opacity.tga
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
                       && near(relic.metallic, 0.72f) && near(relic.roughness, 0.31f)
                       && near(relic.opacity, 0.65f)
                       && near(materials[1].material.opacity, 0.8f),
               "diffuse, emissive, PBR and dissolve/transparency factors are parsed");
        expect(relic.albedoTexture
                       == "characters/wayfarer/textures/sun albedo.tga"
                       && relic.normalTexture
                       == "characters/wayfarer/textures/sun normal.tga"
                       && relic.roughnessTexture
                       == "characters/wayfarer/textures/roughness.tga"
                       && relic.metallicTexture
                       == "characters/wayfarer/textures/metallic.tga"
                       && relic.occlusionTexture
                       == "characters/wayfarer/textures/occlusion.tga"
                       && relic.opacityTexture
                               == "characters/wayfarer/textures/sun opacity.tga"
                       && materials[1].material.opacityTexture
                               == "characters/wayfarer/textures/stone opacity.tga",
               "all supported PBR and opacity texture paths resolve relative to the MTL");
        expect(relic.opacityMapChannel == nuby::ScalarTextureChannel::Matte
                       && materials[1].material.opacityMapChannel
                               == nuby::ScalarTextureChannel::Luminance
                       && near(materials[1].material.opacityMapValueRange.base, 0.0f)
                       && near(materials[1].material.opacityMapValueRange.gain, 1.0f)
                       && relic.opacityMapTransform.clampToEdge
                       && near(relic.opacityMapTransform.uvScale[0], 0.75f)
                       && near(relic.opacityMapTransform.uvOffset[1], 0.1f)
                       && near(relic.opacityMapValueRange.base, 0.2f)
                       && near(relic.opacityMapValueRange.gain, 0.7f),
               "opacity channel, UV/wrap and value range settings are parsed independently");
        expect(relic.metallicMapTransform.clampToEdge
                       && !relic.occlusionMapTransform.clampToEdge
                       && !relic.albedoMapTransform.clampToEdge,
               "MTL -clamp on/off sampling modes are parsed per map");
        expect(near(relic.albedoMapTransform.uvScale[0], 0.5f)
                       && near(relic.albedoMapTransform.uvScale[1], 0.75f)
                       && near(relic.albedoMapTransform.uvOffset[0], 0.1f)
                       && near(relic.albedoMapTransform.uvOffset[1], 0.2f)
                       && near(relic.normalMapTransform.uvScale[0], 2.0f)
                       && near(relic.albedoMapValueRange.base, -0.1f)
                       && near(relic.albedoMapValueRange.gain, 1.3f)
                       && near(relic.normalMapValueRange.base, -0.05f)
                       && near(relic.normalMapValueRange.gain, 1.1f)
                       && near(relic.roughnessMapTransform.uvOffset[0], -0.2f)
                       && near(relic.roughnessMapTransform.uvOffset[1], 0.25f)
                       && near(relic.roughnessMapValueRange.base, 0.03f)
                       && near(relic.roughnessMapValueRange.gain, 0.9f)
                       && near(relic.metallicMapValueRange.base, -0.02f)
                       && near(relic.metallicMapValueRange.gain, 1.2f)
                       && near(relic.occlusionMapValueRange.base, 0.1f)
                       && near(relic.occlusionMapValueRange.gain, 0.8f),
               "UV settings and base/gain ranges parse independently for every supported MTL map");
        expect(near(materials[1].material.roughness,
                    std::sqrt(2.0f / 20.0f)),
               "classic specular exponent maps to a PBR roughness fallback");
    }
}

void testEverySupportedOpacityChannelParses() {
    constexpr char source[] = R"MTL(
newmtl Red
map_d -imfchan r red.tga
newmtl Green
map_d -imfchan g green.tga
newmtl Blue
map_d -imfchan b blue.tga
newmtl Matte
map_d -imfchan m matte.tga
newmtl Luminance
map_d -imfchan l luminance.tga
)MTL";
    std::vector<nuby::WavefrontMaterial> materials;
    std::string error;
    expect(nuby::parseMtl(source, "materials/channels.mtl", materials, error),
           "all supported scalar texture channels parse");
    expect(materials.size() == 5, "all opacity channel fixture materials are retained");
    if (materials.size() == 5) {
        for (std::size_t index = 0; index < materials.size(); ++index) {
            expect(static_cast<unsigned int>(materials[index].material.opacityMapChannel)
                           == index,
                   "opacity map channel enum follows the supported Wavefront channel order");
        }
    }
}

void testInvalidMaterialsAndTraversalAreRejectedTransactionally() {
    std::vector<nuby::WavefrontMaterial> materials(1);
    materials[0].name = "keep";
    std::string error;
    const std::string invalidLibraries[] = {
        "newmtl Bad\nmap_Kd ../outside.tga\n",
        "newmtl Bad\nPr nan\n",
        "newmtl Bad\nd nan\n",
        "newmtl Bad\nTr 1.1\n",
        "newmtl Bad\nmap_d -imfchan z opacity.tga\n",
        "newmtl Bad\nmap_d ../outside.tga\n",
        "newmtl Duplicate\nnewmtl Duplicate\n",
        "newmtl Bad\nmap_Kd -blendu on texture.tga\n",
        "newmtl Bad\nmap_Kd -imfchan r texture.tga\n",
        "newmtl Bad\nmap_Kd -clamp sideways texture.tga\n",
        "newmtl Bad\nmap_Kd -s 2000 2000 texture.tga\n",
        "newmtl Bad\nmap_Kd -mm nan 1 texture.tga\n",
        "newmtl Bad\nmap_d -mm 0 17 mask.tga\n"
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
    expect(!nuby::parseMtl("map_d alpha.tga\n", "materials/file.mtl", materials, error),
           "opacity maps before the first material are rejected");
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
    testEverySupportedOpacityChannelParses();
    testInvalidMaterialsAndTraversalAreRejectedTransactionally();
    testMalformedAndMissingMaterialRecordsAreRejected();
    if (failures != 0) {
        std::cerr << failures << " MTL loader test(s) failed\n";
        return EXIT_FAILURE;
    }
    std::cout << "All MTL loader tests passed\n";
    return EXIT_SUCCESS;
}
