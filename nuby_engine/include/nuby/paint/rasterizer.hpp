#pragma once

#include "display_list.hpp"
#include "font_rasterizer.hpp"
#include "../core/types.hpp"
#include "../layout/text_shaper.hpp"
#include <vector>
#include <string>
#include <cmath>
#include <fstream>
#include <algorithm>
#include <sstream>
#include <iomanip>

namespace nuby::paint {

class SoftwareRasterizer {
private:
    // ESCALA DE RENDER (2026-10-14): el layout trabaja en píxeles lógicos
    // (CSS) como siempre, y aquí se multiplica TODO al rasterizar: rectángulos,
    // radios, sombras, bordes, glifos e imágenes. El framebuffer resultante es
    // (w*escala) x (h*escala), de modo que un píxel del motor = un píxel físico
    // de la pantalla del visitante. Sin esto, en un móvil de densidad 3 el
    // canvas de 390 px se estiraba a 1170 px reales y la interfaz se veía
    // borrosa/pixelada. El layout NO cambia: sigue midiendo en píxeles lógicos,
    // así que las decisiones responsive (anchos de columna, cortes) son las de
    // siempre.
    int width_{1000};   // ancho del framebuffer (píxeles de dispositivo)
    int height_{800};   // alto del framebuffer (píxeles de dispositivo)
    float scale_{1.0f}; // px lógicos -> px de dispositivo
    // ORIGEN de la ventana (px lógicos). Permite pintar SOLO la parte del
    // documento que el usuario está viendo: el búfer no crece con lo largo de
    // la página (antes había que reservar la página entera y una Wikipedia a
    // densidad 3 pedía más de 100 MB). El layout no se toca: solo se desplaza
    // el punto de dibujo.
    float origin_x_{0.0f};
    float origin_y_{0.0f};
    std::vector<uint32_t> framebuffer_; // ARGB 32-bit: 0xAARRGGBB
    std::vector<core::RectF> clip_stack_;

    // Escalado de primitivas (una sola vez, en la entrada de cada función).
    core::RectF S(const core::RectF& r) const {
        return core::RectF((r.x - origin_x_) * scale_, (r.y - origin_y_) * scale_,
                           r.width * scale_, r.height * scale_);
    }
    float S(float v) const { return v * scale_; }

    bool is_clipped(int px, int py) const {
        if (clip_stack_.empty()) return false;
        const auto& clip = clip_stack_.back();
        return px < clip.x || px >= clip.right() || py < clip.y || py >= clip.bottom();
    }

    static inline uint32_t blend_pixel(uint32_t dst, const core::Color& src, float coverage = 1.0f) {
        float alpha = (src.a / 255.0f) * coverage;
        if (alpha <= 0.0f) return dst;
        if (alpha >= 1.0f) return (0xFF << 24) | (src.r << 16) | (src.g << 8) | src.b;

        uint8_t dst_r = (dst >> 16) & 0xFF;
        uint8_t dst_g = (dst >> 8) & 0xFF;
        uint8_t dst_b = dst & 0xFF;

        uint8_t out_r = static_cast<uint8_t>(src.r * alpha + dst_r * (1.0f - alpha));
        uint8_t out_g = static_cast<uint8_t>(src.g * alpha + dst_g * (1.0f - alpha));
        uint8_t out_b = static_cast<uint8_t>(src.b * alpha + dst_b * (1.0f - alpha));

        return (0xFF << 24) | (out_r << 16) | (out_g << 8) | out_b;
    }

public:
    SoftwareRasterizer(int w, int h, core::Color clear_color = core::Color::white(),
                       float scale = 1.0f)
        : width_(static_cast<int>(std::lround(w * (scale > 0.01f ? scale : 1.0f)))),
          height_(static_cast<int>(std::lround(h * (scale > 0.01f ? scale : 1.0f)))),
          scale_(scale > 0.01f ? scale : 1.0f),
          framebuffer_(static_cast<size_t>(width_) * static_cast<size_t>(height_),
                       (0xFF << 24) | (clear_color.r << 16) | (clear_color.g << 8) | clear_color.b) {}

