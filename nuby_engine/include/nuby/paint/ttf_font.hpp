#pragma once

// ============================================================================
// NUBY TRUE TYPE RASTERIZER — texto suave de verdad, sin librerías.
//
// Por qué existe: la fuente interna del motor era un bitmap 8x12 ESCALADO
// -> bordes de sierra incluso con antialiasing ("años 80", dijo el usuario).
// Esta clase lee fuentes TrueType REALES (.ttf, formato Apple/Microsoft) y
// rasteriza sus curvas cuadráticas Bezier con relleno por regla devanado +
// supersampling 3x3. Resultado: texto suave de verdad (como el reference).
//
// Qué implementa (estándar TrueType, todo a mano):
//   • Tablas: head, hhea, maxp, hmtx, loca, glyf, cmap (formato 4 BMP)
//   • Glifos simples: contornos con puntos on/off-curve + flags repetidos
//   • Glifos COMPUESTOS: componentes con offset XY, escala y anclaje por
//     puntos (así salen á é í ó ú ñ ü, compuestas en DejaVu)
//   • Aplanado de curvas cuadráticas a polígonos
//   • Relleno nonzero-winding con 9 muestras por píxel (cobertura real)
//   • Avances por glifo desde hmtx (tipografía proporcional real)
// No usa FreeType, ni stb, ni nada externo. Si el .ttf no existe, el motor
// cae atrás a su bitmap interno (nunca rompe).
// ============================================================================

#include <cstdint>
#include <cstring>
#include <cmath>
#include <string>
#include <vector>
#include <fstream>
#include <algorithm>

namespace nuby::paint {

class TrueTypeFont {
public:
    struct Glyph {
        int w{0}, h{0};          // tamaño del bitmap alfa (8 bpp)
        int ox{0}, oy{0};        // desplazamiento respecto al origen del glifo
        float advance{0.0f};     // avance en píxeles (ya escalado)
        std::vector<uint8_t> alpha;
        bool ok() const { return w > 0 && h > 0 && !alpha.empty(); }
    };

    struct RasterResult {
        Glyph g;
        float advance{0.0f};
    };

    bool load(const std::string& path) {
        std::ifstream f(path, std::ios::binary);
        if (!f) return false;
        data_.assign(std::istreambuf_iterator<char>(f), std::istreambuf_iterator<char>());
        if (data_.size() < 12) return false;

        uint32_t tag = rd32(0);
        if (tag != 0x00010000 && tag != 0x74727565 /*'true'*/) return false;

        int num_tables = rd16(4);
        uint32_t head = 0, hhea = 0, maxp = 0, hmtx = 0, loca = 0, glyf = 0, cmap = 0;
        for (int i = 0; i < num_tables; ++i) {
            size_t off = 12 + (size_t)i * 16;
            if (off + 16 > data_.size()) return false;
            uint32_t t = rd32(off), to = rd32(off + 8);
            switch (t) {
                case 0x68656164: head = to; break; // 'head'
                case 0x68686561: hhea = to; break; // 'hhea'
                case 0x6D617870: maxp = to; break; // 'maxp'
                case 0x686D7478: hmtx = to; break; // 'hmtx'
                case 0x6C6F6361: loca = to; break; // 'loca'
                case 0x676C7966: glyf = to; break; // 'glyf'
                case 0x636D6170: cmap = to; break; // 'cmap'
                default: break;
            }
        }
        if (!head || !hhea || !maxp || !hmtx || !loca || !glyf || !cmap) return false;

        units_per_em_ = rd16(head + 18);
        loca_long_ = rd16s(head + 50) == 1;
        num_glyphs_ = rd16(maxp + 4);
        ascent_em_ = rd16s(hhea + 4);
        descent_em_ = rd16s(hhea + 6);
        num_hmetrics_ = rd16(hhea + 34);
        glyf_base_ = glyf; loca_base_ = loca; hmtx_base_ = hmtx;
        if (!parse_cmap(cmap)) return false;
        if (units_per_em_ <= 0 || num_glyphs_ <= 0) return false;
        loaded_ = true;
        return true;
    }

