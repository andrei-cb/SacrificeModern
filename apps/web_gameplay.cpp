#include "web_gameplay.hpp"
#include "view_camera.hpp"
#include "hud_painter.hpp"
#include "script_overlay.hpp"
#include "renderer_webgpu.hpp"
#include "sacrifice/command_menu.hpp"
#include "sacrifice/fog.hpp"
#include "sacrifice/gameplay_input.hpp"
#include "sacrifice/native_font.hpp"
#include "sacrifice/player_pointer.hpp"
#include "sacrifice/player_visuals.hpp"
#include "sacrifice/scene_match.hpp"
#include "sacrifice/soul_actor.hpp"
#include "sacrifice/spell_combat.hpp"
#include "sacrifice/spell_registry.hpp"
#include "sacrifice/wizard_control.hpp"
#include "web_media.hpp"
#include "webgpu_context.hpp"
#include <emscripten.h>
#include <format>
#include <imgui.h>

void ImGui_ImplWGPU_SetAdditive();
namespace sacrifice {
namespace {
FourCC id(const char* text) {
    return FourCC::from_text(text);
}
ImVec2 at(Vec2 p) {
    return {p.x, p.y};
}
ImTextureID handle(const std::shared_ptr<WebGpuContext::Texture>& image) {
    return image ? static_cast<ImTextureID>(reinterpret_cast<std::uintptr_t>(image->view)) : 0;
}
void additive(ImDrawList& draw) {
    draw.AddCallback([](const ImDrawList*, const ImDrawCmd*) { ImGui_ImplWGPU_SetAdditive(); }, nullptr);
}
struct View : GameplayView {
    View(const Camera& camera, unsigned width, unsigned height)
        : GameplayView(camera.position, camera.yaw, camera.pitch, width, height) {}
};
void mesh(ImDrawList& draw, const Mesh& geometry, ImTextureID texture = 0) {
    draw_hud_mesh(draw, geometry, texture);
}
} // namespace
struct WebGameplay::Impl {
    WebGpuContext& gpu;
    WebSceneRenderer& scene;
    WebMedia& media;
    const ResourceCatalog& catalog;
    SceneMatch& simulation;
    Match& match;
    std::unique_ptr<PlayerControl> player;
    KeyboardDevice keyboard;
    FollowCamera follow = playing_follow();
    unsigned script_zoom_revision{};
    GameOptions options;
    PlayerPointer pointer;
    CommandMenu menu;
    bool third_person{true}, engaged{}, orbit{}, was_cinematic{};
    std::optional<std::uint32_t> released_at; // Match clock at release(): see drive().
    int displayed_book{-1};
    CreatureActor* book_wizard{};
    std::array<bool, 3> allowed{};
    Vec2 box_start{}, box_end{};
    std::uint32_t last_tick{}, menu_target{};
    Vec3 menu_point{};
    struct Texture {
        TextureHeader header;
        std::shared_ptr<WebGpuContext::Texture> image;
    };
    std::map<std::pair<FourCC, FourCC>, Texture> textures;
    std::map<FourCC, std::string> texts;
    struct Font {
        NativeFont font;
        std::shared_ptr<WebGpuContext::Texture> image;
    };
    std::optional<Font> font, number_font, heading_font;
    std::array<GradientBar, 2> styles;
    bool cursor_drawn{};
    bool hover_bracket{}, hover_card{};
    std::uint32_t hover_object{};
    FourCC cursor_icon, hover_context;
    MiniMapControl minimap_control;
    MiniMapTransform minimap_view;
    std::shared_ptr<WebGpuContext::Texture> minimap_image;
    std::uint64_t minimap_revision{~std::uint64_t{0}};
    std::optional<PointerButton> minimap_drag;
    Impl(WebGpuContext& g, WebSceneRenderer& s, const ResourceCatalog& c, WebMedia& audio)
        : gpu(g), scene(s), media(audio), catalog(c), simulation(*s.simulation()), match(simulation.match()) {
        if (match.local_side)
            player = std::make_unique<PlayerControl>(match, *match.local_side);
        pointer.load(catalog);
        menu.load(catalog);
        minimap_control.reset(scene.terrain());
        simulation.triggers().is_selected = [this](const MatchObject& object) {
            return player && !player->selecting_box() && player->selection.contains(&object);
        };
        match.forget_interface = [this](MatchObject& object) {
            if (player)
                player->forget(object);
        };
        match.select_interface = [this](CreatureActor& actor) {
            if (player)
                player->select_spawned(actor);
        };
        for (unsigned i = 0; i < 2; ++i)
            if (const auto source = catalog.find(id("GBAR"), id(i ? "mbar" : "hbar")))
                styles[i] = GradientBar::decode(catalog.read(*source));
        const auto& face = *find_font_face("verdana");
        if (const auto source = catalog.find(id("TXTR"), FourCC{face.texture})) {
            auto image = load_texture(catalog, *source);
            font = Font{NativeFont::build(face, image), gpu.upload(image)};
        }
        const auto& number_face = *find_font_face("bookman");
        if (const auto source = catalog.find(id("TXTR"), FourCC{number_face.texture})) {
            auto image = load_texture(catalog, *source);
            number_font = Font{NativeFont::build(number_face, image), gpu.upload(image)};
        }
        const auto& heading_face = *find_font_face("elgar");
        if (const auto source = catalog.find(id("TXTR"), FourCC{heading_face.texture})) {
            auto image = load_texture(catalog, *source);
            heading_font = Font{NativeFont::build(heading_face, image), gpu.upload(image)};
        }
    }
    ~Impl() {
        release();
        simulation.triggers().is_selected = {};
        match.forget_interface = {};
        match.select_interface = {};
    }
    unsigned locks() const {
        return simulation.triggers().interface_locks;
    }
    void release() {
        engaged = false;
        if (player && player->wizard()) {
            player->wizard()->input_provider = {};
            released_at = match.clock().match;
        }
    }
    const Texture& texture(FourCC name, FourCC tag = id("TXTR")) {
        const auto [entry, added] = textures.try_emplace(std::pair{tag, name});
        if (added && name.value)
            if (const auto source = catalog.find(tag, name)) {
                entry->second.header = decode_texture_header(catalog.read(*source));
                auto image = load_texture(catalog, *source);
                if (tag == id("ICON"))
                    image = texture_frame(image, entry->second.header, 0);
                entry->second.image = gpu.upload(image);
            }
        return entry->second;
    }
    const std::string& text(FourCC name) {
        const auto [entry, added] = texts.try_emplace(name);
        if (added) {
            entry->second = name.text();
            if (const auto source = catalog.find(id("TEXT"), name)) {
                const auto bytes = catalog.read(*source);
                entry->second.assign(reinterpret_cast<const char*>(bytes.data()), bytes.size());
                while (!entry->second.empty() && !entry->second.back())
                    entry->second.pop_back();
            }
        }
        return entry->second;
    }
    void label(ImDrawList& draw, HudRect box, std::string_view string, ImU32 colour = IM_COL32_WHITE,
               float scale = 1.5f, unsigned align = 0) {
        auto paint = hud_painter(draw);
        paint.text(box, string, colour, scale, align);
    }
    void sprite(ImDrawList& draw, FourCC name, unsigned frame, HudRect box, ImU32 colour = IM_COL32_WHITE,
                bool glow = false) {
        auto paint = hud_painter(draw);
        paint.sprite(name, frame, box, colour, glow);
    }
    void icon(ImDrawList& draw, FourCC name, HudRect box, ImU32 colour = IM_COL32_WHITE, bool glow = false) {
        auto paint = hud_painter(draw);
        paint.icon(name, box, colour, glow);
    }
    std::vector<CreatureSpellBook::Entry*> spells() const {
        std::vector<CreatureSpellBook::Entry*> entries;
        if (player && player->wizard()) {
            auto& wizard = *player->wizard();
            for (const auto i : visible_spell_indices(wizard, static_cast<unsigned>(player->spell_tab)))
                entries.push_back(&wizard.wizard_spells[player->spell_tab].entries[i]);
        }
        return entries;
    }
    bool over_hud(Vec2 p, unsigned w, unsigned h) const {
        return player && gameplay_hud_hit(*player, locks(), p, w, h);
    }
    void drive(const InputState& input, bool blocked) {
        if (!player)
            return;
        const bool cinematic = simulation.triggers().cinematic();
        if (input.pressed_codes[67] && !blocked && !cinematic) {
            third_person = !third_person;
            release();
            follow.flags = 0;
        }
        keyboard.poll(blocked || cinematic || !third_person ? std::array<bool, 256>{} : input.scancodes);
        if (blocked || cinematic || !third_person) {
            release();
            return;
        }
        engaged |= keyboard.engaged();
        // As in the desktop frontend: a released (casting) wizard gets one match update without input so
        // 0x46de80 fires its queued cast (event 41) before held keys take over again.
        if (released_at && match.clock().match == *released_at)
            return;
        released_at.reset();
        if (engaged)
            if (auto* wizard = player->wizard())
                wizard->input_provider = [axes = keyboard.axes()] { return axes; };
    }
    MatchObject* pick(const View& view, Vec2 pixel, Vec3& point, bool& ground) {
        return pick_gameplay(match, *player, pointer, scene.terrain(), view, minimap_view, locks(), pixel,
                             point, ground);
    }
    FourCC ability() const {
        return match.spell_registry ? selection_ability(*player, *match.spell_registry) : FourCC{};
    }
    void choose_menu(Vec2 at) {
        switch (select_gameplay_menu(menu, at, *player, match, menu_target, menu_point)) {
        case CommandMenuResult::release_wizard:
            release();
            break;
        case CommandMenuResult::centre_camera:
            third_person = true;
            reset_follow(follow);
            break;
        default:
            break;
        }
    }
    void update_box(const View& view) {
        select_gameplay_box(*player, scene.terrain(), view, minimap_view, false, box_start, box_end);
    }
    void commands(const InputState& input, const View& view) {
        const auto w = view.width, h = view.height;
        player->camera_forward = {view.forward.x, view.forward.z, 0};
        player->camera_right = {view.right.x, view.right.z, 0};
        const auto& codes = input.scancodes;
        player->append = codes[42] || codes[54];
        if (!ImGui::GetIO().WantCaptureKeyboard) {
            if (gameplay_shortcuts(*player, locks(), input.pressed_codes, input.press_modifiers))
                release();
            if (input.pressed_codes[1]) {
                player->cancel_target();
                menu.close();
            }
            if (input.pressed_codes[25] && (input.press_modifiers[25] & 1))
                scene.paused = !scene.paused;
        }
        for (const auto& event : input.pointer_events) {
            const auto button = static_cast<unsigned>(event.button);
            const bool left = event.button == PointerButton::left;
            if (event.kind == PointerEvent::Kind::press)
                allowed[button] = !ImGui::GetIO().WantCaptureMouse;
            if (!allowed[button])
                continue;
            if (event.button == PointerButton::right && player->pending.value) {
                player->cancel_target();
                menu.close();
                allowed[button] = false; // Consume the release/drag too; it must not open a command menu.
                continue;
            }
            const bool map_origin = !(locks() & 4) && MiniMapLayout::make(w, h).contains(event.origin);
            if (event.kind == PointerEvent::Kind::drag_start && map_origin &&
                (event.button == PointerButton::middle || (left && (event.modifiers & 1))))
                minimap_drag = event.button;
            if (minimap_drag == event.button) {
                if (event.kind == PointerEvent::Kind::drag)
                    minimap_control.drag(event.delta.y, h);
                if (event.kind == PointerEvent::Kind::drag_end)
                    minimap_drag.reset();
                continue;
            }
            if (menu.active()) {
                if (event.button == PointerButton::right) {
                    if (event.kind == PointerEvent::Kind::click ||
                        event.kind == PointerEvent::Kind::double_click ||
                        event.kind == PointerEvent::Kind::drag_end)
                        choose_menu(event.point);
                    else if (event.kind == PointerEvent::Kind::drag ||
                             event.kind == PointerEvent::Kind::drag_start)
                        menu.move(event.point);
                }
                continue;
            }
            const bool orbit_event = event.button == PointerButton::middle || (left && (event.modifiers & 1));
            if (orbit_event && !map_origin && !over_hud(event.origin, w, h)) {
                if (event.kind == PointerEvent::Kind::drag_start)
                    orbit = true;
                if (orbit && player->wizard() &&
                    (event.kind == PointerEvent::Kind::drag || event.kind == PointerEvent::Kind::drag_end)) {
                    constexpr float degrees = 180 / 3.14159265f;
                    follow.drag(*player->wizard(), event.delta.x, event.delta.y, w, h,
                                std::atan(view.tan_width) * degrees, std::atan(view.tan_height) * degrees,
                                event.kind == PointerEvent::Kind::drag_end);
                }
                if (event.kind == PointerEvent::Kind::drag_end)
                    orbit = false;
            }
            if (left && !(event.modifiers & 1) && !over_hud(event.origin, w, h) && !player->pending.value) {
                if (event.kind == PointerEvent::Kind::drag_start) {
                    player->append = (event.modifiers & 2) != 0;
                    player->begin_box();
                    box_start = event.origin;
                }
                if (player->selecting_box()) {
                    box_end = event.point;
                    update_box(view);
                    if (event.kind == PointerEvent::Kind::drag_end)
                        player->end_box();
                }
            }
            const bool click =
                event.kind == PointerEvent::Kind::click || event.kind == PointerEvent::Kind::double_click;
            if (!click && !(event.button == PointerButton::right && event.kind == PointerEvent::Kind::press))
                continue;
            const auto result = gameplay_click(
                *player, menu, event, w, h, locks(), options.value(id("xmnu")),
                [&](Vec2 pixel, Vec3& point, bool& ground) { return pick(view, pixel, point, ground); },
                menu_target, menu_point);
            if (result.release)
                release();
            if (result.opened)
                menu_frame = 0;
            if (result.cancelled)
                allowed[button] = false;
        }
    }
    ImGuiHudPainter hud_painter(ImDrawList& draw, bool numbers = false) {
        const auto& face = numbers ? number_font : font;
        return {draw,
                [this](FourCC type, FourCC name) -> HudImage {
                    if (type == id("TXTR") && name == id("#map"))
                        return {handle(minimap_image), {}};
                    const auto& source = texture(name, type);
                    return {handle(source.image), source.header};
                },
                face ? &face->font : nullptr,
                face ? handle(face->image) : ImTextureID{},
                additive,
                true};
    }
    void object_tooltip(const MatchObject& object, FourCC context, Vec2 pixel, unsigned w, unsigned h) {
        auto paint = hud_painter(*ImGui::GetForegroundDrawList());
        paint_object_tooltip(
            paint, {w, h, pixel}, *player, object, context, catalog,
            [this](FourCC name) { return text(name); }, &styles[0]);
    }
    void spell_bar(unsigned w, unsigned h, Vec2 cursor) {
        auto* wizard = player->wizard();
        if (!wizard || !match.spell_registry || (locks() & 2))
            return;
        const bool changed = displayed_book != player->spell_tab || book_wizard != wizard;
        displayed_book = player->spell_tab;
        book_wizard = wizard;
        auto paint = hud_painter(*ImGui::GetBackgroundDrawList());
        auto tooltips = hud_painter(*ImGui::GetForegroundDrawList());
        hover_card |= paint_spell_bar(paint, tooltips, {w, h, cursor}, *wizard, *match.spell_registry,
                                      static_cast<unsigned>(player->spell_tab), changed,
                                      !ImGui::GetIO().WantCaptureMouse && options.value(id("tipo")),
                                      [this](FourCC name) { return text(name); });
    }
    void status(unsigned w, unsigned h, Vec2 cursor) {
        const auto* wizard = player->wizard();
        if (!wizard)
            return;
        auto paint = hud_painter(*ImGui::GetBackgroundDrawList());
        auto numbers = hud_painter(*ImGui::GetBackgroundDrawList(), true);
        auto tooltips = hud_painter(*ImGui::GetForegroundDrawList());
        paint_status_meters(paint, numbers, tooltips, {w, h, cursor}, *wizard, locks(), styles,
                            !ImGui::GetIO().WantCaptureMouse && options.value(id("tipo")),
                            [this](FourCC name) { return text(name); });
    }
    void minimap(unsigned w, unsigned h, float yaw, float tan_width) {
        if ((locks() & 4) || !player->side().fog)
            return;
        const auto& fog = *player->side().fog;
        if (!minimap_image || fog.revision() != minimap_revision) {
            const auto image = terrain_minimap(scene.terrain(), scene.materials(), fog.cells());
            if (minimap_image)
                gpu.update(*minimap_image, image);
            else
                minimap_image = gpu.upload(image);
            minimap_revision = fog.revision();
        }
        auto paint = hud_painter(*ImGui::GetBackgroundDrawList());
        paint_minimap(paint, {w, h, {}}, match, *player, scene.terrain(), minimap_view, yaw, tan_width);
    }
    void selection(const View& view, Vec2 cursor) {
        if (locks() & 1)
            return;
        auto& draw = *ImGui::GetBackgroundDrawList();
        auto paint = hud_painter(draw);
        auto tooltips = hud_painter(*ImGui::GetForegroundDrawList());
        if (match.spell_registry)
            hover_card |= paint_selection_panel(paint, tooltips, {view.width, view.height, cursor}, *player,
                                                *match.spell_registry, ability(),
                                                !ImGui::GetIO().WantCaptureMouse && options.value(id("tipo")),
                                                [this](FourCC name) { return text(name); });
        if (player->selecting_box()) {
            const auto rect = SelectionRectangle::make(box_start, box_end, view.width, view.height);
            draw.AddRect(at(rect.minimum), at(rect.maximum), IM_COL32_WHITE);
        }
        for (const auto& object : match.objects()) {
            auto* actor = dynamic_cast<CreatureActor*>(object.get());
            if (!actor || !unit_hud_visible(*player, *actor))
                continue;
            Vec3 anchor = actor->motion.position;
            if (!actor->write_reference_point(anchor, 0, false))
                continue;
            const auto p = view.project(anchor);
            if (!p)
                continue;
            paint_unit_hud(paint, *actor, *p, view.depth(anchor), view.width,
                           actor->allegiance == &player->side() && player->selection.contains(actor),
                           actor->allegiance != &player->side());
        }
    }
    unsigned menu_frame{};
    void cross() {
        auto paint = hud_painter(*ImGui::GetForegroundDrawList());
        paint_command_menu(paint, menu, menu_frame++);
    }
    void cursor(const View& view, const InputState& input, float seconds) {
        cursor_drawn = false;
        if (!input.pointer_inside || ImGui::GetIO().WantCaptureMouse) {
            pointer.reset();
            return;
        }
        const Vec2 pixel{input.cursor_x, input.cursor_y};
        cursor_icon = id("Cnor");
        PointerFeedback feedback;
        feedback.icon = cursor_icon;
        if (!over_hud(pixel, view.width, view.height)) {
            Vec3 point{};
            bool ground{};
            auto* target = pick(view, pixel, point, ground);
            feedback = pointer_feedback(pointer, *player, match, target, point, ground, {}, menu.active(),
                                        orbit, false, seconds * 60);
            cursor_icon = feedback.icon;
            hover_context = feedback.context;
            hover_object = target ? target->reference() : 0;
            if (feedback.bracket)
                if (const auto geometry = object_hover_bracket(
                        *target, {view.eye, view.right, view.up, view.forward, view.tan_width,
                                  view.tan_height, view.width, view.height})) {
                    mesh(*ImGui::GetBackgroundDrawList(), *geometry);
                    hover_bracket = true;
                }
            if (feedback.tooltip && options.value(id("tipo"))) {
                object_tooltip(*target, feedback.context, pixel, view.width, view.height);
                hover_card = true;
            }
        } else
            pointer.reset();
        auto paint = hud_painter(*ImGui::GetForegroundDrawList());
        cursor_drawn = paint_pointer(paint, feedback, pixel);
    }
    void overlay(unsigned w, unsigned h, float dt) {
        auto& draw = *ImGui::GetForegroundDrawList();
        const auto fonts = [&](std::string_view name) -> HudFontImage {
            const auto& face = name == "elgar" ? heading_font : number_font;
            return face ? HudFontImage{&face->font, handle(face->image)} : HudFontImage{};
        };
        ImGuiMenuPainter paint(draw, 1, [](FourCC) { return ImTextureID{}; }, fonts, additive, true);
        paint_script_overlay(draw, paint, simulation.triggers(), w, h, fonts,
                             [&](const Triggers::Dialogue& line) {
                                 return media.portrait(gpu, line.god, line.emotion, line.id, w, h, dt);
                             }, text(id("GMTu")));
    }
    void frame(const InputState& input, Camera& camera, unsigned w, unsigned h, float seconds, bool blocked) {
        const int ticks = static_cast<int>(scene.ticks() - last_tick);
        last_tick = scene.ticks();
        cursor_drawn = false;
        cursor_icon = {};
        hover_context = {};
        hover_object = 0;
        hover_bracket = hover_card = false;
        pointer.advance(seconds);
        if (!player) {
            if (!blocked)
                camera.update(input, seconds);
            if (!blocked)
                overlay(w, h, seconds);
            return;
        }
        player->tick(ticks);
        menu.tick(ticks);
        const bool cinematic = simulation.triggers().cinematic();
        if (blocked || input.pointer_cancelled || cinematic) {
            menu.close();
            player->cancel_box();
            allowed = {};
            orbit = false;
            follow.flags &= ~4u;
            minimap_drag.reset();
            pointer.reset();
        }
        if (cinematic) {
            cinematic_view(simulation.triggers(), camera, follow);
        } else if (third_person && player->wizard()) {
            const Vec2 pixel{input.cursor_x, input.cursor_y};
            if (!blocked && !ImGui::GetIO().WantCaptureMouse && !over_hud(pixel, w, h) &&
                (options.value(id("ctrl")) & 4) && input.wheel)
                follow.wheel(input.wheel);
            if (!blocked && !ImGui::GetIO().WantCaptureMouse && !over_hud(pixel, w, h))
                follow.edge_scroll(*player->wizard(), pixel, w, h, {input.mouse_x, input.mouse_y}, ticks,
                                   static_cast<unsigned>(options.value(id("ctrl"))), input.pointer_inside);
            follow_view(follow, *player->wizard(), scene.terrain(), simulation.triggers(), script_zoom_revision,
                        ticks, keyboard.held(), camera);
        } else if (!blocked && !ImGui::GetIO().WantCaptureMouse && !ImGui::GetIO().WantCaptureKeyboard)
            camera.update(input, seconds);
        was_cinematic = cinematic;
        const View view(camera, w, h);
        minimap_view = minimap_control.transform(
            scene.terrain(), {camera.position.x, camera.position.z, camera.position.y}, camera.yaw);
        if (!cinematic && third_person) {
            if (!blocked)
                commands(input, view);
            scene.order_markers(player->markers);
            if (!blocked) {
                spell_bar(w, h, {input.cursor_x, input.cursor_y});
                status(w, h, {input.cursor_x, input.cursor_y});
                selection(view, {input.cursor_x, input.cursor_y});
                minimap(w, h, camera.yaw, view.tan_width);
                cross();
                cursor(view, input, seconds);
            }
        } else
            scene.order_markers({});
        if (!blocked)
            overlay(w, h, seconds);
    }
};
WebGameplay::WebGameplay(WebGpuContext& gpu, WebSceneRenderer& scene, const ResourceCatalog& catalog,
                         WebMedia& media)
    : impl_(std::make_unique<Impl>(gpu, scene, catalog, media)) {}
WebGameplay::~WebGameplay() = default;
void WebGameplay::drive(const InputState& input, bool blocked) {
    impl_->drive(input, blocked);
}
void WebGameplay::frame(const InputState& input, Camera& camera, unsigned w, unsigned h, float seconds,
                        bool blocked) {
    impl_->frame(input, camera, w, h, seconds, blocked);
    const auto cursor = impl_->cursor_icon.text();
    const auto context = impl_->hover_context.text();
    EM_ASM(
        {
            const state = Module['pointerStats'] || (Module['pointerStats'] = {});
            state.object = $0;
            state.bracket = !!$1;
            state.tooltip = !!$2;
            state.cursor = UTF8ToString($3);
            state.context = UTF8ToString($4);
        },
        impl_->hover_object, impl_->hover_bracket, impl_->hover_card, cursor.c_str(), context.c_str());
}
void WebGameplay::place_view(Camera& camera) {
    auto& self = *impl_;
    if (self.player && self.player->wizard() && !self.simulation.triggers().cinematic())
        follow_view(self.follow, *self.player->wizard(), self.scene.terrain(), self.simulation.triggers(),
                    self.script_zoom_revision, 0, 0, camera);
}
void WebGameplay::options(const GameOptions& options) {
    impl_->options = options;
    impl_->follow.invert_x = (options.value(FourCC::from_text("ctrl")) & 8) != 0;
    impl_->follow.invert_y = (options.value(FourCC::from_text("ctrl")) & 16) != 0;
}
bool WebGameplay::following() const {
    return impl_->third_person && impl_->player && impl_->player->wizard();
}
bool WebGameplay::cursor_drawn() const {
    return impl_->cursor_drawn;
}
PlayerControl* WebGameplay::player() {
    return impl_->player.get();
}
} // namespace sacrifice
