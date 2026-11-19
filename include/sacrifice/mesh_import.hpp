#pragma once
#include "assets.hpp"
#include <filesystem>
#include <vector>

namespace sacrifice {
// Loads a binary glTF 2.0 (.glb) model into a ModelAsset (renderer space: X right, Y up, Z forward).
// Centers X/Z and places the lowest vertex at Y = 0. If `target_height > 0`, scales uniformly to match.
ModelAsset load_glb(const std::filesystem::path& path, float target_height = 0.0f);

// Loads a Wavefront .obj (and its .mtl / textures if present) into a ModelAsset (Y up).
// Centers X/Z, grounds the base near Y = 0 (with a small root sink), and scales uniformly to `target_height` if > 0.
ModelAsset load_obj(const std::filesystem::path& path, float target_height = 0.0f);

// Finds `folder_name` (e.g. "obj") relative to the working/executable directory and loads all *.obj files in it.
// Results are cached by (folder_name, target_height).
const std::vector<ModelAsset>& load_obj_folder(const std::filesystem::path& folder_name, float target_height = 0.0f);
} // namespace sacrifice