    bool loaded() const { return loaded_; }

    // Avance REAL del glifo en píxeles (hmtx, proporcional — no uniforme)
    float advance(uint32_t cp, float font_size) const {
        if (!loaded_) return 0.0f;
        int gid = glyph_index(cp);
        size_t off;
        if (gid < num_hmetrics_) off = hmtx_base_ + (size_t)gid * 4;
        else off = hmtx_base_ + (size_t)(num_hmetrics_ - 1) * 4;
        if (off + 2 > data_.size()) return font_size * 0.5f;
        return rd16(off) * (font_size / (float)units_per_em_);
    }

    float ascent_px(float font_size) const {
        return ascent_em_ * (font_size / (float)units_per_em_);
    }

    // ---------------- Rasterización REAL a alfa 8bpp ------------------------
    RasterResult raster(uint32_t cp, float font_size) const {
        RasterResult rr;
        if (!loaded_ || font_size < 4.0f) return rr;
        int gid = glyph_index(cp);
        float sc = font_size / (float)units_per_em_;

        GlyphGeom geom;
        build_glyph_geom(gid, geom, 0);
        if (geom.x0 >= geom.x1 || geom.y0 >= geom.y1) {
            // glifo vacío (espacio): ok con avance y cero píxeles
            rr.advance = advance(cp, font_size);
            return rr;
        }

        // Aplanar contornos a polígonos en espacio EM (y hacia arriba)
        std::vector<std::vector<Ptf>> polys;
        int total_pts = 0;
        for (auto& c : geom.contours) total_pts += (int)c.size();
        polys.reserve(geom.contours.size());
        for (auto& c : geom.contours) flatten_contour(c, polys);
        if (polys.empty()) { rr.advance = advance(cp, font_size); return rr; }

        // BBox en píxeles (origen: baseline). Padding 1 px por el AA.
        int px0 = (int)std::floor(geom.x0 * sc) - 1;
        int px1 = (int)std::ceil(geom.x1 * sc) + 1;
        int py_top = (int)std::ceil(geom.y1 * sc) + 1;   // arriba (y mayor)
        int py_bot = (int)std::floor(geom.y0 * sc) - 1;  // abajo (y menor)
        int gw = px1 - px0, gh = py_top - py_bot;
        if (gw <= 0 || gh <= 0 || gw > 4096 || gh > 4096) return rr;

        rr.g.w = gw; rr.g.h = gh;
        rr.g.ox = px0;          // x respecto al origen del glifo
        rr.g.oy = -py_top;      // y del borde superior respecto a la baseline
        rr.g.alpha.assign((size_t)gw * gh, 0);
        rr.g.advance = advance(cp, font_size);
        rr.advance = rr.g.advance;

        // Relleno por regla de devanado (nonzero winding), 3x3 supersampling.
        // Cada muestra evalúa el cruce de rayos sobre TODAS las aristas.
        for (int py = 0; py < gh; ++py) {
            for (int px = 0; px < gw; ++px) {
                int cov = 0;
                for (int sy = 0; sy < 3; ++sy) {
                    for (int sx = 0; sx < 3; ++sx) {
                        float ex = px0 + px + (sx + 0.5f) / 3.0f;   // px coords
                        float ey = py_top - py - (sy + 0.5f) / 3.0f; // y hacia arriba
                        int winding = 0;
                        for (auto& poly : polys) {
                            size_t n = poly.size();
                            for (size_t i = 0; i < n; ++i) {
                                float ax = poly[i].x * sc, ay = poly[i].y * sc;
                                float bx = poly[(i + 1) % n].x * sc, by = poly[(i + 1) % n].y * sc;
                                if (ay <= ey) {
                                    if (by > ey && is_left(ax, ay, bx, by, ex, ey) > 0) ++winding;
                                } else {
                                    if (by <= ey && is_left(ax, ay, bx, by, ex, ey) < 0) --winding;
                                }
                            }
                        }
                        if (winding != 0) ++cov;
                    }
                }
                if (cov) rr.g.alpha[(size_t)py * gw + px] = (uint8_t)(cov * 255 / 9);
            }
        }
        return rr;
    }

private:
    struct Pt { float x, y; bool on; };
    struct Ptf { float x, y; };
    struct GlyphGeom {
        std::vector<std::vector<Pt>> contours;
        float x0{0}, y0{0}, x1{0}, y1{0};
    };

