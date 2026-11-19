#pragma once
#include "assets.hpp"
#include <filesystem>

namespace sacrifice {
// Loads a binary glTF 2.0 (.glb) model into a ModelAsset (renderer space: X right, Y up, Z forward).
// Centers X/Z and places the lowest vertex at Y = 0. If `target_height > 0`, scales uniformly to match.
ModelAsset load_glb(const std::filesystem::path& path, float target_height = 0.0f);
} // namespace sacrifice
