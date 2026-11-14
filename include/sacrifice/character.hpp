#pragma once
#include "wizard.hpp"
#include <filesystem>
#include <optional>
#include <string>
#include <vector>

namespace sacrifice {
class SpellRegistry;
// The native CHARACTER record (FORM:rost profiles): saves\Characters\<game type>\<name>.wiz, 8632 bytes
// written whole by 0x4a0220 and read back by 0x4a02b0. File offsets are the runtime node's minus 16.
struct Character {
    static constexpr std::size_t file_size = 8632;
    static constexpr std::uint16_t version = 3;     // 0x4a00b0.
    static constexpr std::size_t learned_limit = 64; // +180: 256 bytes of definition IDs.
    struct Flag {
        std::string name; // Up to 27 characters, compared case-insensitively (0x4a0640).
        std::int32_t value{};
    };
    std::string name;      // +20, at most 31 characters.
    std::string game_type; // +52: the game type's name (Sacrifice, Scenario, Skirmish...).
    // +84: the campaign position; 0x49ee20 replaces it with the next node when a level completes.
    std::uint32_t campaign_node{};
    FourCC wizard;                                                   // +88, set by FORM:swiz.
    std::array<std::int16_t, 24> stats = WizardProgression::initial_stats; // +92, definition +52.
    std::int32_t maximum_mana{1000}, auxiliary{120};                   // +140/+144 <- definition +144/+140.
    std::array<std::uint8_t, 20> gods{};                             // +148 (god per level, FORM:espb).
    std::int32_t level{}, experience{};                              // +168/+172.
    std::vector<FourCC> learned;                                     // +176 count, +180 IDs.
    std::array<std::byte, 8> reserved{};                             // +436, never written by the game.
    std::int32_t credit_count_80{}, credit_count_20{};               // +444/+448, accumulated.
    std::vector<Flag> flags;                                         // +452 count, +456 32-byte entries.

    [[nodiscard]] static Character decode(ByteView bytes);
    [[nodiscard]] Bytes encode() const;
    [[nodiscard]] std::int32_t flag(std::string_view key) const; // 0x4a06a0: zero when absent.
    void set_flag(std::string_view key, std::int32_t value);
    // 0x4a04d0: a finished wizard definition copied back; the credit counters accumulate.
    void absorb(const WizardProgression& progression);
    // 0x4a0440, run by 0x457750 as a campaign mission places the wizard: the mission god (LEVL dword 9),
    // else the god already chosen for `start`, else rand() % 5 + 1, becomes stat 18 (+128). It fills the
    // unset gods below `start` and every god from `start` on.
    std::uint8_t enter_mission(std::uint32_t god, int start, CrtRandom& random);
    // 0x4a03e0: the chosen wizard's level-zero definition (god zero), as when a character is made.
    void reset_wizard(FourCC id);
    [[nodiscard]] WizardProgression progression() const;
    // 0x49fb90: zero for a valid name. Letters, digits and "!@#$^&*()_-+=|/?[]{}';:<>,.~" count; other
    // characters are dropped. 1..16 counted characters starting with a letter are valid.
    [[nodiscard]] static int validate_name(std::string_view name);
};
// The 15 selectable wizards of 0x4d3838 (WIZD id and the executable's own display name).
struct WizardChoice {
    FourCC id;
    const char* name;
};
extern const std::array<WizardChoice, 15> wizard_choices;
// The character manager (&unk_4DA910) over one directory tree. Characters are listed per game type
// in name order (FindFirstFile order on NTFS).
class CharacterRoster {
  public:
    explicit CharacterRoster(std::filesystem::path root) : root_(std::move(root)) {}
    // Copies a game type's stock characters (installation saves\Characters) the first time it is opened,
    // so the port never writes into the installation.
    void import(const std::filesystem::path& installation, std::string_view game_type) const;
    [[nodiscard]] std::vector<Character> list(std::string_view game_type) const;
    [[nodiscard]] std::vector<Character> list_all() const;
    [[nodiscard]] std::filesystem::path path(const Character& character) const; // 0x4a0140.
    void save(const Character& character) const;                                // 0x4a0220.
    void remove(const Character& character) const;                             // 0x4a0170.
    // 0x4a01a0: the old file is removed and the record written under its new name.
    void rename(Character& character, std::string_view name) const;
    // 0x49fff0: cwe1 needs a character whose BeatCampaign is 1, cwa2 one whose BeatCampaign is 2.
    [[nodiscard]] bool wizard_available(FourCC id) const;
    // 0x4a0060: any character that beat the campaign unlocks the custom god path (FORM:espb).
    [[nodiscard]] bool custom_path_available() const;
    // 0x49e690 (Scenario/Skirmish): the available wizards in table order.
    [[nodiscard]] std::vector<WizardChoice> wizards() const;

  private:
    std::filesystem::path root_;
};
} // namespace sacrifice