    std::vector<uint8_t> data_;
    bool loaded_{false};
    int units_per_em_{0}, num_glyphs_{0}, num_hmetrics_{0};
    int ascent_em_{0}, descent_em_{0};
    bool loca_long_{false};
    uint32_t glyf_base_{0}, loca_base_{0}, hmtx_base_{0};

    // cmap formato 4
    std::vector<uint16_t> seg_end_, seg_start_, seg_delta_, seg_ro_off_;
    size_t seg_ro_base_{0};

    // ---------- lectura big-endian ----------
    uint16_t rd16(size_t o) const {
        return (uint16_t)((data_[o] << 8) | data_[o + 1]);
    }
    int16_t rd16s(size_t o) const { return (int16_t)rd16(o); }
    uint32_t rd32(size_t o) const {
        return ((uint32_t)data_[o] << 24) | ((uint32_t)data_[o + 1] << 16) |
               ((uint32_t)data_[o + 2] << 8) | data_[o + 3];
    }

    bool parse_cmap(uint32_t cmap_off) {
        if (cmap_off + 4 > data_.size()) return false;
        int ntables = rd16(cmap_off + 2);
        for (int i = 0; i < ntables; ++i) {
            size_t rec = cmap_off + 4 + (size_t)i * 8;
            uint16_t platform = rd16(rec), encoding = rd16(rec + 2);
            uint32_t sub = rd32(rec + 4);
            if (cmap_off + sub + 2 > data_.size()) continue;
            uint16_t fmt = rd16(cmap_off + sub);
            // Preferimos BMP (3,1)/(3,0)/(0,*) formato 4
            if (fmt != 4) continue;
            if (platform == 3 || platform == 0) {
                size_t o = cmap_off + sub;
                int seg_count = rd16(o + 6) / 2;
                size_t end_base = o + 14;
                size_t start_base = end_base + (size_t)seg_count * 2 + 2;
                size_t delta_base = start_base + (size_t)seg_count * 2;
                size_t ro_base = delta_base + (size_t)seg_count * 2;
                if (ro_base + (size_t)seg_count * 2 > data_.size()) return false;
                seg_end_.clear(); seg_start_.clear(); seg_delta_.clear(); seg_ro_off_.clear();
                for (int s = 0; s < seg_count; ++s) {
                    seg_end_.push_back(rd16(end_base + (size_t)s * 2));
                    seg_start_.push_back(rd16(start_base + (size_t)s * 2));
                    seg_delta_.push_back(rd16(delta_base + (size_t)s * 2));
                    seg_ro_off_.push_back(rd16(ro_base + (size_t)s * 2));
                }
                seg_ro_base_ = ro_base;
                return true;
            }
        }
        return false;
    }

    int glyph_index(uint32_t cp) const {
        if (cp > 0xFFFF || seg_end_.empty()) return 0;
        size_t lo = 0, hi = seg_end_.size();
        // el segmento cuyo endCode >= cp (primero)
        while (lo < hi) {
            size_t mid = (lo + hi) / 2;
            if (seg_end_[mid] < cp) lo = mid + 1; else hi = mid;
        }
        if (lo >= seg_end_.size()) return 0;
        if (seg_start_[lo] > cp) return 0;
        uint16_t delta = seg_delta_[lo], ro = seg_ro_off_[lo];
        if (ro == 0) return (cp + delta) & 0xFFFF;
        size_t addr = seg_ro_base_ + lo * 2 + ro + 2 * (cp - seg_start_[lo]);
        if (addr + 2 > data_.size()) return 0;
        uint16_t gid = rd16(addr);
        if (gid != 0) gid = (gid + delta) & 0xFFFF;
        return gid;
    }

    uint32_t glyph_offset(int gid) const {
        if (loca_long_) return rd32(loca_base_ + (size_t)gid * 4);
        return rd16(loca_base_ + (size_t)gid * 2) * 2;
    }

