#include "sacrifice/scene_updates.hpp"
namespace sacrifice {
bool apply_terrain_changes(const ResourceCatalog& catalog, SceneMatch& simulation, Terrain& terrain,
                           TerrainMaterials& materials, std::size_t& applied,
                           std::map<FourCC, std::optional<Scar>>& scars) {
    const auto& tiles = simulation.tile_changes();
    auto& requests = simulation.match().scar_requests;
    bool changed = false;
    std::size_t scar_index{};
    while (applied < tiles.size() || scar_index < requests.size()) {
        if (scar_index == requests.size() ||
            (applied < tiles.size() && tiles[applied].order < requests[scar_index].order)) {
            const auto& change = tiles[applied++];
            changed |= materials.release_scar(terrain, change.cell);
            changed |= terrain.tiles.at(change.cell) != change.tile;
            terrain.tiles[change.cell] = change.tile;
        } else {
            const auto& request = requests[scar_index++];
            auto [entry, inserted] = scars.try_emplace(request.id);
            if (inserted)
                if (const auto location = catalog.find_chunk(FourCC::from_text("SCAR"), request.id))
                    entry->second = Scar::decode(catalog.read(*location));
            materials.scar_time = request.tick;
            if (entry->second)
                changed |= apply_scar(*entry->second, request.position, terrain, materials, request.width,
                                      request.height);
        }
    }
    requests.clear();
    return changed;
}
} // namespace sacrifice
