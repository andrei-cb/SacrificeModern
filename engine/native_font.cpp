#include "sacrifice/native_font.hpp"
#include <cstring>

namespace sacrifice {
namespace {
bool blank(char c) { return c && std::strchr(" \n\r\t", c); }
} // namespace
const FontFace* find_font_face(std::string_view name) {
    for (const auto& face : font_faces)
        if (face.name == name)
            return &face;
    return nullptr;
}
NativeFont NativeFont::build(const FontFace& face, const Image& atlas) {
    NativeFont font;
    font.face = &face;
    font.atlas_width = static_cast<int>(atlas.width);
    font.atlas_height = static_cast<int>(atlas.height);
    font.cell_width = font.atlas_width / 16;
    font.cell_height = font.atlas_height / 16;
    // Codes 0..254: the rightmost non-empty glyph column plus one (at least 1); space is half the size.
    for (unsigned c = 0; c < 255; ++c) {
        if (c == ' ') {
            font.widths[c] = face.size / 2;
            continue;
        }
        const auto left = (c & 15) * static_cast<unsigned>(font.cell_width);
        const auto top = (c >> 4) * static_cast<unsigned>(font.cell_height);
        int column = font.cell_width - 1;
        for (; column > 0; --column) {
            bool lit = false;
            for (int y = 0; y < font.cell_height && !lit; ++y)
                lit = atlas.rgba[(top + y) * atlas.width + left + column] >> 24;
            if (lit)
                break;
        }
        font.widths[c] = column + 1;
    }
    return font;
}
int NativeFont::advance(unsigned char c) const {
    // Fixed-pitch faces use the 'W' cell for every character (0x16010b30 / 0x16010cb0).
    return (face->flags & 2 ? widths['W'] : widths[c]) + face->spacing;
}
int NativeFont::width(std::string_view text) const {
    int total = 0;
    for (const auto c : text)
        total += advance(static_cast<unsigned char>(c));
    return total;
}
NativeFont::Line NativeFont::line(std::string_view text, int max_width) const {
    int total = 0, n = 0;
    for (; n < static_cast<int>(text.size()); ++n) {
        if (text[n] == '\\' && n + 1 < static_cast<int>(text.size()) && text[n + 1] == 'n')
            return {n, n + 2};
        total += advance(static_cast<unsigned char>(text[n]));
        if (total > max_width)
            break;
    }
    if (n == static_cast<int>(text.size()))
        return {n, n};
    // Overflow at character n: break at the preceding whitespace, trimming the run before it; without any,
    // the native routine keeps one character fewer than fitted.
    int count = n;
    while (count > 0 && !blank(text[count]))
        --count;
    if (!count)
        return {n - 1, n - 1};
    int skip = 0;
    while (count > 1) {
        if (blank(text[count - 1])) {
            --count;
            continue;
        }
        skip = 1;
        break;
    }
    return {count, count + skip};
}
} // namespace sacrifice
