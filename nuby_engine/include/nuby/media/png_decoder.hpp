#pragma once

// ============================================================================
// NUBY PNG DECODER — decodificador PNG escrito desde cero.
//
// Sin zlib, sin libpng, sin stb_image. Todo aquí está implementado a mano:
//   • RFC 1950 (contenedor zlib: cabecera CMF/FLG + Adler-32 VERIFICADO)
//   • RFC 1951 (DEFLATE: bloques almacenados, Huffman fijo y Huffman
//     dinámico — decodificador canónico por longitudes de código)
//   • RFC 2083 (PNG): chunks IHDR/PLTE/tRNS/IDAT/IEND, filtros 0-4
//     (None/Sub/Up/Average/Paeth), tipos de color 0,2,3,4,6,
//     profundidades 1,2,4,8 y 16 bits.
//
// Lo que NO hace (honesto, con error explícito):
//   • Entrelazado Adam7 → error "Adam7 aún no soportado"
//   • CRC de cada chunk no se verifica (se ignoran) — la imagen se valida
//     por estructura y Adler-32; decisión documentada.
// ============================================================================

#include "image.hpp"
#include <string>
#include <cstring>
#include <array>

namespace nuby::media {

// ---------------------------------------------------------------------------
// inflate DEFLATE (RFC 1951) — decodificador canónico estilo puff
// ---------------------------------------------------------------------------
namespace deflate {

struct BitSource {
    const unsigned char* data;
    size_t size;
    size_t pos{0};   // byte
    unsigned bit{0}; // bit dentro del byte (0..7)

    // lee `count` bits (LSB primero, como manda DEFLATE)
    int bits(int count) {
        if (count == 0) return 0;
        int val = 0;
        for (int i = 0; i < count; ++i) {
            if (pos >= size) return -1;
            val |= ((data[pos] >> bit) & 1) << i;
            if (++bit == 8) { bit = 0; ++pos; }
        }
        return val;
    }
};

// Tabla Huffman canónica: counts por longitud + símbolos ordenados
struct Huffman {
    std::array<unsigned short, 16> count{};
    std::array<unsigned short, 288> symbol{};

