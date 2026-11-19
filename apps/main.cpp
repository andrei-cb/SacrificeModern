#include "hud_painter.hpp"
#include "view_camera.hpp"
#include "fps_overlay.hpp"
#include "sacrifice/save_pages.hpp"
#include "sacrifice/loading_screen.hpp"
#include "script_overlay.hpp"
#include "sacrifice/compat.hpp"
#include "sacrifice/ambient_audio.hpp"
#include "sacrifice/building_actor.hpp"
#include "sacrifice/campaign.hpp"
#include "sacrifice/title_backdrop.hpp"
#include "sacrifice/campaign_session.hpp"
#include "sacrifice/character.hpp"
#include "sacrifice/command_menu.hpp"
#include "sacrifice/effect_audio.hpp"
#include "sacrifice/fog.hpp"
#include "sacrifice/gameplay_input.hpp"
#include "sacrifice/hud.hpp"
#include "sacrifice/match.hpp"
#include "sacrifice/media.hpp"
#include "sacrifice/menu_form.hpp"
#include "sacrifice/motion.hpp"
#include "sacrifice/music.hpp"
#include "sacrifice/native_font.hpp"
#include "sacrifice/options.hpp"
#include "sacrifice/player.hpp"
#include "sacrifice/player_pointer.hpp"
#include "sacrifice/player_visuals.hpp"
#include "sacrifice/presentation_audio.hpp"
#include "sacrifice/renderer.hpp"
#include "sacrifice/saxs.hpp"
#include "sacrifice/scene.hpp"
#include "sacrifice/scene_effects.hpp"
#include "sacrifice/scene_match.hpp"
#include "sacrifice/scene_updates.hpp"
#include "sacrifice/side.hpp"
#include "sacrifice/side_ai.hpp"
#include "sacrifice/skirmish_lobby.hpp"
#include "sacrifice/soul_actor.hpp"
#include "sacrifice/sound.hpp"
#include "sacrifice/spell_actor.hpp"
#include "sacrifice/spell_combat.hpp"
#include "sacrifice/spell_registry.hpp"
#include "sacrifice/storage.hpp"
#include "sacrifice/wizard_control.hpp"
#include "sacrifice/wizard_preview.hpp"
#include "sacrifice/world_save.hpp"
#include <algorithm>
#include <atomic>
#include <chrono>
#include <csignal>
#include <cmath>
#include <format>
#include <functional>
#include <fstream>
#include <imgui.h>
#include <map>
#include <typeinfo>
#include <memory>
#include <nlohmann/json.hpp>
#include <objbase.h>
#include <dbghelp.h>
#include <shellapi.h>
#include <unordered_map>
#include <utility>

