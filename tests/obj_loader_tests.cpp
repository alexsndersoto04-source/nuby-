#include "nuby/obj_loader.hpp"

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

bool near(float a, float b, float epsilon = 0.001f) {
    return std::fabs(a - b) <= epsilon;
}

void testQuadTriangulationAndAttributes() {
    constexpr char obj[] = R"(
        v 0 0 0
        v 1 0 0
        v 1 1 0
        v 0 1 0
        vt 0 0
        vt 1 0
        vt 1 1
        vt 0 1
        vn 0 0 1
        f 1/1/1 2/2/1 3/3/1 4/4/1
    )";
    std::vector<nuby::MeshVertex> vertices;
    std::string error;
    expect(nuby::parseObj(obj, vertices, error), "quad OBJ parses successfully");
    expect(vertices.size() == 6, "quad is triangulated into six vertices");
    if (vertices.size() == 6) {
        expect(near(vertices[2].position[0], 1.0f), "position attribute is preserved");
        bool pairedUvFound = false;
        for (const nuby::MeshVertex& vertex : vertices) {
            if (near(vertex.position[0], 1.0f) && near(vertex.position[1], 1.0f)
                    && near(vertex.uv[0], 1.0f) && near(vertex.uv[1], 1.0f)) {
                pairedUvFound = true;
            }
        }
        expect(pairedUvFound, "UV attribute remains paired with its source position");
        expect(near(vertices[0].normal[2], 1.0f), "authored normal is preserved");
    }
}

void testNegativeIndicesAndGeneratedNormals() {
    constexpr char obj[] = R"(
        v 0 0 0
        v 1 0 0
        v 0 1 0
        f -3 -2 -1
    )";
    std::vector<nuby::MeshVertex> vertices;
    std::string error;
    expect(nuby::parseObj(obj, vertices, error), "negative OBJ indices parse");
    expect(vertices.size() == 3, "triangle remains three expanded vertices");
    if (vertices.size() == 3) {
        expect(near(vertices[0].normal[2], 1.0f), "normal is generated for a face without normals");
    }
}

double signedTriangleAreaZ(const nuby::MeshVertex& a,
                           const nuby::MeshVertex& b,
                           const nuby::MeshVertex& c) {
    return 0.5 * ((b.position[0] - a.position[0]) * (c.position[1] - a.position[1])
                - (b.position[1] - a.position[1]) * (c.position[0] - a.position[0]));
}

void testConcavePolygonEarClippingAndWinding() {
    constexpr char counterClockwise[] = R"OBJ(
        v 0 0 0
        v 2 0 0
        v 2 2 0
        v 1 1 0
        v 0 2 0
        f 1 2 3 4 5
    )OBJ";
    constexpr char clockwise[] = R"OBJ(
        v 0 0 0
        v 2 0 0
        v 2 2 0
        v 1 1 0
        v 0 2 0
        f 5 4 3 2 1
    )OBJ";
    std::vector<nuby::MeshVertex> vertices;
    std::string error;
    expect(nuby::parseObj(counterClockwise, vertices, error),
           "concave OBJ polygon is triangulated successfully");
    expect(vertices.size() == 9, "concave pentagon produces three triangles");
    double area = 0.0;
    for (std::size_t i = 0; i + 2 < vertices.size(); i += 3) {
        area += signedTriangleAreaZ(vertices[i], vertices[i + 1], vertices[i + 2]);
    }
    expect(near(static_cast<float>(area), 3.0f),
           "concave triangulation preserves total signed area without overlap");

    expect(nuby::parseObj(clockwise, vertices, error),
           "concave polygons with reversed winding remain supported");
    area = 0.0;
    for (std::size_t i = 0; i + 2 < vertices.size(); i += 3) {
        area += signedTriangleAreaZ(vertices[i], vertices[i + 1], vertices[i + 2]);
    }
    expect(near(static_cast<float>(area), -3.0f),
           "triangulation preserves the original clockwise winding");
}

