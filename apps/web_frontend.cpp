#include "web_frontend.hpp"
#include "hud_painter.hpp"
#include "sacrifice/campaign.hpp"
#include "sacrifice/campaign_session.hpp"
#include "sacrifice/character.hpp"
#include "sacrifice/menu_form.hpp"
#include "sacrifice/native_font.hpp"
#include "sacrifice/player.hpp"
#include "sacrifice/skirmish_lobby.hpp"
#include "sacrifice/spell_registry.hpp"
#include "sacrifice/title_backdrop.hpp"
#include "sacrifice/storage.hpp"
#include "sacrifice/wizard_preview.hpp"
#include "sacrifice/loading_screen.hpp"
#include "sacrifice/save_pages.hpp"
#include "sacrifice/world_save.hpp"
#include "web_media.hpp"
#include "webgpu_context.hpp"
#include <cctype>
#include <emscripten.h>
#include <format>
#include <imgui.h>
#include <utility>

void ImGui_ImplWGPU_SetAdditive();
namespace sacrifice {
namespace {
FourCC id(const char* name) {
    return FourCC::from_text(name);
}
ImTextureID handle(const std::shared_ptr<WebGpuContext::Texture>& image) {
    return image ? static_cast<ImTextureID>(reinterpret_cast<std::uintptr_t>(image->view)) : 0;
}
} // namespace
struct WebFrontend::Impl {
    WebGpuContext& gpu;
    const ResourceCatalog& catalog;
    std::filesystem::path root;
    WizardPreview wizard_preview{catalog};
    std::shared_ptr<WebGpuContext::Texture> wizard_image;
    CharacterRoster roster{application_data() / "saves/Characters"};
    DocumentStore preferences{application_data() / "web-frontend.json", "sacrifice.web-frontend", 1};
    std::optional<CampaignHost> host;
    std::optional<Character> profile;
    CampaignSession campaign_state;
    MissionEndTransition end_transition;
    std::vector<Character> characters;
    std::vector<WizardChoice> wizards;
    int character_row{}, wizard_row{}, map_row{}, browsed_row{}, summary_page{}, history_row{};
    std::array<char, 32> name{};
    std::array<std::uint8_t, 20> new_gods{};
    std::optional<std::string> edited;
    std::string game_type{"sacrifice"}, error;
    SceneMatch* scene{};
    PlayerControl* player{};
    std::string save_notice;
    nlohmann::json saved_view;
    // FORM sgam / lgam and their dialogs, shared with the desktop build.
    SavePages saves{SavePages::Host{
        root, [this] { return can_save_world(); }, [this](const std::string& file) { save_world(file); },
        [this](const std::string& file) { load_world(file); },
        [this](FourCC next_page, bool) { open(next_page); }, [this](FourCC record) { return text(record); },
        [](const std::string&) {}}};
    std::shared_ptr<WebGpuContext::Texture> save_thumbnail;
    unsigned save_thumbnail_revision{};
    Launch current;
    std::optional<Launch> launch, next;
    // FORM:igam: a launch waits one presented frame behind the loading page, then loads.
    std::shared_ptr<WebGpuContext::Texture> loading_image;
    std::string loading_description;
    std::filesystem::path loading_path;
    bool loading_shown{};
    FourCC page{id("titl")}, return_page{id("titl")};
    bool finish_requested{}, panel_backdrop{};
    int page_frame{}, next_frames{};
    MenuKeys menu_keys;
    float scale{1};
    GameOptions options;
    FourCC options_page{id("gopt")};
    CrtRandom random;
    SkirmishLobby lobby;
    std::vector<std::filesystem::path> maps;
    std::map<FourCC, MenuForm> forms;
    std::map<int, std::shared_ptr<WebGpuContext::Texture>> previews;
    std::optional<SpellRegistry> registry;
    std::map<FourCC, std::string> texts;
    struct Texture {
        TextureHeader header;
        std::shared_ptr<WebGpuContext::Texture> image;
    };
    std::map<std::pair<FourCC, FourCC>, Texture> textures;
    TitleBackdrop backdrop;
    struct Font {
        NativeFont font;
        std::shared_ptr<WebGpuContext::Texture> image;
    };
    std::map<std::string, Font> fonts;
    Impl(WebGpuContext& device, const ResourceCatalog& assets, std::filesystem::path directory)
        : gpu(device), catalog(assets), root(std::move(directory)) {
        if (const auto saved = preferences.read(); saved && saved->contains("options"))
            options.load((*saved)["options"]);
        else if (const auto previous =
                     DocumentStore(application_data() / "web-preview.json", "sacrifice.web-preview", 1)
                         .read())
            options.set(id("swap"), previous->value("swap_buttons", false));
        for (const auto& entry : std::filesystem::directory_iterator(root / "maps"))
            if (entry.path().extension() == ".scp")
                maps.push_back(entry.path());
        std::ranges::sort(maps);
        if (const auto trial = std::ranges::find_if(maps,
                                                    [](const auto& path) {
                                                        auto filename = utf8(path.filename());
                                                        std::ranges::transform(
                                                            filename, filename.begin(), [](unsigned char c) {
                                                                return static_cast<char>(std::tolower(c));
                                                            });
                                                        return filename == "(2) trial.scp";
                                                    });
            trial != maps.end())
            map_row = static_cast<int>(trial - maps.begin());
        for (const auto& face : font_faces) {
            if (const auto source = catalog.find(id("TXTR"), FourCC{face.texture})) {
                auto image = load_texture(catalog, *source);
                fonts.emplace(std::string(face.name),
                              Font{NativeFont::build(face, image), gpu.upload(image)});
            }
        }
    }
    void open(FourCC next_page) {
        page = next_page;
        page_frame = ImGui::GetFrameCount();
        next_frames = 0;
        if (page == id("opts"))
            options_page = id("gopt");
        if (scene)
            scene->match().paused = page.value != 0;
    }
    bool is_front() const {
        if (SavePages::page(page))
            return !saves.in_game;
        return page.value && page != id("gmnu") && page != id("mobj") && page != id("hist") &&
               page != id("summ") && page != id("winr") && page != id("lose") && page != id("losn") &&
               page != id("chos") && !(page == id("opts") && return_page == id("gmnu"));
    }
    bool campaign_type() const {
        return game_type == "sacrifice";
    }
    void persist() {
        if (profile)
            roster.save(*profile);
        // The document writer flushes IDBFS, including the binary .wiz character files.
        save_preferences();
    }
    void save_preferences() {
        preferences.write(
            {{"type", game_type}, {"character", profile ? profile->name : ""}, {"options", options.save()}});
    }
    void reload(std::string selected = {}) {
        characters = roster.list(game_type);
        character_row = characters.empty() ? -1 : 0;
        for (unsigned i = 0; i < characters.size(); ++i)
            if (characters[i].name == selected)
                character_row = i;
    }
    Character* chosen() {
        return character_row >= 0 && character_row < static_cast<int>(characters.size())
                   ? &characters[character_row]
                   : nullptr;
    }
    void open_roster(bool campaign) {
        game_type = campaign ? "sacrifice" : "Skirmish";
        if (campaign && !host)
            host.emplace(root, "sacrifice");
        roster.import(root, game_type);
        reload(profile && profile->game_type == game_type ? profile->name : "");
        open(id("rost"));
    }
    void editor(bool edit) {
        auto* existing = edit ? chosen() : nullptr;
        if (edit && !existing)
            return;
        name.fill(0);
        edited.reset();
        new_gods.fill(campaign_type() ? 0 : static_cast<std::uint8_t>(random.next() % 5 + 1));
        if (existing) {
            new_gods = existing->gods;
            edited = existing->name;
            std::copy_n(existing->name.begin(), std::min(existing->name.size(), name.size() - 1),
                        name.begin());
        }
        wizards = roster.wizards();
        if (campaign_type())
            wizards = {{id("cwe2"), "Eldred"}};
        wizard_row = 0;
        if (existing)
            for (unsigned i = 0; i < wizards.size(); ++i)
                if (wizards[i].id == existing->wizard)
                    wizard_row = i;
        open(id("newc"));
    }
    bool valid_name() const {
        return character_name_status(name.data(), characters, edited) == 0;
    }
    void commit_character() {
        if (!valid_name() || wizards.empty())
            return;
        auto character = edited_character(name.data(), game_type, wizards[wizard_row].id, !campaign_type(),
                                          new_gods, characters, edited);
        save_character_edit(roster, character, edited);
        if (profile && edited && profile->name == *edited)
            profile = character;
        save_preferences();
        reload(character.name);
        open(id("rost"));
    }
    void bind_lobby(unsigned row) {
        if (row >= maps.size())
            return;
        map_row = row;
        const auto map = Archive::open(maps[row]);
        const auto sides = map.find(id("SIDS"));
        lobby.bind(sides ? SideDefinition::decode(map.read(*sides)) : std::vector<SideDefinition>{}, random);
    }
    void select_character() {
        if (!chosen())
            return;
        profile = *chosen();
        if (campaign_type()) {
            profile->wizard = id("cwe2");
            const auto mission = host->start(*profile);
            if (!mission) {
                persist();
                open(id("titl"));
                return;
            }
            next = Launch{*mission, {}, true};
            open(id("cnxt"));
        } else {
            lobby.player_wizard = profile->wizard;
            bind_lobby(map_row);
            open(id("loby"));
        }
    }
    void result(bool won) {
        if (campaign_state.active && profile)
            campaign_state.result(*profile, won);
        else
            campaign_state.outcome = won ? 1 : 0;
    }
    void finish(bool restart = false) {
        finish_requested = false;
        if (!scene)
            return;
        if (!campaign_state.active) {
            if (restart) {
                next = current;
                open(id("cnxt"));
            } else {
                open(id("titl"));
            }
            return;
        }
        const bool proceed = campaign_state.finish(*profile, scene->triggers(), *host, restart);
        const auto mission = proceed ? campaign_state.next(*profile, *host, current.path) : std::nullopt;
        persist();
        if (mission) {
            next = Launch{*mission, {}, true};
            open(id("cnxt"));
        } else
            open(id("titl"));
    }
    void loaded(SceneMatch& simulation, const Launch& request, PlayerControl* controls) {
        player = controls;
        if (request.saved) {
            if (const auto initial = request.saved->character()) {
                profile = *initial;
                game_type = initial->game_type;
                host.emplace(root, game_type);
            }
        }
        scene = &simulation;
        current = request;
        finish_requested = false;
        end_transition.reset();
        campaign_state.outcome = -1;
        campaign_state.active = request.campaign;
        campaign_state.wizard = nullptr;
        if (campaign_state.active && profile) {
            host->begin_mission();
            profile->wizard = id("cwe2");
            if (!request.saved)
                persist();
            campaign_state.enter(*profile, scene->match());
            auto& triggers = scene->triggers();
            bind_campaign_triggers(triggers, *profile, *host);
            triggers.mission_over = [this] { finish_requested = true; };
        }
        scene->match().world_journal = std::make_shared<WorldJournal>(scene->match(), player);
        if (request.saved)
            scene->match().world_journal->restore(request.saved->world);
        finish_requested = false;
        open({});
        error.clear();
        save_notice = request.saved ? "Game loaded." : "";
    }
    void detach() {
        if (scene) {
            scene->triggers().mission_over = {};
            scene->triggers().mission_choice = {};
            scene->triggers().next_mission = {};
        }
        if (scene && scene->match().world_journal)
            scene->match().world_journal->player(nullptr);
        scene = nullptr;
        player = nullptr;
        campaign_state.wizard = nullptr;
    }
    bool can_save_world() const {
        return scene && scene->match().world_journal && campaign_state.outcome == -1 && !finish_requested &&
               !end_transition.action.value;
    }
    // The browser has no synchronous frame readback, so its saves carry no thumbnail.
    void save_world(const std::string& file) {
        auto saved = WorldSave::capture(root, current.path, *scene, current.slots,
                                        campaign_state.active ? campaign_state.snapshot : std::nullopt);
        saved.setup["camera"] = saved_view;
        saved.stamp(profile ? profile->name : std::string(), campaign_state.active ? game_type : std::string("Skirmish"));
        saved.write(file);
    }
    void load_world(const std::string& file) {
        auto saved = WorldSave::read(file);
        if (!saved)
            throw Error("No saved game is available");
        const auto path = saved->mission(root);
        launch = Launch{path, saved->slots(), saved->character().has_value(),
                        std::make_shared<WorldSave>(std::move(*saved))};
    }
    const MenuForm& form(FourCC name) {
        auto [entry, added] = forms.try_emplace(name);
        if (added) {
            const auto source = catalog.find(id("FORM"), name);
            if (!source)
                throw Error("Missing menu form " + name.text());
            entry->second = MenuForm::decode(catalog.read(*source));
            adapt_graphics_page(entry->second);
            if (name == id("gmnu")) {
                auto& menu = entry->second;
                menu.rectangle.size.y += 24;
                for (auto& widget : menu.widgets)
                    if (widget.rectangle.origin.y >= 128)
                        widget.rectangle.origin.y += 24;
                auto restart = form(id("lose")).widgets.at(1);
                restart.rectangle = {{24, 128}, {160, 0}};
                menu.widgets.push_back(restart);
            }
        }
        return entry->second;
    }
    const std::string& text(FourCC name) {
        auto [entry, added] = texts.try_emplace(name);
        if (added && name.value) {
            if (const auto port = port_menu_text(name))
                entry->second = *port;
            else if (const auto source = catalog.find(id("TEXT"), name)) {
                const auto bytes = catalog.read(*source);
                entry->second.assign(reinterpret_cast<const char*>(bytes.data()), bytes.size());
                while (!entry->second.empty() && !entry->second.back())
                    entry->second.pop_back();
            } else
                entry->second = name.text();
        }
        return entry->second;
    }
    const Texture& texture(FourCC name, bool icon = false) {
        const auto tag = id(icon ? "ICON" : "TXTR");
        auto [entry, added] = textures.try_emplace(std::pair{tag, name});
        if (added && name.value)
            if (const auto source = catalog.find(tag, name)) {
                entry->second.header = decode_texture_header(catalog.read(*source));
                auto image = load_texture(catalog, *source);
                if (icon)
                    image = texture_frame(image, entry->second.header, 0);
                entry->second.image = gpu.upload(image);
            }
        return entry->second;
    }
    HudRect physical(HudRect box) const {
        return {{box.origin.x * scale, box.origin.y * scale}, {box.size.x * scale, box.size.y * scale}};
    }
    ImGuiMenuPainter menu_painter(ImDrawList& draw) {
        return {draw,
                scale,
                [this](FourCC name) { return handle(texture(name).image); },
                [this](std::string_view name) -> HudFontImage {
                    const auto it = fonts.find(std::string(name));
                    return it == fonts.end() ? HudFontImage{}
                                             : HudFontImage{&it->second.font, handle(it->second.image)};
                },
                [](ImDrawList& list) {
                    list.AddCallback(
                        [](const ImDrawList*, const ImDrawCmd*) { ImGui_ImplWGPU_SetAdditive(); }, nullptr);
                },
                true};
    }
    void quad(ImDrawList& draw, const char* name, HudRect r, ImVec2 a = {0, 0}, ImVec2 b = {1, 1},
              ImU32 tint = IM_COL32_WHITE) {
        auto paint = menu_painter(draw);
        paint.quad(id(name), r, {Vec2{a.x, a.y}, Vec2{b.x, a.y}, Vec2{a.x, b.y}, Vec2{b.x, b.y}}, tint);
    }
    void quad_uv(ImDrawList& draw, const char* name, HudRect r, std::array<ImVec2, 4> uv, ImU32 tint) {
        auto paint = menu_painter(draw);
        std::array<Vec2, 4> coordinates;
        for (unsigned i = 0; i < uv.size(); ++i)
            coordinates[i] = {uv[i].x, uv[i].y};
        paint.quad(id(name), r, coordinates, tint);
    }
    void label(ImDrawList& draw, HudRect r, std::string_view string, const char* face = "verdana",
               ImU32 colour = IM_COL32_WHITE, unsigned align = 0) {
        auto paint = menu_painter(draw);
        paint.text(face, r, string, colour, align);
    }
    void panel(ImDrawList& draw, const MenuForm& form, HudRect bounds) {
        auto paint = menu_painter(draw);
        paint_menu_panel(paint, form, bounds, !panel_backdrop, text(form.title));
    }
    bool button(ImDrawList& draw, HudRect r, const std::string& key, const std::string& title,
                bool enabled = true) {
        const auto p = physical(r);
        ImGui::SetCursorScreenPos({p.origin.x, p.origin.y});
        ImGui::BeginDisabled(!enabled);
        const bool pressed = ImGui::InvisibleButton(key.c_str(), {p.size.x, p.size.y});
        const bool hover = ImGui::IsItemHovered();
        ImGui::EndDisabled();
        auto paint = menu_painter(draw);
        paint_menu_button(paint, r, hover, enabled);
        paint_menu_button_label(paint, r, title, hover, enabled, 0x2000);
        return pressed && enabled;
    }
    bool enabled(FourCC form, const std::string& command) const {
        if (const auto allowed = saves.enabled(form, command))
            return *allowed;
        FrontendAvailability available;
        available.selected = character_row >= 0 && character_row < static_cast<int>(characters.size());
        available.valid_name = valid_name();
        available.wizards = !wizards.empty();
        available.god_path = !campaign_type();
        available.custom_path = command == "espb" && roster.custom_path_available();
        available.can_start = lobby.can_start();
        available.objectives = scene && !scene->triggers().objectives.empty();
        available.detailed_options = true;
        available.world_load = true; // The title's Load Game always opens FORM:lgam (0x40f6b0).
        available.world_save = can_save_world();
        // Browser output is stereo; device selection and native input timing aren't implemented. The browser
        // always presents at the display rate, so VSync stays as it is.
        if ((form == id("gopt") && command == "tbuf") || command == "cprv" || command == "cdky" || (form == id("sopt") && command == "spek") ||
            (form == id("copt") && (command == "dblc" || command == "krst" || command == "krdl" ||
                                    command == "mspd" || command == "rdem")))
            return false;
        return frontend_enabled(form, command, available);
    }
    void picture(ImDrawList& draw, FourCC name, HudRect r, bool icon = false) {
        const auto& image = texture(name, icon);
        if (!image.image)
            return;
        r = physical(r);
        draw.AddImage(handle(image.image), {r.origin.x, r.origin.y},
                      {r.origin.x + r.size.x, r.origin.y + r.size.y});
    }
    void list(ImDrawList& draw, HudRect r, const char* key, const std::vector<std::string>& values,
              int& row) {
        const auto p = physical(r);
        ImGui::SetCursorScreenPos({p.origin.x, p.origin.y});
        ImGui::BeginChild(key, {p.size.x, p.size.y}, ImGuiChildFlags_None, ImGuiWindowFlags_NoBackground);
        for (unsigned i = 0; i < values.size(); ++i) {
            const auto pos = ImGui::GetCursorScreenPos();
            if (ImGui::Selectable(std::format("##{}", i).c_str(), row == static_cast<int>(i),
                                  ImGuiSelectableFlags_None, {0, 16 * scale}))
                row = i;
            label(*ImGui::GetWindowDrawList(), {{pos.x / scale, pos.y / scale}, {r.size.x, 16}}, values[i]);
        }
        ImGui::EndChild();
    }
    void paint(ImDrawList& draw, const MenuForm& menu, HudRect bounds, FourCC& action, int& action_row,
               int row = -1) {
        ImGui::PushID(menu.id.value);
        ImGui::PushID(row);
        panel(draw, menu, bounds);
        const auto content = menu.content(bounds);
        for (const auto& widget : menu.widgets) {
            auto r = menu.bounds(widget, content);
            const auto command = widget.id.text();
            if (SavePages::page(menu.id)) {
                if (save_thumbnail_revision != saves.thumbnail_revision) {
                    save_thumbnail_revision = saves.thumbnail_revision;
                    save_thumbnail = saves.thumbnail ? gpu.upload(*saves.thumbnail) : nullptr;
                }
                auto painter = menu_painter(draw);
                if (save_page_widget(painter, draw, scale, saves, menu, widget, r, handle(save_thumbnail),
                                     ImGui::GetFrameCount() == page_frame + 1, action))
                    continue;
            }
            if (widget.kind == 2 && widget.texture.value) {
                if ((menu.id == id("opts") && widget.texture != options_page) || widget.texture == id("slid"))
                    continue;
                if (widget.texture == id("SLID")) {
                    ImGui::PushID(command.c_str());
                    ImGui::BeginDisabled(!enabled(menu.id, command));
                    auto painter = menu_painter(draw);
                    if (menu_options_slider(painter, form(widget.texture), r, scale, widget.id, options,
                                            [this](FourCC key) { return text(key); }))
                        save_preferences();
                    ImGui::EndDisabled();
                    ImGui::PopID();
                    continue;
                }
                if (menu.id == id("slts")) {
                    const int index = static_cast<int>(command.back() - '1');
                    paint(draw, form(widget.texture), r, action, action_row, index);
                } else if (command.starts_with("lvl")) {
                    paint(draw, form(widget.texture), r, action, action_row, command.back() - '0');
                } else
                    paint(draw, form(widget.texture), r, action, action_row, row);
            } else if (widget.kind == 1) {
                if (command == "surr" || command == "obsv")
                    continue;
                std::string title = text(widget.text);
                if (const auto setting_label = option_menu_label(menu.id, widget.id, options))
                    title = text(*setting_label);
                bool allow = enabled(menu.id, command);
                if (menu.id == id("1ply") && command == "scen")
                    title = "Skirmish";
                if (menu.id == id("cnxt"))
                    title = "Loading next mission...";
                if (menu.id == id("chos") && scene && !scene->triggers().choices.empty()) {
                    const auto& choice = scene->triggers().choices.front();
                    title = text(command == "cho1" ? choice.first : choice.second);
                }
                if ((menu.id == id("LEVL") || menu.id == id("levl")) && command == "god_" && row > 0 &&
                    row < 10)
                    title = text(FourCC{SkirmishLobby::god_names[new_gods[row]]});
                if (menu.id == id("slot") && row >= 0 && row < 5) {
                    const auto& slot = lobby.slots[row];
                    if (command == "name") {
                        title = slot.state == LobbySlot::player && profile           ? profile->name
                                : slot.state == LobbySlot::computer                  ? text(slot.name)
                                : slot.state == LobbySlot::closed || !slot.available ? "Closed"
                                                                                     : "Open";
                        allow = slot.available && slot.state != LobbySlot::closed && row != lobby.local;
                    } else {
                        title = text(FourCC{SkirmishLobby::god_names[slot.god]});
                        allow = slot.available &&
                                (slot.state == LobbySlot::player || slot.state == LobbySlot::computer);
                    }
                }
                if (button(draw, r, command, title, allow && !panel_backdrop)) {
                    action = widget.id;
                    action_row = row;
                }
            } else if (widget.kind == 12 && command == "logo") {
                if (const auto& logo = texture(TitleBackdrop::logo_texture()); logo.image)
                    draw_hud_mesh(draw, backdrop.logo(physical(r)), handle(logo.image));
            } else if (widget.kind == 3 || (widget.kind == 12 && command != "swiz")) {
                FourCC image = widget.kind == 12   ? widget.id
                               : widget.icon.value ? widget.icon
                                                   : widget.texture;
                if (menu.id == id("slot") && row >= 0 && row < 5) {
                    const auto& slot = lobby.slots[row];
                    if (command == "type") {
                        constexpr std::array<const char*, 4> types{"lslo", "lpla", "lslc", "lplv"};
                        image = id(slot.available ? types[slot.state] : "lslc");
                    }
                    if (command == "pnum") {
                        int number = 1;
                        for (int i = 0; i < row; ++i)
                            number += i != lobby.local;
                        image = row == lobby.local ? id("lsll")
                                                   : id(std::format("lsl{}", std::min(number, 4)).c_str());
                    }
                    if (command == "redy")
                        image =
                            id(slot.state == LobbySlot::player || slot.state == LobbySlot::computer ? "lthu"
                                                                                                    : "lthd");
                    if (command == "team")
                        image = id(std::format("ltm{}", slot.team ? static_cast<char>('0' + slot.team) : 'x')
                                       .c_str());
                }
                if (command == "_map" && !maps.empty()) {
                    const auto index = page == id("cgam") ? browsed_row : map_row;
                    auto [entry, added] = previews.try_emplace(index);
                    if (added) {
                        const auto map = Archive::open(maps[index]);
                        if (const auto record = map.find(id("PREV")))
                            entry->second = gpu.upload(decode_preview(map.read(*record)));
                    }
                    if (entry->second) {
                        const auto p = physical(r);
                        draw.AddImage(handle(entry->second), {p.origin.x, p.origin.y},
                                      {p.origin.x + p.size.x, p.origin.y + p.size.y});
                    } else
                        picture(draw, image, r);
                } else
                    picture(draw, image, r, widget.icon.value != 0);
                if (menu.id == id("slot") && row >= 0 && row < 5 && lobby.slots[row].available &&
                    (command == "type" || command == "team")) {
                    const auto p = physical(r);
                    ImGui::SetCursorScreenPos({p.origin.x, p.origin.y});
                    if (ImGui::InvisibleButton(command.c_str(), {p.size.x, p.size.y})) {
                        action = widget.id;
                        action_row = row;
                    }
                }
            } else if (widget.kind == 12 && command == "swiz") {
                const auto* character = chosen();
                const FourCC wizard = menu.id == id("newc") && !wizards.empty() ? wizards[wizard_row].id
                                      : character                               ? character->wizard
                                                                                : FourCC{};
                if (wizard_preview.update(wizard, ImGui::GetIO().DeltaTime,
                                          static_cast<unsigned>(r.size.x * scale),
                                          static_cast<unsigned>((r.size.y - 24) * scale))) {
                    const auto& image = wizard_preview.image();
                    if (image.rgba.empty())
                        wizard_image.reset();
                    else if (!wizard_image || wgpuTextureGetWidth(wizard_image->texture) != image.width ||
                             wgpuTextureGetHeight(wizard_image->texture) != image.height)
                        wizard_image = gpu.upload(image);
                    else
                        gpu.update(*wizard_image, image);
                }
                if (wizard.value && wizard_image) {
                    const auto p = physical(r);
                    draw.AddImage(handle(wizard_image), {p.origin.x, p.origin.y},
                                  {p.origin.x + p.size.x, p.origin.y + p.size.y - 24 * scale});
                }
                const auto title = std::ranges::find(wizard_choices, wizard, &WizardChoice::id);
                label(draw, {{r.origin.x, r.origin.y + r.size.y - 24}, {r.size.x, 24}},
                      title == wizard_choices.end() ? "" : title->name, "elgar", IM_COL32_WHITE, 1);
                if (!wizard_preview.error().empty())
                    label(draw, r, wizard_preview.error(), "verdana", IM_COL32(255, 120, 100, 255));
            } else if (widget.kind == 6 && command == "pnam") {
                r.size.y = 20;
                const auto p = physical(r);
                ImGui::SetCursorScreenPos({p.origin.x, p.origin.y});
                ImGui::SetNextItemWidth(p.size.x);
                if (ImGui::GetFrameCount() == page_frame + 1)
                    ImGui::SetKeyboardFocusHere();
                ImGui::InputText("##character_name", name.data(), name.size());
            } else if (widget.kind == 5 && command == "rost") {
                std::vector<std::string> names;
                for (const auto& character : characters)
                    names.push_back(character.name);
                list(draw, r, "characters", names, character_row);
            } else if (widget.kind == 5 && command == "cinf" && chosen()) {
                const auto& character = *chosen();
                label(draw, r,
                      std::format("{}\nLevel {}\nCampaign {}", character.name, character.level,
                                  character.campaign_node));
            } else if (widget.kind == 5 && command == "maps") {
                std::vector<std::string> names;
                for (const auto& map : maps)
                    names.push_back(utf8(map.stem()));
                list(draw, r, "maps", names, browsed_row);
            } else if (widget.kind == 5 && command == "resn" && menu.id == id("gopt")) {
                label(draw, r, "Follows browser window", "verdana", IM_COL32_WHITE, 1);
            } else if (widget.kind == 5 && command == "prov" && menu.id == id("sopt")) {
                label(draw, r, "Browser audio (stereo)", "verdana", IM_COL32_WHITE, 1);
            } else if (widget.kind == 5 && command == "GTYP") {
                label(draw, r, "Sacrifice");
            } else if (widget.kind == 7) {
                r.size.y = 16;
                const auto state = options.checked(widget.id);
                auto painter = menu_painter(draw);
                if (menu_checkbox(painter, r, scale, command.c_str(), text(widget.text),
                                  state.has_value() && enabled(menu.id, command), state.value_or(false),
                                  widget.flags))
                    action = widget.id;
            } else if (widget.kind == 10) {
                std::string string = text(widget.text);
                ImU32 colour = IM_COL32_WHITE;
                if (command == "name" && menu.id == id("MAPi"))
                    string = maps.empty() ? "" : utf8(maps[map_row].stem());
                if (command == "lvl#" && row > 0)
                    string = std::to_string(row);
                if (command.starts_with("spl") && row > 0 && row < 10) {
                    if (!registry)
                        registry = SpellRegistry::load(catalog);
                    static constexpr std::array<std::array<const char*, 2>, 5> groups{{{"Slif", "Clif"},
                                                                                       {"Sfir", "Cfir"},
                                                                                       {"Sear", "Cear"},
                                                                                       {"Sair", "Cair"},
                                                                                       {"Sdea", "Cdea"}}};
                    unsigned index = 0;
                    string.clear();
                    if (new_gods[row] >= 1 && new_gods[row] <= 5)
                        for (const auto* group : groups[new_gods[row] - 1])
                            if (const auto* found = registry->group(id(group)))
                                for (const auto& spell : found->definitions)
                                    if (spell.priority / 100u == static_cast<unsigned>(row) &&
                                        index++ == static_cast<unsigned>(command.back() - '0'))
                                        string = text(spell.id);
                }
                if (menu.id == id("hist") && scene && !scene->triggers().journal.empty()) {
                    const auto& entries = scene->triggers().journal;
                    history_row = std::clamp(history_row, 0, static_cast<int>(entries.size()) - 1);
                    if (command == "spkr")
                        string = entries[history_row].title;
                    if (command == "spch")
                        string = entries[history_row].caption;
                }
                if (command == "smod")
                    string = std::array<const char*, 3>{"Creatures and souls", "Structures",
                                                        "Wizard results"}[summary_page];
                if (command == "vers")
                    string = "SacrificeModern - WebGPU";
                if (command == "ask!" && scene && !scene->triggers().choices.empty())
                    string = text(scene->triggers().choices.front().question);
                if (menu.id == id("mobj") && scene && command.starts_with("obj")) {
                    unsigned i = 0;
                    string.clear();
                    for (const auto& objective : scene->triggers().objectives)
                        if ((objective.flags & 1) && i++ == static_cast<unsigned>(command.back() - '1')) {
                            string = "- " + objective.caption;
                            if (objective.flags & 2)
                                colour = IM_COL32(136, 136, 136, 255);
                            break;
                        }
                }
                label(draw, {r.origin, {r.size.x, std::max(16.f, r.size.y)}}, string, "verdana", colour,
                      (widget.flags & 0x800)    ? 1
                      : (widget.flags & 0x4000) ? 2
                                                : 0);
            }
        }
        ImGui::PopID();
        ImGui::PopID();
    }
    void act(FourCC command, int row) {
        if (end_transition.action.value)
            return;
        const auto name = command.text(), current_page = page.text();
        if (SavePages::page(page)) {
            saves.action(page, command);
            return;
        }
        if (name == "lgam" && (current_page == "titl" || current_page == "gmnu")) { // 0x40f6b0 / gmnu.
            saves.open(id("lgam"), current_page == "gmnu");
            return;
        }
        if (name == "sgam" && current_page == "gmnu" && enabled(page, name)) {
            saves.open(id("sgam"), true);
            return;
        }
        const auto route = frontend_route(page, command, row);
        if (route.action != FrontendAction::unhandled) {
            if (!enabled(page, name))
                return;
            if (apply_lobby_action(route.action, row, lobby, random))
                return;
            switch (route.action) {
            case FrontendAction::open:
                if (route.destination == id("opts"))
                    return_page = page;
                open(route.destination);
                break;
            case FrontendAction::campaign:
                open_roster(true);
                break;
            case FrontendAction::skirmish:
                open_roster(false);
                break;
            case FrontendAction::new_character:
                editor(false);
                break;
            case FrontendAction::edit_character:
                editor(true);
                break;
            case FrontendAction::select_character:
                select_character();
                break;
            case FrontendAction::save_character:
                commit_character();
                break;
            case FrontendAction::cancel_character:
                edited.reset();
                open(route.destination);
                break;
            case FrontendAction::delete_character:
                if (chosen()) {
                    roster.remove(*chosen());
                    if (profile && profile->name == chosen()->name && profile->game_type == game_type)
                        profile.reset();
                    save_preferences();
                    reload();
                }
                open(route.destination);
                break;
            case FrontendAction::wizard_next:
                if (!wizards.empty())
                    wizard_row = (wizard_row + 1) % wizards.size();
                break;
            case FrontendAction::wizard_previous:
                if (!wizards.empty())
                    wizard_row = (wizard_row + wizards.size() - 1) % wizards.size();
                break;
            case FrontendAction::god_path:
                new_gods[row] = new_gods[row] % 5 + 1;
                break;
            case FrontendAction::browse_map:
                browsed_row = map_row;
                open(route.destination);
                break;
            case FrontendAction::accept_map:
                if (map_row != browsed_row)
                    bind_lobby(browsed_row);
                open(route.destination);
                break;
            case FrontendAction::start_match:
                if (lobby.can_start()) {
                    next = Launch{maps[map_row], lobby.session(), false};
                    open(id("cnxt"));
                }
                break;
            case FrontendAction::quit:
                EM_ASM({ location.reload(); });
                break;
            default:
                break;
            }
            return;
        }
        if (current_page == "opts") {
            if (!enabled(page, name))
                return;
            if (option_menu_command(options, options_page, command))
                save_preferences();
            if (name == "menu")
                open(return_page);
        } else if (current_page == "chos") {
            if (name == "cho1" || name == "cho2") {
                scene->triggers().answer_choice(name == "cho1" ? 1 : 2);
                open({});
            }
        } else if (current_page == "hist") {
            if (name == "next")
                ++history_row;
            else if (name == "prev")
                --history_row;
            else
                open(id("gmnu"));
        } else if (current_page == "mobj")
            open(id("gmnu"));
        else if (current_page == "summ") {
            if (name == "next" || name == "prev")
                summary_page = std::clamp(summary_page + (name == "next" ? 1 : -1), 0, 2);
            else
                open(return_page);
        } else if (current_page == "gmnu" || current_page == "lose" || current_page == "winr" ||
                   current_page == "losn") {
            if (name == "rstt" || name == "ok__" || name == "exit")
                end_transition.start(command);
            else if (name == "vobj")
                open(id("mobj"));
            else if (name == "hist")
                open(command);
            else if (name == "summ") {
                return_page = page;
                open(command);
            } else if (name == "opts") {
                return_page = page;
                open(command);
            } else if (name == "cncl")
                open({});
        }
    }
    void update(const InputState& input, PlayerControl* player, bool inspector, float seconds) {
        menu_keys = {input.pressed_codes[28], input.pressed_codes[1]};
        if (!scene)
            return;
        const auto* side = scene->match().local_side;
        if (const auto command = end_transition.advance(seconds)) {
            if (*command == id("exit") && campaign_state.active)
                result(false);
            finish(*command == id("rstt"));
            return;
        }
        if (end_transition.action.value)
            return;
        if (finish_requested) {
            if (campaign_state.outcome == -1 && side && side->outcome)
                result(side->outcome == 1);
            finish();
        } else if (!is_front() && campaign_state.outcome == -1 && side && side->outcome) {
            result(side->outcome == 1);
            present_mission_result(scene->match(), side->outcome == 1);
            open(id(campaign_state.outcome == 1 ? "winr" : campaign_state.active ? "lose" : "losn"));
        }
        if (!page.value && !scene->triggers().choices.empty())
            open(id("chos"));
        if (!inspector && input.pressed_codes[1] && !scene->triggers().cinematic()) {
            if (player && player->pending.value)
                player->cancel_target();
            else if (!page.value)
                open(id("gmnu"));
        }
        if (!inspector && scene->triggers().cinematic() &&
            (input.pressed_codes[28] || input.pressed_codes[57] || clicked(input)))
            scene->triggers().dismiss_dialogue(true);
        if (!inspector && !page.value && !scene->triggers().cinematic()) {
            if (input.pressed_codes[59] && can_save_world()) { // F1 'qsav'.
                if (player)
                    player->click(id("qsav"));
                saves.quick_save();
            } else if (input.pressed_codes[60]) { // F2 'qlod'.
                if (player)
                    player->click(id("qlod"));
                saves.quick_load();
            }
        }
    }
    void loading_screen(unsigned w, unsigned h) {
        if (loading_path != launch->path) {
            loading_path = launch->path;
            loading_image.reset();
            loading_description.clear();
            try {
                auto picture = loading_picture(root, launch->path);
                loading_image = gpu.upload(picture.image);
                loading_description = std::move(picture.description);
            } catch (const std::exception&) {
            }
        }
        scale = std::min(w / 640.f, h / 480.f);
        auto& draw = *ImGui::GetForegroundDrawList();
        draw.AddRectFilled({0, 0}, {static_cast<float>(w), static_cast<float>(h)}, IM_COL32_BLACK);
        const auto& igam = form(id("igam"));
        auto paint = menu_painter(draw);
        paint_loading_screen(paint, draw, scale, igam,
                             igam.bounds(static_cast<unsigned>(w / scale), static_cast<unsigned>(h / scale)),
                             handle(loading_image), loading_description, 0);
        loading_shown = true;
    }
    void draw(unsigned w, unsigned h, bool inspector, WebMedia& media) {
        // Compact state for diagnostics without exposing gameplay mutations to JavaScript.
        const auto page_name = page.value ? page.text() : "play";
        const auto mission = utf8(current.path.filename());
        // clang-format off
        EM_ASM({
            const state=Module['frontendStats'] || (Module['frontendStats']={});
            state.page=UTF8ToString($0); state.mission=UTF8ToString($1);
            state.campaign=!!$2; state.node=$3; state.characters=$4; state.error=UTF8ToString($5);
        },page_name.c_str(),mission.c_str(),campaign_state.active,profile ? profile->campaign_node : 0,characters.size(),error.c_str());
        // clang-format on
        saves.service();
        if (launch) {
            loading_screen(w, h);
            return;
        }
        if (!page.value || inspector)
            return;
        scale = std::min(w / 640.f, h / 480.f);
        const unsigned logical_w = static_cast<unsigned>(w / scale),
                       logical_h = static_cast<unsigned>(h / scale);
        ImGui::SetNextWindowPos({0, 0});
        ImGui::SetNextWindowSize({static_cast<float>(w), static_cast<float>(h)});
        ImGui::Begin("##game_menu", nullptr,
                     ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove |
                         ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoBackground |
                         ImGuiWindowFlags_NoBringToFrontOnFocus);
        auto& draw = *ImGui::GetWindowDrawList();
        if (is_front()) {
            draw.AddRectFilled({0, 0}, {static_cast<float>(w), static_cast<float>(h)}, IM_COL32_BLACK);
            backdrop.advance(ImGui::GetIO().DeltaTime);
            if (TitleBackdrop::shows_tunnel(page))
                if (const auto& tunnel = texture(TitleBackdrop::tunnel_texture()); tunnel.image)
                    draw_hud_mesh(draw, backdrop.tunnel(static_cast<float>(w), static_cast<float>(h)),
                                  handle(tunnel.image));
            if (page != id("titl") && !(form(page).flags & 0x40)) {
                FourCC unused;
                int unused_row = -1;
                panel_backdrop = true;
                paint(draw, form(id("titl")), form(id("titl")).bounds(logical_w, logical_h), unused,
                      unused_row);
                panel_backdrop = false;
                draw.AddRectFilled({0, 0}, {static_cast<float>(w), static_cast<float>(h)},
                                   IM_COL32(0, 0, 0, 120));
            }
        } else
            draw.AddRectFilled({0, 0}, {static_cast<float>(w), static_cast<float>(h)},
                               IM_COL32(0, 0, 0, 120));
        FourCC action;
        int row = -1;
        const auto& menu = form(page);
        if (SavePages::page(page) && saves.parent.value) { // sav? / del? sit over their list page.
            FourCC unused;
            int unused_row = -1;
            ImGui::BeginDisabled();
            paint(draw, form(saves.parent), form(saves.parent).bounds(logical_w, logical_h), unused, unused_row);
            ImGui::EndDisabled();
        }
        ImGui::BeginDisabled(end_transition.action.value != 0);
        paint(draw, menu, menu.bounds(logical_w, logical_h), action, row);
        if (page == id("summ") && scene) {
            std::string string;
            for (const auto* side : scene->match().side_order) {
                if (summary_page == 0)
                    string +=
                        std::format("Side {}   Summoned {}   Souls harvested {}\n", side->definition.index,
                                    side->summoned_creatures, side->souls_harvested);
                else if (summary_page == 1)
                    string +=
                        std::format("Side {}   Manaliths {} / {}   Shrines {} / {}\n", side->definition.index,
                                    side->manaliths_created, side->manaliths_destroyed, side->shrines_created,
                                    side->shrines_destroyed);
                else
                    string += std::format("Side {}   Level {}   Kills {}   Banishments {}\n",
                                          side->definition.index, side->peak_level, side->peak_kills,
                                          side->banishments);
            }
            label(draw, {{48, 160}, {544, 144}}, string);
        }
        if (!save_notice.empty())
            label(draw, {{16, 412}, {608, 24}}, save_notice);
        if (!error.empty())
            label(draw, {{16, 440}, {608, 40}}, error, "verdana", IM_COL32(255, 120, 100, 255));
        if (const auto key = menu_keys.action(menu, page_frame == ImGui::GetFrameCount());
            key.value && enabled(page, key.text()))
            action = key;
        if (page == id("cnxt") && ++next_frames >= 2 && next) {
            launch = std::exchange(next, std::nullopt);
        }
        ImGui::EndDisabled();
        ImGui::End();
        if (end_transition.action.value)
            ImGui::GetForegroundDrawList()->AddRectFilled(
                {0, 0}, {static_cast<float>(w), static_cast<float>(h)},
                IM_COL32(0, 0, 0, static_cast<int>(end_transition.alpha * 255)));
        if (action.value) {
            try {
                act(action, row);
            } catch (const std::exception& failure) {
                error = failure.what();
            }
        }
    }
};
WebFrontend::WebFrontend(WebGpuContext& gpu, const ResourceCatalog& catalog, std::filesystem::path root)
    : impl_(std::make_unique<Impl>(gpu, catalog, std::move(root))) {}
WebFrontend::~WebFrontend() {
    impl_->detach();
}
std::optional<WebFrontend::Launch> WebFrontend::take_launch() {
    if (!impl_->launch || !std::exchange(impl_->loading_shown, false))
        return std::nullopt;
    impl_->loading_path.clear();
    auto launch = std::exchange(impl_->launch, std::nullopt);
    if (launch && launch->campaign && (!launch->saved || launch->saved->setup.contains("campaign_god")))
        launch->character = launch->saved ? launch->saved->character() : impl_->profile;
    return launch;
}
void WebFrontend::loaded(SceneMatch& scene, const Launch& launch, PlayerControl* player) {
    impl_->loaded(scene, launch, player);
}
void WebFrontend::view(Vec3 position, float yaw, float pitch) {
    impl_->saved_view = {WorldJournal::point(position), yaw, pitch};
}
void WebFrontend::detach() {
    impl_->detach();
}
void WebFrontend::failed(std::string error) {
    impl_->error = std::move(error);
    if (impl_->page == id("lprg")) // 0x4a5e70: a failed saved-game load shows lerr.
        impl_->saves.load_failed();
    else
        impl_->open(id("titl"));
}
void WebFrontend::update(const InputState& input, PlayerControl* player, bool inspector, float seconds) {
    impl_->update(input, player, inspector, seconds);
}
void WebFrontend::draw(unsigned w, unsigned h, bool inspector, WebMedia& media) {
    impl_->draw(w, h, inspector, media);
}
const GameOptions& WebFrontend::options() const {
    return impl_->options;
}
bool WebFrontend::blocking() const {
    return impl_->page.value != 0;
}
bool WebFrontend::front_end() const {
    return impl_->is_front();
}
bool WebFrontend::fast_forward(const InputState& input) const {
    return impl_->scene && impl_->scene->triggers().cinematic() && input.scancodes[15] && !blocking();
}
} // namespace sacrifice