namespace sacrifice {
namespace {
// Port frame cap ('rate', Max Framerate): waits until `next`, then schedules the following frame. A late
// frame restarts the schedule rather than rushing to catch up.
void limit_frame_rate(std::chrono::steady_clock::time_point& next, int rate) {
    using clock = std::chrono::steady_clock;
    const auto period = std::chrono::duration_cast<clock::duration>(std::chrono::duration<double>(1.0 / std::max(rate, 1)));
    static const HANDLE timer =
        CreateWaitableTimerExW(nullptr, nullptr, CREATE_WAITABLE_TIMER_HIGH_RESOLUTION, TIMER_ALL_ACCESS);
    if (const auto wait = next - clock::now() - std::chrono::milliseconds(1); timer && wait.count() > 0) {
        LARGE_INTEGER due{};
        due.QuadPart = -std::chrono::duration_cast<std::chrono::duration<long long, std::ratio<1, 10'000'000>>>(wait).count();
        if (SetWaitableTimer(timer, &due, 0, nullptr, nullptr, false))
            WaitForSingleObject(timer, INFINITE);
    }
    while (clock::now() < next)
        YieldProcessor();
    const auto now = clock::now();
    next = (next + period < now ? now : next) + period;
}
enum class View { Terrain, Model, Image };
// The viewer camera's basis in renderer space (y up; game x, y, z are renderer x, z, y) and the
// RENDERWINDOW half-angle tangents of Renderer::draw, for cursor rays and overlay projection.
struct ViewBasis : GameplayView {
    ViewBasis(const Camera& camera, unsigned width, unsigned height)
        : GameplayView(camera.position, camera.yaw, camera.pitch, width, height) {}
};
struct Inspector {
    std::filesystem::path root;
    std::vector<std::filesystem::path> maps, packs, videos, music;
    std::unique_ptr<Archive> selected;
    std::unique_ptr<VideoClip> video;
    struct TalkingMovie {
        std::filesystem::path path;
        std::unique_ptr<VideoClip> clip;
        std::shared_ptr<void> image;
        unsigned width{}, height{};
        double time{};
    };
    std::array<TalkingMovie, 2> talking_movies;
    std::uint64_t talking_movie_line{};
    std::unique_ptr<AudioOutput> audio;
    std::unique_ptr<ModelAsset> model;
    std::unique_ptr<SaxsAsset> saxs;
    SaxsTopology saxs_layout;
    std::optional<SaxsAnimation> saxs_animation;
    std::optional<SaxsSequence> saxs_sequence;
    int saxs_clip{-1};
    float saxs_detail{1}, saxs_frame{};
    Campaign campaign;
    ResourceCatalog catalog;
    WizardPreview menu_wizard{catalog};
    std::shared_ptr<void> menu_wizard_image;
    std::size_t selected_archive{};
    std::map<std::uint32_t, TerrainMaterials> material_cache;
    std::optional<TerrainMaterials> world_materials;
    std::map<FourCC, std::optional<Scar>> scar_cache;
    std::size_t applied_tile_changes{};
    std::uint64_t applied_land_revision{};
    Terrain terrain;
    Camera camera, home;
    Vec3 focus;
    std::string status, error, resource_text, resource_description;
    std::size_t entity_count{};
    MapScene scene;
    std::string scene_error;
    std::optional<SkyTextures> sky_textures;
    std::array<std::unique_ptr<VideoClip>, 5> sky_videos;
    std::array<double, 5> sky_video_time{};
    SkyState sky_state;
    GameClock sky_clock;
    CrtRandom match_random;
    std::filesystem::path world_path;
    std::vector<SkirmishSlot> world_slots;
    bool load_world_requested{};
    std::string world_notice;
    std::string load_name; // The save the queued load reads.
    std::optional<Image> menu_thumbnail; // 0x4a76f0: the scene as the game menu opened.
    bool thumbnail_wanted{};
    // Saved-game pages FORM sgam / lgam and their dialogs, shared with the browser build.
    SavePages saves{SavePages::Host{
        root, [this] { return can_save_world(); }, [this](const std::string& name) { save_world(name); },
        [this](const std::string& name) {
            load_name = name;
            load_world_requested = true;
        },
        [this](FourCC page, bool in_game) { show_save_page(page, in_game); },
        [this](FourCC record) { return text_record(record); }, [this](const std::string& line) { log(line); }}};
    std::shared_ptr<void> save_thumbnail_image;
    unsigned save_thumbnail_revision{};
    std::unique_ptr<SceneMatch> simulation;
    std::unique_ptr<PlayerControl> player; // Selection and orders for the local side (0x4da920).
    CommandMenu command_menu;
    PlayerPointer pointer_feedback;
    FourCC pointer_icon{FourCC::from_text("Cnor")}, pointer_warning;
    unsigned pointer_alpha{255};
    Vec2 pointer_position;
    std::optional<unsigned> smoke_hover;
    std::uint32_t smoke_hover_object{};
    std::optional<Vec2> smoke_pointer;
    FourCC smoke_target;
    std::shared_ptr<void> minimap_image;
    std::uint64_t minimap_revision{UINT64_MAX};
    std::uint32_t minimap_side{};
    MiniMapControl minimap_control;
    RenderDetailControl render_detail;
    std::optional<PointerButton> minimap_drag;
    bool minimap_box{};
    std::uint32_t menu_target{};
    Vec3 menu_point;
    unsigned menu_frame{};
    std::array<bool, 256> previous_codes{};
    // Interface audio: SSET:ifac for 0x461bf0 clicks, SSET:advi for advisor slot 0, and one voice channel
    // serving the three acknowledgement slots in order (0x461080) and taunts.
    std::shared_ptr<const SoundSet> interface_set, advisor_set;
    float voice_busy{};
    ScriptAudioState script_audio;
    unsigned script_zoom_revision{};
    std::uint64_t script_choice{};
    std::uintptr_t choice_voice{}, rain_voice{};
    bool choice_paused{};
    std::size_t history_row{SIZE_MAX};
    std::uintptr_t history_voice{};
    unsigned history_frames{};
    int history_scroll{};
    bool history_pending{};
    FourCC tip_text;
    std::vector<FourCC> tip_texts;
    std::size_t tip_index{};
    bool game_menu{};                 // 'menu' 'gmnu' (0x451d10): the in-game menu, which pauses.
    FourCC menu_page;
    int menu_open_frame{-1};
    MenuKeys menu_keys;
    std::map<FourCC, MenuForm> menu_forms;
    bool gameplay_closed{}; // End-page OK/Exit leaves the match for the front end.
    // Native front end (handler table 0x407eb0): FORM:titl -> 1ply -> loby (slts/slot/MAPi) -> cgam.
    // Nonzero while a front-end page replaces the gameplay view.
    FourCC front_page;
    SkirmishLobby lobby;
    CrtRandom lobby_random;
    std::vector<std::filesystem::path> skirmish_maps; // maps/*.scp outside the campaign folders.
    std::size_t lobby_map{}, browsed_map{};
    int maps_scroll{}; // First visible row of cgam's map list.
    std::map<std::size_t, std::shared_ptr<void>> map_previews;
    std::optional<std::vector<SkirmishSlot>> pending_slots; // 'strt' hands the lobby rows to load().
    // FORM:igam, the native loading page (0x4a9ed0 / 0x4a95d0). A queued load shows it for one frame, then
    // runs from the top of the main loop, outside any UI frame, so load() can present progress frames.
    struct Loading {
        std::function<void(Renderer&)> work;
        std::shared_ptr<void> image; // img1..img4: the map's 384x384 DATA:LIMG or data\defaultmap.tga.
        std::string description;     // intr: the map's LANG/ENGL TEXT:DESC (0x4a9290, record +920).
        float progress{};            // prog: completed / total load steps (0x4562e0).
        bool shown{};
    } loading;
    std::function<void(float)> load_progress; // Presents a loading frame; set only while a queued load runs.
    bool defer_loads{}; // The interactive loop shows FORM:igam first; smoke runs load at once.
    bool quit_requested{};
    // FORM:rost / newc / delc / espb (0x4a5020, 0x4a4a90, 0x4a4fc0, 0x4a4e40): characters of the chosen game
    // type (dword_4DBF9C). The selected one (dword_4F3CF4) names the local player and supplies its wizard.
    std::optional<CharacterRoster> roster;
    std::string game_type;              // The game type's name, which is also its character folder.
    std::vector<Character> characters;  // The rost list.
    int roster_row{-1};                 // dword_4F3CF4.
    std::optional<Character> profile;   // The character the session plays.
    std::optional<std::string> edited;  // dword_4F3D20: echr's character; empty for nchr.
    std::string new_name;               // byte_4DC19C, the pnam edit text.
    std::vector<WizardChoice> wizard_list; // dword_4F3BD8: the game type's wizards (+24).
    std::size_t wizard_index{};            // dword_4F3C38.
    std::array<std::uint8_t, 20> new_gods{}; // dword_4F3BC4: a god per level, edited by espb.
    std::shared_ptr<const SpellRegistry> front_registry; // espb's spell names.
    bool custom_path{}; // 0x4a0060 when newc opens: a character beat the campaign.
    // The campaign game type (1ply scmp): its host, whether the loaded map is one of its missions, the
    // player's wizard there, and the mission a victory queues for the next frame.
    std::optional<CampaignHost> campaign_host;
    CampaignSession campaign_state;
    bool cinematic_fast_forward{};
    std::filesystem::path campaign_mission;
    MissionEndTransition end_transition;
    std::optional<std::filesystem::path> queued_mission, next_script_mission;
    bool script_transition{};
    bool campaign_finished{};
    // SIDE::EndGame (0x4717d0) for the local side with a wizard opens the game type's lose (+12) or win
    // (+16) menu through 0x451d10(menu, 1, 1), which pauses; 1 won, -1 lost, 0 while undecided.
    int end_screen{};
    bool end_presented{};
    std::filesystem::path pending_shot; // 'shot' (0x40aa50), written after the frame.
    // Third-person play (0x451ea0 / 0x4525d0 keyboard hand-over and the mode-0 follow camera); F9 returns
    // to the inspector's free camera.
    KeyboardDevice keyboard;
    FollowCamera follow = playing_follow();
    CameraBlend camera_blend; // Drawn view between ticks.
    SceneExitBlend exit_blend; // Mode 1 back to the follow view after a scene.
    CameraShake shake;
    CrtRandom shake_random; // Stands in for the shared rand() of 0x47fbb0.
    bool third_person{true}, keyboard_engaged{}, dragging{};
    std::array<bool, 3> orbit_allowed{};
    std::optional<PointerButton> orbit_button;
    bool left_world{};
    std::array<bool, 3> click_allowed{};
    Vec2 box_origin, box_cursor;
    CreatureActor* driven{};
    std::unordered_map<std::uint32_t, std::string> text_cache;
    std::unordered_map<std::uint32_t, std::shared_ptr<void>> spell_icons;
    std::map<FourCC, TextureHeader> icon_headers;
    std::optional<GradientBar> hover_bar;
    struct HudTexture {
        TextureHeader header;
        std::shared_ptr<void> image;
    };
    std::unordered_map<std::uint32_t, HudTexture> hud_textures;
    TitleBackdrop title_backdrop;
    std::shared_ptr<void> status_frame;
    std::array<GradientBar, 2> status_styles;
    bool status_assets_loaded{};
    std::vector<std::size_t> listed_resources; // The inspector's filtered resource rows (reused).
    bool remastered{};        // The Remastered Look is on (option 'rmst', unless --classic-look).
    bool classic_look{};      // --classic-look: the Remastered Look stays off whatever 'rmst' says.
    bool frame_timing{};      // Port diagnostics: the renderer's frame timing under the top-left corner.
    WeatherLook weather_look; // Its drawing-only weather state.
    bool show_inspector{true}; // F10 is a viewer-only inspector toggle; gameplay HUD remains visible.
    int displayed_book{-1};
    CreatureActor* book_wizard{};
    int frame_ticks{};
    std::unique_ptr<AmbientAudio> ambient_audio;
    std::unique_ptr<EffectAudio> effect_audio; // Positional object, spell, building and animation sounds.
    std::unique_ptr<MusicDirector> music_director; // dword_4dbf44.
    bool music_on{true};
    int match_ticks{}; // 0x4525d0 enables the music once 10 ticks have run.
    bool scene_sound{true};
    bool live_audio{true}; // Smoke captures never open a playback device; match RNG is independent.
    bool silent_script_audio{}; // Still exercise speech scheduling through the device-free audio host.
    float scene_volume{1};
    GameOptions options;   // FORM:opts values (0x4c8db8), persisted under "options".
    FourCC options_page;   // dword_4c9298: the opts subform on show.
    FourCC options_return; // gmnu when the pause menu opened the options.
    bool panel_backdrop{}; // The title page under a dialog is an unfocused, disabled panel.
    int summary_mode{};    // FORM:summ page: 0 combat, 1 souls, 2 structures (0x471400).
    FourCC summary_return; // The win/lose page that opened the summary.
    std::string audio_error;
    int controlled_character{};
    int order_target{}, order_building{};
    bool target_building{}, append_orders{};
    std::array<float, 3> order_point{1280, 1280, 10};
    FourCC order_spell;
    int profile_god{1}, profile_level{1};
    int debug_weather{1}, debug_souls{10};  // Inspector weather kind (1..6, LandscapeEffects order) and soul grant.
    float debug_weather_strength{1}, debug_weather_fade{5};
    int soul_grant{10};
    int selected_soul{};
    View view{View::Terrain};
    bool wireframe{}, preview{}, model_materials{true}, show_entities{true},
        show_environment{true};
    bool play_model{};
    float model_frame{}, preview_rate{4};
    int model_vertices{};
    double video_time{};
    Image displayed_image;
    Image map_preview;
    char filter[128]{};
    explicit Inspector(std::filesystem::path game, bool allow_audio)
        : root(std::move(game)), campaign(Campaign::load(root / "maps/Campaign/sacrifice.cmp")),
          catalog(root) {
        catalog.mount_game();
        live_audio = allow_audio;
        silent_script_audio = !allow_audio;
        ambient_audio = std::make_unique<AmbientAudio>(catalog);
        effect_audio = std::make_unique<EffectAudio>(catalog, allow_audio);
        music_director = std::make_unique<MusicDirector>(root / "data/music", allow_audio);
        for (const auto& p : container_paths(root))
            (p.extension() == ".scp" ? maps : packs).push_back(p);
        for (const auto& p : std::filesystem::directory_iterator(root / "data/anims"))
            if (p.is_regular_file())
                videos.push_back(p.path());
        for (const auto& p : std::filesystem::directory_iterator(root / "data/music"))
            if (p.is_regular_file())
                music.push_back(p.path());
        std::sort(videos.begin(), videos.end());
        std::sort(music.begin(), music.end());
    }
    template <class F> void attempt(F&& f) {
        try {
            f();
            error.clear();
        } catch (const std::exception& e) {
            error = e.what();
            log(error);
        }
    }
    template <class F> void audio_attempt(F&& f) {
        try {
            f();
            audio_error.clear();
        } catch (const std::exception& e) {
            audio_error = e.what();
            scene_sound = false;
            live_audio = false;
            ambient_audio->reset_device();
            effect_audio->reset_device();
            music_director->release_device();
            log(audio_error);
        }
    }
    // 0x4a9ed0 step 0 -> 0x40feb0: the map's picture and description for FORM:igam.
    void queue_load(const std::filesystem::path& path, Renderer& renderer, std::function<void(Renderer&)> work) {
        loading = {};
        loading.work = std::move(work);
        attempt([&] {
            auto picture = loading_picture(root, path);
            loading.description = std::move(picture.description);
            loading.image = renderer.upload_ui_image(picture.image);
        });
        if (!defer_loads)
            run_loading(renderer);
    }
    void run_loading(Renderer& renderer) {
        const auto work = std::move(loading.work);
        loading.work = {};
        if (work)
            work(renderer);
        loading_step(1);
        loading = {};
    }
    void loading_step(float fraction) {
        loading.progress = std::max(loading.progress, fraction);
        if (load_progress)
            load_progress(loading.progress);
    }
    void loading_screen(unsigned width, unsigned height, Renderer& renderer) {
        auto& draw = *ImGui::GetForegroundDrawList();
        draw.AddRectFilled({0, 0}, {static_cast<float>(width), static_cast<float>(height)}, IM_COL32_BLACK);
        const auto& form = native_form(FourCC::from_text("igam"));
        const auto scale = form_scale(width, height);
        scaled_page(draw, scale, [&] {
            auto paint = menu_painter(draw, renderer);
            paint_loading_screen(paint, draw, 1, form,
                                 form.bounds(static_cast<unsigned>(width / scale), static_cast<unsigned>(height / scale)),
                                 static_cast<ImTextureID>(reinterpret_cast<std::uintptr_t>(loading.image.get())),
                                 loading.description, loading.progress);
        });
    }
    void load(const std::filesystem::path& path, Renderer& renderer, bool legacy_campaign_setup = false) {
        const auto archive_id = catalog.mount(path);
        auto archive = std::make_unique<Archive>(Archive::open(path));
        if (path.extension() == ".scp") {
            auto next = Terrain::load(*archive);
            auto material = material_cache.find(next.tileset.value);
            if (material == material_cache.end())
                material =
                    material_cache.emplace(next.tileset.value, TerrainMaterials::load(catalog, next.tileset))
                        .first;
            loading_step(.1f);
            material->second.create_tile_map(next);
            material->second.validate(next);
            world_materials = material->second; // Scars modify per-map copies of tiles.
            auto& materials = *world_materials;
            auto e = archive->find(FourCC::from_text("NTTS"));
            entity_count = e ? decode_entities(archive->read(*e)).size() : 0;
            player.reset();
            simulation.reset();
            command_menu.close();
            game_menu = false;
            menu_page = {};
            gameplay_closed = false;
            end_screen = 0;
            end_presented = false;
            ambient_audio->clear_cache();
            effect_audio->clear_cache();
            history_row = SIZE_MAX;
            history_voice = 0;
            history_pending = false;
            history_frames = 0;
            history_scroll = 0;
            script_audio.line = script_audio.speech = {};
            script_zoom_revision = 0;
            script_choice = choice_voice = 0;
            choice_paused = false;
            script_audio.voices.clear();
            script_audio.sounds.clear();
            rain_voice = 0;
            tip_text = {};
            tip_texts.clear();
            music_director->reset();
            music_director->enable(false);
            match_ticks = 0;
            scene = {};
            scene_error.clear();
            loading_step(.25f);
            try {
                scene = MapScene::compose(catalog, *archive, next);
                scene.scars = apply_scars(catalog, *archive, next, materials, scene.unresolved);
            } catch (const std::exception& e) {
                // Keep the terrain usable when an entity record cannot be composed yet.
                scene_error = e.what();
                log("Scene composition failed: " + scene_error);
            }
            // NTTS souls spawn through 0x458480: creation (0x467d50, activation's two rand() draws), then
            // 0x475c90 draws two wave phases. Building/character activations also share this stream;
            // widget fields and building spell/behaviour side effects remain pending. Start-up RNG is
            // unknown.
            match_random = {};
            terrain = std::move(next);
            minimap_control.reset(terrain);
            render_detail = {};
            minimap_drag.reset();
            minimap_box = false;
            // Maps without NTTS wizards are skirmish maps: the lobby's rows when started from FORM:loby,
            // otherwise slot 1 is the local player and every other listed slot a computer with a random god.
            std::vector<SkirmishSlot> slots;
            if (pending_slots && !scene.wizards)
                slots = *pending_slots;
            else if (!scene.wizards)
                for (unsigned slot = 1; slot <= 5; ++slot)
                    if (std::ranges::any_of(scene.sides, [&](const SideDefinition& side) {
                            const auto nibble = side.flags & 0xf ? side.flags & 0xf : (side.flags & 0x20 ? 1u : 0u);
                            return nibble == slot;
                        }))
                        slots.resize(slot, {SkirmishSlot::Kind::computer, FourCC::from_text("cwd0"), 0});
            // 0x4aa1e0 -> 0x457750 creates the carried wizard at an unclaimed altar after NTTS,
            // even when the player's side already contains scripted wizards (the credits do).
            if (campaign_state.active && profile)
                slots = {{SkirmishSlot::Kind::local, FourCC::from_text("cwe2"), 1}};
            if (!slots.empty() && !pending_slots) {
                slots.front().kind = SkirmishSlot::Kind::local;
                if (campaign_state.active)
                    slots.front().wizard = FourCC::from_text("cwe2");
            }
            pending_slots.reset();
            world_slots = slots;
            if (trace_ai()) {
                trace_clock = trace_next = 0;
                log(std::format("Trace match {}: audio {}, campaign {}", utf8(path.filename()), live_audio,
                                campaign_state.active));
                for (std::size_t i = 0; i < slots.size(); ++i)
                    log(std::format("Trace slot {}: kind {} wizard {} god {} team {}", i + 1,
                                    static_cast<int>(slots[i].kind),
                                    slots[i].wizard.value ? slots[i].wizard.text() : "-", slots[i].god, slots[i].team));
            }
            world_path = path;
            player.reset();
            loading_step(.55f);
            simulation = std::make_unique<SceneMatch>(
                scene, terrain, match_random, &materials, &catalog, slots, campaign_state.active && profile,
                campaign_state.active && profile && !legacy_campaign_setup ? &*profile : nullptr);
            simulation->match().stop_looping_sounds = [this](std::uint32_t emitter) {
                effect_audio->stop_loops(emitter);
            };
            simulation->match().flush_spell_sounds = [this](SpellActor& spell) {
                service_spell_audio(simulation->match(), spell);
            };
            next_script_mission.reset();
            script_transition = false;
            simulation->triggers().next_mission = [this, directory = path.parent_path()](std::string file) {
                auto named = std::filesystem::path(widen(file));
                if (!named.has_extension()) named += L".scp";
                auto candidate = directory / named;
                if (!std::filesystem::exists(candidate))
                    for (const auto& map : maps)
                        if (_wcsicmp(map.filename().c_str(), named.filename().c_str()) == 0) { candidate = map; break; }
                if (std::filesystem::exists(candidate)) next_script_mission = candidate;
                else log("Script next mission is missing: " + file);
            };
            simulation->triggers().mission_over = [this] {
                if (next_script_mission) script_transition = true;
                else { gameplay_closed = true; enter_front_end(FourCC::from_text("titl")); }
            };
            // 0x4a1b10 seeds the session record with option 'fdam'; 0x456000 copies it to dword_4dabc0.
            simulation->match().friendly_damage_scale =
                static_cast<std::uint8_t>(options.value(FourCC::from_text("fdam")));
            // 'ffir' sets session flag 8 (byte 0x4dabc5): projectiles pass their own side only for the first
            // two seconds (0x474fb0 / 0x474f40) instead of always.
            simulation->match().projectile_friendly_fire = options.value(FourCC::from_text("ffir")) != 0;
            // Building footprints (0x44cfa0) rewrite LAND tiles during map construction.
            for (const auto& change : simulation->tile_changes()) {
                materials.release_scar(terrain, change.cell);
                terrain.tiles.at(change.cell) = change.tile;
                if (!terrain.scar_layers.empty())
                    terrain.scar_layers[change.cell] = 0;
            }
            applied_tile_changes = simulation->tile_changes().size();
            applied_land_revision = simulation->match().terrain_revision;
            loading_step(.75f);
            bake_terrain_lighting(terrain, materials, scene, *archive, catalog);
            renderer.set_mesh(terrain.mesh(!preview));
            renderer.set_terrain_materials(materials);
            if (!preview)
                renderer.set_terrain_lod(terrain);
            bind_campaign_wizard();
            if (simulation->match().local_side)
                bind_player_control();
            simulation->match().world_journal =
                std::make_shared<WorldJournal>(simulation->match(), player.get());
            // Skirmish slots and campaign missions already chose the local side; other maps follow the
            // diagnostic character.
            if (auto* actor = character(static_cast<unsigned>(controlled_character));
                actor && !simulation->match().local_side)
                simulation->match().local_side = actor->allegiance;
            for (auto& scene_model : scene.models) {
                if (scene_model.character) {
                    auto& character = *scene_model.character;
                    auto& object = *character.object;
                    const auto& body = *character.body;
                    scene_model.asset.mesh =
                        saxs_mesh(body.model, body.descriptor, *character.topology, object.model_pose());
                    character.uploaded_pose = object.pose_revision();
                }
                if (scene_model.soul) {
                    auto mesh = soul_mesh(*scene_model.soul, scene_model.asset.textures.front()->header);
                    mesh.materials = std::move(scene_model.asset.mesh.materials);
                    scene_model.asset.mesh = std::move(mesh);
                }
            }
            loading_step(.9f);
            renderer.set_scene(scene);
            renderer.set_terrain_edge(nullptr);
            if (scene.environment)
                try {
                    const auto edge = load_edge_texture(catalog, *scene.environment, terrain.tileset);
                    renderer.set_terrain_edge(edge ? &*edge : nullptr);
                } catch (const std::exception& e) {
                    scene_error = e.what();
                    log("Edge texture failed: " + scene_error);
                }
            renderer.clear_environment();
            sky_textures.reset();
            sky_videos = {};
            sky_video_time = {};
            talking_movies = {};
            talking_movie_line = 0;
            sky_state = {};
            sky_clock = {};
            if (scene.environment) {
                try {
                    // Smacker sky slots (0x16040000 opens them as SMACKTEX); the inspector shows frame 0.
                    std::array<Image, 5> frames;
                    for (unsigned i = 0; i < 5; ++i)
                        if (!scene.environment->sky_videos[i].empty()) {
                            sky_videos[i] =
                                std::make_unique<VideoClip>(root / scene.environment->sky_videos[i]);
                            sky_videos[i]->next(frames[i]);
                        }
                    sky_textures = load_sky_textures(catalog, *scene.environment, terrain.tileset, frames);
                    renderer.set_environment(*scene.environment,
                                             build_sky(*scene.environment, *sky_textures, sky_state));
                } catch (const std::exception& e) {
                    scene_error = e.what();
                    log("Environment failed: " + scene_error);
                }
            }
            renderer.clear_image();
            displayed_image = {};
            map_preview = {};
            minimap_image.reset();
            minimap_revision = UINT64_MAX;
            if (auto p = archive->find(FourCC::from_text("PREV")); p) {
                map_preview = decode_preview(archive->read(*p));
                if (preview) {
                    displayed_image = map_preview;
                    renderer.set_image(displayed_image);
                }
            }
            camera = {};
            camera.position = {terrain.width * 5.0f, terrain.maximum + 1300.0f, -800};
            camera.pitch = -0.5f;
            home = camera;
            view = View::Terrain;
            status = std::format(
                "{} ÃƒÂ¢Ã¢â€šÂ¬Ã¢â‚¬Â {}x{} terrain, {} entity records; {} buildings ({} parts), {} creatures, {} wizards, "
                "{} souls, {} widgets, {} scars; {} unresolved IDs",
                utf8(path.filename()), terrain.width, terrain.height, entity_count, scene.buildings,
                scene.parts, scene.creatures, scene.wizards, scene.souls, scene.widgets, scene.scars,
                scene.unresolved.size());
        } else
            status = std::format("{} ÃƒÂ¢Ã¢â€šÂ¬Ã¢â‚¬Â {} resources", utf8(path.filename()), archive->resource_count());
        video.reset();
        resource_text.clear();
        resource_description.clear();
        selected = std::move(archive);
        selected_archive = archive_id;
        play_model = false;
        if (view != View::Model) {
            model.reset();
            saxs.reset();
        }
        log("Loaded " + utf8(path));
    }
    void show(std::size_t index, Renderer& renderer) {
        play_model = false;
        video.reset();
        const auto& record = selected->records()[index];
        if (record.folder())
            return;
        auto bytes = selected->read(index);
        auto tag = record.tag.text();
        resource_text.clear();
        resource_description =
            std::format("{}\n{} bytes; stored {} bytes", record.path, bytes.size(), record.stored_size);
        if (tag == "SAXS" || tag == "SXMD") {
            show_saxs({selected_archive, index}, renderer);
        } else if (tag == "SXSK" && saxs) {
            auto clip = std::find_if(saxs->clips.begin(), saxs->clips.end(), [&](const SaxsAsset::Clip& c) {
                return c.animation.archive == selected_archive && c.animation.record == index;
            });
            if (clip == saxs->clips.end())
                throw Error("SXSK skeleton does not match the selected SAXS model");
            select_clip(static_cast<int>(clip - saxs->clips.begin()), renderer);
        } else if (tag == "MRMM") {
            saxs.reset();
            auto asset = std::make_unique<ModelAsset>(ModelAsset::load(catalog, {selected_archive, index}));
            const auto& mesh = asset->mesh;
            Vec3 low = mesh.vertices[0].position, high = low;
            for (const auto& frame : asset->geometry.frames)
                for (const auto& v : frame) {
                    low.x = std::min(low.x, v.position.x);
                    low.y = std::min(low.y, v.position.y);
                    low.z = std::min(low.z, v.position.z);
                    high.x = std::max(high.x, v.position.x);
                    high.y = std::max(high.y, v.position.y);
                    high.z = std::max(high.z, v.position.z);
                }
            float radius = std::max({high.x - low.x, high.y - low.y, high.z - low.z, 0.01f});
            camera.position = {(low.x + high.x) / 2, (low.y + high.y) / 2 + radius * 0.3f,
                               low.z - radius * 1.5f};
            camera.pitch = -0.15f;
            camera.yaw = 0;
            camera.speed = radius;
            home = camera;
            renderer.set_model(*asset);
            renderer.clear_image();
            view = View::Model;
            displayed_image = {};
            resource_description +=
                std::format("\n{} vertices, {} triangles", mesh.vertices.size(), mesh.indices.size() / 3);
            resource_description += std::format(
                "\n{} scoped materials; {} geometry frames; {} stored LOD faces", mesh.materials.size(),
                asset->geometry.frames.size(), asset->geometry.faces.size());
            for (std::size_t m = 0; m < mesh.materials.size(); ++m) {
                const auto& material = mesh.materials[m];
                resource_description +=
                    std::format("\n{}: flags 0x{:08x}, TXTR:{}", m, material.flags,
                                material.texture.value ? material.texture.text() : "(none)");
            }
            model_vertices = static_cast<int>(mesh.vertices.size());
            model_frame = 0;
            model = std::move(asset);
        } else if (tag == "PREV") {
            displayed_image = decode_preview(bytes);
            renderer.set_image(displayed_image);
            view = View::Image;
        } else if (tag == "TXTR" || tag == "ICON") {
            displayed_image = load_texture(catalog, {selected_archive, index});
            renderer.set_image(displayed_image);
            view = View::Image;
            const auto header = decode_texture_header(bytes);
            resource_description +=
                std::format("\n{}x{}, {} declared frames, {}x{} frame size\nFlags 0x{:08x}, RGB key "
                            "0x{:06x}, {} unused bytes",
                            header.width, header.height, header.frames, header.frame_width,
                            header.frame_height, header.flags, header.key, header.trailing_bytes);
        } else if (tag == "MAPT") {
            auto scope = record.parent;
            while (scope < selected->records().size() &&
                   selected->records()[scope].tag != FourCC::from_text("LAND"))
                scope = selected->records()[scope].parent;
            if (scope >= selected->records().size())
                throw Error("MAPT has no enclosing LAND tileset");
            auto palette = selected->find_chunk(FourCC::from_text("PALT"), FourCC::from_text("LAND"), scope);
            if (!palette)
                throw Error("MAPT tileset has no LAND palette");
            auto tile = decode_terrain_tile(bytes, decode_palette(selected->read(*palette)));
            displayed_image = std::move(tile.mipmaps[0]);
            renderer.set_image(displayed_image);
            view = View::Image;
            resource_description += "\n64x64 terrain tile; seven stored mip levels\nProperties:";
            for (auto value : tile.properties)
                resource_description += std::format(" {:02x}", value);
        } else if (tag == "SAMP") {
            auto sample = decode_sample(bytes);
            auto clip = decode_audio(sample);
            resource_description += std::format("\nAudio: {} Hz, {} channels, {:.2f} seconds",
                                                sample.sample_rate, sample.channels, clip.seconds());
            if (!audio)
                audio = std::make_unique<AudioOutput>();
            audio->play(std::move(clip));
        } else if (tag == "TEXT" || tag == "FNMS") {
            resource_text.assign(reinterpret_cast<const char*>(bytes.data()), bytes.size());
            while (!resource_text.empty() && resource_text.back() == '\0')
                resource_text.pop_back();
        } else {
            if (tag == "TXTR") {
                auto h = decode_texture_header(bytes);
                resource_description +=
                    std::format("\nIndexed texture: {}x{}, palette {}", h.width, h.height, h.palette.text());
            }
            resource_text = "Raw resource bytes (a semantic viewer for this type is not implemented):\n";
            for (std::size_t i = 0; i < std::min<std::size_t>(512, bytes.size()); ++i) {
                if (i % 16 == 0)
                    resource_text += std::format("\n{:04x}: ", i);
                resource_text += std::format("{:02x} ", std::to_integer<unsigned>(bytes[i]));
            }
        }
    }
    void show_saxs(ResourceLocation location, Renderer& renderer) {
        auto asset = std::make_unique<SaxsAsset>(SaxsAsset::load(catalog, location));
        saxs_layout = saxs_topology(asset->model, asset->descriptor, 1);
        saxs_detail = 1;
        saxs_frame = 0;
        saxs_animation.reset();
        saxs_sequence.reset();
        saxs_clip = -1;
        ModelAsset upload;
        upload.mesh = saxs_mesh(asset->model, asset->descriptor, saxs_layout, saxs_bind_pose(asset->model));
        for (const auto& texture : asset->textures)
            upload.textures.push_back(texture);
        renderer.set_model(upload);
        model.reset();
        saxs = std::move(asset);
        renderer.clear_image();
        view = View::Model;
        displayed_image = {};
        if (!saxs->clips.empty())
            select_clip(0, renderer);
        const auto& mesh = upload.mesh;
        Vec3 low = mesh.vertices[0].position, high = low;
        for (const auto& v : mesh.vertices) {
            low = {std::min(low.x, v.position.x), std::min(low.y, v.position.y),
                   std::min(low.z, v.position.z)};
            high = {std::max(high.x, v.position.x), std::max(high.y, v.position.y),
                    std::max(high.z, v.position.z)};
        }
        float radius = std::max({high.x - low.x, high.y - low.y, high.z - low.z, 0.01f});
        focus = {(low.x + high.x) / 2, (low.y + high.y) / 2, (low.z + high.z) / 2};
        // SXCore models face renderer +Z; start in front of them.
        camera.position = {focus.x, focus.y + radius * 0.15f, focus.z + radius * 1.3f};
        camera.pitch = -0.12f;
        camera.yaw = 3.14159265f;
        camera.speed = radius;
        home = camera;
        const auto& m = saxs->model;
        resource_description += std::format(
            "\nSkeletal model: {} body parts, {} bones, {} source vertices\nSAXS scale {}, quality {} / {}\n"
            "Source: {}\n{} compatible SXSK clips in the enclosing folder",
            m.parts.size(), m.bones.size(), m.vertices.size(), saxs->descriptor.scale,
            saxs->descriptor.quality[0], saxs->descriptor.quality[1], saxs->descriptor.source,
            saxs->clips.size());
        for (unsigned p = 0; p < m.parts.size(); ++p) {
            const auto& part = m.parts[p];
            const auto seam = part.patch ? std::format("{} seam links", part.patch->links.size()) : "no seam";
            resource_description +=
                std::format("\nPart {}: {} rings, flags 0x{:04x}, {} explicit faces, {}; SXTX:.{:03} {}x{}, "
                            "material 0x{:08x}",
                            p, part.rings.size(), part.flags, part.faces.size(), seam, p + 1,
                            saxs->textures[p].image.width, saxs->textures[p].image.height,
                            saxs->descriptor.material_flags(p));
        }
    }
    void select_clip(int clip, Renderer& renderer) {
        if (!saxs || clip >= static_cast<int>(saxs->clips.size()))
            throw Error("SAXS clip outside compatible list");
        saxs_animation.reset();
        saxs_sequence.reset();
        saxs_clip = clip;
        saxs_frame = 0;
        if (clip >= 0) {
            const auto& c = saxs->clips[clip];
            saxs_animation = SaxsAnimation::decode(catalog.read(c.animation));
            if (c.sequence)
                saxs_sequence = SaxsSequence::decode(catalog.read(*c.sequence));
        }
        apply_saxs_pose(renderer);
    }
    void apply_saxs_pose(Renderer& renderer, bool rebuild = false) {
        if (!saxs)
            return;
        if (rebuild)
            saxs_layout = saxs_topology(saxs->model, saxs->descriptor, saxs_detail);
        if (saxs_animation && (!std::isfinite(saxs_frame) || saxs_frame < 0 ||
                               saxs_frame > static_cast<float>(saxs_animation->frames.size() - 1)))
            throw Error("SAXS frame outside decoded range");
        // The game evaluates whole frames (SAXSINSTANCE frame << 16).
        const auto pose = saxs_animation ? saxs_pose(saxs->model, *saxs_animation, std::floor(saxs_frame))
                                         : saxs_bind_pose(saxs->model);
        renderer.update_model_mesh(saxs_mesh(saxs->model, saxs->descriptor, saxs_layout, pose));
    }
    void apply_model_pose(Renderer& renderer) {
        if (!model || !std::isfinite(model_frame) || model_frame < 0 ||
            model_frame > model->geometry.frames.size() - 1 ||
            model_vertices < static_cast<int>(model->geometry.minimum_vertices) ||
            model_vertices > static_cast<int>(model->mesh.vertices.size()))
            throw Error("Model frame/detail outside decoded range");
        const auto from = static_cast<unsigned>(model_frame);
        const auto fraction = model_frame - from;
        const auto to = fraction > 0 ? from + 1 : from;
        auto mesh = model->geometry.sample(from, to, fraction, static_cast<unsigned>(model_vertices));
        renderer.update_model_mesh(mesh);
        model->mesh = std::move(mesh);
    }
    // Object ticks continue when the environment is hidden or unavailable. `step` is a deterministic
    // one-tick capture step; normal playback takes variable whole ticks from the game clock.
    // --trace-ai: every five seconds of match time, one line per wizard (see docs/status.md).
    std::uint64_t trace_clock{}, trace_next{};
    // Diagnostic (SACRIFICE_TRACE_ANIM=1): the local side's creatures within 60 of the wizard, one line
    // whenever clip, FSM state, order or order target changes or the clip restarts, plus the wizard's own
    // state. Written to anim_trace.log beside SacrificeModern.log.
    struct AnimTrace {
        std::string key;
        float frame{};
    };
    std::unordered_map<const MatchObject*, AnimTrace> anim_trace;
    std::unique_ptr<std::ofstream> anim_trace_file;
    static bool trace_animation_enabled() {
        static const bool enabled = GetEnvironmentVariableW(L"SACRIFICE_TRACE_ANIM", nullptr, 0) != 0;
        return enabled;
    }
    void trace_animation(Match& match, int ticks) {
        auto* wizard = local_wizard();
        if (!wizard)
            return;
        if (!anim_trace_file) {
            anim_trace_file = std::make_unique<std::ofstream>(application_data() / "anim_trace.log", std::ios::app);
            *anim_trace_file << "=== trace start\n";
        }
        auto& out = *anim_trace_file;
        const auto& w = wizard->motion.position;
        for (const auto& object : match.objects()) {
            auto* actor = dynamic_cast<CreatureActor*>(object.get());
            if (!actor || actor->allegiance != wizard->allegiance || (actor->state_flags & MatchObject::removed_flag))
                continue;
            const auto& p = actor->motion.position;
            const float dx = p.x - w.x, dy = p.y - w.y, distance = std::sqrt(dx * dx + dy * dy);
            if (actor != wizard && distance > 60)
                continue;
            const auto* state = actor->fsm().state();
            const auto order = actor->orders.current();
            std::string key = std::format(
                "{} clip {} state {} flags {:x} order {} target {}", actor->definition().id.text(),
                actor->clip() ? actor->clip()->id.text() : "-", state ? state->id.text() : "-",
                state ? state->flags : 0u, actor->orders.kind(),
                order.target ? std::format("{}:{}", order.target->type, order.target->reference()) : "-");
            auto& entry = anim_trace[actor];
            const bool restarted = key == entry.key && actor->frame() + .001f < entry.frame;
            if (key != entry.key || restarted)
                out << std::format("t{} +{} #{} {}{} frame {:.2f} (was {:.2f}) pos {:.2f},{:.2f},{:.2f} yaw {:.3f} "
                                   "dist {:.1f} controls {},{},{} flags {:x}\n",
                                   match.clock().match, ticks, actor->reference(), restarted ? "RESTART " : "", key,
                                   actor->frame(), entry.frame, p.x, p.y, p.z, actor->motion.yaw, distance,
                                   actor->controls[0], actor->controls[1], actor->controls[2], actor->state_flags);
            entry.key = std::move(key);
            entry.frame = actor->frame();
        }
        out.flush();
    }
    void trace_wizards(Match& match, int ticks) {
        trace_clock += static_cast<unsigned>(std::max(ticks, 0));
        if (trace_clock < trace_next)
            return;
        trace_next = trace_clock + 300;
        for (const auto& object : match.objects()) {
            auto* wizard = dynamic_cast<CreatureActor*>(object.get());
            if (!wizard || wizard->type != MatchObject::wizard || (wizard->state_flags & MatchObject::removed_flag))
                continue;
            const auto order = wizard->orders.current();
            std::string target = "-";
            if (order.target && order.target->source_motion()) {
                const auto& t = order.target->source_motion()->position;
                const auto& p = wizard->motion.position;
                const auto* building = dynamic_cast<const BuildingActor*>(order.target);
                target = std::format("kind {:#x} {} at {:.0f},{:.0f} dist {:.0f}", order.target->kind_flags,
                                     building ? building->part_name().text() : std::string("obj"), t.x, t.y,
                                     std::hypot(t.x - p.x, t.y - p.y, t.z - p.z));
            }
            const auto* state = wizard->fsm().state();
            log(std::format("Trace t{} wizard side {}{}{} at {:.0f},{:.0f} mana {}/{} souls {} health {}/{} "
                            "state {:#x} order {} target {} cast {} remaining {} error {}",
                            trace_clock, wizard->allegiance ? wizard->allegiance->identity : 0u,
                            wizard->computer_wizard ? " computer" : "",
                            wizard->allegiance == match.local_side ? " local" : "", wizard->motion.position.x,
                            wizard->motion.position.y, wizard->mana, wizard->maximum_mana, wizard->soul_count,
                            wizard->stats.current[5], wizard->stats.base[5], state ? state->flags : 0u, order.kind,
                            target, wizard->casting.definition.value ? wizard->casting.definition.text() : "-",
                            wizard->casting.remaining, wizard->casting.error.value ? wizard->casting.error.text() : "-"));
        }
    }
    void tick_scene(float dt, Renderer& renderer, std::optional<int> step = {}, bool upload = true) {
        if (simulation && simulation->match().world_journal)
            simulation->match().world_journal->player(player.get());
        if (const auto command = end_transition.advance(step ? *step / 60.0f : dt)) {
            const auto action = *command;
            if (campaign_state.active)
                finish_campaign(action == FourCC::from_text("rstt"));
            else if (next_script_mission && action != FourCC::from_text("rstt") && end_screen == 1)
                script_transition = true;
            else {
                gameplay_closed = true;
                third_person = false;
                enter_front_end(FourCC::from_text("titl"));
            }
        }
        if (live_audio)
            audio_attempt([&] { ambient_audio->advance(); });
        if (view != View::Terrain)
            return;
        // SMACKTEX::Update runs every tick but only decodes when SmackWait reports the next frame due, so
        // the videos play at their own rate; SmackNextFrame wraps to the first frame after the last.
        for (unsigned i = 0; i < 5; ++i) {
            auto& clip = sky_videos[i];
            if (!clip)
                continue;
            attempt([&] {
                Image frame;
                if (advance_movie(clip, root / scene.environment->sky_videos[i], sky_video_time[i], dt,
                                  frame))
                    renderer.update_sky_texture(i, frame);
            });
        }
        // The match tick (clamped to 12, single ticks for its first 10) drives the objects, and the same
        // ticks reach LAND::Run, as in 0x4525d0.
        auto* match = simulation ? &simulation->match() : nullptr;
        if (match)
            match->direct_control_target = in_cutscene() ? nullptr : local_wizard();
        if (simulation) {
            seed_script_camera(simulation->triggers(), camera);
            if (local_wizard())
                seed_script_zoom(simulation->triggers(), follow);
        }
        const auto ticks = match ? (step ? match->tick(*step) : match->advance(dt * 60.0f))
                                 : (step ? *step : sky_clock.advance(dt * 60.0f));
        frame_ticks = ticks;
        if (match && trace_ai())
            trace_wizards(*match, ticks);
        if (match && ticks > 0 && trace_animation_enabled())
            trace_animation(*match, ticks);
        service_script_presentation();
        service_history_audio();
        if (match) {
            const Vec3 eye{camera.position.x, camera.position.z, camera.position.y};
            if (view == View::Terrain && options.value(FourCC::from_text("shak")))
                for (const auto& request : match->shake_requests)
                    shake.request(eye, request.position, request.amplitude, request.radius);
            match->shake_requests.clear();
        }
        shake.update(ticks);
        if (match && player) {
            player->tick(ticks);
            service_interface_audio(*match, dt);
            // Natively only a side with a wizard (+168) gets the menu; the local skirmish side always has one.
            if (!gameplay_closed && !end_presented && match->local_side && match->local_side->outcome) {
                end_presented = true;
                end_screen = match->local_side->outcome;
                game_menu = false;
                command_menu.close();
                menu_page = FourCC::from_text(end_screen == 1         ? "winr"
                                              : campaign_state.active ? "lose"
                                                                      : "losn");
                if (campaign_state.active)
                    campaign_result(end_screen == 1);
                present_mission_result(*match, end_screen == 1);
            }
        }
        if (match) {
            if (live_audio)
                audio_attempt([&] {
                    // Remastered Look: the world's ambient clips (insects, birds) fall quiet while it rains
                    // or hails. Playback only; the cues were already drawn from the match.
                    const auto& rain = match->weather.effects;
                    const bool storm = remastered && std::max(rain[0].intensity, rain[2].intensity) > .25f;
                    for (const auto& cue : match->ambient_cues)
                        if (!storm)
                            ambient_audio->submit(cue);
                });
            match->ambient_cues.clear();
            update_weather_audio(*effect_audio, *match, rain_voice, live_audio || silent_script_audio,
                                 [this](const auto& work) { audio_attempt(work); });
            service_effect_audio(*match, dt);
            match_ticks += ticks;
            if (match_ticks >= 10)
                music_director->enable(music_on);
            for (const auto& request : match->music_requests) {
                if (request.category == -1) music_director->reset();
                else if (request.category == -2) { music_on = request.weight != 0; music_director->enable(music_on); }
                else music_director->request(request.category, request.weight);
            }
            match->music_requests.clear();
            if (live_audio)
                audio_attempt([&] { music_director->update(ticks, dt, match->tileset().value, false); });
        }
        for (auto& scene_model : scene.models)
            if (scene_model.character && scene_model.character->object) {
                scene_model.character->object->cues.clear(); // Playback awaits the match audio/effect host.
                scene_model.character->object->spell_requests.clear();
                scene_model.character->object->death_requests.clear();
                scene_model.character->object->fall_damage.clear();
                scene_model.character->object->combat_requests.clear();
                scene_model.character->object->burn_requests.clear();
                scene_model.character->object->order_requests.clear();
                scene_model.character->object->credit_requests.clear();
                scene_model.character->object->level_requests.clear();
                scene_model.character->object->grant_requests.clear();
                scene_model.character->object->cast_visual_requests.clear();
            }
        // Building requests are read only by the audio pass above. Drain them every tick, not just on a
        // render upload: ticking without uploads (smoke waits, fast-forward) otherwise grows them without
        // bound, and cr01's credits buildings add ~40 per tick, slowing each later tick.
        for (auto& spawn : scene.spawns)
            if (spawn.kind == MapScene::Spawn::Kind::building && spawn.object) {
                auto& building = static_cast<BuildingActor&>(*spawn.object);
                building.requests.clear();
                building.death_requests.clear();
            }
        sky_state.advance(ticks);
        if (upload)
            sync_scene(renderer);
    }
    struct ParticleTexture {
        TextureHeader header;
        std::shared_ptr<void> view;
    };
    std::map<std::uint32_t, ParticleTexture> particle_textures;
    // Views dropped from the cache stay alive until the renderer has been given batches without them.
    std::vector<std::shared_ptr<void>> retired_particle_views;
    const ParticleTexture& particle_texture(Renderer& renderer, FourCC id) {
        auto [found, inserted] = particle_textures.try_emplace(id.value);
        if (inserted && id.value >= effect_land_edge)
            // Falling land blocks (Bore, Remove Land): the map's edge texture for their walls.
            attempt([&] {
                if (!scene.environment)
                    return;
                if (const auto edge = load_edge_texture(catalog, *scene.environment, terrain.tileset)) {
                    found->second.header = edge->header;
                    found->second.view = renderer.upload_ui_image(edge->image);
                }
            });
        else if (inserted && id.value >= effect_land_tile) {
            // ...and their cell's current map tile (or scar) for the top.
            const auto cell = id.value - effect_land_tile;
            if (world_materials && cell < terrain.tiles.size()) {
                const auto layer = !terrain.scar_layers.empty() && terrain.scar_layers[cell]
                                       ? terrain.scar_layers[cell]
                                       : terrain.tiles[cell];
                const auto& materials = *world_materials;
                const auto* tile = layer < 256 ? (materials.tiles[layer] ? &*materials.tiles[layer] : nullptr)
                                   : layer - 256 < materials.scars.size() ? &materials.scars[layer - 256]
                                                                          : nullptr;
                if (tile) {
                    auto& header = found->second.header;
                    header.width = header.frame_width = tile->mipmaps[0].width;
                    header.height = header.frame_height = tile->mipmaps[0].height;
                    header.frames = 1;
                    found->second.view = renderer.upload_ui_image(tile->mipmaps[0]);
                }
            }
        } else if (inserted && id.value)
            attempt([&] {
                const auto texture = catalog.find_chunk(FourCC::from_text("TXTR"), id);
                if (!texture) {
                    char hex[16];
                    std::snprintf(hex, sizeof hex, "%08x", id.value);
                    throw Error(std::string("missing particle TXTR 0x") + hex);
                }
                found->second.header = decode_texture_header(catalog.read(*texture));
                found->second.view = renderer.upload_ui_image(load_texture(catalog, *texture));
            });
        return found->second;
    }
    // Every live particle of the match's spell objects, grouped by material for the renderer.
    void sync_particles(Renderer& renderer) {
        std::vector<PointLight> lights;
        std::vector<ParticleBatch> batches;
        if (simulation && !remastered) // The Remastered Look's sun shadows replace the native blob shadows.
            append_creature_shadows(simulation->match(), camera, renderer.detail(), batches);
        if (simulation)
            append_match_effects(simulation->match(), camera,
                [&](FourCC id) {
                    const auto& texture = particle_texture(renderer, id);
                    return EffectTexture{texture.header, texture.view.get()};
                }, batches, lights, remastered);
        if (simulation && remastered)
            update_weather_look(weather_look, simulation->match());
        else
            weather_look = {};
        renderer.set_weather(weather_look);
        if (simulation) {
            const auto& match = simulation->match();
            if (scene.environment) {
                auto adjusted = *scene.environment;
                match.weather.fog(adjusted.fog_start, adjusted.fog_end, adjusted.fog_color);
                renderer.update_environment(adjusted);
            }
            if (!simulation->triggers().beacons.empty()) {
            const auto& texture = particle_texture(renderer, FourCC::from_text("safr"));
            if (texture.view) for (const auto& beacon : simulation->triggers().beacons) {
                const auto* target = match.resolve(beacon.object);
                const auto* motion = target ? target->source_motion() : nullptr;
                if (!motion || beacon.alpha <= 0) continue;
                const auto& h = texture.header;
                const unsigned frame = 0; // 0x453b70 uses ticks/30, not the accumulated rotation.
                const unsigned columns = std::max(h.width / std::max(h.frame_width, 1u), 1u);
                const float su = static_cast<float>(h.frame_width) / std::max(h.width, 1u),
                            sv = static_cast<float>(h.frame_height) / std::max(h.height, 1u);
                const float u = frame % columns * su, v = frame / columns * sv;
                const float radius = std::max(1.0f, std::hypot(motion->box_max.x - motion->box_min.x,
                                      motion->box_max.y - motion->box_min.y) * .35355f) * beacon.alpha;
                const auto& pos = motion->position;
                const Vec3 anchor{pos.x + (motion->box_min.x + motion->box_max.x) * .5f,
                                  pos.z + motion->box_max.z - 1, pos.y + (motion->box_min.y + motion->box_max.y) * .5f};
                ParticleBatch batch{0x12012008, texture.view.get()};
                const auto color = 0xffffffu | static_cast<unsigned>(255 * beacon.alpha) << 24;
                constexpr unsigned segments = 64;
                for (unsigned row = 0; row < 2; ++row) for (unsigned j = 0; j <= segments; ++j) {
                    const auto angle = beacon.rotation + 6.28318f * j / segments;
                    const float r = radius * (row ? 1 : .1f), height = row ? 0.0f : 100.0f;
                    const float local_u = static_cast<float>(j) / segments;
                    // The native cone tilts six degrees from vertical and turns about its axis.
                    const float x = std::sin(angle) * r,
                                y = std::cos(angle) * r;
                    batch.mesh.vertices.push_back({{anchor.x + x + std::sin(beacon.rotation) * height * .104528f,
                                                    anchor.y + height * .994522f,
                                                    anchor.z + y + std::cos(beacon.rotation) * height * .104528f}, {},
                                                   {u + local_u * su, v + (row ? sv : 0)}, color});
                }
                for (unsigned j = 0; j < segments; ++j) {
                    const auto next = j + 1, bottom = segments + 1;
                    batch.mesh.indices.insert(batch.mesh.indices.end(), {j, next, j + bottom, next, next + bottom, j + bottom});
                }
                batches.push_back(std::move(batch));
                lights.push_back({true, motion->position, {1, 1, 1}, 0, 1, 50});
            }
            }
        }
        if (player && !menu_page.value && !gameplay_closed) {
            const auto& texture = particle_texture(renderer, FourCC::from_text("aura"));
            if (texture.view)
                for (const auto& marker : player->markers) {
                    ParticleBatch batch{0x1a012009, texture.view.get()};
                    batch.mesh = order_marker_mesh(marker, texture.header,
                                                  {camera.position.x, camera.position.z, camera.position.y});
                    batches.push_back(std::move(batch));
                }
        }
        renderer.set_particles(std::move(batches));
        retired_particle_views.clear();
        renderer.set_point_lights(lights, terrain);
    }
    // Where an object is drawn: blended between ticks while a match runs (Match::presented).
    ObjectMotion shown(const MatchObject& object) const {
        if (simulation)
            return simulation->match().presented(object);
        ObjectMotion result;
        if (const auto* motion = object.source_motion()) {
            result.position = motion->position;
            result.orientation = motion->orientation;
        }
        return result;
    }
    void sync_scene(Renderer& renderer) {
        renderer.append_scene(scene);
        sync_particles(renderer);
        if (simulation && world_materials) {
            const bool changed = apply_terrain_changes(catalog, *simulation, terrain, *world_materials,
                                                       applied_tile_changes, scar_cache);
            if (changed) {
                renderer.update_terrain_tiles(*world_materials, terrain);
                // Falling land blocks look their cells' tiles up again (scars move layers).
                // The renderer still holds this frame's batches, so their views are retired, not freed.
                std::erase_if(particle_textures, [&](const auto& entry) {
                    const bool land = entry.first >= effect_land_tile && entry.first < effect_land_edge;
                    if (land && entry.second.view)
                        retired_particle_views.push_back(entry.second.view);
                    return land;
                });
            }
            if (applied_land_revision != simulation->match().terrain_revision) {
                applied_land_revision = simulation->match().terrain_revision;
                if (preview)
                    renderer.set_mesh(terrain.mesh(false));
                else
                    renderer.update_terrain(terrain);
            }
        }
        // Souls (0x475ee0) advance on the same 1/60 s object ticks; their flat grids hold one material.
        for (std::size_t i = 0; i < scene.models.size(); ++i) {
            auto& soul_model = scene.models[i];
            if (soul_model.spell_visual) {
                const auto* spell = soul_model.spell_visual->object;
                renderer.update_scene_visibility(i, 0, spell && !(spell->state_flags & MatchObject::removed_flag),
                    1, spell && spell->effect == SpellActor::Effect::death
                           ? std::clamp(spell->cast_alpha * 2, 0.f, 1.f) : 1.f);
                if (spell)
                    renderer.update_scene_transform(i, 0, shown(*spell), spell->model_scale);
                if (spell && soul_model.spell_visual->uploaded_frame != spell->model_frame) {
                    const auto& geometry = soul_model.asset.geometry;
                    if (!geometry.frames.empty()) {
                        const float frame = std::clamp(spell->model_frame, 0.f, static_cast<float>(geometry.frames.size() - 1));
                        const auto from = static_cast<unsigned>(frame);
                        renderer.update_scene_model(i, geometry.sample(from,
                            std::min(from + 1, static_cast<unsigned>(geometry.frames.size() - 1)), frame - from));
                    }
                    soul_model.spell_visual->uploaded_frame = spell->model_frame;
                }
            }
            if (soul_model.summon) {
                const auto* spell = soul_model.summon->object;
                const bool echo = spell && spell->effect == SpellActor::Effect::speedup_echo;
                renderer.update_scene_visibility(i, 0, spell && !(spell->state_flags & MatchObject::removed_flag) &&
                                                 (!echo || !spell->echo.hidden),
                                                 1, spell ? std::clamp(static_cast<int>(spell->cast_alpha * 255), 0, 255) / 255.0f : 0);
                if (spell)
                    renderer.update_scene_transform(i, shown(*spell));
                auto& image = *soul_model.summon;
                const auto* clip = echo ? spell->echo.clip.get() : image.clip.get();
                const auto revision = echo ? spell->echo.revision : 1;
                if (spell && clip && image.uploaded_pose != revision) {
                    const auto& body = *image.body;
                    auto mesh = saxs_mesh(body.model, body.descriptor, *image.topology,
                                         saxs_pose(body.model, clip->animation,
                                                   echo ? static_cast<float>(spell->echo.frame) : 0));
                    renderer.update_scene_model(i, mesh);
                    image.uploaded_pose = revision;
                }
            }
            if (soul_model.character) {
                auto& character = *soul_model.character;
                auto* object = character.object;
                if (!object)
                    renderer.update_scene_visibility(i, 0, false);
                if (object) {
                    object->prepare_model();
                    renderer.update_scene_texture(i, object->texture_override.value ?
                        particle_texture(renderer, object->texture_override).view.get() : nullptr, object->grayscale);
                    auto transform = shown(*object);
                    if (object->strafe_mode) {
                        const float vx = object->motion.local_velocity.x, vy = object->motion.local_velocity.y;
                        float target_offset = 0.0f;
                        if (std::abs(vx) > 1e-4f) {
                            target_offset = vy >= 0 ? -std::atan2(vx, std::max(vy, std::abs(vx) * 0.36f))
                                                    : std::atan2(vx, std::max(-vy, std::abs(vx) * 1.4f));
                        }
                        object->strafe_visual_yaw += (target_offset - object->strafe_visual_yaw) * 0.2f;
                        if (std::abs(object->strafe_visual_yaw) > 1e-4f) {
                            const float cs = std::cos(object->strafe_visual_yaw),
                                        sn = std::sin(object->strafe_visual_yaw);
                            const auto o = transform.orientation;
                            for (int k = 0; k < 3; ++k) {
                                transform.orientation[k] = cs * o[k] + sn * o[3 + k];
                                transform.orientation[3 + k] = -sn * o[k] + cs * o[3 + k];
                            }
                        }
                    } else {
                        object->strafe_visual_yaw = 0.0f;
                    }
                    renderer.update_scene_transform(i, transform);
                    renderer.update_scene_visibility(
                        i, 0, !(object->state_flags & MatchObject::removed_flag), 1,
                        static_cast<unsigned>((1.0 - static_cast<double>(object->fade) * object->fade) *
                                              255.0) /
                            255.0f);
                }
                if (object && object->clip() && character.uploaded_pose != object->pose_revision()) {
                    const auto& body = *character.body;
                    auto mesh =
                        saxs_mesh(body.model, body.descriptor, *character.topology, object->model_pose());
                    renderer.update_scene_model(i, mesh);
                    character.uploaded_pose = object->pose_revision();
                }
            }
            if (!soul_model.soul)
                continue;
            const auto& header = soul_model.asset.textures.front()->header;
            if (!soul_model.placements.empty()) {
                const auto& p = soul_model.placements.front().position;
                ObjectMotion transform;
                transform.position = {p.x, p.z, p.y};
                // Drawn between ticks like the other objects; a bundle's ring offset rides along.
                if (const auto* soul = soul_model.soul_object; soul && simulation) {
                    const auto shown = simulation->match().presented(*soul).position;
                    const auto& now = soul->motion.position;
                    transform.position = {transform.position.x + shown.x - now.x,
                                          transform.position.y + shown.y - now.y,
                                          transform.position.z + shown.z - now.z};
                }
                renderer.update_scene_transform(i, transform);
                renderer.update_scene_visibility(i, 0,
                                                 soul_model.visible.empty() || soul_model.visible.front());
            }
            attempt([&] { renderer.update_scene_model(i, soul_mesh(*soul_model.soul, header).vertices); });
        }
        if (simulation)
            simulation->sync_buildings();
        for (auto& spawn : scene.spawns)
            if (spawn.kind == MapScene::Spawn::Kind::building && !spawn.object) {
                for (unsigned state = 0; state < 3; ++state)
                    if (spawn.state_models[state] != SIZE_MAX)
                        renderer.update_scene_visibility(spawn.state_models[state],
                                                         spawn.state_placements[state], false);
            } else if (spawn.kind == MapScene::Spawn::Kind::building) {
                auto& building = static_cast<BuildingActor&>(*spawn.object);
                for (unsigned state = 0; state < 3; ++state)
                    if (spawn.state_models[state] != SIZE_MAX) {
                        const auto model_index = spawn.state_models[state],
                                   placement = spawn.state_placements[state];
                        renderer.update_scene_transform(model_index, placement, shown(building),
                                                        building.model_scale);
                        renderer.update_scene_visibility(model_index, placement,
                                                         scene.models[model_index].visible[placement],
                                                         building.reveal);
                    }
                building.death_requests.clear();
                building.requests.clear(); // Execution remains with the future spell/effect/audio host.
            }
        if (scene.environment && sky_textures)
            attempt([&] {
                renderer.update_sky(sky_mesh(*scene.environment, *sky_textures, sky_state).vertices);
            });
    }
    // The sound manager's object requests (0x44de40 and its direct +52 callers): spell definition events,
    // creature combat/animation sounds, building loops and soul pickups, mixed at the camera listener.
    void service_spell_audio(Match& match, SpellActor& spell) {
        update_spell_audio(*effect_audio, match, spell, live_audio || silent_script_audio,
                           [this](const auto& work) { audio_attempt(work); });
    }
    void service_effect_audio(Match& match, float dt) {
        effect_audio->listener({camera.position.x, camera.position.z, camera.position.y},
                               {std::cos(camera.yaw), -std::sin(camera.yaw), 0});
        update_object_audio(*effect_audio, match, live_audio || silent_script_audio,
                            [this](const auto& work) { audio_attempt(work); });
        if (live_audio || silent_script_audio)
            audio_attempt([&] {
                effect_audio->advance(dt, [&](std::uint32_t ref) -> std::optional<Vec3> {
                    const auto* object = match.resolve(ref);
                    const auto* motion = object ? object->source_motion() : nullptr;
                    return motion ? std::optional(motion->position) : std::nullopt;
                });
            });
    }
    float play_sound(FourCC sample) {
        float seconds = 0;
        if (live_audio)
            audio_attempt([&] {
                AmbientCue cue;
                cue.sample = sample;
                seconds = ambient_audio->submit(cue);
            });
        return seconds;
    }
    const Triggers::JournalEntry* history_entry() const {
        if (!simulation || history_row >= simulation->triggers().journal.size())
            return nullptr;
        return &simulation->triggers().journal[history_row];
    }
    void stop_history_audio() {
        if (history_voice)
            effect_audio->fade(std::exchange(history_voice, 0), 0, 0, true);
        history_pending = false;
    }
    void select_history(std::size_t row) {
        stop_history_audio(); // 0x474b60 replaces, rather than overlaps, the previous recording.
        history_row = row;
        history_scroll = 0;
        history_frames = 0;
        if (const auto* entry = history_entry()) {
            history_pending = entry->sample.value != 0;
            log(std::format("Speech history {}/{}: {} [{}]", row + 1,
                simulation->triggers().journal.size(), entry->title, entry->sample.text()));
        }
    }
    void service_history_audio() {
        if (menu_page != FourCC::from_text("hist")) {
            stop_history_audio();
            return;
        }
        // FORM:hist's spch update callback (0x474a00) starts once on its sixteenth UI update,
        // even while the match is paused. Silent nodes neither play nor acquire a voice.
        if (!history_pending || history_frames++ != 15)
            return;
        history_pending = false;
        if (const auto* entry = history_entry(); entry && (live_audio || silent_script_audio))
            audio_attempt([&] {
                EffectSound sound{entry->sample, entry->sample, 0x6000400, 64, 2}; // 0x473910.
                sound.volume = .5f;
                history_voice = effect_audio->play(sound);
                log(std::format("Speech history replay: {} voice {}", entry->sample.text(), history_voice));
            });
    }
    bool in_cutscene() const {
        return view == View::Terrain && simulation && !gameplay_closed && !end_presented && !front_page.value &&
               simulation->triggers().cinematic();
    }
    unsigned interface_locks() const {
        return simulation ? simulation->triggers().interface_locks : 0;
    }
    bool minimap_hit(Vec2 point, unsigned width, unsigned height) const {
        return !(interface_locks() & 4) && MiniMapLayout::make(width, height).contains(point);
    }
    void service_script_presentation() {
        if (!simulation)
            return;
        auto& triggers = simulation->triggers();
        if (!triggers.choices.empty() && script_choice != triggers.choices.front().id) {
            const auto& choice = triggers.choices.front();
            script_choice = choice.id;
            menu_page = FourCC::from_text("chos");
            choice_paused = !triggers.cinematic() && !simulation->match().paused;
            if (choice_paused)
                simulation->match().paused = true;
            if ((live_audio || silent_script_audio) && choice.sample.value)
                audio_attempt([&] { choice_voice = effect_audio->play({choice.sample, choice.sample, EffectSound::channel_cap | EffectSound::priority, 0x74616c6b, 1}); });
            log("Script choice: " + triggers.text(choice.question));
        }
        if (std::exchange(triggers.close_spell_info, false) && menu_page == FourCC::from_text("bkit")) {
            menu_page = {};
            simulation->match().paused = false;
        }
        const bool speech_allowed = !cinematic_fast_forward && triggers.choices.empty() &&
                                    menu_page != FourCC::from_text("hist");
        update_script_audio(script_audio, *effect_audio, triggers, live_audio || silent_script_audio,
                            speech_allowed, !cinematic_fast_forward,
                            [this](const auto& work) { audio_attempt(work); });
        if (silent_script_audio) {
            const auto speaking = std::ranges::count_if(effect_audio->voices(), [](const auto& voice) {
                return voice.request.channel == 0x74616c6b && !voice.stop_at_target;
            });
            if (speaking > 1)
                throw Error("Overlapping scripted speech voices");
        }
        if (in_cutscene()) {
            command_menu.close();
            if (player) {
                player->cancel_box();
                player->cancel_target();
                if (auto* wizard = player->wizard())
                    wizard->input_provider = {};
            }
            keyboard_engaged = false;
            orbit_button.reset();
            orbit_allowed = {};
            click_allowed = {};
            minimap_drag.reset();
            left_world = dragging = minimap_box = false;
            cinematic_view(triggers, camera, follow); // Leaving it restarts the follow camera at the wizard.
            if (const auto* wizard = local_wizard())
                exit_blend.watch(triggers, camera, wizard->reference(), simulation->match().clock().match);
        }
    }
    void script_overlay(unsigned width, unsigned height, Renderer& renderer) {
        if (end_presented || end_transition.action.value || view != View::Terrain || !simulation || front_page.value)
            return;
        auto& draw = *ImGui::GetForegroundDrawList();
        const float w = static_cast<float>(width), h = static_cast<float>(height);
        const auto portrait = [&](const Triggers::Dialogue& line) {
            // Talking sequence 0x473980: looping Smacker face and an additive frame128 ring.
            if (talking_movie_line != line.id) {
                talking_movies = {};
                talking_movie_line = line.id;
                const auto paths = talking_movie_paths(root, line.god, line.emotion);
                for (unsigned i = 0; i < paths.size(); ++i)
                    talking_movies[i].path = paths[i];
            }
            float portrait_half_height = 0;
            const float centre_y = h / 5;
            for (unsigned layer = 0; layer < talking_movies.size(); ++layer) {
                auto& movie = talking_movies[layer];
                if (movie.path.empty())
                    continue;
                attempt([&] {
                    Image frame;
                    if (advance_movie(movie.clip, movie.path, movie.time, ImGui::GetIO().DeltaTime, frame)) {
                        // SMACKTEX uses black as the face's chroma key. The ring uses additive blend.
                        if (!layer)
                            for (auto& pixel : frame.rgba)
                                if (!(pixel & 0xffffffu))
                                    pixel = 0;
                        movie.width = frame.width;
                        movie.height = frame.height;
                        movie.image = renderer.upload_ui_image(frame);
                    }
                });
                if (movie.image) {
                    const float half_w = movie.width * .5f, half_h = movie.height * .5f;
                    portrait_half_height = std::max(portrait_half_height, half_h);
                    if (layer)
                        renderer.ui_additive(draw);
                    draw.AddImage((ImTextureID)movie.image.get(), {w * .5f - half_w, centre_y - half_h},
                                  {w * .5f + half_w, centre_y + half_h});
                    if (layer)
                        draw.AddCallback(ImDrawCallback_ResetRenderState, nullptr);
                }
            }
            return h / 5 + portrait_half_height;
        };
        const auto fonts = [&](std::string_view name) -> HudFontImage {
            const auto* font = native_font(name, renderer);
            return {font ? &font->font : nullptr,
                    font ? static_cast<ImTextureID>(reinterpret_cast<std::uintptr_t>(font->image.get())) : ImTextureID{}};
        };
        auto paint = menu_painter(draw, renderer);
        paint_script_overlay(draw, paint, simulation->triggers(), width, height, fonts, portrait,
                             text_record(FourCC::from_text("GMTu")));
    }
    // 0x461bf0 click groups resolve in SSET:ifac; the feedback slots play one at a time, slot 0 from
    // SSET:advi and slots 1/2 from the speaker's own sets (0x460f30), each emptied once started.
    void service_interface_audio(Match& match, float dt) {
        update_interface_audio(*player, match, interface_set.get(),
                               options.value(FourCC::from_text("nadv")) ? nullptr : advisor_set.get(),
                               voice_busy, dt, in_cutscene() || !simulation->triggers().dialogue.empty(),
                               [this](FourCC sample) { return play_sound(sample); });
    }
    CreatureActor* local_wizard() const {
        return view == View::Terrain && simulation && player && third_person && !gameplay_closed ? player->wizard() : nullptr;
    }
    // Before the match tick: poll the keyboard (0x451ea0); once any steering key was used the wizard takes
    // its input every frame (+1701 copied to the camera target's +2204 by 0x4525d0, read by 0x45b350).
    void drive(const InputState& input) {
        cinematic_fast_forward = in_cutscene() && input.scancodes[15] &&
                                 simulation->triggers().choices.empty(); // Hold Tab.
        if (in_cutscene()) {
            if (auto* wizard = local_wizard())
                wizard->input_provider = {};
            keyboard_engaged = false;
            keyboard.poll({});
            return;
        }
        keyboard.poll(ImGui::GetIO().WantCaptureKeyboard ? std::array<bool, 256>{} : input.scancodes);
        auto* wizard = local_wizard();
        if (wizard != driven) {
            driven = wizard;
            keyboard_engaged = false;
            reset_follow(follow);
            orbit_button.reset();
            orbit_allowed = {};
            dragging = false;
        }
        if (!wizard)
            return;
        wizard->strafe_mode = options.value(FourCC::from_text("stfe")) != 0;
        if (wizard->strafe_mode && !ImGui::GetIO().WantCaptureKeyboard) {
            const bool q = input.scancodes[16], e = input.scancodes[18];
            wizard->strafe_turn = (e ? 127 : 0) - (q ? 127 : 0);
            keyboard_engaged |= q || e;
        } else {
            wizard->strafe_turn = 0;
        }
        keyboard_engaged |= keyboard.engaged();
        if (keyboard_engaged)
            wizard->input_provider = [axes = keyboard.axes()] { return axes; };
    }
    // 0x46de40 -> 0x452bc0: an order queued for the followed wizard (a cast) hands it back to its order
    // queue until the next key press (0x451ea0).
    void release_wizard() {
        keyboard_engaged = false;
        if (auto* wizard = local_wizard())
            wizard->input_provider = {};
    }
    // After the match tick: native middle/Ctrl-left drags orbit the view ('drag', 0x4810c0).
    bool follow_wizard(const InputState& input, unsigned width, unsigned height) {
        if (in_cutscene())
            return true;
        auto* wizard = local_wizard();
        if (!wizard || !width || !height)
            return false;
        const float aspect = static_cast<float>(height) / static_cast<float>(width),
                    half_width = 45.0f * std::min(1.0f, 0.75f / aspect), half_height = 45.0f * std::min(aspect, 0.75f);
        for (const auto& event : input.pointer_events) {
            const auto button = static_cast<unsigned>(event.button);
            if (event.kind == PointerEvent::Kind::press)
                orbit_allowed[button] = !ImGui::GetIO().WantCaptureMouse &&
                    !over_gameplay_hud(event.point, width, height) &&
                    !minimap_hit(event.point, width, height);
            if (event.kind == PointerEvent::Kind::drag_start && orbit_allowed[button] &&
                (event.button == PointerButton::middle ||
                 (event.button == PointerButton::left && (event.modifiers & 1))))
                orbit_button = event.button;
            if (orbit_button == event.button && event.kind == PointerEvent::Kind::drag) {
                follow.drag(*wizard, event.delta.x, event.delta.y, width, height,
                            half_width, half_height, false);
                dragging = true;
            }
            if (orbit_button == event.button && event.kind == PointerEvent::Kind::drag_end) {
                follow.drag(*wizard, 0, 0, width, height, half_width, half_height, true);
                orbit_button.reset();
                dragging = false;
            }
        }
        if (input.pointer_cancelled && dragging) {
            follow.drag(*wizard, 0, 0, width, height, half_width, half_height, true);
            orbit_button.reset();
            dragging = false;
        }
        if (!ImGui::GetIO().WantCaptureMouse && (options.value(FourCC::from_text("ctrl")) & 4)) // 'mwzm'.
            if (!spell_bar_hit({input.cursor_x, input.cursor_y}, width, height))
                if (!minimap_hit({input.cursor_x, input.cursor_y}, width, height))
                    follow.wheel(input.wheel);
        follow.edge_scroll(*wizard, {input.cursor_x, input.cursor_y}, width, height, {input.mouse_x, input.mouse_y},
                           frame_ticks, static_cast<unsigned>(options.value(FourCC::from_text("ctrl"))),
                           input.pointer_inside);
        follow_view(follow, *wizard, terrain, simulation->triggers(), script_zoom_revision, frame_ticks,
                    keyboard.held(), camera);
        exit_blend.apply(camera, *wizard, terrain, simulation->match().clock().match, frame_ticks);
        return true;
    }
    bool over_gameplay_hud(Vec2 point, unsigned width, unsigned height) const {
        return player && gameplay_hud_hit(*player, interface_locks(), point, width, height, false, false);
    }
    CreatureActor* panel_creature(Vec2 point, unsigned width, unsigned height) const {
        return player ? selection_portrait(*player, point, width, height) : nullptr;
    }
    FourCC panel_ability() const {
        return player && scene.spell_registry ? selection_ability(*player, *scene.spell_registry) : FourCC{};
    }
    MiniMapTransform minimap_transform() const {
        return minimap_control.transform(terrain, {camera.position.x, camera.position.z, camera.position.y}, camera.yaw);
    }
    void update_box(const ViewBasis& basis, unsigned width, unsigned height) {
        select_gameplay_box(*player, terrain, basis, minimap_transform(), minimap_box, box_origin,
                            box_cursor);
    }
    MatchObject* pick_world(const ViewBasis& basis, Vec2 pixel, unsigned width, unsigned height, Vec3& point,
                            bool& ground, const Renderer& renderer) {
        return pick_gameplay(
            simulation->match(), *player, pointer_feedback, terrain, basis, minimap_transform(),
            interface_locks(), pixel, point, ground, [&](const BuildingActor& building) {
                for (const auto& spawn : scene.spawns)
                    if (spawn.object == &building && building.stage() < 3)
                        return renderer.scene_detail_vertices(spawn.state_models[building.stage()],
                                                              spawn.state_placements[building.stage()]);
                return 0u;
            });
    }
    void bind_player_control() {
        auto& match = simulation->match();
        simulation->triggers().is_selected = [this](const MatchObject& object) {
            return player && !player->selecting_box() && player->selection.contains(&object);
        };
        match.forget_interface = [this](MatchObject& object) {
            if (player)
                player->forget(object);
        };
        simulation->triggers().show_tip = [this](FourCC id) {
            if (!options.value(FourCC::from_text("tipo"))) return;
            tip_text = id;
            tip_texts.clear();
            if (!id.value) {
                if (const auto folder = catalog.find_chunk(FourCC::from_text("FLDR"), FourCC::from_text("tips")))
                    for (auto i = folder->record + 1; i < catalog.archive(*folder).records()[folder->record].subtree_end; ++i) {
                        const auto& record = catalog.archive(*folder).records()[i];
                        if (record.parent == folder->record && record.tag == FourCC::from_text("TEXT")) tip_texts.push_back(record.name);
                    }
                if (!tip_texts.empty()) {
                    tip_index = static_cast<std::size_t>(simulation->match().random().next()) * tip_texts.size() / 0x8000;
                    tip_text = tip_texts[tip_index];
                }
            }
            menu_page = FourCC::from_text("tipb");
            simulation->match().paused = true;
        };
        if (!player || &player->side() != match.local_side) {
            player = std::make_unique<PlayerControl>(match, *match.local_side);
            command_menu.load(catalog);
            pointer_feedback.load(catalog);
            if (const auto rules = catalog.find_chunk(FourCC::from_text("IFAC"), FourCC::from_text("snds")))
                player->sound_rules = PlayerControl::decode_sound_rules(catalog.read(*rules));
            const auto sound_set = [&](const char* id) -> std::shared_ptr<const SoundSet> {
                if (const auto set = catalog.find_chunk(FourCC::from_text("SSET"), FourCC::from_text(id)))
                    return std::make_shared<SoundSet>(SoundSet::decode(catalog.read(*set)));
                return {};
            };
            interface_set = sound_set("ifac");
            advisor_set = sound_set("advi");
        }
        if (match.world_journal)
            match.world_journal->player(player.get());
    }
    // Click-release selection and left-drag boxes follow IFAC:norm/mdrg and 0x453120/0x4813e0.
    // IFAC:norm opens xmnu on right-down; IFAC:xmnu selects on right-up or drag-end.
    // Keys follow the default
    // binding table 0x4c78b8 by scan code: 1-0 'selg', ctrl 'assg', shift 'addg', R 'tatt', F 'tgua' and
    // I O P J K L [ ] the formations, Z C N M G H T the 'Kspl' spell keys. Esc cancels a pending target
    // (natively 'menu'); F9 toggles the camera.
    void command(const InputState& input, unsigned width, unsigned height, Renderer& renderer) {
        menu_keys = {input.pressed_codes[28], input.pressed_codes[1]};
        pointer_feedback.advance(ImGui::GetIO().DeltaTime);
        pointer_icon = FourCC::from_text("Cnor");
        pointer_warning = {};
        pointer_alpha = 255;
        pointer_position = {input.cursor_x, input.cursor_y};
        const auto& codes = input.scancodes;
        const auto earlier = previous_codes;
        const auto pressed = [&](unsigned code) {
            return input.pressed_codes[code] || (codes[code] && !earlier[code]);
        };
        previous_codes = codes;
        if (end_transition.action.value) {
            pause_overlay(width, height, renderer);
            return;
        }
        if (simulation && !simulation->triggers().choices.empty()) {
            pause_overlay(width, height, renderer);
            return;
        }
        if (in_cutscene()) {
            pointer_feedback.reset();
            if (pressed(28) || pressed(57) || clicked(input))
                simulation->triggers().dismiss_dialogue(true);
            return;
        }
        if (front_page.value) {
            pointer_feedback.reset();
            front_end(width, height, renderer);
            return;
        }
        if (view != View::Terrain || !simulation || gameplay_closed)
            return;
        if (!menu_page.value && !simulation->triggers().dialogue.empty()) {
            const auto& line = simulation->triggers().dialogue.front();
            if (line.started && (line.ending == 2 || line.ending == 3) &&
                (pressed(28) || clicked(input))) {
                simulation->triggers().dismiss_dialogue();
                return;
            }
        }
        auto& match = simulation->match();
        if (!match.local_side) {
            player.reset();
            return;
        }
        bind_player_control();
        if (smoke_target.value) {
            if (!player->spell_key(smoke_target) || !player->pending.value)
                throw Error("--scene-target requires a learned targeted spell");
            smoke_target = {};
        }
        const ViewBasis basis(camera, width, height);
        player->camera_forward = {std::sin(camera.yaw), std::cos(camera.yaw), 0};
        player->camera_right = {std::cos(camera.yaw), -std::sin(camera.yaw), 0};
        const bool control = codes[29] || codes[157], shift = codes[42] || codes[54];
        const auto modifiers = [&](unsigned code) {
            return input.pressed_codes[code] ? input.press_modifiers[code]
                                            : static_cast<std::uint8_t>((control ? 1 : 0) | (shift ? 2 : 0));
        };
        player->append = shift;
        command_menu.tick(frame_ticks);
        if (!ImGui::GetIO().WantCaptureKeyboard && !game_menu && !end_screen) {
            auto key_modifiers = input.press_modifiers;
            auto key_pressed = input.pressed_codes;
            for (unsigned code = 0; code < key_modifiers.size(); ++code) {
                key_modifiers[code] = modifiers(code);
                key_pressed[code] = pressed(code);
            }
            if (gameplay_shortcuts(*player, interface_locks(), key_pressed, key_modifiers))
                release_wizard();
            // 'menu' (LABEL_200 of 0x461ed0): Esc cancels target mode ('ctrg'), otherwise it opens the
            // in-game menu, which pauses the match; closing it resumes (0x451d10).
            if (pressed(1) && !end_screen && !player->cancel_target()) {
                command_menu.close();
                player->click(FourCC::from_text("menu"));
                game_menu = !game_menu;
                menu_page = game_menu ? FourCC::from_text("gmnu") : FourCC{};
                menu_open_frame = ImGui::GetFrameCount();
                match.paused = game_menu;
                thumbnail_wanted = game_menu; // 0x451d10 -> 0x4a76f0 as the menu opens.
            }
            if (pressed(59) && !game_menu && !menu_page.value) { // F1 'qsav' (0x4a56a0).
                player->click(FourCC::from_text("qsav"));
                thumbnail_wanted = true;
                saves.quick_save();
            } else if (pressed(60)) { // F2 'qlod' (0x4a5710).
                player->click(FourCC::from_text("qlod"));
                saves.quick_load();
            }
            if (pressed(25) && (modifiers(25) & 1) && !game_menu)
                match.paused = !match.paused; // Ctrl+P 'paws' (0x451c70 with -1).
            if (pressed(87)) { // F11 'shot': the first free shot0000.tga .. shot9998.tga.
                player->click(FourCC::from_text("shot"));
                for (int n = 0; n < 9999; ++n)
                    if (pending_shot = std::format("shot{:04}.tga", n); !std::filesystem::exists(pending_shot))
                        break;
            }
            // Numpad 0..4 'Ktnt' -1, 0..3 (0x482650): the wizard's 'xTNT' line, drawn at random for -1.
            static constexpr std::array<std::pair<unsigned, int>, 5> taunts{
                {{82, -1}, {79, 0}, {80, 1}, {81, 2}, {75, 3}}};
            for (const auto& [code, index] : taunts)
                if (pressed(code) && voice_busy <= 0)
                    if (auto* own = player->wizard())
                        if (const auto sample = own->voice_sample(FourCC::from_text("xTNT"), index); sample.value)
                            voice_busy = play_sound(sample);
            if (pressed(67))
                third_person = !third_person;
            if (pressed(68))
                show_inspector = !show_inspector;
        }
        if (menu_page.value) {
            pointer_feedback.reset();
            pause_overlay(width, height, renderer);
            return; // 0x451d10 hides the gameplay view's interface bit 0x20 while a FORM is open.
        }
        const auto route_click = [&](const PointerEvent& event) {
            if (game_menu || end_screen || ImGui::GetIO().WantCaptureMouse || !click_allowed[static_cast<unsigned>(event.button)])
                return;
            const auto result = gameplay_click(
                *player, command_menu, event, width, height, interface_locks(),
                options.value(FourCC::from_text("xmnu")),
                [&](Vec2 pixel, Vec3& point, bool& ground) {
                    return pick_world(basis, pixel, width, height, point, ground, renderer);
                },
                menu_target, menu_point);
            if (result.release)
                release_wizard();
            if (result.opened)
                menu_frame = 0;
            if (result.cancelled)
                click_allowed[static_cast<unsigned>(event.button)] = false;
        };
        const auto select_menu = [&](Vec2 point) {
            switch (select_gameplay_menu(command_menu, point, *player, match, menu_target, menu_point)) {
            case CommandMenuResult::release_wizard:
                release_wizard();
                break;
            case CommandMenuResult::centre_camera:
                third_person = true;
                reset_follow(follow);
                break;
            default:
                break;
            }
        };
        if (input.pointer_cancelled) {
            command_menu.close();
            player->cancel_box();
            left_world = false;
            click_allowed = {};
            minimap_drag.reset();
            minimap_box = false;
        }
        for (const auto& event : input.pointer_events) {
            const auto button = static_cast<unsigned>(event.button);
            const bool map_origin = minimap_hit(event.origin, width, height);
            if (!command_menu.active() && event.kind == PointerEvent::Kind::drag_start && map_origin &&
                click_allowed[button] && (event.button == PointerButton::middle ||
                (event.button == PointerButton::left && (event.modifiers & 1)))) {
                minimap_drag = event.button;
                pointer_feedback.reset();
            }
            if (minimap_drag == event.button) {
                if (event.kind == PointerEvent::Kind::drag)
                    minimap_control.drag(event.delta.y, height);
                if (event.kind == PointerEvent::Kind::drag_end)
                    minimap_drag.reset();
                continue;
            }
            if (command_menu.active()) {
                if (event.button == PointerButton::right &&
                    (event.kind == PointerEvent::Kind::click || event.kind == PointerEvent::Kind::double_click ||
                     event.kind == PointerEvent::Kind::drag_end))
                    select_menu(event.point);
                else if (event.button == PointerButton::right &&
                         (event.kind == PointerEvent::Kind::drag || event.kind == PointerEvent::Kind::drag_start)) {
                    const auto sound = command_menu.move(event.point);
                    if (sound.value)
                        player->click(FourCC::from_text("xmnu"), sound);
                }
                continue;
            }
            if (event.kind == PointerEvent::Kind::press) {
                click_allowed[button] = !ImGui::GetIO().WantCaptureMouse;
                if (event.button == PointerButton::left)
                    left_world = click_allowed[button] && !over_gameplay_hud(event.point, width, height) &&
                                 !SelectionPanelLayout::make(width, height).bounds.contains(event.point);
                if (event.button == PointerButton::right) // IFAC:targ cancels ('ctrg') on the press itself.
                    route_click(event);
            } else if (event.kind == PointerEvent::Kind::click ||
                       event.kind == PointerEvent::Kind::double_click) {
                route_click(event);
            } else if (event.button == PointerButton::left) {
                if (event.kind == PointerEvent::Kind::drag_start && left_world &&
                    !(event.modifiers & 1) && !player->pending.value) {
                    player->append = (event.modifiers & 2) != 0;
                    player->begin_box();
                    box_origin = event.origin;
                    minimap_box = map_origin;
                }
                if (player->selecting_box()) {
                    box_cursor = event.point;
                    if (event.kind == PointerEvent::Kind::drag_end) {
                        update_box(basis, width, height);
                        player->end_box();
                        minimap_box = false;
                        left_world = false;
                    }
                } else if (event.kind == PointerEvent::Kind::drag_end && left_world &&
                           !(event.modifiers & 1) && player->pending.value) {
                    // targ has no left-drag binding, so the native classifier falls back to a click.
                    route_click(event);
                }
            }
        }
        player->append = shift;
        if (player->selecting_box())
            update_box(basis, width, height);
        // 0x453060's box, then Creature::DrawHUD -> MODEL::ShowHUD. Order confirmations are 3D primitives.
        auto* draw = ImGui::GetBackgroundDrawList();
        if (player->selecting_box() && !minimap_box) {
            const auto box = SelectionRectangle::make(box_origin, box_cursor, width, height);
            draw->AddRect({box.minimum.x, box.minimum.y}, {box.maximum.x, box.maximum.y}, IM_COL32_WHITE);
        }
        draw_unit_huds(basis, width, height, renderer);
        status_meters(width, height, renderer);
        if (!(interface_locks() & 2))
            spell_bar(width, height, renderer);
        selection_panel(width, height, renderer);
        if (!(interface_locks() & 4))
            minimap(width, height, renderer);
        if (player->selecting_box() && minimap_box) {
            const auto box = SelectionRectangle::make(box_origin, box_cursor, width, height);
            draw->AddRect({box.minimum.x, box.minimum.y}, {box.maximum.x, box.maximum.y}, IM_COL32_WHITE);
        }
        if (interface_locks() & 1)
            command_menu.close();
        else
            draw_command_menu(renderer);
        world_pointer(input, basis, width, height, renderer);
        pause_overlay(width, height, renderer);
    }
    void draw_unit_huds(const ViewBasis& basis, unsigned width, unsigned height, Renderer& renderer) {
        auto paint = hud_painter(renderer, *ImGui::GetBackgroundDrawList());
        for (const auto& object : simulation->match().objects()) {
            auto* actor = dynamic_cast<CreatureActor*>(object.get());
            if (!actor || !unit_hud_visible(*player, *actor))
                continue;
            const bool chosen = actor->allegiance == &player->side() && player->selection.contains(actor);
            Vec3 anchor = actor->motion.position;
            if (!actor->write_reference_point(anchor, 0, false))
                continue;
            const auto at = basis.project(anchor, width, height);
            if (!at)
                continue;
            const Vec3 delta{anchor.x - basis.eye.x, anchor.z - basis.eye.y, anchor.y - basis.eye.z};
            const float depth = delta.x * basis.forward.x + delta.y * basis.forward.y + delta.z * basis.forward.z;
            paint_unit_hud(paint, *actor, {at->x, at->y}, depth, width, chosen,
                           actor->allegiance != &player->side());
        }
    }
    void selection_panel(unsigned width, unsigned height, Renderer& renderer) {
        if (!scene.spell_registry)
            return;
        auto paint = hud_painter(renderer, *ImGui::GetBackgroundDrawList());
        auto tooltips = hud_painter(renderer, *ImGui::GetForegroundDrawList());
        paint_selection_panel(paint, tooltips, {width, height, pointer_position}, *player,
                               *scene.spell_registry, panel_ability(), !ImGui::GetIO().WantCaptureMouse,
                               [this](FourCC id) { return text_record(id); });
    }
    void minimap(unsigned width, unsigned height, Renderer& renderer) {
        if (!world_materials || terrain.tiles.empty())
            return;
        auto& match = simulation->match();
        const auto& own = player->side();
        if (!own.fog)
            return;
        if (!minimap_image || minimap_side != own.identity || minimap_revision != own.fog->revision()) {
            minimap_image = renderer.upload_ui_image(terrain_minimap(terrain, *world_materials, own.fog->cells()));
            minimap_revision = own.fog->revision();
            minimap_side = own.identity;
        }
        auto paint = hud_painter(renderer, *ImGui::GetBackgroundDrawList());
        paint_minimap(paint, {width, height, pointer_position}, match, *player, terrain,
                       minimap_transform(), camera.yaw, ViewBasis(camera, width, height).tan_width);
    }
    void world_pointer(const InputState& input, const ViewBasis& basis, unsigned width, unsigned height,
                       Renderer& renderer) {
        if (smoke_hover)
            if (auto* actor = character(*smoke_hover)) {
                const auto& m = actor->motion;
                const Vec3 point{m.position.x + (m.box_min.x + m.box_max.x) * .5f,
                                 m.position.y + (m.box_min.y + m.box_max.y) * .5f,
                                 m.position.z + (m.box_min.z + m.box_max.z) * .5f};
                if (const auto pixel = basis.project(point, width, height))
                    pointer_position = {pixel->x, pixel->y};
            }
        if (smoke_hover_object)
            if (const auto* object = simulation->match().resolve(smoke_hover_object))
                if (const auto* m = object->source_motion()) {
                    const Vec3 point{m->position.x + (m->box_min.x + m->box_max.x) * .5f,
                                     m->position.y + (m->box_min.y + m->box_max.y) * .5f,
                                     m->position.z + (m->box_min.z + m->box_max.z) * .5f};
                    if (const auto pixel = basis.project(point, width, height))
                        pointer_position = {pixel->x, pixel->y};
                }
        if (smoke_pointer)
            pointer_position = *smoke_pointer;
        const bool diagnostic = smoke_hover.has_value() || smoke_pointer.has_value() || smoke_hover_object;
        if (minimap_drag) {
            pointer_icon = pointer_feedback.icon(FourCC::from_text("drag"), FourCC::from_text("mmap"));
            pointer_feedback.reset();
            return;
        }
        if ((!input.pointer_inside && !diagnostic) ||
            (ImGui::GetIO().WantCaptureMouse && !diagnostic) || over_gameplay_hud(pointer_position, width, height)) {
            pointer_feedback.reset();
            return;
        }
        Vec3 point{};
        bool ground = false;
        auto* target = pick_world(basis, pointer_position, width, height, point, ground, renderer);
        const bool map_hit = minimap_hit(pointer_position, width, height);
        const auto context = map_hit ? FourCC::from_text("mmap") : PlayerPointer::context(*player, target, ground);
        const auto feedback = sacrifice::pointer_feedback(pointer_feedback, *player, simulation->match(), target,
            point, ground, context, command_menu.active(), dragging,
            map_hit || panel_creature(pointer_position, width, height),
            diagnostic ? 31.0f : ImGui::GetIO().DeltaTime * 60);
        pointer_icon = feedback.icon;
        pointer_alpha = feedback.alpha;
        pointer_warning = feedback.warning;
        if (feedback.bracket)
            if (const auto geometry = object_hover_bracket(*target,
                {basis.eye, basis.right, basis.up, basis.forward, basis.tan_width, basis.tan_height, width, height}))
                hud_mesh(*ImGui::GetBackgroundDrawList(), *geometry);
        if (feedback.tooltip)
            object_tooltip(renderer, *target, context);
    }
    bool draw_pointer(const InputState& input, Renderer& renderer) {
        if (in_cutscene() && simulation->triggers().choices.empty())
            return false;
        if ((!input.pointer_inside && !smoke_hover && !smoke_pointer && !smoke_hover_object) ||
            (show_inspector && !front_page.value && !menu_page.value && ImGui::GetIO().WantCaptureMouse))
            return false;
        auto paint = hud_painter(renderer, *ImGui::GetForegroundDrawList());
        PointerFeedback feedback;
        feedback.icon = pointer_icon;
        feedback.alpha = pointer_alpha;
        feedback.warning = pointer_warning;
        return paint_pointer(paint, feedback, pointer_position);
    }
    ImGuiHudPainter hud_painter(Renderer& renderer, ImDrawList& draw, std::string_view face = "verdana") {
        const auto* font = native_font(face, renderer);
        return {draw, [this, &renderer](FourCC type, FourCC id) -> HudImage {
            if (type == FourCC::from_text("TXTR") && id == FourCC::from_text("#map"))
                return {static_cast<ImTextureID>(reinterpret_cast<std::uintptr_t>(minimap_image.get())), {}};
            if (type == FourCC::from_text("ICON")) {
                const auto image = spell_icon(id, renderer);
                const auto it = icon_headers.find(id);
                return {image, it == icon_headers.end() ? TextureHeader{} : it->second};
            }
            const auto& source = hud_texture(id, renderer);
            return {static_cast<ImTextureID>(reinterpret_cast<std::uintptr_t>(source.image.get())), source.header};
        }, font ? &font->font : nullptr,
        font ? static_cast<ImTextureID>(reinterpret_cast<std::uintptr_t>(font->image.get())) : ImTextureID{},
        [&renderer](ImDrawList& list) { renderer.ui_additive(list); }};
    }
    void object_tooltip(Renderer& renderer, const MatchObject& object, FourCC context) {
        if (!hover_bar)
            if (const auto location = catalog.find_chunk(FourCC::from_text("GBAR"), FourCC::from_text("hbar")))
                hover_bar = GradientBar::decode(catalog.read(*location));
        auto paint = hud_painter(renderer, *ImGui::GetForegroundDrawList());
        const auto display = ImGui::GetIO().DisplaySize;
        paint_object_tooltip(paint, {static_cast<unsigned>(display.x), static_cast<unsigned>(display.y), pointer_position},
                             *player, object, context, catalog,
                             [this](FourCC id) { return text_record(id); }, hover_bar ? &*hover_bar : nullptr);
    }
    void draw_command_menu(Renderer& renderer) {
        if (!command_menu.visible())
            return;
        auto paint = hud_painter(renderer, *ImGui::GetForegroundDrawList());
        paint_command_menu(paint, command_menu, menu_frame++);
    }
    const MenuForm& native_form(FourCC id) {
        auto it = menu_forms.find(id);
        if (it == menu_forms.end()) {
            const auto location = catalog.find(FourCC::from_text("FORM"), id);
            if (!location)
                throw Error("Missing native menu form " + id.text());
            it = menu_forms.emplace(id, MenuForm::decode(catalog.read(*location))).first;
            adapt_graphics_page(it->second);
        }
        return it->second;
    }
    // Native textured UI quads, with the original half-pixel positions and repeating UVs.
    ImGuiMenuPainter menu_painter(ImDrawList& draw, Renderer& renderer) {
        return {draw,
                1,
                [this, &renderer](FourCC id) {
                    const auto& image = hud_texture(id, renderer).image;
                    return static_cast<ImTextureID>(reinterpret_cast<std::uintptr_t>(image.get()));
                },
                [this, &renderer](std::string_view name) -> HudFontImage {
                    const auto* font = native_font(name, renderer);
                    return {
                        font ? &font->font : nullptr,
                        font ? static_cast<ImTextureID>(reinterpret_cast<std::uintptr_t>(font->image.get()))
                             : ImTextureID{}};
                },
                [&renderer](ImDrawList& list) { renderer.ui_additive(list); },
                false};
    }
    void menu_quad(ImDrawList& draw, Renderer& renderer, const char* texture, HudRect r, ImVec2 a = {0, 0},
                   ImVec2 b = {1, 1}, ImU32 tint = IM_COL32_WHITE) {
        auto paint = menu_painter(draw, renderer);
        paint.quad(FourCC::from_text(texture), r,
                   {Vec2{a.x, a.y}, Vec2{b.x, a.y}, Vec2{a.x, b.y}, Vec2{b.x, b.y}}, tint);
    }
    void menu_button(ImDrawList& draw, Renderer& renderer, HudRect r, bool hover, bool enabled) {
        auto paint = menu_painter(draw, renderer);
        paint_menu_button(paint, r, hover, enabled);
    }
    void menu_quad_uv(ImDrawList& draw, Renderer& renderer, const char* texture, HudRect r,
                      std::array<ImVec2, 4> uv, ImU32 tint) {
        auto paint = menu_painter(draw, renderer);
        std::array<Vec2, 4> coordinates;
        for (unsigned i = 0; i < uv.size(); ++i)
            coordinates[i] = {uv[i].x, uv[i].y};
        paint.quad(FourCC::from_text(texture), r, coordinates, tint);
    }
    void menu_panel(ImDrawList& draw, Renderer& renderer, const MenuForm& form, HudRect bounds) {
        auto paint = menu_painter(draw, renderer);
        paint_menu_panel(paint, form, bounds, !panel_backdrop, text_record(form.title));
    }
    // Option effects the port has hosts for: smpv/strv scale effects and music (0x40cd40 x0.01), dmus
    // silences the music, ctrl bits 8/0x10 invert the follow-camera drag. The rest are read where used.
    void apply_options() {
        const auto option = [&](const char* id) { return options.value(FourCC::from_text(id)); };
        const float level = scene_sound ? scene_volume : 0;
        music_on = !option("dmus");
        follow.invert_x = (option("ctrl") & 8) != 0;
        follow.invert_y = (option("ctrl") & 16) != 0;
        audio_attempt([&] {
            ambient_audio->volume(level * option("smpv") / 100.0f);
            effect_audio->volume(level * option("smpv") / 100.0f);
            music_director->volume(level * option("strv") / 100.0f);
        });
    }
    bool menu_action_enabled(FourCC id) const {
        const auto command = id.text();
        if (menu_page == FourCC::from_text("hist")) {
            const auto size = simulation ? simulation->triggers().journal.size() : 0;
            if (command == "prev") return history_row < size && history_row > 0;
            if (command == "next") return history_row < size && history_row + 1 < size;
        }
        if (menu_page == FourCC::from_text("tipb") && command == "next") return !tip_texts.empty();
        if (command == "vobj")
            return simulation && !simulation->triggers().objectives.empty();
        if (command == "sgam")
            return can_save_world();
        return true;
    }
    bool can_save_world() const {
        return simulation && simulation->match().world_journal && !end_screen && !gameplay_closed;
    }
    // In game a save page replaces the pause page; {} closes it and resumes. From the title, titl and
    // lerr re-enter the front end (lerr also after a failed load closed the game).
    void show_save_page(FourCC page, bool in_game) {
        menu_open_frame = ImGui::GetFrameCount();
        if (in_game && simulation && !gameplay_closed) {
            menu_page = page;
            game_menu = page.value != 0;
            simulation->match().paused = game_menu;
        } else if (page == FourCC::from_text("titl") || page == FourCC::from_text("lerr"))
            enter_front_end(page);
        else
            front_page = page;
    }
    void native_menu_action(FourCC id) {
        auto& match = simulation->match();
        const auto command = id.text();
        if (SavePages::page(menu_page)) {
            saves.action(menu_page, id);
            return;
        }
        if (command == "sgam" && menu_page == FourCC::from_text("gmnu")) {
            if (can_save_world())
                saves.open(FourCC::from_text("sgam"), true);
            return;
        }
        if (command == "lgam" && menu_page == FourCC::from_text("gmnu")) {
            saves.open(FourCC::from_text("lgam"), true);
            return;
        }
        if (end_transition.action.value || !menu_action_enabled(id))
            return;
        if (menu_page == FourCC::from_text("chos")) {
            if (command != "cho1" && command != "cho2")
                return;
            simulation->triggers().answer_choice(command == "cho1" ? 1 : 2);
            menu_page = {};
            if (std::exchange(choice_paused, false))
                match.paused = false;
            if (choice_voice)
                effect_audio->fade(std::exchange(choice_voice, 0), 0, 0, true);
        } else if (menu_page == FourCC::from_text("hist")) { // 0x474a00 / 0x474b60.
            if (command == "prev" || command == "next")
                select_history(command == "prev" ? history_row - 1 : history_row + 1);
            else if (command == "cncl") {
                stop_history_audio();
                menu_page = {};
                game_menu = false;
                match.paused = false;
            } else return;
        } else if (menu_page == FourCC::from_text("tipb")) {
            if (command == "next" && !tip_texts.empty()) {
                tip_index = (tip_index + 1) % tip_texts.size();
                tip_text = tip_texts[tip_index];
            } else if (command == "chek") options.toggle(FourCC::from_text("tipo"));
            else if (command == "clos") { menu_page = {}; match.paused = false; }
        } else if (menu_page == FourCC::from_text("summ")) { // 0x470f80: prev/next pages, Close returns.
            if (command == "prev" || command == "next")
                summary_mode = std::clamp(summary_mode + (command == "next" ? 1 : -1), 0, 2);
            else if (command == "ok__")
                menu_page = summary_return;
            else
                return;
        } else if (command == "hist") {
            menu_page = FourCC::from_text("hist");
            select_history(simulation->triggers().journal.empty() ? SIZE_MAX : simulation->triggers().journal.size() - 1);
        } else if (command == "vobj") { // 0x450b1f: objectives use the native FORM:mobj.
            menu_page = FourCC::from_text("mobj");
        } else if (command == "summ") { // 0x450f70 pushes FORM:summ over winr / lose.
            summary_return = menu_page;
            summary_mode = 0;
            menu_page = id;
        } else if (command == "opts") {
            enter_front_end(FourCC::from_text("opts"));
            options_return = FourCC::from_text("gmnu");
            return;
        } else if (command == "surr") {
            menu_page = FourCC::from_text("surr");
        } else if (command == "suno") {
            menu_page = FourCC::from_text("gmnu");
        } else if (command == "suok") {
            menu_page = {};
            game_menu = false;
            match.paused = false;
            player->side().end_game(match, -2, 1); // 0x450aa5: native surrender.
        } else if (command == "cncl" || command == "obsv" || command == "OBSV") {
            menu_page = {};
            game_menu = false;
            end_screen = 0;
            match.paused = false;
            if (command != "cncl")
                third_person = false;
        } else if (command == "rstt" || command == "ok__" || command == "exit") {
            if (command == "rstt" && !campaign_state.active)
                return;
            end_transition.start(id);
        } else
            return;
        if (player)
            player->click(FourCC::from_text("menu"));
        menu_open_frame = ImGui::GetFrameCount();
    }
    void enter_front_end(FourCC page) {
        stop_history_audio();
        front_page = page;
        menu_page = {};
        game_menu = false;
        end_screen = 0;
        command_menu.close();
        if (simulation)
            simulation->match().paused = true;
        show_inspector = false;
        menu_open_frame = ImGui::GetFrameCount();
        if (page == FourCC::from_text("loby") || page == FourCC::from_text("cgam")) {
            scan_skirmish_maps();
            if (lobby.local < 0)
                bind_lobby(lobby_map);
        }
        if (page == FourCC::from_text("cgam"))
            maps_scroll = static_cast<int>(browsed_map) - 3; // Clamped when the list is painted.
        if (page == FourCC::from_text("opts")) {
            options_page = FourCC::from_text("gopt");
            options_return = {};
        }
    }
    void scan_skirmish_maps() {
        if (!skirmish_maps.empty())
            return;
        attempt([&] {
            for (const auto& entry : std::filesystem::directory_iterator(root / "maps"))
                if (entry.is_regular_file() && entry.path().extension() == ".scp")
                    skirmish_maps.push_back(entry.path());
            std::ranges::sort(skirmish_maps);
            const auto trial = std::ranges::find(skirmish_maps, root / "maps/(2) Trial.scp");
            lobby_map = browsed_map = trial == skirmish_maps.end() ? 0 : trial - skirmish_maps.begin();
        });
    }
    // 'scen' / cgam 'ok__': the chosen map's SIDS slot nibbles decide which lobby rows exist.
    void bind_lobby(std::size_t index) {
        if (index >= skirmish_maps.size())
            return;
        lobby_map = browsed_map = index;
        attempt([&] {
            const auto archive = Archive::open(skirmish_maps[index]);
            std::vector<SideDefinition> sides;
            if (const auto record = archive.find(FourCC::from_text("SIDS")))
                sides = SideDefinition::decode(archive.read(*record));
            lobby.bind(sides, lobby_random);
        });
    }
    // Game type virtuals: +112 (only Skirmish) starts a new character on a random god for every level and
    // lets espb edit the path; +28 (only campaigns) shows the level in cinf.
    bool type_god_path() const { return game_type == "Skirmish"; }
    bool type_campaign() const { return campaign_host && _stricmp(game_type.c_str(), campaign_host->name().c_str()) == 0; }
    bool type_shows_level() const { return type_campaign(); }
    // scmp: the main campaign's game type is its descriptor, maps\campaign\sacrifice.cmp.
    void open_campaign() {
        if (!campaign_host)
            attempt([&] { campaign_host.emplace(root, "sacrifice"); });
        if (campaign_host)
            open_roster(campaign_host->name());
    }
    // 0x49ee20 / 0x4a1c30: save the character at its node and play that mission; with no mission left the
    // campaign ends at the title (+88, 0x49eab0).
    void launch_mission(const std::optional<std::filesystem::path>& mission, Renderer& renderer) {
        if (!mission || !profile) {
            campaign_state.active = false;
            enter_front_end(FourCC::from_text("titl"));
            return;
        }
        profile->wizard = FourCC::from_text("cwe2");
        attempt([&] { roster->save(*profile); });
        campaign_state.active = true;
        campaign_state.outcome = -1;
        campaign_mission = *mission;
        campaign_host->begin_mission();
        end_transition.reset();
        front_page = {};
        queue_load(*mission, renderer, [this, path = *mission](Renderer& renderer) {
            attempt([&] { load(path, renderer); });
            third_person = true;
            show_inspector = false;
            reset_follow(follow);
            exit_blend = {};
            place_campaign_camera(0); // Seed the playing view before a first-tick cinematic takes it.
        });

    }
    // Smoke: 1ply scmp -> rost -> select for one character, created as a new Eldred on first use.
    void smoke_campaign(std::string name, Renderer& renderer) {
        std::optional<int> level; // "Name@N[:EARTH]" starts at a node or one of its god missions.
        std::string choice;
        if (const auto colon = name.find(':'); colon != std::string::npos) {
            choice = name.substr(colon + 1);
            name.resize(colon);
        }
        if (const auto at = name.find('@'); at != std::string::npos) {
            level = std::stoi(name.substr(at + 1));
            name.resize(at);
        }
        open_campaign();
        if (!campaign_host || !roster)
            throw Error("Campaign host failed: " + error);
        const auto found = std::ranges::find_if(characters, [&](const Character& c) { return c.name == name; });
        if (found != characters.end())
            profile = *found;
        else {
            if (Character::validate_name(name))
                throw Error("--campaign requires a valid character name");
            Character created;
            created.name = name;
            created.game_type = campaign_host->name();
            created.reset_wizard(campaign_host->wizards().front());
            profile = created;
        }
        if (level)
            profile->campaign_node = static_cast<std::uint32_t>(*level);
        auto mission = campaign_host->start(*profile);
        if (!choice.empty()) {
            const auto choice_entry = std::ranges::find_if(CampaignHost::choices, [&](const char* entry) {
                return _stricmp(entry, choice.c_str()) == 0;
            });
            if (choice_entry == CampaignHost::choices.end() || !campaign_host->choose(*profile,
                    static_cast<unsigned>(choice_entry - CampaignHost::choices.begin())))
                throw Error("Campaign node does not offer choice " + choice);
            mission = campaign_host->victory(*profile);
        }
        if (!mission)
            throw Error("The character has finished the campaign");
        log("Campaign smoke: " + profile->name + " at node " + std::to_string(profile->campaign_node) + ", " +
            utf8(*mission));
        launch_mission(mission, renderer);
        if (!error.empty())
            throw Error(error);
        if (!campaign_state.wizard) {
            std::string sides;
            for (const auto& placed : scene.models)
                if (placed.character && placed.character->object &&
                    placed.character->object->kind_flags == MatchObject::wizard &&
                    placed.character->object->allegiance) {
                    const auto& definition = placed.character->object->allegiance->definition;
                    sides += std::format(" side {} flags {:#x};", definition.index, definition.flags);
                }
            throw Error("No slot-1 wizard took the character; wizards:" + sides);
        }
    }
    void place_campaign_camera(int ticks) {
        if (!campaign_state.wizard || !simulation || simulation->triggers().cinematic())
            return;
        follow_view(follow, *campaign_state.wizard, terrain, simulation->triggers(), script_zoom_revision, ticks, 0,
                    camera);
    }
    void smoke_portal(std::string choice, Renderer& renderer) {
        const auto found = std::ranges::find_if(CampaignHost::choices, [&](const char* entry) {
            return _stricmp(entry, choice.c_str()) == 0;
        });
        if (found == CampaignHost::choices.end() || found == CampaignHost::choices.begin())
            throw Error("portal requires LIFE, FIRE, EARTH, AIR or DEATH");
        const unsigned god = static_cast<unsigned>(found - CampaignHost::choices.begin());
        const auto* node = campaign_host->node(static_cast<int>(profile->campaign_node));
        if (!node || !node->missions.contains(*found))
            throw Error("This campaign node has no " + choice + " portal");
        const auto expected = campaign_host->mission_path(node->missions.at(*found));
        using namespace trigger_code;
        static constexpr unsigned operands[]{MissionIdEtherealRealm, MissionIdPersephone, MissionIdPyro,
                                               MissionIdJames, MissionIdStratos, MissionIdCharnel};
        const TriggerMarker* destination = nullptr;
        for (const auto& trigger : scene.triggers) {
            bool portal = false;
            for (std::size_t i = 0; i + 1 < trigger.action.size(); ++i)
                portal |= trigger.action[i] == ActionSetNextMissionId && trigger.action[i + 1] == operands[god];
            if (!portal)
                continue;
            for (std::size_t i = 0; i + 2 < trigger.condition.size(); ++i)
                if (trigger.condition[i] == LocationMarker && trigger.condition[i + 1] == ValueMarkerId)
                    for (const auto& marker : scene.markers)
                        if (marker.id == trigger.condition[i + 2])
                            destination = &marker;
        }
        if (!destination)
            throw Error("No scripted portal marker for " + choice);
        const Vec3 target = destination->position;
        bool ordered = false;
        for (unsigned tick = 0; campaign_state.active && tick < 36000; ++tick) {
            place_campaign_camera(1);
            tick_scene(1.0f / 60, renderer, 1, false);
            if (!campaign_state.active)
                break;
            auto& triggers = simulation->triggers();
            if (!triggers.choices.empty()) {
                native_menu_action(FourCC::from_text("cho1"));
                ordered = false;
            }
            if (triggers.cinematic())
                ordered = false;
            else if (!ordered && triggers.dialogue.empty()) {
                if (!campaign_state.wizard->issue_order({2, nullptr, target}))
                    throw Error("Wizard refused portal movement order");
                ordered = true;
            }
            triggers.dismiss_dialogue(); // Supply the click for any manual-duration line.
        }
        if (campaign_state.active)
            throw Error(std::format(
                "Wizard did not reach the scripted {} portal: position {},{},{}; target {},{},{}; cinematic "
                "{}; dialogue {}; unsupported {}",
                choice, campaign_state.wizard->motion.position.x, campaign_state.wizard->motion.position.y,
                campaign_state.wizard->motion.position.z, target.x, target.y, target.z,
                simulation->triggers().cinematic(), simulation->triggers().dialogue.size(),
                simulation->triggers().unsupported().size()));
        service_campaign_transition(renderer);
        if (campaign_mission != expected || !campaign_state.wizard ||
            simulation->match().mission_god != god ||
            campaign_state.wizard->progression.stats[18] != static_cast<int>(god) ||
            campaign_state.wizard->progression.level < simulation->match().minimum_wizard_level)
            throw Error("Portal loaded the wrong mission, god or wizard level");
        log(std::format("Campaign portal {}: loaded {}, god {}, level {}, spells {}", choice,
                        utf8(campaign_mission.filename()), god, campaign_state.wizard->progression.level,
                        campaign_state.wizard->progression.learned.size()));
    }
    void smoke_campaign_actions(std::string actions, Renderer& renderer) {
        while (!actions.empty()) {
            const auto comma = actions.find(',');
            const auto action = actions.substr(0, comma);
            actions = comma == std::string::npos ? std::string{} : actions.substr(comma + 1);
            if (action == "character") {
                if (!profile || !roster)
                    throw Error("Character report requires a campaign profile");
                const auto saved = Character::decode(read_file(roster->path(*profile)));
                log(std::format("Campaign saved character {}: node {} level {} mana {} kills {} gibs {} BeatCampaign {}",
                    saved.name, saved.campaign_node, saved.level, saved.maximum_mana,
                    saved.credit_count_80, saved.credit_count_20, saved.flag("BeatCampaign")));
                for (const auto& flag : saved.flags)
                    log(std::format("Campaign saved flag {} = {}", flag.name, flag.value));
                log(std::format("Campaign roster unlocks: custom path {}, Mithras {}, Marduk {}",
                    roster->custom_path_available(), roster->wizard_available(FourCC::from_text("cwe1")),
                    roster->wizard_available(FourCC::from_text("cwa2"))));
                continue;
            }
            if (action == "skirmish-editor") {
                open_roster("Skirmish");
                open_character_editor(false);
                log(std::format("Campaign unlock UI: custom path {}, button {}", custom_path,
                    front_enabled(FourCC::from_text("newc"), "espb")));
                continue;
            }
            if (!campaign_state.active || !simulation)
                throw Error("Campaign action requires an active mission: " + action);
            bind_player_control();
            if (action == "world-save" || action.starts_with("world-save:")) {
                save_world(action.size() > 11 ? action.substr(11) : std::string(WorldSave::quick_name));
            } else if (action == "world-load" || action.starts_with("world-load:")) {
                load_name = action.size() > 11 ? action.substr(11) : std::string(WorldSave::quick_name);
                restore_world(renderer);
            } else if (action.starts_with("save-name:")) {
                saves.rename(action.substr(10));
            } else if (action.starts_with("menu:")) { // A native FORM command on the open pause page.
                native_menu_action(FourCC::from_text(action.substr(5)));
                for (int i = 0; i < 3; ++i)
                    saves.service();
                service_campaign_transition(renderer);
                log(std::format("Campaign menu {}: page {}, save list {}, selected {} '{}', preview {}",
                                action.substr(5), menu_page.text(), saves.names.size(), saves.row, saves.name,
                                saves.preview ? saves.preview->map : std::string("-")));
            } else if (action == "quick-save") {
                saves.quick_save();
                for (int i = 0; i < 3; ++i)
                    saves.service();
                log(std::format("Campaign quick save: page {}, exists {}", menu_page.text(),
                                WorldSave::exists(WorldSave::quick_name)));
            } else if (action == "quick-load") {
                saves.quick_load();
                service_campaign_transition(renderer);
            } else if (action.starts_with("portal:")) {
                smoke_portal(action.substr(7), renderer);
            } else if (action == "yes" || action == "no") {
                native_menu_action(FourCC::from_text(action == "yes" ? "cho1" : "cho2"));
            } else if (action.starts_with("wait:") || action.starts_with("wait-batch:") ||
                       action.starts_with("wait-choice:") || action.starts_with("wait-play:")) {
                const bool stop_at_choice = action.starts_with("wait-choice:");
                const bool stop_at_play = action.starts_with("wait-play:");
                const bool batched = action.starts_with("wait-batch:");
                bool saw_cinematic = simulation->triggers().cinematic();
                const auto ticks = std::stoul(action.substr(action.find(':') + 1));
                for (unsigned tick = 0; campaign_state.active && tick < ticks;) {
                    const auto step = static_cast<int>(batched ? std::min<unsigned long>(12, ticks - tick) : 1);
                    place_campaign_camera(step);
                    tick_scene(step / 60.0f, renderer, step, false);
                    tick += step;
                    if (end_presented)
                        break; // The match is paused here; further passes cannot advance its outcome.
                    if (!simulation->triggers().choices.empty()) {
                        if (stop_at_choice)
                            break;
                        native_menu_action(FourCC::from_text("cho1"));
                    }
                    simulation->triggers().dismiss_dialogue();
                    saw_cinematic |= simulation->triggers().cinematic();
                    if (stop_at_play && saw_cinematic && !simulation->triggers().cinematic() &&
                        simulation->triggers().dialogue.empty())
                        break;
                }
                if (stop_at_play && campaign_state.active &&
                    (!saw_cinematic || simulation->triggers().cinematic() ||
                     !simulation->triggers().dialogue.empty()))
                    throw Error("Campaign cinematic did not return to play");
                std::string stopped;
                for (const auto code : simulation->triggers().unsupported())
                    stopped += " " + std::to_string(code);
                log("Campaign wait stopping codes:" + (stopped.empty() ? std::string(" none") : stopped));
                service_campaign_transition(renderer);
            } else if (action == "next-line") {
                if (simulation->triggers().dialogue.empty())
                    throw Error("No cinematic line to advance");
                simulation->triggers().dismiss_dialogue(true);
                tick_scene(1.f / 60, renderer, 1, false);
            } else if (action.starts_with("fast:")) {
                const auto frames = std::stoul(action.substr(5));
                if (!in_cutscene())
                    throw Error("Fast-forward requires an active cinematic");
                const auto start = simulation->match().clock().match;
                cinematic_fast_forward = true;
                for (unsigned frame = 0;
                     frame < frames && in_cutscene() && simulation->triggers().choices.empty() &&
                     !end_transition.action.value;
                     ++frame)
                    for (unsigned i = 0; i < 8 && in_cutscene() && simulation->triggers().choices.empty() &&
                                         !end_transition.action.value;
                         ++i)
                        tick_scene(1.f / 60, renderer, 1, false);
                cinematic_fast_forward = false;
                log(std::format("Cinematic fast-forward: {} match ticks", simulation->match().clock().match - start));
            } else if (action.starts_with("marker:") || action.starts_with("near:")) {
                if (simulation->triggers().cinematic())
                    throw Error("Campaign marker movement requires the cinematic to finish first");
                const bool at_actor = action.starts_with("near:");
                const auto id = std::stoul(action.substr(at_actor ? 5 : 7));
                std::optional<Vec3> destination;
                if (at_actor) {
                    for (const auto& object : simulation->match().objects())
                        if (object->script_id == id && object->source_motion())
                            destination = object->source_motion()->position;
                } else if (const auto marker = std::ranges::find(scene.markers, id, &TriggerMarker::id);
                           marker != scene.markers.end())
                    destination = marker->position;
                if (!destination || !campaign_state.wizard)
                    throw Error("Missing campaign smoke destination " + std::to_string(id));
                campaign_state.wizard->motion.position = *destination;
                campaign_state.wizard->motion.position.z = static_cast<float>(
                    terrain_surface(*simulation->match().terrain, destination->x, destination->y));
                campaign_state.wizard->motion.velocity = {};
                campaign_state.wizard->orders.clear();
                if (player)
                    player->cancel_target();
                place_campaign_camera(1);
            } else if (action.starts_with("move:")) {
                if (!player || simulation->triggers().cinematic())
                    throw Error("Campaign movement requires player control");
                const auto separator = action.find(':', 5);
                if (separator == std::string::npos)
                    throw Error("Expected move:ACTOR:MARKER");
                const auto actor_id = std::stoul(action.substr(5, separator - 5));
                const auto marker_id = std::stoul(action.substr(separator + 1));
                MatchObject* actor = nullptr;
                for (const auto& object : simulation->match().objects())
                    if (object->script_id == actor_id)
                        actor = object.get();
                const auto marker = std::ranges::find(scene.markers, marker_id, &TriggerMarker::id);
                if (!actor || marker == scene.markers.end() || !player->select(actor) ||
                    !player->move(nullptr, marker->position))
                    throw Error("Campaign movement could not be issued");
            } else if (action.starts_with("cast:")) {
                const auto spell = FourCC::from_text(action.substr(5));
                if (!player || simulation->triggers().cinematic() || !player->spell_key(spell))
                    throw Error("Campaign spell could not be issued: " + spell.text());
            } else if (action.starts_with("convert:")) {
                // The player's wizard casts Convert (sacc) at the soul anchored to a scripted corpse.
                const auto id = std::stoul(action.substr(8));
                auto* wizard = campaign_state.wizard;
                MatchObject* corpse = nullptr;
                for (const auto& object : simulation->match().objects())
                    if (object->script_id == id)
                        corpse = object.get();
                MatchObject* soul = nullptr;
                for (auto* candidate : simulation->souls())
                    if (corpse && candidate->anchor == corpse)
                        soul = candidate;
                const auto sacc = FourCC::from_text("sacc");
                if (!wizard || !soul || simulation->triggers().cinematic() ||
                    !wizard->issue_spell_order(sacc, soul, soul->source_motion()->position))
                    throw Error(std::format("Campaign convert needs a wizard with sacc and a soul on {}", id));
            } else if (action.starts_with("mana:")) {
                // Route shortcut: refill the player's wizard to N mana, clamped to its maximum.
                auto* wizard = campaign_state.wizard;
                if (!wizard)
                    throw Error("Campaign mana requires a wizard");
                wizard->mana = std::min(static_cast<std::int32_t>(std::stol(action.substr(5))), wizard->maximum_mana);
            } else if (action.starts_with("spell:")) {
                // The player's wizard casts SPELL at the object with a script id (e.g. sacu for desecration),
                // or at @DEF: the player's newest creature of that definition (summons have no script id).
                const auto separator = action.find(':', 6);
                const auto spell = FourCC::from_text(action.substr(6, separator - 6));
                const auto selector = action.substr(separator + 1);
                auto* wizard = campaign_state.wizard;
                MatchObject* target = nullptr;
                for (const auto& object : simulation->match().objects()) {
                    if (object->state_flags & MatchObject::removed_flag)
                        continue;
                    if (selector.starts_with("@")) {
                        const auto* actor = dynamic_cast<const CreatureActor*>(object.get());
                        if (actor && wizard && actor->allegiance == wizard->allegiance &&
                            actor->definition().id.text() == selector.substr(1))
                            target = object.get();
                    } else if (object->script_id == std::stoul(selector))
                        target = object.get();
                }
                const auto& id = selector;
                if (!wizard || !target || !target->source_motion() || simulation->triggers().cinematic() ||
                    !wizard->issue_spell_order(spell, target, target->source_motion()->position))
                    throw Error(std::format("Campaign spell {} could not target {}", spell.text(), id));
                const auto check = wizard->cast_check(spell);
                log(std::format("Campaign spell {}: mana {}, check {} {}", spell.text(),
                                wizard->mana, check.code, check.reason.text()));
            } else if (action.starts_with("souls:")) {
                if (!campaign_state.wizard)
                    throw Error("Campaign soul grant requires a wizard");
                campaign_state.wizard->change_souls(std::stoi(action.substr(6)));
            } else if (action.starts_with("kill-side:")) {
                // Normal player-attributed damage to every creature of one side (spawned ones have no id).
                const auto index = std::stoul(action.substr(10));
                auto& match = simulation->match();
                std::vector<CreatureActor*> targets;
                for (const auto& object : match.objects())
                    if (auto* actor = dynamic_cast<CreatureActor*>(object.get());
                        actor && actor->kind_flags != MatchObject::wizard && actor->allegiance &&
                        actor->allegiance->definition.index == index &&
                        actor->stats.current[CreatureStats::health] > 0)
                        targets.push_back(actor);
                for (auto* actor : targets)
                    actor->damage(match, {0, 10000000, campaign_state.wizard->reference()});
                log(std::format("Campaign side {}: hit {} creatures", index, targets.size()));
            } else if (action.starts_with("destroy-type:")) {
                // Ruin a side's primary buildings of one type through the normal destroy path.
                const auto separator = action.find(':', 13);
                const auto index = std::stoul(action.substr(13, separator - 13));
                auto type = action.substr(separator + 1);
                auto reversed = type;
                std::ranges::reverse(reversed);
                auto& match = simulation->match();
                std::vector<BuildingActor*> targets;
                for (const auto& object : match.objects())
                    if (auto* building = dynamic_cast<BuildingActor*>(object.get());
                        building && !building->primary && building->allegiance &&
                        building->allegiance->definition.index == index &&
                        (building->definition_id.text() == type || building->definition_id.text() == reversed))
                        targets.push_back(building);
                if (targets.empty())
                    for (const auto& object : match.objects())
                        if (auto* building = dynamic_cast<BuildingActor*>(object.get()); building && !building->primary)
                            log(std::format("Campaign building {} side {} id {}", building->definition_id.text(),
                                            building->allegiance ? static_cast<int>(building->allegiance->definition.index) : -1,
                                            building->script_id));
                for (auto* building : targets)
                    building->destroy(match, campaign_state.wizard->reference());
                log(std::format("Campaign side {}: destroyed {} {}", index, targets.size(), type));
            } else if (action == "vars") {
                std::string text;
                for (const auto& [id, name, value] : simulation->triggers().variable_values(0))
                    text += std::format(" v{}[{}]={}", id, name, value);
                log("Campaign variables:" + (text.empty() ? std::string(" none") : text));
            } else if (action.starts_with("units:")) {
                // A side's creatures and wizards: definition, script id, kind and health.
                const auto index = std::stoul(action.substr(6));
                std::string text;
                for (const auto& object : simulation->match().objects())
                    if (const auto* actor = dynamic_cast<const CreatureActor*>(object.get());
                        actor && actor->allegiance && actor->allegiance->definition.index == index)
                        text += std::format(" {}#{}/k{}/h{}", actor->definition().id.text(), actor->script_id,
                                            actor->kind_flags, actor->stats.current[CreatureStats::health]);
                log(std::format("Campaign units side {}:{}", index, text));
            } else if (action == "census") {
                // Live object counts by class and kind, to spot accumulation over a long run.
                std::map<std::string, int> classes;
                for (const auto& object : simulation->match().objects())
                    ++classes[std::format("{}/{}", typeid(*object).name(), object->kind_flags)];
                std::string text;
                for (const auto& [name, total] : classes)
                    text += std::format(" {}={}", name, total);
                log(std::format("Campaign census: {} objects, {} trigger instances;{}",
                                simulation->match().objects().size(), simulation->triggers().instances(), text));
            } else if (action == "counters") {
                for (const auto& value : simulation->triggers().display_values())
                    log(std::format("Campaign counter {} = {}", value.name, value.value));
                if (const auto* wizard = campaign_state.wizard) {
                    std::string spells;
                    for (const auto id : wizard->progression.learned)
                        spells += " " + id.text();
                    log(std::format("Campaign resources: mana {}/{}, souls {}, spells{}", wizard->mana,
                                    wizard->maximum_mana, wizard->soul_count, spells));
                }
            } else if (action.starts_with("inspect:")) {
                const auto id = std::stoul(action.substr(8));
                for (const auto& object : simulation->match().objects())
                    if (object->script_id == id)
                        if (const auto* actor = dynamic_cast<const CreatureActor*>(object.get())) {
                            const auto order = actor->orders.kind();
                            const auto& p = actor->motion.position;
                            log(std::format(
                                "Campaign actor {} {}: position {:.2f},{:.2f},{:.2f}; order {}; state {:x}; "
                                "FSM {:x}; mana {}; shield {}; health {}; side {}",
                                id, actor->definition().id.text(), p.x, p.y, p.z, order, actor->state_flags,
                                actor->fsm().state() ? actor->fsm().state()->flags : 0, actor->mana,
                                actor->named(FourCC::from_text("PROT")) != nullptr,
                                actor->stats.current[CreatureStats::health],
                                actor->allegiance ? static_cast<int>(actor->allegiance->definition.index)
                                                  : -1));
                        }
            } else if (action == "objectives") {
                game_menu = true;
                simulation->match().paused = true;
                native_menu_action(FourCC::from_text("vobj"));
                if (menu_page != FourCC::from_text("mobj"))
                    throw Error("Mission objectives did not open");
                for (const auto& objective : simulation->triggers().objectives)
                    log(std::format("Mission objective: flags {} {}", objective.flags, objective.caption));
            } else if (action == "history") {
                if (simulation->triggers().cinematic())
                    throw Error("Speech history requires the cinematic to finish first");
                game_menu = true;
                simulation->match().paused = true;
                native_menu_action(FourCC::from_text("hist"));
                if (menu_page != FourCC::from_text("hist"))
                    throw Error("Speech history did not open");
            } else if (action.starts_with("history:")) {
                if (menu_page != FourCC::from_text("hist"))
                    throw Error("Speech history navigation requires its menu");
                const auto command = action.substr(8);
                if (command == "playing") {
                    if (!history_voice || !effect_audio->alive(history_voice))
                        throw Error("Speech history recording did not start");
                } else if (command == "close") {
                    const auto voice = history_voice;
                    native_menu_action(FourCC::from_text("cncl"));
                    if (history_voice || history_pending || simulation->match().paused || menu_page.value ||
                        (voice && std::ranges::any_of(effect_audio->voices(), [&](const auto& playing) {
                            return playing.key == voice && !playing.stop_at_target;
                        })))
                        throw Error("Speech history close did not stop playback and resume play");
                } else if (command == "prev" || command == "next") {
                    const auto id = FourCC::from_text(command);
                    if (!menu_action_enabled(id))
                        throw Error("Speech history navigation is unavailable");
                    native_menu_action(id);
                } else throw Error("Unknown speech history action: " + command);
            } else if (action == "hover:manalith") {
                BuildingActor* target = nullptr;
                for (const auto& object : simulation->match().objects())
                    if (auto* building = dynamic_cast<BuildingActor*>(object.get());
                        building && !building->primary && ((building->building_flags & 0x55a0) == 0x55a0 ||
                        (building->building_flags & 0x2810) == 0x2810)) {
                        target = building;
                        break;
                    }
                if (!target)
                    throw Error("No manalith to hover");
                smoke_hover_object = target->reference();
                third_person = false;
                const auto& m = target->motion;
                const float z = m.position.z + (m.box_min.z + m.box_max.z) * .5f;
                camera.position = {m.position.x, z + 12, m.position.y - 25};
                camera.yaw = 0;
                camera.pitch = -std::atan2(12.f, 25.f);
                log(std::format("Hover manalith {}: {}", target->reference(), target->definition_id.text()));
            } else if (action.starts_with("damage:") || action.starts_with("destroy:")) {
                const bool destroy = action.starts_with("destroy:");
                const auto prefix = destroy ? 8u : 7u;
                const auto separator = action.find(':', prefix);
                const auto id = std::stoul(action.substr(prefix, separator == std::string::npos ? separator : separator - prefix));
                const auto amount = separator == std::string::npos ? 10000000 : std::stoi(action.substr(separator + 1));
                const auto source_separator = separator == std::string::npos ? separator : action.find(':', separator + 1);
                auto* source = static_cast<MatchObject*>(campaign_state.wizard);
                if (source_separator != std::string::npos) {
                    source = nullptr;
                    const auto source_id = std::stoul(action.substr(source_separator + 1));
                    for (const auto& object : simulation->match().objects())
                        if (object->script_id == source_id)
                            source = object.get();
                    if (!source)
                        throw Error("Missing campaign damage source " + std::to_string(source_id));
                }
                MatchObject* target = nullptr;
                for (const auto& object : simulation->match().objects())
                    if (object->script_id == id)
                        target = object.get();
                bool hit = false;
                int health = 0;
                if (target && source && amount > 0) {
                    auto& match = simulation->match();
                    if (auto* building = dynamic_cast<BuildingActor*>(target)) {
                        if (destroy) {
                            // Exercise the normal ruin/side-event path independently of the altar ritual.
                            building = building->primary ? building->primary : building;
                            building->destroy(match, source->reference());
                            hit = true;
                        } else
                            hit = building->damage(match, {0, amount, source->reference()});
                        health = building->health;
                    } else if (auto* actor = dynamic_cast<CreatureActor*>(target); actor && !destroy) {
                        hit = actor->damage(match, {0, amount, source->reference()});
                        health = actor->stats.current[CreatureStats::health];
                    }
                }
                if (!hit)
                    throw Error("Campaign damage could not hit object " + std::to_string(id));
                log(std::format("Campaign object {}: health {}", id, health));
            } else if (action == "win" || action == "lose") {
                auto& match = simulation->match();
                match.local_side->end_game(match, action == "win" ? 1 : -1, 1);
                tick_scene(0, renderer, 0, false);
            } else if (action == "end") {
                simulation->triggers().mission_over();
                service_campaign_transition(renderer);
            } else if (action == "menu") {
                game_menu = true;
                menu_page = FourCC::from_text("gmnu");
                simulation->match().paused = true;
            } else if (action == "restart" || action == "ok") {
                if (action == "ok" && !end_presented)
                    throw Error("Campaign OK requires a victory or defeat page");
                const auto mission = campaign_mission;
                const auto snapshot = campaign_state.snapshot;
                native_menu_action(FourCC::from_text(action == "restart" ? "rstt" : "ok__"));
                tick_scene(1, renderer, 60, false);
                service_campaign_transition(renderer);
                if (action == "restart" &&
                    (campaign_mission != mission || !snapshot || !campaign_state.snapshot ||
                     campaign_state.snapshot->encode() != snapshot->encode()))
                    throw Error("Campaign restart changed the mission or starting character");
            } else
                throw Error("Unknown --campaign-actions entry: " + action);
            log(std::format("Campaign action {}: page {}, mission {}, node {}, level {}, result {}", action,
                            menu_page.value    ? menu_page.text()
                            : front_page.value ? front_page.text()
                                               : "play",
                            utf8(campaign_mission.filename()), profile->campaign_node, profile->level,
                            campaign_state.outcome));
        }
        sync_scene(renderer);
        if (!error.empty())
            throw Error(error);
    }
    // The newest wizard on the local side is the one created after NTTS by 0x457750.
    // Scripted wizards on that same side keep their own bodies and progression.
    void bind_campaign_wizard() {
        campaign_state.wizard = nullptr;
        if (!campaign_state.active || !profile || !simulation)
            return;
        campaign_state.enter(*profile, simulation->match());
        auto& triggers = simulation->triggers();
        bind_campaign_triggers(triggers, *profile, *campaign_host);
        // Action 170 (0x49eef0) ends the mission at once: a won mission keeps its campaign variables as
        // character flags (0x47d9d0). 0x49ea20 then goes on by the result 0x49e960 left undecided (-1):
        // a win continues as the end page does, a loss restores the character for the title (+88), and
        // an undecided end (the prologue's cutscene) restores it and still advances (+104, cnxt, +84).
        triggers.mission_over = [this] {
            const auto* side = simulation ? simulation->match().local_side : nullptr;
            if (!campaign_state.active || !profile || !side)
                return;
            if (campaign_state.outcome == -1 && side->outcome)
                campaign_result(side->outcome == 1);
            finish_campaign();
        };
    }
    // +76 (0x49ea00), called when the end page opens, before its OK or Restart.
    void campaign_result(bool won) {
        if (profile)
            campaign_state.result(*profile, won);
    }
    // +80 (0x49ea20). Never replace the scene inside a trigger callback; cnxt's show event
    // (+84, 0x49e130) performs the next load on the front-end frame. Restart (+56) overrides
    // the selected portal with the exact current mission and restores the starting character.
    void finish_campaign(bool restart = false) {
        if (!campaign_state.active || !profile || !campaign_host)
            return;
        const bool proceed = campaign_state.finish(*profile, simulation->triggers(), *campaign_host, restart);
        gameplay_closed = true;
        third_person = false;
        queued_mission.reset();
        if (restart)
            queued_mission = campaign_mission;
        log(std::format("Campaign mission over: result {}, character node {} level {}, page {}",
                        campaign_state.outcome, profile->campaign_node, profile->level,
                        proceed ? "cnxt" : "titl"));
        enter_front_end(FourCC::from_text(proceed ? "cnxt" : "titl"));
    }
    void save_world(const std::string& name) {
        if (!can_save_world())
            throw Error("This game cannot be saved now");
        auto save = WorldSave::capture(root, world_path, *simulation, world_slots,
                                       campaign_state.active ? campaign_state.snapshot : std::nullopt);
        save.setup["camera"] = {WorldJournal::point(camera.position), camera.yaw, camera.pitch};
        // 0x4a7a70: +28 the game type, +60 the session profile, +92 the map path, +24 time(nullptr).
        save.stamp(player_name(), profile ? profile->game_type : std::string("Skirmish"));
        if (menu_thumbnail)
            save.thumbnail(*menu_thumbnail);
        save.write(name);
        log("World saved: " + utf8(WorldSave::path(name)));
    }
    void restore_world(Renderer& renderer) {
        const auto saved = WorldSave::read(load_name);
        if (!saved)
            throw Error("No saved game is available");
        const auto path = saved->mission(root);
        const auto character = saved->character();
        pending_slots = saved->slots();
        campaign_state = {};
        if (character) {
            profile = *character;
            game_type = character->game_type;
            campaign_host.emplace(root, game_type);
            if (!roster)
                roster.emplace(application_data() / "saves/Characters");
            campaign_state.active = true;
            campaign_mission = path;
        }
        match_random.state = saved->setup.at("seed").get<std::uint32_t>();
        load(path, renderer, !saved->setup.contains("campaign_god"));
        auto& match = simulation->match();
        match.world_journal->restore(saved->world);
        if (saved->setup.contains("camera")) {
            const auto& c = saved->setup.at("camera");
            camera.position = {c.at(0).at(0).get<float>(), c.at(0).at(1).get<float>(),
                               c.at(0).at(2).get<float>()};
            camera.yaw = c.at(1).get<float>();
            camera.pitch = c.at(2).get<float>();
        }
        queued_mission.reset();
        next_script_mission.reset();
        script_transition = false;
        end_transition.reset();
        end_screen = 0;
        end_presented = false;
        gameplay_closed = false;
        front_page = {};
        menu_page = {};
        game_menu = false;
        match.paused = false;
        third_person = true;
        keyboard_engaged = false;
        reset_follow(follow);
        driven = nullptr;
        show_inspector = false;
        saves.in_game = false;
        saves.parent = {};
        sync_scene(renderer);
        log("World loaded: " + load_name + " (" + utf8(path) + ")");
    }
    void service_campaign_transition(Renderer& renderer) {
        if (std::exchange(load_world_requested, false)) {
            std::filesystem::path shown;
            try {
                if (const auto saved = WorldSave::read(load_name))
                    shown = saved->mission(root);
            } catch (const std::exception&) {
            }
            queue_load(shown, renderer, [this](Renderer& renderer) {
                try {
                    restore_world(renderer);
                } catch (const std::exception& e) {
                    // 0x4a5e70 'prog': a failed load closes the game and opens FORM:lerr over the title.
                    log("World load failed: " + std::string(e.what()));
                    saves.load_failed();
                }
            });
            return;
        }
        if (script_transition && next_script_mission) {
            const auto mission = *next_script_mission;
            script_transition = false;
            queue_load(mission, renderer, [this, mission](Renderer& renderer) {
                attempt([&] { load(mission, renderer); });
                third_person = true;
                reset_follow(follow);
                exit_blend = {};
            });
            return;
        }
        if (front_page == FourCC::from_text("cnxt")) {
            if (!queued_mission && campaign_host && profile)
                queued_mission = campaign_state.next(*profile, *campaign_host, campaign_mission);
            if (!queued_mission) {
                if (profile && roster)
                    attempt([&] { roster->save(*profile); });
                enter_front_end(FourCC::from_text("titl"));
            }
        }
        if (queued_mission) {
            const auto mission = std::exchange(queued_mission, std::nullopt);
            log("Campaign next: " + utf8(mission->filename()));
            launch_mission(mission, renderer);
        }
    }
    // 0x4a3ee0 then FORM:rost: the game type's characters, created under the port's own data folder.
    void open_roster(std::string type) {
        game_type = std::move(type);
        if (!roster)
            roster.emplace(application_data() / "saves" / "Characters");
        attempt([&] { roster->import(root, game_type); });
        reload_roster(profile && profile->game_type == game_type ? profile->name : std::string());
        enter_front_end(FourCC::from_text("rost"));
    }
    void reload_roster(const std::string& select) {
        characters.clear();
        attempt([&] { characters = roster->list(game_type); });
        // 0x4b1730: the list keeps its selection; a fresh list selects its first row.
        roster_row = characters.empty() ? -1 : 0;
        for (std::size_t i = 0; i < characters.size(); ++i)
            if (characters[i].name == select)
                roster_row = static_cast<int>(i);
    }
    const Character* roster_selection() const {
        return roster_row >= 0 && roster_row < static_cast<int>(characters.size()) ? &characters[roster_row] : nullptr;
    }
    // nchr / echr -> newc (0x4a49d0): the god buffer (random for +112), the wizard list and its cursor.
    void open_character_editor(bool edit) {
        const auto* current = edit ? roster_selection() : nullptr;
        if (edit && !current)
            return;
        edited = current ? std::optional(current->name) : std::nullopt;
        if (!current)
            new_name.clear();
        else
            new_name = current->name;
        const std::uint8_t god = type_god_path() ? static_cast<std::uint8_t>(lobby_random.next() % 5 + 1) : 0;
        new_gods.fill(god);
        wizard_list = roster->wizards();
        if (type_campaign()) { // The campaign's +24 is its WIZARDTYPE list (Eldred for the main one).
            wizard_list.clear();
            for (const auto id : campaign_host->wizards())
                if (const auto choice = std::ranges::find(wizard_choices, id, &WizardChoice::id);
                    choice != wizard_choices.end())
                    wizard_list.push_back(*choice);
        }
        wizard_index = 0;
        custom_path = roster->custom_path_available();
        if (current) {
            for (std::size_t i = 0; i < wizard_list.size(); ++i)
                if (wizard_list[i].id == current->wizard)
                    wizard_index = i;
            new_gods = current->gods;
        }
        enter_front_end(FourCC::from_text("newc"));
    }
    // 0x4a4d20: zero when the name is valid and unused by another character of this game type.
    int character_name_status() const {
        return sacrifice::character_name_status(new_name, characters, edited);
    }
    // _OK_ (0x4a4d60): a new character, or the edited one with the chosen wizard reset to level zero.
    void commit_character() {
        if (character_name_status() != 0 || wizard_list.empty())
            return;
        attempt([&] {
            auto c = edited_character(new_name, game_type, wizard_list[wizard_index].id, type_god_path(),
                                      new_gods, characters, edited);
            save_character_edit(*roster, c, edited);
            if (profile && edited && profile->name == *edited)
                profile = c;
        });
        reload_roster(new_name);
        enter_front_end(FourCC::from_text("rost"));
    }
    // schr (+40): Scenario/Skirmish keep the character for the session and go on to the lobby.
    void select_character() {
        const auto* current = roster_selection();
        if (!current)
            return;
        profile = *current;
        if (type_campaign()) { // 0x49ee00 -> 0x49ee20: the character's current mission.
            queued_mission = campaign_host->start(*profile);
            campaign_finished = !queued_mission;
            return;
        }
        lobby.player_wizard = profile->wizard;
        scan_skirmish_maps();
        bind_lobby(lobby_map);
        enter_front_end(FourCC::from_text("loby"));
    }
    std::string player_name() const { return profile ? profile->name : std::string("Player"); }
    // 0x4a07d0 for 0..6: rand, pers, pyro, jams, stra, char, obsv.
    std::string god_name(unsigned god) {
        static constexpr std::array<const char*, 7> ids{"rand", "pers", "pyro", "jams", "stra", "char", "obsv"};
        return god < ids.size() ? text_record(FourCC::from_text(ids[god])) : std::string();
    }
    const SpellRegistry* load_front_registry() {
        if (!front_registry)
            attempt([&] { front_registry = std::make_shared<const SpellRegistry>(SpellRegistry::load(catalog)); });
        return front_registry.get();
    }
    // 0x44fbf0(level, god, spells only): the god's spell and creature groups, priority / 100 == level.
    std::vector<FourCC> path_spells(unsigned level, unsigned god) {
        std::vector<FourCC> result;
        if (!load_front_registry() || god < 1 || god > 5)
            return result;
        static constexpr std::array<std::array<const char*, 2>, 5> groups{
            {{"Slif", "Clif"}, {"Sfir", "Cfir"}, {"Sear", "Cear"}, {"Sair", "Cair"}, {"Sdea", "Cdea"}}};
        for (const auto* group : groups[god - 1])
            if (const auto* found = front_registry->group(FourCC::from_text(group)))
                for (const auto& definition : found->definitions)
                    if (definition.priority / 100u == level)
                        result.push_back(definition.id); // Runtime +16, shown as its TEXT record.
        return result;
    }
    // Map archives carry a 128x128 PREV image; FORM:MAPi / cgam fall back to TXTR:SHIN.
    ImTextureID lobby_preview(std::size_t index, Renderer& renderer) {
        auto [entry, added] = map_previews.try_emplace(index);
        if (added && index < skirmish_maps.size())
            attempt([&] {
                const auto archive = Archive::open(skirmish_maps[index]);
                const auto record = archive.find(FourCC::from_text("PREV"));
                if (!record)
                    return;
                entry->second = renderer.upload_ui_image(decode_preview(archive.read(*record)));
            });
        return static_cast<ImTextureID>(reinterpret_cast<std::uintptr_t>(entry->second.get()));
    }
    // Native button (0x4ae4a0): FMp2 caps around a repeating FBbg body; the label sits 18 pixels in from
    // each side, vertically centred, white when hovered, 0xc3c3ff enabled and 0x888888 disabled.
    bool form_button(ImDrawList& draw, Renderer& renderer, HudRect r, const std::string& id,
                     const std::string& label, bool enabled, std::uint32_t flags = 0x2000) {
        ImGui::SetCursorScreenPos({r.origin.x, r.origin.y});
        ImGui::BeginDisabled(!enabled);
        const bool clicked = ImGui::InvisibleButton(id.c_str(), {r.size.x, r.size.y});
        const bool hovered = ImGui::IsItemHovered();
        ImGui::EndDisabled();
        menu_button(draw, renderer, r, hovered, enabled);
        auto paint = menu_painter(draw, renderer);
        paint_menu_button_label(paint, r, label, hovered, enabled, flags);
        return clicked && enabled;
    }
    struct LoadedFont {
        NativeFont font;
        std::shared_ptr<void> image;
    };
    std::map<std::string, LoadedFont, std::less<>> native_fonts;
    // FONTMGR select (0x16011950) by face name; atlases load on first use.
    const LoadedFont* native_font(std::string_view name, Renderer& renderer) {
        if (const auto it = native_fonts.find(name); it != native_fonts.end())
            return it->second.image ? &it->second : nullptr;
        auto& entry = native_fonts[std::string(name)];
        attempt([&] {
            const auto* face = find_font_face(name);
            const auto location =
                face ? catalog.find(FourCC::from_text("TXTR"), FourCC{face->texture}) : std::nullopt;
            if (!location)
                throw Error("Missing native font " + std::string(name));
            const auto atlas = load_texture(catalog, *location);
            entry.font = NativeFont::build(*face, atlas);
            entry.image = renderer.upload_ui_image(atlas);
        });
        return entry.image ? &entry : nullptr;
    }
    // 0x4ad6a0: widget flags 0x200/0x400/0x800/0x1000 pick verdana, bookman, elgar or verdanacolor.
    const LoadedFont* widget_font(std::uint32_t flags, std::string_view fallback, Renderer& renderer) {
        return native_font(widget_font_name(flags, fallback), renderer);
    }
    // FONTMGR draw (dword_4DBF84 +16): glyph quads modulated by an opaque 0xRRGGBB colour.
    void native_text(ImDrawList& draw, const LoadedFont& font, HudRect box, std::uint32_t rgb, unsigned alignment,
                     std::string_view text, float scale = 1) {
        const auto texture = static_cast<ImTextureID>(reinterpret_cast<std::uintptr_t>(font.image.get()));
        const ImU32 colour = IM_COL32(rgb >> 16 & 255, rgb >> 8 & 255, rgb & 255, 255);
        font.font.layout(box.origin.x, box.origin.y, box.size.x / scale, box.size.y / scale, alignment, text,
                         [&](const NativeFont::Glyph& g) {
                             const float x = box.origin.x + (g.x - box.origin.x) * scale,
                                         y = box.origin.y + (g.y - box.origin.y) * scale;
                             draw.AddImage(texture, {x, y}, {x + g.width * scale, y + g.height * scale}, {g.u0, g.v0},
                                           {g.u1, g.v1}, colour);
                         });
    }
    bool front_enabled(FourCC page, const std::string& id) const {
        if (const auto enabled = saves.enabled(page, id))
            return *enabled;
        FrontendAvailability available;
        available.selected = roster_selection() != nullptr;
        available.valid_name = character_name_status() == 0;
        available.wizards = !wizard_list.empty();
        available.god_path = type_god_path();
        available.custom_path = custom_path;
        available.can_start = lobby.can_start();
        available.objectives = simulation && !simulation->triggers().objectives.empty();
        available.detailed_options = available.surrender = available.observer = true;
        available.world_load = true; // The title's Load Game always opens FORM:lgam (0x40f6b0).
        return frontend_enabled(page, id, available);
    }
    // FORM:slot icons (0x4a26f0 draws them from the row record).
    FourCC lobby_icon(const std::string& id, int row) const {
        const auto& slot = lobby.slots[static_cast<std::size_t>(row)];
        const bool seated = slot.available && (slot.state == LobbySlot::computer || slot.state == LobbySlot::player);
        if (id == "pnum") {
            if (row == lobby.local)
                return FourCC::from_text("lsll");
            int number = 1;
            for (int i = 0; i < row; ++i)
                number += i != lobby.local;
            return FourCC::from_text(std::format("lsl{}", std::min(number, 4)));
        }
        if (id == "type") {
            if (!slot.available)
                return FourCC::from_text("lslc");
            constexpr std::array<const char*, 4> types{"lslo", "lpla", "lslc", "lplv"};
            return FourCC::from_text(types[slot.state]);
        }
        if (id == "team")
            return FourCC::from_text(seated && slot.team ? std::format("ltm{}", slot.team) : std::string("ltmx"));
        if (id == "redy")
            return FourCC::from_text(seated ? "lthu" : "lthd");
        return {};
    }
    // 0x4b45c0 (kind 7): an FMp2 cap (hover in its right half), the FMp4 box (ticked in its right half),
    // an FBbg body and the mirrored cap; the label starts 36 pixels in, vertically centred, in
    // verdanacolor unless the flags pick a font: 0xc3c3ff when enabled, 0x888888 when not.
    bool form_checkbox(ImDrawList& draw, Renderer& renderer, HudRect r, const std::string& id,
                       const std::string& label, bool enabled, bool checked, std::uint32_t flags) {
        auto paint = menu_painter(draw, renderer);
        return menu_checkbox(paint, r, 1, id.c_str(), label, enabled, checked, flags);
    }

