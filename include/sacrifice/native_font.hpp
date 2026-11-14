#pragma once
#include "assets.hpp"
#include <array>
#include <string_view>

namespace sacrifice {
// game3d.dll FONTMGR descriptors (0x160550a0, 28 bytes each): name, TXTR atlas, point size, glyph spacing
// (+0x0c), extra line height (+0x10), baseline lift (+0x14) and flags (+0x18; 2 = fixed pitch).
struct FontFace {
    std::string_view name;
    std::uint32_t texture;
    int size, spacing, leading, baseline;
    std::uint32_t flags;
};
inline constexpr std::array<FontFace, 5> font_faces{{
    {"debug", 0x666e6462, 8, 1, 1, 0, 1},         // fndb
    {"verdanacolor", 0x666e3038, 8, 1, 3, 1, 1},  // fn08, paletted button text
    {"bookman", 0x66743132, 10, 1, 4, 2, 1},      // ft12
    {"elgar", 0x666e3132, 12, 1, 3, 1, 1},        // fn12
    {"verdana", 0x666e7774, 8, 1, 3, 1, 1},       // fnwt
}};
[[nodiscard]] const FontFace* find_font_face(std::string_view name);

// A 16x16 glyph-cell atlas indexed by byte value (0x16010970 builds the width table).
class NativeFont {
  public:
    struct Line {
        int count{}, skip{}; // Characters drawn, characters consumed (wrap whitespace or a literal "\n").
    };
    struct Glyph {
        float x{}, y{}, width{}, height{};
        float u0{}, v0{}, u1{}, v1{};
    };
    static NativeFont build(const FontFace& face, const Image& atlas);
    const FontFace* face{};
    std::array<int, 256> widths{};
    int cell_width{16}, cell_height{16}, atlas_width{256}, atlas_height{256};
    [[nodiscard]] int advance(unsigned char c) const;
    [[nodiscard]] int line_height() const { return face->size + face->leading; } // 0x16010b30 height.
    [[nodiscard]] int width(std::string_view text) const;                          // 0x16010b30 width.
    [[nodiscard]] Line line(std::string_view text, int max_width) const;           // 0x16010d80.
    // 0x16011470: wrapped, aligned text within a w x h box. Alignment 1 centres, 2 right-aligns and 4
    // justifies lines filling more than two thirds of the box (all but the last).
    template <class Emit>
    void layout(float x, float y, float box_width, float box_height, unsigned alignment, std::string_view text,
                Emit&& emit) const {
        float line_y = y;
        while (!text.empty()) {
            const auto [count, skip] = line(text, static_cast<int>(box_width));
            if (count <= 0 && skip <= 0)
                break;
            const auto row = text.substr(0, static_cast<std::size_t>(std::max(count, 0)));
            const int row_width = width(row);
            float left = x, extra = 0;
            if (alignment & 1)
                left = x + static_cast<float>(static_cast<int>(box_width) / 2 - row_width / 2);
            else if (alignment & 2)
                left = x + box_width - static_cast<float>(row_width);
            else if ((alignment & 4) && text.size() > static_cast<std::size_t>(skip) &&
                     row_width > static_cast<int>(box_width) * 2 / 3 && !row.empty())
                extra = (box_width - static_cast<float>(row_width)) / static_cast<float>(row.size());
            float pen = left;
            const float top = line_y - static_cast<float>(face->baseline);
            for (const auto ch : row) {
                const auto c = static_cast<unsigned char>(ch);
                if (c > ' ') { // 0x160110a0: control characters and space only advance.
                    const auto column = static_cast<float>(c & 15), cell_row = static_cast<float>(c >> 4);
                    const auto u = column * cell_width / atlas_width, v = cell_row * cell_height / atlas_height;
                    emit(Glyph{pen - .5f, top - .5f, static_cast<float>(widths[c]), static_cast<float>(cell_height), u, v,
                               u + static_cast<float>(widths[c]) / atlas_width,
                               v + static_cast<float>(cell_height) / atlas_height});
                }
                pen += static_cast<float>(advance(c)) + extra;
            }
            text.remove_prefix(std::min(text.size(), static_cast<std::size_t>(std::max(skip, 0))));
            line_y += static_cast<float>(line_height());
            if (line_y >= y + box_height)
                break;
        }
    }
};
} // namespace sacrifice
