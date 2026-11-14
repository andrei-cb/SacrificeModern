#pragma once
#include "assets.hpp"
#include "building.hpp"
#include "creature_actor.hpp"
#include "side.hpp"
#include "sound.hpp"
#include "trigger.hpp"
#include <deque>
#include <map>
#include <optional>

namespace sacrifice {
class WidgetField;
// NTTS record kinds dispatched by Sacrifice.exe 0x4567d0.
enum class EntityKind : std::uint32_t {
    Building = 2,
    Wizard = 4,
    Soul = 8, // Free soul spawned by 0x458480.
    Creature = 64,
    Scar = 128,
    ScriptPoint = 4096, // Indexed location list built by 0x47dd50; not drawn.
    WidgetField = 0x2000,
};
// Common NTTS prefix after the type/size words: Z-up world position, then type-specific fields.
struct MapEntity {
    std::uint32_t type{};
    Vec3 position;
    float angle{};         // Degrees for buildings and creatures; radians for wizards (0x457860).
    FourCC id;             // BLDG, WIZD, CRE8, or SCAR name for the kinds that carry one.
    std::uint32_t flags{}; // NTTS +28 for buildings/creatures/wizards.
    std::int32_t side{};   // +32; identity only until native SIDE controllers are bound.
    // +40: the name trigger code refers to (0x458240 / 0x457cb0 pass it to 0x46af00, object +1044).
    std::uint32_t script_id{0xffffffff};
    Vec2 extent; // Script points (+44): circle radius, or box half extents.
    int wizard_level{-1};
    std::uint8_t wizard_god{};
    static MapEntity decode(const EntityRecord& record);
};
// WIDG mesh inside a WIDC folder: vertex count, u16 triangle count, u16 flags, texture ID, then 20-byte
// x/y/z/u/v vertices and 4-byte triangles (three u8 indices and an unused byte), as consumed by
// WIDGET::DrawWidgetList.
struct WidgetModel {
    struct Point {
        Vec3 position;
        Vec2 uv;
    };
    std::uint16_t flags{}; // Meaning not yet recovered.
    FourCC texture;
    std::vector<Point> points;
    std::vector<std::array<std::uint8_t, 3>> triangles;
    static WidgetModel decode(ByteView data);
};
// SCAR terrain decal: width at 8, height at 12, 256 BGRx palette entries at 16, then (index, alpha 0..8)
// texel pairs at 1040 (LAND::ApplyScarring 0x16045850).
struct Scar {
    unsigned width{}, height{};
    std::array<std::uint32_t, 256> palette{}; // RGBA, as terrain tile images.
    std::vector<std::array<std::uint8_t, 2>> texels;
    static Scar decode(ByteView data);
};
// LAND::ApplyScarring, zero source offset and no procedural wrapping. Zero dimensions use the full SCAR.
// Returns whether any terrain texels changed; repeated hits reuse each cell's private texture.
bool apply_scar(const Scar& scar, Vec3 position, Terrain& terrain, TerrainMaterials& materials,
                unsigned width = 0, unsigned height = 0, std::int16_t priority = 0);
// Applies the map's NTTS scars to copies of the affected terrain tiles; returns the number applied.
unsigned apply_scars(const ResourceCatalog& catalog, const Archive& map, Terrain& terrain,
                     TerrainMaterials& materials, std::vector<std::string>& unresolved);
// 428-byte ENVI chunk copied by Sacrifice.exe 0x456b80, which applies its light (0x456e40), sky, and fog.
struct Environment {
    float sun_intensity{};      // 4: LIGHT3D::SetIntensity.
    float ambient_intensity{};  // 8: _3dngn::SetAmbientIntensity.
    Vec3 ambient;               // 12: 0..255 floats, scaled by 1/255 for _3dngn::SetAmbientColor.
    std::uint32_t sky_color{};  // 24: D3DCOLOR for LAND::SetSkyColor (view clear color).
    std::uint32_t sky_layers{}; // 28: bit i selects texture slot i, bit 5 + i a Smacker path instead.
    int alpha_low{}, alpha_high{};
    float alpha_low_value{}, alpha_high_value{}; // 32..44: LAND::SetSkyAlphaTable arguments.
    std::array<FourCC, 5> sky_textures;          // 48 + 64 i: TXTR names (SKY_, SKYT, SKYB, SUN_, UNDR).
    std::array<std::string, 5> sky_videos;       // Same slots when bit 5 + i is set.
    Vec3 sun_direction;                          // 372: towards the sun; 0x456b80 negates it for SetDir.
    Vec3 sun_color;                              // 384: RGB bytes scaled by 1/255.
    float terrain_gloss{}, terrain_gloss_width{}; // 392/396: LAND specular intensity/width.
    std::uint32_t terrain_gloss_color{}; // 400: RGB byte color for the light bake.
    FourCC edge;                                 // 404: LAND::SetEdgeTexture TXTR (land view skirts).
    std::uint32_t fog_color{}, fog_mode{};       // 408, 412: LAND::SetFogging.
    float fog_start{}, fog_end{}, fog_density{}; // 416..424; start clamped to >= 100 and end to >= start.
    static Environment decode(ByteView data);
};
// LAND cloud state: scroll positions (LAND floats 75..78), the cylinder azimuth (0x16056fac), and the
// 9x9 grids at LAND+68 (alpha-table indices, then grays) sampled from the runtime noise table.
struct SkyState {
    float noise_x{}, noise_y{}, offset_x{}, offset_y{};
    float azimuth{};
    std::array<std::uint8_t, 162> cells{};
    SkyState(); // LAND::Init: GenerateSkyRows(0, 9) at the origin.
    // LAND::AnimateSky with the stock wind, in game ticks (Sacrifice.exe converts ticks at 1/60 s).
    void advance(int ticks);