    float render_scale() const { return scale_; }
    void set_origin(float x, float y) { origin_x_ = x; origin_y_ = y; }

    void clear(const core::Color& color = core::Color::white()) {
        uint32_t c = (0xFF << 24) | (color.r << 16) | (color.g << 8) | color.b;
        std::fill(framebuffer_.begin(), framebuffer_.end(), c);
        clip_stack_.clear();
    }

    void fill_rect(const core::RectF& rect_in, const core::Color& color) {
        const core::RectF rect = S(rect_in);
        int x0 = std::max(0, static_cast<int>(std::floor(rect.x)));
        int y0 = std::max(0, static_cast<int>(std::floor(rect.y)));
        int x1 = std::min(width_, static_cast<int>(std::ceil(rect.right())));
        int y1 = std::min(height_, static_cast<int>(std::ceil(rect.bottom())));

        for (int y = y0; y < y1; ++y) {
            for (int x = x0; x < x1; ++x) {
                if (is_clipped(x, y)) continue;
                size_t idx = y * width_ + x;
                framebuffer_[idx] = blend_pixel(framebuffer_[idx], color);
            }
        }
    }

    void fill_rounded_rect(const core::RectF& rect_in, const core::BorderRadius& radius_in, const core::Color& color) {
        const core::RectF rect = S(rect_in);
        const core::BorderRadius radius = core::BorderRadius(
            S(radius_in.top_left), S(radius_in.top_right),
            S(radius_in.bottom_right), S(radius_in.bottom_left));
        float r = radius.top_left;
        if (r <= 0.0f) {
            fill_rect(rect, color);
            return;
        }

        int x0 = std::max(0, static_cast<int>(std::floor(rect.x)));
        int y0 = std::max(0, static_cast<int>(std::floor(rect.y)));
        int x1 = std::min(width_, static_cast<int>(std::ceil(rect.right())));
        int y1 = std::min(height_, static_cast<int>(std::ceil(rect.bottom())));

        float half_w = rect.width / 2.0f;
        float half_h = rect.height / 2.0f;
        float center_x = rect.x + half_w;
        float center_y = rect.y + half_h;

        for (int y = y0; y < y1; ++y) {
            for (int x = x0; x < x1; ++x) {
                if (is_clipped(x, y)) continue;

                // Signed Distance Field for Rounded Box
                float px = std::abs(x + 0.5f - center_x) - (half_w - r);
                float py = std::abs(y + 0.5f - center_y) - (half_h - r);

                float dist = 0.0f;
                if (px > 0.0f && py > 0.0f) {
                    dist = std::sqrt(px * px + py * py) - r;
                } else {
                    dist = std::max(px, py) - r;
                }

                if (dist <= 0.0f) {
                    // Inside box
                    size_t idx = y * width_ + x;
                    framebuffer_[idx] = blend_pixel(framebuffer_[idx], color, 1.0f);
                } else if (dist < 1.0f) {
                    // Subpixel Anti-aliasing boundary
                    float coverage = 1.0f - dist;
                    size_t idx = y * width_ + x;
                    framebuffer_[idx] = blend_pixel(framebuffer_[idx], color, coverage);
                }
            }
        }
    }

