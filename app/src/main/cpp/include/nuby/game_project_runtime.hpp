#pragma once

#include "nuby/game_project.hpp"

#include <iosfwd>
#include <string>

namespace nuby {

// Platform-independent lifecycle for one loaded game project. It owns only the
// selected project's data; Android asset access and rendering stay injected at
// the host boundary.
class GameProjectRuntime final {
public:
    bool load(std::istream& descriptor, const ProjectAssetReader& readAsset,
              std::string& error);
    void unload();
    bool update(float deltaSeconds);
    bool saveDescriptor(std::ostream& output, std::string& error) const;

    bool loaded() const { return loaded_; }
    const GameProject* project() const { return loaded_ ? &project_ : nullptr; }
    Scene* scene() { return loaded_ ? &scene_ : nullptr; }
    const Scene* scene() const { return loaded_ ? &scene_ : nullptr; }

private:
    GameProject project_;
    Scene scene_;
    bool loaded_ = false;
};

}  // namespace nuby