    // Construye contornos del glifo (resuelve compuestos, profundidad tope 4)
    void build_glyph_geom(int gid, GlyphGeom& out, int depth) const {
        if (gid <= 0 || gid >= num_glyphs_ || depth > 4) return;
        uint32_t off = glyph_offset(gid), next = glyph_offset(gid + 1);
        if (next <= off) return; // vacío
        size_t o = glyf_base_ + off;
        if (o + 10 > data_.size()) return;
        int16_t ncontours = rd16s(o);
        float gx0 = rd16s(o + 2), gy0 = rd16s(o + 4), gx1 = rd16s(o + 6), gy1 = rd16s(o + 8);
        if (out.contours.empty()) { out.x0 = gx0; out.y0 = gy0; out.x1 = gx1; out.y1 = gy1; }
        else {
            out.x0 = std::min(out.x0, gx0); out.y0 = std::min(out.y0, gy0);
            out.x1 = std::max(out.x1, gx1); out.y1 = std::max(out.y1, gy1);
        }

        if (ncontours > 0) {
            // -------- glifo SIMPLE --------
            size_t p = o + 10;
            std::vector<uint16_t> ends(ncontours);
            for (int i = 0; i < ncontours; ++i) { ends[i] = rd16(p); p += 2; }
            uint16_t instr_len = rd16(p); p += 2 + instr_len; // instrucciones: fuera
            int total_pts = ends[ncontours - 1] + 1;
            // flags (con repeticiones)
            std::vector<uint8_t> flags(total_pts);
            for (int i = 0; i < total_pts;) {
                if (p >= data_.size()) return;
                uint8_t fl = data_[p++];
                flags[i++] = fl;
                if (fl & 0x08) {
                    if (p >= data_.size()) return;
                    int rep = data_[p++];
                    for (int r = 0; r < rep && i < total_pts; ++r) flags[i++] = fl;
                }
            }
            // coordenadas X (deltas)
            std::vector<float> xs(total_pts), ys(total_pts);
            float acc = 0;
            for (int i = 0; i < total_pts; ++i) {
                uint8_t fl = flags[i];
                int16_t ax = 0;
                if (fl & 0x02) {
                    int v = (p < data_.size()) ? data_[p++] : 0;
                    ax = (fl & 0x10) ? v : -v;
                } else if (!(fl & 0x10)) {
                    if (p + 2 <= data_.size()) { ax = rd16s(p); p += 2; }
                }
                acc += ax; xs[i] = acc;
            }
            acc = 0;
            for (int i = 0; i < total_pts; ++i) {
                uint8_t fl = flags[i];
                int16_t ay = 0;
                if (fl & 0x04) {
                    int v = (p < data_.size()) ? data_[p++] : 0;
                    ay = (fl & 0x20) ? v : -v;
                } else if (!(fl & 0x20)) {
                    if (p + 2 <= data_.size()) { ay = rd16s(p); p += 2; }
                }
                acc += ay; ys[i] = acc;
            }
            int start = 0;
            for (int c = 0; c < ncontours; ++c) {
                std::vector<Pt> contour;
                for (int i = start; i <= ends[c]; ++i)
                    contour.push_back({xs[i], ys[i], (flags[i] & 0x01) != 0});
                if (contour.size() >= 2) out.contours.push_back(contour);
                start = ends[c] + 1;
            }
        } else if (ncontours < 0) {
            // -------- glifo COMPUESTO --------
            size_t p = o + 10;
            bool more = true;
            while (more && p + 4 <= data_.size()) {
                uint16_t fl = rd16(p); uint16_t comp_gid = rd16(p + 2); p += 4;
                float dx = 0, dy = 0, scx = 1.0f, scy = 1.0f, sc01 = 0.0f, sc10 = 0.0f;
                int arg1 = 0, arg2 = 0;
                bool args_words = (fl & 0x0001) != 0;
                bool args_xy = (fl & 0x0002) != 0;
                if (args_words) {
                    if (p + 4 > data_.size()) return;
                    arg1 = rd16s(p); arg2 = rd16s(p + 2); p += 4;
                } else {
                    if (p + 2 > data_.size()) return;
                    arg1 = (int8_t)data_[p]; arg2 = (int8_t)data_[p + 1]; p += 2;
                }
                if (fl & 0x0008) { // WE_HAVE_A_SCALE
                    float f2_14 = rd16s(p) / 16384.0f; p += 2;
                    scx = scy = f2_14;
                } else if (fl & 0x0040) { // X_AND_Y_SCALE
                    scx = rd16s(p) / 16384.0f; scy = rd16s(p + 2) / 16384.0f; p += 4;
                } else if (fl & 0x0080) { // TWO_BY_TWO
                    scx = rd16s(p) / 16384.0f; sc01 = rd16s(p + 2) / 16384.0f;
                    sc10 = rd16s(p + 4) / 16384.0f; scy = rd16s(p + 6) / 16384.0f; p += 8;
                }
                GlyphGeom comp;
                build_glyph_geom(comp_gid, comp, depth + 1);
                if (args_xy) { dx = (float)arg1; dy = (float)arg2; }
                else if (!comp.contours.empty() && !out.contours.empty()) {
                    // Anclaje por puntos: arg1 = punto del padre, arg2 = del componente
                    Ptf pp{}, cc_{};
                    if (point_by_index(out, arg1, pp) && point_by_index(comp, arg2, cc_)) {
                        dx = pp.x - cc_.x; dy = pp.y - cc_.y;
                    }
                }
                for (auto& contour : comp.contours) {
                    std::vector<Pt> tc;
                    for (auto& q : contour) {
                        float nx = scx * q.x + sc01 * q.y + dx;
                        float ny = sc10 * q.x + scy * q.y + dy;
                        tc.push_back({nx, ny, q.on});
                    }
                    out.contours.push_back(tc);
                }
                more = (fl & 0x0020) != 0; // MORE_COMPONENTS
            }
        }
    }

