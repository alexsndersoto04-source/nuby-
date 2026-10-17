#include "nuby/game_project_runtime.hpp"

#include <cmath>
#include <istream>
#include <ostream>
#include <utility>

namespace nuby {

bool GameProjectRuntime::load(std::istream& descriptor,
                              const ProjectAssetReader& readAsset,
                              std::string& error) {
    GameProject candidateProject;
    Scene candidateScene;
    if (!GameProjectLoader::load(descriptor, readAsset,
                                 candidateProject, candidateScene, error)) {
        return false;
    }

    project_ = std::move(candidateProject);
    scene_ = std::move(candidateScene);
    loaded_ = true;
    error.clear();
    return true;
}

void GameProjectRuntime::unload() {
    project_ = GameProject{};
    scene_.clear();
    loaded_ = false;
}

bool GameProjectRuntime::update(float deltaSeconds) {
    if (!loaded_ || !std::isfinite(deltaSeconds) || deltaSeconds < 0.0f) return false;
    scene_.update(deltaSeconds);
    return true;
}

bool GameProjectRuntime::saveDescriptor(std::ostream& output,
                                        std::string& error) const {
    if (!loaded_) {
        error = "No game project is loaded";
        return false;
    }
    return GameProjectLoader::save(project_, output, error);
}

}  // namespace nuby
