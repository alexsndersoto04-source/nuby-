#pragma once

// ============================================================================
// FORMATO DE FRAME DE NUBY (RLE propio, PackBits a nivel de píxel)
//
// Lo usan las DOS builds del motor:
//   • el servidor nativo (src/server/web_server.cpp) para enviar el frame al
//     "monitor" del visitante por HTTP;
//   • la build de navegador (wasm/nuby_wasm.cpp) para entregar el mismo frame
//     a la página que hospeda el motor.
//
// Header: [u32 w][u32 h][u32 len_datos]  (little endian)
// Datos:  RGB por píxel — el alfa se descarta: el frame ya está compuesto.
// ============================================================================

#include <cstdint>
#include <vector>

namespace nuby::server {

inline void rle_put_u32(std::vector<unsigned char>& out, uint32_t v) {
    out.push_back((unsigned char)(v & 0xFF));
    out.push_back((unsigned char)((v >> 8) & 0xFF));
    out.push_back((unsigned char)((v >> 16) & 0xFF));
    out.push_back((unsigned char)((v >> 24) & 0xFF));
}

inline std::vector<unsigned char> encode_rle(const std::vector<uint32_t>& px, int w, int h) {
    std::vector<unsigned char> out;
    rle_put_u32(out, (uint32_t)w);
    rle_put_u32(out, (uint32_t)h);
    rle_put_u32(out, 0); // longitud parcheada al final

    size_t total = px.size();
    size_t i = 0;
    while (i < total) {
        // ¿cuántos píxeles iguales siguen?
        size_t run = 1;
        while (i + run < total && px[i + run] == px[i] && run < 128) ++run;
        if (run >= 3) {
            out.push_back((unsigned char)(257 - run)); // 129..254 → 3..128 reps
            // framebuffer 0xAARRGGBB → mandamos R,G,B
            out.push_back((unsigned char)((px[i] >> 16) & 0xFF)); // R
            out.push_back((unsigned char)((px[i] >> 8) & 0xFF));  // G
            out.push_back((unsigned char)(px[i] & 0xFF));         // B
            i += run;
            continue;
        }
        // literal: agrupa hasta 128 píxeles no-repetidos
        size_t lit_start = i;
        size_t lit_count = 0;
        while (i < total && lit_count < 128) {
            size_t r2 = 1;
            while (i + r2 < total && px[i + r2] == px[i] && r2 < 128) ++r2;
            if (r2 >= 3) break;
            i += r2;
            lit_count += r2;
        }
        out.push_back((unsigned char)(lit_count - 1));
        for (size_t k = 0; k < lit_count; ++k) {
            uint32_t p = px[lit_start + k];
            out.push_back((unsigned char)((p >> 16) & 0xFF));
            out.push_back((unsigned char)((p >> 8) & 0xFF));
            out.push_back((unsigned char)(p & 0xFF));
        }
    }

    uint32_t len = (uint32_t)(out.size() - 12);
    out[8]  = (unsigned char)(len & 0xFF);
    out[9]  = (unsigned char)((len >> 8) & 0xFF);
    out[10] = (unsigned char)((len >> 16) & 0xFF);
    out[11] = (unsigned char)((len >> 24) & 0xFF);
    return out;
}

} // namespace nuby::server
