#pragma once
#include "assets.hpp"
#include "core.hpp"
#include "trigger_codes.hpp"
#include <functional>
#include <deque>
#include <list>
#include <set>
#include <span>
#include <tuple>

namespace sacrifice {
class Match;
class MatchObject;
class MatchSides;
struct MatchSide;
struct SpellDefinition;
// TRIG records loaded by 0x47d200: u32 count, then per trigger the flags, the side mask (0x80000000 is
// kept, other masks take a nonzero loader override), the condition and action byte sizes and both code
// streams of 32-bit words.
struct TriggerDefinition {
    std::uint32_t flags{}, sides{};
    std::vector<std::uint32_t> condition, action;
    static std::vector<TriggerDefinition> decode(ByteView bytes, std::uint32_t side_override = 0);
};
// NTTS script point (0x47dd50, 56 bytes): id +0, shape +4 (0x4000000 circle of radius extent.x,
// 0x8000000 box of half extents extent rotated by `angle` degrees), Z-up position +8 raised to the
// ground, angle +20, extents +28 / +32.
struct TriggerMarker {
    std::uint32_t id{}, shape{};
    Vec3 position;
    float angle{};
    Vec2 extent;
};
// The game object's trigger list (+1564) run by 0x47d920 after the object pass. Each trigger keeps one
// instance per side in its mask (0x476c60 at map finalization, appended in side index order) with
// flags = trigger flags | 1: bit 0 retries a false condition 30 ticks later instead of deleting the
// instance, bit 1 deletes it after its actions finish; 0x80000000 marks a met condition whose actions
// are still running. Code the reconstruction does not support yet stops only that instance.
class Triggers {
  public:
    Triggers(std::vector<TriggerDefinition> definitions, MatchSides& sides,
             std::span<const std::string> variables = {}, std::vector<TriggerMarker> markers = {});
    void start(std::uint32_t skirmish_sides = 0); // 0x47d9a0.
    void tick(Match& match); // 0x47d920; the game time (+636) is the match tick count.
    [[nodiscard]] const std::set<std::uint32_t>& unsupported() const {
        return unsupported_;
    }
    [[nodiscard]] std::size_t instances() const;
    // Action 172 calls the game type's +120 with a mission choice (0x4d38c8 order: 0 DEFAULT, 1 LIFE,
    // 2 FIRE, 3 EARTH, 4 AIR, 5 DEATH); only the campaign type records it.
    std::function<void(unsigned)> mission_choice;
    // Action 170 calls the game type's +80 (0x49eef0 for campaigns: a won mission copies the campaign
    // variables into the character, see campaign_values()).
    std::function<void()> mission_over;
    // 0x47d050 under a game type with +12 set: "[CAMPAIGN] name" variables drop the prefix, take flag 2
    // and start from the character flag of that name (0x4a06a0).
    void bind_campaign(const std::function<std::int32_t(std::string_view)>& flag);
    // 0x47d9d0: flag-2 variables as (name, value) for 0x4a0720.
    [[nodiscard]] std::vector<std::pair<std::string, std::int32_t>> campaign_values() const;
    // Smoke diagnostic: nonzero variables as (id, name, value), per-side ones read for `side`.
    [[nodiscard]] std::vector<std::tuple<std::size_t, std::string, std::int32_t>> variable_values(int side) const;
    // Compatibility diagnostic for the former consumed-without-host paths. Named instructions now
    // have hosts; format errors and genuinely unknown extensions stop only their owning instance.
    [[nodiscard]] const std::set<std::uint32_t>& deferred() const {
        return deferred_;
    }
    struct Objective { FourCC text; std::uint32_t flags{}; std::string caption; };
    std::vector<Objective> objectives; // MOBJ order; bit 1 visible, bit 2 completed.
    std::uint32_t interface_locks{}; // dword_4DA924, action 198 bits 1/2/4/8/16.
    [[nodiscard]] bool cinematic() const { return cinematic_; }
    // Presentation advances on match ticks, including headless campaign runs. Coordinates are Z-up.
    struct CameraTrack {
        Vec3 point, start, target;
        std::uint32_t object{};
        float elapsed{}, duration{};
    };
    CameraTrack camera_eye, camera_aim;
    float camera_zoom{.25f}, camera_zoom_target{.25f}; // View +220/+224, action 121.
    unsigned camera_zoom_revision{};
    struct Fade {
        std::uint32_t rgb{};
        float alpha{}, target{}, rate{};
    } fade;
    struct Dialogue {
        std::uint64_t id{};
        std::uint32_t speaker{};
        int god{-1};
        FourCC name, text, sample;
        unsigned style{}, emotion{}, ending{};
        int time{};
        std::string caption;
        float remaining{};
        bool started{};
    };
    struct Choice {
        std::uint64_t id{};
        FourCC question, first, second, sample;
        std::size_t variable{};
    };
    std::deque<Choice> choices; // Actions 206/209, FORM:chos; answers are 1 and 2.
    void answer_choice(unsigned answer);
    std::deque<Dialogue> dialogue;
    [[nodiscard]] std::string speaker_name(const Dialogue& line) const;
    struct JournalEntry {
        FourCC text, sample;
        std::string title, caption;
    };
    std::vector<JournalEntry> journal; // 260/261: stored recordings, never automatic speech (0x474940).
    std::deque<Dialogue> voices; // 258: independent, uncaptioned creature speech.
    std::function<std::string(FourCC)> text;
    std::function<float(FourCC)> sample_seconds;
    void bind_presentation(const ResourceCatalog& catalog);
    void seed_camera(Vec3 eye, Vec3 aim);
    void dismiss_dialogue(bool force = false); // Force also advances a timed cinematic line.
    std::int32_t game_timer{};
    bool timer_visible{};
    struct DisplayValue { std::string name; std::int32_t value{}; };
    [[nodiscard]] std::vector<DisplayValue> display_values() const;
    std::function<bool(const MatchObject&)> is_selected;
    std::function<std::string(FourCC)> filename;
    std::function<void(std::string)> next_mission;
    std::function<void(FourCC)> show_tip; // Zero requests a random tip.
    std::deque<Dialogue> sounds; // Action 98: independent samples, completion suspends their owner.
    struct Beacon { std::uint32_t object{}; float alpha{}, target{1}, rotation{}; };
    std::vector<Beacon> beacons;
    bool close_spell_info{}; // Action 255 ('bkit').

