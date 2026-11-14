#pragma once
#include "weather.hpp"
#include "assets.hpp"
#include "sacrifice/core.hpp"
#include <functional>
#include <list>
#include <map>
#include <unordered_map>
#include <memory>

namespace sacrifice {
struct SoundSet;
class SpellRegistry;
struct MatchSide;
struct ObjectMotion;
struct Terrain;
struct Vec3;
struct MrmGeometry;
class SpatialWorld;
class Navigation;
class Match;
class CreatureActor;
class SoulActor;
class BuildingActor;
class Triggers;
class CloudNoise;
class WorldJournal;
struct CreatureDefinition;
// Match objects as Sacrifice.exe runs them (0x453940 pass, 0x46a6c0 common run).
class MatchObject {
  public:
    // The behaviour handler (+24 context, +268 function, run by 0x44bda0): 0 means the handler switched
    // (0x44b770 assigns one by id from the spell function table 0x4dbf74) and the new one runs at once, a
    // positive value ends this tick, -1 clears the handler (0x44b770 with 0), -2 removes the object.
    using Behaviour = std::function<int(MatchObject&, int ticks)>;
    // 0x467d50 types (+876); activation groups (+948) take the masks at 0x4c0468.
    enum Type : std::uint32_t {
        wizard = 1,
        soul = 2,
        creature = 4,
        building = 16,
        spell = 32,
        spell_run = 64,
        type_128 = 128,
    };
    static unsigned group(std::uint32_t type);
    // Watchers queued by 0x44be70: four slots at +308, newest first. A record names the watching object
    // (its +24 context), a key, the handler id run when the slot fires and an argument word (it also
    // keeps the context's owner, +264, which firing swaps in). Slots fired by 0x44bee0: 0 damage taken
    // (0x454950, a 12-byte record), 1 a 12-byte event from 0x482f50, 2 death (0x45a720 creatures,
    // 0x4823f0).
    struct Watcher {
        MatchObject* watcher{};
        std::uintptr_t key{};
        std::uintptr_t handler{};
        std::uint32_t argument{};
    };
    static constexpr std::uint32_t periodic_flag = 8;      // +4: spawn the periodic object every 60 ticks.
    static constexpr std::uint32_t paused_run_flag = 0x80; // +564 low byte sign: runs while paused.
    static constexpr std::uint32_t removed_flag = 0x10000; // +564
    static constexpr std::uint32_t ran_flag = 0x400000;    // +564: set after a run that kept it.
    virtual ~MatchObject() = default;
    // Vtable +176. Returns true when the object is to be removed.
    virtual bool run(Match& match, int ticks);
    virtual bool friendly(const MatchObject* source) const;
    virtual bool enemy(const MatchObject* source) const;
    virtual const ObjectMotion* source_motion() const {
        return nullptr;
    }
    virtual int damage_dealt(Match&, MatchObject&, int, int amount) {
        return amount;
    }
    virtual void reference_event(Match& match, MatchObject& object, int code);
    std::uint32_t reference() const {
        return reference_;
    }
    MatchSide* allegiance{};      // +456, independent of controller/owner.
    std::uint32_t script_id{0xffffffff}; // +1044, the NTTS name trigger code refers to (0x46af00).
    MatchObject* controller{};    // +452.
    MatchObject* owner{};         // +460, used to credit projectiles/spells to a creature/wizard.
    MatchObject* effect_target{}; // +476, cleared by reference events with bit 0x10.
    struct NamedReference {
        FourCC name;
        MatchObject* object{};
    };
    std::vector<NamedReference> named_references;            // +880, eight ordered records (+944 count).
    bool name_reference(FourCC name, MatchObject* object);   // 0x4692a0: unique pair, capacity eight.
    MatchObject* named(FourCC name) const;                   // 0x4693f0: first matching name.
    void unname(FourCC name, const MatchObject* object);     // 0x469300: erase matching pairs.
    std::uint32_t last_damage_source{}, credited_attacker{}; // +1076 / +1072.
    std::uint32_t kind_flags{};                              // +4
    std::uint32_t state_flags{};                             // +564
    std::uint32_t object_flags{};  // +480, including targetability exclusion bit 0x2000000.
    std::uint32_t common_status{}; // +568, distinct from creature status at +2872.
    int phase{};                   // +16 (also copied to +20 on activation)
    int countdown{};               // +868, the side work queue delay (0x473060).
    std::uint32_t seen_by{}, hidden_from{}; // +8 / +12: side identities that recorded / lost the object.
    MatchSide* scan_side{};        // +864: the next side 0x4724e0 examines for this observer.
    int timer{};                   // +1012
    std::uint32_t type{};          // +876
    Behaviour behaviour;
    std::array<std::list<Watcher>, 4> watchers;
    std::uint32_t watch_mask{}; // +304: slots that fire (all after activation).
    // The context's argument stack (+260): firing pushes the slot's words and the watcher's argument.
    std::vector<std::uint32_t> stack;