void testSmoothingGroupsGenerateAreaWeightedVertexNormals() {
    constexpr char smooth[] = R"OBJ(
        v 0 0 0
        v 1 0 0
        v 0 1 0
        v 0 0 1
        s on
        f 1 2 3
        s 01
        f 1 4 2
    )OBJ";
    constexpr char flat[] = R"OBJ(
        v 0 0 0
        v 1 0 0
        v 0 1 0
        v 0 0 1
        s off
        f 1 2 3
        s 0
        f 1 4 2
    )OBJ";
    std::vector<nuby::MeshVertex> vertices;
    std::string error;
    expect(nuby::parseObj(smooth, vertices, error),
           "on and numeric OBJ smoothing groups parse");
    if (vertices.size() == 6) {
        const float diagonal = std::sqrt(0.5f);
        for (std::size_t index : {0U, 3U}) {
            expect(near(vertices[index].normal[0], 0.0f)
                           && near(vertices[index].normal[1], diagonal)
                           && near(vertices[index].normal[2], diagonal),
                   "shared positions in one smoothing group receive averaged normals");
        }
    }

    expect(nuby::parseObj(flat, vertices, error), "s off and s 0 retain flat shading");
    if (vertices.size() == 6) {
        expect(near(vertices[0].normal[1], 0.0f) && near(vertices[0].normal[2], 1.0f)
                       && near(vertices[3].normal[1], 1.0f)
                       && near(vertices[3].normal[2], 0.0f),
               "disabled smoothing uses independent per-face normals");
    }

    constexpr char separateGroups[] = R"OBJ(
        v 0 0 0
        v 1 0 0
        v 0 1 0
        v 0 0 1
        s 1
        f 1 2 3
        s 2
        f 1 4 2
    )OBJ";
    expect(nuby::parseObj(separateGroups, vertices, error),
           "different smoothing groups remain independently generated");
    if (vertices.size() == 6) {
        expect(near(vertices[0].normal[2], 1.0f)
                       && near(vertices[3].normal[1], 1.0f),
               "separate group IDs do not blend neighboring face normals");
    }

    constexpr char explicitNormal[] = R"OBJ(
        v 0 0 0
        v 1 0 0
        v 0 1 0
        v 0 0 1
        vn 1 0 0
        s 1
        f 1//1 2 3
        f 1 4 2
    )OBJ";
    expect(nuby::parseObj(explicitNormal, vertices, error),
           "authored normals coexist with generated smoothing normals");
    if (vertices.size() == 6) {
        expect(near(vertices[0].normal[0], 1.0f)
                       && near(vertices[0].normal[1], 0.0f)
                       && near(vertices[0].normal[2], 0.0f),
               "explicit OBJ normal is preserved instead of smoothed over");
    }
}

void testInvalidSmoothingGroupsAreRejected() {
    std::vector<nuby::MeshVertex> vertices;
    std::string error;
    expect(!nuby::parseObj("s 0 extra\nv 0 0 0\nv 1 0 0\nv 0 1 0\nf 1 2 3\n",
                           vertices, error),
           "smoothing records with extra fields are rejected");
    expect(!nuby::parseObj("s smooth\nv 0 0 0\nv 1 0 0\nv 0 1 0\nf 1 2 3\n",
                           vertices, error),
           "nonnumeric smoothing group identifiers are rejected");
}

void testInvalidPolygonBoundariesAreRejected() {
    constexpr char degenerate[] = "v 0 0 0\nv 1 1 0\nv 2 2 0\nf 1 2 3\n";
    constexpr char selfIntersecting[] =
            "v 0 0 0\nv 2 2 0\nv 0 2 0\nv 2 0 0\nf 1 2 3 4\n";
    std::vector<nuby::MeshVertex> vertices;
    std::string error;
    expect(!nuby::parseObj(degenerate, vertices, error),
           "zero-area polygons are rejected instead of generating unstable normals");
    expect(!nuby::parseObj(selfIntersecting, vertices, error),
           "self-intersecting polygon boundaries are rejected");
}

void testInvalidIndexIsRejected() {
    constexpr char obj[] = R"(
        v 0 0 0
        v 1 0 0
        v 0 1 0
        f 1 2 9
    )";
    std::vector<nuby::MeshVertex> vertices;
    std::string error;
    expect(!nuby::parseObj(obj, vertices, error), "out-of-range OBJ index is rejected");
    expect(vertices.empty(), "failed parse does not return partial geometry");
    expect(!error.empty(), "failed parse reports a useful error");
}

void testObjectAndGroupNamesArePreservedOnSubmeshes() {
    constexpr char obj[] = R"OBJ(
        o Ship Hull
        g Front Plates
        v 0 0 0
        v 1 0 0
        v 0 1 0
        v 0 0 1
        f 1 2 3
        g Rear Plates
        f 1 4 2
        o Sail
        g Fabric
        f 1 3 4
    )OBJ";
    nuby::ObjModel model;
    std::string error;
    expect(nuby::parseObjModel(obj, model, error),
           "OBJ object and group declarations parse");
    expect(model.submeshes.size() == 3,
           "object/group boundaries create separately named submesh ranges");
    if (model.submeshes.size() == 3) {
        expect(model.submeshes[0].objectName == "Ship Hull"
                       && model.submeshes[0].groupName == "Front Plates"
                       && model.submeshes[1].objectName == "Ship Hull"
                       && model.submeshes[1].groupName == "Rear Plates"
                       && model.submeshes[2].objectName == "Sail"
                       && model.submeshes[2].groupName == "Fabric",
               "object and group names are retained for each material range");
    }
}

void testMalformedNumbersAndUnsupportedLibraryPathsAreRejected() {
    nuby::ObjModel model;
    std::string error;
    expect(!nuby::parseObjModel("v nan 0 0\nv 1 0 0\nv 0 1 0\nf 1 2 3\n",
                               model, error),
           "non-finite OBJ coordinates are rejected");
    expect(!nuby::parseObjModel("mtllib ../outside.mtl\n", model, error),
           "parent traversal in a material library reference is rejected");
    expect(!nuby::parseObjModel("mtllib -o 1 1 1\n", model, error),
           "unsupported mtllib options are rejected explicitly");
}

