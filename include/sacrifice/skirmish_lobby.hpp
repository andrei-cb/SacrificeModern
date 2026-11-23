#pragma once
#include "scene_match.hpp"
#include "side.hpp"
#include <array>

namespace sacrifice {
// The single-player skirmish lobby behind FORM:loby / slts / slot (handlers 0x4a30e0, 0x4a29c0, 0x4a26f0).
// Each of the five rows is a 12-byte record: state, map availability, god, team, computer name and wizard.
struct LobbySlot {
    enum State : std::uint8_t { open, computer, closed, player };
    State state{open};
    bool available{};       // +1: the map lists a side for this slot nibble.
    std::uint8_t god{};     // +2: 0 random, 1..5 Persephone, Pyro, James, Stratos, Charnel (0x4d3e34).
    std::uint8_t team{};    // +3: 0 none, 1..3 (0x4d3e54 icons ltmx/ltm1..3).
    FourCC name;            // +4: computer TEXT id rwNN (0x44f9d0).
    FourCC wizard;          // +8: computer WIZD from 0x4ce920 (0x44fa20).
};
class SkirmishLobby {
  public:
    static constexpr std::array<std::uint32_t, 5> computer_wizards{0x63776c30, 0x63776530, 0x63776630, 0x63776130,
                                                                   0x63776430}; // cwl0 cwe0 cwf0 cwa0 cwd0
    static constexpr std::array<std::uint32_t, 6> god_names{0x72616e64, 0x70657273, 0x7079726f,
                                                            0x6a616d73, 0x73747261, 0x63686172}; // rand..char
    std::array<LobbySlot, 5> slots{};
    int local{-1};                    // The player's row (byte 2 of dword_4f3994).
    FourCC player_wizard{0x63776430}; // Profile wizard; profiles (FORM:rost) are not reconstructed.
    // Rows follow the map's side nibbles. The player takes the first available row; the others start as
    // computers so a new skirmish is immediately playable (the native session's opening states are not
    // recovered).
    void bind(std::span<const SideDefinition> sides, CrtRandom& random);
    // 0x4a0cb0: open -> computer (new name/wizard, random god, no team) -> closed -> open.
    void cycle_type(int row, CrtRandom& random);
    void cycle_team(int row); // 0x4a0c10: (team + 1) & 3.
    void cycle_god(int row);  // 0x4a0b50: (god + 1) % 6; observer/custom entries need multiplayer/profiles.
    void take(int row);       // 0x4a1160: the player moves to an open or computer row.
    // 0x4a1800 offline: at least two participants that are not all on one team; port also allows a solo player.
    [[nodiscard]] bool can_start() const;
    [[nodiscard]] std::vector<SkirmishSlot> session() const;
};
} // namespace sacrifice
