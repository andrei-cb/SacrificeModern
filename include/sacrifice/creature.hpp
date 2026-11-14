#pragma once
#include "core.hpp"
#include <functional>

namespace sacrifice {
// CRE8/WIZD share the serialized prefix copied eight bytes into the runtime definition by
// 0x44f160/0x44f2f0. WIZD's runtime defaults/experience adjustments are separate from this decoder.
struct CreatureDefinition {
    FourCC name, id, icon;
    std::uint32_t kind{};                 // 2 CRE8, 8 WIZD (not the match object's type mask).
    std::int32_t value{};                 // Serialized +24, runtime +32: 0x482d90 kill credit is five times this.
    std::uint16_t rank{};                 // Serialized +40: the acknowledging member outranks the rest.
    std::array<std::int16_t, 24> stats{}; // Serialized +44, runtime +52.
    std::array<FourCC, 8> spells{};
    FourCC run_spell;
    std::int32_t melee_damage_type{}, melee_damage{}; // Serialized +128/+132, runtime +136/+140.
    std::int32_t mana{};
    FourCC behaviour_fsm, animation_fsm, body;
    std::uint32_t flags{};
    std::vector<FourCC> sounds;
    std::array<FourCC, 64> animations{};
    // 0x455390: up to eight 12-byte event-triggered spell requests after the animation slots.
    struct EventSpell {
        std::int32_t event{};
        std::uint16_t chance{}, flags{};
        FourCC spell;
    };
    std::array<EventSpell, 8> event_spells{};
    static CreatureDefinition decode(ByteView data);
};

// The three 24-short arrays at creature +2244/+2292/+2340. Spell handlers edit target and set bits;
// 0x45a670 approaches by 10 per tick, resets targets to base, and skips health and slots 18/19.
struct CreatureStats {
    static constexpr unsigned health = 5, regeneration = 6, resistance = 9;
    std::array<std::int16_t, 24> current{}, base{}, target{};
    std::uint32_t changed{};
    void reset(const std::array<std::int16_t, 24>& values);
    void advance(int ticks);
};

// The 16-entry table at +2396 (wizards have three more tables). 0x44ffd0 inserts after equal sort
// keys and ignores duplicate ids except for setting flag 1. 0x4501b0 advances active cooldowns only.
struct CreatureSpellBook {
    struct Entry {
        FourCC id;
        std::uint16_t priority{};
        std::uint32_t flags{};
        int parameter{}, index{-1};  // Native entry +12/+16, distinct from the cooldown words.
        int remaining{}, duration{}; // +20/+24: duration is retained until the countdown completes.
    };
    std::vector<Entry> entries;
    bool add(FourCC id, std::uint16_t priority, bool enabled = false);
    void level(int value); // 0x450140: availability bit 2, with an explicit-enabled override.
    void cooldown(FourCC id, int base, int extra, const std::function<int(FourCC)>& reload);
    void advance(int ticks);
};

// 0x45a210: health regeneration in 16.16, scaled by the current/max mana ratio. The caller supplies
// the alive/permission gates (FSM 0x800, CRE8 0x2000, or an eligible manalith), as 0x45a300 does.
struct CreatureRegeneration {
    std::int32_t fraction{};
    bool advance(CreatureStats& stats, int ticks, std::int32_t mana, std::int32_t maximum_mana, bool alive,
                 bool allowed);
};

// FSMC layout read by 0x45e240/0x45e2b0/0x45e320. Keep function names as data; the reconstruction
// binds names to native functions, never to original executable addresses.
struct CreatureFsm {
    struct Link {
        FourCC group, state;
    };
    struct Event {
        std::int32_t id{}, animation{-1};
        Link destination;
        std::string handler;
        std::optional<std::uint8_t> network_index;
    };
    struct State {
        FourCC id;
        std::int32_t animation{-1};
        std::uint32_t flags{};
        std::string name, enter, run, exit;
        std::vector<Event> events; // First matching event wins (0x45e800).
    };
    struct Group {
        FourCC id;
        std::string name;
        std::uint32_t flags{};
        std::vector<State> states;
    };
    struct Location {
        std::size_t group{}, state{};
        auto operator<=>(const Location&) const = default;
    };
    FourCC id;
    std::string name;
    Link initial;
    std::vector<Group> groups;
    std::vector<std::int32_t> event_ids; // Sorted, unique, at most 255 (0x45e410).
    static CreatureFsm decode(ByteView data);
    // Loading links groups/states at the head; resolve in reverse serialized order (0x45e600/620).
    [[nodiscard]] std::optional<Location> resolve(Link link, std::optional<std::size_t> local = {}) const;
    [[nodiscard]] const State& at(Location where) const;
};

// The state/transition part of 0x4550f0 and offline 0x454d90. Model/physics flags, animation changes,
// and event notifications belong to the host. An absent nonempty handler fails explicitly.
class CreatureFsmRunner {
  public:
    explicit CreatureFsmRunner(const CreatureFsm& definition) : definition_(definition) {}
    using Handler = std::function<int(CreatureFsmRunner&, std::string_view, int)>;
    Handler handler;
    std::function<void(const CreatureFsm::State*, const CreatureFsm::State&)> changed;
    std::function<void(int, std::uint32_t)> animation;
    std::function<void(int)> notified;
    int start();
    int enter(CreatureFsm::Location destination);
    int event(std::int32_t id);
    void run(int ticks);
    [[nodiscard]] const CreatureFsm::State* state() const;
    [[nodiscard]] std::optional<CreatureFsm::Location> location() const {
        return current_;
    }

  private:
    int call(std::string_view name, int ticks);
    const CreatureFsm& definition_;
    std::optional<CreatureFsm::Location> current_;
};
} // namespace sacrifice
