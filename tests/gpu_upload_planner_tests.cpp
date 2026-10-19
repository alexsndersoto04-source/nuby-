#include "nuby/gpu_upload_planner.hpp"

#include <cstdlib>
#include <iostream>

namespace {
int failures = 0;

void expect(bool condition, const char* message) {
    if (!condition) {
        std::cerr << "FAIL: " << message << '\n';
        ++failures;
    }
}

void testLargeVertexAndIndexBuffersRespectPerTurnBudget() {
    constexpr std::uint64_t budget = 512U * 1024U;
    constexpr std::uint64_t vertexBytes = 1U * 1024U * 1024U + 137U;
    constexpr std::uint64_t indexBytes = 300U * 1024U + 9U;
    std::uint64_t vertexOffset = 0U;
    std::uint64_t indexOffset = 0U;
    std::size_t turns = 0U;
    while (vertexOffset < vertexBytes || indexOffset < indexBytes) {
        std::uint64_t remaining = budget;
        nuby::BufferUploadChunk chunk;
        if (vertexOffset < vertexBytes) {
            expect(nuby::planBufferUploadChunk(vertexBytes, vertexOffset, remaining, chunk),
                   "pending vertex buffers always produce a progressing chunk");
            expect(chunk.offset == vertexOffset && chunk.byteCount <= remaining,
                   "vertex chunks are contiguous and budget bounded");
            vertexOffset += chunk.byteCount;
            remaining -= chunk.byteCount;
        }
        if (remaining != 0U && indexOffset < indexBytes) {
            expect(nuby::planBufferUploadChunk(indexBytes, indexOffset, remaining, chunk),
                   "pending index buffers use the unused frame budget");
            expect(chunk.offset == indexOffset && chunk.byteCount <= remaining,
                   "index chunks are contiguous and share the same frame budget");
            indexOffset += chunk.byteCount;
            remaining -= chunk.byteCount;
        }
        expect(remaining <= budget, "per-turn buffer transfer never exceeds its cap");
        ++turns;
        expect(turns < 10U, "large buffer transfer completes in a bounded number of turns");
    }
    expect(vertexOffset == vertexBytes && indexOffset == indexBytes && turns == 3U,
           "vertex and index ranges complete exactly without gaps or duplicate bytes");
}

void testInvalidAndExhaustedBufferRangesAreRejected() {
    nuby::BufferUploadChunk chunk{9U, 9U};
    expect(!nuby::planBufferUploadChunk(0U, 0U, 512U, chunk)
                   && chunk.offset == 0U && chunk.byteCount == 0U,
           "zero-sized buffer has no upload chunk");
    expect(!nuby::planBufferUploadChunk(100U, 100U, 512U, chunk),
           "fully uploaded buffer has no redundant transfer");
    expect(!nuby::planBufferUploadChunk(100U, 0U, 0U, chunk),
           "zero upload budget cannot advance a buffer");
    expect(!nuby::planBufferUploadChunk(100U, 101U, 10U, chunk),
           "out-of-range upload cursor is rejected");
    expect(nuby::planBufferUploadChunk(100U, 90U, 512U, chunk)
                   && chunk.offset == 90U && chunk.byteCount == 10U,
           "final chunk is clipped exactly to the remaining bytes");
}
}  // namespace

int main() {
    testLargeVertexAndIndexBuffersRespectPerTurnBudget();
    testInvalidAndExhaustedBufferRangesAreRejected();
    if (failures != 0) {
        std::cerr << failures << " GPU upload planner test(s) failed\n";
        return EXIT_FAILURE;
    }
    std::cout << "All GPU upload planner tests passed\n";
    return EXIT_SUCCESS;
}
