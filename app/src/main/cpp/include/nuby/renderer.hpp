#pragma once

#include <array>
#include <cstdint>
#include <string>
#include <unordered_map>
#include <vector>

#include <GLES3/gl3.h>
#include <android/asset_manager.h>

#include "nuby/adventure_game.hpp"
#include "nuby/frustum.hpp"
#include "nuby/game_project_runtime.hpp"
#include "nuby/gltf_loader.hpp"
#include "nuby/image_loader.hpp"
#include "nuby/mtl_loader.hpp"
#include "nuby/obj_loader.hpp"
#include "nuby/sample_game.hpp"
#include "nuby/scene.hpp"
#include "nuby/shadow_cascades.hpp"

namespace nuby {

class Renderer final {
public:
    Renderer() = default;
    ~Renderer();

    void initialize(AAssetManager* assetManager);
    void resize(int width, int height);
    void render(float deltaSeconds);
    void touch(int action, float x, float y, float pinchDistance, int pointerCount);
    void setGameMode(bool enabled);
    void setDemoMode(int mode);
    void setAdventureMoveInput(float horizontal, float forward);
    void jumpAdventureCharacter();
    void restartGame();
    bool loadProject(const std::string& descriptorAssetPath);
    void unloadProject();
    bool projectLoaded() const { return projectRuntime_.loaded(); }
    std::string projectStatusText() const;
    std::string gameStatusText() const;
    bool shadowsEnabled() const { return shadowsAvailable_; }
    bool postProcessingEnabled() const { return postProcessingAvailable_; }
    int rendererTier() const {
        return fallbackRenderer_ ? 0 : csmShaderSupported_ ? 2 : 1;
    }
    // 0 = sample OBJ is not referenced by the active scene, 1 = fallback,
    // 2 = mesh loaded, 3 = mesh, MTL material and referenced maps loaded.
    int sampleAssetStatus() const;

private:
    struct Mesh {
        GLuint vao = 0;
        GLuint vbo = 0;
        GLuint ebo = 0;
        GLsizei vertexCount = 0;
        GLsizei indexCount = 0;
        std::array<float, 3> boundsCenter{0.0f, 0.0f, 0.0f};
        float boundsRadius = 0.0f;
        bool boundsValid = false;
    };

    struct ImportedPart {
        Mesh mesh;
        std::string objectName;
        std::string groupName;
        Material material;
        bool hasMaterial = false;
        bool skinned = false;
        bool alphaMask = false;
        float alphaCutoff = 0.5f;
    };

    struct ImportedModel {
        std::vector<ImportedPart> parts;
        std::vector<SkeletalJoint> skeleton;
        std::vector<GltfAnimation> animations;
    };

    struct DrawItem {
        const Mesh* mesh = nullptr;
        std::array<float, 16> model{};
        const Material* material = nullptr;
        std::size_t materialSortKey = 0;
        float cameraDistanceSquared = 0.0f;
        const SkeletalPose* pose = nullptr;
        bool alphaMask = false;
        float alphaCutoff = 0.5f;
    };

    struct ShadowDrawItem {
        const Mesh* mesh = nullptr;
        std::array<float, 16> model{};
        const SkeletalPose* pose = nullptr;
        const Material* material = nullptr;
        bool alphaMask = false;
        float alphaCutoff = 0.5f;
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
        GLint useInstancing = -1;
        GLint skinningEnabled = -1;
        GLint color = -1;
        GLint emission = -1;
        GLint metallic = -1;
        GLint roughness = -1;
        GLint opacity = -1;
        GLint alphaMaskEnabled = -1;
        GLint alphaCutoff = -1;
        GLint ambientOcclusion = -1;
        GLint ambientSphericalHarmonics = -1;
        GLint albedoTexture = -1;
        GLint normalTexture = -1;
        GLint metallicRoughnessTexture = -1;
        GLint metallicTexture = -1;
        GLint roughnessTexture = -1;
        GLint occlusionTexture = -1;
        GLint opacityTexture = -1;
        GLint emissiveTexture = -1;
        GLint hasAlbedoTexture = -1;
        GLint hasNormalTexture = -1;
        GLint hasMetallicRoughnessTexture = -1;
        GLint hasMetallicTexture = -1;
        GLint hasRoughnessTexture = -1;
        GLint hasOcclusionTexture = -1;
        GLint hasOpacityTexture = -1;
        GLint hasEmissiveTexture = -1;
        GLint opacityMapChannel = -1;
        GLint emissiveUvScale = -1;
        GLint emissiveUvOffset = -1;
        GLint opacityUvScale = -1;
        GLint opacityUvOffset = -1;
        std::array<GLint, 7> textureUvScale{};
        std::array<GLint, 7> textureUvOffset{};
        std::array<GLint, 8> textureMapValueRange{};
        GLint cameraPosition = -1;
        GLint lightDirection = -1;
        GLint view = -1;
        std::array<GLint, kDirectionalShadowCascadeCount> lightViewProjection{};
        GLint cascadeFarSplits = -1;
        GLint shadowMap = -1;
        GLint shadowsEnabled = -1;
        GLint grid = -1;
        GLint pointLightCount = -1;
        GLint pointLightPositionRange = -1;
        GLint pointLightColorIntensity = -1;
    };

