#include "sacrifice/gameplay_hud.hpp"
#include "sacrifice/building_actor.hpp"
#include "sacrifice/creature_actor.hpp"
#include "sacrifice/side.hpp"
#include <cmath>
#include <format>

namespace sacrifice {
namespace {
FourCC id(const char* text) {
    return FourCC::from_text(text);
}
std::string values(std::string line, std::initializer_list<std::string> arguments) {
    std::size_t at = 0;
    for (const auto& value : arguments) {
        auto p = line.find('%', at);
        while (p != std::string::npos && p + 1 < line.size() && line[p + 1] != 'd' && line[p + 1] != 's')
            p = line.find('%', p + 1);
        if (p == std::string::npos || p + 1 == line.size())
            break;
        line.replace(p, 2, value);
        at = p + value.size();
    }
    return line;
}
std::string object_name(const MatchObject& object, const HudText& text) {
    if (const auto* actor = dynamic_cast<const CreatureActor*>(&object))
        return text(actor->definition().id);
    if (const auto* building = dynamic_cast<const BuildingActor*>(&object))
        return text(building->definition_id);
    return {};
}
HudRect tooltip_box(Vec2 size, const HudViewport& view) {
    constexpr float scale = hud_tooltip_scale;
    float x = view.cursor.x + 16 * scale, y = view.cursor.y + std::floor((16 * scale - size.y) * .5f);
    if (x + size.x > view.width)
        x = view.cursor.x - size.x;
    return {{std::clamp(x, 0.f, std::max(view.width - size.x, 0.f)),
             std::clamp(y, 0.f, std::max(view.height - size.y, 0.f))},
            size};
}
void spell_tooltip(HudPainter& paint, const HudViewport& view, const CreatureActor& wizard,
                   const SpellDefinition& spell, const HudText& text) {
    const auto mana = wizard.cast_cost(spell).first;
    HudLines lines{{text(spell.id), 0xffffffff},
                   {values(text(id("cost")), {std::to_string(mana), std::to_string(wizard.mana)}),
                    mana > wizard.mana ? 0xff0000ffu : 0xffffffffu}};
    if (spell.kind == 2)
        lines.emplace_back(values(text(id("sprq")), {std::to_string(spell.required_souls)}),
                           spell.required_souls > wizard.soul_count ? 0xff0000ffu : 0xffffffffu);
    paint_tooltip(paint, view, lines);
}
} // namespace
void paint_tooltip(HudPainter& paint, const HudViewport& view, const HudLines& lines) {
    const auto* font = paint.font();
    if (!font || lines.empty())
        return;
    constexpr float scale = hud_tooltip_scale;
    const float line = font->line_height() * scale;
    int width = 0;
    for (const auto& [text, colour] : lines)
        width = std::max(width, font->width(text));
    const auto box = tooltip_box({(width + 6) * scale, 6 * scale + line * lines.size()}, view);
    paint.fill(box, 0xaf000000);
    for (std::size_t i = 0; i < lines.size(); ++i)
        paint.text(
            {{box.origin.x + 3 * scale, box.origin.y + 3 * scale + i * line}, {(width + 1) * scale, line}},
            lines[i].first, lines[i].second, scale);
}
void paint_object_tooltip(HudPainter& paint, const HudViewport& view, const PlayerControl& player,
                          const MatchObject& object, FourCC context, const ResourceCatalog& catalog,
                          const HudText& text, const GradientBar* health_bar) {
    const auto* actor = dynamic_cast<const CreatureActor*>(&object);
    const auto* building = dynamic_cast<const BuildingActor*>(&object);
    if (!actor && !building) {
        paint_tooltip(paint, view, {{text(context), 0xffffffff}});
        return;
    }
    const auto* font = paint.font();
    if (!font)
        return;
    auto* owner = object.controller;
    const bool local = object.allegiance == &player.side(), own_wizard = &object == player.wizard();
    auto name = object_name(object, text);
    if (owner && owner != &object) {
        if (catalog.find(id("TEXT"), id("NofW")))
            name = values(text(id("NofW")), {name, object_name(*owner, text)});
        else
            name = values(text(id("WofN")), {object_name(*owner, text), name});
    }
    constexpr float scale = hud_tooltip_scale;
    const auto line = font->line_height() * scale;
    const float width =
        std::max(220.f, static_cast<float>(font->width(name) + (owner && owner != &object ? 66 : 50))) *
        scale;
    float height = (building ? ((building->building_flags & 0x80) ? 56.f : 40.f) : 70.f) * scale;
    if (own_wizard)
        height += 3 * line;
    const auto box = tooltip_box({width, height}, view);
    const auto [x, y] = box.origin;
    paint.fill(box, 0xaf000000);
    paint.icon(actor ? actor->definition().icon : building->definition_id,
               {{x + 3 * scale, y + 3 * scale}, {32 * scale, 32 * scale}});
    const auto row = [&](const std::string& string, float at) {
        paint.text({{x + 39 * scale, at}, {width - 42 * scale, line}}, string);
    };
    float text_y = y + 3 * scale;
    row(name, text_y);
    text_y += line;
    if (owner == &object && actor) {
        row(values(text(id("lvl#")), {std::to_string(actor->progression.level)}), text_y);
        text_y += line;
    }
    if (local && actor && owner) {
        row(values(text(id("nkil")), {std::to_string(actor->credit_count_80)}), text_y);
        row(values(text(id("ngib")), {std::to_string(actor->credit_count_20)}), text_y + line);
    }
    const auto bar = [&](int value, int maximum, float at, float size) {
        if (health_bar)
            paint.mesh(health_bar->horizontal(
                {x + 8 * scale, at}, {width - 16 * scale, size},
                static_cast<int>(static_cast<std::int64_t>(value) * 65536 / std::max(maximum, 1))));
    };
    if (actor)
        bar(actor->stats.current[CreatureStats::health], actor->stats.base[CreatureStats::health],
            y + 48 * scale, 12 * scale);
    else if (building->building_flags & 0x80)
        bar(building->health, building->maximum_health, y + 40 * scale, 12 * scale);
    if (own_wizard) {
        paint.text({{x + 9 * scale, y + height - 26 * scale}, {width - 18 * scale, line}}, text(id("uexp")));
        bar(actor->progression.experience,
            WizardProgression::thresholds[std::clamp(actor->progression.level, 0, 14)],
            y + height - 16 * scale, 8 * scale);
    }
}
std::vector<std::size_t> visible_spell_indices(const CreatureActor& wizard, unsigned book) {
    std::vector<std::size_t> result;
    const auto& entries = wizard.wizard_spells.at(book).entries;
    for (std::size_t i = 0; i < entries.size() && result.size() < 16; ++i)
        if (!(entries[i].flags & 2))
            result.push_back(i);
    return result;
}
bool paint_spell_bar(HudPainter& paint, HudPainter& tooltips, const HudViewport& view, CreatureActor& wizard,
                     const SpellRegistry& registry, unsigned book, bool changed, bool hover,
                     const HudText& text) {
    const auto layout = SpellBarLayout::make(view.width, view.height);
    const auto visible = visible_spell_indices(wizard, book);
    auto& entries = wizard.wizard_spells.at(book).entries;
    const float step = 16 * layout.scale, y = layout.top + 2 * step - .5f;
    constexpr float texel = .5f / 128;
    const auto strips = 2 * std::max<std::size_t>(5, visible.size());
    for (std::size_t i = 0; i < strips; ++i)
        paint.image(id("fram"), {{i * step - .5f, y}, {step, step}}, {.5f + texel, .25f + texel},
                    {.625f - texel, .375f - texel});
    const float end = strips * step - step * .25f - .5f;
    paint.image(id("fram"), {{end, y}, {step, view.height - y}}, {.625f + texel, .375f - texel},
                {1 - texel, .25f + texel});
    const auto aligned = [](HudRect r) {
        r.origin.x -= .5f;
        r.origin.y -= .5f;
        return r;
    };
    for (unsigned tab = 0; tab < 3; ++tab) {
        paint.sprite(id("tabs"), tab, aligned(layout.tabs[tab]));
        if (tab == book)
            paint.sprite(id("tabs"), 3, aligned(layout.tabs[tab]));
    }
    for (unsigned i = 0; i < visible.size(); ++i)
        paint.sprite(id("page"), book, aligned(layout.slots[i]));
    // All icons, then warnings, cooldowns and readiness flashes, including effects across slots.
    for (unsigned pass = 0; pass < 4; ++pass)
        for (unsigned i = 0; i < visible.size(); ++i) {
            auto& entry = entries[visible[i]];
            const auto* definition = registry.find(entry.id);
            if (!definition)
                continue;
            const auto slot = layout.slots[i];
            if (pass == 0) {
                if (changed)
                    entry.index = -1;
                const auto visual =
                    SpellIconVisual::make(slot, wizard.mana, wizard.cast_cost(*definition).first);
                if (visual.rectangle.size.x > 0)
                    paint.icon(definition->icon, visual.rectangle, (visual.alpha << 24) | 0xffffffu,
                               visual.additive);
                if (visual.additive && entry.index <= 0)
                    entry.index = -2;
                else if (!visual.additive && entry.index == -2)
                    entry.index = 0;
            } else if (pass == 1) {
                const auto flags = definition->spell ? definition->spell->property_flags : 0;
                if (definition->kind == 2 && wizard.soul_count < definition->required_souls) {
                    paint.sprite(id("spi2"), 0, {slot.origin, {slot.size.x * .5f, slot.size.y * .5f}});
                    if (entry.index <= 0)
                        entry.index = -2;
                } else if (((flags & 0x400) && !wizard.casting.requirements[1]) ||
                           ((flags & 0x200) && !wizard.casting.requirements[0]) ||
                           ((flags & 0x100) && !wizard.casting.requirements[2])) {
                    paint.icon(id("ncst"), slot);
                    if (entry.index <= 0)
                        entry.index = -2;
                }
            } else if (pass == 2 && entry.remaining > 0) {
                paint.mesh(spell_cooldown(slot, entry.remaining, entry.duration), true);
                if (entry.index <= 0)
                    entry.index = -2;
            } else if (pass == 3 && entry.index >= 0) {
                paint.sprite(
                    id("Ifls"), static_cast<unsigned>(entry.index >> 8),
                    {{slot.origin.x - (slot.size.x + 1) * .5f, slot.origin.y - (slot.size.y + 1) * .5f},
                     {slot.size.x * 2, slot.size.y * 2}},
                    0xffffffff, true);
                entry.index += 192;
                if (entry.index >= 16 * 256)
                    entry.index = -1;
            }
        }
    if (const auto hit = layout.hit(view.cursor, static_cast<unsigned>(visible.size())); hit && hover) {
        if (hit->tab) {
            constexpr std::array<const char*, 3> names{"Scr8", "Smag", "Sstr"};
            paint_tooltip(tooltips, view, {{text(id(names[hit->index])), 0xffffffff}});
        } else if (const auto* spell = registry.find(entries[visible[hit->index]].id))
            spell_tooltip(tooltips, view, wizard, *spell, text);
        return tooltips.font() != nullptr;
    }
    return false;
}
std::optional<Mesh> object_hover_bracket(const MatchObject& object, const HudCamera& camera) {
    const auto* motion = object.source_motion();
    if (!motion)
        return {};
    Box3 box{motion->box_min, motion->box_max};
    Orientation orientation = motion_orientation(0);
    if (const auto* actor = dynamic_cast<const CreatureActor*>(&object))
        if (const auto* bounds = actor->prepared_bounds()) {
            box = bounds->visual;
            orientation = motion->orientation;
        }
    std::array<Vec3, 8> projected;
    for (unsigned i = 0; i < projected.size(); ++i) {
        const Vec3 p{i & 1 ? box.maximum.x : box.minimum.x, i & 2 ? box.maximum.y : box.minimum.y,
                     i & 4 ? box.maximum.z : box.minimum.z};
        const Vec3 d{motion->position.x + p.x * orientation[0] + p.y * orientation[3] + p.z * orientation[6] -
                         camera.eye.x,
                     motion->position.z + p.x * orientation[2] + p.y * orientation[5] + p.z * orientation[8] -
                         camera.eye.y,
                     motion->position.y + p.x * orientation[1] + p.y * orientation[4] + p.z * orientation[7] -
                         camera.eye.z};
        projected[i] = {(d.x * camera.right.x + d.y * camera.right.y + d.z * camera.right.z) /
                            camera.tan_width,
                        -(d.x * camera.up.x + d.y * camera.up.y + d.z * camera.up.z) / camera.tan_height,
                        d.x * camera.forward.x + d.y * camera.forward.y + d.z * camera.forward.z};
    }
    if (const auto rectangle = hover_screen_bounds(projected, camera.width, camera.height)) {
        const auto* side = object.allegiance;
        return hover_bracket_mesh(
            *rectangle, camera.width,
            side ? unit_side_colour(side->definition.index == 31 ? 8 : side->definition.color) : 0xff000000u);
    }
    return {};
}
PointerFeedback pointer_feedback(PlayerPointer& pointer, const PlayerControl& player, const Match& match,
                                 MatchObject* target, Vec3 point, bool ground, FourCC context, bool menu,
                                 bool dragging, bool blocked, float hover_ticks) {
    PointerFeedback result;
    result.context = context.value ? context : PlayerPointer::context(player, target, ground);
    const auto mode = id(menu                     ? "xmnu"
                         : dragging               ? "drag"
                         : player.selecting_box() ? "mdrg"
                         : player.pending.value   ? "targ"
                                                  : "norm");
    result.icon = pointer.icon(mode, result.context);
    if (player.pending == id("cast") && player.pending_spell.value && match.spell_registry)
        if (const auto* spell = match.spell_registry->find(player.pending_spell); spell && spell->icon.value)
            result.icon = spell->icon;
    if (player.pending.value && player.target_check(target, point, ground)) {
        result.alpha = 128;
        result.warning = id("ncst");
    }
    result.bracket = target && pointer.retained(match) == target && !blocked && !dragging && !menu &&
                     !player.selecting_box();
    const bool card =
        target && !blocked && !menu && !dragging && !player.selecting_box() && result.context != id("sds!");
    pointer.hover(card ? target->reference() : 0, result.context, std::clamp(hover_ticks, 0.0f, 60.0f));
    result.tooltip = card && pointer.ready();
    if (!result.icon.value)
        result.icon = id("Cnor");
    return result;
}
bool paint_pointer(HudPainter& paint, const PointerFeedback& feedback, Vec2 pixel) {
    const auto* header = paint.header(id("ICON"), feedback.icon);
    if (!header)
        return false;
    const auto h = *header;
    const Vec2 at{pixel.x - h.hotspot_x - .5f, pixel.y - h.hotspot_y - .5f};
    paint.icon(feedback.icon, {at, {static_cast<float>(h.frame_width), static_cast<float>(h.frame_height)}},
               (feedback.alpha << 24) | 0xffffffu);
    if (feedback.warning.value)
        if (const auto* warning = paint.header(id("ICON"), feedback.warning))
            paint.icon(
                feedback.warning,
                {at, {static_cast<float>(warning->frame_width), static_cast<float>(warning->frame_height)}});
    return true;
}
bool paint_selection_panel(HudPainter& paint, HudPainter& tooltips, const HudViewport& view,
                           const PlayerControl& player, const SpellRegistry& registry, FourCC ability,
                           bool hover, const HudText& text) {
    const auto layout = SelectionPanelLayout::make(view.width, view.height);
    const auto* frame = paint.header(id("TXTR"), id("fram"));
    if (!frame || !frame->width || !frame->height)
        return false;
    const float tx = .5f / frame->width, ty = .5f / frame->height, x = layout.bounds.origin.x - .5f,
                y = layout.bounds.origin.y - .5f, split = x + layout.divider,
                right = x + layout.bounds.size.x, middle = y + layout.bounds.size.y * .5f,
                bottom = y + layout.bounds.size.y;
    paint.image(id("fram"), {{x, y}, {split - x, middle - y}}, {.25f, ty}, {tx, 1 - ty});
    paint.image(id("fram"), {{split, y}, {right - split, middle - y}}, {tx, ty}, {.5f - tx, 1 - ty});
    paint.image(id("fram"), {{x, middle}, {split - x, bottom - middle}}, {.25f, 1 - ty}, {tx, ty});
    paint.image(id("fram"), {{split, middle}, {right - split, bottom - middle}}, {tx, 1 - ty},
                {.5f - tx, ty});
    bool card = false;
    const auto& entries = player.selection.entries();
    for (unsigned i = 0; i < layout.slots.size() && i < entries.size(); ++i) {
        const auto* actor = dynamic_cast<const CreatureActor*>(entries[i].object);
        if (!actor)
            continue;
        const auto& r = layout.slots[i];
        paint.icon(actor->definition().icon, {{r.origin.x - .5f, r.origin.y - .5f}, {r.size.y, r.size.y}});
        const auto meter = [&](float at, int value, int maximum, std::uint32_t colour) {
            const float fraction = std::clamp(static_cast<float>(value) / std::max(maximum, 1), 0.f, 1.f),
                        end = r.origin.y + r.size.y - 1, top = end - (r.size.y - 2) * fraction;
            paint.fill({{at, r.origin.y + 1}, {2, end - r.origin.y - 1}}, (colour & 0xffffff) | 0x40000000);
            paint.fill({{at, top}, {2, end - top}}, colour);
        };
        meter(r.origin.x + r.size.y, actor->stats.current[CreatureStats::health],
              actor->stats.base[CreatureStats::health], 0xff0000c0);
        if (actor->maximum_mana)
            meter(r.origin.x + r.size.y + (r.size.x - r.size.y) * .5f, actor->mana, actor->maximum_mana,
                  0xffc06000);
        if (hover && r.contains(view.cursor)) {
            paint_tooltip(tooltips, view, {{text(actor->definition().id), 0xffffffff}});
            card = true;
        }
    }
    if (const auto* spell = ability.value ? registry.find(ability) : nullptr) {
        const auto& r = layout.ability;
        paint.icon(spell->icon, {{r.origin.x - .5f, r.origin.y - .5f}, r.size});
        if (hover && r.contains(view.cursor)) {
            paint_tooltip(tooltips, view, {{text(spell->id), 0xffffffff}});
            card = true;
        }
    }
    return card && tooltips.font();
}
void paint_status_meters(HudPainter& paint, HudPainter& numbers, HudPainter& tooltips,
                         const HudViewport& view, const CreatureActor& wizard, unsigned locks,
                         const std::array<GradientBar, 2>& styles, bool hover, const HudText& text) {
    const float scale = hud_scale(view.width, view.height);
    constexpr float texel = .5f / 128;
    for (unsigned i = 0; i < 3; ++i) {
        if ((locks & (i == 2 ? 16 : 8)) || (i == 1 && !wizard.maximum_mana))
            continue;
        const float width = (i == 2 ? 64 : 32) * scale,
                    left = view.width - (i == 2 ? 128 : (i + 1) * 32) * scale;
        paint.image(id("fram"), {{left - .5f, -.5f}, {width, 48 * scale}}, {.5f + texel, texel},
                    {.75f - texel, .25f - texel});
        paint.image(id("fram"), {{left - .5f, 48 * scale - .5f}, {width, 48 * scale}},
                    {.5f + texel, .25f - texel}, {.75f - texel, texel});
        if (i == 2) {
            paint.sprite(id("spir"), (wizard.phase >> 2) & 7,
                         {{left + 4 * scale, 8 * scale}, {56 * scale, 80 * scale}}, 0xafffffff, true);
            if (const auto* font = numbers.font()) {
                const auto line = static_cast<float>(font->line_height());
                const HudRect box{{left + 4 * scale, 8 * scale + (80 * scale - line) * .5f},
                                  {56 * scale, line}};
                const auto count = std::to_string(wizard.soul_count);
                numbers.text({{box.origin.x + 1, box.origin.y + 1}, box.size}, count, 0xff000000, 1, 1);
                numbers.text(box, count, 0xffffffff, 1, 1);
            }
            continue;
        }
        const int value = i ? wizard.mana : wizard.stats.current[CreatureStats::health],
                  maximum = i ? wizard.maximum_mana : wizard.stats.base[CreatureStats::health];
        const auto fraction =
            static_cast<int>(std::clamp(static_cast<std::int64_t>(value) * 65536 / std::max(maximum, 1),
                                        std::int64_t{0}, std::int64_t{65536}));
        paint.mesh(styles[i].vertical({left + 4 * scale, 8 * scale}, {24 * scale, 80 * scale}, fraction));
        if (hover && HudRect{{left, 0}, {32 * scale, 96 * scale}}.contains(view.cursor))
            paint_tooltip(
                tooltips, view,
                {{std::format("{}: {}/{}", text(id(i ? "mbar" : "hbar")), value, maximum), 0xffffffff}});
    }
}
void paint_command_menu(HudPainter& paint, const CommandMenu& menu, unsigned frame) {
    if (!menu.visible())
        return;
    const auto at = menu.centre();
    constexpr std::array<Vec2, 5> offsets{{{0, 0}, {0, -40}, {0, 40}, {-40, 0}, {40, 0}}};
    const auto entries = menu.displayed();
    const auto* header = paint.header(id("TXTR"), id("iglo"));
    const auto frames = header ? std::max(header->frames, 1u) : 1;
    for (unsigned i = 0; i < entries.size(); ++i) {
        if (!entries[i])
            continue;
        const Vec2 pos{at.x + offsets[i].x - 16.5f, at.y + offsets[i].y - 16.5f};
        paint.icon(entries[i]->icon, {pos, {32, 32}});
        paint.sprite(id("iglo"), frame % frames, {{pos.x - 4, pos.y - 4}, {40, 40}}, 0xffffffff, true);
    }
}
void paint_unit_hud(HudPainter& paint, const CreatureActor& actor, Vec2 anchor, float depth, unsigned width,
                    bool selected, bool health_only) {
    const auto layout = UnitHudLayout::make(anchor, depth, width);
    const auto* side = actor.allegiance;
    auto tint =
        side ? unit_side_colour(side->definition.index == 31 ? 8 : side->definition.color) : 0xff000000;
    if (actor.timer && (actor.phase & 0x1f) <= 0xf)
        tint = 0xff0000ff;
    paint.sprite(id("sarr"), 0, layout.arrow, tint);
    if (!selected && !health_only)
        return;
    const std::array<float, 2> fractions{static_cast<float>(actor.stats.current[CreatureStats::health]) /
                                             std::max<int>(actor.stats.base[CreatureStats::health], 1),
                                         static_cast<float>(actor.mana) / std::max(actor.maximum_mana, 1)};
    constexpr std::array<std::uint32_t, 2> colours{0xff0000ff, 0xffff7f00};
    for (unsigned i = 0; i < (actor.maximum_mana && !health_only ? 2u : 1u); ++i) {
        const auto& r = layout.bars[i];
        const float filled = r.size.x * std::clamp(fractions[i], 0.f, 1.f);
        const auto c = colours[i], dim = ((c >> 1) & 0x7f7f7f7f) | 0xff000000u;
        paint.fill({r.origin, {filled, r.size.y}}, c);
        paint.fill({{r.origin.x + filled, r.origin.y}, {r.size.x - filled, r.size.y}}, dim);
    }
}
bool unit_hud_visible(const PlayerControl& player, const CreatureActor& actor) {
    if (!actor.allegiance || (actor.state_flags & MatchObject::removed_flag) || (actor.common_status & 0x80))
        return false;
    const auto& side = player.side();
    if (actor.allegiance == &side)
        return player.selection.contains(&actor);
    if (!actor.alive())
        return false;
    const auto known = std::ranges::find(side.known, &actor, &MatchSide::Known::object);
    return known != side.known.end() && known->stamp + 12 > side.stamp;
}
void paint_minimap(HudPainter& paint, const HudViewport& view, const Match& match,
                   const PlayerControl& player, const Terrain& terrain, const MiniMapTransform& transform,
                   float yaw, float tan_width) {
    if (!terrain.width || !terrain.height)
        return;
    const auto layout = MiniMapLayout::make(view.width, view.height);
    Mesh land, background;
    land.vertices.push_back(
        {{layout.centre.x, layout.centre.y, 0},
         {},
         {transform.centre.x / (terrain.width * 10.f), transform.centre.y / (terrain.height * 10.f)}});
    background.vertices.push_back({{layout.centre.x, layout.centre.y, 0}, {}, {}, 0xff000000});
    for (unsigned i = 0; i <= 24; ++i) {
        const float angle = i * (6.2831853f / 24), s = std::sin(angle), c = std::cos(angle);
        const auto point = transform.world({.5f + s * .46875f, .5f + c * .46875f});
        land.vertices.push_back({{layout.centre.x + s * .46875f * layout.bounds.size.x,
                                  layout.centre.y + c * .46875f * layout.bounds.size.y, 0},
                                 {},
                                 {point.x / (terrain.width * 10.f), point.y / (terrain.height * 10.f)}});
        background.vertices.push_back({{layout.centre.x + s * .5f * layout.bounds.size.x,
                                        layout.centre.y + c * .5f * layout.bounds.size.y, 0},
                                       {},
                                       {},
                                       0xff000000});
        if (i) {
            land.indices.insert(land.indices.end(), {0, i, i + 1});
            background.indices.insert(background.indices.end(), {0, i, i + 1});
        }
    }
    paint.mesh(background);
    paint.mesh(land, false, id("#map"));
    const auto project = [&](Vec3 point) {
        const auto p = transform.normalized(point);
        return Vec2{layout.bounds.origin.x + p.x * layout.bounds.size.x,
                    layout.bounds.origin.y + p.y * layout.bounds.size.y};
    };
    const auto clip_line = [&](Vec2 a, Vec2 b) {
        const float dx = b.x - a.x, dy = b.y - a.y, x0 = a.x - layout.centre.x, y0 = a.y - layout.centre.y,
                    aa = dx * dx + dy * dy, bb = 2 * (x0 * dx + y0 * dy),
                    cc = x0 * x0 + y0 * y0 - layout.radius * layout.radius,
                    determinant = bb * bb - 4 * aa * cc;
        if (aa <= 0 || determinant < 0)
            return;
        const float lo = std::max(0.f, (-bb - std::sqrt(determinant)) / (2 * aa)),
                    hi = std::min(1.f, (-bb + std::sqrt(determinant)) / (2 * aa));
        if (lo >= hi)
            return;
        const float nx = -dy * .5f / std::sqrt(aa), ny = dx * .5f / std::sqrt(aa);
        Mesh line;
        for (float t : {lo, hi})
            for (float sign : {-1.f, 1.f})
                line.vertices.push_back(
                    {{a.x + dx * t + sign * nx, a.y + dy * t + sign * ny, 0}, {}, {}, 0xff00ffff});
        line.indices = {0, 1, 2, 1, 3, 2};
        paint.mesh(line);
    };
    if (const auto* wizard = player.wizard()) {
        const auto at = project(wizard->motion.position);
        const float half = std::atan(tan_width);
        for (float side : {-1.f, 1.f}) {
            auto point = wizard->motion.position;
            point.x += (std::sin(yaw) + side * std::cos(yaw) * std::sin(half)) * 200;
            point.y += (std::cos(yaw) - side * std::sin(yaw) * std::sin(half)) * 200;
            const auto end = project(point);
            clip_line(at, {at.x + (end.x - at.x) * 100, at.y + (end.y - at.y) * 100});
        }
    }
    for (const auto& object : match.objects())
        if (const auto marker = minimap_marker(match, player.side(), *object, transform, layout)) {
            const auto at = marker->centre;
            const auto size = marker->half_size;
            if (marker->textured) {
                paint.sprite(id("mmic"), marker->icon, {{at.x - size, at.y - size}, {2 * size, 2 * size}},
                             marker->colour, false, marker->angle);
            } else
                paint.fill({{at.x - size, at.y - size}, {2 * size, 2 * size}}, marker->colour);
        }
    const auto* header = paint.header(id("TXTR"), id("fram"));
    if (!header || !header->width || !header->height)
        return;
    const float tx = .5f / header->width, ty = .5f / header->height;
    Mesh ring;
    for (unsigned i = 0; i <= 24; ++i) {
        const float angle = i * (6.2831853f / 24), s = std::sin(angle), c = std::cos(angle);
        for (float r : {.5f, .4375f})
            ring.vertices.push_back({{layout.centre.x + s * r * layout.bounds.size.x,
                                      layout.centre.y + c * r * layout.bounds.size.y, 0},
                                     {},
                                     {.5f + tx + (1 - std::abs(s) * r * 2) * (.5f - 2 * tx),
                                      .5f + ty + (1 - std::abs(c) * r * 2) * (.5f - 2 * ty)}});
        if (i) {
            const auto a = (i - 1) * 2;
            ring.indices.insert(ring.indices.end(), {a, a + 1, a + 2, a + 1, a + 3, a + 2});
        }
    }
    paint.mesh(ring, false, id("fram"));
    const float s = std::sin(yaw), c = std::cos(yaw), size = layout.bounds.size.x * .075f,
                x = layout.centre.x + s * layout.bounds.size.x * .4765625f,
                y = layout.centre.y + c * layout.bounds.size.y * .4765625f;
    Mesh compass;
    for (const Vec2 p : {Vec2{-1, -1}, Vec2{1, -1}, Vec2{1, 1}, Vec2{-1, 1}})
        compass.vertices.push_back({{x + (p.x * c + p.y * s) * size, y + (-p.x * s + p.y * c) * size, 0},
                                    {},
                                    {p.x < 0 ? .75f + tx : 1 - tx, p.y < 0 ? ty : .25f - ty}});
    compass.indices = {0, 1, 2, 0, 2, 3};
    paint.mesh(compass, false, id("fram"));
}
} // namespace sacrifice
