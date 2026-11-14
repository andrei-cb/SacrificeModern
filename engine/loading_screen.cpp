#include "sacrifice/loading_screen.hpp"
#include "sacrifice/core.hpp"
#include <cstring>
#include <fstream>
#include <iterator>

namespace sacrifice {
namespace {
// 0x40feb0: an 8-bit 384x384 picture with a 256-entry palette, split over img1..img4.
Image indexed_picture(const std::uint8_t* pixels, const std::uint8_t* palette, bool bgr, bool bottom_up) {
    constexpr unsigned side = 384;
    Image image{side, side, std::vector<std::uint32_t>(side * side)};
    for (unsigned y = 0; y < side; ++y)
        for (unsigned x = 0; x < side; ++x) {
            const auto* c = &palette[pixels[(bottom_up ? side - 1 - y : y) * side + x] * 3u];
            const std::uint32_t r = c[bgr ? 2 : 0], g = c[1], b = c[bgr ? 0 : 2];
            image.rgba[y * side + x] = 0xff000000u | b << 16 | g << 8 | r;
        }
    return image;
}
} // namespace
LoadingPicture loading_picture(const std::filesystem::path& root, const std::filesystem::path& map) {
    LoadingPicture result;
    bool pictured = false;
    if (map.extension() == ".scp") {
        const auto archive = Archive::open(map);
        if (const auto limg = archive.find_chunk(FourCC::from_text("DATA"), FourCC::from_text("LIMG"))) {
            const auto bytes = archive.read(*limg);
            if (bytes.size() == 384 * 384 + 768) {
                const auto* data = reinterpret_cast<const std::uint8_t*>(bytes.data());
                result.image = indexed_picture(data, data + 384 * 384, false, false);
                pictured = true;
            }
        }
        if (const auto language = archive.find_chunk(FourCC::from_text("LANG"), FourCC::from_text("ENGL")))
            if (const auto desc = archive.find_chunk(FourCC::from_text("TEXT"), FourCC::from_text("DESC"), *language)) {
                const auto bytes = archive.read(*desc);
                const auto* begin = reinterpret_cast<const char*>(bytes.data());
                result.description.assign(begin, strnlen(begin, bytes.size()));
            }
    }
    if (!pictured) {
        std::ifstream file(root / "data/defaultmap.tga", std::ios::binary);
        const std::vector<std::uint8_t> tga{std::istreambuf_iterator<char>(file), {}};
        // Colour-mapped type 1, 256 BGR entries, 8-bit pixels; descriptor bit 0x20 means top-down.
        if (tga.size() < 18 || tga[1] != 1 || tga[2] != 1 || tga[7] != 24 || tga[16] != 8)
            throw Error("Unsupported data\\defaultmap.tga");
        const std::size_t palette = 18 + tga[0], pixels = palette + 768;
        if (tga.size() < pixels + 384 * 384)
            throw Error("Truncated data\\defaultmap.tga");
        result.image = indexed_picture(tga.data() + pixels, tga.data() + palette, true, !(tga[17] & 0x20));
    }
    return result;
}
} // namespace sacrifice