void testMaterialLibrariesAndSubmeshAssignments() {
    constexpr char obj[] = R"OBJ(
        mtllib materials/relics.mtl common.mtl
        v 0 0 0
        v 1 0 0
        v 0 1 0
        v 0 0 1
        usemtl Sun Relic
        f 1 2 3
        usemtl Stone
        f 1 4 2
        usemtl Sun Relic
        f 1 3 4
    )OBJ";
    nuby::ObjModel model;
    std::string error;
    expect(nuby::parseObjModel(obj, model, error),
           "OBJ material library references and assignments parse");
    expect(model.materialLibraries.size() == 2
                   && model.materialLibraries[0] == "materials/relics.mtl"
                   && model.materialLibraries[1] == "common.mtl",
           "OBJ retains multiple unique MTL references in source order");
    expect(model.submeshes.size() == 3,
           "usemtl boundaries retain separate contiguous submeshes");
    if (model.submeshes.size() == 3) {
        expect(model.submeshes[0].materialName == "Sun Relic"
                       && model.submeshes[1].materialName == "Stone"
                       && model.submeshes[2].materialName == "Sun Relic",
               "material names including spaces map to their face ranges");
        expect(model.submeshes[0].vertices.size() == 3
                       && model.submeshes[1].vertices.size() == 3
                       && model.submeshes[2].vertices.size() == 3,
               "every material range contains its own expanded triangles");
    }

    expect(!nuby::parseObjModel("mtllib ../outside.mtl\nv 0 0 0\n", model, error),
           "OBJ MTL references cannot traverse outside the asset directory");
    expect(model.submeshes.empty() && model.materialLibraries.empty(),
           "failed OBJ parse leaves no partial model data");
}

void testPackagedSampleAsset() {
    std::ifstream file("app/src/main/assets/meshes/orbital_shard.obj");
    expect(file.good(), "packaged orbital shard asset exists");
    if (!file.good()) return;

    const std::string source((std::istreambuf_iterator<char>(file)),
                             std::istreambuf_iterator<char>());
    std::vector<nuby::MeshVertex> vertices;
    std::string error;
    expect(nuby::parseObj(source, vertices, error), "packaged sample mesh parses");
    expect(vertices.size() == 24, "sample shard contains eight triangulated faces");

    nuby::ObjModel model;
    expect(nuby::parseObjModel(source, model, error),
           "packaged sample OBJ preserves material ranges");
    expect(model.materialLibraries.size() == 1
                   && model.materialLibraries[0] == "orbital_shard.mtl"
                   && model.submeshes.size() == 1
                   && model.submeshes[0].materialName == "ShardMetal",
           "packaged sample references its MTL and assigns its named material");
    std::ifstream library("app/src/main/assets/meshes/orbital_shard.mtl");
    expect(library.good(), "packaged orbital shard material library exists");

    std::ifstream pillarFile("app/src/main/assets/meshes/ruin_pillar.obj");
    expect(pillarFile.good(), "zone scene pillar resource is packaged as an OBJ asset");
    if (pillarFile.good()) {
        const std::string pillarSource((std::istreambuf_iterator<char>(pillarFile)),
                                       std::istreambuf_iterator<char>());
        nuby::ObjModel pillarModel;
        expect(nuby::parseObjModel(pillarSource, pillarModel, error)
                       && !pillarModel.submeshes.empty()
                       && !pillarModel.submeshes[0].vertices.empty(),
               "the zone pillar OBJ resource parses into renderable geometry");
        expect(pillarModel.materialLibraries.size() == 1
                       && pillarModel.materialLibraries[0] == "ruin_pillar.mtl"
                       && pillarModel.submeshes[0].materialName == "AgedStone",
               "the zone pillar resource references its standalone stone material");
    }
}
}  // namespace

int main() {
    testQuadTriangulationAndAttributes();
    testNegativeIndicesAndGeneratedNormals();
    testConcavePolygonEarClippingAndWinding();
    testSmoothingGroupsGenerateAreaWeightedVertexNormals();
    testInvalidSmoothingGroupsAreRejected();
    testInvalidPolygonBoundariesAreRejected();
    testInvalidIndexIsRejected();
    testObjectAndGroupNamesArePreservedOnSubmeshes();
    testMalformedNumbersAndUnsupportedLibraryPathsAreRejected();
    testMaterialLibrariesAndSubmeshAssignments();
    testPackagedSampleAsset();
    if (failures != 0) {
        std::cerr << failures << " OBJ loader test(s) failed\n";
        return EXIT_FAILURE;
    }
    std::cout << "All OBJ loader tests passed\n";
    return EXIT_SUCCESS;
}