    void draw_box_shadow(const core::RectF& rect_in, const core::PointF& offset_in, float blur_in, const core::Color& color) {
        const core::RectF rect = S(rect_in);
        const core::PointF offset(offset_in.x * scale_, offset_in.y * scale_);
        const float blur = S(blur_in);
        if (blur <= 0.0f) {
            core::RectF shadow_r(rect.x + offset.x, rect.y + offset.y, rect.width, rect.height);
            fill_rect(shadow_r, color);
            return;
        }

        core::RectF shadow_r(rect.x + offset.x - blur, rect.y + offset.y - blur,
                             rect.width + blur * 2.0f, rect.height + blur * 2.0f);

        int x0 = std::max(0, static_cast<int>(std::floor(shadow_r.x)));
        int y0 = std::max(0, static_cast<int>(std::floor(shadow_r.y)));
        int x1 = std::min(width_, static_cast<int>(std::ceil(shadow_r.right())));
        int y1 = std::min(height_, static_cast<int>(std::ceil(shadow_r.bottom())));

        float orig_x = rect.x + offset.x;
        float orig_y = rect.y + offset.y;
        float orig_r = orig_x + rect.width;
        float orig_b = orig_y + rect.height;

        for (int y = y0; y < y1; ++y) {
            for (int x = x0; x < x1; ++x) {
                if (is_clipped(x, y)) continue;

                // Approximate distance to original rectangle
                float dx = std::max({orig_x - x, 0.0f, x - orig_r});
                float dy = std::max({orig_y - y, 0.0f, y - orig_b});
                float dist = std::sqrt(dx * dx + dy * dy);

                if (dist < blur) {
                    float factor = 1.0f - (dist / blur);
                    float gaussian = factor * factor * (3.0f - 2.0f * factor); // smoothstep
                    size_t idx = y * width_ + x;
                    framebuffer_[idx] = blend_pixel(framebuffer_[idx], color, gaussian * 0.4f);
                }
            }
        }
    }

    void draw_linear_gradient(const core::RectF& rect_in, float angle_deg, const std::vector<std::pair<float, core::Color>>& stops) {
        const core::RectF rect = S(rect_in);
        if (stops.empty()) return;
        if (stops.size() == 1) {
            fill_rect(rect, stops[0].second);
            return;
        }

        int x0 = std::max(0, static_cast<int>(std::floor(rect.x)));
        int y0 = std::max(0, static_cast<int>(std::floor(rect.y)));
        int x1 = std::min(width_, static_cast<int>(std::ceil(rect.right())));
        int y1 = std::min(height_, static_cast<int>(std::ceil(rect.bottom())));

        for (int y = y0; y < y1; ++y) {
            for (int x = x0; x < x1; ++x) {
                if (is_clipped(x, y)) continue;

                // Compute t along gradient axis (vertical by default 180deg)
                float t = (rect.height > 0) ? (y - rect.y) / rect.height : 0.0f;
                t = std::clamp(t, 0.0f, 1.0f);

                // Interpolate color stops
                core::Color col = stops[0].second;
                for (size_t s = 0; s + 1 < stops.size(); ++s) {
                    if (t >= stops[s].first && t <= stops[s + 1].first) {
                        float range = stops[s + 1].first - stops[s].first;
                        float local_t = (range > 0) ? (t - stops[s].first) / range : 0.0f;
                        const auto& c0 = stops[s].second;
                        const auto& c1 = stops[s + 1].second;

                        col = core::Color(
                            static_cast<uint8_t>(c0.r * (1.0f - local_t) + c1.r * local_t),
                            static_cast<uint8_t>(c0.g * (1.0f - local_t) + c1.g * local_t),
                            static_cast<uint8_t>(c0.b * (1.0f - local_t) + c1.b * local_t),
                            static_cast<uint8_t>(c0.a * (1.0f - local_t) + c1.a * local_t)
                        );
                        break;
                    }
                }

                size_t idx = y * width_ + x;
                framebuffer_[idx] = blend_pixel(framebuffer_[idx], col);
            }
        }
    }

    void draw_border(const core::RectF& rect, const core::Edges& widths, const core::Color& color) {
        if (widths.top > 0) fill_rect(core::RectF(rect.x, rect.y, rect.width, widths.top), color);
        if (widths.bottom > 0) fill_rect(core::RectF(rect.x, rect.bottom() - widths.bottom, rect.width, widths.bottom), color);
        if (widths.left > 0) fill_rect(core::RectF(rect.x, rect.y, widths.left, rect.height), color);
        if (widths.right > 0) fill_rect(core::RectF(rect.right() - widths.right, rect.y, widths.right, rect.height), color);
    }

