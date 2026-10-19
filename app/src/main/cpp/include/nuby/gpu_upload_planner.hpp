#pragma once

#include <cstdint>

namespace nuby {

struct BufferUploadChunk {
    std::uint64_t offset = 0U;
    std::uint64_t byteCount = 0U;
};

// Selects the next contiguous byte range for a buffer transfer. A successful
// plan always makes progress and never exceeds byteBudget.
bool planBufferUploadChunk(std::uint64_t totalBytes, std::uint64_t uploadedBytes,
                           std::uint64_t byteBudget, BufferUploadChunk& chunk);

}  // namespace nuby
