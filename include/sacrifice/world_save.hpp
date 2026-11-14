#pragma once
#include "character.hpp"
#include "player.hpp"
#include "scene_match.hpp"
#include "storage.hpp"

namespace sacrifice {
// Stable IDs and simulation commands; no native object memory or function pointers.
class WorldJournal {
  public:
    enum class Command {
        order,
        protect_wizard,
        cast,
        spell_key,
        ability_key,
        cast_slot,
        resolve_target,
        formation,
        dismiss,
        choice,
        debug_wizard
    };
    explicit WorldJournal(Match& match, PlayerControl* player = nullptr);
    void player(PlayerControl* player) {
        player_ = player;
    }
    struct Scope {
        WorldJournal* journal{};
        Scope(Match& match, Command command, nlohmann::json arguments);
        ~Scope();
    };
    void begin_tick(int ticks);
    void end_tick();
    void input(std::uint32_t object, std::array<int, 3> axes);
    nlohmann::json save() const;
    void restore(const nlohmann::json& document);
    bool replaying() const {
        return replaying_;
    }
    static nlohmann::json point(Vec3 point);
    static std::uint32_t reference(const MatchObject* object) {
        return object ? object->reference() : 0;
    }

  private:
    Match& match_;
    PlayerControl* player_{};
    std::vector<std::uint8_t> tape_;
    nlohmann::json frame_;
    unsigned depth_{};
    bool ticking_{}, replaying_{};
    std::string failure_;
    void append(const nlohmann::json& event);
    nlohmann::json context() const;
    void context(const nlohmann::json& value);
    nlohmann::json controls() const;
    void controls(const nlohmann::json& value);
    nlohmann::json fingerprint() const;
    void dispatch(Command command, const nlohmann::json& arguments);
};

// Named saved games, as native saves\games\<name>.sac (0x4a7940). The payload is the port's world
// continuation, not the native object dump, so the port keeps its own extension in its own folder.
struct WorldSave {
    nlohmann::json setup, world;
    // Native name rule 0x4a7920: 1..15 characters. The port also rejects characters a file name
    // cannot carry.
    static constexpr std::size_t name_limit = 15;
    static constexpr std::string_view quick_name = "Quicksave"; // F1/F2, 0x4a56a0 / 0x4a5710.
    static constexpr unsigned thumbnail_size = 128;              // 0x4a7610: 128x128, 16-bit.
    static bool valid_name(std::string_view name);
    static std::filesystem::path directory();
    static std::filesystem::path path(std::string_view name, const std::filesystem::path& folder = directory());
    // Saved names in case-insensitive order (FindFirstFile over saves\games\*.sac, 0x4a7960).
    static std::vector<std::string> names(const std::filesystem::path& folder = directory());
    static bool exists(std::string_view name, const std::filesystem::path& folder = directory());
    static std::optional<WorldSave> read(std::string_view name, const std::filesystem::path& folder = directory());
    void write(std::string_view name, const std::filesystem::path& folder = directory()) const;
    static void remove(std::string_view name, const std::filesystem::path& folder = directory()); // 0x4a78e0
    // The description lines of 0x4a5830 'desc': header +60 (profile), +28 (game type), +92 (map) and +24
    // (time, "%x  %I:%M %p").
    struct Header {
        std::string profile, game_type, map;
        std::int64_t time{};
    };
    Header header() const;
    void header(const Header& value);
    // 0x4a7a70: the header as a save is written: the map path with backslashes and time(nullptr).
    void stamp(std::string profile, std::string game_type);
    // 0x4a76f0 / 0x4a7680: the 128x128 scene captured when the game menu opens, stored as RGB565.
    std::optional<Image> thumbnail() const;
    void thumbnail(const Image& image);
    static Image scale_thumbnail(const Image& frame);
    std::filesystem::path mission(const std::filesystem::path& root) const;
    static nlohmann::json map(const std::filesystem::path& root, const std::filesystem::path& path);
    static WorldSave capture(const std::filesystem::path& root, const std::filesystem::path& path,
                             SceneMatch& scene, std::span<const SkirmishSlot> slots,
                             const std::optional<Character>& starting_character);
    std::vector<SkirmishSlot> slots() const;
    std::optional<Character> character() const;
};
} // namespace sacrifice
