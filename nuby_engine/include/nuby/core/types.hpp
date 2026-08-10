#pragma once

#include <string>
#include <vector>
#include <memory>
#include <cstdint>
#include <cmath>
#include <algorithm>
#include <sstream>
#include <iomanip>
#include <unordered_map>
#include <chrono>
#include <iostream>

namespace nuby::core {

// RGBA 32-bit Color representation
struct Color {
    uint8_t r{0};
    uint8_t g{0};
    uint8_t b{0};
    uint8_t a{255};

    constexpr Color() = default;
    constexpr Color(uint8_t r, uint8_t g, uint8_t b, uint8_t a = 255)
        : r(r), g(g), b(b), a(a) {}

    static Color transparent() { return Color(0, 0, 0, 0); }
    static Color white() { return Color(255, 255, 255, 255); }
    static Color black() { return Color(0, 0, 0, 255); }
    static Color red() { return Color(255, 0, 0, 255); }
    static Color green() { return Color(0, 180, 0, 255); }
    static Color blue() { return Color(30, 144, 255, 255); }

    bool operator==(const Color& other) const {
        return r == other.r && g == other.g && b == other.b && a == other.a;
    }

    bool is_transparent() const { return a == 0; }

    std::string to_hex_string() const {
        std::ostringstream ss;
        ss << "#" << std::hex << std::setfill('0')
           << std::setw(2) << static_cast<int>(r)
           << std::setw(2) << static_cast<int>(g)
           << std::setw(2) << static_cast<int>(b);
        if (a < 255) {
            ss << std::setw(2) << static_cast<int>(a);
        }
        return ss.str();
    }

    std::string to_rgba_string() const {
        std::ostringstream ss;
        ss << "rgba(" << static_cast<int>(r) << ", "
           << static_cast<int>(g) << ", "
           << static_cast<int>(b) << ", "
           << (a / 255.0f) << ")";
        return ss.str();
    }

    static Color from_hex(const std::string& hex) {
        std::string h = hex;
        if (h.empty()) return Color::black();
        if (h[0] == '#') h = h.substr(1);

        if (h.length() == 3) {
            // #RGB -> #RRGGBB
            std::string expanded;
            for (char c : h) {
                expanded += c;
                expanded += c;
            }
            h = expanded;
        }

        if (h.length() == 6) {
            uint32_t val = std::stoul(h, nullptr, 16);
            return Color((val >> 16) & 0xFF, (val >> 8) & 0xFF, val & 0xFF, 255);
        } else if (h.length() == 8) {
            uint32_t val = std::stoul(h, nullptr, 16);
            return Color((val >> 24) & 0xFF, (val >> 16) & 0xFF, (val >> 8) & 0xFF, val & 0xFF);
        }
        return Color::black();
    }

    static Color parse(const std::string& str);
};

template <typename T>
struct Point {
    T x{0};
    T y{0};

    Point() = default;
    Point(T x, T y) : x(x), y(y) {}
};

template <typename T>
struct Size {
    T width{0};
    T height{0};

    Size() = default;
    Size(T w, T h) : width(w), height(h) {}
};

template <typename T>
struct Rect {
    T x{0};
    T y{0};
    T width{0};
    T height{0};

    Rect() = default;
    Rect(T x, T y, T w, T h) : x(x), y(y), width(w), height(h) {}

    T right() const { return x + width; }
    T bottom() const { return y + height; }

    bool contains(T px, T py) const {
        return px >= x && px <= right() && py >= y && py <= bottom();
    }

    bool intersects(const Rect<T>& other) const {
        return !(x >= other.right() || right() <= other.x ||
                 y >= other.bottom() || bottom() <= other.y);
    }

    Rect<T> intersection(const Rect<T>& other) const {
        T nx = std::max(x, other.x);
        T ny = std::max(y, other.y);
        T nr = std::min(right(), other.right());
        T nb = std::min(bottom(), other.bottom());
        if (nr < nx || nb < ny) return Rect<T>(0, 0, 0, 0);
        return Rect<T>(nx, ny, nr - nx, nb - ny);
    }
};

using RectF = Rect<float>;
using PointF = Point<float>;
using SizeF = Size<float>;

struct Edges {
    float top{0.0f};
    float right{0.0f};
    float bottom{0.0f};
    float left{0.0f};

    Edges() = default;
    Edges(float t, float r, float b, float l)
        : top(t), right(r), bottom(b), left(l) {}
    explicit Edges(float all) : top(all), right(all), bottom(all), left(all) {}

    float horizontal() const { return left + right; }
    float vertical() const { return top + bottom; }
};

struct BorderRadius {
    float top_left{0.0f};
    float top_right{0.0f};
    float bottom_right{0.0f};
    float bottom_left{0.0f};

    BorderRadius() = default;
    explicit BorderRadius(float all)
        : top_left(all), top_right(all), bottom_right(all), bottom_left(all) {}
    BorderRadius(float tl, float tr, float br, float bl)
        : top_left(tl), top_right(tr), bottom_right(br), bottom_left(bl) {}
    
    bool has_radius() const {
        return top_left > 0 || top_right > 0 || bottom_right > 0 || bottom_left > 0;
    }
};

} // namespace nuby::core