  protected:
    // Vtable +188: the class update after the common run.
    virtual void update(Match&, int) {}
    // The periodic counter's 0x44c230: a type 32 object running handler 0x4348d0 for spell 42.
    virtual void periodic(Match&);
    // Class parts of activation (vtable +12, after 0x4681f0 clears the common state) and deactivation.
    virtual void activated(Match&) {}
    virtual void deactivated(Match&) {}
    // 0x46a780: model reference and bounds sync, then the spatial update.
    virtual void sync_model(Match&) {}
    friend class Match;

  private:
    std::uint32_t reference_{};      // Stable opaque 32-bit token, including across reuse of a pooled object.
    MatchObject* group_next_{};      // +972, independent of the global run list's +964 links.
    MatchObject** group_previous_{}; // +976, address of the preceding link or group head.
    int run_behaviours(int ticks);
};
// What the ambient picker 0x452090 asks for: one of the world's three SSET groups, a selected SAMP,
// and an ambient effect
// (0x4523e0 with kind and three ranged values).
struct AmbientCue {
    std::optional<FourCC> group; // World category from the picker, including an unresolved/missing group.
    std::optional<FourCC> sample;
    float volume{0.5f}; // 0x452090: flags 0x16000000, category 1, no spatial flags or pitch change.
    int effect_kind{};
    float effect_values[3]{};
};
class Match {
  public:
    std::shared_ptr<WorldJournal> world_journal;
    explicit Match(CrtRandom& random, FourCC tileset) : random_(random), tileset_(tileset) {}
    // 0x4681f0 / 0x493410: activation draws the phase and count from rand() and links the object at the
    // head of the active list, or after `after`.
    MatchObject& activate(std::unique_ptr<MatchObject> object, const MatchObject* after = nullptr);
    // 0x467d50: reuse the newest inactive object of this type from its group, else make one; activated.
    MatchObject& create(std::uint32_t type, const std::function<std::unique_ptr<MatchObject>()>& make,
                        const MatchObject* after = nullptr,
                        const std::function<void(MatchObject&)>& configure = {});
    // 0x468350 (vtable +16, reached by removal): frees its watchers and moves it to the head of its
    // group's inactive list.
    void deactivate(const MatchObject& object);
    MatchObject* resolve(std::uint32_t reference) const;
    void notify_reference(MatchObject& object, int code);
    // 0x491ee0: independent activation-group head order, kind mask, saved-next callback traversal.
    int visit_group(unsigned group, unsigned mask, const std::function<int(MatchObject&)>& callback);
    void attribute_damage(MatchObject& target, int type, int& amount, std::uint32_t source);
    // 0x44be70 / 0x44bfe0 / 0x44c040 on the shared 127-record pool (0x4dd5c8); false when it is empty.
    bool watch(MatchObject& object, unsigned slot, const MatchObject::Watcher& watcher);
    void unwatch(MatchObject& object, unsigned slot, std::uintptr_t key, const MatchObject* watcher);
    void unwatch(MatchObject& object, std::uintptr_t key);
    // 0x44bee0: runs the slot's watchers if the mask allows it, newest first. Each (0x44be00) pushes the
    // words and its argument, swaps in its handler and runs it with 0 ticks until it returns non-zero,
    // then restores the watcher's own handler; the handlers may rewrite the words.
    void fire(MatchObject& object, unsigned slot, std::span<std::uint32_t> words = {});
    // The spell function table (0x4dbf74) behind handler ids; 0x44b770 assigns by id.
    std::map<std::uintptr_t, MatchObject::Behaviour> functions;
    void assign(MatchObject& object, std::uintptr_t id);
    [[nodiscard]] unsigned free_watchers() const {
        return free_watchers_;
    }
    [[nodiscard]] const std::list<std::unique_ptr<MatchObject>>& inactive(unsigned group) const {
        return inactive_.at(group);
    }
    // 0x40a8a0 then 0x452700 / 0x4525d0 for `elapsed` 1/60 s ticks of real time; returns the ticks run.
    int advance(float elapsed);
    // The match update for whole ticks (after the main loop's truncation).
    int tick(int ticks);
    [[nodiscard]] const std::list<std::unique_ptr<MatchObject>>& objects() const {
        return active_;
    }
    [[nodiscard]] const GameClock& clock() const {
        return clock_;
    }
    // Presentation only (not native): an object's position and orientation blended between its pose
    // before the last live tick update and its current pose, by the clock's leftover fraction, so
    // drawing is smooth when frames outnumber the 60 ticks per second. Large jumps are not blended.
    [[nodiscard]] ObjectMotion presented(const MatchObject& object) const;
    // The same blend factor, for presentation state the host keeps itself (the camera).
    [[nodiscard]] float presentation_blend() const;
    bool interpolate{true}; // Port option 'smth': off draws the current pose (whole ticks only).
    CrtRandom& random() {
        return random_;
    }
    bool running = true;                  // dword_4DBB38 bit 8 without 0x80000010: a local match in progress.
    bool paused{}; // dword_4DBB38 bit 0x10: 'paws' (0x451c70) toggles it offline; stops the running pass.
    bool cinematic{}; // dword_4DBB0D: suspend normal orders and the computer-side controller.
    void set_cinematic(bool enabled); // 0x453450 / 0x453400; retain orders behind a kind-33 marker.
    std::uint8_t friendly_damage_scale{}; // dword_4dabc0 low byte; loaded match settings remain pending.
    // Session options +669/+670/+671/+674: 0x451170 zeroes them; SceneMatch fills them from the map's LEVL
    // record as the front end's lobby branch does (0x4a1e10 -> 0x456000).
    std::uint8_t starting_souls{}, minimum_wizard_level{}, maximum_wizard_level{}, experience_scale{};
    std::uint32_t mission_god{}; // +681, campaign branch only: LEVL dword 9, the god 0x457750 hands 0x4a0440.
    std::optional<std::uint8_t> campaign_god; // 0x4a0440 resolves this before the carried wizard is created.
    MatchSide* local_side{}; // dword_4dabb4, set by the future player/side controller host.
    MatchObject* direct_control_target{}; // Third-person camera target; excludes unsolicited steering (0x493b70).
    std::vector<AmbientCue> ambient_cues;
    // Remove persistent emitter audio before a pooled object's reference can be reused.
    std::function<void(std::uint32_t)> stop_looping_sounds;
    std::function<void(class SpellActor&)> flush_spell_sounds;
    // The local interface's lists (0x461e70): reference events with bit 4 (death 15, side change 14, removal -1).
    std::function<void(MatchObject&)> forget_interface;
    LandscapeEffects weather;
    struct WeatherSound { int action{}; FourCC sample, group; float volume{}; };
    std::vector<WeatherSound> weather_sounds;
    std::shared_ptr<const SoundSet> sound_set; // Global SSET:spel; selection runs inline before effect RNG.
    std::shared_ptr<const SpellRegistry> spell_registry;
    // Native global LAND/spatial/model services used by moving spell objects.
    const Terrain* terrain{};
    SpatialWorld* spatial{};
    std::vector<MatchSide*> side_order; // Native SIDE list order, including non-index aliases.
    Navigation* navigation{}; // Native shared path/occupancy service, advanced before the object pass.
    // 0x4525d0 runs the map triggers after the object pass while the local side's game is undecided.
    Triggers* triggers{};
    std::map<FourCC, std::shared_ptr<const MrmGeometry>> spell_models;
    std::map<FourCC, Bytes> spell_mounds;
    std::function<bool(FourCC)> prepare_creature;
    std::function<CreatureActor*(MatchSide&, FourCC, Vec3)> create_script_creature;
    std::function<void(class SpellActor&)> create_summon_preview;
    std::function<void(class SpellActor&)> create_spell_model;
    // 0x44d0b0 → LAND::DentMap 0x160382f0. Match::tick drains the queue onto `terrain`.
    struct TerrainDent { Vec3 position; float radius{}, depth{}; };
    std::vector<TerrainDent> terrain_dents;
    std::uint64_t terrain_revision{}; // Orders queued SCAR and SetTile events within a drawn frame.
    // 0x44cb90: SCAR requests survive their short-lived effect objects until the terrain host applies them.
    struct ScarRequest {
        FourCC id;
        Vec3 position;
        std::uint32_t tick{};
        std::uint64_t order{};
        unsigned width{}, height{}; // Zero uses the whole resource; particle blood stamps use 8x8.
    };
    std::vector<ScarRequest> scar_requests;
    // 0x453260 runs against the local view while the effect ticks; requests wait for the host's camera.
    struct ShakeRequest {
        Vec3 position;
        int amplitude{}; // Pixels at the source.
        float radius{};
    };
    std::vector<ShakeRequest> shake_requests;
    // 0x40c5d0 music requests {category, weight} for the process-wide music manager (MusicDirector).
    struct MusicRequest {
        int category{};
        float weight{};
    };
    std::vector<MusicRequest> music_requests;
    void music(int category, float weight) {
        music_requests.push_back({category, weight});
    }
    [[nodiscard]] FourCC tileset() const {
        return tileset_;
    }
    void shake(Vec3 position, int amplitude, float radius) {
        shake_requests.push_back({position, amplitude, radius});
    }
    // 0x44d0b0 flag 1: queue LAND::DentMap. Match::tick applies the queue to `terrain`.
    void dent(Vec3 position, float radius, float depth);
    void displace(Terrain::Displacement& patch, float scale);
    // LAND::SetEdge through the match terrain; 0 when the land is missing or already a hole.
    int set_edge(int x, int y, int flags = 1, int size = 1);
    // LAND::DentMap for every queued request, then navigation/LOD refresh.
    void apply_terrain_dents();
    std::function<const CreatureDefinition*(FourCC)> creature_definition; // Loaded CRE8 rules, or null.
    // Last argument is the native source/insertion object; ceremonies insert after their spell.
    std::function<CreatureActor*(CreatureActor&, FourCC, Vec3, const MatchObject*)> create_creature;
    std::function<SoulActor*(Vec3, MatchObject*, int)> create_souls;
    // 0x457cb0 offline: definition id, position, yaw (radians), side index, NTTS-style flags, controller
    // (+452, also selecting its side) and the optional mana spout the new primary reserves (mode 2).
    // Returns the primary part, or null when the definition is missing or needs an absent reservation.
    std::function<BuildingActor*(FourCC, Vec3, float, int, std::uint32_t, MatchObject*, MatchObject*)>
        create_building;
    // Particle hosts draw from their own stream (natively the shared rand()) so visuals never move game RNG.
    CrtRandom visual_random;
    bool blood_solid{true}; // dword_4dc03c bit 0 (initially set); graphics settings UI is pending.
    std::shared_ptr<CloudNoise> cloud_noise; // CLOUDPRIM initializes one lattice shared by all clouds.
    // TXTR frame counts for particle materials (TEXTURE vtable +36); one frame when unbound.
    std::function<unsigned(FourCC)> texture_frames;
    bool allied_soul_pickup{};       // Native setting bit 4 at 0x4dabc5.
    bool shared_vision{};            // Native setting bit 0x40: allied sight and minimap edge markers.
    bool projectile_friendly_fire{}; // Native settings bit 8 at 0x4dabc5 (option ffir).

  private:
    friend class WorldJournal;
    void ambient(int ticks);
    CrtRandom& random_;
    FourCC tileset_;
    GameClock clock_;
    struct PresentedPose {
        Vec3 position;
        std::array<float, 9> orientation;
    };
    std::unordered_map<const MatchObject*, PresentedPose> previous_poses_; // Before the last live update.
    int presented_ticks_{1};
    int ambient_timer_ = 60; // 0x4cebc4 starts at 60, so the first tick picks.
    bool land_changed_{}, land_heights_changed_{}; // Refresh navigation once at the end of the tick.
    std::list<std::unique_ptr<MatchObject>> active_;
    std::array<std::list<std::unique_ptr<MatchObject>>, 5> inactive_;
    unsigned free_watchers_ = 127;
    std::uint32_t next_reference_{1};
    std::map<std::uint32_t, MatchObject*> references_;
    std::array<MatchObject*, 10> group_heads_{}; // 0..4 active, 5..9 inactive; 0x493410.
    void unlink_group(MatchObject& object);
    void link_group(MatchObject& object, unsigned group);
    void release_watchers(MatchObject& object);
};
} // namespace sacrifice
