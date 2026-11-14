#include "sacrifice/menu_presentation.hpp"
#include <algorithm>
#include <array>
#include <cctype>
#include <cmath>
#include <format>
namespace sacrifice {
namespace {
FourCC id(const char* text) {
    return FourCC::from_text(text);
}
} // namespace
FourCC MenuKeys::action(const MenuForm& form, bool just_opened) const {
    if (just_opened)
        return {};
    return cancel ? form.cancel : accept ? form.accept : FourCC{};
}
std::string_view widget_font_name(std::uint32_t flags, std::string_view fallback) {
    return flags & 0x200    ? "verdana"
           : flags & 0x400  ? "bookman"
           : flags & 0x800  ? "elgar"
           : flags & 0x1000 ? "verdanacolor"
                            : fallback;
}
unsigned widget_alignment(std::uint32_t flags) {
    const auto mask = flags & 0x10e000;
    return mask == 0x2000 ? 1u : mask == 0x8000 ? 2u : 0u;
}
void paint_menu_panel(MenuPainter& paint, const MenuForm& form, HudRect bounds, bool enabled,
                      std::string_view title) {
    const auto quad_image = [&](const char* texture, HudRect box, Vec2 a, Vec2 b, std::uint32_t colour) {
        paint.quad(id(texture), box, {a, Vec2{b.x, a.y}, Vec2{a.x, b.y}, b}, colour);
    };
    const auto content = form.content(bounds);
    if (form.flags & 4)
        quad_image("FMbg",
                   {{content.origin.x - 8, content.origin.y - 8}, {content.size.x + 16, content.size.y + 16}},
                   {0, 0}, {(content.size.x + 16) / 32, (content.size.y + 16) / 32},
                   (static_cast<std::uint32_t>(form.alpha) << 24) | 0xffffffu);
    if (!(form.flags & 2))
        return;
    const bool titled = form.flags & 1;
    const float s = enabled ? .5f : 0; // 0x4ace10; subforms inherit the page focus (0x4ae130).
    const std::uint32_t tint = enabled ? 0xffffffff : 0x66ffffff;
    const float X = bounds.origin.x, Y = bounds.origin.y, W = bounds.size.x, H = bounds.size.y;
    const float cy = content.origin.y, cw = content.size.x, ch = content.size.y;
    const float rows = H / 16 - 5, columns = cw / 16 - 4;
    const auto quad = [&](const char* texture, float x, float y, float w, float h, Vec2 a, Vec2 b, Vec2 c,
                          Vec2 d) {
        paint.quad(FourCC::from_text(texture), {{x, y}, {w, h}}, {a, b, c, d}, tint);
    };
    // Left column.
    if (titled) {
        quad("FMp1", X, Y, 32, 32, {s, .5f}, {s + .5f, .5f}, {s, 1}, {s + .5f, 1});
        quad("FMp1", X, cy, 16, 16, {s, .25f}, {s + .25f, .25f}, {s, .5f}, {s + .25f, .5f});
    } else
        quad("FMp1", X, Y, 32, 32, {s, 0}, {s + .5f, 0}, {s, .5f}, {s + .5f, .5f});
    quad("FMp2", X, cy + 16, 16, 16, {0, .5f}, {.5f, .5f}, {0, 1}, {.5f, 1});
    quad("FMp2", X, cy + 32, 16, 16, {.5f, .5f}, {1, .5f}, {.5f, 1}, {1, 1});
    quad("FMbo", X, cy + 48, 16, ch - 64, {0, 1}, {0, 0}, {rows, 1}, {rows, 0});
    quad("FMp2", X, cy + ch - 48, 16, 16, {.5f, 1}, {1, 1}, {.5f, .5f}, {1, .5f});
    quad("FMp2", X, cy + ch - 32, 16, 16, {0, 1}, {.5f, 1}, {0, .5f}, {.5f, .5f});
    quad("FMp1", X, cy + ch - 16, 32, 32, {s, .5f}, {s + .5f, .5f}, {s, 0}, {s + .5f, 0});
    // Top strip: the title bar, or joins and a repeated edge.
    if (titled)
        quad("FMtb", X + 32, Y, cw - 32, 32, {0, 0}, {(cw / 16 - 2) * .5f, 0}, {0, 1},
             {(cw / 16 - 2) * .5f, 1});
    else {
        quad("FMp2", X + 32, Y, 16, 16, {0, .5f}, {0, 1}, {.5f, .5f}, {.5f, 1});
        quad("FMp2", X + 48, Y, 16, 16, {.5f, .5f}, {.5f, 1}, {1, .5f}, {1, 1});
        quad("FMbo", X + 64, Y, W - 128, 16, {0, 1}, {columns, 1}, {0, 0}, {columns, 0});
        quad("FMp2", X + W - 64, Y, 16, 16, {.5f, 1}, {.5f, .5f}, {1, 1}, {1, .5f});
        quad("FMp2", X + W - 48, Y, 16, 16, {0, 1}, {0, .5f}, {.5f, 1}, {.5f, .5f});
    }
    // Bottom strip.
    const float bottom = Y + H - 16;
    quad("FMp2", X + 32, bottom, 16, 16, {.5f, .5f}, {.5f, 1}, {0, .5f}, {0, 1});
    quad("FMp2", X + 48, bottom, 16, 16, {1, .5f}, {1, 1}, {.5f, .5f}, {.5f, 1});
    quad("FMbo", X + 64, bottom, W - 128, 16, {0, 0}, {columns, 0}, {0, 1}, {columns, 1});
    quad("FMp2", X + W - 64, bottom, 16, 16, {1, 1}, {1, .5f}, {.5f, 1}, {.5f, .5f});
    quad("FMp2", X + W - 48, bottom, 16, 16, {.5f, 1}, {.5f, .5f}, {0, 1}, {0, .5f});
    // Right column, mirrored.
    const float right = X + cw;
    if (titled) {
        quad("FMp1", right, Y, 32, 32, {s + .5f, .5f}, {s, .5f}, {s + .5f, 1}, {s, 1});
        quad("FMp1", right + 16, cy, 16, 16, {s + .25f, .25f}, {s, .25f}, {s + .25f, .5f}, {s, .5f});
    } else
        quad("FMp1", right, Y, 32, 32, {s + .5f, 0}, {s, 0}, {s + .5f, .5f}, {s, .5f});
    quad("FMp2", right + 16, cy + 16, 16, 16, {.5f, .5f}, {0, .5f}, {.5f, 1}, {0, 1});
    quad("FMp2", right + 16, cy + 32, 16, 16, {1, .5f}, {.5f, .5f}, {1, 1}, {.5f, 1});
    quad("FMbo", right + 16, cy + 48, 16, ch - 64, {0, 0}, {0, 1}, {rows, 0}, {rows, 1});
    quad("FMp2", right + 16, cy + ch - 48, 16, 16, {1, 1}, {.5f, 1}, {1, .5f}, {.5f, .5f});
    quad("FMp2", right + 16, cy + ch - 32, 16, 16, {.5f, 1}, {0, 1}, {.5f, .5f}, {0, .5f});
    quad("FMp1", right, cy + ch - 16, 32, 32, {s + .5f, .5f}, {s, .5f}, {s + .5f, 0}, {s, 0});
    if ((form.flags & 3) == 3 && form.title.value) {
        const auto height = static_cast<float>(paint.line_height("elgar"));
        const auto top = bounds.origin.y + static_cast<float>(static_cast<int>(32 - height) / 2);
        paint.text("elgar", {{bounds.origin.x + 2, top + 1}, {bounds.size.x, height}}, title, 0xff000000, 1);
        paint.text("elgar", {{bounds.origin.x, top}, {bounds.size.x, height}}, title, 0xffffffff, 1);
    }
    if (!enabled)
        paint.fill(bounds, 0x28000000);
}
void paint_menu_button(MenuPainter& paint, HudRect r, bool hovered, bool enabled) {
    const auto tint = enabled ? 0xffffffffu : 0x66ffffffu;
    const float u = hovered ? .5f : 0;
    const auto quad = [&](const char* texture, HudRect box, Vec2 a, Vec2 b) {
        paint.quad(id(texture), box, {a, Vec2{b.x, a.y}, Vec2{a.x, b.y}, b}, tint);
    };
    quad("FMp2", {r.origin, {16, r.size.y}}, {u, 0}, {u + .5f, .5f});
    quad("FBbg", {{r.origin.x + 16, r.origin.y}, {r.size.x - 32, r.size.y}}, {0, 0},
         {(r.size.x - 32) / 16, 1});
    quad("FMp2", {{r.origin.x + r.size.x - 16, r.origin.y}, {16, r.size.y}}, {u + .5f, 0}, {u, .5f});
}
void paint_menu_button_label(MenuPainter& paint, HudRect r, std::string_view label, bool hovered,
                             bool enabled, std::uint32_t flags) {
    const auto face = widget_font_name(flags, "verdanacolor");
    const auto height = static_cast<float>(paint.line_height(face));
    const auto top = r.origin.y + static_cast<float>(static_cast<int>(r.size.y - height) / 2);
    paint.text(face, {{r.origin.x + 18, top}, {r.size.x - 36, height}}, label,
               enabled ? (hovered ? 0xffffffffu : 0xffffc3c3u) : 0xff888888u, widget_alignment(flags));
}
void paint_menu_checkbox(MenuPainter& paint, HudRect r, std::string_view label, bool hovered, bool enabled,
                         bool checked, std::uint32_t flags) {
    r.size.y = 16;
    const auto tint = enabled ? 0xffffffffu : 0x66ffffffu;
    const float u = hovered ? .5f : 0, box = checked ? .5f : 0;
    const auto quad = [&](const char* texture, HudRect bounds, Vec2 a, Vec2 b) {
        paint.quad(id(texture), bounds, {a, Vec2{b.x, a.y}, Vec2{a.x, b.y}, b}, tint);
    };
    quad("FMp2", {r.origin, {16, 16}}, {u, 0}, {u + .5f, .5f});
    quad("FMp4", {{r.origin.x + 16, r.origin.y}, {16, 16}}, {box, 0}, {box + .5f, .5f});
    quad("FBbg", {{r.origin.x + 32, r.origin.y}, {r.size.x - 48, 16}}, {0, 0}, {r.size.x / 16 - 3, 1});
    quad("FMp2", {{r.origin.x + r.size.x - 16, r.origin.y}, {16, 16}}, {u + .5f, 0}, {u, .5f});
    const auto face = widget_font_name(flags, "verdanacolor");
    const auto height = static_cast<float>(paint.line_height(face));
    paint.text(face, {{r.origin.x + 36, r.origin.y + std::floor((16 - height) / 2)}, {r.size.x - 52, height}},
               label, enabled ? 0xffffc3c3u : 0xff888888u, 0);
}
void paint_menu_slider(MenuPainter& paint, HudRect r, float position, bool left, bool right) {
    const auto quad = [&](const char* texture, HudRect bounds, Vec2 a, Vec2 b) {
        paint.quad(id(texture), bounds, {a, Vec2{b.x, a.y}, Vec2{a.x, b.y}, b}, 0xffffffffu);
    };
    quad("FMp5", {r.origin, {16, 16}}, {left ? .5f : 0, 0}, {left ? 1.f : .5f, .5f});
    quad("FSbg", {{r.origin.x + 16, r.origin.y}, {r.size.x - 32, 16}}, {0, 0}, {1, 1});
    quad("FMp5", {{r.origin.x + 16 + (r.size.x - 48) * position, r.origin.y}, {16, 16}}, {0, .5f}, {.5f, 1});
    quad("FMp5", {{r.origin.x + r.size.x - 16, r.origin.y}, {16, 16}}, {right ? 1.f : .5f, 0},
         {right ? .5f : 0, .5f});
}
bool option_menu_command(GameOptions& options, FourCC& page, FourCC command) {
    const auto c = command.text();
    if (c == "gopt" || c == "sopt" || c == "copt") {
        page = command;
        return false;
    }
    if (c == "dflt")
        options.reset();
    else if (c == "gdet")
        options.set(command, (options.value(command) + 1) % 8);
    else if (c == "tfil")
        options.set(command, (options.value(command) + 1) % 3);
    else if (c == "wmod")
        options.set(command, (options.value(command) + 1) % 2);
    else if (c == "spek")
        options.set(command, (options.value(command) + 1) % 4);
    else if (options.checked(command))
        options.toggle(command);
    else
        return false;
    return true;
}
void adapt_graphics_page(MenuForm& form) {
    if (form.id != id("gopt"))
        return;
    // Dropped: the resolution list (the window sets the size), and the settings the port does not vary.
    constexpr std::array<const char*, 8> dropped{"resn", "bscr", "xscr", "-fog", "strp", "ctr!", "mscr", "gama"};
    std::erase_if(form.widgets, [&](const MenuForm::Widget& widget) {
        return std::ranges::any_of(dropped, [&](const char* name) { return widget.id == id(name); });
    });
    // Checkboxes move left into the list's column; the rows below close up under them.
    std::optional<MenuForm::Widget> label, button, checkbox;
    float y = 0;
    for (auto& widget : form.widgets) {
        if (widget.kind == 7) {
            widget.rectangle.origin.x = 8;
            y = std::max(y, widget.rectangle.origin.y);
            checkbox = widget;
        }
        if (widget.id == id("GDET"))
            label = widget;
        if (widget.id == id("gdet"))
            button = widget;
    }
    // Added checkboxes under the last one: the framerate readout, drawing between ticks, view shake and the
    // remastered look.
    if (checkbox)
        for (const char* added : {"sfps", "smth", "shak", "rmst"}) {
            auto widget = *checkbox;
            widget.id = widget.text = id(added);
            widget.help = {};
            y += 20;
            widget.rectangle.origin.y = y;
            form.widgets.push_back(widget);
        }
    // The slider and button rows follow 24 apart, then the added window mode row.
    float row_y = y + 32;
    for (const char* row : {"rate", "tfil", "tres", "gdet"}) {
        for (auto& widget : form.widgets)
            if (widget.kind != 7 && (widget.id == id(row) || (widget.kind == 10 && widget.text == id(row))))
                widget.rectangle.origin.y = row_y;
        row_y += 24;
    }
    if (label && button) {
        label->id = id("WMOD");
        label->text = id("wmod");
        label->help = {};
        label->rectangle.origin.y = row_y;
        button->id = id("wmod");
        button->text = id("wm_0");
        button->help = {};
        button->rectangle.origin.y = row_y;
        form.widgets.push_back(*label);
        form.widgets.push_back(*button);
    }
}
std::optional<std::string_view> port_menu_text(FourCC record) {
    static constexpr std::array<std::pair<const char*, std::string_view>, 10> texts{{
        {"sfps", "Show Framerate"},
        {"shak", "Camera Shake"},
        {"smth", "Smooth Motion"},
        {"rmst", "Remastered Look"},
        {"widj", "Vegetation"},
        {"rate", "Max Framerate"},
        {"tbuf", "VSync"},
        {"wmod", "Window Mode"},
        {"wm_0", "Windowed"},
        {"wm_1", "Borderless"},
    }};
    for (const auto& [name, text] : texts)
        if (record == id(name))
            return text;
    return std::nullopt;
}
std::optional<FourCC> option_menu_label(FourCC page, FourCC command, const GameOptions& options) {
    const auto p = page.text(), c = command.text();
    const char* prefix = p == "sopt" && c == "spek"   ? "spk"
                         : p == "gopt" && c == "gdet" ? "gd_"
                         : p == "gopt" && c == "tfil" ? "tf_"
                         : p == "gopt" && c == "wmod" ? "wm_"
                                                      : nullptr;
    if (!prefix)
        return {};
    return FourCC::from_text(std::format("{}{}", prefix, options.value(command)));
}
FrontendRoute frontend_route(FourCC page, FourCC command, int row) {
    const auto p = page.text(), c = command.text();
    const auto open = [](const char* next) { return FrontendRoute{FrontendAction::open, id(next)}; };
    if (p == "espb") {
        if (c == "god_" && row > 0 && row < 10)
            return {FrontendAction::god_path, {}};
        if (c == "echr")
            return open("newc");
        return {};
    }
    if (row >= 0) {
        if (c == "type")
            return {FrontendAction::lobby_type, {}};
        if (c == "team")
            return {FrontendAction::lobby_team, {}};
        if (c == "god_")
            return {FrontendAction::lobby_god, {}};
        if (c == "name")
            return {FrontendAction::lobby_take, {}};
        return {};
    }
    if (p == "titl") {
        if (c == "g_1p")
            return open("1ply");
        if (c == "opts")
            return open("opts");
        if (c == "quit")
            return open("qgam");
    } else if (p == "1ply") {
        if (c == "scmp")
            return {FrontendAction::campaign, {}};
        if (c == "scen")
            return {FrontendAction::skirmish, {}};
        if (c == "menu")
            return open("titl");
    } else if (p == "rost") {
        if (c == "nchr")
            return {FrontendAction::new_character, {}};
        if (c == "echr")
            return {FrontendAction::edit_character, {}};
        if (c == "schr")
            return {FrontendAction::select_character, {}};
        if (c == "dchr")
            return open("delc");
        if (c == "menu")
            return open("1ply");
    } else if (p == "newc") {
        if (c == "wiz+")
            return {FrontendAction::wizard_next, {}};
        if (c == "wiz-")
            return {FrontendAction::wizard_previous, {}};
        if (c == "espb")
            return open("espb");
        if (c == "_OK_" || c == "pnam")
            return {FrontendAction::save_character, {}};
        if (c == "cncl")
            return {FrontendAction::cancel_character, id("rost")};
    } else if (p == "delc") {
        if (c == "ok__")
            return {FrontendAction::delete_character, id("rost")};
        if (c == "cncl")
            return open("rost");
    } else if (p == "loby") {
        if (c == "gset")
            return {FrontendAction::browse_map, id("cgam")};
        if (c == "cncl")
            return open("rost");
        if (c == "strt")
            return {FrontendAction::start_match, {}};
    } else if (p == "cgam") {
        if (c == "ok__")
            return {FrontendAction::accept_map, id("loby")};
        if (c == "cncl")
            return open("loby");
    } else if (p == "qgam") {
        if (c == "ok__")
            return {FrontendAction::quit, {}};
        if (c == "cncl")
            return open("titl");
    }
    return {};
}
bool apply_lobby_action(FrontendAction action, int row, SkirmishLobby& lobby, CrtRandom& random) {
    if (row < 0 || row >= static_cast<int>(lobby.slots.size()))
        return false;
    switch (action) {
    case FrontendAction::lobby_type:
        lobby.cycle_type(row, random);
        break;
    case FrontendAction::lobby_team:
        lobby.cycle_team(row);
        break;
    case FrontendAction::lobby_god:
        lobby.cycle_god(row);
        break;
    case FrontendAction::lobby_take:
        lobby.take(row);
        break;
    default:
        return false;
    }
    return true;
}
int character_name_status(std::string_view name, const std::vector<Character>& characters,
                          const std::optional<std::string>& edited) {
    if (const auto invalid = Character::validate_name(name))
        return invalid;
    for (const auto& c : characters)
        if (std::ranges::equal(
                c.name, name,
                [](unsigned char a, unsigned char b) { return std::tolower(a) == std::tolower(b); }) &&
            (!edited || c.name != *edited))
            return -1;
    return 0;
}
Character edited_character(std::string_view name, std::string_view game_type, FourCC wizard, bool god_path,
                           const std::array<std::uint8_t, 20>& gods, const std::vector<Character>& characters,
                           const std::optional<std::string>& edited) {
    Character result;
    if (edited) {
        const auto it = std::ranges::find(characters, *edited, &Character::name);
        if (it == characters.end())
            throw Error("Edited character no longer exists");
        result = *it;
    }
    result.name = name;
    result.game_type = game_type;
    result.reset_wizard(wizard);
    if (god_path)
        result.gods = gods;
    return result;
}
void save_character_edit(const CharacterRoster& roster, Character& character,
                         const std::optional<std::string>& edited) {
    if (edited && *edited != character.name) {
        const auto name = character.name;
        character.name = *edited;
        roster.rename(character, name);
    } else
        roster.save(character);
}
bool frontend_enabled(FourCC page, std::string_view c, const FrontendAvailability& a) {
    const auto p = page.text();
    if (c == "sgam")
        return a.world_save;
    if (c == "lgam")
        return a.world_load;
    if (c == "g_mp" || c == "cstc" || c.starts_with("tut"))
        return false;
    if (c == "surr")
        return a.surrender;
    if (c == "obsv")
        return a.observer;
    if (c == "vobj")
        return a.objectives;
    if (c == "espb")
        // Port deviation: native also needs a.custom_path (0x4a0060, a character with BeatCampaign); the
        // custom god path is open to every Skirmish character here. See docs/status.md.
        return a.god_path;
    if (p == "titl")
        return c == "g_1p" || c == "opts" || c == "quit";
    if (p == "1ply")
        return c == "scmp" || c == "scen" || c == "menu";
    if (p == "opts")
        return a.detailed_options ? c != "kopt" && c != "cmpt" && c != "cprv" && c != "cdky"
                                  : c == "sopt" || c == "menu";
    if (p == "gopt")
        return a.detailed_options && (c == "gdet" || c == "rate" || c == "widj" || c == "tfil" || c == "dmap" ||
                                      c == "bmap" || c == "tbuf" || c == "wmod" || c == "sfps" ||
                                      c == "smth" || c == "shak" || c == "rmst");
    if (p == "rost" && (c == "schr" || c == "echr" || c == "dchr"))
        return a.selected;
    if (p == "newc" && c == "_OK_")
        return a.valid_name && a.wizards;
    if (c == "strt")
        return a.can_start;
    if (p == "cgam")
        return c == "ok__" || c == "cncl";
    return true;
}
} // namespace sacrifice