    // Decodifica UN código caminando bit a bit (algoritmo puff, correcto y simple)
    int decode(BitSource& s) const {
        int code = 0, first = 0, index = 0;
        for (int len = 1; len <= 15; ++len) {
            int b = s.bits(1);
            if (b < 0) return -1;
            code |= b;
            int cnt = count[len];
            if (code - cnt < first) return symbol[index + (code - first)];
            index += cnt;
            first = (first + cnt) << 1;
            code <<= 1;
        }
        return -2; // código inválido
    }
};

inline int build_huffman(Huffman& h, const unsigned char* lengths, int n) {
    h.count.fill(0);
    for (int i = 0; i < n; ++i) {
        if (lengths[i] > 15) return -1;
        h.count[lengths[i]]++;
    }
    if (h.count[0] == (unsigned short)n) return 0; // ningún código: válido si solo hay distancias vacías
    h.count[0] = 0;
    // offsets de símbolos
    int offs[16];
    offs[1] = 0;
    for (int len = 1; len < 15; ++len) offs[len + 1] = offs[len] + h.count[len];
    for (int i = 0; i < n; ++i)
        if (lengths[i] != 0) h.symbol[offs[lengths[i]]++] = i;
    return 0;
}

// Tablas de longitudes/distancias extra-bits (RFC 1951 §3.2.5)
static const short LEN_BASE[29] = {
    3,4,5,6,7,8,9,10,11,13,15,17,19,23,27,31,35,43,51,59,67,83,99,115,131,163,195,227,258};
static const short LEN_EXTRA[29] = {
    0,0,0,0,0,0,0,0,1,1,1,1,2,2,2,2,3,3,3,3,4,4,4,4,5,5,5,5,0};
static const short DIST_BASE[30] = {
    1,2,3,4,5,7,9,13,17,25,33,49,65,97,129,193,257,385,513,769,1025,1537,2049,3073,
    4097,6145,8193,12289,16385,24577};
static const short DIST_EXTRA[30] = {
    0,0,0,0,1,1,2,2,3,3,4,4,5,5,6,6,7,7,8,8,9,9,10,10,11,11,12,12,13,13};

// Infla `src` (flujo DEFLATE crudo) en `out`. Devuelve true si OK.
inline bool inflate(const unsigned char* src, size_t src_size,
                    std::vector<unsigned char>& out, std::string& err) {
    BitSource s{src, src_size, 0, 0};
    bool last = false;
    while (!last) {
        int bfinal = s.bits(1);
        int btype = s.bits(2);
        if (bfinal < 0 || btype < 0) { err = "deflate: cabecera de bloque truncada"; return false; }
        last = bfinal != 0;

        if (btype == 0) {
            // almacenado: alinear a byte y copiar LEN bytes
            if (s.bit) { s.bit = 0; ++s.pos; }
            if (s.pos + 4 > s.size) { err = "deflate: bloque almacenado truncado"; return false; }
            unsigned len = s.data[s.pos] | (s.data[s.pos+1] << 8);
            unsigned nlen = s.data[s.pos+2] | (s.data[s.pos+3] << 8);
            if ((len ^ 0xFFFF) != nlen) { err = "deflate: LEN/NLEN inconsistente"; return false; }
            s.pos += 4;
            if (s.pos + len > s.size) { err = "deflate: datos almacenados truncados"; return false; }
            out.insert(out.end(), src + s.pos, src + s.pos + len);
            s.pos += len;
        } else if (btype == 1 || btype == 2) {
            Huffman lit, dst;
            if (btype == 1) {
                // Huffman fijo
                unsigned char lengths[288];
                for (int i = 0; i < 144; ++i) lengths[i] = 8;
                for (int i = 144; i < 256; ++i) lengths[i] = 9;
                for (int i = 256; i < 280; ++i) lengths[i] = 7;
                for (int i = 280; i < 288; ++i) lengths[i] = 8;
                build_huffman(lit, lengths, 288);
                unsigned char dlens[30];
                for (int i = 0; i < 30; ++i) dlens[i] = 5;
                build_huffman(dst, dlens, 30);
            } else {
                // Huffman dinámico
                int hlit = s.bits(5) + 257;
                int hdist = s.bits(5) + 1;
                int hclen = s.bits(4) + 4;
                if (hlit > 286 || hdist > 30) { err = "deflate: HLIT/HDIST fuera de rango"; return false; }
                static const int ORDER[19] = {16,17,18,0,8,7,9,6,10,5,11,4,12,3,13,2,14,1,15};
                unsigned char cl_lengths[19]{};
                for (int i = 0; i < hclen; ++i) {
                    int v = s.bits(3);
                    if (v < 0) { err = "deflate: code-lengths truncados"; return false; }
                    cl_lengths[ORDER[i]] = (unsigned char)v;
                }
                Huffman cl;
                if (build_huffman(cl, cl_lengths, 19) != 0) { err = "deflate: code-lengths inválidos"; return false; }
                unsigned char lengths[288 + 30]{};
                int idx = 0;
                while (idx < hlit + hdist) {
                    int sym = cl.decode(s);
                    if (sym < 0) { err = "deflate: código de longitudes inválido"; return false; }
                    if (sym < 16) lengths[idx++] = (unsigned char)sym;
                    else {
                        int repeat = 0; unsigned char prev = 0;
                        if (sym == 16) {
                            if (idx == 0) { err = "deflate: repetición sin previo"; return false; }
                            prev = lengths[idx - 1];
                            repeat = 3 + s.bits(2);
                        } else if (sym == 17) repeat = 3 + s.bits(3);
                        else if (sym == 18) repeat = 11 + s.bits(7);
                        if (idx + repeat > hlit + hdist) { err = "deflate: repetición desbordada"; return false; }
                        for (int r = 0; r < repeat; ++r) lengths[idx++] = prev;
                    }
                }
                if (build_huffman(lit, lengths, hlit) != 0 ||
                    build_huffman(dst, lengths + hlit, hdist) != 0) {
                    err = "deflate: huffman dinámico inválido"; return false;
                }
            }

            // datos comprimidos
            for (;;) {
                int sym = lit.decode(s);
                if (sym < 0) { err = "deflate: literal/length inválido"; return false; }
                if (sym < 256) { out.push_back((unsigned char)sym); continue; }
                if (sym == 256) break; // fin de bloque
                sym -= 257;
                if (sym >= 29) { err = "deflate: símbolo de longitud fuera de rango"; return false; }
                int len = LEN_BASE[sym] + s.bits(LEN_EXTRA[sym]);
                int dsym = dst.decode(s);
                if (dsym < 0 || dsym >= 30) { err = "deflate: distancia inválida"; return false; }
                int dist = DIST_BASE[dsym] + s.bits(DIST_EXTRA[dsym]);
                if ((size_t)dist > out.size()) { err = "deflate: distancia antes del inicio"; return false; }
                size_t from = out.size() - dist;
                for (int k = 0; k < len; ++k) out.push_back(out[from + k]);
            }
        } else {
            err = "deflate: BTYPE=3 (reservado)"; return false;
        }
    }
    return true;
}

// zlib (RFC 1950): verifica FLG (incl. FCHECK) y Adler-32 del resultado
inline bool zlib_decompress(const unsigned char* src, size_t size,
                            std::vector<unsigned char>& out, std::string& err) {
    if (size < 6) { err = "zlib: demasiado corto"; return false; }
    unsigned cmf = src[0], flg = src[1];
    if ((cmf & 0x0F) != 8) { err = "zlib: método != deflate"; return false; }
    if ((cmf * 256 + flg) % 31 != 0) { err = "zlib: FCHECK inválido"; return false; }
    if (flg & 0x20) { err = "zlib: FDICT (preset dictionary) no soportado"; return false; }
    if (!inflate(src + 2, size - 6, out, err)) return false;
    // Adler-32 real del descomprimido
    uint32_t a = 1, b = 0;
    for (unsigned char c : out) { a = (a + c) % 65521u; b = (b + a) % 65521u; }
    uint32_t adler = (b << 16) | a;
    uint32_t stored = (uint32_t(src[size-4]) << 24) | (uint32_t(src[size-3]) << 16) |
                      (uint32_t(src[size-2]) << 8) | uint32_t(src[size-1]);
    if (adler != stored) { err = "zlib: Adler-32 no coincide (datos corruptos)"; return false; }
    return true;
}

} // namespace deflate

// ---------------------------------------------------------------------------
// Decodificador PNG (RFC 2083)
// ---------------------------------------------------------------------------
class PngDecoder {
public:
    // Decodifica `data` (bytes del archivo) en `out`. false + error si no se pudo.
    static bool decode(const std::string& data, Image& out, std::string& err) {
        static const unsigned char SIG[8] = {137, 80, 78, 71, 13, 10, 26, 10};
        if (data.size() < 8 || memcmp(data.data(), SIG, 8) != 0) {
            err = "no es PNG (firma ausente)"; return false;
        }
        size_t pos = 8;
        int width = 0, height = 0, depth = 0, ctype = 0, interlace = 0;
        std::vector<unsigned char> palette, pal_alpha, idat;

        while (pos + 8 <= data.size()) {
            uint32_t len = be32(data, pos);
            std::string type = data.substr(pos + 4, 4);
            pos += 8;
            if (pos + len + 4 > data.size()) { err = "chunk truncado: " + type; return false; }
            const unsigned char* body = (const unsigned char*)data.data() + pos;

            if (type == "IHDR") {
                if (len != 13) { err = "IHDR de tamaño inválido"; return false; }
                width = (int)be32(data, pos);
                height = (int)be32(data, pos + 4);
                depth = body[8]; ctype = body[9];
                if (body[10] != 0) { err = "compresión PNG != 0"; return false; }
                if (body[11] != 0) { err = "filtro de método PNG != 0"; return false; }
                interlace = body[12];
                if (interlace == 1) { err = "PNG entrelazado (Adam7) aún no soportado"; return false; }
                if (interlace != 0) { err = "interlace PNG inválido"; return false; }
            } else if (type == "PLTE") {
                palette.assign(body, body + len);
            } else if (type == "tRNS") {
                pal_alpha.assign(body, body + len);
            } else if (type == "IDAT") {
                idat.insert(idat.end(), body, body + len);
            } else if (type == "IEND") {
                break;
            }
            pos += len + 4; // + CRC (no se verifica — decisión documentada arriba)
        }

        if (width <= 0 || height <= 0) { err = "sin IHDR"; return false; }
        if (width > 16384 || height > 16384) { err = "dimensiones absurdas (>16384)"; return false; }
        if (idat.empty()) { err = "sin IDAT"; return false; }

        int channels = channels_of(ctype);
        if (channels == 0) { err = "tipo de color PNG inválido: " + std::to_string(ctype); return false; }
        bool depth_ok = (depth == 8 || depth == 16) ||
                        (ctype == 3 && (depth == 1 || depth == 2 || depth == 4)) ||
                        (ctype == 0 && (depth == 1 || depth == 2 || depth == 4));
        if (!depth_ok) {
            err = "profundidad " + std::to_string(depth) + " no válida para tipo " + std::to_string(ctype);
            return false;
        }

        std::vector<unsigned char> raw;
        if (!deflate::zlib_decompress(idat.data(), idat.size(), raw, err)) return false;

        // bytes por pixel (para filtros) mínimo 1; y bytes por línea
        int bpp = std::max(1, (channels * depth) / 8);
        size_t stride = ((size_t)width * channels * depth + 7) / 8;
        if (raw.size() < (stride + 1) * (size_t)height) {
            err = "datos descomprimidos insuficientes (imagen truncada)";
            return false;
        }

        // ---- des-filtrado real (filtros 0..4 con Paeth) ----
        std::vector<unsigned char> recon(stride * (size_t)height);
        for (int y = 0; y < height; ++y) {
            const unsigned char* src = raw.data() + (size_t)y * (stride + 1) + 1;
            unsigned filter = raw[(size_t)y * (stride + 1)];
            unsigned char* cur = recon.data() + (size_t)y * stride;
            const unsigned char* prev = y ? recon.data() + (size_t)(y - 1) * stride : nullptr;
            if (filter > 4) { err = "filtro PNG inválido: " + std::to_string(filter); return false; }
            for (size_t x = 0; x < stride; ++x) {
                int a = x >= (size_t)bpp ? cur[x - bpp] : 0;
                int b = prev ? prev[x] : 0;
                int c = (prev && x >= (size_t)bpp) ? prev[x - bpp] : 0;
                int v = src[x];
                switch (filter) {
                    case 0: cur[x] = (unsigned char)v; break;
                    case 1: cur[x] = (unsigned char)(v + a); break;
                    case 2: cur[x] = (unsigned char)(v + b); break;
                    case 3: cur[x] = (unsigned char)(v + ((a + b) >> 1)); break;
                    case 4: cur[x] = (unsigned char)(v + paeth(a, b, c)); break;
                }
            }
        }

        // ---- a RGBA 8-bit ----
        out.width = width;
        out.height = height;
        out.pixels.assign((size_t)width * height, 0xFF000000u);

        for (int y = 0; y < height; ++y) {
            const unsigned char* row = recon.data() + (size_t)y * stride;
            for (int x = 0; x < width; ++x) {
                uint8_t r = 0, g = 0, bl = 0, al = 255;
                if (depth == 8 || depth == 16) {
                    const unsigned char* p = row + (size_t)x * channels * (depth / 8);
                    auto sample = [&](int ch) { return depth == 8 ? p[ch] : p[ch * 2]; }; // MSB en 16-bit
                    switch (ctype) {
                        case 0: r = g = bl = sample(0); break;
                        case 2: r = sample(0); g = sample(1); bl = sample(2); break;
                        case 3: { // paleta 8-bit
                            unsigned idx = p[0];
                            if (idx * 3 + 2 >= palette.size()) { err = "índice de paleta fuera de rango"; return false; }
                            r = palette[idx*3]; g = palette[idx*3+1]; bl = palette[idx*3+2];
                            if (idx < pal_alpha.size()) al = pal_alpha[idx];
                            break;
                        }
                        case 4: r = g = bl = sample(0); al = sample(1); break;
                        case 6: r = sample(0); g = sample(1); bl = sample(2); al = sample(3); break;
                    }
                } else {
                    // 1/2/4 bits: paleta (tipo 3) o gris (tipo 0), empaquetado MSB
                    size_t bitpos = (size_t)x * depth;
                    unsigned byte = row[bitpos / 8];
                    unsigned shift = 8 - depth - (bitpos % 8);
                    unsigned idx = (byte >> shift) & ((1u << depth) - 1);
                    if (ctype == 3) {
                        if (idx * 3 + 2 >= palette.size()) { err = "índice de paleta fuera de rango"; return false; }
                        r = palette[idx*3]; g = palette[idx*3+1]; bl = palette[idx*3+2];
                        if (idx < pal_alpha.size()) al = pal_alpha[idx];
                    } else {
                        uint8_t gray = (uint8_t)(idx * 255 / ((1u << depth) - 1));
                        r = g = bl = gray;
                    }
                }
                out.pixels[(size_t)y * width + x] =
                    (uint32_t(al) << 24) | (uint32_t(r) << 16) | (uint32_t(g) << 8) | bl;
            }
        }
        return true;
    }

private:
    static uint32_t be32(const std::string& d, size_t p) {
        return (uint32_t)(unsigned char)d[p] << 24 | (uint32_t)(unsigned char)d[p+1] << 16 |
               (uint32_t)(unsigned char)d[p+2] << 8 | (uint32_t)(unsigned char)d[p+3];
    }
    static int channels_of(int ctype) {
        switch (ctype) {
            case 0: return 1; case 2: return 3; case 3: return 1;
            case 4: return 2; case 6: return 4;
        }
        return 0;
    }
    static int paeth(int a, int b, int c) {
        int p = a + b - c;
        int pa = std::abs(p - a), pb = std::abs(p - b), pc = std::abs(p - c);
        return (pa <= pb && pa <= pc) ? a : (pb <= pc ? b : c);
    }
};

} // namespace nuby::media