    void draw_text(const std::string& text, float x, float y, float font_size, int font_weight, const core::Color& color) {
        // El glifo se rasteriza a tamaño*escala: TTF real a resolución de pantalla.
        x = S(x - origin_x_); y = S(y - origin_y_); font_size = S(font_size);
        float cx = x;
        // Decodificación UTF-8 real: itera por codepoints, no por bytes
        for (size_t i = 0; i < text.size();) {
            uint32_t cp;
            unsigned char c = (unsigned char)text[i];
            if (c < 0x80) { cp = c; i += 1; }
            else if ((c & 0xE0) == 0xC0 && i + 1 < text.size()) {
                cp = ((c & 0x1F) << 6) | ((unsigned char)text[i + 1] & 0x3F); i += 2;
            } else if ((c & 0xF0) == 0xE0 && i + 2 < text.size()) {
                cp = ((c & 0x0F) << 12) | (((unsigned char)text[i + 1] & 0x3F) << 6) | ((unsigned char)text[i + 2] & 0x3F); i += 3;
            } else if ((c & 0xF8) == 0xF0 && i + 3 < text.size()) {
                cp = ((c & 0x07) << 18) | (((unsigned char)text[i + 1] & 0x3F) << 12) |
                     (((unsigned char)text[i + 2] & 0x3F) << 6) | ((unsigned char)text[i + 3] & 0x3F); i += 4;
            } else { cp = 0xFFFD; i += 1; }
            FontRasterizer::render_glyph(framebuffer_, width_, height_, cp, cx, y, font_size, font_weight, color);
            cx += FontRasterizer::advance_cp(cp, font_size, font_weight);
        }
    }

    // Imagen decodificada REAL escalada por vecino más cercano y mezclada
    // source-over píxel a píxel (respeta el clip activo y el alfa por píxel).
    void draw_image(const std::shared_ptr<media::Image>& img, const core::RectF& dst_in) {
        const core::RectF dst = S(dst_in);
        if (!img || !img->valid() || dst.width <= 0 || dst.height <= 0) return;
        for (int y = (int)dst.y; y < (int)(dst.y + dst.height); ++y) {
            if (y < 0 || y >= height_) continue;
            int sy = (int)((y - dst.y) * img->height / dst.height);
            if (sy < 0) sy = 0; if (sy >= img->height) sy = img->height - 1;
            for (int x = (int)dst.x; x < (int)(dst.x + dst.width); ++x) {
                if (x < 0 || x >= width_ || is_clipped(x, y)) continue;
                int sx = (int)((x - dst.x) * img->width / dst.width);
                if (sx < 0) sx = 0; if (sx >= img->width) sx = img->width - 1;
                uint32_t sp = img->at(sx, sy);
                core::Color src((sp >> 16) & 0xFF, (sp >> 8) & 0xFF, sp & 0xFF, (sp >> 24) & 0xFF);
                framebuffer_[y * width_ + x] = blend_pixel(framebuffer_[y * width_ + x], src);
            }
        }
    }

    void execute_display_list(const DisplayList& display_list) {
        for (const auto& cmd : display_list.get_commands()) {
            switch (cmd.type) {
                case CommandType::FILL_RECT:
                    fill_rect(cmd.rect, cmd.color);
                    break;
                case CommandType::FILL_ROUNDED_RECT:
                    fill_rounded_rect(cmd.rect, cmd.radius, cmd.color);
                    break;
                case CommandType::DRAW_BORDER:
                    draw_border(cmd.rect, cmd.border_widths, cmd.border_color);
                    break;
                case CommandType::DRAW_BOX_SHADOW:
                    draw_box_shadow(cmd.rect, cmd.shadow_offset, cmd.blur_radius, cmd.color);
                    break;
                case CommandType::DRAW_LINEAR_GRADIENT:
                    draw_linear_gradient(cmd.rect, cmd.gradient_angle, cmd.gradient_stops);
                    break;
                case CommandType::DRAW_TEXT:
                    draw_text(cmd.text, cmd.rect.x, cmd.rect.y, cmd.font_size, cmd.font_weight, cmd.color);
                    break;
                case CommandType::DRAW_IMAGE:
                    draw_image(cmd.image, cmd.rect);
                    break;
                case CommandType::PUSH_CLIP:
                    clip_stack_.push_back(S(cmd.rect));
                    break;
                case CommandType::POP_CLIP:
                    if (!clip_stack_.empty()) clip_stack_.pop_back();
                    break;
            }
        }
    }

