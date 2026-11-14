#include "sacrifice/gameplay_input.hpp"
#include "sacrifice/building_actor.hpp"
#include "sacrifice/creature_actor.hpp"
#include "sacrifice/fog.hpp"
#include "sacrifice/side.hpp"
#include "sacrifice/soul_actor.hpp"
#include "sacrifice/spell_combat.hpp"
#include <cmath>
namespace sacrifice {
namespace {
FourCC id(const char* text) {
    return FourCC::from_text(text);
}
} // namespace
GameplayView::GameplayView(Vec3 position, float yaw, float pitch, unsigned w, unsigned h)
    : eye(position), width(w), height(h) {
    forward = {std::sin(yaw) * std::cos(pitch), std::sin(pitch), std::cos(yaw) * std::cos(pitch)};
    right = {std::cos(yaw), 0, -std::sin(yaw)};
    up = {forward.y * right.z, forward.z * right.x - forward.x * right.z, -forward.y * right.x};
    const float aspect = static_cast<float>(h) / std::max(w, 1u);
    tan_width = std::tan(.785398163f * std::min(1.f, .75f / std::max(aspect, .0001f)));
    tan_height = std::tan(.785398163f * std::min(aspect, .75f));
}
Vec3 GameplayView::ray(float x, float y, unsigned w, unsigned h) const {
    const float nx = (2 * x / w - 1) * tan_width, ny = (1 - 2 * y / h) * tan_height;
    return {forward.x + right.x * nx + up.x * ny, forward.z + right.z * nx + up.z * ny,
            forward.y + right.y * nx + up.y * ny};
}
float GameplayView::depth(Vec3 p) const {
    return (p.x - eye.x) * forward.x + (p.z - eye.y) * forward.y + (p.y - eye.z) * forward.z;
}
std::optional<Vec2> GameplayView::project(Vec3 p, unsigned w, unsigned h) const {
    const auto z = depth(p);
    if (z <= 1)
        return {};
    const Vec3 d{p.x - eye.x, p.z - eye.y, p.y - eye.z};
    return Vec2{(1 + (d.x * right.x + d.y * right.y + d.z * right.z) / z / tan_width) * w / 2,
                (1 - (d.x * up.x + d.y * up.y + d.z * up.z) / z / tan_height) * h / 2};
}
CreatureActor* selection_portrait(const PlayerControl& player, Vec2 pixel, unsigned width, unsigned height) {
    const auto layout = SelectionPanelLayout::make(width, height);
    const auto& entries = player.selection.entries();
    for (unsigned i = 0; i < layout.slots.size() && i < entries.size(); ++i)
        if (layout.slots[i].contains(pixel))
            return dynamic_cast<CreatureActor*>(entries[i].object);
    return nullptr;
}
FourCC selection_ability(const PlayerControl& player, const SpellRegistry& spells) {
    const CreatureActor* leader = nullptr;
    FourCC result;
    for (const auto& entry : player.selection.entries())
        if (const auto* actor = dynamic_cast<const CreatureActor*>(entry.object);
            actor && actor->type != MatchObject::wizard && !actor->ritual_building)
            for (const auto& spell : actor->spells.entries) {
                const auto* definition = spells.find(spell.id);
                if (definition && definition->kind == 4 && definition->spell &&
                    !(definition->spell->property_flags & 0x1000000) &&
                    (!leader || actor->definition().rank > leader->definition().rank)) {
                    result = spell.id;
                    leader = actor;
                }
            }
    return result;
}
MatchObject* pick_gameplay(Match& match, PlayerControl& player, PlayerPointer& pointer,
                           const Terrain& terrain, const GameplayView& view, const MiniMapTransform& minimap,
                           unsigned locks, Vec2 pixel, Vec3& point, bool& on_ground,
                           const std::function<unsigned(const BuildingActor&)>& detail_vertices) {
    const auto width = view.width, height = view.height;
    if (auto* icon = selection_portrait(player, pixel, width, height)) {
        pointer.clear_target();
        on_ground = false;
        point = icon->motion.position;
        return icon;
    }
    const auto mini = MiniMapLayout::make(width, height);
    if (!(locks & 4) && mini.contains(pixel)) {
        pointer.clear_target();
        // 0x4672a0: kinds 29 (souls excluded), square icon hit boxes and native priority.
        // Drawing and picking share projection, known-object gating and edge placement.
        const auto transform = minimap;
        MatchObject* hit = nullptr;
        unsigned priority = 0;
        for (unsigned group = 0; group < 5; ++group)
            match.visit_group(group, 29, [&](MatchObject& object) {
                const auto marker = minimap_marker(match, player.side(), object, transform, mini);
                if (marker && marker->contains(pixel) && (!hit || marker->priority > priority)) {
                    hit = &object;
                    priority = marker->priority;
                }
                return 0;
            });
        if (hit) {
            on_ground = false;
            spell_target_point(point, hit);
            return hit;
        }
        point = minimap.world({(pixel.x - mini.bounds.origin.x) / mini.bounds.size.x,
                               (pixel.y - mini.bounds.origin.y) / mini.bounds.size.y});
        point.z = terrain_height(terrain, point.x, point.y);
        on_ground = point.z > 0 && player.side().fog && player.side().fog->known(point);
        return nullptr;
    }
    const Vec3 eye{view.eye.x, view.eye.z, view.eye.y};
    const auto ray = view.ray(pixel.x, pixel.y, width, height);
    // 0x480de0 uses LAND's triangle intercept as the initial nearest hit.
    const float land = terrain_intercept(terrain, eye, ray, point, 0, true);
    on_ground = land > 0;
    double nearest = on_ground ? land : 1.0e14;
    MatchObject* hit = nullptr;
    for (const auto& object : match.objects()) {
        const auto* motion = object->source_motion();
        const unsigned mask = player.pending.value && !(player.target_mask() & 8) ? 0x17 : 0x1f;
        if (!motion || !(object->kind_flags & mask) ||
            (object->state_flags & (MatchObject::removed_flag | 0x1000000)))
            continue;
        auto* actor = dynamic_cast<CreatureActor*>(object.get());
        if (actor) {
            const auto flags = actor->fsm().state() ? actor->fsm().state()->flags : 0;
            if ((flags & 0x1000000) || ((flags & 0x80000) && actor->allegiance != match.local_side) ||
                (!actor->alive() && actor->allegiance != match.local_side))
                continue;
        }
        if (const auto* soul = dynamic_cast<const SoulActor*>(object.get())) {
            if ((soul->soul_flags & 0xc0000000) || soul->visual.growth <= 0)
                continue;
            // Souls are waving, camera-facing FLAG sprites, not the point-sized
            // motion box. Match their visible footprint and give small/distant sprites
            // a usable cursor margin. This path is shared by hover and spell clicks.
            const auto at = view.project(motion->position, width, height);
            if (!at)
                continue;
            const Vec3 d{motion->position.x - eye.x, motion->position.z - eye.z, motion->position.y - eye.y};
            const float depth = d.x * view.forward.x + d.y * view.forward.y + d.z * view.forward.z;
            const float scale = height / (2 * view.tan_height * depth),
                        extent = .1f + 1.65f * soul->visual.growth,
                        margin = std::max(10.0f, width * (10.0f / 1280));
            const float half_x = std::max(margin, (extent * .75f + .3f) * scale),
                        half_y = std::max(margin, extent * .7f * scale);
            if (std::abs(pixel.x - at->x) <= half_x && std::abs(pixel.y - at->y) <= half_y &&
                depth < nearest) {
                nearest = depth;
                hit = object.get();
            }
            continue;
        }
        // 0x480c90: projected model bounds have a two-pixel margin; <=10x10 gets a cheap hit.
        Vec2 low{1.0e14f, 1.0e14f}, high{-1.0e14f, -1.0e14f};
        bool clipped = false;
        for (unsigned corner = 0; corner < 8; ++corner) {
            const Vec3 p{motion->position.x + (corner & 1 ? motion->box_max.x : motion->box_min.x),
                         motion->position.y + (corner & 2 ? motion->box_max.y : motion->box_min.y),
                         motion->position.z + (corner & 4 ? motion->box_max.z : motion->box_min.z)};
            if (const auto at = view.project(p, width, height)) {
                low = {std::min(low.x, at->x), std::min(low.y, at->y)};
                high = {std::max(high.x, at->x), std::max(high.y, at->y)};
            } else
                clipped = true;
        }
        if (!clipped && (pixel.x < static_cast<int>(low.x) - 2 || pixel.x > static_cast<int>(high.x) + 2 ||
                         pixel.y < static_cast<int>(low.y) - 2 || pixel.y > static_cast<int>(high.y) + 2))
            continue;
        double distance = -1;
        if (!clipped && high.x - low.x <= 10 && high.y - low.y <= 10) {
            const Vec3 d{motion->position.x - eye.x, motion->position.z - eye.z, motion->position.y - eye.y};
            distance = d.x * view.forward.x + d.y * view.forward.y + d.z * view.forward.z;
        } else if (actor)
            distance = actor->pick_model(eye, ray);
        else if (const auto* building = dynamic_cast<const BuildingActor*>(object.get());
                 building && building->model() && building->model_scale > 0) {
            const auto& o = motion->orientation;
            const auto inverse = [&](Vec3 p) {
                return Vec3{(p.x * o[0] + p.y * o[1] + p.z * o[2]) / building->model_scale,
                            (p.x * o[3] + p.y * o[4] + p.z * o[5]) / building->model_scale,
                            (p.x * o[6] + p.y * o[7] + p.z * o[8]) / building->model_scale};
            };
            const unsigned vertices = detail_vertices ? detail_vertices(*building) : 0;
            distance = building_intercept(
                *building->model(),
                inverse({eye.x - motion->position.x, eye.y - motion->position.y, eye.z - motion->position.z}),
                inverse(ray), vertices);
        } else {
            // Other model classes retain their box fallback.
            const Vec3 from{eye.x - motion->position.x, eye.y - motion->position.y,
                            eye.z - motion->position.z};
            distance = motion_segment_contact(from, {ray.x * 30000, ray.y * 30000, ray.z * 30000},
                                              {motion->box_min, motion->box_max}, 0);
            if (distance >= 0)
                distance *= 30000;
        }
        if (distance > 0 && distance < nearest) {
            nearest = distance;
            hit = object.get();
        }
    }
    if (hit) {
        // 0x44e020: target orders use the model collision center, rather than land behind the target.
        spell_target_point(point, hit);
    }
    const unsigned retain_mask = player.pending.value ? player.target_mask() & 0x17 : 7;
    auto* retained = pointer.retain(match, hit, pixel, width, retain_mask,
                                    !player.pending.value || !player.target_check(hit, point, on_ground));
    if (retained != hit && retained) {
        hit = retained;
        on_ground = false;
        spell_target_point(point, hit);
    }
    return hit;
}
void select_gameplay_box(PlayerControl& player, const Terrain& terrain, const GameplayView& view,
                         const MiniMapTransform& minimap, bool map_box, Vec2 start, Vec2 end) {
    const auto width = view.width, height = view.height;
    const auto box = SelectionRectangle::make(start, end, width, height);
    const Vec3 eye{view.eye.x, view.eye.z, view.eye.y};
    std::vector<MatchObject*> candidates;
    // 0x4813e0 visits the side lists in native group/head order, not the match allocation order.
    for (const auto& group : player.side().objects)
        for (auto* object : group) {
            if (!player.selectable(*object))
                continue;
            const auto* motion = object->source_motion();
            if (!motion)
                continue;
            if (map_box) {
                if (minimap_box_contains(minimap, MiniMapLayout::make(width, height), start, end,
                                         motion->position))
                    candidates.push_back(object);
                continue;
            }
            const Vec3 centre{motion->position.x + (motion->box_min.x + motion->box_max.x) * .5f,
                              motion->position.y + (motion->box_min.y + motion->box_max.y) * .5f,
                              motion->position.z + (motion->box_min.z + motion->box_max.z) * .5f};
            const auto pixel = view.project(centre, width, height);
            if (!pixel || !box.contains({pixel->x, pixel->y}))
                continue;
            const Vec3 delta{centre.x - eye.x, centre.y - eye.y, centre.z - eye.z};
            Vec3 hit;
            const auto fraction = terrain_intercept(
                terrain, eye, delta, hit, delta.x * delta.x + delta.y * delta.y + delta.z * delta.z);
            if (fraction > 0 && fraction < 1)
                continue;
            candidates.push_back(object);
        }
    player.update_box(candidates);
}
bool gameplay_shortcuts(PlayerControl& player, unsigned locks, const std::array<bool, 256>& pressed,
                        const std::array<std::uint8_t, 256>& modifiers) {
    bool release = false;
    for (unsigned code = 2; code <= 11; ++code)
        if (pressed[code]) {
            const int group = code == 11 ? 0 : static_cast<int>(code) - 1;
            if ((modifiers[code] & 1) && group)
                player.assign_group(group);
            else if ((modifiers[code] & 2) && group)
                player.add_group(group);
            else if (!(modifiers[code] & 3))
                player.select_group(group);
        }
    if (pressed[19])
        player.pending = id("atck");
    if (pressed[33])
        player.pending = id("gurd");
    constexpr std::pair<unsigned, const char*> formations[] = {{23, "fsem"}, {24, "fcir"}, {25, "fpha"},
                                                               {36, "fwdj"}, {37, "fskr"}, {38, "flin"},
                                                               {26, "ffll"}, {27, "fflr"}};
    for (const auto& [key, formation] : formations)
        if (pressed[key] && !(modifiers[key] & 3))
            player.formation(id(formation));
    if (!(locks & 2)) {
        constexpr std::pair<unsigned, const char*> spells[] = {
            {44, "spup"}, {46, "sacc"}, {49, "maho"}, {50, "mlth"}, {34, "grdn"}, {35, "heal"}, {20, "tele"}};
        for (const auto& [key, spell] : spells)
            if (pressed[key] && !(modifiers[key] & 3) && player.choose_spell(id(spell)) && !player.pending.value)
                release = true;
        if (pressed[15])
            player.open_tab(3);
    }
    return release;
}
FourCC command_menu_root(const PlayerControl& player, MatchObject* hit, bool ground, bool map_hit) {
    if (map_hit)
        return id("goto");
    const auto context = PlayerPointer::context(player, hit, ground);
    const bool hostile = context == id("eunt") || context == id("ebdg");
    if (const auto* soul = dynamic_cast<const SoulActor*>(hit))
        return id(soul->soul_flags & 0xc0000000                   ? "none"
                  : soul->eligible_sides & player.side().identity ? "goto"
                                                                  : "sacc");
    if (const auto* building = dynamic_cast<const BuildingActor*>(hit))
        return id(!(building->building_flags & 0x55b7) ? "none"
                  : hostile                            ? "atck"
                  : building->allegiance == &player.side() || building->primary ||
                          (building->building_flags & 0x2810) != 0x2810 || building->reserved
                      ? "gurd"
                      : "lith");
    if (hit && (hit->kind_flags & 8))
        return id("gurd");
    if (hit && (hit->kind_flags & 5))
        return id(hostile ? "atck" : (hit->common_status & 8) ? "goto" : "gurd");
    return id("goto");
}
CommandMenuResult select_gameplay_menu(CommandMenu& menu, Vec2 pixel, PlayerControl& player, Match& match,
                                       std::uint32_t reference, Vec3 point) {
    const auto entry = menu.select(pixel);
    player.click(id("xmnu"), id(entry ? "slct" : "cncl"));
    player.click(id("xmsl"));
    if (!entry)
        return CommandMenuResult::none;
    auto* target = match.resolve(reference);
    const auto command = entry->command.text();
    if (command == "goto")
        player.move(target, point);
    else if (command == "gurd")
        player.guard(target, point);
    else if (command == "atck")
        player.attack(target, point);
    else if (command == "tgot" || command == "tgua" || command == "tatt")
        player.pending = id(command == "tgot" ? "goto" : command == "tgua" ? "gurd" : "atck");
    else if (command == "form")
        player.formation(entry->argument);
    else if (command == "pwiz")
        player.protect_wizard();
    else if (command == "Kabi")
        player.ability_key();
    else if (command == "Xspl" && player.spell_key(entry->argument)) {
        // 'Xspl' resolves a targeted spell at once against the cross's saved target (0x461950 with no icon),
        // then leaves target mode whether or not the target was accepted (0x461910).
        if (player.pending == id("cast")) {
            const bool cast = player.resolve_target(target, point);
            player.cancel_target();
            return cast ? CommandMenuResult::release_wizard : CommandMenuResult::none;
        }
        return CommandMenuResult::release_wizard;
    }
    else if (command == "ccam")
        return CommandMenuResult::centre_camera;
    return CommandMenuResult::none;
}
bool gameplay_hud_hit(const PlayerControl& player, unsigned locks, Vec2 p, unsigned width, unsigned height,
                      bool minimap, bool portraits) {
    const auto* wizard = player.wizard();
    const auto scale = hud_scale(width, height);
    const auto panel = SelectionPanelLayout::make(width, height);
    return (!(locks & 2) && wizard &&
            SpellBarLayout::make(width, height)
                .hit(p,
                     static_cast<unsigned>(
                         visible_spell_indices(*wizard, static_cast<unsigned>(player.spell_tab)).size()))) ||
           (!(locks & 1) && panel.bounds.contains(p) &&
            (portraits || !selection_portrait(player, p, width, height))) ||
           (minimap && !(locks & 4) && MiniMapLayout::make(width, height).contains(p)) ||
           (!(locks & 8) && p.x >= width - 64 * scale && p.y >= 0 && p.y < 96 * scale) ||
           (!(locks & 16) && p.x >= width - 128 * scale && p.y >= 0 && p.y < 96 * scale);
}
GameplayClickResult gameplay_click(PlayerControl& player, CommandMenu& menu, const PointerEvent& event,
                                   unsigned width, unsigned height, unsigned locks, int menu_delay,
                                   const std::function<MatchObject*(Vec2, Vec3&, bool&)>& pick,
                                   std::uint32_t& menu_target, Vec3& menu_point) {
    GameplayClickResult result;
    const bool left = event.button != PointerButton::right;
    if (!left && event.kind != PointerEvent::Kind::press) {
        player.cancel_target();
        return result;
    }
    if (!left && player.cancel_target()) {
        menu.close();
        result.cancelled = true;
        return result;
    }
    if (event.button == PointerButton::left) {
        if (!(locks & 1) && SelectionPanelLayout::make(width, height).ability.contains(event.point)) {
            player.choose_ability();
            return result;
        }
        if (!(locks & 2))
            if (auto* wizard = player.wizard()) {
                const auto entries = visible_spell_indices(*wizard, static_cast<unsigned>(player.spell_tab));
                if (const auto hit = SpellBarLayout::make(width, height)
                                         .hit(event.point, static_cast<unsigned>(entries.size()))) {
                    if (hit->tab)
                        player.open_tab(hit->index);
                    else
                        result.release =
                            player.choose_spell(
                                wizard->wizard_spells[player.spell_tab].entries[entries[hit->index]].id) &&
                            !player.pending.value;
                    return result;
                }
            }
    }
    if (gameplay_hud_hit(player, locks, event.point, width, height, false, false) ||
        gameplay_hud_hit(player, locks, event.origin, width, height, false, false))
        return result;
    player.append = (event.modifiers & 2) != 0;
    Vec3 point{};
    bool ground{};
    auto* hit = pick(event.point, point, ground);
    const bool map = !(locks & 4) && MiniMapLayout::make(width, height).contains(event.point);
    if (left) {
        if (!player.pending.value && map) {
            if (hit || ground)
                player.select_near(point);
        } else if (player.pending.value && (hit || ground)) {
            const bool casting = player.pending == id("cast");
            result.release = player.resolve_target(hit, point) && casting;
        } else if (hit && player.selectable(*hit)) {
            if (event.kind == PointerEvent::Kind::double_click || (event.modifiers & 1) ||
                event.button == PointerButton::middle)
                player.select_type(*hit);
            else
                player.select(hit);
        }
    } else if (!(locks & 1) && (hit || ground)) {
        menu_target = hit ? hit->reference() : 0;
        menu_point = point;
        result.opened = menu.open(command_menu_root(player, hit, ground, map), event.point, menu_delay);
        if (result.opened)
            player.click(id("xmnu"));
    }
    return result;
}
} // namespace sacrifice
