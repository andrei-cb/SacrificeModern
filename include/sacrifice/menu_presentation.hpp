#pragma once
#include "character.hpp"
#include "menu_form.hpp"
#include "options.hpp"
#include "skirmish_lobby.hpp"

namespace sacrifice {
struct MenuKeys {
    bool accept{}, cancel{};
    FourCC action(const MenuForm& form, bool just_opened) const;
};
class MenuPainter {
  public:
    virtual ~MenuPainter() = default;
    virtual void quad(FourCC texture, HudRect box, std::array<Vec2, 4> uv, std::uint32_t colour) = 0;
    virtual void fill(HudRect box, std::uint32_t colour) = 0;
    virtual int line_height(std::string_view face) = 0;
    virtual void text(std::string_view face, HudRect box, std::string_view text, std::uint32_t colour,
                      unsigned alignment) = 0;
};
std::string_view widget_font_name(std::uint32_t flags, std::string_view fallback);
unsigned widget_alignment(std::uint32_t flags);
void paint_menu_panel(MenuPainter& paint, const MenuForm& form, HudRect bounds, bool enabled,
                      std::string_view title);
void paint_menu_button(MenuPainter& paint, HudRect box, bool hovered, bool enabled);
void paint_menu_button_label(MenuPainter& paint, HudRect box, std::string_view label, bool hovered,
                             bool enabled, std::uint32_t flags);
void paint_menu_checkbox(MenuPainter& paint, HudRect box, std::string_view label, bool hovered, bool enabled,
                         bool checked, std::uint32_t flags);
void paint_menu_slider(MenuPainter& paint, HudRect box, float position, bool left, bool right);
// Applies settings or changes the selected subpage; navigation back stays with the host.
bool option_menu_command(GameOptions& options, FourCC& page, FourCC command);
std::optional<FourCC> option_menu_label(FourCC page, FourCC command, const GameOptions& options);
// Port layout of FORM:gopt (not native): drops the resolution list and the fixed settings, moves the
// checkboxes left, and adds the Window Mode row. Applied once after decoding.
void adapt_graphics_page(MenuForm& form);
// Port labels for renamed or added option texts (Vegetation, Max Framerate, VSync, Window Mode).
std::optional<std::string_view> port_menu_text(FourCC record);
enum class FrontendAction {
    unhandled,
    open,
    campaign,
    skirmish,
    new_character,
    edit_character,
    select_character,
    delete_character,
    save_character,
    cancel_character,
    wizard_next,
    wizard_previous,
    browse_map,
    accept_map,
    start_match,
    quit,
    god_path,
    lobby_type,
    lobby_team,
    lobby_god,
    lobby_take
};
struct FrontendRoute {
    FrontendAction action{};
    FourCC destination;
};
FrontendRoute frontend_route(FourCC page, FourCC command, int row);
bool apply_lobby_action(FrontendAction action, int row, SkirmishLobby& lobby, CrtRandom& random);
int character_name_status(std::string_view name, const std::vector<Character>& characters,
                          const std::optional<std::string>& edited);
Character edited_character(std::string_view name, std::string_view game_type, FourCC wizard, bool god_path,
                           const std::array<std::uint8_t, 20>& gods, const std::vector<Character>& characters,
                           const std::optional<std::string>& edited);
void save_character_edit(const CharacterRoster& roster, Character& character,
                         const std::optional<std::string>& edited);
struct FrontendAvailability {
    bool selected{}, valid_name{}, wizards{}, god_path{}, custom_path{}, can_start{}, objectives{};
    bool detailed_options{}, surrender{}, observer{}, world_save{}, world_load{};
};
bool frontend_enabled(FourCC page, std::string_view command, const FrontendAvailability& available);
} // namespace sacrifice
