#pragma once

#include "../core/types.hpp"
#include "../core/string_utils.hpp"
#include "../paint/font_rasterizer.hpp"
#include <string>
#include <vector>
#include <cmath>

namespace nuby::layout {

struct GlyphMetric {
    float advance_width{8.0f};
    float height{16.0f};
    float bearing_x{0.0f};
    float bearing_y{12.0f};
};

struct TextRun {
    std::string text;
    core::RectF rect;
    float font_size{16.0f};
    int font_weight{400};
    core::Color color{0, 0, 0, 255};
};

class TextShaper {
public:
    // La fuente del motor es un bitmap monoespaciado 8x12: el avance real es
    // uniforme. Antes se estimaba por carácter y el texto se solapaba al
    // pintarlo — ahora medidor y rasterizador comparten la misma métrica,
    // INCLUIDO el zoom de texto de Configuración (text_zoom): si medir y
    // pintar no usan el mismo factor, el texto vuelve a solaparse.
    static float estimate_char_width(char, float font_size, int font_weight) {
        return paint::FontRasterizer::glyph_advance(font_size, font_weight);
    }

    static float measure_text_width(const std::string& text, float font_size, int font_weight) {
        float width = 0.0f;
        // Avance POR CODEPOINT real (con TTF son avances proporcionales del
        // propio archivo de fuente; UTF-8 decodificado de verdad, no por byte)
        for (size_t i = 0; i < text.size();) {
            uint32_t cp;
            unsigned char c = (unsigned char)text[i];
            if (c < 0x80) { cp = c; i += 1; }
            else if ((c & 0xE0) == 0xC0 && i + 1 < text.size()) {
                cp = ((c & 0x1F) << 6) | ((unsigned char)text[i + 1] & 0x3F); i += 2;
            } else if ((c & 0xF0) == 0xE0 && i + 2 < text.size()) {
                cp = ((c & 0x0F) << 12) | (((unsigned char)text[i + 1] & 0x3F) << 6) |
                     ((unsigned char)text[i + 2] & 0x3F); i += 3;
            } else if ((c & 0xF8) == 0xF0 && i + 3 < text.size()) {
                cp = ((c & 0x07) << 18) | (((unsigned char)text[i + 1] & 0x3F) << 12) |
                     (((unsigned char)text[i + 2] & 0x3F) << 6) | ((unsigned char)text[i + 3] & 0x3F); i += 4;
            } else { cp = c; i += 1; }
            width += paint::FontRasterizer::advance_cp(cp, font_size, font_weight);
        }
        return width;
    }

    static std::vector<TextRun> wrap_text(const std::string& text, float max_width, float font_size, int font_weight, const core::Color& color, float line_height) {
        std::vector<TextRun> runs;
        if (text.empty()) return runs;

        auto words = core::StringUtils::split_whitespace(text);
        if (words.empty()) return runs;

        float current_x = 0.0f;
        float current_y = 0.0f;
        std::string current_line;
        float space_width = estimate_char_width(' ', font_size, font_weight);

        for (size_t i = 0; i < words.size(); ++i) {
            const auto& word = words[i];
            float word_width = measure_text_width(word, font_size, font_weight);

            if (!current_line.empty() && (current_x + space_width + word_width) > max_width && max_width > 50.0f) {
                // Emit current line
                TextRun run;
                run.text = current_line;
                run.rect = core::RectF(0.0f, current_y, current_x, line_height);
                run.font_size = font_size;
                run.font_weight = font_weight;
                run.color = color;
                runs.push_back(run);

                current_line = word;
                current_x = word_width;
                current_y += line_height;
            } else {
                if (!current_line.empty()) {
                    current_line += " ";
                    current_x += space_width;
                }
                current_line += word;
                current_x += word_width;
            }
        }

        if (!current_line.empty()) {
            TextRun run;
            run.text = current_line;
            run.rect = core::RectF(0.0f, current_y, current_x, line_height);
            run.font_size = font_size;
            run.font_weight = font_weight;
            run.color = color;
            runs.push_back(run);
        }

        return runs;
    }
};

} // namespace nuby::layout