    // FORM:SLID rows (host 0x410550): the name label is the setting's TEXT, the numb label its 0x40cd40
    // display. The kind-4 slider (0x4afcb0, horizontal) draws FMp5 arrows from the top quadrants (the
    // right one mirrored, the right halves while pressed), the FSbg track between them and the FMp5
    // thumb from the bottom-left quadrant at 16 + (width - 48) x position. One arrow step is one unit
    // (0x410550 sets the step to 1 / (max - min)); dragging the track sets the value directly.
    void options_slider(ImDrawList& draw, Renderer& renderer, const MenuForm& form, HudRect bounds, FourCC setting) {
        auto paint = menu_painter(draw, renderer);
        if (menu_options_slider(paint, form, bounds, 1, setting, options,
                                [this](FourCC key) { return text_record(key); }))
            apply_options();
    }

    int text_list(ImDrawList& draw, Renderer& renderer, HudRect r, const MenuForm::Widget& widget, const char* id,
                  std::span<const std::string> rows, int& chosen, int& scroll, bool interactive = true) {
        auto paint = menu_painter(draw, renderer);
        return menu_text_list(paint, 1, r, widget, id, rows, chosen, scroll, interactive);
    }
    // 0x4a3f10 (cinf): "Level: %d" for game types with levels, then "%d) %s" per chosen god on levels 1..9.
    std::vector<std::string> character_info(const Character& c) {
        std::vector<std::string> lines;
        if (type_shows_level())
            lines.push_back(std::format("Level: {}", c.level));
        for (int i = 1; i < 10 && c.gods[static_cast<std::size_t>(i)]; ++i)
            lines.push_back(std::format("{}) {}", i, god_name(c.gods[static_cast<std::size_t>(i)])));
        return lines;
    }
    void wizard_preview(ImDrawList& draw, Renderer& renderer, HudRect r, FourCC wizard) {
        if (!wizard.value)
            return;
        const auto display = ImGui::GetIO().DisplaySize;
        const float scale = std::min(display.x / 640.f, display.y / 480.f);
        if (menu_wizard.update(wizard, ImGui::GetIO().DeltaTime, static_cast<unsigned>(r.size.x * scale),
                               static_cast<unsigned>((r.size.y - 24) * scale))) {
            menu_wizard_image.reset();
            if (!menu_wizard.image().rgba.empty())
                menu_wizard_image = renderer.upload_ui_image(menu_wizard.image());
        }
        if (menu_wizard_image)
            draw.AddImage(static_cast<ImTextureID>(reinterpret_cast<std::uintptr_t>(menu_wizard_image.get())),
                          {r.origin.x, r.origin.y}, {r.origin.x + r.size.x, r.origin.y + r.size.y - 24});
        if (!menu_wizard.error().empty())
            if (const auto* font = native_font("verdana", renderer))
                native_text(draw, *font, r, 0xff8888, 0, menu_wizard.error());
        const auto choice = std::ranges::find(wizard_choices, wizard, &WizardChoice::id);
        if (const auto* font = native_font("verdana", renderer); font && choice != wizard_choices.end())
            native_text(draw, *font, {{r.origin.x, r.origin.y + r.size.y - 20}, {r.size.x, 16}}, 0xffffff, 1,
                        choice->name);
    }
    bool save_widget(ImDrawList& draw, Renderer& renderer, const MenuForm& form, const MenuForm::Widget& widget,
                     HudRect r, FourCC& action) {
        if (save_thumbnail_revision != saves.thumbnail_revision) {
            save_thumbnail_revision = saves.thumbnail_revision;
            save_thumbnail_image = saves.thumbnail ? renderer.upload_ui_image(*saves.thumbnail) : nullptr;
        }
        auto paint = menu_painter(draw, renderer);
        return save_page_widget(paint, draw, 1, saves, form, widget, r,
                                static_cast<ImTextureID>(reinterpret_cast<std::uintptr_t>(save_thumbnail_image.get())),
                                menu_open_frame == ImGui::GetFrameCount() - 1, action);
    }
    // A save page in front of the paused match; a dialog (sav?, del?, nohd) sits over its list page.
    void save_overlay(unsigned width, unsigned height, Renderer& renderer) {
        saves.service();
        if (!SavePages::page(menu_page))
            return;
        const auto& form = native_form(menu_page);
        const auto scale = form_scale(width, height);
        const auto logical_width = static_cast<unsigned>(width / scale),
                   logical_height = static_cast<unsigned>(height / scale);
        ImGui::SetNextWindowPos({0, 0});
        ImGui::SetNextWindowSize({static_cast<float>(width), static_cast<float>(height)});
        ImGui::Begin("##native_save_menu", nullptr, ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove |
                     ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoBackground |
                     ImGuiWindowFlags_NoBringToFrontOnFocus);
        auto& draw = *ImGui::GetWindowDrawList();
        FourCC action;
        int row = -1;
        scaled_page(draw, scale, [&] {
            if (saves.parent.value) {
                const auto& parent = native_form(saves.parent);
                FourCC ignored;
                int ignored_row{};
                ImGui::BeginDisabled();
                front_form(draw, renderer, parent, parent.bounds(logical_width, logical_height), -1, ignored,
                           ignored_row);
                ImGui::EndDisabled();
            }
            front_form(draw, renderer, form, form.bounds(logical_width, logical_height), -1, action, row);
        });
        if (const auto key = menu_keys.action(form, menu_open_frame == ImGui::GetFrameCount()); key.value)
            action = key;
        ImGui::End();
        if (action.value)
            saves.action(menu_page, action);
    }
    int roster_scroll{}, info_scroll{};
    // One FORM page and its subforms; slot rows carry their lobby row index.
    void front_form(ImDrawList& draw, Renderer& renderer, const MenuForm& form, HudRect bounds, int row,
                    FourCC& action, int& action_row) {
        menu_panel(draw, renderer, form, bounds);
        const auto content = form.content(bounds);
        const bool slot_form = form.id == FourCC::from_text("slot");
        const auto* slot = slot_form ? &lobby.slots[static_cast<std::size_t>(row)] : nullptr;
        const bool seated = slot && slot->available &&
                            (slot->state == LobbySlot::computer || slot->state == LobbySlot::player);
        ImGui::PushID(static_cast<int>(form.id.value) ^ row);
        for (const auto& widget : form.widgets) {
            const auto r = form.bounds(widget, content);
            const auto id = widget.id.text();
            if (SavePages::page(form.id) && save_widget(draw, renderer, form, widget, r, action))
                continue;
            if (widget.kind == 2 && form.id == FourCC::from_text("opts") && widget.texture != options_page)
                continue;
            if (widget.kind == 2 && widget.texture == FourCC::from_text("SLID")) {
                ImGui::PushID(id.c_str());
                ImGui::BeginDisabled(!front_enabled(form.id, id));
                options_slider(draw, renderer, native_form(widget.texture), r, widget.id);
                ImGui::EndDisabled();
                ImGui::PopID();
            } else if (widget.kind == 2 && widget.texture.value && widget.texture != FourCC::from_text("slid")) {
                // cgam's slid sliders (sprite counts, limits, damage) are not reconstructed.
                const auto& child = native_form(widget.texture);
                // espb's lvl1..lvl9 rows (LEVL/levl) carry their level, as 0x4a4e40 reads the row id.
                const int child_row = id.starts_with("NP#")   ? id[3] - '1'
                                      : id.starts_with("lvl") ? id[3] - '0'
                                                              : row;
                ImGui::PushID(id.c_str());
                front_form(draw, renderer, child, r, child_row, action, action_row);
                ImGui::PopID();
            } else if (widget.kind == 3) {
                ImTextureID image{};
                if (id == "_map")
                    image = lobby_preview(form.id == FourCC::from_text("cgam") ? browsed_map : lobby_map, renderer);
                if (id == "_map" && !image)
                    hud_sprite(draw, renderer, widget.texture, 0, r);
                else if (slot)
                    image = spell_icon(lobby_icon(id, row), renderer);
                else if (widget.icon.value)
                    image = spell_icon(widget.icon, renderer);
                else if (widget.texture.value)
                    hud_sprite(draw, renderer, widget.texture, 0, r);
                if (image)
                    draw.AddImage(image, {r.origin.x - .5f, r.origin.y - .5f},
                                  {r.origin.x + r.size.x - .5f, r.origin.y + r.size.y - .5f});
                // 0x4a26f0: the type and team icons cycle their row.
                if (slot && slot->available && (id == "type" || (id == "team" && seated))) {
                    ImGui::SetCursorScreenPos({r.origin.x, r.origin.y});
                    if (ImGui::InvisibleButton(id.c_str(), {r.size.x, r.size.y}))
                        action = widget.id, action_row = row;
                }
            } else if (widget.kind == 1) {
                std::string label = text_record(widget.text);
                bool enabled = front_enabled(form.id, id);
                // The port's Scenario entry opens the skirmish lobby over the stock maps, so it says so.
                if (form.id == FourCC::from_text("1ply") && id == "scen")
                    label = "Skirmish";
                if (const auto setting_label = option_menu_label(form.id, widget.id, options))
                    label = text_record(*setting_label);
                if (id == "god_" && (form.id == FourCC::from_text("LEVL") || form.id == FourCC::from_text("levl")) &&
                    row > 0 && row < 10)
                    label = god_name(new_gods[static_cast<std::size_t>(row)]); // 0x4a4e40 'god_'.
                if (slot) {
                    // The local row carries the selected character's name (session +36).
                    if (id == "name") {
                        label = slot->state == LobbySlot::player     ? player_name()
                                : slot->state == LobbySlot::computer ? text_record(slot->name)
                                : slot->state == LobbySlot::closed || !slot->available ? std::string("Closed")
                                                                                       : std::string("Open");
                        enabled = slot->available && slot->state != LobbySlot::closed && row != lobby.local;
                    } else {
                        label = seated ? text_record(FourCC{SkirmishLobby::god_names[slot->god]}) : std::string();
                        enabled = seated;
                    }
                }
                if (form_button(draw, renderer, r, id, label, enabled, widget.flags))
                    action = widget.id, action_row = row;
            } else if (widget.kind == 5 && id == "maps") {
                // A verdana row per stock skirmish map, scrolled by the wheel; a double-click accepts.
                // The native list host's frame and scroll bar are not recovered.
                draw.AddRectFilled({r.origin.x, r.origin.y}, {r.origin.x + r.size.x, r.origin.y + r.size.y},
                                   IM_COL32(0, 0, 0, 120));
                if (const auto* font = widget_font(widget.flags, "verdana", renderer)) {
                    const float row_height = static_cast<float>(font->font.line_height() + 2);
                    const int rows = static_cast<int>((r.size.y - 4) / row_height);
                    ImGui::SetCursorScreenPos({r.origin.x, r.origin.y});
                    ImGui::InvisibleButton("maps", {r.size.x, r.size.y});
                    if (ImGui::IsItemHovered())
                        maps_scroll -= static_cast<int>(ImGui::GetIO().MouseWheel);
                    maps_scroll = std::clamp(maps_scroll, 0,
                                             std::max(0, static_cast<int>(skirmish_maps.size()) - rows));
                    if (ImGui::IsItemClicked()) {
                        const auto hit = maps_scroll +
                            static_cast<int>((ImGui::GetIO().MousePos.y - r.origin.y - 2) / row_height);
                        if (hit >= 0 && hit < static_cast<int>(skirmish_maps.size())) {
                            browsed_map = static_cast<std::size_t>(hit);
                            if (ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left))
                                action = FourCC::from_text("ok__");
                        }
                    }
                    for (int i = 0; i < rows && maps_scroll + i < static_cast<int>(skirmish_maps.size()); ++i) {
                        const auto index = static_cast<std::size_t>(maps_scroll + i);
                        const float top = r.origin.y + 2 + i * row_height;
                        if (index == browsed_map)
                            draw.AddRectFilled({r.origin.x + 2, top}, {r.origin.x + r.size.x - 2, top + row_height},
                                               IM_COL32(70, 70, 150, 170));
                        native_text(draw, *font, {{r.origin.x + 4, top + 2}, {r.size.x - 8, row_height}},
                                    index == browsed_map ? 0xffffff : 0xc3c3ff, 0,
                                    utf8(skirmish_maps[index].stem()));
                    }
                }
            } else if (widget.kind == 5 && id == "rost") {
                std::vector<std::string> names;
                for (const auto& c : characters)
                    names.push_back(c.name);
                if (text_list(draw, renderer, r, widget, "rost", names, roster_row, roster_scroll) == 2)
                    action = FourCC::from_text("schr");
            } else if (widget.kind == 5 && id == "cinf") {
                if (const auto* c = roster_selection()) {
                    int none = -1;
                    text_list(draw, renderer, r, widget, "cinf", character_info(*c), none, info_scroll, false);
                } else
                    draw.AddRectFilled({r.origin.x, r.origin.y}, {r.origin.x + r.size.x, r.origin.y + r.size.y},
                                       IM_COL32(0, 0, 0, 120));
            } else if (widget.kind == 6 && id == "pnam") {
                // The native edit box (kind 6, 0x4b3780 text at +1097); 31 characters as byte_4DC19C allows.
                draw.AddRectFilled({r.origin.x, r.origin.y}, {r.origin.x + r.size.x, r.origin.y + 16},
                                   IM_COL32(0, 0, 0, 160));
                ImGui::SetCursorScreenPos({r.origin.x, r.origin.y});
                ImGui::PushItemWidth(r.size.x);
                ImGui::PushStyleColor(ImGuiCol_FrameBg, IM_COL32(0, 0, 0, 0));
                std::array<char, 32> buffer{};
                std::ranges::copy(new_name.substr(0, 31), buffer.begin());
                if (menu_open_frame == ImGui::GetFrameCount() - 1)
                    ImGui::SetKeyboardFocusHere();
                if (ImGui::InputText("##pnam", buffer.data(), buffer.size()))
                    new_name = buffer.data();
                ImGui::PopStyleColor();
                ImGui::PopItemWidth();
            } else if (widget.kind == 5 && id == "prov") {
                // The 3D provider list (Miles providers natively); the port mixes through one device.
                draw.AddRectFilled({r.origin.x, r.origin.y}, {r.origin.x + r.size.x, r.origin.y + r.size.y},
                                   IM_COL32(0, 0, 0, 120));
                if (const auto* font = native_font("verdana", renderer))
                    native_text(draw, *font, {{r.origin.x + 4, r.origin.y + 3}, {r.size.x - 8, r.size.y - 4}},
                                0x888888, 0, "Default audio device");
            } else if (widget.kind == 5 && id == "GTYP") {
                draw.AddRectFilled({r.origin.x, r.origin.y}, {r.origin.x + r.size.x, r.origin.y + r.size.y},
                                   IM_COL32(0, 0, 0, 120));
                if (const auto* font = native_font("verdana", renderer))
                    native_text(draw, *font, {{r.origin.x + 4, r.origin.y + 3}, {r.size.x - 8, r.size.y - 4}},
                                0xffffff, 0, "Sacrifice");
            } else if (widget.kind == 7) {
                // Options checkboxes are live; cgam's rule toggles stay inert.
                const auto state = form.id == FourCC::from_text("sopt") || form.id == FourCC::from_text("copt") ||
                                   form.id == FourCC::from_text("gopt")
                                       ? options.checked(widget.id)
                                       : std::nullopt;
                if (form_checkbox(draw, renderer, r, id, text_record(widget.text),
                                  state.has_value() && front_enabled(form.id, id),
                                  state.value_or(false), widget.flags))
                    action = widget.id;
            } else if (widget.kind == 10) {
                std::string text;
                if (id == "vers")
                    text = std::format("v{:04}.{:02}.{:02}", 678, 5, 21); // 0x40f6b0 "v%04u.%02u.%02u".
                else if (id == "name" && form.id == FourCC::from_text("MAPi"))
                    text = lobby_map < skirmish_maps.size() ? utf8(skirmish_maps[lobby_map].stem()) : std::string();
                else if (id == "lvl#" && row > 0) // 0x4a4e40 'lvl#': "%d".
                    text = std::to_string(row);
                else if (id.size() == 4 && id.starts_with("spl") && row > 0 && row < 10) {
                    // 'god_' refresh: the level's spells for the row's god, blank past the last one.
                    const auto spells = path_spells(static_cast<unsigned>(row), new_gods[static_cast<std::size_t>(row)]);
                    const auto index = static_cast<std::size_t>(id[3] - '0');
                    text = index < spells.size() ? text_record(spells[index]) : std::string();
                }
                else if (widget.text.value)
                    text = text_record(widget.text);
                if (const auto* font = widget_font(widget.flags, "verdana", renderer))
                    native_text(draw, *font, {r.origin, {r.size.x, std::max(r.size.y, 16.0f)}}, 0xffffff,
                                widget_alignment(widget.flags), text);
            } else if (widget.kind == 12 && id == "swiz") {
                const auto* c = roster_selection();
                const auto wizard = form.id == FourCC::from_text("newc")
                                        ? (wizard_index < wizard_list.size() ? wizard_list[wizard_index].id : FourCC{})
                                        : c ? c->wizard : FourCC{};
                wizard_preview(draw, renderer, r, wizard);
            } else if (widget.kind == 12 && id == "logo") {
                if (const auto& logo = hud_texture(TitleBackdrop::logo_texture(), renderer); logo.image)
                    hud_mesh(draw, title_backdrop.logo(r),
                             static_cast<ImTextureID>(reinterpret_cast<std::uintptr_t>(logo.image.get())));
            } else if (widget.kind == 12) {
                if (catalog.find(FourCC::from_text("TXTR"), widget.id))
                    hud_sprite(draw, renderer, widget.id, 0, r);
            }
        }
        ImGui::PopID();
    }
    // FORM pages are authored for 640x480; 0x4ae060 centres them in physical pixels, which leaves them
    // tiny on modern windows. The port lays a page out on a logical screen of the window's size divided
    // by a uniform scale, then scales the finished vertices and maps the mouse back for hit tests.
    static float form_scale(unsigned width, unsigned height) {
        return std::max(1.0f, std::min(static_cast<float>(width) / 640, static_cast<float>(height) / 480));
    }
    template <class Paint> void scaled_page(ImDrawList& draw, float scale, Paint&& paint) {
        auto& io = ImGui::GetIO();
        const auto mouse = io.MousePos;
        if (ImGui::IsMousePosValid(&mouse))
            io.MousePos = {mouse.x / scale, mouse.y / scale};
        const int first = draw.VtxBuffer.Size;
        paint();
        io.MousePos = mouse;
        for (int i = first; i < draw.VtxBuffer.Size; ++i) {
            draw.VtxBuffer[i].pos.x *= scale;
            draw.VtxBuffer[i].pos.y *= scale;
        }
    }
    void front_end(unsigned width, unsigned height, Renderer& renderer) {
        service_campaign_transition(renderer);
        if (!front_page.value)
            return;
        if (campaign_finished) { // +88 after the last level: the campaign ends at the title.
            campaign_finished = false;
            enter_front_end(FourCC::from_text("titl"));
        }
        const auto& form = native_form(front_page);
        ImGui::SetNextWindowPos({0, 0});
        ImGui::SetNextWindowSize({static_cast<float>(width), static_cast<float>(height)});
        ImGui::Begin("##native_front_end", nullptr, ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove |
                     ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoBackground |
                     ImGuiWindowFlags_NoBringToFrontOnFocus);
        auto& draw = *ImGui::GetWindowDrawList();
        draw.AddRectFilled({0, 0}, {static_cast<float>(width), static_cast<float>(height)}, IM_COL32_BLACK);
        title_backdrop.advance(ImGui::GetIO().DeltaTime);
        if (TitleBackdrop::shows_tunnel(front_page))
            if (const auto& tunnel = hud_texture(TitleBackdrop::tunnel_texture(), renderer); tunnel.image)
                hud_mesh(draw, title_backdrop.tunnel(static_cast<float>(width), static_cast<float>(height)),
                         static_cast<ImTextureID>(reinterpret_cast<std::uintptr_t>(tunnel.image.get())));
        const auto scale = form_scale(width, height);
        const auto logical_width = static_cast<unsigned>(width / scale),
                   logical_height = static_cast<unsigned>(height / scale);
        FourCC action;
        int row = -1;
        scaled_page(draw, scale, [&] {
            // Dialog pages (1ply, cgam, qgam) sit over the title page as in the native FORM stack.
            const auto& title = native_form(FourCC::from_text("titl"));
            if (SavePages::page(form.id) && saves.parent.value) {
                const auto& parent = native_form(saves.parent);
                FourCC ignored;
                int ignored_row{};
                ImGui::BeginDisabled();
                front_form(draw, renderer, parent, parent.bounds(logical_width, logical_height), -1, ignored,
                           ignored_row);
                ImGui::EndDisabled();
            } else if (form.id != title.id && !(form.flags & 0x40)) {
                FourCC ignored;
                int ignored_row{};
                const auto page = front_page;
                front_page = title.id;
                ImGui::BeginDisabled();
                panel_backdrop = true;
                front_form(draw, renderer, title, title.bounds(logical_width, logical_height), -1, ignored,
                           ignored_row);
                panel_backdrop = false;
                ImGui::EndDisabled();
                front_page = page;
                draw.AddRectFilled({0, 0}, {static_cast<float>(logical_width), static_cast<float>(logical_height)},
                                   IM_COL32(0, 0, 0, 120));
            }
            front_form(draw, renderer, form, form.bounds(logical_width, logical_height), -1, action, row);
        });
        if (const auto key = menu_keys.action(form, menu_open_frame == ImGui::GetFrameCount());
            key.value && front_enabled(front_page, key.text()))
            action = key;
        if (!world_notice.empty()) {
            ImGui::SetCursorScreenPos({16.f, static_cast<float>(height) - 36});
            ImGui::TextUnformatted(world_notice.c_str());
        }
        ImGui::End();
        if (action.value)
            front_action(action, row, renderer);
    }
    void front_action(FourCC id, int row, Renderer& renderer) {
        const auto command = id.text(), page = front_page.text();
        if (SavePages::page(front_page)) {
            saves.action(front_page, id);
            return;
        }
        if (command == "lgam" && page == "titl") { // 0x40f6b0: Load Game opens FORM:lgam.
            saves.open(FourCC::from_text("lgam"), false);
            return;
        }
        const auto open = [&](const char* next) { enter_front_end(FourCC::from_text(next)); };
        const auto route = frontend_route(front_page, id, row);
        if (route.action != FrontendAction::unhandled) {
            if (!front_enabled(front_page, command))
                return;
            if (apply_lobby_action(route.action, row, lobby, lobby_random))
                return;
            switch (route.action) {
            case FrontendAction::open:
                enter_front_end(route.destination);
                break;
            case FrontendAction::campaign:
                open_campaign();
                break;
            case FrontendAction::skirmish:
                open_roster("Skirmish");
                break;
            case FrontendAction::new_character:
                open_character_editor(false);
                break;
            case FrontendAction::edit_character:
                open_character_editor(true);
                break;
            case FrontendAction::select_character:
                select_character();
                break;
            case FrontendAction::save_character:
                commit_character();
                break;
            case FrontendAction::cancel_character:
                edited.reset();
                enter_front_end(route.destination);
                break;
            case FrontendAction::delete_character:
                if (const auto* c = roster_selection()) {
                    attempt([&] { roster->remove(*c); });
                    if (profile && profile->name == c->name && profile->game_type == c->game_type)
                        profile.reset();
                    reload_roster({});
                }
                enter_front_end(route.destination);
                break;
            case FrontendAction::wizard_next:
                if (!wizard_list.empty())
                    wizard_index = (wizard_index + 1) % wizard_list.size();
                break;
            case FrontendAction::wizard_previous:
                if (!wizard_list.empty())
                    wizard_index = (wizard_index + wizard_list.size() - 1) % wizard_list.size();
                break;
            case FrontendAction::god_path:
                new_gods[row] = new_gods[row] % 5 + 1;
                break;
            case FrontendAction::browse_map:
                browsed_map = lobby_map;
                enter_front_end(route.destination);
                break;
            case FrontendAction::accept_map:
                if (browsed_map != lobby_map)
                    bind_lobby(browsed_map);
                enter_front_end(route.destination);
                break;
            case FrontendAction::quit:
                quit_requested = true;
                break;
            case FrontendAction::start_match:
                if (lobby.can_start() && lobby_map < skirmish_maps.size()) {
                    pending_slots = lobby.session();
                    // A skirmish is never a campaign mission: an earlier campaign session in this run would
                    // otherwise make load() replace the lobby's wizard with the campaign's Eldred (cwe2).
                    campaign_state = {};
                    front_page = {};
                    queue_load(skirmish_maps[lobby_map], renderer, [this, path = skirmish_maps[lobby_map]](Renderer& renderer) {
                        attempt([&] { load(path, renderer); });
                        pending_slots.reset();
                        third_person = true;
                        show_inspector = false;
                    });
                }
                break;
            default:
                break;
            }
            return;
        }
        if (page == "opts") { // 0x40d080 and the page handlers 0x40e820 / 0x40eb10.
            if (!front_enabled(front_page, command))
                return;
            if (option_menu_command(options, options_page, id))
                apply_options();
            if (command == "menu") {
                if (options_return.value) { // Back to the pause menu over the paused match.
                    front_page = {};
                    menu_page = options_return;
                    game_menu = true;
                    menu_open_frame = ImGui::GetFrameCount();
                } else
                    open("titl");
            }
        }
    }
    // Native FORM:gmnu / FORM:winr / FORM:losn replace the stand-in ImGui windows.
    void pause_overlay(unsigned width, unsigned height, Renderer& renderer) {
        if (SavePages::page(menu_page)) {
            save_overlay(width, height, renderer);
            return;
        }
        auto& match = simulation->match();
        if (menu_page.value) {
            auto form = native_form(menu_page);
            if (campaign_state.active && form.id == FourCC::from_text("gmnu")) {
                // Port shortcut to the native defeat page's +56 command, using its stock TEXT.
                auto restart = native_form(FourCC::from_text("lose")).widgets.at(1);
                restart.rectangle = {{24, 104}, {160, 0}};
                for (auto& widget : form.widgets)
                    if (widget.rectangle.origin.y >= 104)
                        widget.rectangle.origin.y += 24;
                form.rectangle.size.y += 24;
                form.widgets.push_back(restart);
            }
            const bool choice_form = form.id == FourCC::from_text("chos") && !simulation->triggers().choices.empty();
            if (choice_form) {
                const auto& choice = simulation->triggers().choices.front();
                for (auto& widget : form.widgets)
                    widget.text = widget.id == FourCC::from_text("ask!") ? choice.question :
                        widget.id == FourCC::from_text("cho1") ? choice.first : choice.second;
            }
            const auto form_text = [&](FourCC id) -> std::string {
                return choice_form && simulation->triggers().text ? simulation->triggers().text(id) : text_record(id);
            };
            const auto scale = form_scale(width, height);
            const auto bounds = form.bounds(static_cast<unsigned>(width / scale), static_cast<unsigned>(height / scale)),
                       content = form.content(bounds);
            ImGui::SetNextWindowPos({0, 0});
            ImGui::SetNextWindowSize({static_cast<float>(width), static_cast<float>(height)});
            ImGui::Begin("##native_game_menu", nullptr, ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove |
                         ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoBackground |
                         ImGuiWindowFlags_NoBringToFrontOnFocus);
            auto& draw = *ImGui::GetWindowDrawList();
            FourCC action;
            scaled_page(draw, scale, [&] {
                menu_panel(draw, renderer, form, bounds);
                for (const auto& widget : form.widgets) {
                    // 0x45093b / 0x450be8: these occupy the same row and are mutually exclusive.
                    // A playing side gets Surrender; the native observer gets Finish Match.
                    if (form.id == FourCC::from_text("gmnu") &&
                        ((widget.id == FourCC::from_text("exit") && !player->side().outcome) ||
                         (widget.id == FourCC::from_text("surr") && player->side().outcome)))
                        continue;
                    const auto rectangle = form.bounds(widget, content);
                    if (form.id == FourCC::from_text("summ") &&
                        ((widget.id == FourCC::from_text("prev") && summary_mode == 0) ||
                         (widget.id == FourCC::from_text("next") && summary_mode == 2)))
                        continue;
                    if (form.id == FourCC::from_text("summ") && widget.id == FourCC::from_text("smod")) {
                        if (const auto* font = widget_font(widget.flags, "verdana", renderer))
                            native_text(draw, *font, {rectangle.origin, {rectangle.size.x, 16}}, 0xffffff,
                                        widget_alignment(widget.flags),
                                        text_record(FourCC::from_text(std::format("btn{}", summary_mode + 1))));
                        continue;
                    }
                    if (widget.kind == 3 && widget.texture.value) {
                        hud_sprite(draw, renderer, widget.texture, 0, rectangle);
                    } else if (widget.kind == 3 && widget.icon.value) {
                        if (const auto image = spell_icon(widget.icon, renderer))
                            draw.AddImage(image, {rectangle.origin.x - .5f, rectangle.origin.y - .5f},
                                          {rectangle.origin.x + rectangle.size.x - .5f,
                                           rectangle.origin.y + rectangle.size.y - .5f});
                    } else if (widget.kind == 1) {
                        // summ's untitled arrows take the prev/next TEXT.
                        const auto label = form_text(widget.text.value ? widget.text : widget.help);
                        if (form_button(draw, renderer, rectangle, widget.id.text(), label,
                                        menu_action_enabled(widget.id), widget.flags))
                            action = widget.id;
                    } else if (widget.kind == 10 && form.id == FourCC::from_text("hist")) {
                        const auto* entry = history_entry();
                        const auto* font = widget_font(widget.flags, "verdana", renderer);
                        if (!entry || !font)
                            continue;
                        if (widget.id == FourCC::from_text("spkr")) {
                            native_text(draw, *font, rectangle, 0xffffff, widget_alignment(widget.flags), entry->title);
                        } else if (widget.id == FourCC::from_text("spch")) {
                            std::vector<std::string_view> rows;
                            std::string_view rest = entry->caption;
                            while (!rest.empty()) {
                                const auto line = font->font.line(rest, static_cast<int>(rectangle.size.x));
                                if (line.skip <= 0) break;
                                rows.push_back(rest.substr(0, static_cast<std::size_t>(std::max(line.count, 0))));
                                rest.remove_prefix(std::min(rest.size(), static_cast<std::size_t>(line.skip)));
                            }
                            const auto height = font->font.line_height();
                            const auto visible = std::max(1, static_cast<int>(rectangle.size.y) / height);
                            const auto last = std::max(0, static_cast<int>(rows.size()) - visible);
                            if (ImGui::IsMouseHoveringRect({rectangle.origin.x, rectangle.origin.y},
                                {rectangle.origin.x + rectangle.size.x, rectangle.origin.y + rectangle.size.y}))
                                history_scroll -= static_cast<int>(ImGui::GetIO().MouseWheel * 3);
                            if (ImGui::IsKeyPressed(ImGuiKey_UpArrow)) --history_scroll;
                            if (ImGui::IsKeyPressed(ImGuiKey_DownArrow)) ++history_scroll;
                            history_scroll = std::clamp(history_scroll, 0, last);
                            for (int row = 0; row < visible && history_scroll + row < static_cast<int>(rows.size()); ++row)
                                native_text(draw, *font,
                                    {{rectangle.origin.x, rectangle.origin.y + row * height},
                                     {rectangle.size.x, static_cast<float>(height)}},
                                    0xffffff, widget_alignment(widget.flags), rows[history_scroll + row]);
                        }
                    } else if (form.id == FourCC::from_text("tipb") && widget.id == FourCC::from_text("tipl")) {
                        if (const auto* font = widget_font(widget.flags, "verdana", renderer))
                            native_text(draw, *font, rectangle, 0xffffff, widget_alignment(widget.flags),
                                        simulation->triggers().text(tip_text));
                    } else if (widget.kind == 7 && form.id == FourCC::from_text("tipb")) {
                        if (form_checkbox(draw, renderer, rectangle, widget.id.text(), text_record(widget.text), true,
                                          options.value(FourCC::from_text("tipo")) != 0, widget.flags)) action = widget.id;
                    } else if (widget.kind == 10 && form.id == FourCC::from_text("mobj")) {
                        // 0x47e2e0 packs the visible objectives into obj1..obj6, prefixing each with "- ".
                        const auto id = widget.id.text();
                        if (id.size() != 4 || !id.starts_with("obj") || id[3] < '1' || id[3] > '6')
                            continue;
                        unsigned row = 0;
                        for (const auto& objective : simulation->triggers().objectives) {
                            if (!(objective.flags & 1))
                                continue;
                            if (row++ != static_cast<unsigned>(id[3] - '1'))
                                continue;
                            if (const auto* font = widget_font(widget.flags, "verdana", renderer))
                                native_text(draw, *font, rectangle, objective.flags & 2 ? 0x888888 : 0xffffff,
                                            widget_alignment(widget.flags), "- " + objective.caption);
                            break;
                        }
                    } else if (widget.kind == 10 && widget.text.value) {
                        if (const auto* font = widget_font(widget.flags, "verdana", renderer))
                            native_text(draw, *font,
                                        {rectangle.origin, {rectangle.size.x, std::max(rectangle.size.y, 16.0f)}},
                                        0xffffff, widget_alignment(widget.flags), form_text(widget.text));
                    }
                }
                if (form.id == FourCC::from_text("summ"))
                    summary_table(draw, renderer, match, content);
            });
            if (const auto key = menu_keys.action(form, menu_open_frame == ImGui::GetFrameCount()); key.value)
                action = key;
            if (end_transition.action.value)
                ImGui::GetForegroundDrawList()->AddRectFilled(
                    {0, 0}, {static_cast<float>(width), static_cast<float>(height)},
                    IM_COL32(0, 0, 0, static_cast<int>(end_transition.alpha * 255)));
            if (!world_notice.empty()) {
                ImGui::SetCursorScreenPos({16.f, static_cast<float>(height) - 36});
                ImGui::TextUnformatted(world_notice.c_str());
            }
            ImGui::End();
            if (action.value)
                native_menu_action(action);
        } else if (match.paused) {
            const char* label = "Paused";
            const auto size = ImGui::CalcTextSize(label);
            ImGui::GetForegroundDrawList()->AddText(ImVec2{(width - size.x) * .5f, height * .3f},
                                                    IM_COL32(255, 255, 255, 240), label);
        }
    }
    // 0x4710e0 builds the table once: column headers sm10..sm40 (bookman, flags 0x42400) at x 224 + 76 i,
    // y 56; per listed side n (rows step 16 from y 96, skipped sides keep their row) a name sm0n (elgar,
    // 0x12800) from x 8 to 224 and values smin (bookman, 0x12400). 0x471400 fills them for the page:
    // headers from mcm%d / mso%d / mst%d (missing records are blank), values from the side statistics,
    // and only sides with a wizard (+168) get a row.
    void summary_table(ImDrawList& draw, Renderer& renderer, const Match& match, HudRect content) {
        static constexpr std::array<const char*, 3> headers{"mcm", "mso", "mst"};
        const auto* bookman = widget_font(0x42400, "bookman", renderer);
        const auto* elgar = widget_font(0x12800, "elgar", renderer);
        if (!bookman || !elgar)
            return;
        const auto cell = [&](const LoadedFont& font, float x, float y, float w, const std::string& text) {
            native_text(draw, font,
                        {{content.origin.x + x, content.origin.y + y}, {w, static_cast<float>(font.font.line_height())}},
                        0xffffff, widget_alignment(0x2000), text);
        };
        for (int column = 1; column <= 4; ++column) {
            const auto id = FourCC::from_text(std::format("{}{}", headers[summary_mode], column));
            if (catalog.find(FourCC::from_text("TEXT"), id))
                cell(*bookman, 224.0f + 76 * (column - 1), 56, 76, text_record(id));
        }
        int computer = 0;
        for (std::size_t row = 0; row < match.side_order.size(); ++row) {
            const auto& side = *match.side_order[row];
            if (side.objects[1].empty())
                continue;
            const float y = 96.0f + 16 * static_cast<float>(row);
            // Native +36 is the session's player name: the local character's; computers stay labelled.
            cell(*elgar, 8, y, 216,
                 &side == match.local_side ? player_name() : std::format("Computer {}", ++computer));
            const std::array<std::array<int, 4>, 3> values{{
                {side.peak_kills, side.banishments, side.peak_level, -1},
                {side.summoned_creatures, side.souls_harvested, side.soul_count, -1},
                {side.manaliths_created, side.shrines_created, side.manaliths_destroyed, side.shrines_destroyed},
            }};
            for (int column = 0; column < 4; ++column)
                if (const auto value = values[summary_mode][column]; value >= 0)
                    cell(*bookman, 224.0f + 76 * column, y, 76, std::to_string(value));
        }
    }
    // TEXT records by id (RESOURCEMGR::GetRecord(0, 'TEXT', id)), cached.
    const std::string& text_record(FourCC id) {
        auto [entry, added] = text_cache.try_emplace(id.value);
        if (added) {
            if (const auto port = port_menu_text(id))
                entry->second = *port;
            else if (const auto location = catalog.find(FourCC::from_text("TEXT"), id)) {
                const auto bytes = catalog.read(*location);
                entry->second.assign(reinterpret_cast<const char*>(bytes.data()), bytes.size());
                while (!entry->second.empty() && entry->second.back() == '\0')
                    entry->second.pop_back();
            } else
                entry->second = id.text();
        }
        return entry->second;
    }
    const HudTexture& hud_texture(FourCC id, Renderer& renderer) {
        auto [entry, added] = hud_textures.try_emplace(id.value);
        if (added)
            attempt([&] {
                const auto source = catalog.find(FourCC::from_text("TXTR"), id);
                if (!source)
                    throw Error("Missing HUD texture " + id.text());
                entry->second.header = decode_texture_header(catalog.read(*source));
                entry->second.image = renderer.upload_ui_image(load_texture(catalog, *source));
            });
        return entry->second;
    }
    void hud_sprite(ImDrawList& draw, Renderer& renderer, FourCC id, unsigned frame, HudRect rectangle,
                    ImU32 tint = IM_COL32_WHITE, bool additive = false, float angle = 0) {
        auto paint = hud_painter(renderer, draw);
        paint.sprite(id, frame, rectangle, tint, additive, angle);
    }
    void hud_mesh(ImDrawList& draw, const Mesh& geometry, ImTextureID texture = {}) {
        draw_hud_mesh(draw, geometry, texture);
    }
    // 0x45a0a0 / 0x459eb0, descriptors 0x4cfa24 / 0x4cfa3c: two mirrored frame halves,
    // with a 24x80 GBAR inset in each 32x96 frame at the right edge of the main viewport.
    void status_meters(unsigned width, unsigned height, Renderer& renderer) {
        const auto* wizard = player ? player->wizard() : nullptr;
        if (!wizard)
            return;
        if (!status_assets_loaded) {
            status_assets_loaded = true;
            attempt([&] {
                for (unsigned i = 0; i < 2; ++i) {
                    const auto source = catalog.find(FourCC::from_text("GBAR"),
                                                     FourCC::from_text(i ? "mbar" : "hbar"));
                    if (!source)
                        throw Error("Missing native HUD gradient bar");
                    status_styles[i] = GradientBar::decode(catalog.read(*source));
                }
                status_frame = hud_texture(FourCC::from_text("fram"), renderer).image;
            });
        }
        if (!status_frame)
            return;
        auto paint = hud_painter(renderer, *ImGui::GetBackgroundDrawList());
        auto numbers = hud_painter(renderer, *ImGui::GetBackgroundDrawList(), "bookman");
        auto tooltips = hud_painter(renderer, *ImGui::GetForegroundDrawList());
        paint_status_meters(paint, numbers, tooltips, {width, height, pointer_position}, *wizard,
                             interface_locks(), status_styles, !ImGui::GetIO().WantCaptureMouse,
                             [this](FourCC id) { return text_record(id); });
    }
    // ICON uses the indexed TXTR layout and a palette in its own resource scope. Keep the GPU views
    // separate from the inspector image, and cache missing icons too (internal definitions can lack one).
    ImTextureID spell_icon(FourCC id, Renderer& renderer) {
        auto [entry, added] = spell_icons.try_emplace(id.value);
        if (added && id.value)
            attempt([&] {
                if (const auto location = catalog.find(FourCC::from_text("ICON"), id)) {
                    const auto header = decode_texture_header(catalog.read(*location));
                    icon_headers.emplace(id, header);
                    entry->second = renderer.upload_ui_image(
                        texture_frame(load_texture(catalog, *location), header, 0));
                }
            });
        return static_cast<ImTextureID>(reinterpret_cast<std::uintptr_t>(entry->second.get()));
    }
    void native_tooltip(Renderer& renderer, const std::vector<std::pair<std::string, std::uint32_t>>& lines) {
        auto paint = hud_painter(renderer, *ImGui::GetForegroundDrawList());
        HudLines rgba;
        for (const auto& [text, rgb] : lines)
            rgba.emplace_back(text, IM_COL32(rgb >> 16 & 255, rgb >> 8 & 255, rgb & 255, 255));
        const auto& io = ImGui::GetIO();
        paint_tooltip(paint, {static_cast<unsigned>(io.DisplaySize.x), static_cast<unsigned>(io.DisplaySize.y),
                              {io.MousePos.x, io.MousePos.y}}, rgba);
    }
    std::vector<std::size_t> visible_spells(const CreatureActor& wizard) const {
        return visible_spell_indices(wizard, static_cast<unsigned>(player->spell_tab));
    }
    std::optional<SpellBarLayout::Hit> spell_bar_hit(Vec2 point, unsigned width, unsigned height) const {
        const auto* wizard = player ? player->wizard() : nullptr;
        if (!wizard || view != View::Terrain || (interface_locks() & 2))
            return {};
        return SpellBarLayout::make(width, height).hit(point, static_cast<unsigned>(visible_spells(*wizard).size()));
    }
    void spell_bar(unsigned width, unsigned height, Renderer& renderer) {
        auto* wizard = player ? player->wizard() : nullptr;
        if (!wizard || !simulation || !simulation->match().spell_registry)
            return;
        const bool changed = displayed_book != player->spell_tab || book_wizard != wizard;
        displayed_book = player->spell_tab;
        book_wizard = wizard;
        auto paint = hud_painter(renderer, *ImGui::GetBackgroundDrawList());
        auto tooltips = hud_painter(renderer, *ImGui::GetForegroundDrawList());
        const auto& io = ImGui::GetIO();
        paint_spell_bar(paint, tooltips, {width, height, {io.MousePos.x, io.MousePos.y}}, *wizard,
                         *simulation->match().spell_registry, static_cast<unsigned>(player->spell_tab),
                         changed, !io.WantCaptureMouse, [this](FourCC id) { return text_record(id); });
    }
    CreatureActor* character(unsigned ordinal) {
        for (auto& scene_model : scene.models)
            if (scene_model.character && ordinal-- == 0)
                return scene_model.character->object;
        return nullptr;
    }
    void tick_model(float dt, Renderer& renderer) {
        if (view == View::Model && saxs) {
            if (!saxs_animation || !play_model)
                return;
            attempt([&] {
                // Preview at the SKEL-declared frames per second with SAXSANIM::Animate's
                // loop-trigger/restart rule; simulation timing is not reconstructed yet.
                const auto count = static_cast<unsigned>(saxs_animation->frames.size());
                if (saxs_sequence)
                    saxs_frame = saxs_sequence->advance(saxs_frame, dt * saxs_sequence->framerate, count);
                else
                    saxs_frame = std::fmod(saxs_frame + dt * preview_rate, static_cast<float>(count));
                saxs_frame = std::min(saxs_frame, static_cast<float>(count - 1));
                apply_saxs_pose(renderer);
            });
            if (!error.empty())
                play_model = false;
            return;
        }
        if (view == View::Model && model && play_model && model->geometry.frames.size() > 1) {
            attempt([&] {
                model_frame = std::fmod(model_frame + dt * preview_rate,
                                        static_cast<float>(model->geometry.frames.size() - 1));
                apply_model_pose(renderer);
            });
            if (!error.empty())
                play_model = false;
        }
    }
    void apply_debug_wizard_profile(CreatureActor& actor, int god, int level) {
        auto& match = simulation->match();
        configure_debug_wizard(match, actor, god, level);
        if (player && player->wizard() == &actor)
            player->cancel_target();
        displayed_book = -1;
        order_spell = {};
    }
    void ui(Renderer& renderer) {
        if (view == View::Terrain && player && player->wizard())
            ImGui::SetNextWindowSizeConstraints({200, 100},
                {16384, std::max(100.0f, SpellBarLayout::make(static_cast<unsigned>(ImGui::GetIO().DisplaySize.x),
                    static_cast<unsigned>(ImGui::GetIO().DisplaySize.y)).top - 40)});
        ImGui::SetNextWindowPos({12, 12}, ImGuiCond_FirstUseEver);
        ImGui::SetNextWindowSize({380, 720}, ImGuiCond_FirstUseEver);
        ImGui::Begin("SacrificeModern inspector");
        // The Graphics page's Remastered Look option (drawing only; applied from the next frame).
        ImGui::BeginDisabled(classic_look);
        if (bool look = options.value(FourCC::from_text("rmst")) != 0 && !classic_look;
            ImGui::Checkbox("Remastered Look", &look))
            options.set(FourCC::from_text("rmst"), look);
        ImGui::EndDisabled();
        if (classic_look)
            ImGui::SetItemTooltip("Off because of --classic-look.");
        ImGui::Checkbox("Frame timing", &frame_timing);
        ImGui::SetItemTooltip("Milliseconds per frame on the CPU and GPU, by part of the scene.");
        ImGui::SeparatorText("Wizard shortcuts");
        auto* debug_wizard = view == View::Terrain && player ? player->wizard() : nullptr;
        if (!debug_wizard && view == View::Terrain && simulation && simulation->match().local_side)
            debug_wizard = dynamic_cast<CreatureActor*>(simulation->match().local_side->wizard());
        ImGui::BeginDisabled(!debug_wizard);
        ImGui::Combo("Wizard god", &profile_god, "None\0Persephone\0Pyro\0James\0Stratos\0Charnel\0");
        if (ImGui::Button("Max level + apply god"))
            attempt([&] { apply_debug_wizard_profile(*debug_wizard, profile_god, 14); });
        ImGui::SameLine();
        if (ImGui::Button("Apply god"))
            attempt([&] { apply_debug_wizard_profile(*debug_wizard, profile_god,
                                                    debug_wizard->progression.level); });
        ImGui::EndDisabled();
        if (debug_wizard)
            ImGui::TextWrapped("Current level: %d. Applies god spells and refills health/mana.",
                               debug_wizard->progression.level);
        else
            ImGui::TextDisabled("Load a map with a player wizard to use these shortcuts.");
        ImGui::BeginDisabled(!debug_wizard);
        ImGui::InputInt("Souls", &debug_souls);
        ImGui::SameLine();
        if (ImGui::Button("Grant"))
            attempt([&] { debug_wizard->change_souls(debug_souls); });
        if (debug_wizard)
            ImGui::Text("Souls held: %d", debug_wizard->soul_count);
        ImGui::EndDisabled();
        ImGui::SeparatorText("Weather");
        {
            const bool live = view == View::Terrain && simulation;
            ImGui::BeginDisabled(!live);
            // Kinds follow the ATMOS table: rain, snow, hail, embers, ash, swirl.
            static constexpr const char* weather_names[]{"Rain", "Snow", "Hail", "Embers", "Ash", "Swirl"};
            int choice = debug_weather - 1;
            if (ImGui::Combo("Kind", &choice, weather_names, 6))
                debug_weather = choice + 1;
            ImGui::SliderFloat("Strength", &debug_weather_strength, 0, 1);
            ImGui::SliderFloat("Fade (s)", &debug_weather_fade, 0, 30);
            if (ImGui::Button("Set weather"))
                attempt([&] { simulation->match().weather.set(static_cast<unsigned>(debug_weather),
                                                              debug_weather_strength, debug_weather_fade); });
            ImGui::SameLine();
            if (ImGui::Button("Clear all"))
                attempt([&] {
                    auto& weather = simulation->match().weather;
                    for (unsigned kind = 1; kind <= 6; ++kind)
                        if (weather.effects[kind - 1].intensity > 0 || weather.effects[kind - 1].target > 0)
                            weather.set(kind, 0, debug_weather_fade);
                });
            if (live) {
                const auto& effects = simulation->match().weather.effects;
                std::string active;
                for (unsigned n = 0; n < 6; ++n)
                    if (effects[n].intensity > 0.005f)
                        active += std::format("{}{} {:.0f}%", active.empty() ? "" : ", ", weather_names[n],
                                              effects[n].intensity * 100);
                ImGui::TextWrapped("Active: %s", active.empty() ? "none" : active.c_str());
            }
            ImGui::EndDisabled();
        }
        ImGui::Separator();
        ImGui::TextUnformatted("C++20 / x64 / Direct3D 11");
        ImGui::TextUnformatted("F10: show/hide inspector");
        ImGui::TextWrapped(
            "Map simulation: movement, melee and converted spells. Campaign and AI are in progress.");
        ImGui::TextWrapped("%s", status.c_str());
        ImGui::Separator();
        ImGui::Checkbox("Wireframe", &wireframe);
        ImGui::SameLine();
        if (bool vsync = options.value(FourCC::from_text("tbuf")) != 0; ImGui::Checkbox("VSync", &vsync))
            options.set(FourCC::from_text("tbuf"), vsync); // The Graphics page's VSync checkbox.
        ImGui::BeginDisabled(view != View::Terrain);
        if (ImGui::Checkbox("Minimap projection (diagnostic)", &preview)) {
            attempt([&] {
                if (preview && !map_preview.rgba.empty()) {
                    displayed_image = map_preview;
                    renderer.set_image(displayed_image);
                } else
                    renderer.clear_image();
                renderer.set_mesh(terrain.mesh(!preview));
                if (!preview)
                    renderer.set_terrain_lod(terrain);
            });
        }
        ImGui::Checkbox("Map entities", &show_entities);
        ImGui::SameLine();
        ImGui::Checkbox("Environment", &show_environment);
        if (ImGui::Checkbox("Ambient sound", &scene_sound)) {
            if (scene_sound)
                live_audio = true;
            apply_options();
        }
        ImGui::SameLine();
        ImGui::Checkbox("Music", &music_on);
        if (ImGui::SliderFloat("Ambient volume", &scene_volume, 0, 1, "%.2f"))
            apply_options();
        if (!audio_error.empty())
            ImGui::TextWrapped("Ambient audio: %s", audio_error.c_str());
        if (!scene_error.empty())
            ImGui::TextWrapped("Entities: %s", scene_error.c_str());
        if (!scene.unresolved.empty() && ImGui::TreeNode("Unresolved entity IDs")) {
            for (const auto& text : scene.unresolved)
                ImGui::BulletText("%s", text.c_str());
            ImGui::TreePop();
        }
        ImGui::EndDisabled();
        if (view == View::Model && saxs) {
            const auto current = saxs_clip < 0 ? std::string("Bind pose") : saxs->clips[saxs_clip].name;
            if (ImGui::BeginCombo("Clip", current.c_str())) {
                if (ImGui::Selectable("Bind pose", saxs_clip < 0))
                    attempt([&] { select_clip(-1, renderer); });
                for (int i = 0; i < static_cast<int>(saxs->clips.size()); ++i) {
                    ImGui::PushID(i);
                    if (ImGui::Selectable(saxs->clips[i].name.c_str(), saxs_clip == i))
                        attempt([&] { select_clip(i, renderer); });
                    ImGui::PopID();
                }
                ImGui::EndCombo();
            }
            ImGui::BeginDisabled(!saxs_animation);
            const auto last = saxs_animation ? static_cast<float>(saxs_animation->frames.size() - 1) : 0.0f;
            if (ImGui::SliderFloat("Animation frame", &saxs_frame, 0, last, "%.0f"))
                attempt([&] { apply_saxs_pose(renderer); });
            ImGui::Checkbox("Play clip", &play_model);
            if (saxs_sequence)
                ImGui::Text("SKEL rate %d frames/s, %zu triggers", saxs_sequence->framerate,
                            saxs_sequence->triggers.size());
            else
                ImGui::SliderFloat("Preview frames/sec", &preview_rate, 0.1f, 60, "%.1f");
            ImGui::EndDisabled();
            if (ImGui::SliderFloat("Detail factor", &saxs_detail, 1, 100, "%.2f",
                                   ImGuiSliderFlags_Logarithmic))
                attempt([&] { apply_saxs_pose(renderer, true); });
            ImGui::Text("%zu render vertices, %zu triangles", saxs_layout.vertices.size(),
                        saxs_layout.triangles.size());
        }
        if (view == View::Model && model) {
            const auto& geometry = model->geometry;
            bool changed = false;
            ImGui::BeginDisabled(geometry.frames.size() < 2);
            changed |= ImGui::SliderFloat("Geometry frame", &model_frame, 0,
                                          static_cast<float>(geometry.frames.size() - 1), "%.2f");
            ImGui::Checkbox("Play frame preview", &play_model);
            ImGui::SliderFloat("Preview frames/sec", &preview_rate, 0.1f, 30, "%.1f");
            ImGui::EndDisabled();
            changed |= ImGui::SliderInt("Detail vertices", &model_vertices,
                                        static_cast<int>(geometry.minimum_vertices),
                                        static_cast<int>(geometry.frames[0].size()));
            if (changed)
                attempt([&] { apply_model_pose(renderer); });
            ImGui::Text("%zu active triangles / %zu stored faces", model->mesh.indices.size() / 3,
                        geometry.faces.size());
        }
        ImGui::BeginDisabled(view != View::Model);
        ImGui::Checkbox("Model materials", &model_materials);
        ImGui::EndDisabled();
        if (view == View::Terrain)
            ImGui::TextWrapped(preview ? "Diagnostic minimap projection."
                                       : "Original terrain tiles, seven mip levels, and baked lightmap.");
        else if (view == View::Model && saxs)
            ImGui::TextWrapped(
                "SXCore skeletal mesh: ring lofting, seams, and caps per the original builder. "
                "Factor 1 is the closest original detail. Root height follows the clip; "
                "horizontal root motion and lighting are inspector approximations.");
        else if (view == View::Model)
            ImGui::TextWrapped("Decoded morph frames, progressive detail, and original scoped materials. "
                               "Playback speed is a development preview.");
        if (view == View::Terrain && simulation) {
            for (const auto& side : simulation->sides().instances())
                if (side->outcome)
                    ImGui::Text("Side %u has %s", side->definition.index, side->outcome == 1 ? "won" : "lost");
            if (const auto& unsupported = simulation->triggers().unsupported(); !unsupported.empty())
                ImGui::Text("%zu trigger codes stopped instances (reconstruction pending)", unsupported.size());
        }
        if (view == View::Terrain && scene.creatures + scene.wizards &&
            ImGui::CollapsingHeader("Character motion diagnostics")) {
            ImGui::SliderInt("Character", &controlled_character, 0,
                             static_cast<int>(scene.creatures + scene.wizards) - 1);
            if (auto* actor = character(static_cast<unsigned>(controlled_character))) {
                simulation->match().local_side = actor->allegiance;
                auto control = actor->direct_controls.value_or(actor->controls);
                bool changed = ImGui::SliderInt("Turn", &control[0], -128, 128);
                changed |= ImGui::SliderInt("Forward / flight pitch", &control[1], -128, 128);
                changed |= ImGui::SliderInt("Flight throttle", &control[2], 0, 128);
                if (changed)
                    actor->set_direct_controls(control);
                if (ImGui::Button("Stop controls")) {
                    actor->direct_controls.reset();
                    actor->input_provider = {};
                    actor->controls = {};
                    actor->orders.clear();
                }
                const auto& p = actor->motion.position;
                ImGui::Text("World %.3f, %.3f, %.3f", p.x, p.y, p.z);
                ImGui::Text("Health %d / %d   Mana %d / %d", actor->stats.current[5], actor->stats.base[5],
                            actor->mana, actor->maximum_mana);
                ImGui::Text("Souls %d", actor->soul_count);
                if (actor->casting.error.value)
                    ImGui::Text("Cast rejected: %s", actor->casting.error.text().c_str());
                else if (actor->casting.accepted)
                    ImGui::Text("Cast accepted: %s", actor->casting.definition.text().c_str());
                ImGui::TextWrapped("Direct controller inputs. Issuing an order releases these controls.");
                ImGui::SeparatorText("Orders");
                const auto state = actor->fsm().state();
                const bool orderable = state && (state->flags & 1) && !(state->flags & 0x20000000);
                if (!orderable)
                    ImGui::TextUnformatted(state && (state->flags & 0x20000000)
                                               ? "Script-controlled unit"
                                               : "Orders unavailable in current state");
                ImGui::BeginDisabled(!orderable);
                ImGui::InputFloat3("Destination X/Y/Z", order_point.data());
                if (ImGui::Button("Use current position"))
                    order_point = {p.x, p.y, p.z};
                ImGui::Checkbox("Append to queue", &append_orders);
                const Vec3 destination{order_point[0], order_point[1], order_point[2]};
                if (ImGui::Button("Move"))
                    actor->issue_order({1, nullptr, destination}, append_orders ? 1 : 0);
                ImGui::SameLine();
                if (ImGui::Button("Move and hold"))
                    actor->issue_order({4, nullptr, destination}, append_orders ? 1 : 0);
                ImGui::SameLine();
                if (ImGui::Button("Face point"))
                    actor->issue_order({36, nullptr, destination});
                ImGui::Checkbox("Target building", &target_building);
                MatchObject* target = nullptr;
                if (target_building && !scene.building_groups.empty()) {
                    ImGui::SliderInt("Building target", &order_building, 0,
                                     static_cast<int>(scene.building_groups.size()) - 1);
                    const auto& group = scene.building_groups[order_building];
                    target = scene.spawns[group.first_spawn + group.definition.primary_part()].object;
                } else {
                    ImGui::SliderInt("Character target", &order_target, 0,
                                     static_cast<int>(scene.creatures + scene.wizards) - 1);
                    target = character(static_cast<unsigned>(order_target));
                }
                if (target) {
                    if (ImGui::Button("Follow"))
                        actor->issue_order({3, target}, append_orders ? 1 : 0);
                    ImGui::SameLine();
                    if (ImGui::Button(actor->type == MatchObject::wizard ? "Cast selected spell" : "Attack"))
                        attempt([&] {
                            if (actor->type == MatchObject::wizard) {
                                if (!order_spell.value || !actor->issue_spell_order(order_spell, target))
                                    throw Error("Select a learned spell first");
                            } else if (!actor->issue_order({6, target}, append_orders ? 1 : 0))
                                throw Error("This character has no melee attack");
                        });
                    if (const auto* c = dynamic_cast<const CreatureActor*>(target))
                        ImGui::Text("Target health %d / %d", c->stats.current[5], c->stats.base[5]);
                    else if (const auto* b = dynamic_cast<const BuildingActor*>(target))
                        ImGui::Text("Target health %d / %d", b->health, b->maximum_health);
                }
                if (actor->type == MatchObject::wizard && ImGui::TreeNode("Wizard setup")) {
                    ImGui::InputInt("Soul grant", &soul_grant);
                    if (ImGui::Button("Add souls (diagnostic)"))
                        actor->change_souls(std::max(soul_grant, 0));
                    ImGui::Combo("God", &profile_god, "None\0Persephone\0Pyro\0James\0Stratos\0Charnel\0");
                    ImGui::SliderInt("Level", &profile_level, 0, 14);
                    if (ImGui::Button("Apply wizard profile"))
                        attempt([&] { apply_debug_wizard_profile(*actor, profile_god, profile_level); });
                    ImGui::TreePop();
                }
                if (ImGui::BeginCombo("Known spell",
                                      order_spell.value ? order_spell.text().c_str() : "Select spell")) {
                    const auto show = [&](const CreatureSpellBook& book) {
                        for (const auto& entry : book.entries) {
                            const auto* definition = simulation && simulation->match().spell_registry
                                                         ? simulation->match().spell_registry->find(entry.id)
                                                         : nullptr;
                            const auto run = definition && definition->spell ? definition->spell->run_id : 0;
                            const bool available = (definition && definition->kind == 2) ||
                                                   run == spell_function_id("HealRun") ||
                                                   run == spell_function_id("FireballRun") ||
                                                   run == spell_function_id("GenericShootStart") ||
                                                   run == spell_function_id("SacSpellStart");
                            ImGui::BeginDisabled(!available);
                            const auto label = entry.id.text() + (available ? "" : " (not available yet)");
                            if (ImGui::Selectable(label.c_str(), entry.id == order_spell))
                                order_spell = entry.id;
                            ImGui::EndDisabled();
                        }
                    };
                    show(actor->spells);
                    for (const auto& book : actor->wizard_spells)
                        show(book);
                    ImGui::EndCombo();
                }
                if (order_spell.value) {
                    if (ImGui::Button("Cast at target"))
                        attempt([&] {
                            if (!actor->issue_spell_order(order_spell, target))
                                throw Error("Spell is not in this character's spell book");
                        });
                    ImGui::SameLine();
                    if (ImGui::Button("Cast at point"))
                        attempt([&] {
                            if (!actor->issue_spell_order(order_spell, nullptr, destination))
                                throw Error("Spell is not in this character's spell book");
                        });
                }
                ImGui::Text("Queued orders: %u", static_cast<unsigned>(actor->orders.size()));
                if (actor->type == MatchObject::wizard && simulation) {
                    const auto souls = simulation->souls();
                    ImGui::Text("Soul bundles on map: %zu", souls.size());
                    if (!souls.empty()) {
                        selected_soul = std::clamp(selected_soul, 0, static_cast<int>(souls.size()) - 1);
                        ImGui::SliderInt("Soul bundle", &selected_soul, 0,
                                         static_cast<int>(souls.size()) - 1);
                        auto* soul = souls[selected_soul];
                        ImGui::Text("Souls in bundle: %d", soul->count);
                        const bool eligible =
                            actor->allegiance && (soul->eligible_sides & actor->allegiance->identity);
                        if (!eligible)
                            ImGui::TextUnformatted("Enemy souls require conversion");
                        if (ImGui::Button("Move to soul"))
                            actor->issue_order({1, nullptr, soul->motion.position});
                        ImGui::BeginDisabled(eligible || !actor->find_spell(FourCC::from_text("sacc")));
                        ImGui::SameLine();
                        if (ImGui::Button("Convert soul"))
                            actor->issue_spell_order(FourCC::from_text("sacc"), soul);
                        ImGui::EndDisabled();
                    }
                }
                ImGui::EndDisabled();
            }
        }
        if (ImGui::Button("Reset camera"))
            camera = home;
        ImGui::SameLine();
        ImGui::SetNextItemWidth(180);
        ImGui::SliderFloat("Speed", &camera.speed, 0.01f, 3000, "%.2f", ImGuiSliderFlags_Logarithmic);
        ImGui::TextWrapped("Right mouse: look. WASD: move. Q/E: down/up. Shift: faster.");
        if (!error.empty()) {
            ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1, .45f, .3f, 1));
            ImGui::TextWrapped("%s", error.c_str());
            ImGui::PopStyleColor();
        }
        if (ImGui::BeginTabBar("content")) {
            if (ImGui::BeginTabItem("Maps")) {
                ImGui::BeginChild("maps", {0, 320});
                for (const auto& path : maps) {
                    auto text = utf8(path.lexically_relative(root / "maps")); // No disk access per frame.
                    if (ImGui::Selectable(text.c_str()))
                        attempt([&] { load(path, renderer); });
                }
                ImGui::EndChild();
                ImGui::EndTabItem();
            }
            if (ImGui::BeginTabItem("Packs")) {
                ImGui::BeginChild("packs", {0, 320});
                for (const auto& path : packs) {
                    auto text = utf8(path.lexically_relative(root / "data"));
                    if (ImGui::Selectable(text.c_str()))
                        attempt([&] { load(path, renderer); });
                }
                ImGui::EndChild();
                ImGui::EndTabItem();
            }
            if (ImGui::BeginTabItem("Campaign")) {
                ImGui::TextWrapped("%s", campaign.name.c_str());
                ImGui::BeginChild("campaign", {0, 320});
                for (const auto& [n, level] : campaign.levels) {
                    auto name = "Level " + std::to_string(n);
                    if (ImGui::TreeNode(name.c_str())) {
                        for (const auto& [choice, file] : level.missions) {
                            auto label = choice + ": " + file;
                            if (ImGui::Selectable(label.c_str()))
                                attempt([&] { load(root / "maps/Campaign/Sacrifice" / file, renderer); });
                        }
                        ImGui::TreePop();
                    }
                }
                ImGui::EndChild();
                ImGui::EndTabItem();
            }
            if (ImGui::BeginTabItem("Media")) {
                if (ImGui::Button("Stop sound") && audio)
                    audio->stop();
                ImGui::BeginChild("media", {0, 320});
                ImGui::TextUnformatted("Smacker videos");
                for (const auto& path : videos) {
                    auto label = utf8(path.filename());
                    if (ImGui::Selectable(label.c_str()))
                        attempt([&] {
                            auto next = std::make_unique<VideoClip>(path);
                            Image image;
                            if (!next->next(image))
                                throw Error("Video has no frames");
                            displayed_image = std::move(image);
                            renderer.set_image(displayed_image);
                            video = std::move(next);
                            video_time = 0;
                            view = View::Image;
                            status = label;
                        });
                }
                ImGui::Separator();
                ImGui::TextUnformatted("Music");
                for (const auto& path : music) {
                    auto label = utf8(path.filename());
                    if (ImGui::Selectable(label.c_str()))
                        attempt([&] {
                            auto clip = decode_audio(path);
                            if (!audio)
                                audio = std::make_unique<AudioOutput>();
                            audio->play(std::move(clip));
                            status = "Playing " + label;
                        });
                }
                ImGui::EndChild();
                ImGui::EndTabItem();
            }
            ImGui::EndTabBar();
        }
        if (selected) {
            ImGui::Separator();
            ImGui::InputText("Filter", filter, sizeof(filter));
            ImGui::BeginChild("resources", {0, 220});
            // Only the visible rows are emitted; a large map archive otherwise cost most of a frame.
            listed_resources.clear();
            for (std::size_t i = 0; i < selected->records().size(); ++i) {
                const auto& e = selected->records()[i];
                if (!e.folder() && (!filter[0] || e.path.find(filter) != std::string::npos))
                    listed_resources.push_back(i);
            }
            ImGuiListClipper clipper;
            clipper.Begin(static_cast<int>(listed_resources.size()));
            while (clipper.Step())
            for (int row = clipper.DisplayStart; row < clipper.DisplayEnd; ++row) {
                const auto i = listed_resources[static_cast<std::size_t>(row)];
                const auto& e = selected->records()[i];
                ImGui::PushID(static_cast<int>(i));
                auto label = e.tag.text() + ":" + e.name.text();
                if (ImGui::Selectable(label.c_str()))
                    attempt([&] { show(i, renderer); });
                ImGui::PopID();
            }
            ImGui::EndChild();
        }
        ImGui::End();
        if (!resource_description.empty()) {
            ImGui::SetNextWindowPos({420, 12}, ImGuiCond_FirstUseEver);
            ImGui::SetNextWindowSize({560, 320}, ImGuiCond_FirstUseEver);
            ImGui::Begin("Resource details");
            ImGui::TextWrapped("%s", resource_description.c_str());
            ImGui::Separator();
            ImGui::TextUnformatted(resource_text.c_str(), resource_text.c_str() + resource_text.size());
            ImGui::End();
        }
        if (view == View::Image && renderer.image_handle()) {
            ImGui::SetNextWindowPos({420, 80}, ImGuiCond_FirstUseEver);
            ImGui::SetNextWindowSize({600, 560}, ImGuiCond_FirstUseEver);
            ImGui::Begin("Image / video");
            auto available = ImGui::GetContentRegionAvail();
            auto scale = std::min(available.x / displayed_image.width, available.y / displayed_image.height);
            ImGui::Image(static_cast<ImTextureID>(reinterpret_cast<std::uintptr_t>(renderer.image_handle())),
                         {displayed_image.width * scale, displayed_image.height * scale});
            ImGui::End();
        }
    }
};
int run(const Arguments& args) {
    auto root = discover_game_dir(args.game_dir);
    set_trace_ai(args.trace_ai);
    log("SacrificeModern 0.1.0 / C++20 / x64 / MSVC " + std::to_string(_MSC_VER));
    log("Asset directory: " + utf8(root));
    Window window(args.smoke);
    Renderer renderer(window.handle());
    Inspector inspector(root, !args.smoke); // Captures retain audio RNG without opening/playing a device.
    // Smoke runs keep the default options; their scene ticks run before the frame loop sets this each frame.
    inspector.classic_look = args.classic_look;
    inspector.remastered = !args.classic_look && inspector.options.value(FourCC::from_text("rmst")) != 0;
    inspector.load(args.map.empty() ? root / "maps/(2) Trial.scp"
                                    : (args.map.is_absolute() ? args.map : root / args.map),
                   renderer);
    if (!args.file.empty()) {
        inspector.load(args.file.is_absolute() ? args.file : root / args.file, renderer);
        if (!args.resource.empty()) {
            auto resource = inspector.selected->find_path(args.resource);
            if (!resource)
                throw Error("Requested resource was not found: " + args.resource);
            inspector.show(*resource, renderer);
        }
    }
    if (inspector.saxs && inspector.view == View::Model) {
        if (args.model_vertices)
            throw Error("--model-vertices applies to MRMM models; use --model-detail for SAXS");
        if (!args.clip.empty()) {
            const auto& clips = inspector.saxs->clips;
            auto it =
                std::find_if(clips.begin(), clips.end(), [&](const auto& c) { return c.name == args.clip; });
            if (it == clips.end())
                throw Error("Requested clip is not compatible with the SAXS model: " + args.clip);
            inspector.select_clip(static_cast<int>(it - clips.begin()), renderer);
        }
        if (args.model_detail)
            inspector.saxs_detail = *args.model_detail;
        if (args.model_frame)
            inspector.saxs_frame = *args.model_frame;
        inspector.apply_saxs_pose(renderer, true);
        inspector.play_model = args.model_preview;
    } else if (!args.clip.empty() || args.model_detail)
        throw Error("--clip and --model-detail require a selected SAXS model");
    else if (args.model_frame || args.model_vertices || args.model_preview) {
        if (!inspector.model || inspector.view != View::Model)
            throw Error("Model pose options require a selected MRMM resource");
        if (args.model_frame)
            inspector.model_frame = *args.model_frame;
        if (args.model_vertices && *args.model_vertices) {
            if (*args.model_vertices > inspector.model->mesh.vertices.size())
                throw Error("Requested model detail exceeds vertex count");
            inspector.model_vertices = static_cast<int>(*args.model_vertices);
        }
        inspector.apply_model_pose(renderer);
        inspector.play_model = args.model_preview;
    }
    if (args.view_yaw && inspector.view == View::Model) {
        // Orbit the fitted model camera around its target for comparison captures.
        auto& c = inspector.camera;
        const auto yaw = *args.view_yaw * 3.14159265f / 180.0f;
        const auto distance = std::hypot(c.position.z - inspector.focus.z, c.position.x - inspector.focus.x);
        c.position.x = inspector.focus.x - std::sin(yaw) * distance;
        c.position.z = inspector.focus.z - std::cos(yaw) * distance;
        c.yaw = yaw;
        inspector.home = c;
    }
    inspector.show_environment = !args.hide_environment;
    if (!args.campaign.empty()) {
        if (!args.smoke)
            throw Error("--campaign requires --smoke");
        inspector.smoke_campaign(args.campaign, renderer);
    }
    // Without a requested file or map the executable opens on the native title page.
    if (args.front_end.value || (!args.smoke && args.file.empty() && args.map.empty()))
        inspector.enter_front_end(args.front_end.value ? args.front_end : FourCC::from_text("titl"));
    if (args.camera && inspector.view == View::Terrain) {
        inspector.third_person = false; // Explicit diagnostic cameras must survive the follow-camera update.
        const auto& [x, y, height, yaw, pitch] = *args.camera;
        inspector.camera.position = {x, height, y};
        inspector.camera.yaw = yaw * 3.14159265f / 180.0f;
        inspector.camera.pitch = pitch * 3.14159265f / 180.0f;
        inspector.home = inspector.camera;
    }
    if (args.creature_damage) {
        if (!args.smoke || inspector.view != View::Terrain || !inspector.simulation)
            throw Error("--scene-creature-damage requires --smoke and a terrain view");
        const auto [ordinal, amount] = *args.creature_damage;
        auto* actor = inspector.character(static_cast<unsigned>(ordinal));
        if (!actor)
            throw Error("--scene-creature-damage character outside map");
        if (!actor->damage(inspector.simulation->match(), {0, amount, 0}))
            throw Error("--scene-creature-damage target is not damageable");
        log(std::format("Damage preview character {} at {:.3f},{:.3f},{:.3f}; blood particles {}", ordinal,
                        actor->motion.position.x, actor->motion.position.y, actor->motion.position.z,
                        actor->blood_host ? actor->blood_host->particles(inspector.simulation->match()).count() : 0));
        inspector.sync_scene(renderer);
    }
    if (args.scene_burn) {
        if (!args.smoke || inspector.view != View::Terrain || !inspector.simulation)
            throw Error("--scene-burn requires --smoke and a terrain view");
        const auto [ordinal, duration] = *args.scene_burn;
        auto* actor = inspector.character(static_cast<unsigned>(ordinal));
        if (!actor || !actor->alive() || !actor->bone_count())
            throw Error("--scene-burn requires a live character with a SAXS body");
        const auto& m = actor->motion;
        const Vec3 position{m.position.x + m.reference_center.x, m.position.y + m.reference_center.y,
                            m.position.z + m.reference_center.z};
        actor->burn_near(position, 5, duration);
        log(std::format("Burn preview character {} at {:.3f},{:.3f},{:.3f}; masks {:x}/{:x}, {} bones, {} ticks",
                        ordinal, m.position.x, m.position.y, m.position.z, actor->permanent_burns,
                        actor->temporary_burns, actor->bone_count(), actor->burn_ticks));
    }
    if (args.scene_altar_ruin) {
        if (!args.smoke || inspector.view != View::Terrain || !inspector.simulation)
            throw Error("--scene-altar-ruin requires --smoke and a terrain view");
        auto* wizard = inspector.character(*args.scene_altar_ruin);
        if (!wizard || !wizard->home_altar)
            throw Error("--scene-altar-ruin requires a wizard with a home altar");
        auto& match = inspector.simulation->match();
        const auto origin = wizard->home_altar->motion.position;
        wizard->home_altar->common_status |= 0x100; // Native no-respawn flag; use ordinary death/elimination.
        for (auto* side : match.side_order)
            if (side->controller)
                side->controller->flags |= 8;
        if (!wizard->damage(match, {0, 20000, 0}))
            throw Error("Altar ruin preview cannot damage its wizard");
        unsigned ticks = 0, parts = 0, towers = 0;
        while (!parts && ticks < 1600) {
            inspector.tick_scene(1.0f / 60, renderer, 1, false);
            ++ticks;
            for (const auto& object : match.objects())
                if (const auto* spell = dynamic_cast<const SpellActor*>(object.get());
                    spell && spell->effect == SpellActor::Effect::altar_ruin) {
                    ++parts;
                    towers += spell->ruin.tower;
                }
        }
        if (!parts)
            throw Error("Altar ruin preview did not reach wizard elimination");
        inspector.third_person = false;
        if (!args.camera) {
            inspector.camera.position = {origin.x, origin.z + 35, origin.y - 140};
            inspector.camera.yaw = 0;
            inspector.camera.pitch = -.1f;
        }
        log(std::format("Altar ruin preview at {:.3f},{:.3f},{:.3f}: {} parts, {} towers, {} native warm-up ticks",
                        origin.x, origin.y, origin.z, parts, towers, ticks));
    }
    if (args.scene_warmup) {
        if (!args.smoke || inspector.view != View::Terrain)
            throw Error("--scene-warmup requires --smoke and a terrain view");
        for (unsigned tick = 0; tick < *args.scene_warmup; ++tick)
            inspector.tick_scene(1.0f / 60, renderer, 1, false);
        if (!inspector.error.empty())
            throw Error(inspector.error);
    }
    if (args.scene_ritual.value) {
        if (!args.smoke || inspector.view != View::Terrain || !inspector.simulation)
            throw Error("--scene-ritual requires --smoke and a terrain view");
        auto& host = *inspector.simulation;
        auto& match = host.match();
        CreatureActor* wizard = match.local_side ? dynamic_cast<CreatureActor*>(match.local_side->wizard()) : nullptr;
        for (const auto& object : match.objects())
            if (auto* actor = dynamic_cast<CreatureActor*>(object.get()); actor && actor->type == MatchObject::wizard &&
                actor->home_altar && actor->allegiance && actor->allegiance->identity == 2) {
                wizard = actor;
                break;
            }
        if (!wizard || !wizard->home_altar)
            for (const auto& object : match.objects())
                if (auto* actor = dynamic_cast<CreatureActor*>(object.get());
                    actor && actor->type == MatchObject::wizard && actor->home_altar) {
                    wizard = actor;
                    break;
                }
        CreatureActor* opponent = nullptr;
        if (wizard)
            for (const auto& object : match.objects())
                if (auto* actor = dynamic_cast<CreatureActor*>(object.get());
                    actor && actor->type == MatchObject::wizard && actor->home_altar &&
                    actor->allegiance != wizard->allegiance) {
                    opponent = actor;
                    if (actor->allegiance && actor->allegiance->identity == 4)
                        break;
                }
        if (!wizard || !wizard->home_altar || !opponent)
            throw Error("Ritual preview needs two enemy wizards with altars");
        wizard->allegiance->enemies |= opponent->allegiance->identity;
        opponent->allegiance->enemies |= wizard->allegiance->identity;
        for (auto* side : match.side_order)
            if (side->controller)
                side->controller->flags |= 8;
        const bool desecrate = args.scene_ritual == FourCC::from_text("sacu");
        auto& altar = *(desecrate ? opponent->home_altar : wizard->home_altar);
        auto point = altar.sacrifice_point();
        if (desecrate) {
            wizard->motion.position = altar.motion.position;
            wizard->motion.position.x += 35;
        }
        auto victim_point = altar.motion.position;
        victim_point.x += 30;
        auto* victim = host.create_creature(desecrate ? *wizard : *opponent, FourCC::from_text("maho"), victim_point);
        if (!victim)
            throw Error("Ritual preview cannot create its victim");
        // Same native acceptance path as the conversion regression's second ceremony, after escort.
        victim->fsm().event(42);
        victim->fsm().event(29);
        if (!altar.request_sacrifice(args.scene_ritual, victim, nullptr, wizard))
            throw Error("Ritual preview reservation was refused");
        auto* ceremony = SpellActor::launch_altar_process(match, *wizard, altar);
        if (!ceremony)
            throw Error("Ritual preview ceremony was refused");
        inspector.third_person = false;
        if (!args.camera) {
            inspector.camera.position = {point.x, point.z - 5, point.y - 28};
            inspector.camera.yaw = 0;
            inspector.camera.pitch = std::atan2(5.0f, 28.0f);
        }
        unsigned ready_ticks = 0;
        while (ceremony->effect == SpellActor::Effect::altar_process &&
               !(ceremony->state_flags & MatchObject::removed_flag) && ceremony->stage != 97 && ready_ticks < 4000) {
            inspector.tick_scene(1.0f / 60, renderer, 1, false);
            ++ready_ticks;
        }
        if (ceremony->effect != SpellActor::Effect::altar_process ||
            (ceremony->state_flags & MatchObject::removed_flag) || ceremony->stage != 97) {
            for (const auto* doctor : ceremony->sacrifice.doctors)
                if (doctor) {
                    const auto& m = doctor->motion;
                    const auto& c = doctor->doctor_circle;
                    const auto* blocker = doctor->approach_blocker;
                    log(std::format("Ritual doctor {}: state {} flags {:x}/{:x}, pos {:.4f},{:.4f},{:.4f}, "
                                    "yaw {:.4f}, circle {:.6f}->{:.6f}, arrived {}, approach {}, blocker {}",
                                    doctor->reference(), doctor->fsm().state()->id.text(), doctor->fsm().state()->flags,
                                    doctor->state_flags, m.position.x, m.position.y, m.position.z, m.yaw,
                                    c.current_angle, c.angle, c.arrived, doctor->approach_result,
                                    blocker ? blocker->reference() : 0));
                    log(std::format("Ritual doctor controls {},{},{}; velocity {:.6f},{:.6f},{:.6f}; "
                                    "path {:.4f},{:.4f},{:.4f}, avoid {:.4f}, flags {:x}/{:x}",
                                    doctor->controls[0], doctor->controls[1], doctor->controls[2], m.local_velocity.x,
                                    m.local_velocity.y, m.local_velocity.z, doctor->path.target.x, doctor->path.target.y,
                                    doctor->path.target.z, doctor->path.avoidance, m.flags, m.collision_mask));
                }
            throw Error(std::format("Ritual preview did not reach processing: effect {}, stage {}, flags {}",
                                    static_cast<unsigned>(ceremony->effect), ceremony->stage, ceremony->sacrifice.flags));
        }
        log(std::format("Ritual preview {} at {:.3f},{:.3f},{:.3f}", args.scene_ritual.text(),
                        point.x, point.y, point.z));
        log(std::format("Ritual preview reached processing after {} native ticks", ready_ticks));
    }
    if (args.scene_control) {
        if (inspector.view != View::Terrain)
            throw Error("--scene-control requires a terrain view");
        const auto& control = *args.scene_control;
        auto* actor = inspector.character(static_cast<unsigned>(control[0]));
        if (!actor)
            throw Error("--scene-control character outside map");
        actor->set_direct_controls({control[1], control[2], control[3]});
        inspector.controlled_character = control[0];
    }
    if (args.scene_move) {
        const auto value = *args.scene_move;
        auto* actor = inspector.character(static_cast<unsigned>(value[0]));
        if (!actor || !inspector.simulation)
            throw Error("--scene-move character outside map");
        actor->issue_order({1, nullptr, Vec3{value[1], value[2], value[3]}});
    }
    if (args.scene_hover) {
        if (!args.smoke || !inspector.character(*args.scene_hover))
            throw Error("--scene-hover requires --smoke and a map character");
        inspector.smoke_hover = args.scene_hover;
        inspector.show_inspector = false;
    }
    if (args.scene_pointer) {
        if (!args.smoke)
            throw Error("--scene-pointer requires --smoke");
        inspector.smoke_pointer = Vec2{(*args.scene_pointer)[0], (*args.scene_pointer)[1]};
        inspector.show_inspector = false;
    }
    if (args.scene_target.value) {
        if (!args.smoke)
            throw Error("--scene-target requires --smoke");
        inspector.smoke_target = args.scene_target;
    }
    if (args.scene_select) {
        auto* actor = inspector.character(*args.scene_select);
        if (!args.smoke || !actor || !actor->allegiance || !inspector.simulation)
            throw Error("--scene-select requires --smoke and a selectable map creature");
        auto& match = inspector.simulation->match();
        match.local_side = actor->allegiance;
        inspector.player = std::make_unique<PlayerControl>(match, *actor->allegiance);
        inspector.command_menu.load(inspector.catalog);
        inspector.pointer_feedback.load(inspector.catalog);
        if (!inspector.player->select(actor))
            throw Error("--scene-select requires a selectable map creature");
    }
    if (args.scene_attack) {
        const auto value = *args.scene_attack;
        auto* actor = inspector.character(value[0]);
        auto* target = inspector.character(value[1]);
        if (!actor || !target)
            throw Error("--scene-attack requires valid characters and an attacking unit");
        if (args.scene_select) {
            if (!inspector.player->attack(target, target->motion.position))
                throw Error("Selected unit could not issue an attack command");
        } else if (!actor->issue_order({6, target}))
            throw Error("--scene-attack requires an attacking unit");
    }
    if (args.scene_souls) {
        auto* actor = inspector.character(static_cast<unsigned>((*args.scene_souls)[0]));
        if (!actor)
            throw Error("--scene-souls character outside map");
        actor->change_souls((*args.scene_souls)[1]);
    }
    if (args.scene_soul_move) {
        const auto value = *args.scene_soul_move;
        auto* actor = inspector.character(static_cast<unsigned>(value[0]));
        const auto souls = inspector.simulation ? inspector.simulation->souls() : std::vector<SoulActor*>{};
        if (!actor || static_cast<std::size_t>(value[1]) >= souls.size())
            throw Error("--scene-soul-move requires valid character and soul bundle ordinals");
        actor->issue_order({1, nullptr, souls[value[1]]->motion.position});
    }
    if (args.scene_cast) {
        const auto& [ordinal, spell, target_index] = *args.scene_cast;
        auto* actor = inspector.character(ordinal);
        auto* target = inspector.character(target_index);
        MatchObject* cast_target = target;
        // Explicit smoke casts may inspect any installed wizard spell, including later god levels.
        // Grant this entry and enough mana for one cast; ordinary gameplay uses the learned book/stats.
        if (args.smoke && actor && (actor->kind_flags & 1) && inspector.simulation) {
            auto& match = inspector.simulation->match();
            if (const auto* definition = match.spell_registry->find(spell); definition && definition->spell) {
                match.spell_registry->add_to_books(actor->wizard_spells, spell, true);
                const auto cost = actor->cast_cost(*definition).first;
                actor->maximum_mana = std::max(actor->maximum_mana, cost);
                actor->mana = std::max(actor->mana, cost);
                if (target && !actor->cast_target(*definition, target))
                    cast_target = nullptr;
            }
        }
        if (spell == FourCC::from_text("sacc") && inspector.simulation)
            for (auto* soul : inspector.simulation->souls())
                if (soul->anchor == target) {
                    cast_target = soul;
                    break;
                }
        if (args.smoke && spell == FourCC::from_text("swnd") && actor && target && inspector.simulation) {
            auto at = actor->motion.position;
            at.y += 20; // Inspect a short journey, outside the caster's automatic pickup radius.
            cast_target = inspector.simulation->create_souls(at, nullptr, 1);
        }
        if (!actor || !target || !actor->issue_spell_order(spell, cast_target, target->motion.position)) {
            std::string message = "--scene-cast requires valid characters and a learned spell. Wizard books:";
            unsigned index = 0;
            for (const auto& model : inspector.scene.models)
                if (model.character && model.character->object) {
                    const auto& wizard = *model.character->object;
                    if (wizard.kind_flags & 1) {
                        message += std::format("\n{} {} ({:.1f},{:.1f},{:.1f}):", index,
                            wizard.definition().id.text(), wizard.motion.position.x,
                            wizard.motion.position.y, wizard.motion.position.z);
                        for (const auto& book : wizard.wizard_spells)
                            for (const auto& entry : book.entries)
                                if (!(entry.flags & 2))
                                    message += " " + entry.id.text();
                    }
                    ++index;
                }
            throw Error(message);
        }
    }
    if (args.building_damage) {
        if (!args.smoke || inspector.view != View::Terrain || !inspector.simulation)
            throw Error("--scene-building-damage requires --smoke and a terrain view");
        const auto ordinal = static_cast<std::size_t>((*args.building_damage)[0]);
        if (ordinal >= inspector.scene.building_groups.size())
            throw Error("--scene-building-damage building outside map");
        const auto& group = inspector.scene.building_groups[ordinal];
        auto& spawn = inspector.scene.spawns[group.first_spawn + group.definition.primary_part()];
        auto& actor = static_cast<BuildingActor&>(*spawn.object);
        if (!actor.damage(inspector.simulation->match(), {0, (*args.building_damage)[1], 0}))
            throw Error("--scene-building-damage target is not damageable");
        inspector.sync_scene(renderer);
    }
    if (args.building_reveal) {
        if (!args.smoke || inspector.view != View::Terrain || !inspector.simulation)
            throw Error("--scene-building-reveal requires --smoke and a terrain view");
        const auto [ordinal, reveal] = *args.building_reveal;
        if (ordinal >= inspector.scene.building_groups.size())
            throw Error("--scene-building-reveal building outside map");
        const auto& group = inspector.scene.building_groups[ordinal];
        const auto& spawn = inspector.scene.spawns[group.first_spawn + group.definition.primary_part()];
        static_cast<BuildingActor&>(*spawn.object).reveal = reveal;
        inspector.tick_scene(0, renderer, 0, false); // Secondary parts copy the primary even at zero ticks.
        inspector.sync_scene(renderer);
    }
    if (!args.campaign_actions.empty()) {
        if (!args.smoke || args.campaign.empty())
            throw Error("--campaign-actions requires --smoke --campaign");
        inspector.smoke_campaign_actions(args.campaign_actions, renderer);
    }
    const auto initial_character_count = inspector.scene.creatures + inspector.scene.wizards;
    if (args.scene_ticks) {
        if (!args.smoke || inspector.view != View::Terrain)
            throw Error("--scene-ticks requires --smoke and a terrain view");
        std::size_t scar_seen = 0;
        if (args.scene_weather && inspector.simulation)
            inspector.simulation->match().weather.set(*args.scene_weather, 1, 0);
        for (unsigned tick = 0; tick < *args.scene_ticks; ++tick) {
            inspector.tick_scene(1.0f / 60, renderer, 1, false);
            if (args.scene_cast && inspector.simulation) {
                const auto& scars = inspector.simulation->match().scar_requests;
                for (; scar_seen < scars.size(); ++scar_seen) {
                    const auto& scar = scars[scar_seen];
                    log(std::format("Scene tick {}: SCAR:{} at {:.3f},{:.3f},{:.3f}", tick + 1,
                                    scar.id.text(), scar.position.x, scar.position.y, scar.position.z));
                }
            }
        }
        inspector.sync_scene(renderer);
        if (!inspector.error.empty())
            throw Error(inspector.error);
        if (args.scene_cast) {
            unsigned uploaded = 0;
            for (const auto& model : inspector.scene.models)
                if (model.spell_visual && model.spell_visual->uploaded_frame >= 0)
                    ++uploaded;
            log(std::format("Scene spell models with successful dynamic uploads: {}", uploaded));
            if (inspector.simulation) {
                unsigned surfaces = 0, sprites = 0, clouds = 0, beams = 0;
                for (const auto& object : inspector.simulation->match().objects()) {
                    const auto* spell = dynamic_cast<SpellActor*>(object.get());
                    if (spell && spell->effect == SpellActor::Effect::generic_ground_trail)
                        for (const auto& system : spell->particle_systems)
                            if (system.alive()) {
                                const auto& at = system.particles().front().position;
                                const auto& old = system.particles().back();
                                log(std::format("Scene ground trail: TXTR:{}; {} particles at {:.1f},{:.1f},{:.1f}; oldest frame {:.1f}/{} size {:.2f} alpha {:.1f}",
                                    system.texture().text(), system.count(), at.x, at.y, at.z,
                                    old.frame, system.frames(), old.size, old.color[3]));
                            }
                    if (!spell || spell->spell_id != std::get<1>(*args.scene_cast) ||
                        (spell->state_flags & MatchObject::removed_flag))
                        continue;
                    if (spell->surface.kind != SpellActor::CurvedSurface::none && spell->surface.opacity > 0)
                        ++surfaces;
                    if (spell->sprite.enabled && spell->sprite.size > 0)
                        ++sprites;
                    if (spell->altar_cloud.enabled && spell->altar_cloud.opacity > 0)
                        ++clouds;
                    if (spell->beam.enabled)
                        ++beams;
                    if (surfaces + sprites + clouds + beams == 1)
                        log(std::format("Scene spell visual: stage {} at {:.1f},{:.1f},{:.1f}",
                            spell->stage, spell->motion.position.x, spell->motion.position.y,
                            spell->motion.position.z));
                }
                log(std::format("Scene cast visuals: {} surfaces, {} sprites, {} clouds, {} beams",
                    surfaces, sprites, clouds, beams));
            }
        }
        if (!args.campaign.empty() && inspector.simulation) {
            const auto& triggers = inspector.simulation->triggers();
            const auto codes = [](const std::set<std::uint32_t>& list) {
                std::string text;
                for (const auto code : list)
                    text += " " + std::to_string(code >= 0x10000 ? code & 0xffff : code) +
                            (code >= 0x30000 ? "o" : code >= 0x10000 ? "a" : "");
                return text.empty() ? std::string(" none") : text;
            };
            const auto* wizard = inspector.campaign_state.wizard;
            const auto* side = inspector.simulation->match().local_side;
            log(std::format("Campaign smoke after {} ticks: {} trigger instances; wizard level {} god {} spells {} "
                            "mana {}/{} health {}/{}; outcome {}; unsupported:{}; deferred:{}",
                            *args.scene_ticks, triggers.instances(), wizard ? wizard->progression.level : -1,
                            wizard ? wizard->progression.stats[18] : 0,
                            wizard ? wizard->progression.learned.size() : 0,
                            wizard ? wizard->mana : 0, wizard ? wizard->maximum_mana : 0,
                            wizard ? wizard->stats.current[5] : 0, wizard ? wizard->stats.base[5] : 0,
                            side ? side->outcome : 0, codes(triggers.unsupported()), codes(triggers.deferred())));
        }
        if (args.scene_ritual.value) {
            unsigned beams = 0, rings = 0, globes = 0, clouds = 0, streamers = 0;
            for (const auto& object : inspector.simulation->match().objects())
                if (const auto* spell = dynamic_cast<const SpellActor*>(object.get())) {
                    beams += spell->beam.enabled;
                    rings += spell->altar_ring.enabled && spell->altar_ring.radius > 0;
                    clouds += spell->altar_cloud.enabled && spell->altar_cloud.opacity > 0;
                    streamers += spell->altar_streamer.enabled && spell->altar_streamer.primitive.points().size() > 1;
                    globes += spell->effect == SpellActor::Effect::desecration_globe && spell->portal.opacity > 0;
                    if (spell->effect == SpellActor::Effect::desecration_globe && spell->portal.opacity > 0)
                        log(std::format("Ritual globe at {:.3f},{:.3f},{:.3f}", spell->motion.position.x,
                                        spell->motion.position.y, spell->motion.position.z));
                }
            log(std::format("Ritual preview at {} ticks: {} beam hosts, {} rings, {} globes, {} clouds, {} streamers",
                            *args.scene_ticks, beams, rings, globes, clouds, streamers));
        }
        const auto report = [&](unsigned ordinal) {
            const auto* actor = inspector.character(ordinal);
            if (!actor)
                return;
            const auto line = std::format(
                "Character {}: position {:.3f},{:.3f},{:.3f}; health {}/{}; mana "
                "{}/{}; souls {}; order {}; queued {}; cast {}; error {}",
                ordinal, actor->motion.position.x, actor->motion.position.y, actor->motion.position.z,
                actor->stats.current[5], actor->stats.base[5], actor->mana, actor->maximum_mana,
                actor->soul_count, actor->orders.kind(), actor->orders.size(), actor->casting.accepted,
                actor->casting.error.value ? actor->casting.error.text() : "none");
            log(line);
            inspector.status += "\n" + line;
        };
        if (args.scene_move)
            report(static_cast<unsigned>((*args.scene_move)[0]));
        if (args.scene_soul_move) {
            report(static_cast<unsigned>((*args.scene_soul_move)[0]));
            inspector.status +=
                std::format("\nSoul bundles remaining: {}", inspector.simulation->souls().size());
        }
        if (args.scene_attack) {
            report((*args.scene_attack)[0]);
            report((*args.scene_attack)[1]);
        }
        if (args.scene_cast) {
            report(std::get<0>(*args.scene_cast));
            if (std::get<2>(*args.scene_cast) != static_cast<int>(std::get<0>(*args.scene_cast)))
                report(std::get<2>(*args.scene_cast));
            unsigned ordinal = 0;
            for (const auto& model : inspector.scene.models)
                if (model.character) {
                    if (ordinal >= initial_character_count && model.character->object)
                        report(ordinal);
                    ++ordinal;
                }
        }
    }
    DocumentStore settings(application_data() / "settings.json", "SacrificeModern.settings", 1);
    if (!args.smoke)
        inspector.attempt([&] {
            if (auto saved = settings.read()) {
                inspector.wireframe = saved->value("wireframe", false);
                auto speed = saved->value("camera_speed", 500.0f);
                if (std::isfinite(speed))
                    inspector.camera.speed = std::clamp(speed, 0.01f, 3000.0f);
                inspector.scene_sound = saved->value("ambient_sound", true);
                const auto volume = saved->value("ambient_volume", 1.0f);
                if (std::isfinite(volume))
                    inspector.scene_volume = std::clamp(volume, 0.0f, 1.0f);
                if (const auto it = saved->find("options"); it != saved->end())
                    inspector.options.load(*it);
                inspector.apply_options();
            }
        });
    if (args.graphics_detail)
        inspector.options.set(FourCC::from_text("gdet"), static_cast<int>(*args.graphics_detail));
    inspector.defer_loads = !args.smoke;
    auto previous = std::chrono::steady_clock::now(), next_frame = previous;
    unsigned frames{};
    const auto vsync = [&] { return inspector.options.value(FourCC::from_text("tbuf")) != 0; };
    FpsOverlay fps_overlay;
    // Port diagnostics: the frame timing readout, refreshed every half second.
    float timing_elapsed{};
    std::vector<std::string> timing_lines;
    const auto describe_timings = [](const RenderTimings& t) {
        std::vector<std::string> lines;
        lines.push_back(std::format("CPU draw {:.2f} ms (plants {:.2f}, grass {:.2f}), end {:.2f}", t.cpu_draw,
                                    t.cpu_widgets, t.cpu_grass, t.cpu_end));
        if (t.gpu) {
            lines.push_back(std::format("GPU {:.2f} ms: shadow {:.2f}, land {:.2f}, models {:.2f}", t.gpu_total,
                                        t.gpu_shadow, t.gpu_land, t.gpu_models));
            lines.push_back(std::format("    foliage {:.2f}, effects {:.2f}, post+UI {:.2f}, other {:.2f}",
                                        t.gpu_foliage, t.gpu_effects, t.gpu_post, t.gpu_other));
        }
        lines.push_back(std::format("plants {:.0f}k tris, grass {:.0f}k tris, shadow casters {:.0f}",
                                    t.widget_triangles / 1000, t.grass_triangles / 1000, t.casters));
        return lines;
    };
    const bool bench = args.smoke && args.bench_frames;
    const unsigned smoke_frames = 3 + (bench ? *args.bench_frames : 0);
    auto bench_start = std::chrono::steady_clock::now();
    while (window.pump()) {
        window.borderless(!args.smoke && inspector.options.value(FourCC::from_text("wmod")) != 0);
        if (!window.width() || !window.height()) {
            Sleep(20);
            previous = std::chrono::steady_clock::now();
            continue;
        }
        auto now = std::chrono::steady_clock::now();
        auto dt = std::min(std::chrono::duration<float>(now - previous).count(), 0.1f);
        previous = now;
        if (inspector.loading.work) {
            if (inspector.loading.shown) {
                inspector.load_progress = [&](float) {
                    renderer.begin(window.width(), window.height());
                    inspector.loading_screen(window.width(), window.height(), renderer);
                    renderer.end(false);
                };
                inspector.run_loading(renderer);
                inspector.load_progress = {};
                previous = std::chrono::steady_clock::now(); // Loading time is not simulation time.
                continue;
            }
            renderer.begin(window.width(), window.height());
            inspector.loading_screen(window.width(), window.height(), renderer);
            renderer.end(vsync());
            inspector.loading.shown = true;
            continue;
        }
        window.swap_buttons(inspector.options.value(FourCC::from_text("swap")) != 0);
        inspector.drive(window.input());
        inspector.tick_model(dt, renderer);
        if (!args.scene_ticks) {
            const bool fast = inspector.cinematic_fast_forward;
            inspector.tick_scene(dt, renderer, {}, !fast);
            for (unsigned i = 1;
                 fast && i < 8 && inspector.in_cutscene() &&
                 inspector.simulation->triggers().choices.empty() && !inspector.end_transition.action.value;
                 ++i)
                inspector.tick_scene(dt, renderer, {}, false);
            if (fast)
                inspector.sync_scene(renderer);
        }
        const bool tick_view = inspector.follow_wizard(window.input(), window.width(), window.height());
        if (!tick_view && (!ImGui::GetIO().WantCaptureKeyboard || window.input().looking))
            inspector.camera.update(window.input(), dt);
        if (inspector.video) {
            inspector.video_time += dt;
            if (inspector.video_time >= inspector.video->frame_seconds()) {
                inspector.video_time = 0;
                inspector.attempt([&] {
                    if (inspector.video->next(inspector.displayed_image))
                        renderer.set_image(inspector.displayed_image);
                    else
                        inspector.video.reset();
                });
            }
        }
        if (inspector.simulation)
            inspector.simulation->match().interpolate = inspector.options.value(FourCC::from_text("smth")) != 0;
        inspector.remastered = !args.classic_look && inspector.options.value(FourCC::from_text("rmst")) != 0;
        renderer.set_remastered(inspector.remastered);
        renderer.begin(window.width(), window.height());
        renderer.set_detail(inspector.render_detail.update(dt > 0 ? 1 / dt : 0,
            inspector.options.detail_target(), inspector.options.value(FourCC::from_text("gdet")),
            !inspector.simulation || inspector.simulation->match().clock().match <= 10),
            inspector.options.value(FourCC::from_text("widj")) != 0,
            inspector.options.value(FourCC::from_text("tfil")),
            inspector.options.value(FourCC::from_text("bmap")) ? 2 : inspector.options.value(FourCC::from_text("dmap")) ? 1 : 0);
        if (!args.hide_ui)
            inspector.command(window.input(), window.width(), window.height(), renderer);
        if (!inspector.options.value(FourCC::from_text("shak")))
            inspector.shake = {};
        inspector.camera.centre_offset = inspector.shake.offset(inspector.shake_random);
        if (inspector.view != View::Image)
            renderer.draw(inspector.simulation
                              ? inspector.camera_blend.shown(inspector.camera,
                                                             inspector.simulation->match().clock().match,
                                                             inspector.simulation->match().presentation_blend(),
                                                             tick_view)
                              : inspector.camera,
                          inspector.wireframe, inspector.model_materials,
                          inspector.view == View::Terrain && inspector.show_entities,
                          inspector.view == View::Terrain && inspector.show_environment);
        if (std::exchange(inspector.thumbnail_wanted, false) && inspector.view != View::Image)
            inspector.attempt([&] { inspector.menu_thumbnail = WorldSave::scale_thumbnail(renderer.frame_image()); });
        if (!args.hide_ui && inspector.show_inspector && !inspector.in_cutscene())
            inspector.ui(renderer);
        const bool native_cursor = !args.hide_ui && inspector.draw_pointer(window.input(), renderer);
        window.native_cursor(native_cursor);
        if (native_cursor)
            ImGui::GetIO().ConfigFlags |= ImGuiConfigFlags_NoMouseCursorChange;
        else
            ImGui::GetIO().ConfigFlags &= ~ImGuiConfigFlags_NoMouseCursorChange;
        if (inspector.in_cutscene() && inspector.simulation->triggers().choices.empty())
            ImGui::SetMouseCursor(ImGuiMouseCursor_None);
        inspector.script_overlay(window.width(), window.height(), renderer);
        const bool last_smoke_frame = args.smoke && frames == smoke_frames;
        fps_overlay.draw(dt, inspector.options.value(FourCC::from_text("sfps")) != 0);
        if (inspector.frame_timing && !bench) {
            timing_elapsed += dt;
            if (timing_elapsed >= .5f) {
                timing_elapsed = 0;
                timing_lines = describe_timings(renderer.timings());
            }
            auto& draw = *ImGui::GetForegroundDrawList();
            const float size = ImGui::GetFontSize();
            float y = 8 + size * std::max(ImGui::GetIO().DisplaySize.y / 480.f * .9f, 1.f) * 1.2f;
            for (const auto& line : timing_lines) {
                draw.AddText({9, y + 1}, IM_COL32(0, 0, 0, 200), line.c_str());
                draw.AddText({8, y}, IM_COL32(255, 255, 160, 255), line.c_str());
                y += size * 1.15f;
            }
        }
        renderer.end(vsync() && !bench, last_smoke_frame ? args.output : std::exchange(inspector.pending_shot, {}));
        ++frames;
        if (bench && frames == 3) { // Warm-up done: time from here.
            renderer.timings();
            bench_start = std::chrono::steady_clock::now();
        }
        if (bench && last_smoke_frame) {
            const double seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - bench_start).count();
            const auto t = renderer.timings();
            std::string report = std::format("Benchmark: {} frames, {:.2f} ms per frame ({:.0f} FPS)", *args.bench_frames,
                                             seconds * 1000 / *args.bench_frames, *args.bench_frames / seconds);
            for (const auto& line : describe_timings(t))
                report += "\n  " + line;
            log(report);
            std::printf("%s\n", report.c_str());
        }
        if (!args.smoke)
            limit_frame_rate(next_frame, inspector.options.value(FourCC::from_text("rate")));
        if (last_smoke_frame || inspector.quit_requested)
            break;
    }
    if (!args.smoke)
        settings.write({{"wireframe", inspector.wireframe},
                        {"camera_speed", inspector.camera.speed},
                        {"ambient_sound", inspector.scene_sound},
                        {"ambient_volume", inspector.scene_volume},
                        {"options", inspector.options.save()}});
    log("Inspector exited normally");
    return 0;
}
} // namespace
} // namespace sacrifice
namespace crash_report {
// On a crash (an SEH fault on any thread, an uncaught C++ exception, abort or a CRT check), write the
// faulting thread's symbolized stack to SacrificeModern.log and to a crash-*.txt beside it, with a
// minidump, then exit. The stack names functions and lines when SacrificeModern.pdb sits beside the exe.
std::atomic<bool> busy;
std::string describe(const EXCEPTION_RECORD& record) {
    std::string text = std::format("exception 0x{:08x} at 0x{:x}", static_cast<unsigned>(record.ExceptionCode),
                                   reinterpret_cast<std::uintptr_t>(record.ExceptionAddress));
    if ((record.ExceptionCode == EXCEPTION_ACCESS_VIOLATION || record.ExceptionCode == EXCEPTION_IN_PAGE_ERROR) &&
        record.NumberParameters >= 2)
        text += std::format(" ({} 0x{:x})",
                            record.ExceptionInformation[0] == 0   ? "reading"
                            : record.ExceptionInformation[0] == 8 ? "executing"
                                                                  : "writing",
                            record.ExceptionInformation[1]);
    switch (record.ExceptionCode) {
    case EXCEPTION_ACCESS_VIOLATION: text += " access violation"; break;
    case EXCEPTION_STACK_OVERFLOW: text += " stack overflow"; break;
    case EXCEPTION_INT_DIVIDE_BY_ZERO: text += " integer divide by zero"; break;
    case EXCEPTION_ILLEGAL_INSTRUCTION: text += " illegal instruction"; break;
    case 0xe06d7363: text += " C++ exception"; break;
    default: break;
    }
    return text;
}
std::string stack_text(CONTEXT context) {
    const HANDLE process = GetCurrentProcess(), thread = GetCurrentThread();
    wchar_t exe[MAX_PATH]{};
    GetModuleFileNameW(nullptr, exe, MAX_PATH);
    const auto directory = std::filesystem::path(exe).parent_path().wstring();
    SymSetOptions(SYMOPT_UNDNAME | SYMOPT_DEFERRED_LOADS | SYMOPT_LOAD_LINES);
    SymInitializeW(process, directory.c_str(), TRUE);
    STACKFRAME64 frame{};
    frame.AddrPC = {context.Rip, 0, AddrModeFlat};
    frame.AddrFrame = {context.Rbp, 0, AddrModeFlat};
    frame.AddrStack = {context.Rsp, 0, AddrModeFlat};
    std::string text;
    for (int n = 0; n < 64; ++n) {
        if (!StackWalk64(IMAGE_FILE_MACHINE_AMD64, process, thread, &frame, &context, nullptr,
                         SymFunctionTableAccess64, SymGetModuleBase64, nullptr) ||
            !frame.AddrPC.Offset)
            break;
        const DWORD64 address = frame.AddrPC.Offset;
        std::string module = "?";
        IMAGEHLP_MODULEW64 info{};
        info.SizeOfStruct = sizeof info;
        if (SymGetModuleInfoW64(process, address, &info))
            module = sacrifice::utf8(std::filesystem::path(info.ImageName).filename());
        text += std::format("  #{:02} {}+0x{:x}", n, module, address - info.BaseOfImage);
        alignas(SYMBOL_INFO) char buffer[sizeof(SYMBOL_INFO) + 512]{};
        auto* symbol = reinterpret_cast<SYMBOL_INFO*>(buffer);
        symbol->SizeOfStruct = sizeof(SYMBOL_INFO);
        symbol->MaxNameLen = 511;
        DWORD64 offset{};
        if (SymFromAddr(process, address, &offset, symbol))
            text += std::format(" {}+0x{:x}", symbol->Name, offset);
        IMAGEHLP_LINE64 line{};
        line.SizeOfStruct = sizeof line;
        DWORD column{};
        if (SymGetLineFromAddr64(process, address, &column, &line))
            text += std::format(" [{}:{}]", std::filesystem::path(line.FileName).filename().string(),
                                line.LineNumber);
        text += '\n';
    }
    SymCleanup(process);
    return text;
}
[[noreturn]] void report(EXCEPTION_POINTERS* pointers, const std::string& reason) {
    if (busy.exchange(true))
        for (;;)
            Sleep(INFINITE); // Another thread is already writing the report and will end the process.
    std::filesystem::path directory;
    try {
        directory = sacrifice::application_data();
    } catch (...) {
        directory = std::filesystem::current_path();
    }
    SYSTEMTIME now{};
    GetLocalTime(&now);
    const auto stem = std::format("crash-{:04}{:02}{:02}-{:02}{:02}{:02}", now.wYear, now.wMonth, now.wDay,
                                  now.wHour, now.wMinute, now.wSecond);
    CONTEXT context{};
    if (pointers)
        context = *pointers->ContextRecord;
    else
        RtlCaptureContext(&context);
    std::string text = "CRASH: " + reason + std::format(" (thread {})\n", GetCurrentThreadId());
    text += stack_text(context);
    const auto dump_path = directory / (stem + ".dmp");
    const HANDLE file = CreateFileW(dump_path.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS,
                                    FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file != INVALID_HANDLE_VALUE) {
        MINIDUMP_EXCEPTION_INFORMATION exception{GetCurrentThreadId(), pointers, FALSE};
        MiniDumpWriteDump(GetCurrentProcess(), GetCurrentProcessId(), file,
                          static_cast<MINIDUMP_TYPE>(MiniDumpWithIndirectlyReferencedMemory |
                                                     MiniDumpScanMemory | MiniDumpWithThreadInfo),
                          pointers ? &exception : nullptr, nullptr, nullptr);
        CloseHandle(file);
        text += "Minidump: " + sacrifice::utf8(dump_path) + "\n";
    }
    try {
        std::ofstream(directory / (stem + ".txt")) << text;
        sacrifice::log(text);
    } catch (...) {
    }
    OutputDebugStringA(text.c_str());
    if (std::wstring_view(GetCommandLineW()).find(L"--smoke") == std::wstring_view::npos)
        try {
            MessageBoxW(nullptr,
                        sacrifice::widen("SacrificeModern crashed. The stack trace is in\n" +
                              sacrifice::utf8(directory / (stem + ".txt")) + "\nand at the end of SacrificeModern.log.")
                            .c_str(),
                        L"SacrificeModern", MB_OK | MB_ICONERROR);
        } catch (...) {
        }
    TerminateProcess(GetCurrentProcess(), 3);
    for (;;)
        Sleep(INFINITE);
}
LONG WINAPI unhandled(EXCEPTION_POINTERS* pointers) {
    report(pointers, describe(*pointers->ExceptionRecord));
}
void install() {
    ULONG reserve = 64 * 1024; // Room for the handler after a stack overflow.
    SetThreadStackGuarantee(&reserve);
    SetUnhandledExceptionFilter(unhandled);
    std::set_terminate([] {
        std::string reason = "std::terminate";
        if (const auto current = std::current_exception())
            try {
                std::rethrow_exception(current);
            } catch (const std::exception& e) {
                reason += std::string(" after uncaught exception: ") + e.what();
            } catch (...) {
                reason += " after uncaught non-standard exception";
            }
        report(nullptr, reason);
    });
    std::signal(SIGABRT, [](int) { report(nullptr, "abort()"); });
    _set_abort_behavior(0, _WRITE_ABORT_MSG | _CALL_REPORTFAULT);
    _set_invalid_parameter_handler([](const wchar_t*, const wchar_t*, const wchar_t*, unsigned, std::uintptr_t) {
        report(nullptr, "CRT invalid parameter");
    });
    _set_purecall_handler([] { report(nullptr, "pure virtual call"); });
    // Not a game option: lets the handler be checked without a real crash.
    if (GetEnvironmentVariableW(L"SACRIFICE_TEST_CRASH", nullptr, 0))
        *static_cast<volatile int*>(nullptr) = 0;
}
} // namespace crash_report
int WINAPI wWinMain(HINSTANCE, HINSTANCE, PWSTR, int) {
    crash_report::install();
    auto result = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    try {
        int count{};
        auto** raw = CommandLineToArgvW(GetCommandLineW(), &count);
        if (!raw)
            throw sacrifice::Error("Cannot read command line");
        std::vector<std::string> values;
        for (int i = 1; i < count; ++i)
            values.push_back(sacrifice::utf8(std::filesystem::path(raw[i])));
        LocalFree(raw);
        auto args = sacrifice::Arguments::parse(values);
        auto status = sacrifice::run(args);
        if (SUCCEEDED(result))
            CoUninitialize();
        return status;
    } catch (const std::exception& e) {
        OutputDebugStringA(e.what());
        try {
            sacrifice::log(e.what());
        } catch (...) {
        }
        if (std::wstring_view(GetCommandLineW()).find(L"--smoke") == std::wstring_view::npos)
            MessageBoxW(nullptr, sacrifice::widen(e.what()).c_str(), L"SacrificeModern",
                        MB_OK | MB_ICONERROR);
        if (SUCCEEDED(result))
            CoUninitialize();
        return 1;
    }
}
