#include "sacrifice/assets.hpp"
#include "sacrifice/campaign.hpp"
#include "sacrifice/creature.hpp"
#include "sacrifice/saxs.hpp"
#include "sacrifice/scene.hpp"
#include "sacrifice/sound.hpp"
#include <fstream>
#include <iostream>
#include <map>
#include <nlohmann/json.hpp>
#include <set>

using namespace sacrifice;
using nlohmann::json;
namespace {
json metadata(const Archive& a) {
    json entries = json::array();
    for (const auto& r : a.records())
        entries.push_back({{"path", r.path},
                           {"name", r.name.text()},
                           {"tag", r.tag.text()},
                           {"offset", r.offset},
                           {"stored_size", r.stored_size},
                           {"size", r.size},
                           {"children", r.children},
                           {"flags", r.flags}});
    return {{"file", a.label()},
            {"size", a.file_size()},
            {"entry_count", a.records().size()},
            {"leaf_count", a.resource_count()},
            {"entries", entries}};
}
int validate(const Arguments& args, const std::filesystem::path& root) {
    ResourceCatalog catalog(root);
    catalog.mount_game();
    std::map<std::uint32_t, TerrainMaterials> materials;
    std::map<std::string, json> reference;
    if (!args.reference.empty()) {
        std::ifstream in(args.reference);
        json ref;
        in >> ref;
        for (const auto& p : ref)
            reference.emplace(p.at("file").get<std::string>(), p);
    }
    std::size_t containers = 0, resources = 0, scenarios = 0, entities = 0, models = 0, samples = 0,
                textures = 0, model_materials = 0, model_textures = 0, animated_model_textures = 0,
                textures_with_trailing_data = 0, texture_frame_overflows = 0;
    std::size_t geometry_frames = 0, animated_models = 0, lod_levels = 0, full_detail_faces = 0,
                stored_faces = 0, normal_links = 0, geometry_tails = 0;
    std::size_t creature_definitions = 0, wizard_definitions = 0, creature_fsms = 0, creature_states = 0,
                creature_events = 0;
    std::size_t sound_sets = 0, sound_groups = 0, sound_references = 0;
    std::size_t side_tables = 0, side_records = 0;
    std::size_t saxs_models = 0, saxs_parts = 0, saxs_bones = 0, saxs_source_vertices = 0, saxs_patches = 0,
                saxs_explicit_parts = 0, saxs_textures = 0, saxs_clips = 0, saxs_posed_clips = 0,
                saxs_animations = 0, saxs_animation_frames = 0, saxs_frame_key_mismatches = 0,
                saxs_sequences = 0, saxs_triggers = 0, saxs_zero_rotations = 0, saxs_near_vertices = 0,
                saxs_near_triangles = 0, saxs_far_triangles = 0;
    std::vector<std::string> failures;
    json results = json::array();
    std::set<std::string> visited;
    for (const auto& path : container_paths(root)) {
        auto relative = utf8(std::filesystem::relative(path, root));
        try {
            auto a = Archive::open(path);
            a.validate_payloads();
            ++containers;
            resources += a.resource_count();
            if (!reference.empty()) {
                auto it = reference.find(relative);
                if (it == reference.end())
                    throw Error("Not present in reference inventory");
                const auto& ref = it->second;
                auto actual = metadata(a);
                visited.insert(relative);
                if (actual["size"] != ref["size"] || actual["leaf_count"] != ref["leaf_count"] ||
                    actual["entries"] != ref["entries"])
                    throw Error("Resource directory differs from Python reference");
            }
            if (path.extension() == ".scp") {
                auto terrain = Terrain::load(a);
                auto material = materials.find(terrain.tileset.value);
                if (material == materials.end())
                    material =
                        materials
                            .emplace(terrain.tileset.value, TerrainMaterials::load(catalog, terrain.tileset))
                            .first;
                material->second.create_tile_map(terrain);
                material->second.validate(terrain);
                ++scenarios;
                auto n = a.find(FourCC::from_text("NTTS"));
                if (n)
                    entities += decode_entities(a.read(*n)).size();
            }
            for (std::size_t i = 0; i < a.records().size(); ++i) {
                const auto& e = a.records()[i];
                if (e.folder())
                    continue;
                auto tag = e.tag.text();
                try {
                    if (tag == "SIDS") {
                        const auto definitions = SideDefinition::decode(a.read(i));
                        ++side_tables;
                        side_records += definitions.size();
                    } else if (tag == "CRE8" || tag == "WIZD") {
                        const auto definition = CreatureDefinition::decode(a.read(i));
                        if (definition.id != e.name || definition.kind != (tag == "CRE8" ? 2u : 8u))
                            throw Error("Creature definition id/kind differs from resource directory");
                        if (!catalog.find_chunk(FourCC::from_text("SAXC"), definition.body))
                            throw Error("Missing creature SAXC body: " + definition.body.text());
                        if (!catalog.find_chunk(FourCC::from_text("FSMC"), definition.animation_fsm))
                            throw Error("Missing creature FSMC: " + definition.animation_fsm.text());
                        ++(definition.kind == 2 ? creature_definitions : wizard_definitions);
                    } else if (tag == "FSMC") {
                        const auto machine = CreatureFsm::decode(a.read(i));
                        if (machine.id != e.name)
                            throw Error("FSMC id differs from resource directory");
                        ++creature_fsms;
                        for (const auto& group : machine.groups)
                            for (const auto& state : group.states) {
                                ++creature_states;
                                creature_events += state.events.size();
                            }
                    } else if (tag == "MRMM") {
                        auto model = ModelAsset::load(catalog, {catalog.mount(path), i});
                        model_materials += model.mesh.materials.size();
                        const auto& geometry = model.geometry;
                        geometry_frames += geometry.frames.size();
                        animated_models += geometry.frames.size() > 1;
                        lod_levels += geometry.levels.size();
                        stored_faces += geometry.faces.size();
                        full_detail_faces += model.mesh.indices.size() / 3;
                        normal_links += geometry.normal_links;
                        geometry_tails += geometry.trailing_bytes != 0;
                        if (geometry.sample(0, 0, 0, geometry.minimum_vertices).indices.size() / 3 !=
                            geometry.minimum_faces)
                            throw Error("Minimum-detail face count mismatch");
                        for (unsigned frame = 1; frame < geometry.frames.size(); ++frame) {
                            auto pose = geometry.sample(frame - 1, frame, 0.5f);
                            if (pose.indices.size() / 3 != geometry.maximum_faces)
                                throw Error("Morph topology changed");
                        }
                        for (const auto& texture : model.textures)
                            if (texture) {
                                ++model_textures;
                                if (texture->header.frames > 1)
                                    ++animated_model_textures;
                            }
                        ++models;
                    } else if (tag == "SXMD") {
                        auto asset = SaxsAsset::load(catalog, {catalog.mount(path), i});
                        const auto& model = asset.model;
                        ++saxs_models;
                        saxs_parts += model.parts.size();
                        saxs_bones += model.bones.size();
                        saxs_source_vertices += model.vertices.size();
                        saxs_textures += asset.textures.size();
                        saxs_clips += asset.clips.size();
                        for (const auto& part : model.parts) {
                            saxs_patches += part.patch.has_value();
                            saxs_explicit_parts += !part.faces.empty();
                        }
                        const auto near = saxs_topology(model, asset.descriptor, 1);
                        saxs_near_vertices += near.vertices.size();
                        saxs_near_triangles += near.triangles.size();
                        saxs_far_triangles += saxs_topology(model, asset.descriptor, 8).triangles.size();
                        (void)saxs_topology(model, asset.descriptor, 3);
                        for (const auto& clip : asset.clips) {
                            const auto animation = SaxsAnimation::decode(catalog.read(clip.animation));
                            for (float frame : {0.0f, static_cast<float>(animation.frames.size() - 1)}) {
                                auto mesh = saxs_mesh(model, asset.descriptor, near,
                                                      saxs_pose(model, animation, frame));
                                if (mesh.indices.size() / 3 > near.triangles.size())
                                    throw Error("SAXS pose changed topology");
                            }
                            ++saxs_posed_clips;
                        }
                    } else if (tag == "SXSK") {
                        const auto animation = SaxsAnimation::decode(a.read(i));
                        ++saxs_animations;
                        saxs_animation_frames += animation.frames.size();
                        saxs_frame_key_mismatches += animation.declared_frames != animation.frames.size();
                        saxs_zero_rotations += animation.zero_rotations;
                    } else if (tag == "SKEL") {
                        saxs_triggers += SaxsSequence::decode(a.read(i)).triggers.size();
                        ++saxs_sequences;
                    } else if (tag == "SAMP") {
                        (void)decode_sample(a.read(i));
                        ++samples;
                    } else if (tag == "SSET") {
                        const auto set = SoundSet::decode(a.read(i));
                        ++sound_sets;
                        sound_groups += set.groups.size();
                        for (const auto& group : set.groups)
                            sound_references += group.samples.size();
                    } else if (tag == "TXTR" || tag == "ICON") {
                        (void)load_texture(a, i);
                        const auto header = decode_texture_header(a.read(i));
                        if (header.trailing_bytes)
                            ++textures_with_trailing_data;
                        if (header.frames > static_cast<std::uint64_t>(header.width / header.frame_width) *
                                                (header.height / header.frame_height))
                            ++texture_frame_overflows;
                        ++textures;
                    } else if (tag == "PALT") {
                        (void)decode_palette(a.read(i));
                    }
                } catch (const std::exception& ex) {
                    failures.push_back(relative + e.path + ": " + ex.what());
                }
            }
            results.push_back(
                {{"file", relative}, {"resources", a.resource_count()}, {"status", "validated"}});
        } catch (const std::exception& e) {
            failures.push_back(relative + ": " + e.what());
        }
    }
    for (const auto& [path, value] : reference)
        if (!visited.contains(path))
            failures.push_back("Missing reference container: " + path);
    auto campaign = Campaign::load(root / "maps/Campaign/sacrifice.cmp");
    std::size_t missions = 0;
    for (const auto& [number, level] : campaign.levels)
        for (const auto& [choice, file] : level.missions) {
            ++missions;
            if (!std::filesystem::is_regular_file(root / "maps/Campaign/Sacrifice" / file))
                failures.push_back("Missing campaign mission: " + file);
        }
    std::size_t tile_count = 0;
    for (const auto& [id, set] : materials)
        for (const auto& tile : set.tiles)
            if (tile)
                ++tile_count;
    json report = {{"tilesets", materials.size()},
                   {"mounted_packs", catalog.archives().size()},
                   {"terrain_tiles", tile_count},
                   {"containers", containers},
                   {"resources", resources},
                   {"scenarios", scenarios},
                   {"entity_records", entities},
                   {"side_tables", side_tables},
                   {"side_records", side_records},
                   {"creature_definitions", creature_definitions},
                   {"wizard_definitions", wizard_definitions},
                   {"creature_fsms", creature_fsms},
                   {"creature_states", creature_states},
                   {"creature_events", creature_events},
                   {"static_models", models},
                   {"geometry_frames", geometry_frames},
                   {"animated_mrm_models", animated_models},
                   {"lod_levels", lod_levels},
                   {"stored_mrm_faces", stored_faces},
                   {"full_detail_mrm_faces", full_detail_faces},
                   {"corrected_normal_links", normal_links},
                   {"models_with_unused_lod_tail", geometry_tails},
                   {"model_materials", model_materials},
                   {"model_textures", model_textures},
                   {"animated_model_textures", animated_model_textures},
                   {"saxs_models", saxs_models},
                   {"saxs_body_parts", saxs_parts},
                   {"saxs_bones", saxs_bones},
                   {"saxs_source_vertices", saxs_source_vertices},
                   {"saxs_patched_parts", saxs_patches},
                   {"saxs_explicit_face_parts", saxs_explicit_parts},
                   {"saxs_part_textures", saxs_textures},
                   {"saxs_compatible_clips", saxs_clips},
                   {"saxs_posed_clips", saxs_posed_clips},
                   {"saxs_near_render_vertices", saxs_near_vertices},
                   {"saxs_near_triangles", saxs_near_triangles},
                   {"saxs_factor8_triangles", saxs_far_triangles},
                   {"saxs_animations", saxs_animations},
                   {"saxs_animation_frames", saxs_animation_frames},
                   {"saxs_frame_key_mismatches", saxs_frame_key_mismatches},
                   {"saxs_zero_rotation_keys", saxs_zero_rotations},
                   {"saxs_sequences", saxs_sequences},
                   {"saxs_sequence_triggers", saxs_triggers},
                   {"samples", samples},
                   {"sound_sets", sound_sets},
                   {"sound_groups", sound_groups},
                   {"sound_sample_references", sound_references},
                   {"textures", textures},
                   {"textures_with_trailing_data", textures_with_trailing_data},
                   {"texture_frame_overflows", texture_frame_overflows},
                   {"campaign_levels", campaign.levels.size()},
                   {"mission_choices", missions},
                   {"failures", failures},
                   {"files", results}};
    auto summary = report;
    summary.erase("files");
    summary["failure_count"] = failures.size();
    if (failures.size() > 20)
        summary["failures"] = std::vector<std::string>(failures.begin(), failures.begin() + 20);
    std::cout << summary.dump(2) << '\n';
    if (!args.output.empty()) {
        std::ofstream out(args.output);
        if (!out)
            throw Error("Cannot open output file");
        out << report.dump(2) << '\n';
    }
    return failures.empty() ? 0 : 1;
}
} // namespace
int main(int argc, char** argv) {
    try {
        std::vector<std::string> values;
        for (int i = 1; i < argc; ++i)
            values.emplace_back(argv[i]);
        auto args = Arguments::parse(values);
        if (args.command.empty() || args.command == "help") {
            std::cout << "SacrificeModern native asset tools (x64 / C++20)\n"
                         "  validate --game-dir DIR [--reference JSON] [--output JSON]\n"
                         "  inspect --file WAD_OR_SCP\n"
                         "  extract --file WAD_OR_SCP --resource /TAG:NAME/... --output FILE\n"
                         "  campaign --game-dir DIR\n"
                         "  catalog --game-dir DIR [--output JSON]\n"
                         "  scenes --game-dir DIR\n";
            return 0;
        }
        if (args.command == "inspect" || args.command == "extract") {
            if (args.file.empty())
                throw Error("--file is required");
            auto a = Archive::open(args.file);
            if (args.command == "inspect") {
                std::cout << metadata(a).dump(2) << '\n';
                return 0;
            }
            auto r = a.find_path(args.resource);
            if (!r)
                throw Error("Resource path not found");
            if (args.output.empty())
                throw Error("--output is required");
            const auto canonical_input = std::filesystem::weakly_canonical(args.file),
                       canonical_output = std::filesystem::weakly_canonical(args.output);
            if (canonical_input == canonical_output)
                throw Error("Output must not overwrite the input archive");
            auto b = a.read(*r);
            std::ofstream out(args.output, std::ios::binary);
            if (!out || !out.write(reinterpret_cast<const char*>(b.data()), b.size()))
                throw Error("Cannot write extracted resource");
            return 0;
        }
        auto root = discover_game_dir(args.game_dir);
        if (args.command == "validate")
            return validate(args, root);
        if (args.command == "catalog") {
            ResourceCatalog catalog(root);
            catalog.mount_game();
            json packs = json::array();
            for (const auto& archive : catalog.archives())
                packs.push_back({{"file", utf8(std::filesystem::relative(widen(archive.label()), root))},
                                 {"resources", archive.resource_count()}});
            json report = {{"lookup", "first mounted, depth first; flag 4 prunes unmatched subtrees"},
                           {"packs", packs}};
            std::cout << report.dump(2) << '\n';
            if (!args.output.empty()) {
                std::ofstream out(args.output);
                if (!out || !(out << report.dump(2) << '\n'))
                    throw Error("Cannot write catalog report");
            }
            return 0;
        }
        if (args.command == "scenes") {
            // Composes every map's buildings/creatures and summarizes NTTS kinds and unresolved IDs.
            ResourceCatalog catalog(root);
            catalog.mount_game();
            std::map<std::string, unsigned> kinds, unresolved, fog_modes;
            unsigned maps = 0, buildings = 0, parts = 0, creatures = 0, fields = 0, widgets = 0, scars = 0,
                     joined = 0, layers = 0, wizards = 0, souls = 0, environments = 0, sky_triangles = 0;
            std::map<std::uint32_t, TerrainMaterials> materials;
            for (const auto& path : container_paths(root)) {
                if (path.extension() != ".scp")
                    continue;
                catalog.mount(path);
                const auto archive = Archive::open(path);
                auto terrain = Terrain::load(archive);
                auto tileset = materials.find(terrain.tileset.value);
                if (tileset == materials.end())
                    tileset =
                        materials
                            .emplace(terrain.tileset.value, TerrainMaterials::load(catalog, terrain.tileset))
                            .first;
                joined += tileset->second.create_tile_map(terrain);
                auto scene = MapScene::compose(catalog, archive, terrain);
                auto scarred = tileset->second;
                scars += apply_scars(catalog, archive, terrain, scarred, scene.unresolved);
                layers += static_cast<unsigned>(scarred.scars.size());
                ++maps;
                buildings += scene.buildings;
                parts += scene.parts;
                creatures += scene.creatures;
                wizards += scene.wizards;
                souls += scene.souls;
                if (scene.environment) {
                    ++environments;
                    ++fog_modes[std::to_string(scene.environment->fog_mode)];
                    sky_triangles += static_cast<unsigned>(
                        build_sky(*scene.environment,
                                  load_sky_textures(catalog, *scene.environment, terrain.tileset))
                            .mesh.indices.size() /
                        3);
                }
                fields += scene.widget_fields;
                widgets += scene.widgets;
                for (const auto& [kind, count] : scene.kinds)
                    kinds[std::to_string(kind)] += count;
                for (const auto& text : scene.unresolved)
                    ++unresolved[text];
            }
            std::cout << json({{"maps", maps},
                               {"buildings", buildings},
                               {"building_parts", parts},
                               {"creatures", creatures},
                               {"wizards", wizards},
                               {"souls", souls},
                               {"environments", environments},
                               {"sky_triangles", sky_triangles},
                               {"fog_modes", fog_modes},
                               {"widget_fields", fields},
                               {"widgets", widgets},
                               {"joined_tiles", joined},
                               {"scars", scars},
                               {"scarred_tiles", layers},
                               {"kinds", kinds},
                               {"unresolved", unresolved}})
                             .dump(2)
                      << '\n';
            return 0;
        }
        if (args.command == "campaign") {
            auto c = Campaign::load(root / "maps/Campaign/sacrifice.cmp");
            json levels = json::array();
            for (const auto& [number, l] : c.levels)
                levels.push_back({{"level", number}, {"missions", l.missions}});
            std::cout
                << json({{"name", c.name}, {"wizard_types", c.wizard_types}, {"levels", levels}}).dump(2)
                << '\n';
            return 0;
        }
        throw Error("Unknown command: " + args.command);
    } catch (const std::exception& e) {
        std::cerr << "error: " << e.what() << '\n';
        return 1;
    }
}
