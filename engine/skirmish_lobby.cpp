#include "sacrifice/skirmish_lobby.hpp"
#include <cstdio>

namespace sacrifice {
namespace {
// 0x44f9d0: sprintf("rw%02d", rand() % 50), byte-reversed into a TEXT id.
FourCC computer_name(CrtRandom& random) {
    char text[8];
    std::snprintf(text, sizeof text, "rw%02d", static_cast<int>(random.next() % 50));
    return FourCC::from_text(text);
}
} // namespace

void SkirmishLobby::bind(std::span<const SideDefinition> sides, CrtRandom& random) {
    slots = {};
    local = -1;
    for (unsigned row = 0; row < slots.size(); ++row) {
        for (const auto& side : sides) {
            const auto nibble = side.flags & 0xf ? side.flags & 0xf : (side.flags & 0x20 ? 1u : 0u);
            if (nibble == row + 1)
                slots[row].available = true;
        }
        if (!slots[row].available)
            continue;
        if (local < 0) {
            local = static_cast<int>(row);
            slots[row].state = LobbySlot::player;
        } else {
            slots[row].state = LobbySlot::computer;
            slots[row].name = computer_name(random);
            slots[row].wizard = FourCC{computer_wizards[random.next() % 5]};
        }
    }
}

void SkirmishLobby::cycle_type(int row, CrtRandom& random) {
    if (row < 0 || row >= static_cast<int>(slots.size()))
        return;
    auto& slot = slots[static_cast<std::size_t>(row)];
    switch (slot.state) {
    case LobbySlot::open:
        if (!slot.available) { // 0x4a2080 / +1: an unlisted row skips straight to closed.
            slot.state = LobbySlot::closed;
            break;
        }
        slot.state = LobbySlot::computer;
        slot.name = computer_name(random);
        slot.wizard = FourCC{computer_wizards[random.next() % 5]};
        slot.god = slot.team = 0;
        break;
    case LobbySlot::computer:
        slot.state = LobbySlot::closed;
        break;
    case LobbySlot::closed:
        slot.state = LobbySlot::open;
        break;
    case LobbySlot::player: // The local row cannot be cycled; other players would be kicked (BOOT).
        break;
    }
}

void SkirmishLobby::cycle_team(int row) {
    if (row >= 0 && row < static_cast<int>(slots.size())) {
        auto& slot = slots[static_cast<std::size_t>(row)];
        if (slot.state == LobbySlot::computer || slot.state == LobbySlot::player)
            slot.team = static_cast<std::uint8_t>((slot.team + 1) & 3);
    }
}

void SkirmishLobby::cycle_god(int row) {
    if (row >= 0 && row < static_cast<int>(slots.size())) {
        auto& slot = slots[static_cast<std::size_t>(row)];
        if (slot.state == LobbySlot::computer || slot.state == LobbySlot::player)
            slot.god = static_cast<std::uint8_t>((slot.god + 1) % 6);
    }
}

void SkirmishLobby::take(int row) {
    if (row < 0 || row >= static_cast<int>(slots.size()) || row == local)
        return;
    auto& target = slots[static_cast<std::size_t>(row)];
    if (!target.available || target.state == LobbySlot::closed)
        return;
    if (local >= 0) {
        auto& previous = slots[static_cast<std::size_t>(local)];
        previous.state = LobbySlot::open;
        target.god = previous.god;
        target.team = previous.team;
    }
    target.state = LobbySlot::player;
    local = row;
}

bool SkirmishLobby::can_start() const {
    int participants = 0, teams = 0;
    unsigned seen = 0;
    for (const auto& slot : slots) {
        if (!slot.available || (slot.state != LobbySlot::computer && slot.state != LobbySlot::player))
            continue;
        ++participants;
        if (!slot.team)
            teams += 2; // A player without a team counts as its own side.
        else if (!(seen & (1u << slot.team))) {
            seen |= 1u << slot.team;
            ++teams;
        }
    }
    return local >= 0 && (participants == 1 || (participants > 1 && teams > 1));
}

std::vector<SkirmishSlot> SkirmishLobby::session() const {
    std::vector<SkirmishSlot> result(slots.size());
    for (std::size_t row = 0; row < slots.size(); ++row) {
        const auto& slot = slots[row];
        auto& entry = result[row];
        if (slot.state == LobbySlot::player) {
            entry = {SkirmishSlot::Kind::local, player_wizard, slot.god};
        } else if (slot.state == LobbySlot::computer) {
            entry = {SkirmishSlot::Kind::computer, slot.wizard, slot.god};
        }
        entry.team = slot.team;
    }
    return result;
}
} // namespace sacrifice
