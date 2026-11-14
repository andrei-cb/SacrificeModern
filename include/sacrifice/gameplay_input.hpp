#pragma once
#include "gameplay_hud.hpp"
#include "player_input.hpp"

namespace sacrifice {
class BuildingActor;
struct GameplayView {
    Vec3 eye, forward, right, up;
    float tan_width{}, tan_height{};
    unsigned width{}, height{};
    GameplayView(Vec3 position, float yaw, float pitch, unsigned width, unsigned height);
    Vec3 ray(float x, float y, unsigned width, unsigned height) const;
    Vec3 ray(Vec2 pixel) const {
        return ray(pixel.x, pixel.y, width, height);
    }
    float depth(Vec3 point) const;
    std::optional<Vec2> project(Vec3 point, unsigned width, unsigned height) const;
    std::optional<Vec2> project(Vec3 point) const {
        return project(point, width, height);
    }
};
CreatureActor* selection_portrait(const PlayerControl& player, Vec2 pixel, unsigned width, unsigned height);
FourCC selection_ability(const PlayerControl& player, const SpellRegistry& spells);
MatchObject* pick_gameplay(Match& match, PlayerControl& player, PlayerPointer& pointer,
                           const Terrain& terrain, const GameplayView& view, const MiniMapTransform& minimap,
                           unsigned locks, Vec2 pixel, Vec3& point, bool& ground,
                           const std::function<unsigned(const BuildingActor&)>& detail_vertices = {});
void select_gameplay_box(PlayerControl& player, const Terrain& terrain, const GameplayView& view,
                         const MiniMapTransform& minimap, bool map_box, Vec2 start, Vec2 end);
bool gameplay_shortcuts(PlayerControl& player, unsigned locks, const std::array<bool, 256>& pressed,
                        const std::array<std::uint8_t, 256>& modifiers);
FourCC command_menu_root(const PlayerControl& player, MatchObject* target, bool ground, bool map_hit);
enum class CommandMenuResult { none, release_wizard, centre_camera };
CommandMenuResult select_gameplay_menu(CommandMenu& menu, Vec2 pixel, PlayerControl& player, Match& match,
                                       std::uint32_t target, Vec3 point);
bool gameplay_hud_hit(const PlayerControl& player, unsigned locks, Vec2 pixel, unsigned width,
                      unsigned height, bool minimap = true, bool portraits = true);
struct GameplayClickResult {
    bool release{}, opened{}, cancelled{};
};
GameplayClickResult gameplay_click(PlayerControl& player, CommandMenu& menu, const PointerEvent& event,
                                   unsigned width, unsigned height, unsigned locks, int menu_delay,
                                   const std::function<MatchObject*(Vec2, Vec3&, bool&)>& pick,
                                   std::uint32_t& menu_target, Vec3& menu_point);
} // namespace sacrifice
