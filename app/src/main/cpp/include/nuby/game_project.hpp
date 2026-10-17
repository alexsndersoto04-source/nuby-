#pragma once

#include "nuby/scene.hpp"

#include <functional>
#include <istream>
#include <ostream>
#include <string>

namespace nuby {

// Metadata for one game project. Projects own their scenes and assets; the
// runtime engine only loads the selected project's startup scene.
struct GameProject {
    std::string displayName;
    std::string packageIdentifier;
    std::string startupScene;
};

using ProjectAssetReader = std::function<bool(const std::string& relativePath,
                                               std::string& contents,
                                               std::string& error)>;

class GameProjectLoader final {
public:
    static constexpr unsigned int kCurrentVersion = 1;

    // Loads a project descriptor and its startup scene transactionally: on
    // failure neither output object is changed. The asset callback receives
    // only a validated project-relative path.
    static bool load(std::istream& descriptor,
                     const ProjectAssetReader& readAsset,
                     GameProject& project,
                     Scene& scene,
                     std::string& error);

    static bool save(const GameProject& project, std::ostream& output,
                     std::string& error);
};

}  // namespace nuby
