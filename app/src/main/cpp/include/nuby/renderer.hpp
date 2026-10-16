#pragma once

#include <array>
#include <cstdint>
#include <string>
#include <unordered_map>
#include <vector>

#include <GLES3/gl3.h>
#include <android/asset_manager.h>

#include "nuby/obj_loader.hpp"
#include "nuby/scene.hpp"

namespace nuby {

class Renderer final {
public:
    Renderer() = default;
    ~Renderer() = default;

    void initialize(AAssetManager* assetManager);
    void resize(int width, int height);
    void render(float deltaSeconds);
    void touch(int action, float x, float y);
    bool shadowsEnabled() const { return shadowsAvailable_; }
    bool postProcessingEnabled() const { return postProcessingAvailable_; }
    bool sampleMeshLoaded() const {
        return importedMeshes_.find("meshes/orbital_shard.obj") != importedMeshes_.end();
    }

private:
    struct Mesh {
        GLuint vao = 0;
        GLuint vbo = 0;
        GLsizei vertexCount = 0;
    };

    struct PostUniforms {
        GLint extractScene = -1;
        GLint blurSource = -1;
        GLint blurDirection = -1;
        GLint blurTexelSize = -1;
        GLint compositeScene = -1;
        GLint compositeBloom = -1;
    };

    struct Uniforms {
        GLint viewProjection = -1;
        GLint model = -1;
        GLint color = -1;
        GLint emission = -1;
        GLint metallic = -1;
        GLint roughness = -1;
        GLint ambientOcclusion = -1;
        GLint cameraPosition = -1;
        GLint lightDirection = -1;
        GLint lightViewProjection = -1;
        GLint shadowMap = -1;
        GLint shadowsEnabled = -1;
        GLint grid = -1;
    };

    GLuint compileShader(GLenum type, const char* source);
    GLuint createProgram(const char* vertexSource, const char* fragmentSource);
    Mesh uploadMesh(const std::vector<MeshVertex>& vertices);
    bool loadObjAsset(const std::string& assetPath, Mesh& mesh);
    Mesh makeSphere(int latitudeSegments, int longitudeSegments);
    Mesh makeTorus(int ringSegments, int tubeSegments, float majorRadius, float minorRadius);
    Mesh makeGround();
    const Mesh* meshFor(const Entity& entity) const;
    void draw(const Mesh& mesh, const std::array<float, 16>& model,
              const Material& material);
    void buildScene();
    void initializeShadowMap();
    void renderShadowMap(const std::array<float, 16>& lightViewProjection);
    void initializePostProcessing();
    void resizePostProcessing();
    void releasePostProcessing();
    void renderBloom();
    void drawFullscreen();

    AAssetManager* assetManager_ = nullptr;
    std::unordered_map<std::string, Mesh> importedMeshes_;
    GLuint program_ = 0;
    GLuint shadowProgram_ = 0;
    GLuint shadowFramebuffer_ = 0;
    GLuint shadowDepthTexture_ = 0;
    GLuint bloomExtractProgram_ = 0;
    GLuint blurProgram_ = 0;
    GLuint compositeProgram_ = 0;
    GLuint sceneFramebuffer_ = 0;
    GLuint sceneColorTexture_ = 0;
    GLuint sceneDepthBuffer_ = 0;
    std::array<GLuint, 2> bloomFramebuffers_{};
    std::array<GLuint, 2> bloomTextures_{};
    GLuint fullscreenVao_ = 0;
    GLuint fullscreenVbo_ = 0;
    bool shadowsAvailable_ = false;
    bool postProcessingAvailable_ = false;
    int bloomWidth_ = 1;
    int bloomHeight_ = 1;
    int postWidth_ = 0;
    int postHeight_ = 0;
    PostUniforms postUniforms_{};
    GLint shadowProgramLightViewProjection_ = -1;
    GLint shadowProgramModel_ = -1;
    static constexpr int kShadowMapSize = 512;
    Uniforms uniforms_{};
    Mesh sphere_{};
    Mesh torus_{};
    Mesh ground_{};
    Scene scene_;
    int width_ = 1;
    int height_ = 1;
    float yaw_ = 0.45f;
    float pitch_ = 0.18f;
    float lastTouchX_ = 0.0f;
    float lastTouchY_ = 0.0f;
    std::array<float, 16> viewProjection_{};
    std::array<float, 16> lightViewProjection_{};
    std::array<float, 3> cameraPosition_{};
};

}  // namespace nuby
