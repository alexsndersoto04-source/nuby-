#include "nuby/scene.hpp"
#include "nuby/scene_io.hpp"

#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>

int main(int argc, char** argv) {
    if (argc != 2) {
        std::cerr << "usage: generate_ruin_zone_scenes <assets-directory>\n";
        return 2;
    }

    const std::filesystem::path outputDirectory =
            std::filesystem::path(argv[1]) / "scenes" / "zones";
    std::filesystem::create_directories(outputDirectory);
    for (int zone = 0; zone < 3; ++zone) {
        nuby::Scene fragment;
        const int firstPillar = zone * 2;
        for (int localIndex = 0; localIndex < 2; ++localIndex) {
            const int pillarIndex = firstPillar + localIndex;
            const float z = -2.0f - static_cast<float>(pillarIndex) * 2.25f;
            for (int side : {-1, 1}) {
                const int number = pillarIndex * 2 + (side > 0 ? 2 : 1);
                nuby::Entity& pillar = fragment.create(
                        "Ruin Zone " + std::to_string(zone + 1)
                                + " Pillar " + std::to_string(number),
                        nuby::Primitive::ImportedMesh);
                pillar.meshAsset = "meshes/ruin_pillar.obj";
                pillar.transform.position = {static_cast<float>(side) * 4.35f, -0.12f, z};
                pillar.transform.scale = {0.38f, 1.18f, 0.42f};
                pillar.material.color = {0.34f, 0.30f, 0.22f};
                pillar.material.roughness = 0.96f;
            }
        }

        const std::string fileName = "ruins_0" + std::to_string(zone + 1) + ".scene";
        std::ofstream output(outputDirectory / fileName, std::ios::binary);
        std::string error;
        if (!output || !nuby::SceneSerializer::save(fragment, output, error)) {
            std::cerr << "failed to write " << fileName << ": " << error << '\n';
            return 1;
        }
    }
    return 0;
}
