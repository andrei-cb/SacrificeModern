#pragma once
#include <source_location>
#include "building_actor.hpp"
#include "navigation.hpp"
#include "scene.hpp"
#include "spatial.hpp"
#include "trigger.hpp"
#include <bitset>

namespace sacrifice {
struct Character;
// Native object/spatial host shared by the viewer and simulation checks. Scene and terrain must
// outlive it. Optional materials bind the native navigation grid. Player/AI controllers and campaign
// are pending; implemented spell families share this match's services.
// A front-end player slot as 0x4a9d70 reads it (0x4a9ae0) for skirmish slots 1..5. Slot n selects the
// first listed side whose flag nibble is n (nibble zero with flag 0x20 counts as 1).
struct SkirmishSlot {
    enum class Kind { none, local, computer, observer } kind{};
    FourCC wizard;      // WIZD id (stock: cw{d,a,l,f,e}{0,1,2}).
    std::uint8_t god{}; // 1..5 fills all twenty god choices; zero draws rand() % 5 + 1.
    std::uint8_t team{}; // Lobby team 1..3 (slot +3); rows sharing one become mutual allies.
};
class SceneMatch {
  public:
    std::uint32_t initial_random_state{};
    // Skirmish slots create their wizards (0x4a9d70) after the map objects and before finalization
    // (0x456510: relation normalization, then trigger instances). Without slots no wizard is added.
    // A campaign session (+52 bit 0 clear) reads the other LEVL option branch of 0x4a1e10.
    SceneMatch(MapScene& scene, const Terrain& terrain, CrtRandom& random,
               const TerrainMaterials* materials = nullptr, const ResourceCatalog* catalog = nullptr,
               std::span<const SkirmishSlot> skirmish = {}, bool campaign = false,
               const Character* carried_character = nullptr);
    ~SceneMatch();
    Match& match() {
        return match_;
    }
    SpatialWorld& spatial() {
        return spatial_;
    }
    MatchSides& sides() {
        return sides_;
    }
    Triggers& triggers() {
        return triggers_;
    }
    Navigation* navigation() {
        return navigation_.get();
    }
    void prepare_models(); // View preparation, separate from ticking the match.
    void sync_buildings(); // Selects the visible model state for each independent building part.
    CreatureActor* create_script_creature(MatchSide& side, FourCC id, Vec3 point);
    CreatureActor* create_creature(CreatureActor& caster, FourCC id, Vec3 point,
                                   const MatchObject* after = nullptr);
    SoulActor* create_souls(Vec3 point, MatchObject* source, int count);
    // Native 0x457cb0 for buildings created during the match (altar fountains, later structure casts).
    BuildingActor* create_building(FourCC id, Vec3 position, float yaw, int side, std::uint32_t flags,
                                   MatchObject* controller = nullptr, MatchObject* reservation = nullptr);
    std::vector<SoulActor*> souls() const;
    // 0x457bd0 / 0x457a00 / 0x457860 offline: a wizard at the side's first unclaimed altar (0x457570),
    // 15 units toward the nearest 0x2810 primary building (else the map centre) and facing it
    // (0x457690). Returns null when the side has no such altar.
    CreatureActor* create_wizard(MatchSide& side, FourCC id, std::uint8_t god, bool computer);

    // LAND::SetTile writes since construction, in order: cell (y * width + x) and tile index. The
    // overlay replaces any scar layer on that cell; renderer re-upload is a host responsibility.
    struct TileChange {
        unsigned cell{};
        std::uint8_t tile{};
        std::uint64_t order{};
    };
    const std::vector<TileChange>& tile_changes() const {
        return tile_changes_;
    }
    std::uint8_t tile(unsigned cell) const;

  private:
    MapScene& scene_;
    // Copied: hosts may pass short-lived materials. Present tiles have LAND::GetTileProperties.
    std::optional<std::array<std::array<std::uint8_t, 12>, 256>> tile_properties_;
    std::bitset<256> present_tiles_;
    std::vector<std::uint8_t> tiles_; // Live LAND tile map (TMAP cells, then SetTile writes).
    std::vector<TileChange> tile_changes_;
    void stamp_footprint(Vec3 position, const Building& definition); // 0x44cfa0, 8x8 cells.
    const ResourceCatalog* catalog_{};
    std::map<FourCC, SceneModel::Character> creature_resources_;
    const SceneModel::Character& creature_resources(FourCC id, std::source_location where = std::source_location::current()) const; // Throws naming the missing id.
    std::map<std::size_t, MrmGeometry> building_geometry_; // Stable addresses while scene.models grows.
    std::map<FourCC, std::optional<Building>> building_definitions_;
    std::multimap<FourCC, std::size_t> free_building_models_; // Dynamic part models whose actor was reused.
    std::size_t dynamic_models_{};                            // First model index owned by one dynamic part.
    std::size_t building_model(FourCC id);
    bool prepare_creature(FourCC id, FourCC tag = FourCC::from_text("CRE8"));
    void bind_character_model(CreatureActor& actor, SceneModel model, FourCC id);
    std::optional<ModelAsset::Texture> soul_texture_;
    void bind_soul_visual(SoulActor& actor, std::vector<std::size_t> models);
    MatchSides sides_;                       // Side lists outlive actors and their pooled objects.
    Triggers triggers_;                      // Instances start after the map objects (0x456510).
    SpatialWorld spatial_;                   // Outlives Match's active and inactive actors.
    std::unique_ptr<Navigation> navigation_; // Outlives the actors' cached route nodes.
    Match match_;
    void clear_bindings();
};
} // namespace sacrifice