    bool point_by_index(const GlyphGeom& g, int idx, Ptf& out) const {
        int i = 0;
        for (auto& c : g.contours) {
            for (auto& q : c) { if (i == idx) { out = {q.x, q.y}; return true; } ++i; }
        }
        return false;
    }

    // Aplana un contorno TrueType (on/off curve) a un polígono
    void flatten_contour(const std::vector<Pt>& c, std::vector<std::vector<Ptf>>& polys) const {
        std::vector<Ptf> poly;
        size_t n = c.size();
        // Punto inicial efectivo
        Ptf start;
        if (c[0].on) start = {c[0].x, c[0].y};
        else if (c[n - 1].on) start = {c[n - 1].x, c[n - 1].y};
        else start = {(c[0].x + c[n - 1].x) / 2, (c[0].y + c[n - 1].y) / 2};
        poly.push_back(start);
        Ptf cur = start;
        for (size_t i = 0; i < n; ++i) {
            const Pt& p1 = c[i];
            if (p1.on) {
                if (!(p1.x == cur.x && p1.y == cur.y)) poly.push_back({p1.x, p1.y});
                cur = {p1.x, p1.y};
            } else {
                // curva cuadrática: control p1, destino = siguiente on (o medio)
                Ptf end;
                const Pt& p2 = c[(i + 1) % n];
                if (p2.on) end = {p2.x, p2.y};
                else end = {(p1.x + p2.x) / 2, (p1.y + p2.y) / 2};
                const int STEPS = 10;
                for (int s = 1; s <= STEPS; ++s) {
                    float t = (float)s / STEPS, u = 1 - t;
                    poly.push_back({
                        u * u * cur.x + 2 * u * t * p1.x + t * t * end.x,
                        u * u * cur.y + 2 * u * t * p1.y + t * t * end.y });
                }
                cur = end;
            }
        }
        if (poly.size() >= 3) polys.push_back(poly);
    }

    static float is_left(float ax, float ay, float bx, float by, float px, float py) {
        return (bx - ax) * (py - ay) - (px - ax) * (by - ay);
    }
};

} // namespace nuby::paint
