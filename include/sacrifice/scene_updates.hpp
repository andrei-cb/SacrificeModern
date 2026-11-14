#pragma once
#include "scene_match.hpp"
#include <map>

namespace sacrifice {
bool apply_terrain_changes(const ResourceCatalog& catalog, SceneMatch& simulation, Terrain& terrain,
                           TerrainMaterials& materials, std::size_t& applied,
                           std::map<FourCC, std::optional<Scar>>& scars);
} // namespace sacrifice