  private:
    void generate_rows(int first, int count);
    void generate_columns(int first, int count);
};
// ENVI sky slots (SKY_, SKYT, SKYB, SUN_, UNDR) from the tileset's LAND folder; `video_frames` supply the
// Smacker slots.
using SkyTextures = std::array<std::optional<ModelAsset::Texture>, 5>;
SkyTextures load_sky_textures(const ResourceCatalog& catalog, const Environment& environment, FourCC tileset,
                              const std::array<Image, 5>& video_frames = {});
// The ENVI edge texture, looked up like the sky slots; empty when the map names none.
std::optional<ModelAsset::Texture> load_edge_texture(const ResourceCatalog& catalog,
                                                     const Environment& environment, FourCC tileset);
// LAND::DrawSky geometry in renderer space (cloud plane, UNDR and SUN_ quads, two sky cylinders). The
// topology depends only on the environment and slots, so later states can replace just the vertices.
Mesh sky_mesh(const Environment& environment, const SkyTextures& slots, const SkyState& state);
ModelAsset build_sky(const Environment& environment, const SkyTextures& slots, const SkyState& state = {});
// Renderer-space instance: position (world X, height, world Y) and rotation about the vertical axis.
struct Placement {
    Vec3 position;
    float yaw{}, scale{1};
};
// Free soul FLAG (0x475c90 creates, 0x475ee0 updates per tick): growth (+1100) rises 0.005 per tick
// from 0 at activation to 1, the wave phases start at rand() / 16384 and advance 0.015 / 0.01 per tick,
// and the frame advances 0.25 per tick within half of the atlas.
struct SoulState {
    float phase_x{}, phase_y{}, frame{}, growth{1};
    float alignment{}, scale{1};
    unsigned atlas_offset{};
    bool positioned{}; // The match supplies the moving FLAG origin instead of a fixed grown placement.
    void advance(int ticks, unsigned frames);
};
// FLAG::Draw 0x16022b60 grid for a grown, screen-oriented soul (local X right, Y up, Z away).
Mesh soul_mesh(const SoulState& state, const TextureHeader& atlas);
struct SceneModel {
    FourCC id;
    ModelAsset asset;
    std::vector<Placement> placements;
    std::vector<bool> visible; // Empty means all visible; building states share batched model uploads.
    std::vector<float> reveal; // Per-placement MRMINSTANCE +208 construction reveal; empty means 1.
    // FLAG::ScreenOriented: drawn in the camera's rotation (local X right, Y up), ignoring yaw.
    bool screen_oriented{};
    // WIDGET::Draw: a widget field carries each widget's anchor (renderer space) in the vertex normal, is
    // drawn to `widget_range` horizontally with its model material, and from 0.8 x range on fades out with
    // `widget_fade_flags`. Zero range marks an ordinary model.
    float widget_range{};
    std::uint32_t widget_fade_flags{};
    std::shared_ptr<const WidgetField> widget_field;
    // Free souls carry their FLAG state; their vertices are replaced as it advances.
    std::optional<SoulState> soul;
    class SoulActor* soul_object{};
    struct Character {
        std::shared_ptr<const CreatureActorResources> rules;
        std::shared_ptr<const SaxsAsset> body;
        std::shared_ptr<const SaxsTopology> topology;
        CreatureActor* object{}; // Owned by the inspector's Match after activation.
        std::uint64_t uploaded_pose{};
    };
    std::optional<Character> character;
    struct SummonPreview {
        FourCC creature;
        class SpellActor* object{};
        std::shared_ptr<const SaxsAsset> body;
        std::shared_ptr<const SaxsTopology> topology;
        std::shared_ptr<const CreatureClip> clip;
        std::uint64_t uploaded_pose{};
    };
    std::optional<SummonPreview> summon;
    struct SpellVisual { class SpellActor* object{}; float uploaded_frame{-1}; };
    std::optional<SpellVisual> spell_visual;
    static SceneModel make_character(Character resources);
    // CRE8 creatures, or WIZD wizards (same layout; 0x44f840 requires kind 8).
    static SceneModel load_character(const ResourceCatalog& catalog, FourCC id,
                                     FourCC tag = FourCC::from_text("CRE8"));
};
struct MapScene {
    struct Spawn {
        enum class Kind { building, character, soul } kind;
        std::size_t model{SIZE_MAX}, placement_index{};
        Placement placement;
        std::uint32_t flags{};
        std::int32_t side{};
        MatchObject* object{}; // Owned by the host Match, including invisible model-less building parts.
        std::size_t building_group{SIZE_MAX};
        unsigned building_part{};
        std::array<std::size_t, 3> state_models{SIZE_MAX, SIZE_MAX, SIZE_MAX};
        std::array<std::size_t, 3> state_placements{};
        int wizard_level{-1};
        std::uint8_t wizard_god{};
        std::uint32_t script_id{0xffffffff};
    };
    struct BuildingGroup {
        Building definition;
        std::size_t first_spawn{};
        Vec3 position; // Map X/Y/Z passed to 0x457cb0.
        FourCC id;
    };
    std::vector<SceneModel> models;
    std::vector<Spawn> spawns; // NTTS order, independent of batched rendering model/placement order.
    std::deque<BuildingGroup> building_groups; // Stable: parts keep definition pointers while it grows.
    // Remastered Look (drawing only): map X, Y and radius around mana fountains and altars, whose ground is a
    // tiled picture. The new plants and the grass layer keep off it.
    std::vector<Vec3> clearings;
    std::shared_ptr<const SoundSet> sound_set;
    std::shared_ptr<const SpellRegistry> spell_registry;
    std::map<FourCC, std::shared_ptr<const MrmGeometry>> spell_models;
    std::vector<SideDefinition> sides;
    std::vector<TriggerDefinition> triggers; // TRIG, loaded without a side override.
    std::vector<Triggers::Objective> objectives;
    std::vector<std::string> trigger_variables; // TVAR names in id order (0x47d200 / 0x47d760).
    std::map<FourCC, std::string> trigger_files; // Mission FLDR:FILE filename operands.
    std::map<FourCC, std::string> trigger_text; // Mission LANG/ENGL/TRIG, IDs repeat between maps.
    std::vector<TriggerMarker> markers;         // NTTS script points (0x47dd50).
    std::map<std::uint32_t, unsigned> kinds; // NTTS type -> records.
    unsigned buildings{}, parts{}, creatures{}, widget_fields{}, widgets{}, scars{}, wizards{}, souls{};
    std::vector<std::string> unresolved;
    std::optional<Environment> environment;
    // Inspector composition of buildings, creatures, wizards, free souls (type 8), and widget fields
    // (scars: apply_scars); type 4096 script points are counted only.
    static MapScene compose(const ResourceCatalog& catalog, const Archive& map, const Terrain& terrain);
};
// 0x457300 -> LAND::GenerateLightMap: convert the stored RGB filter to the terrain lighting.
// Call once after buildings and their footprints are placed, before uploading terrain vertices.
void bake_terrain_lighting(Terrain& terrain, const TerrainMaterials& materials, const MapScene& scene,
                           const Archive& map, const ResourceCatalog& catalog);
// Bilinear height of the 10-unit HMAP grid at a Z-up world XY (clamped to the map).
// `true_height` follows LAND::GetTrueHeight: the plane height even on a triangle that touches a hole.
float terrain_height(const Terrain& terrain, float x, float y, bool true_height = false);
} // namespace sacrifice
