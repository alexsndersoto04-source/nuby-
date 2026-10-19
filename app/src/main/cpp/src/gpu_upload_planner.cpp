#include "nuby/gpu_upload_planner.hpp"

#include <algorithm>

namespace nuby {

bool planBufferUploadChunk(std::uint64_t totalBytes, std::uint64_t uploadedBytes,
                           std::uint64_t byteBudget, BufferUploadChunk& chunk) {
    chunk = {};
    if (totalBytes == 0U || uploadedBytes >= totalBytes || byteBudget == 0U) return false;
    chunk.offset = uploadedBytes;
    chunk.byteCount = std::min(totalBytes - uploadedBytes, byteBudget);
    return chunk.byteCount != 0U;
}

}  // namespace nuby
