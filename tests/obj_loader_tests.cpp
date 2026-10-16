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
        expect(near(vertices[2].uv[0], 1.0f) && near(vertices[2].uv[1], 1.0f),
               "UV attribute is preserved");
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
}
}  // namespace

int main() {
    testQuadTriangulationAndAttributes();
    testNegativeIndicesAndGeneratedNormals();
    testInvalidIndexIsRejected();
    testPackagedSampleAsset();
    if (failures != 0) {
        std::cerr << failures << " OBJ loader test(s) failed\n";
        return EXIT_FAILURE;
    }
    std::cout << "All OBJ loader tests passed\n";
    return EXIT_SUCCESS;
}
