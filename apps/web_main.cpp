#include "renderer_webgpu.hpp"
#include "sacrifice/core.hpp"
#include "sacrifice/menu_form.hpp"
#include "sacrifice/platform.hpp"
#include "sacrifice/player.hpp"
#include "sacrifice/scene_match.hpp"
#include "sacrifice/spell_registry.hpp"
#include "sacrifice/storage.hpp"
#include "sacrifice/wizard_control.hpp"
#include "sacrifice/world_save.hpp"
#include "web_frontend.hpp"
#include "web_gameplay.hpp"
#include "view_camera.hpp"
#include "fps_overlay.hpp"
#include "web_media.hpp"
#include "webgpu_context.hpp"
#include <emscripten.h>
#include <emscripten/html5.h>
#include <format>
#include <imgui.h>
#include <imgui_impl_sdl2.h>
#include <imgui_impl_wgpu.h>

namespace sacrifice {
namespace {
void show_error(const std::string& message) {
    EM_ASM({ Module['showError'](UTF8ToString($0)); }, message.c_str());
}
struct WebApp {
    Window window;
    WebGpuContext gpu;
    ResourceCatalog catalog;
    std::unique_ptr<WebSceneRenderer> scene;
    std::unique_ptr<WebGameplay> gameplay;
    std::unique_ptr<WebMedia> media;
    std::unique_ptr<WebFrontend> frontend;
    bool menu_audio{true};
    Camera camera, home;
    CameraBlend camera_blend; // Drawn view between ticks.
    FpsOverlay fps_overlay;
    std::filesystem::path requested_map;
    std::vector<std::filesystem::path> maps;
    std::string map_name, map_error;
    double last_frame{}, next_frame{};
    int map_choice{};
    int profile_god{1};
    FourCC preview_spell;
    bool cast_self{}, step_requested{};
    std::string cast_status;
    DocumentStore preferences{application_data() / "web-preview.json", "sacrifice.web-preview", 1};
    struct Entry {
        ResourceLocation location;
        std::string name;
    };
    struct Artwork {
        HudRect bounds;
        std::shared_ptr<WebGpuContext::Texture> texture;
    };
    std::vector<Entry> textures;
    std::vector<Artwork> artwork;
    std::shared_ptr<WebGpuContext::Texture> preview;
    Image preview_image;
    std::string preview_error;
    ImGuiTextFilter filter;
    int selected{-1};
    bool initialized{}, inspector{};
    explicit WebApp(const Arguments& args) : catalog(discover_game_dir(args.game_dir)) {
        if (const auto settings = preferences.read()) {
            inspector = settings->value("inspector", false);
        }
        catalog.mount_game();
        const auto root = discover_game_dir(args.game_dir);
        requested_map = args.map.empty() ? args.file : args.map;
        if (!requested_map.empty() && requested_map.is_relative())
            requested_map = root / requested_map;
        for (const auto& file : std::filesystem::directory_iterator(root / "maps"))
            if (file.path().extension() == ".scp")
                maps.push_back(file.path());
        std::ranges::sort(maps);
        for (std::size_t a = 0; a < catalog.archives().size(); ++a) {
            const auto& archive = catalog.archives()[a];
            for (std::size_t r = 0; r < archive.records().size(); ++r) {
                const auto& record = archive.records()[r];
                if (!record.folder() && record.tag == FourCC::from_text("TXTR"))
                    textures.push_back({{a, r}, utf8(game_path(archive.label()).filename()) + record.path});
            }
        }
        // Request async callbacks only after all fallible asset initialization has completed.
        gpu.start();
    }
    void initialize_ui() {
        IMGUI_CHECKVERSION();
        ImGui::CreateContext();
        ImGui::GetIO().IniFilename = nullptr;
        ImGui::StyleColorsDark();
        const float dpi = static_cast<float>(emscripten_get_device_pixel_ratio());
        ImGui::GetStyle().ScaleAllSizes(dpi);
        ImGui::GetStyle().FontScaleDpi = dpi;
        if (!ImGui_ImplSDL2_InitForOther(window.handle()))
            throw Error("SDL ImGui initialization failed");
        ImGui_ImplWGPU_InitInfo info;
        info.Device = gpu.device();
        info.RenderTargetFormat = gpu.format();
        if (!ImGui_ImplWGPU_Init(&info))
            throw Error("WebGPU ImGui initialization failed");
        if (const auto source = catalog.find(FourCC::from_text("FORM"), FourCC::from_text("titl"))) {
            const auto form = MenuForm::decode(catalog.read(*source));
            const auto content = form.content(form.bounds(640, 480));
            for (const auto& widget : form.widgets)
                if (widget.kind == 3 && widget.texture.value)
                    if (const auto image = catalog.find(FourCC::from_text("TXTR"), widget.texture)) {
                        const auto header = decode_texture_header(catalog.read(*image));
                        artwork.push_back(
                            {form.bounds(widget, content),
                             gpu.upload(texture_frame(load_texture(catalog, *image), header, 0))});
                    }
        }
        initialized = true;
        media = std::make_unique<WebMedia>(catalog, discover_game_dir({}));
        frontend = std::make_unique<WebFrontend>(gpu, catalog, discover_game_dir({}));
        if (!requested_map.empty()) {
            load_map(requested_map);
            inspector = !scene;
        } else
            inspector = false;
        last_frame = emscripten_get_now();
        EM_ASM({ Module['appReady'](); });
    }
    void load_map(const std::filesystem::path& path) {
        load_map(WebFrontend::Launch{path, {}, false});
    }
    void load_map(const WebFrontend::Launch& request) {
        const auto& path = request.path;
        try {
            catalog.mount(path);
            const auto map = Archive::open(path);
            auto next = std::make_unique<WebSceneRenderer>(gpu);
            next->models = true;
            next->load(catalog, map, request.slots, request.campaign,
                       request.saved ? std::optional(request.saved->setup.at("seed").get<std::uint32_t>())
                                     : std::nullopt,
                       request.character ? &*request.character : nullptr);
            camera = next->initial_camera();
            home = camera;
            // Controllers retain match objects; destroy them before their scene.
            auto controls = std::make_unique<WebGameplay>(gpu, *next, catalog, *media);
            controls->place_view(camera);
            frontend->detach();
            try {
                frontend->loaded(*next->simulation(), request, controls->player());
            } catch (...) {
                frontend->detach();
                throw;
            }
            if (request.saved && request.saved->setup.contains("camera") &&
                !request.saved->setup.at("camera").is_null()) {
                const auto& c = request.saved->setup.at("camera");
                camera.position = {c.at(0).at(0).get<float>(), c.at(0).at(1).get<float>(),
                                   c.at(0).at(2).get<float>()};
                camera.yaw = c.at(1).get<float>();
                camera.pitch = c.at(2).get<float>();
            }
            media->bind(next->simulation());
            gameplay.reset();
            scene = std::move(next);
            gameplay = std::move(controls);
            seed_script_camera(scene->simulation()->triggers(), camera);
            preview_spell = {};
            cast_status.clear();
            map_name = utf8(path.filename());
            map_error.clear();
            EM_ASM({ document.body.dataset.scene = 'ready'; });
        } catch (const std::exception& error) {
            map_error = error.what();
            frontend->failed(map_error);
            log("Web map load failed: " + map_error);
            EM_ASM(
                {
                    document.body.dataset.scene = 'error';
                    Module['sceneError'] = UTF8ToString($0);
                    console.warn(Module['sceneError']);
                },
                map_error.c_str());
        }
    }
    void save_preferences() {
        preferences.write({{"inspector", inspector}});
    }
    void select(int index) {
        selected = index;
        preview.reset();
        preview_image = {};
        preview_error.clear();
        try {
            preview_image = load_texture(catalog, textures.at(index).location);
            preview = gpu.upload(preview_image);
        } catch (const std::exception& error) {
            preview_error = error.what();
        }
    }
    static ImTextureID texture_id(const std::shared_ptr<WebGpuContext::Texture>& texture) {
        return static_cast<ImTextureID>(reinterpret_cast<std::uintptr_t>(texture->view));
    }
    void scene_controls() {
        auto* simulation = scene ? scene->simulation() : nullptr;
        auto* match = simulation ? &simulation->match() : nullptr;
        auto* wizard =
            match && match->local_side ? dynamic_cast<CreatureActor*>(match->local_side->wizard()) : nullptr;
        ImGui::SeparatorText("Wizard shortcuts");
        ImGui::BeginDisabled(!wizard);
        ImGui::Combo("Wizard god", &profile_god, "None\0Persephone\0Pyro\0James\0Stratos\0Charnel\0");
        if (ImGui::Button("Max level + apply god")) {
            configure_debug_wizard(*match, *wizard, profile_god, 14);
            if (gameplay && gameplay->player())
                gameplay->player()->cancel_target();
            preview_spell = {};
            cast_status.clear();
        }
        ImGui::SameLine();
        if (ImGui::Button("Focus wizard")) {
            const auto& pos = wizard->motion.position;
            camera.position = {pos.x - std::sin(camera.yaw) * 100, pos.z + 40,
                               pos.y - std::cos(camera.yaw) * 100};
            camera.pitch = -.2f;
        }
        if (wizard) {
            ImGui::Text("Level %d | Mana %d/%d", wizard->progression.level, wizard->mana,
                        wizard->maximum_mana);
            if (ImGui::BeginCombo("Preview spell", preview_spell.value ? preview_spell.text().c_str()
                                                                       : "Choose a learned spell")) {
                for (const auto& book : wizard->wizard_spells)
                    for (const auto& entry : book.entries)
                        if (ImGui::Selectable(entry.id.text().c_str(), entry.id == preview_spell))
                            preview_spell = entry.id;
                ImGui::EndCombo();
            }
            ImGui::Checkbox("Target wizard", &cast_self);
            ImGui::SameLine();
            ImGui::BeginDisabled(!preview_spell.value);
            if (ImGui::Button("Cast preview spell")) {
                auto point = wizard->motion.position;
                if (!cast_self) {
                    point.x += std::sin(camera.yaw) * 40;
                    point.y += std::cos(camera.yaw) * 40;
                    point.z = terrain_height(scene->terrain(), point.x, point.y);
                }
                const bool accepted =
                    wizard->issue_spell_order(preview_spell, cast_self ? wizard : nullptr, point);
                cast_status =
                    accepted ? "Cast order queued." : "Cast rejected by the spell's target/cost rules.";
            }
            ImGui::EndDisabled();
            if (!cast_status.empty())
                ImGui::TextUnformatted(cast_status.c_str());
        } else
            ImGui::TextDisabled("Open Trial to create a local wizard for spell previews.");
        ImGui::EndDisabled();
        if (!scene)
            return;
        ImGui::SeparatorText("Live scene");
        ImGui::Checkbox("Pause simulation", &scene->paused);
        ImGui::SameLine();
        if (ImGui::Button("Step one tick"))
            step_requested = true;
        ImGui::SameLine();
        ImGui::Checkbox("Effects", &scene->effects);
        ImGui::Text("Tick %u | %zu animated objects | %zu effect triangles", scene->ticks(),
                    scene->animated_count(), scene->particle_count());
        if (simulation && !simulation->triggers().dialogue.empty()) {
            const auto& line = simulation->triggers().dialogue.front();
            ImGui::TextWrapped("%s", line.caption.c_str());
            if (ImGui::Button("Advance dialogue"))
                simulation->triggers().dismiss_dialogue(true);
        }
    }
    void frame() {
        // Port frame cap ('rate', Max Framerate) over the browser's display-rate callbacks: a callback more
        // than 1 ms before the next due time is skipped, leaving its events queued for the next one.
        if (frontend) {
            const double now = emscripten_get_now(),
                         period = 1000. / std::max(frontend->options().value(FourCC::from_text("rate")), 1);
            if (now < next_frame - 1)
                return;
            next_frame = next_frame + period < now ? now + period : next_frame + period;
        }
        if (!window.pump()) {
            emscripten_cancel_main_loop();
            return;
        }
        if (!gpu.error().empty())
            throw Error(gpu.error());
        if (!gpu.ready())
            return;
        if (!initialized)
            initialize_ui();
        if (window.input().pressed_codes[68]) {
            inspector = !inspector;
            save_preferences();
        }
        if (auto request = frontend->take_launch())
            load_map(*request);
        ImGui_ImplWGPU_NewFrame();
        ImGui_ImplSDL2_NewFrame();
        // Match the WebGPU attachment and game/input coordinates, all in physical pixels.
        // SDL's drawable size can lag browser zoom/resize events.
        ImGui::GetIO().DisplaySize = {static_cast<float>(window.width()),
                                      static_cast<float>(window.height())};
        ImGui::GetIO().DisplayFramebufferScale = {1, 1};
        ImGui::NewFrame();
        const double now = emscripten_get_now();
        const float dt = static_cast<float>(std::clamp((now - last_frame) / 1000., 0., .1));
        last_frame = now;
        const auto& options = frontend->options();
        window.swap_buttons(options.value(FourCC::from_text("swap")) != 0);
        window.borderless(options.value(FourCC::from_text("wmod")) != 0);
        media->options(options);
        if (scene)
            scene->options(options, dt > 0 ? 1 / dt : 0);
        if (gameplay)
            gameplay->options(options);
        frontend->update(window.input(), gameplay ? gameplay->player() : nullptr, inspector, dt);
        const bool fast = frontend->fast_forward(window.input());
        if (gameplay) {
            const bool blocked = inspector || frontend->blocking();
            gameplay->drive(window.input(), blocked || ImGui::GetIO().WantCaptureKeyboard);
            window.free_camera = !blocked && !gameplay->following();
            auto& simulation = *scene->simulation();
            seed_script_camera(simulation.triggers(), camera);
            simulation.match().direct_control_target =
                !simulation.triggers().cinematic() && gameplay->following() ? gameplay->player()->wizard()
                                                                            : nullptr;
            scene->advance(dt * (fast ? 8 : 1), step_requested);
            step_requested = false;
            if (!frontend->front_end())
                gameplay->frame(window.input(), camera, window.width(), window.height(), dt, blocked);
            window.native_cursor(!frontend->blocking() && gameplay->cursor_drawn());
        }
        const bool front = frontend->front_end();
        if (front && !menu_audio)
            media->reset();
        menu_audio = front;
        media->update(!front && scene ? scene->simulation() : nullptr,
                      !front && gameplay ? gameplay->player() : nullptr, camera, dt, fast);
        const auto w = static_cast<float>(window.width()), h = static_cast<float>(window.height());
        const float scale = std::min(w / 640, h / 480), x = (w - 640 * scale) / 2, y = (h - 480 * scale) / 2;
        if (!scene)
            for (const auto& image : artwork) {
                const auto& r = image.bounds;
                ImGui::GetBackgroundDrawList()->AddImage(
                    texture_id(image.texture), {x + r.origin.x * scale, y + r.origin.y * scale},
                    {x + (r.origin.x + r.size.x) * scale, y + (r.origin.y + r.size.y) * scale});
            }
        frontend->view(camera.position, camera.yaw, camera.pitch);
        frontend->draw(window.width(), window.height(), inspector, *media);
        if (inspector) {
            ImGui::SetNextWindowPos({w * .04f, h * .05f}, ImGuiCond_FirstUseEver);
            ImGui::SetNextWindowSize({w * .92f, h * .9f}, ImGuiCond_FirstUseEver);
            const bool was_open = inspector;
            if (ImGui::Begin("SacrificeModern - Web asset inspector", &inspector)) {
                scene_controls();
                media->controls();
                ImGui::Text("%zu resource packs Â· %zu textures", catalog.archives().size(), textures.size());
                ImGui::TextDisabled(
                    "WASD: wizard | Tab: spell books | Middle/Ctrl-drag: orbit | Wheel: zoom | "
                    "F9: free camera | F10: inspector");
                if (ImGui::Button("Open Trial map")) {
                    load_map(discover_game_dir({}) / "maps/(2) Trial.scp");
                }
                ImGui::SameLine();
                if (ImGui::BeginCombo("Map",
                                      maps.empty() ? "No maps" : utf8(maps[map_choice].filename()).c_str())) {
                    for (std::size_t i = 0; i < maps.size(); ++i)
                        if (ImGui::Selectable(utf8(maps[i].filename()).c_str(),
                                              map_choice == static_cast<int>(i)))
                            map_choice = static_cast<int>(i);
                    ImGui::EndCombo();
                }
                if (!maps.empty()) {
                    ImGui::SameLine();
                    if (ImGui::Button("Open"))
                        load_map(maps[map_choice]);
                }
                if (!map_error.empty())
                    ImGui::TextWrapped("%s", map_error.c_str());
                if (scene) {
                    ImGui::Text("%s - %zu models, %zu triangles, %zu unresolved assets", map_name.c_str(),
                                scene->model_count(), scene->triangle_count(), scene->unresolved_count());
                    ImGui::Checkbox("Objects", &scene->models);
                    ImGui::SameLine();
                    ImGui::Checkbox("Plants", &scene->widgets);
                    ImGui::SameLine();
                    ImGui::Checkbox("Sky and fog", &scene->environment);
                    ImGui::SameLine();
                    if (ImGui::Button("Reset camera"))
                        camera = home;
                    ImGui::TextDisabled("Esc: game menu | Hold Tab during cinematics: fast forward");
                }
                filter.Draw("Filter textures");
                if (ImGui::BeginChild("textures", {ImGui::GetContentRegionAvail().x * .42f, 0},
                                      ImGuiChildFlags_Borders)) {
                    // Build the filtered index list, then clip the rows to avoid submitting thousands of
                    // labels.
                    std::vector<int> rows;
                    for (std::size_t i = 0; i < textures.size(); ++i)
                        if (filter.PassFilter(textures[i].name.c_str()))
                            rows.push_back(static_cast<int>(i));
                    ImGuiListClipper clip;
                    clip.Begin(static_cast<int>(rows.size()));
                    while (clip.Step())
                        for (int row = clip.DisplayStart; row < clip.DisplayEnd; ++row) {
                            const auto i = rows[row];
                            ImGui::PushID(i);
                            if (ImGui::Selectable(textures[i].name.c_str(), selected == i))
                                select(i);
                            ImGui::PopID();
                        }
                }
                ImGui::EndChild();
                ImGui::SameLine();
                ImGui::BeginChild("preview", {}, ImGuiChildFlags_Borders);
                if (preview) {
                    ImGui::Text("%u Ã— %u", preview_image.width, preview_image.height);
                    const auto available = ImGui::GetContentRegionAvail();
                    const float size =
                        std::min(available.x / preview_image.width, available.y / preview_image.height);
                    if (size > 0)
                        ImGui::Image(texture_id(preview),
                                     {preview_image.width * size, preview_image.height * size});
                } else if (!preview_error.empty())
                    ImGui::TextWrapped("%s", preview_error.c_str());
                else
                    ImGui::TextUnformatted("Choose a texture to preview it.");
                ImGui::EndChild();
            }
            ImGui::End();
            if (was_open != inspector)
                save_preferences();
        }
        fps_overlay.draw(dt, frontend->options().value(FourCC::from_text("sfps")) != 0);
        ImGui::Render();
        if (scene) {
            auto* simulation = scene->simulation();
            const bool tick_view = simulation && gameplay && !inspector && !frontend->front_end() &&
                                   (gameplay->following() || simulation->triggers().cinematic());
            scene->prepare(simulation ? camera_blend.shown(camera, simulation->match().clock().match,
                                                           simulation->match().presentation_blend(), tick_view)
                                      : camera,
                           window.width(), window.height());
            EM_ASM(
                {
                    const stats = Module['sceneStats'] || (Module['sceneStats'] = {});
                    stats.ticks = $0;
                    stats.animated = $1;
                    stats.effectTriangles = $2;
                },
                scene->ticks(), scene->animated_count(), scene->particle_count());
            if (const auto* player = gameplay ? gameplay->player() : nullptr)
                if (const auto* wizard = player->wizard()) {
                    const auto& pos = wizard->motion.position;
                    EM_ASM(
                        {
                            const stats = Module['sceneStats'];
                            const wizard = stats.wizard || (stats.wizard = {});
                            wizard.x = $0;
                            wizard.y = $1;
                            wizard.z = $2;
                            wizard.mana = $3;
                            wizard.souls = $4;
                            stats.selected = $5;
                            stats.book = $6;
                            stats.targeting = !!$7;
                            stats.following = !!$8;
                            stats.cinematic = !!$9;
                            wizard.maximumMana = $10;
                            wizard.level = $11;
                            wizard.id = UTF8ToString($12);
                        },
                        pos.x, pos.y, pos.z, wizard->mana, wizard->soul_count, player->selection.size(),
                        player->spell_tab, player->pending.value, gameplay->following(),
                        scene->simulation()->triggers().cinematic(), wizard->maximum_mana,
                        wizard->progression.level, wizard->definition().id.text().c_str());
                }
            gpu.draw(
                ImGui::GetDrawData(), window.width(), window.height(),
                [&](WGPUCommandEncoder encoder, WGPUTextureView target) { scene->render(encoder, target); });
        } else
            gpu.draw(ImGui::GetDrawData(), window.width(), window.height());
    }
};
} // namespace
} // namespace sacrifice
int main(int argc, char** argv) {
    try {
        std::vector<std::string> arguments(argv + 1, argv + argc);
        // The app owns async adapter/device callbacks and lives until the browser page closes.
        auto* app = new sacrifice::WebApp(sacrifice::Arguments::parse(arguments));
        emscripten_set_main_loop_arg(
            [](void* user) {
                try {
                    static_cast<sacrifice::WebApp*>(user)->frame();
                } catch (const std::exception& error) {
                    sacrifice::show_error(error.what());
                    emscripten_cancel_main_loop();
                }
            },
            app, 0, false);
        return 0;
    } catch (const std::exception& error) {
        sacrifice::show_error(error.what());
        return 1;
    }
}
