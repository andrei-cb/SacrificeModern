#pragma once
#include "assets.hpp"
#include "wad.hpp"
#include <filesystem>
#include <string>

namespace sacrifice {
// The FORM:igam loading page's content for a map (0x4a9ed0 step 0 -> 0x40feb0): the map's DATA:LIMG
// picture (384x384 indexed pixels, then an RGB palette) or else data\defaultmap.tga, and the map's
// English TEXT:DESC for the 'intr' widget.
struct LoadingPicture {
    Image image;
    std::string description;
};
LoadingPicture loading_picture(const std::filesystem::path& root, const std::filesystem::path& map);
} // namespace sacrifice
