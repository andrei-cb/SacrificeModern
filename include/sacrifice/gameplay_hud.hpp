#pragma once
#include "command_menu.hpp"
#include "native_font.hpp"
#include "player_pointer.hpp"
#include "player_visuals.hpp"
#include "spell_registry.hpp"

namespace sacrifice {
// Presentation uses resource IDs and renderer-independent geometry; backends supply only painting.
class HudPainter {
  public:
    virtual ~HudPainter() = default;
    virtual const NativeFont* font() const = 0;
    virtual const TextureHeader* header(FourCC type, FourCC id) = 0;
    virtual void fill(HudRect box, std::uint32_t colour) = 0;
    virtual void image(FourCC id, HudRect box, Vec2 uv0, Vec2 uv1) = 0;
    virtual void icon(FourCC id, HudRect box, std::uint32_t colour = 0xffffffff, bool additive = false) = 0;
    virtual void sprite(FourCC id, unsigned frame, HudRect box, std::uint32_t colour = 0xffffffff,
                        bool additive = false, float angle = 0) = 0;
    virtual void text(HudRect box, std::string_view text, std::uint32_t colour = 0xffffffff,
                      float scale = 1.5f, unsigned alignment = 0) = 0;
    virtual void mesh(const Mesh& geometry, bool additive = false, FourCC texture = {}) = 0;
};
struct HudViewport {
    unsigned width{}, height{};
    Vec2 cursor;
};
using HudText = std::function<std::string(FourCC)>;
using HudLines = std::vector<std::pair<std::string, std::uint32_t>>; // RGBA, like mesh vertices.
inline constexpr float hud_tooltip_scale = 1.5f;
void paint_tooltip(HudPainter& paint, const HudViewport& view, const HudLines& lines);
void paint_object_tooltip(HudPainter& paint, const HudViewport& view, const PlayerControl& player,
                          const MatchObject& object, FourCC context, const ResourceCatalog& catalog,
                          const HudText& text, const GradientBar* health_bar);
std::vector<std::size_t> visible_spell_indices(const CreatureActor& wizard, unsigned book);
bool paint_spell_bar(HudPainter& paint, HudPainter& tooltips, const HudViewport& view, CreatureActor& wizard,
                     const SpellRegistry& registry, unsigned book, bool changed, bool hover,
                     const HudText& text);
struct HudCamera {
    Vec3 eye, right, up, forward; // Renderer Y-up coordinates.
    float tan_width{}, tan_height{};
    unsigned width{}, height{};
};
std::optional<Mesh> object_hover_bracket(const MatchObject& object, const HudCamera& camera);
struct PointerFeedback {
    FourCC context, icon, warning;
    unsigned alpha{255};
    bool bracket{}, tooltip{};
};
PointerFeedback pointer_feedback(PlayerPointer& pointer, const PlayerControl& player, const Match& match,
                                 MatchObject* target, Vec3 point, bool ground, FourCC context, bool menu,
                                 bool dragging, bool blocked, float hover_ticks);
bool paint_pointer(HudPainter& paint, const PointerFeedback& feedback, Vec2 pixel);
bool paint_selection_panel(HudPainter& paint, HudPainter& tooltips, const HudViewport& view,
                           const PlayerControl& player, const SpellRegistry& registry, FourCC ability,
                           bool hover, const HudText& text);
void paint_status_meters(HudPainter& paint, HudPainter& numbers, HudPainter& tooltips,
                         const HudViewport& view, const CreatureActor& wizard, unsigned locks,
                         const std::array<GradientBar, 2>& styles, bool hover, const HudText& text);
void paint_command_menu(HudPainter& paint, const CommandMenu& menu, unsigned frame);
// `health_only` draws just the health bar (no mana) above another side's arrow. The stock game shows bars
// only for selected own units; this is a modern readability addition.
void paint_unit_hud(HudPainter& paint, const CreatureActor& actor, Vec2 anchor, float depth, unsigned width,
                    bool selected, bool health_only = false);
bool unit_hud_visible(const PlayerControl& player, const CreatureActor& actor);
// The host supplies its generated terrain image as TXTR:#map.
void paint_minimap(HudPainter& paint, const HudViewport& view, const Match& match,
                   const PlayerControl& player, const Terrain& terrain, const MiniMapTransform& transform,
                   float yaw, float tan_width);
} // namespace sacrifice
