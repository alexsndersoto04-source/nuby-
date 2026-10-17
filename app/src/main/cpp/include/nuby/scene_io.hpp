#pragma once

#include <iosfwd>
#include <string>

namespace nuby {
class Scene;

// Versioned, dependency-free text serialization for editor/game scene files.
class SceneSerializer final {
public:
    static constexpr unsigned int kCurrentVersion = 8;

    static bool save(const Scene& scene, std::ostream& output, std::string& error);
    static bool load(Scene& scene, std::istream& input, std::string& error);
};

}  // namespace nuby
