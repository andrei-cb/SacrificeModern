#pragma once
#include "building.hpp"
#include "match.hpp"
#include "spatial.hpp"

namespace sacrifice {
// Building-part activation/model/spatial/common-motion portion of 0x457cb0 and 0x465910.
// Spell/effect/audio requests remain explicit until their native hosts are reconstructed.
class BuildingActor : public MatchObject {
  public:
    ~BuildingActor() override;
    void initialize(const MrmGeometry* model, Vec3 position, float yaw, std::uint32_t map_flags,
                    std::uintptr_t side, const Terrain& terrain, SpatialWorld& spatial);
    void configure(Match& match, const Building& definition, unsigned part,
                   std::array<const MrmGeometry*, 3> models, unsigned initial_state);
    bool run(Match& match, int ticks) override;
    void set_stage(Match& match, unsigned state); // 0x465300 applies to the primary and all linked parts.
    // 0x465f40; a secondary cannot initiate destruction. A revealed building reports side event 16
    // with the destroyer, whose side counts it (code 25).
    void destroy(Match& match, std::uint32_t source = 0);
    // 0x464ff0 offline: only a primary with definition bit 0x10 accepts. Mode 2 replaces any reservation;
    // other modes return an existing one unchanged. Returns the reservation, or null when refused.
    MatchObject* reserve(MatchObject* object, int mode);
    // 0x4657a0/0x4657e0: newest guard first; creature +1480 is the reverse link.
    void add_guard(Match& match, CreatureActor& actor);
    void remove_guard(CreatureActor& actor);
    std::vector<CreatureActor*> guards;
    bool damageable() const;
    struct Ritual {
        CreatureActor* victim{}; // +1112.
        CreatureActor* wizard{}; // +1116.
        FourCC action;           // +1120.
        int remaining{}, fraction{}, initial_health{}, damage_rate{}, mana_rate{}, fixed_rate{};
    } ritual;
    bool request_sacrifice(FourCC action, CreatureActor* victim, CreatureActor* doctor,
                           CreatureActor* wizard);
    Vec3 sacrifice_point() const; // 0x4650b0, rotated definition parameters +100..108.
    FourCC part_name() const {
        return part_ ? part_->name : FourCC{};
    }
    FourCC definition_id; // +1160: BLDG resource id for TEXT/ICON, distinct from part name (+1220).
    float model_scale{1}; // MRMINSTANCE::SetScale, distinct from construction reveal.
    const ObjectMotion* source_motion() const override {
        return &motion;
    }
    void reference_event(Match& match, MatchObject& object, int code) override;
    struct Damage {
        std::uint32_t kind{};
        std::int32_t amount{};
        std::uint32_t source{}; // Native 32-bit identity carried through watcher words, not a host pointer.
    };
    bool damage(Match& match,
                Damage value); // Allied scaling/source attribution; ritual redistribution pending.
    void accumulate_damage(std::int32_t amount, int ticks, std::uint32_t source = 0);
    // 0x4650d0 arithmetic, used by the future ordered side/radius scan (0x465880).
    void supply_mana(std::int16_t affinity, std::int32_t& mana, std::int32_t maximum, std::int32_t& fraction,
                     int ticks) const;
    // 0x465090: the definition's effect offset (+1192, BLDG +88) through the building frame.
    Vec3 effect_point() const;
    unsigned stage() const {
        return static_cast<unsigned>(stage_);
    }
    const MrmGeometry* model() const {
        return model_;
    }
    ObjectMotion motion;
    std::uintptr_t side{};
    BuildingActor* primary{}; // +1096, null for the primary part itself.
    MatchObject* reserved{};  // +1148, disables mana and controls sound lifecycle.
    float reveal{1};          // +1092: construction reveal, copied to MRMINSTANCE +208 (not geometric scale).
    std::uint32_t building_flags{}; // +1156, cleared on ruin.
    std::int32_t health{}, maximum_health{}, threshold{}, complement_threshold{};
    std::int32_t network_identity{
        -1}; // Original local activation uses -1; zero disables direct health edits.
    std::int32_t accumulated_damage{}, damage_clock{};
    float regeneration_fraction{}, regeneration_rate{};
    std::int32_t mana_rate{}, mana_clock{}, proximity_clock{};
    bool health_dirty{};
    int completed{}; // +1276 active level (script 204/205); setup writes 1, ruin clears it.
    std::uintptr_t sound_handle{};    // Effect audio voice key, assigned by the host on sound_start.
    struct Request {
        enum class Kind {
            handler,
            spell,
            damage_watcher,
            side_event,
            notify,
            ruin_effect,
            building_spawn, // Recorded only when the host has no building factory.
            sound_start,
            sound_stop,
            proximity_scan,
            mana_scan
        } kind;
        std::uint32_t code{};
        MatchObject* target{};
        int ticks{};
    };
    std::vector<Request> requests;
    std::vector<int> death_requests;

  protected:
    void activated(Match&) override;
    void deactivated(Match&) override;
    void sync_model(Match&) override;
    void update(Match&, int ticks) override;

  private:
    SpatialWorld* spatial_{};
    Match* match_{};
    const Terrain* terrain_{};
    const MrmGeometry* model_{};
    std::array<const MrmGeometry*, 3> models_{};
    const Building* definition_{};
    const Building::Part* part_{};
    int stage_{-1};
    std::uint32_t damage_source_{};
    Orientation model_orientation_{};
    float bounds_scale_{1};
    bool model_dirty_{true};
    void transition(Match& match, unsigned state);
    void prepare_bounds();
    void tick_damage(Match& match, int ticks);
    void scan_mana(Match& match, int ticks); // 0x465880 / 0x492230 / 0x492300.
    void scan_proximity(Match& match);       // 0x465830, first list-order qualifying wizard.
};
} // namespace sacrifice