  private:
    friend class WorldJournal;
    // 0x47d050 records (524 bytes): id = TVAR order, value +4 or 32 per-side values for flag 4
    // ("[SIDE]"), flags +132, name +136.
    struct Variable {
        std::string raw, name;
        std::uint32_t flags{};
        std::int32_t value{};
        std::array<std::int32_t, 32> sides{};
        std::uint32_t displayed{};
    };
    struct Instance {
        int side{};
        std::uint32_t flags{};
        std::uint32_t skirmish_sides{}; // Opponents eligible for a stock skirmish result.
        const std::vector<std::uint32_t>* code{};
        std::size_t pc{};
        std::uint32_t wait{};                 // +20, game time before which the instance sleeps.
        std::array<std::uint32_t, 8> locals{}; // +24, cleared with each pass.
        std::uint64_t dialogue{}, choice{};
    };
    struct Trigger {
        TriggerDefinition definition;
        std::uint32_t pending{}; // +16, sides still without an instance.
        std::list<Instance> instances;
    };
    struct Filter {
        std::uint32_t code{}, comparison{}, type{};
        std::int32_t value{};
        MatchObject* object{}; // Codes 17 / 18.
    };
    // 0x47b810 result: mode 0 anywhere, 1 within `radius` of an object's footprint, 2 inside a marker.
    struct Location {
        int mode{};
        MatchObject* object{};
        const TriggerMarker* marker{};
        float radius{};
        float height{2}; // Camera offset; action-location 210 supplies an explicit value.
    };
    // 0x478350 order: kind -1 issues nothing; state bits `clear` then `set` apply to object +564.
    struct Order {
        int kind{};
        MatchObject* target{};
        Vec3 point;
        std::uint32_t set{}, clear{};
        const SpellDefinition* spell{}; // Order 9: SPEL from 0x476f00, stored at native +32.
    };
    Order order(Instance& instance);
    std::uint32_t resource_id(Instance& instance, std::uint32_t tag);
    std::uint32_t ntt_type(Instance& instance);
    std::vector<MatchObject*> object_list(Instance& instance);
    std::vector<MatchObject*> quantified(MatchSide& side, Instance& instance);
    void state(MatchObject* target, std::uint32_t status, bool on);
    void teleport(MatchObject& target, const Location* where);
    static std::int32_t resource(const MatchObject& target, std::uint32_t resource);
    static std::int32_t stat(const class CreatureActor& target, std::uint32_t stat);
    void give_resource(MatchObject& target, std::uint32_t resource, std::int32_t amount);
    void apply(MatchObject& object, const Order& order); // 0x478a70.
    std::vector<TriggerMarker> markers_;
    Location location(Instance& instance, int mode);                // 0x47b810.
    const TriggerMarker& marker(Instance& instance);                // 0x476e10 / 0x47de40.
    static bool inside(const MatchObject& object, const Location& location); // 0x478c50.
    std::vector<Trigger> triggers_;
    MatchSides& sides_;
    Match* match_{};
    std::set<std::uint32_t> unsupported_, deferred_;
    bool cinematic_{}; // dword_4DBB0D, set by actions 109 / 110.
    std::uint64_t dialogue_serial_{};
    std::uint32_t presentation_time_{};
    void advance_presentation();
    void move_camera(CameraTrack& track, const Location& where, unsigned ticks);
    Vec3 camera_object(const MatchObject& object, bool eye) const;
    std::vector<Variable> variables_;
    Variable& variable(Instance& instance);                            // 0x4777d0.
    std::int32_t& value(Variable& variable, const Instance& instance); // Flag 4 indexes the side.
    // 0x478010: an object code accepted by `mask`, then 'clct' (the side's wizard), 'slct' (selection)
    // or a script id searched on `owner` or, without one, on every side (0x452b80).
    MatchObject* object(Instance& instance, MatchSide* owner, std::uint32_t mask);
    const SpellDefinition* spell(Instance& instance); // 0x476f00: code 13 or 135 plus a SPEL id.
    void run(Trigger& trigger);
    std::uint32_t next(Instance& instance);
    bool condition(MatchSide& side, Instance& instance); // 0x47c330.
    bool action(MatchSide& side, Instance& instance);    // 0x479390; false yields until the next tick.
    bool count_condition(MatchSide& side, Instance& instance, int mode); // 0x47bdd0.
    bool count(MatchSide& side, const Filter& filter, const Location& where, const MatchObject* observer = nullptr, int* total = nullptr, std::vector<MatchObject*>* selected = nullptr, const MatchSide* viewer = nullptr);
    Filter filter(Instance& instance);
    bool mask_condition(MatchSide& side, Instance& instance, std::uint32_t mask, std::uint32_t selector); // 0x47bc70.
    std::uint32_t comparison(Instance& instance, std::int32_t& value);   // 0x477a60.
    std::int32_t number(Instance& instance);                             // 0x4774a0.
    MatchSide* side(Instance& instance);                                 // 0x477840.
};
} // namespace sacrifice