    bool save_ppm(const std::string& filename) const {
#if defined(__wasi__)
        // En WebAssembly el navegador no deja escribir en disco: estas
        // funciones son herramientas de depuracion del motor nativo.
        (void)filename;
        return false;
#else
        std::ofstream file(filename, std::ios::binary);
        if (!file.is_open()) return false;

        file << "P6\n" << width_ << " " << height_ << "\n255\n";
        std::vector<uint8_t> rgb_data(width_ * height_ * 3);
        for (int i = 0; i < width_ * height_; ++i) {
            uint32_t px = framebuffer_[i];
            rgb_data[i * 3 + 0] = (px >> 16) & 0xFF; // R
            rgb_data[i * 3 + 1] = (px >> 8) & 0xFF;  // G
            rgb_data[i * 3 + 2] = px & 0xFF;         // B
        }
        file.write(reinterpret_cast<const char*>(rgb_data.data()), rgb_data.size());
        return true;
#endif
    }

    bool save_bmp(const std::string& filename) const {
#if defined(__wasi__)
        // En WebAssembly el navegador no deja escribir en disco: estas
        // funciones son herramientas de depuracion del motor nativo.
        (void)filename;
        return false;
#else
        std::ofstream file(filename, std::ios::binary);
        if (!file.is_open()) return false;

        uint32_t row_size = ((width_ * 24 + 31) / 32) * 4;
        uint32_t image_size = row_size * height_;
        uint32_t file_size = 54 + image_size;

        uint8_t header[54] = {
            'B', 'M',
            static_cast<uint8_t>(file_size), static_cast<uint8_t>(file_size >> 8),
            static_cast<uint8_t>(file_size >> 16), static_cast<uint8_t>(file_size >> 24),
            0, 0, 0, 0, 54, 0, 0, 0,
            40, 0, 0, 0,
            static_cast<uint8_t>(width_), static_cast<uint8_t>(width_ >> 8),
            static_cast<uint8_t>(width_ >> 16), static_cast<uint8_t>(width_ >> 24),
            static_cast<uint8_t>(height_), static_cast<uint8_t>(height_ >> 8),
            static_cast<uint8_t>(height_ >> 16), static_cast<uint8_t>(height_ >> 24),
            1, 0, 24, 0, 0, 0, 0, 0,
            static_cast<uint8_t>(image_size), static_cast<uint8_t>(image_size >> 8),
            static_cast<uint8_t>(image_size >> 16), static_cast<uint8_t>(image_size >> 24),
            0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0
        };
        file.write(reinterpret_cast<const char*>(header), 54);

        std::vector<uint8_t> row_buffer(row_size, 0);
        for (int y = height_ - 1; y >= 0; --y) {
            for (int x = 0; x < width_; ++x) {
                uint32_t px = framebuffer_[y * width_ + x];
                row_buffer[x * 3 + 0] = px & 0xFF;         // B
                row_buffer[x * 3 + 1] = (px >> 8) & 0xFF;  // G
                row_buffer[x * 3 + 2] = (px >> 16) & 0xFF; // R
            }
            file.write(reinterpret_cast<const char*>(row_buffer.data()), row_size);
        }
        return true;
#endif
    }

    const std::vector<uint32_t>& get_pixels() const { return framebuffer_; }
    int get_width() const { return width_; }
    int get_height() const { return height_; }
};

} // namespace nuby::paint