    GLuint compileShader(GLenum type, const char* source);
    GLuint createProgram(const char* vertexSource, const char* fragmentSource);
    Mesh uploadMesh(const std::vector<MeshVertex>& vertices);
    Mesh uploadSkinnedMesh(const GltfPrimitive& primitive);
    Mesh uploadStaticGltfMesh(const GltfPrimitive& primitive);
    bool loadObjAsset(const std::string& assetPath, ImportedModel& model);
    bool loadGlbAsset(const std::string& assetPath, ImportedModel& model);
    void uploadSkinPalette(const SkeletalPose* pose);
    bool loadImageAsset(const std::string& assetPath, GLuint& texture);
    Mesh makeSphere(int latitudeSegments, int longitudeSegments);
    Mesh makeTorus(int ringSegments, int tubeSegments, float majorRadius, float minorRadius);
    Mesh makeGround();
    const Mesh* meshFor(const Entity& entity) const;
    void draw(const Mesh& mesh, const std::array<float, 16>& model,
              const Material& material,
              const std::vector<std::array<float, 16>>* instanceModels = nullptr,
              const SkeletalPose* pose = nullptr,
              bool alphaMask = false, float alphaCutoff = 0.5f);
    bool meshIntersectsFrustum(const Mesh& mesh, const std::array<float, 16>& model,
                               const ViewFrustum& frustum) const;
    void buildScene();
    void loadSceneAssets();
    void synchronizeSkeletalClipDurations(Scene& scene);
    void releaseSceneAssets();
    Scene& activeScene();
    const Scene& activeScene() const;
    std::string projectAssetPath(const std::string& relativePath) const;
    void initializeShadowMap();
    void renderShadowMaps();
    void initializePostProcessing();
    void resizePostProcessing();
    void releasePostProcessing();
    void renderBloom();
    void drawFullscreen();

    AAssetManager* assetManager_ = nullptr;
    std::unordered_map<std::string, ImportedModel> importedMeshes_;
    std::unordered_map<std::uint32_t, SkeletalPose> evaluatedSkeletalPoses_;
    std::unordered_map<std::string, GLuint> textures_;
    std::unordered_map<std::string, std::string> embeddedImageData_;
    std::vector<DrawItem> opaqueDrawItems_;
    std::vector<DrawItem> transparentDrawItems_;
    std::vector<ShadowDrawItem> shadowDrawItems_;
    std::vector<std::array<float, 16>> instanceModelsScratch_;
    GLuint program_ = 0;
    GLuint repeatSampler_ = 0;
    GLuint clampSampler_ = 0;
    GLuint shadowProgram_ = 0;
    GLuint instanceBuffer_ = 0;
    GLuint skinPaletteBuffer_ = 0;
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
    bool shadowCascadesValid_ = false;
    bool postProcessingAvailable_ = false;
    bool csmShaderSupported_ = false;
    bool fallbackRenderer_ = false;
    int bloomWidth_ = 1;
    int bloomHeight_ = 1;
    int postWidth_ = 0;
    int postHeight_ = 0;
    PostUniforms postUniforms_{};
    GLint shadowProgramLightViewProjection_ = -1;
    GLint shadowProgramModel_ = -1;
    GLint shadowProgramUseInstancing_ = -1;
    GLint shadowProgramSkinningEnabled_ = -1;
    GLint shadowProgramAlphaMaskEnabled_ = -1;
    GLint shadowProgramAlphaCutoff_ = -1;
    GLint shadowProgramBaseColorAlpha_ = -1;
    GLint shadowProgramAlbedoTexture_ = -1;
    GLint shadowProgramHasAlbedoTexture_ = -1;
    GLint shadowProgramAlbedoUvScale_ = -1;
    GLint shadowProgramAlbedoUvOffset_ = -1;
    static constexpr int kMaximumGpuJoints = 64;
    static constexpr GLuint kSkinPaletteBindingPoint = 0;
    static constexpr int kShadowMapSize = 512;
    static constexpr int kMaxPointLights = 4;
    Uniforms uniforms_{};
    std::vector<PointLightData> pointLights_;
    std::array<float, 27> ambientSphericalHarmonics_{};
    std::array<float, kMaxPointLights * 4> pointLightPositionRanges_{};
    std::array<float, kMaxPointLights * 4> pointLightColorIntensities_{};
    Mesh sphere_{};
    Mesh torus_{};
    Mesh ground_{};
    Scene scene_;
    GameProjectRuntime projectRuntime_;
    std::string projectAssetPrefix_;
    std::string projectLoadError_;
    SampleGame sampleGame_;
    AdventureGame adventureGame_;
    int demoMode_ = 0;
    bool gameMode_ = false;
    bool adventureMode_ = false;
    bool baseMeshesInitialized_ = false;
    int width_ = 1;
    int height_ = 1;
    float lastTouchX_ = 0.0f;
    float lastTouchY_ = 0.0f;
    float lastPinchDistance_ = 0.0f;
    float gameSwipeDistance_ = 0.0f;
    std::array<float, 16> viewProjection_{};
    std::array<float, 16> view_{};
    std::array<ShadowMatrix4, kDirectionalShadowCascadeCount> shadowViewProjections_{};
    std::array<float, 3> shadowCascadeFarSplits_{};
    std::array<float, 3> cameraPosition_{};
};

}  // namespace nuby
