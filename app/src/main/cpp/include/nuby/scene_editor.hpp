#pragma once

#include "nuby/game_project_runtime.hpp"

#include <cstdint>
#include <string>

namespace nuby {

// Small platform-independent authoring session for the Android visual editor.
// It edits a real versioned project scene; rendering and file I/O remain host-owned.
class SceneEditorSession final {
public:
    bool createDefaultProject(std::string& error);
    bool loadProjectText(const std::string& descriptorText,
                         const std::string& startupSceneText,
                         std::string& error);
    bool saveProjectText(std::string& descriptorText,
                         std::string& startupSceneText,
                         std::string& error) const;
    void close();
    bool active() const { return runtime_.loaded(); }
    bool update(float deltaSeconds);
    Scene* scene() { return runtime_.scene(); }
    const Scene* scene() const { return runtime_.scene(); }
    const Entity* selectedEntity() const;
    bool selectEntity(std::uint32_t entityId);
    std::string selectionStatus() const;

    bool addPrimitive(Primitive primitive, std::string& error);
    bool selectNext(int direction);
    bool moveSelected(float x, float y, float z);
    bool rotateSelected(float x, float y, float z);
    bool scaleSelected(float factor);
    bool cycleSelectedColor();
    bool duplicateSelected(std::string& error);
    bool deleteSelected();

private:
    bool isEditable(const Entity& entity) const;
    Entity* selectedEntityMutable();
    std::uint32_t selectedEntityId_ = 0;
    GameProjectRuntime runtime_;
};

}  // namespace nuby
