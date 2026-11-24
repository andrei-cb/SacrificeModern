#include "sacrifice/scene.hpp"
#include "sacrifice/mesh_import.hpp"
#include "sacrifice/widgets.hpp"
#include "sacrifice/creature.hpp"
#include "sacrifice/saxs.hpp"
#include "sacrifice/spell_registry.hpp"
#include <algorithm>
#include <cctype>
#include <cmath>
#include <climits>
#include <cstring>
#include <format>
#include <set>

namespace sacrifice {
namespace {
constexpr float degrees = 0.01745327777777778f;          // Constant used by 0x4581e0 and 0x457cb0.
constexpr float creature_degrees = 0.01745277777777778f; // Constant used by 0x458300.
bool finite(Vec3 v) {
    return std::isfinite(v.x) && std::isfinite(v.y) && std::isfinite(v.z);
}
Vec3 vec3(Reader& r) {
    Vec3 v;
    v.x = r.f32();
    v.y = r.f32();
    v.z = r.f32();
    return v;
}
} // namespace
MapEntity MapEntity::decode(const EntityRecord& record) {
    Reader r(record.payload, "NTTS entity");
    MapEntity entity;
    entity.type = record.type;
    entity.position = vec3(r);
    if (!finite(entity.position))
        throw Error(std::format("NTTS record at {} has a non-finite position", record.offset));
    if (r.remaining() >= 8) {
        entity.angle = r.f32();
        entity.id = {r.u32()};
        if (r.remaining() >= 8 &&
            (entity.type == 2 || entity.type == 4 || entity.type == 64 || entity.type == 4096)) {
            entity.flags = r.u32();
            entity.side = std::bit_cast<std::int32_t>(r.u32());
            if (r.remaining() >= 8) {
                r.u32();
                entity.script_id = r.u32();
                if (entity.type == 2 && r.remaining() >= 4)
                    entity.reservation_id = r.u32();
                if (entity.type == 4096 && r.remaining() >= 8) {
                    entity.extent.x = r.f32();
                    entity.extent.y = r.f32();
                }
                if (entity.type == 4 && r.remaining() >= 12) {
                    entity.wizard_level = std::bit_cast<std::int32_t>(r.u32());
                    r.u32();
                    entity.wizard_god = static_cast<std::uint8_t>(r.u32());
                }
            }
        }
    }
    return entity;
}
Building Building::decode(ByteView data) {
    Reader r(data, "BLDG");
    Building building;
    building.flags = r.u32();
    const auto footprint = r.take(64);
    std::ranges::transform(footprint, building.footprint.begin(),
                           [](std::byte b) { return static_cast<std::uint8_t>(b); });
    building.maximum_health = std::bit_cast<std::int32_t>(r.u32());
    building.threshold_fraction = r.f32();
    building.threshold_complement = r.f32();
    building.regeneration_seconds = r.f32();
    building.mana_rate = r.f32();
    for (auto& value : building.parameters)
        value = r.f32();
    building.property = {r.u32()};
    building.sound = {r.u32()};
    const auto count = r.u32();
    building.primary = {r.u32()};
    // 0x457cb0 keeps part objects in a 32-entry stack array.
    if (count == 0 || count > 32 || r.remaining() != count * 48ull)
        throw Error("Invalid BLDG part table");
    for (std::uint32_t i = 0; i < count; ++i) {
        Part part;
        part.name = {r.u32()};
        for (auto& model : part.models)
            model = {r.u32()};
        part.model = part.models[0];
        part.properties = r.u32();
        part.offset = vec3(r);
        part.angle = r.f32();
        part.start_spell = {r.u32()};
        part.spell_40 = {r.u32()};
        part.destroyed_spell = {r.u32()};
        if (!finite(part.offset) || !std::isfinite(part.angle))
            throw Error("Non-finite BLDG part placement");
        building.parts.push_back(part);
    }
    return building;
}
unsigned Building::primary_part() const {
    if (primary.value)
        for (unsigned i = 0; i < parts.size(); ++i)
            if (parts[i].name == primary)
                return i;
    return 0;
}
FourCC Building::Part::model_at(unsigned state, std::uint32_t building_flags) const {
    if (state >= models.size())
        throw Error("Building model state outside 0..2");
    return models[building_flags & 1 ? 0 : state];
}
WidgetModel WidgetModel::decode(ByteView data) {
    Reader r(data, "WIDG");
    WidgetModel model;
    const auto points = r.u32();
    // The second word is a u16 triangle count and u16 flags (1 in the ethr/JA_A/ST_A sets).
    const std::uint32_t triangles = r.u16();
    model.flags = r.u16();
    model.texture = {r.u32()};
    if (!points || points > 256 || !triangles || r.remaining() != points * 20ull + triangles * 4ull)
        throw Error("Invalid WIDG counts");
    for (std::uint32_t i = 0; i < points; ++i) {
        Point point;
        point.position = vec3(r);
        point.uv = {r.f32(), r.f32()};
        if (!finite(point.position) || !std::isfinite(point.uv.x) || !std::isfinite(point.uv.y))
            throw Error("Non-finite WIDG point");
        model.points.push_back(point);
    }
    for (std::uint32_t i = 0; i < triangles; ++i) {
        std::array<std::uint8_t, 3> t{r.u8(), r.u8(), r.u8()};
        r.u8();
        if (t[0] >= points || t[1] >= points || t[2] >= points)
            throw Error("WIDG triangle outside points");
        model.triangles.push_back(t);
    }
    return model;
}
float terrain_height(const Terrain& terrain, float x, float y, bool true_height) {
    // LAND::GetHeight 0x16040ed0 / GetPlane 0x16040730: outside the map, or on a triangle touching a
    // hole (height bit 15), the height is zero.
    if (!(x >= 0 && y >= 0 && x < terrain.width * 10.0 && y < terrain.height * 10.0))
        return 0;
    const auto cx = static_cast<unsigned>(static_cast<std::int64_t>(x * 0.1)),
               cy = static_cast<unsigned>(static_cast<std::int64_t>(y * 0.1));
    if (cx >= terrain.width || cy >= terrain.height)
        return 0;
    // On the last row/column the corners read past the edge as the original buffer lays them out.
    const auto x1 = cx + 1, y1 = cy + 1;
    const std::uint16_t r00 = terrain.raw(cx, cy), r10 = terrain.raw(x1, cy), r01 = terrain.raw(cx, y1),
                        r11 = terrain.raw(x1, y1);
    const double h00 = terrain.at(cx, cy), h10 = terrain.at(x1, cy), h01 = terrain.at(cx, y1),
                 h11 = terrain.at(x1, y1);
    const double fx = x - cx * 10.0, fy = y - cy * 10.0;
    const auto hole = [&](std::uint16_t value) { return !true_height && (value & 0x8000) != 0; };
    if (terrain.main_diagonal(cx, cy)) {
        // Diagonal (0,0)-(1,1).
        if (fy <= fx) {
            if (hole(r00 | r10 | r11))
                return 0;
            return static_cast<float>(h00 + (h10 - h00) * fx / 10 + (h11 - h10) * fy / 10);
        }
        if (hole(r00 | r01 | r11))
            return 0;
        return static_cast<float>(h00 + (h11 - h01) * fx / 10 + (h01 - h00) * fy / 10);
    }
    // Diagonal (1,0)-(0,1).
    if (10.0 - fy >= fx) {
        if (hole(r00 | r10 | r01))
            return 0;
        return static_cast<float>(h00 + (h10 - h00) * fx / 10 + (h01 - h00) * fy / 10);
    }
    if (hole(r10 | r01 | r11))
        return 0;
    return static_cast<float>(h11 + (h11 - h01) * (fx - 10) / 10 + (h11 - h10) * (fy - 10) / 10);
}
SceneModel SceneModel::make_character(Character resources) {
    SceneModel model{resources.rules->definition.body};
    model.character = std::move(resources);
    auto& character = *model.character;
    const auto& rules = *character.rules;
    const auto initial = rules.fsm.resolve(rules.fsm.initial);
    const auto slot = initial ? rules.fsm.at(*initial).animation : -1;
    auto clip = slot >= 0 ? rules.clips[slot] : nullptr;
    if (!clip || clip->animation.bones != rules.bones)
        for (const auto& candidate : rules.clips)
            if (candidate && candidate->animation.bones == rules.bones) {
                clip = candidate;
                break;
            }
    const auto& body = *character.body;
    model.asset.mesh =
        saxs_mesh(body.model, body.descriptor, *character.topology,
                  clip ? saxs_pose(body.model, clip->animation, 0) : saxs_bind_pose(body.model));
    for (const auto& texture : body.textures)
        model.asset.textures.push_back(texture);
    return model;
}
SceneModel SceneModel::load_character(const ResourceCatalog& catalog, FourCC id, FourCC tag) {
    const auto definition_chunk = catalog.find_chunk(tag, id);
    if (!definition_chunk)
        throw Error("Missing " + tag.text() + ":" + id.text());
    const auto definition = CreatureDefinition::decode(catalog.read(*definition_chunk));
    const auto folder = catalog.find_chunk(FourCC::from_text("SAXC"), definition.body);
    if (!folder)
        throw Error("Missing summon SAXC:" + definition.body.text());
    auto body = std::make_shared<SaxsAsset>(SaxsAsset::load(catalog, *folder));
    auto topology = std::make_shared<SaxsTopology>(saxs_topology(body->model, body->descriptor));
    auto rules = std::make_shared<CreatureActorResources>(
        CreatureActorResources::load(catalog, definition, static_cast<unsigned>(body->model.bones.size())));
    return make_character({std::move(rules), std::move(body), std::move(topology)});
}
MapScene MapScene::compose(const ResourceCatalog& catalog, const Archive& map, const Terrain& terrain) {
    MapScene scene;
    scene.spell_registry = std::make_shared<SpellRegistry>(SpellRegistry::load(catalog));
    for (const auto& group : scene.spell_registry->groups())
        for (const auto& definition : group.definitions) {
            if (!definition.spell)
                continue;
            const auto run = definition.spell->run_id;
            FourCC id;
            const auto parameter = [&](const char* key, const char* fallback) {
                return FourCC{static_cast<unsigned>(definition.spell->integer(FourCC::from_text(key), FourCC::from_text(fallback).value))};
            };
            if (run == spell_function_id("FireballRun") || run == spell_function_id("VolcanoRun") ||
                run == spell_function_id("FrozenGroundStart"))
                id = parameter("tmod", "bold");
            else if (run == spell_function_id("RockRun"))
                id = parameter("tmod", "rokm");
            else if (run == spell_function_id("RockClawpultStart"))
                id = parameter("cmod", "rokm");
            else if (run == spell_function_id("KlapponStart") || run == spell_function_id("BoulderSquatRun"))
                id = FourCC::from_text("roks");
            else if (run == spell_function_id("EruptRun") || run == spell_function_id("HaloOfEarthRun"))
                id = FourCC::from_text("roks"); // 0x444950 / 0x438760 debris chunks.
            else if (run == spell_function_id("BovineInterventionStart"))
                id = FourCC::from_text("bovi");
            else if (run == spell_function_id("DeathSpellRun"))
                id = FourCC::from_text("deth");
            else if (run == spell_function_id("Dragonfire"))
                id = parameter("tmdl", "dfir");
            else if (run == spell_function_id("BlindRageRun"))
                id = FourCC::from_text("rage");
            else if (run == spell_function_id("MeteorShowerRun"))
                id = FourCC::from_text("rock");
            else if (run == spell_function_id("DecomposeStart") || run == spell_function_id("BoreRun"))
                id = parameter("tmod", "rock");
            else if (run == spell_function_id("RockFormRun"))
                id = FourCC{static_cast<unsigned>(definition.spell->integer(FourCC::from_text("tmdl"), 0))};
            if (!id.value || scene.spell_models.contains(id))
                continue;
            const auto folder = catalog.find_chunk(FourCC::from_text("MRMC"), id);
            const auto mesh = folder ? catalog.find_chunk(FourCC::from_text("MRMM"), id, *folder) : std::nullopt;
            if (!mesh)
                throw Error("Missing spell MRMC:" + id.text());
            scene.spell_models.emplace(id, std::make_shared<MrmGeometry>(MrmGeometry::decode(catalog.read(*mesh))));
        }
    if (const auto sides = map.find(FourCC::from_text("SIDS")))
        scene.sides = SideDefinition::decode(map.read(*sides));
    if (const auto trig = map.find(FourCC::from_text("TRIG")))
        scene.triggers = TriggerDefinition::decode(map.read(*trig));
    if (const auto language = map.find_chunk(FourCC::from_text("LANG"), FourCC::from_text("ENGL")))
        if (const auto folder = map.find_chunk(FourCC::from_text("FLDR"), FourCC::from_text("TRIG"), *language))
            for (auto i = *folder + 1; i < map.records()[*folder].subtree_end; ++i)
                if (map.records()[i].tag == FourCC::from_text("TEXT") && map.records()[i].parent == *folder) {
                    const auto bytes = map.read(i);
                    const auto* begin = reinterpret_cast<const char*>(bytes.data());
                    scene.trigger_text.emplace(map.records()[i].name, std::string(begin, strnlen(begin, bytes.size())));
                }
    if (const auto folder = map.find_chunk(FourCC::from_text("FLDR"), FourCC::from_text("FILE")))
        for (auto i = *folder + 1; i < map.records()[*folder].subtree_end; ++i)
            if (map.records()[i].tag == FourCC::from_text("TEXT") && map.records()[i].parent == *folder) {
                const auto bytes = map.read(i);
                const auto* begin = reinterpret_cast<const char*>(bytes.data());
                scene.trigger_files.emplace(map.records()[i].name, std::string(begin, strnlen(begin, bytes.size())));
            }
    if (const auto record = map.find(FourCC::from_text("MOBJ"))) {
        const auto bytes = map.read(*record);
        Reader reader(bytes, "MOBJ");
        const auto count = reader.u32();
        if (count > reader.remaining() / 8 || reader.remaining() != count * 8ull)
            throw Error("Invalid mission objective table");
        const auto language = map.find_chunk(FourCC::from_text("LANG"), FourCC::from_text("ENGL"));
        const auto captions = language ? map.find_chunk(FourCC::from_text("FLDR"), FourCC::from_text("MOBJ"), *language) : std::nullopt;
        for (unsigned i = 0; i < count; ++i) {
            Triggers::Objective objective{FourCC{reader.u32()}, reader.u32(), {}};
            if (captions)
                if (const auto label = map.find_chunk(FourCC::from_text("TEXT"), objective.text, *captions)) {
                    const auto text = map.read(*label);
                    objective.caption.assign(reinterpret_cast<const char*>(text.data()),
                        strnlen(reinterpret_cast<const char*>(text.data()), text.size()));
                }
            scene.objectives.push_back(std::move(objective));
        }
    }
    // 0x47d200: a TVAR chunk packs a count and C strings; maps store a TVAR folder of TEXT records whose
    // enumeration order assigns the ids (0x47d760).
    auto tvar = map.find_chunk(FourCC::from_text("TVAR"));
    if (!tvar)
        tvar = map.find_chunk(FourCC::from_text("FLDR"), FourCC::from_text("TVAR"));
    if (tvar) {
        const auto& folder = map.records()[*tvar];
        const auto text = [](const Bytes& bytes, std::size_t at) {
            const auto* begin = reinterpret_cast<const char*>(bytes.data() + at);
            return std::string(begin, strnlen(begin, bytes.size() - at));
        };
        if (folder.folder()) {
            for (auto i = *tvar + 1; i < folder.subtree_end; ++i)
                if (map.records()[i].parent == *tvar)
                    scene.trigger_variables.push_back(text(map.read(i), 0));
        } else {
            const auto bytes = map.read(*tvar);
            Reader r(bytes, "TVAR");
            const auto count = r.u32();
            for (std::size_t i = 0, at = 4; i < count; ++i) {
                if (at >= bytes.size())
                    throw Error("Error reading TVAR");
                scene.trigger_variables.push_back(text(bytes, at));
                at += scene.trigger_variables.back().size() + 1;
            }
        }
    }
    if (const auto envi = map.find(FourCC::from_text("ENVI")))
        scene.environment = Environment::decode(map.read(*envi));
    if (const auto sounds = catalog.find_chunk(FourCC::from_text("SSET"), FourCC::from_text("spel")))
        scene.sound_set = std::make_shared<SoundSet>(SoundSet::decode(catalog.read(*sounds)));
    else
        scene.unresolved.push_back("missing SSET:spel");
    const auto ntts = map.find(FourCC::from_text("NTTS"));
    if (!ntts)
        return scene;
    std::map<std::uint32_t, std::size_t> models; // Model ID -> scene.models index, or SIZE_MAX if missing.
    std::vector<std::shared_ptr<WidgetField>> fields; // Given the clearings once every building is known.
#if 0
    // Just for testing: replace Trial map trees with random pine .obj models from obj/ when Remastered Look is active.
    std::vector<SceneModel> remastered_trees;
    {
        std::string label(map.label());
        for (char& ch : label)
            ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
        if (label.find("trial") != std::string::npos) {
            const auto& pines = load_obj_folder("obj", 1.0f);
            remastered_trees.reserve(pines.size());
            for (std::size_t pi = 0; pi < pines.size(); ++pi) {
                SceneModel m{FourCC::from_text("pine")};
                m.asset = pines[pi];
                m.look_mode = 2; // Remastered Look only.
                remastered_trees.push_back(std::move(m));
            }
        }
    }
    const auto is_trial_tree = [](FourCC id) {
        const auto name = id.text();
        return name == "pct1" || name == "pct2" || name == "pct4" || name == "pct5" || name == "pct7" ||
               name == "ptre";
    };
#endif
    std::set<std::string> unresolved;
    const auto missing = [&](std::string text) {
        if (unresolved.insert(text).second)
            scene.unresolved.push_back(std::move(text));
    };
    const auto instance = [&](FourCC id) -> SceneModel* {
        auto [found, inserted] = models.try_emplace(id.value, SIZE_MAX);
        if (inserted) {
            try {
                SceneModel model{id};
                const auto folder = catalog.find_chunk(FourCC::from_text("MRMC"), id);
                const auto mesh =
                    folder ? catalog.find_chunk(FourCC::from_text("MRMM"), id, *folder) : std::nullopt;
                if (!mesh)
                    throw Error("missing MRMC:" + id.text());
                model.asset = ModelAsset::load(catalog, *mesh);
                found->second = scene.models.size();
                scene.models.push_back(std::move(model));
            } catch (const std::exception& e) {
                missing(e.what());
            }
        }
        return found->second == SIZE_MAX ? nullptr : &scene.models[found->second];
    };
    std::map<FourCC, std::pair<std::shared_ptr<const SaxsAsset>, std::shared_ptr<const SaxsTopology>>> bodies;
    std::map<std::pair<FourCC, std::uint32_t>, std::shared_ptr<const CreatureActorResources>> characters;
    const auto character = [&](CreatureDefinition definition) -> SceneModel* {
        try {
            auto& body = bodies[definition.body];
            if (!body.first) {
                const auto folder = catalog.find_chunk(FourCC::from_text("SAXC"), definition.body);
                if (!folder)
                    throw Error("missing SAXC:" + definition.body.text());
                auto asset = std::make_shared<SaxsAsset>(SaxsAsset::load(catalog, *folder));
                auto topology =
                    std::make_shared<SaxsTopology>(saxs_topology(asset->model, asset->descriptor));
                body = {std::move(asset), std::move(topology)};
            }
            auto& rules = characters[{definition.id, definition.kind}];
            if (!rules)
                rules = std::make_shared<CreatureActorResources>(CreatureActorResources::load(
                    catalog, definition, static_cast<unsigned>(body.first->model.bones.size())));
            // Each placement needs independent vertices and FSM state; definitions and clip keys are shared.
            scene.models.push_back(SceneModel::make_character({rules, body.first, body.second}));
            return &scene.models.back();
        } catch (const std::exception& e) {
            missing("CRE8/WIZD:" + definition.id.text() + ": " + e.what());
            return nullptr;
        }
    };
    // Free souls (0x458480 -> 0x475c90) are game3d FLAG primitives: screen oriented, material 0x0a011008,
    // global TXTR:spir, one model each so their waves and frames advance independently.
    std::optional<ModelAsset::Texture> soul_texture;
    bool soul_failed = false;
    const auto soul = [&]() -> SceneModel* {
        if (!soul_texture && !soul_failed) {
            try {
                const auto texture = catalog.find_chunk(FourCC::from_text("TXTR"), FourCC::from_text("spir"));
                if (!texture)
                    throw Error("missing TXTR:spir");
                const auto header = decode_texture_header(catalog.read(*texture));
                soul_texture = ModelAsset::Texture{header, load_texture(catalog, *texture), *texture};
            } catch (const std::exception& e) {
                soul_failed = true;
                missing(e.what());
            }
        }
        if (!soul_texture)
            return nullptr;
        SceneModel model{FourCC::from_text("spir")};
        model.screen_oriented = true;
        // Grown with zero phases; spawning it as a match object (spawn_soul) draws its phases.
        SoulState state;
        model.soul = state;
        model.asset.textures.push_back(*soul_texture);
        model.asset.mesh = soul_mesh(state, soul_texture->header);
        model.asset.mesh.materials.push_back({0x0a011008, model.id});
        scene.models.push_back(std::move(model));
        return &scene.models.back();
    };
    std::map<std::uint32_t, std::optional<std::pair<WidgetModel, ModelAsset::Texture>>> widget_models;
    // WIDGET::Draw selects lights with flags 0x10001, folding directional lights into the ambient at 0.7071
    // of their color x intensity; _3dngn::LightPoint then adds only runtime point lights, so a loaded map
    // lights every widget with clamp(ambient + 0.7071 x sun) and alpha 255.
    std::uint32_t widget_color = 0xffffffff;
    Vec3 widget_ambient{1, 1, 1};
    if (scene.environment) {
        const auto& e = *scene.environment;
        widget_ambient = {e.ambient.x * e.ambient_intensity + e.sun_color.x * e.sun_intensity * .70709997f,
                          e.ambient.y * e.ambient_intensity + e.sun_color.y * e.sun_intensity * .70709997f,
                          e.ambient.z * e.ambient_intensity + e.sun_color.z * e.sun_intensity * .70709997f};
        const auto channel = [](float ambient, float sun) {
            return static_cast<std::uint32_t>(std::clamp(ambient + sun * 0.70709997f, 0.0f, 1.0f) * 255.0f +
                                              0.5f);
        };
        widget_color = channel(e.ambient.x * e.ambient_intensity, e.sun_color.x * e.sun_intensity) |
                       channel(e.ambient.y * e.ambient_intensity, e.sun_color.y * e.sun_intensity) << 8 |
                       channel(e.ambient.z * e.ambient_intensity, e.sun_color.z * e.sun_intensity) << 16 |
                       0xff000000u;
    }
    for (const auto& record : decode_entities(map.read(*ntts))) {
        ++scene.kinds[record.type];
        if (record.type == static_cast<std::uint32_t>(EntityKind::WidgetField)) {
            Reader r(record.payload, "NTTS widget field");
            const FourCC id{r.u32()};
            const auto count = r.u32();
            if (r.remaining() != count * 12ull)
                throw Error(std::format("NTTS widget field at {} has an invalid entry count", record.offset));
            auto [found, inserted] = widget_models.try_emplace(id.value);
            if (inserted) {
                try {
                    const auto folder = catalog.find_chunk(FourCC::from_text("WIDC"), id);
                    const auto mesh =
                        folder ? catalog.find_chunk(FourCC::from_text("WIDG"), id, *folder) : std::nullopt;
                    if (!mesh)
                        throw Error("missing WIDC:" + id.text());
                    auto model = WidgetModel::decode(catalog.read(*mesh));
                    const auto texture =
                        catalog.find_chunk(FourCC::from_text("TXTR"), model.texture, *folder);
                    if (!texture)
                        throw Error("WIDC:" + id.text() + ": missing TXTR:" + model.texture.text());
                    const auto header = decode_texture_header(catalog.read(*texture));
                    ModelAsset::Texture image{
                        header, texture_frame(load_texture(catalog, *texture), header, 0), *texture};
                    found->second.emplace(std::move(model), std::move(image));
                } catch (const std::exception& e) {
                    missing(e.what());
                }
            }
            if (!found->second || !count)
                continue;
            // WIDGET::AddWidget 0x1602bcf0 stores (cos, sin) of the angle and LAND::GetHeight of the
            // position; DrawWidgetList 0x1602c760 transforms each point on the CPU without scaling.
            const auto& [widget, texture] = *found->second;
            SceneModel field{id};
            auto& mesh = field.asset.mesh;
            // WIDGETMODEL::Load 0x1602b5e0: WIDG flags 1 blend and 2 add in both lists; otherwise the near
            // list is opaque and the fading list blended. All are two-sided and modulate the texture.
            const std::uint32_t near_flags = widget.flags == 1   ? 0x08011009
                                             : widget.flags == 2 ? 0x08012009
                                                                 : 0x08010001;
            field.widget_fade_flags = widget.flags == 2 ? 0x08012009 : 0x08011009;
            // Draw range: 50 x RMS of the point bounding box extents, clamped to 30..200, at full detail
            // (WIDGET::Draw lerps from max(0.1 x range, 5) by the square root of the view's detail).
            Vec3 low = widget.points.front().position, high = low;
            for (const auto& point : widget.points) {
                low = {std::min(low.x, point.position.x), std::min(low.y, point.position.y),
                       std::min(low.z, point.position.z)};
                high = {std::max(high.x, point.position.x), std::max(high.y, point.position.y),
                        std::max(high.z, point.position.z)};
            }
            const float dx = low.x - high.x, dy = low.y - high.y, dz = low.z - high.z;
            field.widget_range =
                std::clamp(std::sqrt((dx * dx + dy * dy + dz * dz) * 0.33333334f) * 50.0f, 30.0f, 200.0f);
            mesh.materials.push_back({near_flags, widget.texture});
            field.asset.textures.push_back(texture);
            mesh.vertices.reserve(widget.points.size() * count);
            mesh.indices.reserve(widget.triangles.size() * 3 * count);
            std::vector<WidgetField::Placement> placements;
            placements.reserve(count);
            for (std::uint32_t i = 0; i < count; ++i) {
                const float x = r.f32(), y = r.f32(), angle = r.f32();
                if (!std::isfinite(x) || !std::isfinite(y) || !std::isfinite(angle))
                    throw Error(std::format("NTTS widget field at {} has a non-finite entry", record.offset));
                const float c = std::cos(angle), s = std::sin(angle), height = terrain_height(terrain, x, y);
                placements.push_back({{x, y, height}, angle});
                const auto base = static_cast<std::uint32_t>(mesh.vertices.size());
                for (const auto& point : widget.points) {
                    const auto& p = point.position;
                    Vertex v;
                    v.position = {p.x * c + p.y * s + x, p.z + height, y - p.x * s + p.y * c};
                    v.normal = {x, height, y};
                    v.uv = {point.uv.x, 1 - point.uv.y}; // WIDGETMODEL::Load stores 1 - v.
                    v.color = widget_color;
                    mesh.vertices.push_back(v);
                }
                for (const auto& t : widget.triangles) {
                    mesh.indices.insert(mesh.indices.end(), {base + t[0], base + t[1], base + t[2]});
                    mesh.triangle_materials.push_back(0);
                }
            }
            scene.widgets += count;
            ++scene.widget_fields;
            auto widget_field = std::make_shared<WidgetField>(std::make_shared<WidgetModel>(widget),
                                                              std::move(placements), widget_ambient);
            fields.push_back(widget_field);
            // Remastered Look: generated plants of the same role (grass, flower, bush...) replace it.
            const auto style = widget_style(id.text(), widget, texture.image);
            widget_field->set_flora(make_flora(style, id.value), make_flora(style, id.value, 4, true));
            field.widget_field = std::move(widget_field);
            field.placements.push_back({});
            scene.models.push_back(std::move(field));
            continue;
        }
        const auto entity = MapEntity::decode(record);
        if (entity.type == static_cast<std::uint32_t>(EntityKind::ScriptPoint)) {
            // 0x47dd50 raises the marker to the ground and keeps only the shape bits.
            auto position = entity.position;
            position.z = std::max(position.z, terrain_height(terrain, position.x, position.y));
            scene.markers.push_back({entity.script_id, entity.flags & 0xc000000, position, entity.angle,
                                     entity.extent});
        } else if (entity.type == static_cast<std::uint32_t>(EntityKind::Soul)) {
            if (auto* model = soul()) {
                // 0x475ee0 keeps an unattached soul at LAND height + 2 x growth (1 when grown).
                const auto& p = entity.position;
                model->placements.push_back({{p.x, terrain_height(terrain, p.x, p.y) + 2, p.y}});
                scene.spawns.push_back({MapScene::Spawn::Kind::soul,
                                        static_cast<std::size_t>(model - scene.models.data()),
                                        model->placements.size() - 1, model->placements.back()});
                ++scene.souls;
            }
        } else if (entity.type == static_cast<std::uint32_t>(EntityKind::Building)) {
            const auto location = catalog.find_chunk(FourCC::from_text("BLDG"), entity.id);
            if (!location) {
                missing("missing BLDG:" + entity.id.text());
                continue;
            }
            const auto building = Building::decode(catalog.read(*location));
            ++scene.buildings;
            // 0x457cb0: parts are offset in the building's rotated frame, then dropped to LAND::GetHeight.
            const float angle = entity.angle * degrees, c = std::cos(angle), s = std::sin(angle);
            const auto group = scene.building_groups.size();
            scene.building_groups.push_back({building, scene.spawns.size(), entity.position, entity.id});
            // Fountains ('font', 'cafn'...) and altars ('alt1'...) sit on a picture of tiles.
            if (const auto name = entity.id.text();
                name == "font" || name.ends_with("fn") || name.starts_with("alt"))
                scene.clearings.push_back({entity.position.x, entity.position.y, 20});
            for (unsigned part_index = 0; part_index < building.parts.size(); ++part_index) {
                const auto& part = building.parts[part_index];
                const float x = part.offset.x * c - part.offset.y * s + entity.position.x;
                const float y = part.offset.x * s + part.offset.y * c + entity.position.y;
                const Placement placement{{x, terrain_height(terrain, x, y), y},
                                          part.angle * degrees + angle};
                scene.spawns.push_back(
                    {MapScene::Spawn::Kind::building, SIZE_MAX, 0, placement, entity.flags, entity.side});
                auto& spawn = scene.spawns.back();
                spawn.building_group = group;
                spawn.building_part = part_index;
                spawn.script_id = entity.script_id;
                spawn.reservation_id = entity.reservation_id;
                const unsigned initial_state = entity.flags & 0x10000 ? 2 : 0;
                std::map<std::uint32_t, std::pair<std::size_t, std::size_t>> placed;
                for (unsigned state = 0; state < 3; ++state) {
                    const auto selected = part.model_at(state, building.flags);
                    if (!selected.value)
                        continue;
                    auto found = placed.find(selected.value);
                    if (found == placed.end()) {
                        auto* model = instance(selected);
                        if (!model)
                            continue;
                        const auto model_index = static_cast<std::size_t>(model - scene.models.data());
                        const auto placement_index = model->placements.size();
                        model->visible.resize(placement_index, true);
                        model->reveal.resize(placement_index, 1);
                        model->placements.push_back(placement);
                        model->visible.push_back(false);
                        model->reveal.push_back(1);
                        found = placed.emplace(selected.value, std::pair{model_index, placement_index}).first;
                    }
                    spawn.state_models[state] = found->second.first;
                    spawn.state_placements[state] = found->second.second;
                    if (state == initial_state)
                        scene.models[found->second.first].visible[found->second.second] = true;
                }
                spawn.model = spawn.state_models[initial_state];
                spawn.placement_index = spawn.state_placements[initial_state];
                ++scene.parts;
            }
#if 0
            // Just for testing: swap Trial trees to random obj/ pines in Remastered Look.
            if (!remastered_trees.empty() && is_trial_tree(entity.id)) {
                float orig_height = 0.0f;
                for (std::size_t si = scene.building_groups[group].first_spawn; si < scene.spawns.size(); ++si) {
                    for (std::size_t mi : scene.spawns[si].state_models) {
                        if (mi < scene.models.size()) {
                            scene.models[mi].look_mode = 1; // Classic Look only.
                            const auto& b = scene.models[mi].asset.geometry.bounds;
                            orig_height = std::max(orig_height, b.maximum.z - b.minimum.z);
                        }
                    }
                }
                // Deterministic per-tree hash (never touches the match random stream).
                std::uint32_t h = static_cast<std::uint32_t>(std::lround(entity.position.x * 13.0f)) * 374761393u ^
                                  static_cast<std::uint32_t>(std::lround(entity.position.y * 17.0f)) * 668265263u;
                h = (h ^ (h >> 13)) * 1274126177u;
                h ^= h >> 16;
                const std::size_t pick = h % remastered_trees.size();
                const float h_jitter = 0.90f + 0.25f * (static_cast<float>((h >> 8) & 0xffu) / 255.0f);
                const float yaw = 6.2831853f * (static_cast<float>((h >> 16) & 0xffu) / 255.0f);
                const float scale = (orig_height > 2.0f ? orig_height : 22.0f) * h_jitter;
                const float y_ground = terrain_height(terrain, entity.position.x, entity.position.y);
                remastered_trees[pick].placements.push_back({{entity.position.x, y_ground, entity.position.y}, yaw, scale});
            }
#endif
        } else if (entity.type == static_cast<std::uint32_t>(EntityKind::Creature) ||
                   entity.type == static_cast<std::uint32_t>(EntityKind::Wizard)) {
            const bool wizard = entity.type == static_cast<std::uint32_t>(EntityKind::Wizard);
            const auto tag = FourCC::from_text(wizard ? "WIZD" : "CRE8");
            const auto id = entity.id;
            const auto location = catalog.find_chunk(tag, id);
            if (!location) {
                missing("missing " + tag.text() + ":" + entity.id.text());
                continue;
            }
            // CRE8 and WIZD byte 148 names the SAXC body in all stock data (peas->peas, ment->FAmr,
            // klpd->dklp, cwd0->xwiz). Creatures (0x458300) convert the angle at byte 20 from degrees with
            // 0.01745277777777778. Wizards (0x457860) pass it to 0x410bc0 as radians and end at the record
            // position; the altar link (0x457570: the side's first unclaimed building part with flag bit 0,
            // whose +452 then points at the wizard) is gameplay state with nothing to draw.
            const auto definition = CreatureDefinition::decode(catalog.read(*location));
            if (auto* model = character(definition)) {
                const auto& p = entity.position;
                // SXCore output is already in world units: the factory 0x408f20 passes the descriptor scale
                // (0.001) to SAXS::SAXS 0x16027720, which the SXMD loader applies to bones and vertices.
                model->placements.push_back({{p.x, terrain_height(terrain, p.x, p.y), p.y},
                                             wizard ? entity.angle : entity.angle * creature_degrees});
                scene.spawns.push_back(
                    {MapScene::Spawn::Kind::character, static_cast<std::size_t>(model - scene.models.data()),
                     model->placements.size() - 1, model->placements.back(), entity.flags, entity.side});
                scene.spawns.back().wizard_level = entity.wizard_level;
                scene.spawns.back().script_id = entity.script_id;
                scene.spawns.back().wizard_god = entity.wizard_god;
                ++(wizard ? scene.wizards : scene.creatures);
            }
        }
    }
#if 0
    // Just for testing: append Remastered Look test pine trees.
    for (auto& tree : remastered_trees)
        if (!tree.placements.empty())
            scene.models.push_back(std::move(tree));
#endif
    for (const auto& field : fields)
        field->set_clearings(scene.clearings);
    return scene;
}
void SoulState::advance(int ticks, unsigned frames) {
    growth = std::min(growth + ticks * 0.005f, 1.0f);
    phase_x += ticks * 0.015f;
    phase_y += ticks * 0.01f;
    // Free souls are owned by every side (-1), so a local player sees the first half of the atlas.
    const auto half = static_cast<float>(std::max(frames / 2, 1u));
    frame += ticks * 0.25f;
    while (frame >= half)
        frame -= half;
}
Mesh soul_mesh(const SoulState& state, const TextureHeader& atlas) {
    // 0x475ee0: height 0.1 + 1.65 x growth, width 0.75 x height. FLAG::SetParams keeps half extents; a
    // screen-oriented flag reuses the X half extent as the wave depth. An unattached soul rises with its
    // growth to LAND + 2 x growth; placements sit at the grown LAND + 2, so the grid drops by 2 - 2 x growth.
    const float height = (0.1f + 1.65f * state.growth) * state.scale, width = height * 0.75f, hx = width / 2,
                hz = height / 2, lift = state.positioned ? 0 : 2 * state.growth - 2;
    // FLAG::Draw picks up to 8 cells per side from the projected size; the inspector uses the close-up 8
    // rows and 6 columns from the aspect.
    constexpr unsigned rows = 8, columns = 6;
    const double a = std::sin(state.phase_x + 0.785), b = std::sin(state.phase_y + 0.785);
    const double x0 = -hx - hx * b * 0.4, y0 = a * -hx * b, z0 = hz - a * hz * 0.4;
    const unsigned atlas_columns = std::max(atlas.width / std::max(atlas.frame_width, 1u), 1u);
    const auto frame = static_cast<unsigned>(state.frame) + state.atlas_offset;
    const float su = static_cast<float>(atlas.frame_width) / atlas.width,
                sv = static_cast<float>(atlas.frame_height) / atlas.height,
                u0 = static_cast<float>(frame % atlas_columns) * su,
                v0 = static_cast<float>(frame / atlas_columns) * sv;
    Mesh mesh;
    // Rows run from the top edge down, with V rising from the frame origin.
    for (unsigned r = 0; r <= rows; ++r) {
        const double s = std::sin(r * (1.57 / rows) + state.phase_y);
        for (unsigned c = 0; c <= columns; ++c) {
            const double t = std::sin(c * (1.57 / columns) + state.phase_x);
            const double x = hx * s * 0.4 + x0 + c * (width / columns), depth = t * hx * s + y0,
                         z = t * hz * 0.4 + z0 - r * (height / rows);
            Vertex v;
            v.position = {static_cast<float>(x), static_cast<float>(z) + lift, static_cast<float>(depth)};
            v.normal = {0, 0, -1};
            // FLAG::Draw writes the model colour (+164), which PRIMITIVE::InitModel sets to white and the soul
            // never changes: the blue/red comes from the atlas half. 0x475ee0's colour goes to its light.
            v.uv = {u0 + su * c / columns, v0 + sv * r / rows};
            mesh.vertices.push_back(v);
        }
    }
    constexpr unsigned stride = columns + 1;
    for (unsigned r = 0; r < rows; ++r)
        for (unsigned c = 0; c < columns; ++c) {
            const unsigned q = r * stride + c;
            mesh.indices.insert(mesh.indices.end(),
                                {q, q + 1, q + stride, q + 1, q + stride + 1, q + stride});
            mesh.triangle_materials.insert(mesh.triangle_materials.end(), {0, 0});
        }
    return mesh;
}
} // namespace sacrifice
namespace sacrifice {
Scar Scar::decode(ByteView data) {
    Reader r(data, "SCAR");
    r.take(8);
    Scar scar;
    scar.width = r.u32();
    scar.height = r.u32();
    if (!scar.width || !scar.height || scar.width > 1024 || scar.height > 1024 ||
        r.remaining() != 1024 + 2ull * scar.width * scar.height)
        throw Error("Invalid SCAR dimensions");
    for (auto& color : scar.palette) {
        const auto b = r.u8(), g = r.u8(), red = r.u8();
        r.u8();
        color =
            0xff000000u | (static_cast<std::uint32_t>(b) << 16) | (static_cast<std::uint32_t>(g) << 8) | red;
    }
    scar.texels.resize(static_cast<std::size_t>(scar.width) * scar.height);
    for (auto& texel : scar.texels) {
        texel = {r.u8(), r.u8()};
        if (texel[1] > 8)
            throw Error("SCAR alpha above 8");
    }
    return scar;
}
namespace {
// MakeScarLUT 0x16045490: for each RGB555 value (red high, blue low) the first palette entry nearest by
// squared distance to the 5-bit grid point scaled by 8.
std::vector<std::uint8_t> scar_inverse_palette(const std::array<std::uint32_t, 256>& palette) {
    std::vector<std::uint8_t> table(32768);
    for (unsigned i = 0; i < table.size(); ++i) {
        const int red = static_cast<int>(i >> 10) * 8, green = static_cast<int>((i >> 5) & 31) * 8,
                  blue = static_cast<int>(i & 31) * 8;
        int best = 0x100000;
        for (unsigned k = 0; k < 256; ++k) {
            const int dr = static_cast<int>(palette[k] & 255) - red,
                      dg = static_cast<int>((palette[k] >> 8) & 255) - green,
                      db = static_cast<int>((palette[k] >> 16) & 255) - blue;
            if (dr * dr + dg * dg + db * db < best) {
                best = dr * dr + dg * dg + db * db;
                table[i] = static_cast<std::uint8_t>(k);
            }
        }
    }
    return table;
}
// CreateNextMipMap 0x16046230: per channel sum of the 2x2 block, plus 15 when below 1009, to 5 bits through
// the inverse palette.
void rebuild_mipmaps(TerrainTile& tile, const std::array<std::uint32_t, 256>& palette,
                     const std::vector<std::uint8_t>& inverse) {
    for (unsigned level = 1; level < tile.mipmaps.size(); ++level) {
        const auto& from = tile.mipmaps[level - 1];
        auto& to = tile.mipmaps[level];
        for (unsigned y = 0; y < to.height; ++y)
            for (unsigned x = 0; x < to.width; ++x) {
                unsigned key = 0;
                for (unsigned channel = 0; channel < 3; ++channel) {
                    unsigned sum = 0;
                    for (unsigned dy = 0; dy < 2; ++dy)
                        for (unsigned dx = 0; dx < 2; ++dx)
                            sum += (from.rgba[(y * 2 + dy) * from.width + x * 2 + dx] >> (channel * 8)) & 255;
                    if (sum < 1009)
                        sum += 15;
                    key |= (sum >> 5) << (10 - channel * 5);
                }
                to.rgba[y * to.width + x] = palette[inverse[key]];
            }
    }
}
} // namespace
std::uint32_t TerrainMaterials::scar_layer(Terrain& terrain, std::size_t cell, std::int16_t priority) {
    const auto base = terrain.tiles.at(cell);
    if (tile_properties[base][0] & 2)
        return 0;
    if (scar_slots.empty()) {
        if (!scar_limit || scar_limit > 256)
            throw Error("Invalid LAND scar pool size");
        scar_slots.resize(scar_limit);
        for (unsigned i = scar_limit; i; --i)
            scar_free.push_back(i - 1);
    }
    auto& layer = terrain.scar_layers.at(cell);
    if (layer) {
        auto& slot = scar_slots.at(layer - 256);
        slot.priority = std::max(slot.priority, priority);
        slot.touched = scar_time;
        return layer;
    }
    if (scar_free.empty()) {
        // Native used-list traversal: lowest priority, then oldest signed LAND time. Ties keep
        // the first record, so allocations made in one tick are not an LRU queue.
        unsigned victim = UINT_MAX;
        std::int16_t lowest = 32767;
        auto oldest = std::bit_cast<std::int32_t>(scar_time);
        for (const auto i : scar_used) {
            const auto& slot = scar_slots[i];
            const auto time = std::bit_cast<std::int32_t>(slot.touched);
            if (slot.priority < lowest || (slot.priority == lowest && time < oldest)) {
                victim = i;
                lowest = slot.priority;
                oldest = time;
            }
        }
        if (victim == UINT_MAX)
            return 0;
        release_scar(terrain, scar_slots[victim].cell);
    }
    if (!tiles[base])
        throw Error("Scarred cell has no terrain tile");
    const auto index = scar_free.back();
    scar_free.pop_back();
    scar_used.insert(scar_used.begin(), index);
    scar_slots[index] = {cell, base, priority, scar_time};
    if (scars.size() <= index)
        scars.resize(index + 1);
    scars[index] = *tiles[base];
    scars[index].properties = tile_properties[base];
    return layer = 256 + index;
}
bool TerrainMaterials::release_scar(Terrain& terrain, std::size_t cell) {
    if (terrain.scar_layers.empty() || !terrain.scar_layers.at(cell))
        return false;
    const auto index = terrain.scar_layers[cell] - 256;
    const auto found = std::ranges::find(scar_used, index);
    if (found == scar_used.end())
        throw Error("LAND scar layer has no allocation");
    scar_used.erase(found);
    scar_free.push_back(index);
    terrain.tiles[cell] = scar_slots[index].base;
    terrain.scar_layers[cell] = 0;
    scar_slots[index].cell = SIZE_MAX;
    return true;
}
bool apply_scar(const Scar& scar, Vec3 position, Terrain& terrain, TerrainMaterials& materials,
                unsigned width, unsigned height, std::int16_t priority) {
    width = width ? width : scar.width;
    height = height ? height : scar.height;
    if (width > scar.width || height > scar.height || scar.texels.size() != static_cast<std::size_t>(scar.width) * scar.height)
        throw Error("Invalid SCAR region");
    std::set<std::size_t> changed;
    bool allocated = false;
    const auto x0 = static_cast<std::int64_t>(position.x * 6.4) - (width >> 1),
               y0 = static_cast<std::int64_t>(position.y * 6.4) - (height >> 1);
    if (terrain.scar_layers.empty())
        terrain.scar_layers.assign(terrain.tiles.size(), 0);
    // ApplyScarring visits each intersected tile once, rather than reallocating it for every row
    // when an oversized scar exhausts the pool. Clipping also handles stamps outside the map.
    const auto xend = x0 + width, yend = y0 + height;
    for (auto cy = std::max<std::int64_t>(y0 >> 6, 0); cy <= (yend - 1) >> 6 && cy < terrain.height; ++cy)
        for (auto cx = std::max<std::int64_t>(x0 >> 6, 0); cx <= (xend - 1) >> 6 && cx < terrain.width; ++cx) {
            const auto cell = static_cast<std::size_t>(cy) * terrain.width + static_cast<std::size_t>(cx);
            const auto previous = terrain.scar_layers[cell];
            const auto layer = materials.scar_layer(terrain, cell, priority);
            if (!layer)
                continue;
            allocated |= layer != previous;
            if (!previous)
                changed.erase(layer - 256); // A recycled slot starts with its new base tile's mipmaps.
            for (auto gy = std::max(y0, cy * 64); gy < std::min(yend, (cy + 1) * 64); ++gy)
                for (auto gx = std::max(x0, cx * 64); gx < std::min(xend, (cx + 1) * 64); ++gx) {
                    const auto [index, alpha] = scar.texels[static_cast<std::size_t>(gy - y0) * scar.width + gx - x0];
                    if (!alpha)
                        continue;
                    if (materials.scar_inverse.empty())
                        materials.scar_inverse = scar_inverse_palette(materials.palette);
                    auto& pixel = materials.scars[layer - 256].mipmaps[0].rgba[(gy & 63) * 64 + (gx & 63)];
                    unsigned key = 0;
                    for (unsigned channel = 0; channel < 3; ++channel) {
                        const auto tile = (pixel >> (channel * 8)) & 255,
                                   color = (scar.palette[index] >> (channel * 8)) & 255;
                        key |= (((tile * (8 - alpha)) >> 3) + ((color * alpha) >> 3)) >> 3 << (10 - channel * 5);
                    }
                    pixel = materials.palette[materials.scar_inverse[key]];
                    changed.insert(layer - 256);
                }
        }
    for (const auto tile : changed)
        rebuild_mipmaps(materials.scars[tile], materials.palette, materials.scar_inverse);
    return allocated || !changed.empty();
}
unsigned apply_scars(const ResourceCatalog& catalog, const Archive& map, Terrain& terrain,
                     TerrainMaterials& materials, std::vector<std::string>& unresolved) {
    const auto ntts = map.find(FourCC::from_text("NTTS"));
    if (!ntts)
        return 0;
    std::map<std::uint32_t, std::optional<Scar>> scars;
    unsigned applied = 0;
    for (const auto& record : decode_entities(map.read(*ntts))) {
        if (record.type != static_cast<std::uint32_t>(EntityKind::Scar))
            continue;
        // Record: position at 8, SCAR ID at 24, applied width/height in texels at 28/32 (0x4567d0 passes a
        // zero source offset, ApplyScarring flags 1/0, and no single-tile filter).
        const auto entity = MapEntity::decode(record);
        Reader r(record.payload, "NTTS scar");
        r.take(20);
        const auto width = r.u32(), height = r.u32();
        auto [found, inserted] = scars.try_emplace(entity.id.value);
        if (inserted) {
            if (const auto location = catalog.find_chunk(FourCC::from_text("SCAR"), entity.id))
                found->second = Scar::decode(catalog.read(*location));
            else
                unresolved.push_back("missing SCAR:" + entity.id.text());
        }
        if (!found->second)
            continue;
        const auto& scar = *found->second;
        if (width > scar.width || height > scar.height)
            throw Error(std::format("NTTS scar at {} exceeds SCAR:{}", record.offset, entity.id.text()));
        apply_scar(scar, entity.position, terrain, materials, width, height, 1);
        ++applied;
    }
    return applied;
}
} // namespace sacrifice
namespace sacrifice {
Environment Environment::decode(ByteView data) {
    Reader r(data, "ENVI");
    // 0x456b80 rejects the chunk unless its leading size word and the payload size are both 428.
    if (data.size() != 428 || r.u32() != 428)
        throw Error("ENVI is not a 428-byte environment record");
    Environment e;
    e.sun_intensity = r.f32();
    e.ambient_intensity = r.f32();
    const auto ambient = vec3(r);
    e.ambient = {ambient.x / 255, ambient.y / 255, ambient.z / 255};
    e.sky_color = r.u32();
    e.sky_layers = r.u32();
    e.alpha_low = static_cast<int>(r.u32());
    e.alpha_high = static_cast<int>(r.u32());
    e.alpha_low_value = r.f32();
    e.alpha_high_value = r.f32();
    for (unsigned i = 0; i < 5; ++i) {
        const auto slot = r.take(64);
        if (e.sky_layers & (1u << i))
            e.sky_textures[i] = FourCC{Reader(slot, "ENVI sky slot").u32()};
        else if (e.sky_layers & (32u << i)) {
            const auto end = std::find(slot.begin(), slot.end(), std::byte{0});
            e.sky_videos[i].assign(reinterpret_cast<const char*>(slot.data()),
                                   static_cast<std::size_t>(end - slot.begin()));
        }
    }
    r.take(4); // 368: not read by 0x456b80.
    e.sun_direction = vec3(r);
    const auto sun = r.take(4);
    e.sun_color = {std::to_integer<unsigned>(sun[0]) / 255.0f, std::to_integer<unsigned>(sun[1]) / 255.0f,
                   std::to_integer<unsigned>(sun[2]) / 255.0f};
    r.take(4); // 388: unused by the runtime terrain light bake.
    e.terrain_gloss = r.f32();
    e.terrain_gloss_width = r.f32();
    e.terrain_gloss_color = r.u32();
    e.edge = FourCC{r.u32()};
    e.fog_color = r.u32();
    e.fog_mode = r.u32();
    e.fog_start = std::max(r.f32(), 100.0f);
    e.fog_end = std::max(r.f32(), e.fog_start);
    e.fog_density = r.f32();
    if (!finite(e.ambient) || !finite(e.sun_direction) || !std::isfinite(e.fog_end))
        throw Error("ENVI has non-finite values");
    return e;
}
namespace {
// Original Z-up world to renderer space.
Vec3 render_space(float x, float y, float z) {
    return {x, z, y};
}
std::uint32_t gray(unsigned value, unsigned alpha) {
    return value | value << 8 | value << 16 | alpha << 24;
}
// 0x160437b0 fills this 32x32x32 table (Z fastest) with MSVCRT rand() the first time LAND::Init runs. The
// stock game passes seed 0, so no srand precedes it and the original continues whatever rand() state the
// process has reached; this starts from the CRT's initial state.
const std::array<std::uint16_t, 32768>& sky_noise_table() {
    static const auto table = [] {
        std::array<std::uint16_t, 32768> values{};
        CrtRandom random;
        for (auto& value : values)
            value = static_cast<std::uint16_t>(random.next());
        return values;
    }();
    return table;
}
// 0x16043800: trilinear lookup at p / 320, wrapping every 32 cells, scaled to [0, 1).
double sky_noise(float x, float y, float z) {
    const auto& table = sky_noise_table();
    const double px = x * 0.003125, py = y * 0.003125;
    const float pz = static_cast<float>(z * 0.003125);
    auto ix = static_cast<std::int64_t>(px), iy = static_cast<std::int64_t>(py),
         iz = static_cast<std::int64_t>(pz);
    // The original decrements the truncated cell for any negative input, exact integers included.
    if (x < 0)
        --ix;
    if (y < 0)
        --iy;
    if (z < 0)
        --iz;
    const auto at = [&](std::int64_t cx, std::int64_t cy, std::int64_t cz) {
        return static_cast<double>(table[((cx & 31) * 32 + (cy & 31)) * 32 + (cz & 31)]);
    };
    const double fy = py - static_cast<double>(iy);
    const float fx = static_cast<float>(px - static_cast<double>(ix));
    const double fz = pz - static_cast<double>(iz);
    const float near_z1 =
        static_cast<float>((at(ix, iy + 1, iz + 1) - at(ix, iy, iz + 1)) * fy + at(ix, iy, iz + 1));
    const float far_z1 = static_cast<float>((at(ix + 1, iy + 1, iz + 1) - at(ix + 1, iy, iz + 1)) * fy +
                                            at(ix + 1, iy, iz + 1));
    const float near_z0 = static_cast<float>((at(ix, iy + 1, iz) - at(ix, iy, iz)) * fy + at(ix, iy, iz));
    const double z0 =
        (fy * (at(ix + 1, iy + 1, iz) - at(ix + 1, iy, iz)) + at(ix + 1, iy, iz) - near_z0) * fx + near_z0;
    return (z0 + ((far_z1 - near_z1) * fx + near_z1 - z0) * fz) * 0.000030517578125;
}
// One cell of the LAND+68 grids: the alpha-table index from Z 512 and the gray from Z 0.
void sky_cell(std::array<std::uint8_t, 162>& cells, int cell, float x, float y) {
    cells[cell] = static_cast<std::uint8_t>(static_cast<std::int64_t>(sky_noise(x, y, 512) * 255.0));
    cells[81 + cell] =
        static_cast<std::uint8_t>(127 - static_cast<std::int64_t>(sky_noise(x, y, 0) * -128.0f));
}
// Stock wind passed by Sacrifice.exe 0x4525d0 to LAND::Run (data at 0x4cebc8).
constexpr float wind_x = 0.075f, wind_y = -0.06f;
} // namespace
SkyState::SkyState() {
    generate_rows(0, 9); // LAND::Init, with every position zero.
}
// LAND::GenerateSkyRows: row `first + k` samples X from the scroll position plus k cells and Y from the
// row's own offset plus one cell per column.
void SkyState::generate_rows(int first, int count) {
    float x = noise_x * 0.125f;
    const auto y0 = static_cast<float>((noise_y + first * 2560.0) * 0.125);
    for (int k = 0; k < count; ++k, x += 320) {
        float y = y0;
        for (int column = 0; column < 9; ++column, y += 320)
            sky_cell(cells, (first + k) * 9 + column, x, y);
    }
}
// LAND::GenerateSkyColumns: column `first + k` steps X down its rows and Y by k cells.
void SkyState::generate_columns(int first, int count) {
    const auto x0 = static_cast<float>((noise_x + first * 2560.0) * 0.125);
    float y = noise_y * 0.125f;
    for (int k = 0; k < count; ++k, y += 320) {
        float x = x0;
        for (int row = 0; row < 9; ++row, x += 320)
            sky_cell(cells, row * 9 + first + k, x, y);
    }
}
// LAND::AnimateSky: turn the cylinders, scroll the clouds by 10 x wind per tick, and when the grid offset
// leaves (-2560, 2560) move it one cell back, shift both byte grids, and generate the uncovered edge.
void SkyState::advance(int ticks) {
    azimuth = static_cast<float>(azimuth + ticks * 0.00025);
    const double step = 10.0 * ticks;
    const auto dx = static_cast<float>(step * wind_x), dy = static_cast<float>(step * wind_y);
    noise_x += dx;
    noise_y += dy;
    offset_x += dx;
    offset_y += dy;
    const auto shift_columns = [&](bool right) {
        for (int half = 0; half < 162; half += 81)
            for (int row = 0; row < 9; ++row) {
                auto* line = cells.data() + half + row * 9;
                if (right)
                    std::memmove(line + 1, line, 8);
                else
                    std::memmove(line, line + 1, 8);
            }
    };
    const auto shift_rows = [&](bool down) {
        for (int half = 0; half < 162; half += 81) {
            auto* grid = cells.data() + half;
            if (down)
                std::memmove(grid + 9, grid, 72);
            else
                std::memmove(grid, grid + 9, 72);
        }
    };
    if (offset_x >= 2560) {
        offset_x -= 2560;
        shift_columns(true);
        generate_columns(0, 1);
    } else if (offset_x <= -2560) {
        offset_x += 2560;
        shift_columns(false);
        generate_columns(8, 1);
    }
    if (offset_y >= 2560) {
        offset_y -= 2560;
        shift_rows(true);
        generate_rows(0, 1);
    } else if (offset_y <= -2560) {
        offset_y += 2560;
        shift_rows(false);
        generate_rows(8, 1);
    }
}
SkyTextures load_sky_textures(const ResourceCatalog& catalog, const Environment& environment, FourCC tileset,
                              const std::array<Image, 5>& video_frames) {
    SkyTextures slots;
    const auto land = catalog.find_chunk(FourCC::from_text("LAND"), tileset);
    for (unsigned i = 0; i < 5; ++i) {
        if (environment.sky_layers & (1u << i)) {
            const auto name = environment.sky_textures[i];
            auto location = catalog.find_chunk(FourCC::from_text("TXTR"), name, land);
            if (!location)
                location = catalog.find_chunk(FourCC::from_text("TXTR"), name);
            if (!location)
                throw Error("ENVI sky texture missing: TXTR:" + name.text());
            const auto header = decode_texture_header(catalog.read(*location));
            slots[i] = ModelAsset::Texture{header, texture_frame(load_texture(catalog, *location), header, 0),
                                           *location};
        } else if ((environment.sky_layers & (32u << i)) && !video_frames[i].rgba.empty())
            slots[i] = ModelAsset::Texture{{}, video_frames[i], {}};
    }
    return slots;
}
std::optional<ModelAsset::Texture> load_edge_texture(const ResourceCatalog& catalog,
                                                     const Environment& environment, FourCC tileset) {
    if (!environment.edge.value)
        return std::nullopt;
    auto location = catalog.find_chunk(FourCC::from_text("TXTR"), environment.edge,
                                       catalog.find_chunk(FourCC::from_text("LAND"), tileset));
    if (!location)
        location = catalog.find_chunk(FourCC::from_text("TXTR"), environment.edge);
    if (!location)
        throw Error("ENVI edge texture missing: TXTR:" + environment.edge.text());
    const auto header = decode_texture_header(catalog.read(*location));
    return ModelAsset::Texture{header, texture_frame(load_texture(catalog, *location), header, 0), *location};
}
Mesh sky_mesh(const Environment& environment, const SkyTextures& slots, const SkyState& state) {
    Mesh mesh;
    const auto material = [&](unsigned slot, std::uint32_t flags) {
        const Mesh::Material m{flags, FourCC{0x736b7930u + slot}}; // "sky0".."sky4"
        const auto found = std::find(mesh.materials.begin(), mesh.materials.end(), m);
        if (found != mesh.materials.end())
            return static_cast<std::uint32_t>(found - mesh.materials.begin());
        mesh.materials.push_back(m);
        return static_cast<std::uint32_t>(mesh.materials.size() - 1);
    };
    const auto triangle = [&](std::uint32_t m, std::uint32_t a, std::uint32_t b, std::uint32_t c) {
        mesh.indices.insert(mesh.indices.end(), {a, b, c});
        mesh.triangle_materials.push_back(m);
    };
    const auto vertex = [&](Vec3 p, Vec2 uv, std::uint32_t color) {
        Vertex v;
        v.position = p;
        v.normal = {0, -1, 0};
        v.uv = uv;
        v.color = color;
        mesh.vertices.push_back(v);
        return static_cast<std::uint32_t>(mesh.vertices.size() - 1);
    };
    // LAND::MakeSkyVB/DrawSkyVB: 9x9 cloud plane at 3276.8, 2560 apart, translated by the scroll offset
    // - 10240 + 1280, one texture repeat per cell; vertex gray and SetSkyAlphaTable alpha from the grids.
    if (slots[0]) {
        std::array<std::uint8_t, 256> alpha{};
        const float low = std::clamp(environment.alpha_low_value, 0.0f, 1.0f),
                    high = std::clamp(environment.alpha_high_value, 0.0f, 1.0f);
        const int first = environment.alpha_low, last = environment.alpha_high;
        for (int i = 0; i < 256; ++i) {
            float value = high * 255;
            if (i < first)
                value = low; // The original stores the unscaled 0..1 value here.
            else if (i <= last)
                value = (last == first ? low
                                       : static_cast<float>(i - first) / static_cast<float>(last - first) *
                                                 (high - low) +
                                             low) *
                        255;
            alpha[i] = static_cast<std::uint8_t>(value);
        }
        const auto m = material(0, 0x06011009);
        const auto base = static_cast<std::uint32_t>(mesh.vertices.size());
        for (unsigned row = 0; row < 9; ++row)
            for (unsigned column = 0; column < 9; ++column) {
                const auto cell = row * 9 + column;
                vertex(render_space(column * 2560.0f + state.offset_x - 8960,
                                    row * 2560.0f + state.offset_y - 8960, 3276.8f),
                       {static_cast<float>(column), static_cast<float>(row)},
                       gray(state.cells[81 + cell], alpha[state.cells[cell]]));
            }
        for (std::uint32_t row = 0; row < 8; ++row)
            for (std::uint32_t column = 0; column < 8; ++column) {
                const auto v = base + row * 9 + column;
                triangle(m, v, v + 1, v + 9);
                triangle(m, v + 1, v + 10, v + 9);
            }
    }
    // LAND::DrawAzSkyPolygon: a square of half-diagonal `radius` about (1280, 1280, z) turned by `azimuth`,
    // in the view's sky color (white unless ATMOS weather tints it).
    const auto polygon = [&](unsigned slot, std::uint32_t flags, float z, float radius, float azimuth) {
        if (!slots[slot])
            return;
        const auto m = material(slot, flags | 0x0e010001);
        const float s = std::sin(azimuth) * radius, c = std::cos(azimuth) * radius;
        const auto a = vertex(render_space(1280 + s, 1280 + c, z), {0, 0}, 0xffffffff);
        const auto b = vertex(render_space(1280 + c, 1280 - s, z), {1, 0}, 0xffffffff);
        const auto d = vertex(render_space(1280 - c, 1280 + s, z), {0, 1}, 0xffffffff);
        const auto e = vertex(render_space(1280 - s, 1280 - c, z), {1, 1}, 0xffffffff);
        triangle(m, a, b, d);
        triangle(m, d, e, b);
    };
    polygon(4, 0, 0, 3000, -state.azimuth); // UNDR below the world.
    polygon(3, 0x2008, 3276.5f, 1000, 0);   // SUN_, additive.
    // LAND::MakeSkyCylinderVB: 7 rings of 32 points on the cubic Bezier (radius, height) profile at
    // 0x16056f64, U ping-ponging every four columns, V by ring, both inset by the texel size.
    // DrawSkyCylinderVB places SKYT at (1280, 1280, 1500) turned by the azimuth about Z; SKYB uses Euler
    // angles (azimuth, pi, 0), which mirror Y and Z before the same turn.
    constexpr std::array<std::array<float, 2>, 4> profile{
        {{1500, 1500}, {3165, 1493}, {4847, 1070}, {6000, 0}}};
    const float turn_sin = std::sin(state.azimuth), turn_cos = std::cos(state.azimuth);
    const auto cylinder = [&](unsigned slot, bool mirrored, unsigned blended_bands) {
        if (!slots[slot])
            return;
        const auto& texture = *slots[slot];
        const float inset = texture.image.width ? 1.0f / static_cast<float>(texture.image.width) : 1 / 256.0f,
                    span = 1 - 2 * inset;
        const auto blended = material(slot, texture.header.flags & 0x10000 ? 0x06011009u : 0x06012009u);
        const auto opaque = material(slot, 0x06010001);
        const auto base = static_cast<std::uint32_t>(mesh.vertices.size());
        for (unsigned ring = 0; ring < 7; ++ring) {
            const float t = static_cast<float>(ring) / 6, u = 1 - t;
            const float a = u * u * u, b = 3 * u * u * t, c = 3 * u * t * t, d = t * t * t;
            const float radius =
                a * profile[0][0] + b * profile[1][0] + c * profile[2][0] + d * profile[3][0];
            const float height =
                a * profile[0][1] + b * profile[1][1] + c * profile[2][1] + d * profile[3][1];
            for (unsigned column = 0; column < 32; ++column) {
                const float angle = static_cast<float>(column) * 0.19634938f;
                float tu = static_cast<float>(column & 3) * span * 0.25f + inset;
                if (!(column & 4))
                    tu = 1 - tu;
                const float x = std::sin(angle) * radius,
                            y = (mirrored ? -1.0f : 1.0f) * std::cos(angle) * radius;
                vertex(render_space(1280 + x * turn_cos - y * turn_sin, 1280 + x * turn_sin + y * turn_cos,
                                    1500 + (mirrored ? -height : height)),
                       {tu, t * span + inset}, 0xffffffff);
            }
        }
        for (std::uint32_t band = 0; band < 6; ++band) {
            const auto m = band < blended_bands ? blended : opaque;
            for (std::uint32_t column = 0; column < 32; ++column) {
                const auto v = base + band * 32 + column, next = base + band * 32 + (column + 1) % 32;
                triangle(m, v + 32, v, next + 32);
                triangle(m, next + 32, v, next);
            }
        }
    };
    cylinder(1, false, 3);
    cylinder(2, true, 0);
    return mesh;
}
ModelAsset build_sky(const Environment& environment, const SkyTextures& slots, const SkyState& state) {
    ModelAsset sky;
    sky.mesh = sky_mesh(environment, slots, state);
    for (const auto& material : sky.mesh.materials)
        sky.textures.push_back(slots[material.texture.value - 0x736b7930u]);
    return sky;
}
} // namespace sacrifice
