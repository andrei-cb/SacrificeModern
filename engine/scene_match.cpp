#include "sacrifice/scene_match.hpp"
#include "sacrifice/building_actor.hpp"
#include "sacrifice/character.hpp"
#include "sacrifice/side_ai.hpp"
#include "sacrifice/soul_actor.hpp"
#include "sacrifice/spell_registry.hpp"
#include "sacrifice/spell_actor.hpp"
#include <algorithm>
#include <cmath>

namespace sacrifice {
namespace {
constexpr std::array<SideDefinition, 1> unbound_sides{{{31, 0, 0, 0, 0}}};
} // namespace
SceneMatch::SceneMatch(MapScene& scene, const Terrain& terrain, CrtRandom& random,
                       const TerrainMaterials* materials, const ResourceCatalog* catalog,
                       std::span<const SkirmishSlot> skirmish, bool campaign, const Character* carried_character)
    : initial_random_state(random.state), scene_(scene), catalog_(catalog), tiles_(terrain.tiles),
      sides_(scene.sides.empty() ? std::span<const SideDefinition>(unbound_sides)
                                 : std::span<const SideDefinition>(scene.sides)),
      triggers_(scene.triggers, sides_, scene.trigger_variables, scene.markers),
      match_(random, terrain.tileset) {
    triggers_.objectives = scene.objectives;
    if (catalog)
        triggers_.bind_presentation(*catalog);
    triggers_.text = [fallback = std::move(triggers_.text), captions = scene.trigger_text](FourCC id) {
        if (const auto found = captions.find(id); found != captions.end())
            return found->second;
        return fallback ? fallback(id) : std::string{};
    };
    triggers_.filename = [files = scene.trigger_files](FourCC id) {
        const auto found = files.find(id);
        if (found == files.end()) throw Error("Missing trigger FILE:" + id.text());
        return found->second;
    };
    if (materials) {
        tile_properties_ = materials->tile_properties;
        for (unsigned tile = 0; tile < 256; ++tile)
            present_tiles_[tile] = materials->tiles[tile].has_value();
    }
    match_.sound_set = scene.sound_set;
    match_.spell_registry = scene.spell_registry;
    if (catalog)
        for (const auto id : {FourCC::from_text("erpt"), FourCC::from_text("volc")})
            if (const auto profile = catalog->find_chunk(FourCC::from_text("DATA"), id))
                match_.spell_mounds.emplace(id, catalog->read(*profile));
    for (const auto& side : sides_.instances())
        side->match = &match_;
    match_.terrain = &terrain;
    match_.spatial = &spatial_;
    if (catalog)
        match_.texture_frames = [catalog, frames = std::map<std::uint32_t, unsigned>{}](FourCC id) mutable {
            auto [found, inserted] = frames.try_emplace(id.value, 1u);
            if (inserted)
                try {
                    if (const auto texture = catalog->find_chunk(FourCC::from_text("TXTR"), id))
                        found->second = std::max(decode_texture_header(catalog->read(*texture)).frames, 1u);
                } catch (const std::exception&) {
                }
            return found->second;
        };
    // Match options (0x4a1b10 defaults, 0x4a1e10 lobby branch, copied in by 0x456000): the LEVL start and
    // maximum wizard levels (capped at 9), the starting souls and the experience scale 32. A campaign
    // takes dwords 6..9 instead: souls, start and maximum level, and the mission god (+681).
    if (campaign) {
        match_.starting_souls = static_cast<std::uint8_t>(terrain.scenario[5]);
        match_.minimum_wizard_level = static_cast<std::uint8_t>(terrain.scenario[6]);
        match_.maximum_wizard_level = static_cast<std::uint8_t>(std::min(terrain.scenario[7], 9u));
        match_.mission_god = terrain.scenario[8];
        match_.experience_scale = 32;
    } else if (terrain.scenario[0]) {
        match_.starting_souls = static_cast<std::uint8_t>(terrain.scenario[2]);
        match_.minimum_wizard_level = static_cast<std::uint8_t>(terrain.scenario[3]);
        match_.maximum_wizard_level = static_cast<std::uint8_t>(std::min(terrain.scenario[4], 9u));
        match_.experience_scale = 32;
    }
    for (const auto& side : sides_.instances())
        match_.side_order.push_back(side.get());
    for (const auto& model : scene.models)
        if (model.soul && !model.asset.textures.empty()) {
            soul_texture_ = *model.asset.textures.front();
            break;
        }
    if (catalog_ || soul_texture_)
        match_.create_souls = [this](Vec3 point, MatchObject* source, int amount) {
            return create_souls(point, source, amount);
        };
    if (catalog_) {
        match_.create_spell_model = [this](SpellActor& spell) {
            auto vacant = std::ranges::find_if(scene_.models, [&](const SceneModel& model) {
                return model.spell_visual && !model.spell_visual->object && model.id == spell.model_id;
            });
            const auto slot = static_cast<std::size_t>(vacant - scene_.models.begin());
            if (vacant == scene_.models.end()) {
                const auto folder = catalog_->find_chunk(FourCC::from_text("MRMC"), spell.model_id);
                const auto record = folder ? catalog_->find_chunk(FourCC::from_text("MRMM"), spell.model_id, *folder) : std::nullopt;
                if (!record)
                    throw Error("Missing spell model:" + spell.model_id.text());
                SceneModel model{spell.model_id};
                model.asset = ModelAsset::load(*catalog_, *record);
                model.placements.emplace_back();
                model.spell_visual = SceneModel::SpellVisual{};
                scene_.models.push_back(std::move(model));
            }
            scene_.models[slot].spell_visual->object = &spell;
            scene_.models[slot].spell_visual->uploaded_frame = -1;
            spell.clear_preview = [this, slot] { scene_.models[slot].spell_visual->object = nullptr; };
        };
        match_.create_summon_preview = [this](SpellActor& spell) {
            if (spell.effect == SpellActor::Effect::speedup_echo) {
                // Echoes copy the hasted target, which may be a wizard whose resources load under 'WIZD'.
                const auto* parent = dynamic_cast<const SpellActor*>(spell.effect_target);
                const auto* target = parent ? dynamic_cast<const CreatureActor*>(parent->effect_target) : nullptr;
                prepare_creature(spell.summon_id, target && target->definition().kind == 8u
                                                      ? FourCC::from_text("WIZD") : FourCC::from_text("CRE8"));
            }
            const auto& resources = creature_resources(spell.summon_id);
            const auto* clip = spell.effect == SpellActor::Effect::speedup_echo ? spell.echo.clip.get() :
                (resources.rules->clips[56] ? resources.rules->clips[56] : resources.rules->clips[0]).get();
            if (!clip)
                return;
            auto vacant = std::ranges::find_if(scene_.models, [&](const SceneModel& model) {
                return model.summon && !model.summon->object && model.summon->creature == spell.summon_id;
            });
            std::size_t slot = static_cast<std::size_t>(vacant - scene_.models.begin());
            if (vacant == scene_.models.end()) {
                auto model = SceneModel::make_character(resources);
                model.character.reset(); // A frozen SAXS preview, with no creature or simulation activation.
                model.summon = SceneModel::SummonPreview{spell.summon_id};
                model.asset.mesh = saxs_mesh(resources.body->model, resources.body->descriptor,
                                             *resources.topology, saxs_pose(resources.body->model, clip->animation,
                                                 spell.effect == SpellActor::Effect::speedup_echo ? spell.echo.frame : 0));
                model.placements.emplace_back();
                scene_.models.push_back(std::move(model));
            }
            scene_.models[slot].summon->object = &spell;
            scene_.models[slot].summon->body = resources.body;
            scene_.models[slot].summon->topology = resources.topology;
            scene_.models[slot].summon->clip = spell.effect == SpellActor::Effect::speedup_echo ? spell.echo.clip :
                (resources.rules->clips[56] ? resources.rules->clips[56] : resources.rules->clips[0]);
            scene_.models[slot].summon->uploaded_pose = 0;
            spell.clear_preview = [this, slot] { scene_.models[slot].summon->object = nullptr; };
        };
        match_.prepare_creature = [this](FourCC id) { return prepare_creature(id); };
        match_.creature_definition = [this](FourCC id) -> const CreatureDefinition* {
            try {
                prepare_creature(id);
                return &creature_resources(id).rules->definition;
            } catch (const Error&) {
                return nullptr;
            }
        };
        match_.create_script_creature = [this](MatchSide& side, FourCC id, Vec3 point) {
            return create_script_creature(side, id, point);
        };
        match_.create_creature = [this](CreatureActor& caster, FourCC id, Vec3 point,
                                        const MatchObject* after) {
            return create_creature(caster, id, point, after);
        };
    }
    if (materials) {
        navigation_ = std::make_unique<Navigation>(NavigationGrid::build(terrain, *materials));
        match_.navigation = navigation_.get();
        navigation_->rebuild_static = [this](Navigation& navigation) {
            // 0x470260 -> 0x465c40, newest building first. Run(0) refreshes bounds/grounding before
            // stamping collision boxes whose lower face lies within one unit of the ground.
            match_.visit_group(2, 16, [&](MatchObject& object) {
                if (auto* building = dynamic_cast<BuildingActor*>(&object); building && building->model()) {
                    building->run(match_, 0);
                    if (building->state_flags & 2)
                        for (const auto& box : building->motion.collision_boxes)
                            if (box.minimum.z <= 1) {
                                const std::array<float, 6> bounds{box.minimum.x, box.minimum.y,
                                                                  box.minimum.z, box.maximum.x,
                                                                  box.maximum.y, box.maximum.z};
                                navigation.stamp(2, building->motion.position, bounds, 6, true);
                            }
                }
                return 0;
            });
            match_.visit_group(4, 32, [&](MatchObject& object) {
                if (auto* spell = dynamic_cast<SpellActor*>(&object))
                    spell->stamp_navigation(navigation);
                return 0;
            });
        };
    }
    match_.spell_models = scene.spell_models;
    dynamic_models_ = scene.models.size();
    if (catalog_)
        match_.create_building = [this](FourCC id, Vec3 position, float yaw, int side, std::uint32_t flags,
                                        MatchObject* controller, MatchObject* reservation) {
            return create_building(id, position, yaw, side, flags, controller, reservation);
        };
    try {
        // Fountains created while configuring ruined altars append already-bound spawns.
        const auto map_spawns = scene.spawns.size();
        for (std::size_t index = 0; index < map_spawns; ++index) {
            auto& spawn = scene.spawns[index];
            auto* allegiance = sides_.get(spawn.kind == MapScene::Spawn::Kind::soul ? 31 : spawn.side);
            if (!allegiance)
                throw Error("Map scene side table has no neutral side 31");
            const auto side = reinterpret_cast<std::uintptr_t>(allegiance);
            if (spawn.kind == MapScene::Spawn::Kind::building) {
                const auto& group = scene.building_groups.at(spawn.building_group);
                const auto count = group.definition.parts.size();
                if (group.first_spawn != index || !count || count > map_spawns - index)
                    throw Error("Invalid building spawn group");
                // Factory 0x457cb0 activates all parts, links the primary, then configures all parts.
                for (std::size_t part = 0; part < count; ++part)
                    scene.spawns[index + part].object = &match_.create(
                        MatchObject::building, [] { return std::make_unique<BuildingActor>(); });
                auto* primary =
                    static_cast<BuildingActor*>(scene.spawns[index + group.definition.primary_part()].object);
                const auto group_flags = spawn.flags;
                // Configuring a ruined altar may create its fountain and grow the spawn list.
                for (std::size_t part = 0; part < count; ++part) {
                    const auto placed = scene.spawns[index + part];
                    auto& object = static_cast<BuildingActor&>(*placed.object);
                    allegiance->attach(object);
                    object.primary = &object == primary ? nullptr : primary;
                    const auto& p = placed.placement;
                    std::array<const MrmGeometry*, 3> models{};
                    for (unsigned state = 0; state < 3; ++state)
                        if (placed.state_models[state] != SIZE_MAX) {
                            const auto model = placed.state_models[state];
                            const auto geometry =
                                building_geometry_.try_emplace(model, scene.models.at(model).asset.geometry)
                                    .first;
                            models[state] = &geometry->second;
                        }
                    const unsigned state = placed.flags & 0x10000 ? 2 : 0;
                    object.initialize(models[state], {p.position.x, p.position.z, p.position.y}, p.yaw,
                                      placed.flags, side, terrain, spatial_);
                    object.configure(match_, group.definition, placed.building_part, models, state);
                    object.definition_id = group.id;
                    if (placed.flags & 0x40000)
                        object.reveal = 0;
                }
                primary->completed = !(group_flags & 0x40000);
                primary->script_id = spawn.script_id;
                if ((group.definition.flags & 0x80000000) && !(group_flags & 0x20000))
                    stamp_footprint(group.position, group.definition);
                primary->requests.push_back({BuildingActor::Request::Kind::side_event, 14});
                index += count - 1;
                continue;
            }
            auto& model = scene.models.at(spawn.model);
            if (spawn.kind == MapScene::Spawn::Kind::character) {
                auto& character = *model.character;
                const auto type =
                    character.rules->definition.kind == 8 ? MatchObject::wizard : MatchObject::creature;
                auto& object = static_cast<CreatureActor&>(match_.create(
                    type, [&] { return std::make_unique<CreatureActor>(character.rules); }, nullptr,
                    [&](MatchObject& reused) {
                        auto& actor = static_cast<CreatureActor&>(reused);
                        actor.configure(character.rules);
                        actor.initial_wizard_profile.reset();
                        if (type == MatchObject::wizard && spawn.wizard_level >= 0)
                            actor.initial_wizard_profile = {spawn.wizard_level, spawn.wizard_god};
                    }));
                character.object = &object;
                allegiance->attach(object);
                const auto& p = spawn.placement;
                object.motion.position = {p.position.x, p.position.z, p.position.y};
                object.motion.yaw = p.yaw;
                object.motion.orientation = motion_orientation(p.yaw);
                object.terrain = &terrain;
                object.bind_body(character.body);
                object.initialize_model(match_);
                object.spatial_side = side;
                object.bind_spatial(spatial_);
                // 0x458240: map flag 4 places the creature already dead (FSM event 26 -> DETH/FDTH), as
                // pr01's battlefield bodies are; it runs before the status flags below.
                if (spawn.flags & 4)
                    object.fsm().event(26);
                object.common_status |= 0x10 | (spawn.flags & 0x10 ? 1 : 0) | (spawn.flags & 2 ? 8 : 0) |
                                      (spawn.flags & 0x40000000 ? 2 : 0) |
                                      (spawn.flags & 0x20000000 ? 4 : 0) |
                                      (spawn.flags & 0x2000000 ? 0x100 : 0);
                if (spawn.flags & 8)
                    object.stats.current[5] = object.stats.target[5] = object.stats.base[5] / 10;
                object.script_id = spawn.script_id;
                spawn.object = &object;
            } else {
                auto& object = static_cast<SoulActor&>(
                    match_.create(MatchObject::soul, [] { return std::make_unique<SoulActor>(); }, nullptr,
                                  [&](MatchObject& reused) {
                                      for (auto& prior : scene_.models)
                                          if (prior.soul_object == &reused) {
                                              prior.soul_object = nullptr;
                                              prior.visible.assign(1, false);
                                          }
                                      for (auto& prior : scene_.spawns)
                                          if (prior.object == &reused)
                                              prior.object = nullptr;
                                  }));
                const auto spawn_model = spawn.model;
                const auto p = spawn.placement.position;
                const auto frames = scene.models.at(spawn_model).asset.textures.front()->header.frames;
                allegiance->attach(object);
                object.initialize(match_, {p.x, p.z, p.y - 2}, nullptr, 1, frames);
                object.bind_spatial(spatial_);
                bind_soul_visual(object, {spawn_model});
                spawn.object = &object;
            }
        }
        // Resolve 0x457530 / 0x4818a0 links after every NTTS record exists. Some maps serialize
        // their wizard before the altar; keep activation/RNG order while completing those links.
        for (auto& spawn : scene.spawns)
            if (spawn.kind == MapScene::Spawn::Kind::character && spawn.wizard_level >= 0)
                if (auto* wizard = dynamic_cast<CreatureActor*>(spawn.object);
                    wizard && wizard->type == MatchObject::wizard)
                    match_.visit_group(2, 16, [&](MatchObject& candidate) {
                        auto* altar = dynamic_cast<BuildingActor*>(&candidate);
                        if (altar && !altar->primary && altar->allegiance == wizard->allegiance &&
                            (altar->building_flags & 1) &&
                            (!altar->controller || altar->controller == wizard)) {
                            altar->controller = wizard;
                            wizard->home_altar = altar;
                            return -1;
                        }
                        return 0;
                    });
        // Complete side-controller links for records that preceded their side's wizard.
        for (const auto& object : match_.objects())
            if ((object->kind_flags & 0x14) && !object->controller && object->allegiance)
                object->controller = object->allegiance->wizard();
        // 0x4a9d70: slot n takes the first listed side whose flag nibble is n.
        std::uint32_t skirmish_sides = 0;
        for (unsigned slot = 1; slot <= 5 && slot <= skirmish.size(); ++slot) {
            const auto& entry = skirmish[slot - 1];
            MatchSide* side = nullptr;
            for (auto* candidate : match_.side_order) {
                const auto nibble = (candidate->flags & 0xf) ? (candidate->flags & 0xf) : (candidate->flags & 0x20 ? 1u : 0u);
                if (nibble == slot) {
                    side = candidate;
                    break;
                }
            }
            // An observer slot only places the camera (0x46b200), which awaits the player host.
            if (!side || (entry.kind != SkirmishSlot::Kind::local && entry.kind != SkirmishSlot::Kind::computer))
                continue;
            const bool computer = entry.kind == SkirmishSlot::Kind::computer;
            if (computer)
                side->flags |= 0x40; // 0x4565f0 later gives such sides the AI controller (0x471f60).
            else
                match_.local_side = side;
            auto god = entry.god;
            if (campaign && !computer && carried_character) {
                // 0x457750 calls 0x4a0440 before placement and wizard activation consume rand().
                // Keep the character untouched until CampaignSession captures its restart snapshot.
                auto carried = *carried_character;
                god = carried.enter_mission(match_.mission_god, match_.minimum_wizard_level, match_.random());
                match_.campaign_god = god;
            }
            if (!god)
                god = static_cast<std::uint8_t>(match_.random().next() % 5 + 1);
            else if (god > 5)
                throw Error("Invalid skirmish god");
            if (create_wizard(*side, entry.wizard, god, computer) && !campaign)
                skirmish_sides |= side->identity;
        }
        // Lobby teams: the native alliance site for slot +3 is not recovered; rows sharing a nonzero team
        // are made mutual allies before normalization, which then keeps both directions consistent.
        std::array<MatchSide*, 5> team_sides{};
        for (unsigned slot = 1; slot <= 5 && slot <= skirmish.size(); ++slot)
            for (auto* candidate : match_.side_order) {
                const auto nibble =
                    (candidate->flags & 0xf) ? (candidate->flags & 0xf) : (candidate->flags & 0x20 ? 1u : 0u);
                if (nibble == slot) {
                    team_sides[slot - 1] = skirmish[slot - 1].team ? candidate : nullptr;
                    break;
                }
            }
        for (unsigned a = 0; a < 5; ++a)
            for (unsigned b = 0; b < 5; ++b)
                if (a != b && team_sides[a] && team_sides[b] && skirmish[a].team == skirmish[b].team) {
                    team_sides[a]->allies |= team_sides[b]->identity;
                    team_sides[a]->enemies &= ~team_sides[b]->identity;
                }
        // 0x456510: relation normalization for each listed side, then trigger instances.
        for (auto* side : match_.side_order)
            side->normalize(sides_);
        sync_buildings();
        triggers_.start(skirmish_sides);
        match_.triggers = &triggers_;
        // 0x4565f0: settings flag 0x8000000 gives every computer side its controller (0x471f60(16, 1.0)).
        // Hosts without the resource catalog have no building/creature factory for its casts.
        for (auto* side : match_.side_order)
            if ((side->flags & 0x40) && !side->controller && match_.create_building)
                side->controller = std::make_unique<SideController>(match_, *side, 16, 1.0f);
    } catch (...) {
        clear_bindings();
        throw;
    }
}
void SceneMatch::clear_bindings() {
    for (auto& spawn : scene_.spawns)
        spawn.object = nullptr;
    for (auto& model : scene_.models)
        if (model.character)
            model.character->object = nullptr;
        else if (model.soul)
            model.soul_object = nullptr;
        else if (model.summon)
            model.summon->object = nullptr;
        else if (model.spell_visual)
            model.spell_visual->object = nullptr;
}
SceneMatch::~SceneMatch() {
    clear_bindings();
}
void SceneMatch::bind_soul_visual(SoulActor& actor, std::vector<std::size_t> models) {
    actor.sync_visual = [this, models = std::move(models)](SoulActor& object, bool visible) {
        const bool bundle = object.count > 1 && object.count <= 8;
        double angle = static_cast<double>(object.phase) * .02617991666666666;
        const float step = static_cast<float>(6.28318 / std::max(object.count, 1));
        for (const auto index : models) {
            auto& model = scene_.models.at(index);
            model.soul = object.visual;
            model.soul_object = visible ? &object : nullptr;
            model.visible.assign(1, visible);
            Vec3 p = object.motion.position;
            if (bundle) {
                model.soul->phase_x = static_cast<float>(angle + object.visual.phase_x);
                model.soul->phase_y = static_cast<float>(angle + object.visual.phase_y);
                model.soul->scale = static_cast<float>(1.0 - (object.count - 1) * .1);
                p.x = static_cast<float>(std::sin(angle) * .300000011920929 + p.x);
                p.y = static_cast<float>(std::cos(angle) * .300000011920929 + p.y);
                angle += step;
            }
            model.placements.assign(1, {{p.x, p.z, p.y}});
        }
    };
    actor.sync_visual(actor, true);
}
SoulActor* SceneMatch::create_souls(Vec3 point, MatchObject* source, int amount) {
    if (!amount)
        return nullptr;
    if (!soul_texture_) {
        if (!catalog_)
            throw Error("Soul creation requires the spir texture resource");
        const auto texture = catalog_->find_chunk(FourCC::from_text("TXTR"), FourCC::from_text("spir"));
        if (!texture)
            throw Error("Missing soul TXTR:spir");
        soul_texture_ = ModelAsset::Texture{decode_texture_header(catalog_->read(*texture)),
                                            load_texture(*catalog_, *texture), *texture};
    }
    auto& actor = static_cast<SoulActor&>(match_.create(
        MatchObject::soul, [] { return std::make_unique<SoulActor>(); }, source,
        [&](MatchObject& reused) {
            for (auto& model : scene_.models)
                if (model.soul_object == &reused) {
                    model.soul_object = nullptr;
                    model.visible.assign(1, false);
                }
            for (auto& spawn : scene_.spawns)
                if (spawn.object == &reused)
                    spawn.object = nullptr;
        }));
    if (!source)
        if (auto* neutral = sides_.get(31))
            neutral->attach(actor);
    actor.initialize(match_, point, source, amount, soul_texture_->header.frames);
    actor.bind_spatial(spatial_);
    const unsigned sprites = amount > 1 && amount <= 8 ? static_cast<unsigned>(amount) : 1;
    std::vector<std::size_t> models;
    for (unsigned sprite = 0; sprite < sprites; ++sprite) {
        auto vacant = std::ranges::find_if(scene_.models, [&](const SceneModel& model) {
            const auto index = static_cast<std::size_t>(&model - scene_.models.data());
            return model.soul && !model.soul_object &&
                   std::ranges::find(models, index) == models.end() &&
                   !std::ranges::any_of(scene_.spawns, [&](const MapScene::Spawn& spawn) {
                       return spawn.kind == MapScene::Spawn::Kind::soul && !spawn.object && spawn.model == index;
                   });
        });
        if (vacant == scene_.models.end()) {
            SceneModel model{FourCC::from_text("spir")};
            model.screen_oriented = true;
            model.soul = actor.visual;
            model.asset.mesh = soul_mesh(actor.visual, soul_texture_->header);
            model.asset.mesh.materials.push_back({0x0a011008, model.id});
            model.asset.textures.push_back(*soul_texture_);
            scene_.models.push_back(std::move(model));
            models.push_back(scene_.models.size() - 1);
        } else
            models.push_back(static_cast<std::size_t>(vacant - scene_.models.begin()));
    }
    bind_soul_visual(actor, std::move(models));
    return &actor;
}
std::vector<SoulActor*> SceneMatch::souls() const {
    std::vector<SoulActor*> result;
    for (const auto& object : match_.objects())
        if (auto* soul = dynamic_cast<SoulActor*>(object.get()); soul && !(soul->soul_flags & 0x80000000))
            result.push_back(soul);
    return result;
}
bool SceneMatch::prepare_creature(FourCC id, FourCC tag) {
    const auto ready = [&] {
        const auto& clips = creature_resources(id).rules->clips;
        return clips[56] || clips[0]; // 0x434f10: preferred summon pose, then animation zero.
    };
    if (creature_resources_.contains(id))
        return ready();
    for (const auto& model : scene_.models)
        if (model.character && model.character->rules->definition.kind == (tag == FourCC::from_text("WIZD") ? 8u : 2u) &&
            model.character->rules->definition.id == id) {
            auto resources = *model.character;
            resources.object = nullptr;
            resources.uploaded_pose = 0;
            creature_resources_.emplace(id, std::move(resources));
            return ready();
        }
    if (!catalog_)
        throw Error("Dynamic creatures require a resource catalog");
    auto model = SceneModel::load_character(*catalog_, id, tag);
    creature_resources_.emplace(id, std::move(*model.character));
    return ready();
}
CreatureActor* SceneMatch::create_creature(CreatureActor& caster, FourCC id, Vec3 point,
                                           const MatchObject* after) {
    if (!caster.allegiance || !match_.terrain)
        throw Error("Creature creation requires a side and terrain");
    prepare_creature(id);
    auto model = SceneModel::make_character(creature_resources(id));
    const auto resources = *model.character;
    auto& actor = static_cast<CreatureActor&>(match_.create(
        MatchObject::creature, [&] { return std::make_unique<CreatureActor>(resources.rules); },
        after ? after : &caster,
        [&](MatchObject& reused) {
            for (auto& prior : scene_.models)
                if (prior.character && prior.character->object == &reused)
                    prior.character->object = nullptr;
            for (auto& spawn : scene_.spawns)
                if (spawn.object == &reused)
                    spawn.object = nullptr;
            auto& creature = static_cast<CreatureActor&>(reused);
            creature.configure(resources.rules);
            creature.initial_wizard_profile.reset();
        }));
    const auto& source = after && after->source_motion() ? *after->source_motion() : caster.motion;
    actor.motion.orientation = source.orientation;
    actor.motion.yaw = source.yaw;
    actor.motion.pitch = source.pitch;
    actor.motion.roll = source.roll;
    actor.motion.position = {point.x, point.y,
                             static_cast<float>(terrain_surface(*match_.terrain, point.x, point.y))};
    caster.allegiance->attach(actor);
    actor.terrain = match_.terrain;
    actor.bind_body(resources.body);
    actor.initialize_model(match_);
    actor.spatial_side = reinterpret_cast<std::uintptr_t>(caster.allegiance);
    actor.bind_spatial(spatial_);
    if (!(actor.status_flags & 0x100) && !(actor.fsm().state()->flags & 0x2000) && (caster.kind_flags & 1))
        caster.allegiance->event(caster, &actor, 8);
    bind_character_model(actor, std::move(model), id);
    return &actor;
}
CreatureActor* SceneMatch::create_script_creature(MatchSide& side, FourCC id, Vec3 point) {
    if (!match_.terrain)
        throw Error("Creature creation requires a side and terrain");
    prepare_creature(id);
    auto model = SceneModel::make_character(creature_resources(id));
    const auto resources = *model.character;
    auto& actor = static_cast<CreatureActor&>(match_.create(
        MatchObject::creature, [&] { return std::make_unique<CreatureActor>(resources.rules); },
        nullptr,
        [&](MatchObject& reused) {
            for (auto& prior : scene_.models)
                if (prior.character && prior.character->object == &reused)
                    prior.character->object = nullptr;
            for (auto& spawn : scene_.spawns)
                if (spawn.object == &reused)
                    spawn.object = nullptr;
            auto& creature = static_cast<CreatureActor&>(reused);
            creature.configure(resources.rules);
            creature.initial_wizard_profile.reset();
        }));
    actor.script_id = 0;
    actor.motion.orientation = {1, 0, 0, 0, 1, 0, 0, 0, 1};
    actor.motion.yaw = actor.motion.pitch = actor.motion.roll = 0;
    actor.motion.position = {point.x, point.y,
                             static_cast<float>(terrain_surface(*match_.terrain, point.x, point.y))};
    side.attach(actor);
    actor.terrain = match_.terrain;
    actor.bind_body(resources.body);
    actor.initialize_model(match_);
    actor.spatial_side = reinterpret_cast<std::uintptr_t>(&side);
    actor.bind_spatial(spatial_);
    bind_character_model(actor, std::move(model), id);
    return &actor;
}
void SceneMatch::bind_character_model(CreatureActor& actor, SceneModel model, FourCC id) {
    const auto& resources = *model.character;
    model.character->object = &actor;
    model.asset.mesh =
        saxs_mesh(resources.body->model, resources.body->descriptor, *resources.topology, actor.model_pose());
    model.character->uploaded_pose = actor.pose_revision();
    const auto& p = actor.motion.position;
    model.placements.push_back({{p.x, p.z, p.y}, actor.motion.yaw});
    const auto vacant = std::ranges::find_if(scene_.models, [&](const SceneModel& candidate) {
        return candidate.character && !candidate.character->object &&
               candidate.character->rules->definition.id == id;
    });
    if (vacant != scene_.models.end()) {
        vacant->character = std::move(model.character);
        vacant->character->uploaded_pose = 0; // Re-upload this activation's pose into existing GPU buffers.
        vacant->placements = std::move(model.placements);
        vacant->asset.mesh = std::move(model.asset.mesh);
    } else {
        const bool wizard = model.character->rules->definition.kind == 8;
        scene_.models.push_back(std::move(model));
        ++(wizard ? scene_.wizards : scene_.creatures);
    }
}
CreatureActor* SceneMatch::create_wizard(MatchSide& side, FourCC id, std::uint8_t god, bool computer) {
    if (!match_.terrain)
        throw Error("Wizard creation requires terrain");
    BuildingActor* altar = nullptr; // 0x457530 over the side's building group.
    for (auto* object : side.objects[2])
        if (auto* building = dynamic_cast<BuildingActor*>(object);
            !altar && building && (building->kind_flags & MatchObject::building) && !building->controller &&
            !building->primary && (building->building_flags & 1))
            altar = building;
    if (!altar)
        return nullptr;
    const auto& terrain = *match_.terrain;
    // 0x457690: toward the nearest complete fountain-like primary (0x4575a0), else the map centre.
    Vec3 target{1280, 1280, 0};
    target.z = static_cast<float>(terrain_true_height(terrain, target.x, target.y));
    const auto& from = altar->motion.position;
    bool found = false;
    float best = 0;
    match_.visit_group(2, MatchObject::building, [&](MatchObject& object) {
        auto* building = dynamic_cast<BuildingActor*>(&object);
        if (building && !building->primary && (building->building_flags & 0x2810) == 0x2810) {
            const auto& p = building->motion.position;
            const float dx = from.x - p.x, dy = from.y - p.y, dz = from.z - p.z;
            const float distance = dz * dz + dy * dy + dx * dx;
            if (!found || distance < best) {
                found = true;
                best = distance;
                target = p;
            }
        }
        return 0;
    });
    const double dx = target.x - from.x, dy = target.y - from.y;
    const double scale = 15.0 / std::sqrt(dy * dy + dx * dx);
    const auto ox = static_cast<float>(scale * dx), oy = static_cast<float>(scale * dy);
    Vec3 position{ox + from.x, oy + from.y, 0};
    position.z = static_cast<float>(terrain_true_height(terrain, position.x, position.y));
    const auto yaw = std::hypot(ox, oy) == 0 ? 0.0f : static_cast<float>(std::atan2(-ox, oy)); // 0x410920
    if (position.x >= 0 && position.y >= 0 && position.x < 2560 && position.y < 2560)
        position.z = terrain_height(terrain, position.x, position.y);
    prepare_creature(id, FourCC::from_text("WIZD"));
    auto model = SceneModel::make_character(creature_resources(id));
    const auto resources = *model.character;
    // 0x467d50(1, 0): newest pooled wizard or a new one, at the head of the active list.
    auto& actor = static_cast<CreatureActor&>(match_.create(
        MatchObject::wizard, [&] { return std::make_unique<CreatureActor>(resources.rules); }, nullptr,
        [&](MatchObject& reused) {
            for (auto& prior : scene_.models)
                if (prior.character && prior.character->object == &reused)
                    prior.character->object = nullptr;
            for (auto& spawn : scene_.spawns)
                if (spawn.object == &reused)
                    spawn.object = nullptr;
            auto& wizard = static_cast<CreatureActor&>(reused);
            wizard.configure(resources.rules);
            wizard.initial_wizard_profile = CreatureActor::WizardProfile{match_.minimum_wizard_level, god};
        }));
    side.attach(actor);
    actor.computer_wizard = computer;
    actor.home_altar = altar;
    altar->controller = &actor;
    if (match_.starting_souls) // 0x457860: the match's starting souls (+669).
        actor.change_souls(match_.starting_souls);
    actor.motion.position = position;
    actor.motion.yaw = yaw;
    actor.motion.orientation = motion_orientation(yaw);
    actor.view_yaw = yaw; // 0x457860 +744.
    actor.terrain = match_.terrain;
    actor.bind_body(resources.body);
    actor.initialize_model(match_);
    actor.spatial_side = reinterpret_cast<std::uintptr_t>(&side);
    actor.bind_spatial(spatial_);
    bind_character_model(actor, std::move(model), id);
    return &actor;
}
std::size_t SceneMatch::building_model(FourCC id) {
    if (const auto released = free_building_models_.find(id); released != free_building_models_.end()) {
        const auto index = released->second;
        free_building_models_.erase(released);
        return index;
    }
    // One placement per dynamic part: uploaded batches cannot gain placements after append_scene.
    SceneModel model{id};
    const auto shared = std::ranges::find_if(scene_.models, [&](const SceneModel& candidate) {
        return candidate.id == id && !candidate.character && !candidate.soul && !candidate.widget_range &&
               !candidate.asset.geometry.frames.empty();
    });
    if (shared != scene_.models.end())
        model.asset = shared->asset;
    else {
        const auto folder = catalog_->find_chunk(FourCC::from_text("MRMC"), id);
        const auto mesh = folder ? catalog_->find_chunk(FourCC::from_text("MRMM"), id, *folder) : std::nullopt;
        if (!mesh)
            throw Error("Missing building model MRMC:" + id.text());
        model.asset = ModelAsset::load(*catalog_, *mesh);
    }
    model.placements.push_back({});
    model.visible.assign(1, false);
    model.reveal.assign(1, 1);
    const auto index = scene_.models.size();
    building_geometry_.insert_or_assign(index, model.asset.geometry);
    scene_.models.push_back(std::move(model));
    return index;
}
std::uint8_t SceneMatch::tile(unsigned cell) const {
    return cell < tiles_.size() ? tiles_[cell] : 0;
}
const SceneModel::Character& SceneMatch::creature_resources(FourCC id, std::source_location where) const {
    const auto found = creature_resources_.find(id);
    if (found == creature_resources_.end())
        throw Error("Creature resources were not prepared: " + id.text() + " at line " + std::to_string(where.line()));
    return found->second;
}
void SceneMatch::stamp_footprint(Vec3 position, const Building& definition) {
    const auto& terrain = *match_.terrain;
    // 0x44cfa0 (cells 8 x 8): the first cell centre is 35.5 units below the position on both axes.
    float x = static_cast<float>(position.x - (8 * 5.0 - 4.5)),
          y = static_cast<float>(position.y - (8 * 5.0 - 4.5));
    const auto* cell = definition.footprint.data();
    for (int row = 0; row < 8; ++row) {
        for (int column = 0; column < 8; ++column, ++cell) {
            // LAND::GetTileProperties is null for tiles absent from the tileset.
            if (*cell && tile_properties_ && present_tiles_[*cell]) {
                // LAND::SetTile(point, tiles, 1, 1): -(__int64)((p + 0.05) * -0.1f), bounds checked,
                // then one MSVCRT rand() (shared with the game) picks the only tile.
                const auto tx = -static_cast<std::int64_t>((x + 0.05) * -0.1000000014901161),
                           ty = -static_cast<std::int64_t>((y + 0.05) * -0.1000000014901161);
                if (tx >= 0 && ty >= 0 && tx < terrain.width && ty < terrain.height) {
                    match_.random().next();
                    const auto index = static_cast<unsigned>(ty * terrain.width + tx);
                    if (index < tiles_.size()) {
                        tiles_[index] = *cell; // A scarred word (>= 0x100) releases its scar first.
                        tile_changes_.push_back({index, *cell, ++match_.terrain_revision});
                        if (navigation_)
                            navigation_->retile(index, (*tile_properties_)[*cell]);
                    }
                }
            }
            x = static_cast<float>(x + 10.0);
        }
        x = static_cast<float>(x - 8 * 10.0);
        y = static_cast<float>(y + 10.0);
    }
}
BuildingActor* SceneMatch::create_building(FourCC id, Vec3 position, float yaw, int side,
                                           std::uint32_t flags, MatchObject* controller,
                                           MatchObject* reservation) {
    if (!catalog_ || !match_.terrain)
        throw Error("Dynamic buildings require a resource catalog and terrain");
    // Offline 0x498ae0/0x498bd0 always permit creation; a wizard controller supplies its side (+456).
    if (controller && !(controller->kind_flags & 1))
        controller = nullptr;
    auto* allegiance = controller ? controller->allegiance : sides_.get(side);
    if (!allegiance)
        throw Error("Non existant side");
    auto [cached, inserted] = building_definitions_.try_emplace(id);
    if (inserted)
        if (const auto location = catalog_->find_chunk(FourCC::from_text("BLDG"), id))
            cached->second = Building::decode(catalog_->read(*location));
    if (!cached->second || cached->second->parts.empty())
        return nullptr; // "Cannot find building" / "Building has no elements".
    if ((cached->second->flags & 0x20) && !reservation)
        return nullptr; // "No mana spout specified".
    const auto& terrain = *match_.terrain;
    const auto group_index = scene_.building_groups.size();
    scene_.building_groups.push_back({*cached->second, scene_.spawns.size(), position, id});
    const auto& definition = scene_.building_groups.back().definition;
    const auto count = definition.parts.size();
    // Pass one (0x467d50 per part): activate and place. Reused pooled parts release their old bindings.
    std::vector<BuildingActor*> parts;
    const double c = std::cos(yaw), s = std::sin(yaw);
    for (std::size_t part = 0; part < count; ++part) {
        auto& object = static_cast<BuildingActor&>(match_.create(
            MatchObject::building, [] { return std::make_unique<BuildingActor>(); }, nullptr,
            [&](MatchObject& reused) {
                for (auto& spawn : scene_.spawns)
                    if (spawn.object == &reused) {
                        spawn.object = nullptr;
                        for (auto& model : spawn.state_models)
                            if (model != SIZE_MAX && model >= dynamic_models_) {
                                scene_.models[model].visible.assign(1, false);
                                free_building_models_.emplace(scene_.models[model].id, model);
                                model = SIZE_MAX;
                            }
                    }
            }));
        parts.push_back(&object);
        const auto& record = definition.parts[part];
        // 0x410bc0 / 0x4119b0 row-vector rotation, then LAND::GetHeight; part angle in native degrees.
        const auto x = static_cast<float>(record.offset.x * c - record.offset.y * s + position.x);
        const auto y = static_cast<float>(record.offset.x * s + record.offset.y * c + position.y);
        const auto part_yaw = static_cast<float>(record.angle * 0.01745327777777778 + yaw);
        MapScene::Spawn spawn{MapScene::Spawn::Kind::building, SIZE_MAX, 0,
                              {{x, terrain_height(terrain, x, y), y}, part_yaw}, flags, side};
        spawn.object = &object;
        spawn.building_group = group_index;
        spawn.building_part = static_cast<unsigned>(part);
        std::map<std::uint32_t, std::size_t> placed;
        for (unsigned state = 0; state < 3; ++state)
            if (const auto selected = record.model_at(state, definition.flags); selected.value) {
                auto found = placed.find(selected.value);
                if (found == placed.end())
                    found = placed.emplace(selected.value, building_model(selected)).first;
                spawn.state_models[state] = found->second;
                scene_.models[found->second].placements.front() = spawn.placement;
            }
        spawn.model = spawn.state_models[flags & 0x10000 ? 2 : 0];
        scene_.spawns.push_back(spawn);
        ++scene_.parts;
    }
    ++scene_.buildings;
    auto* primary = parts[definition.primary_part()];
    // Pass two (0x4651f0 with 0x46a780): link, configure and apply the map-style flags.
    const auto first = scene_.building_groups[group_index].first_spawn;
    for (std::size_t part = 0; part < count; ++part) {
        auto& object = *parts[part];
        const auto placed = scene_.spawns[first + part];
        allegiance->attach(object);
        object.controller = controller;
        object.primary = &object == primary ? nullptr : primary;
        std::array<const MrmGeometry*, 3> models{};
        for (unsigned state = 0; state < 3; ++state)
            if (placed.state_models[state] != SIZE_MAX)
                models[state] = &building_geometry_.at(placed.state_models[state]);
        const unsigned state = (flags >> 15) & 2;
        const auto& p = placed.placement;
        object.initialize(models[state], {p.position.x, p.position.z, p.position.y}, p.yaw, flags,
                          reinterpret_cast<std::uintptr_t>(allegiance), terrain, spatial_);
        object.configure(match_, definition, placed.building_part, models, state);
        object.definition_id = id;
        if (flags & 0x40000)
            object.reveal = 0;
    }
    primary->completed = !(flags & 0x40000);
    if ((cached->second->flags & 0x80000000) && !(flags & 0x20000))
        stamp_footprint(position, *cached->second);
    // 0x464ff0 with the spout as `this`: the new building reserves it, replacing a cast's reservation.
    if (auto* spout = dynamic_cast<BuildingActor*>(reservation))
        spout->reserve(primary, 2);
    primary->requests.push_back({BuildingActor::Request::Kind::side_event, 14});
    if (primary->allegiance) // Dynamic creation only: map buildings are placed before statistics count.
        primary->allegiance->event(*primary, nullptr, 14);
    return primary;
}
void SceneMatch::prepare_models() {
    for (auto& model : scene_.models)
        if (model.character && model.character->object)
            model.character->object->prepare_model();
    sync_buildings();
}
void SceneMatch::sync_buildings() {
    for (const auto& spawn : scene_.spawns)
        if (spawn.kind == MapScene::Spawn::Kind::building && !spawn.object) {
            // A pooled part reused by a dynamic building leaves its old placements hidden.
            for (unsigned state = 0; state < 3; ++state)
                if (spawn.state_models[state] != SIZE_MAX)
                    scene_.models[spawn.state_models[state]].visible[spawn.state_placements[state]] = false;
        } else if (spawn.kind == MapScene::Spawn::Kind::building) {
            const auto& actor = static_cast<const BuildingActor&>(*spawn.object);
            for (unsigned state = 0; state < 3; ++state)
                if (spawn.state_models[state] != SIZE_MAX) {
                    auto& model = scene_.models[spawn.state_models[state]];
                    model.visible[spawn.state_placements[state]] = false;
                    model.reveal.resize(model.placements.size(), 1);
                    model.reveal[spawn.state_placements[state]] = actor.reveal;
                }
            const auto state = actor.stage();
            if (state < 3 && spawn.state_models[state] != SIZE_MAX)
                scene_.models[spawn.state_models[state]].visible[spawn.state_placements[state]] =
                    !(actor.state_flags & MatchObject::removed_flag);
        }
}
} // namespace sacrifice
