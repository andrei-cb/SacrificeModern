#pragma once
#include "creature.hpp"
#include "creature_orders.hpp"
#include "match.hpp"
#include "motion.hpp"
#include "navigation.hpp"
#include "saxs.hpp"
#include "wizard.hpp"

namespace sacrifice {
class SpatialWorld;
class SpellActor;
class BuildingActor;
struct SpellDefinition;
struct SpellData;
struct SoundSet;
// 0x453340: keeps `point` when it is on land and free for `mask`, otherwise tries up to 32 random points
// within `radius` of `centre`; false when none was free.
bool open_point(Match& match, Vec3& point, Vec3 centre, float radius, unsigned mask);
// 0x458ae0: ordered summon offsets in the source transform, then random alternatives.
// Scripted creation (0x478f40) first tries the source position itself.
bool find_creature_point(Match& match, Vec3& point, Vec3 centre, const Orientation& orientation,
                         bool try_centre = false);
struct CreatureClip {
    FourCC id;
    SaxsAnimation animation;
    SaxsSequence sequence;
    float rate{};           // SAXSANIM +16: frames per 1/60 s tick.
    float attack_seconds{}; // +20: seconds per clip / count of atck triggers (or whole duration).
};
// Shared immutable definitions/clips. The model's bone count gates animation selection (0x455210).
struct CreatureActorResources {
    CreatureDefinition definition;
    CreatureFsm fsm;
    unsigned bones{};
    std::array<std::shared_ptr<const CreatureClip>, 64> clips;
    std::vector<std::shared_ptr<const SoundSet>> sound_sets; // Definition order, 0x495530 first hit.
    static CreatureActorResources load(const ResourceCatalog& catalog, CreatureDefinition definition,
                                       unsigned bones);
};

// Creature 0x45a620: animation/stats, common run and the 0x45a8b0 movement controller.
// Native order execution, navigation, approach and combat are bound; behavior FSMs, remaining spell
// bodies and side/player controllers remain reconstruction work.
class CreatureActor : public MatchObject {
  public:
    explicit CreatureActor(std::shared_ptr<const CreatureActorResources> resources);
    ~CreatureActor() override;
    // Rebind immutable definition data before activation when the type pool reuses this object.
    void configure(std::shared_ptr<const CreatureActorResources> resources);
    bool run(Match& match, int ticks) override;
    bool friendly(const MatchObject* source) const override;
    bool enemy(const MatchObject* source) const override;
    const ObjectMotion* source_motion() const override {
        return &motion;
    }
    int damage_dealt(Match& match, MatchObject& target, int damage_type, int amount) override;
    void reference_event(Match& match, MatchObject& object, int code) override;
    void earn_credit(int amount, std::uint32_t flags);
    // Vtable +156 (0x482e10, kill, flag 0x80) and +160 (0x482eb0, gib, flag 0x20) on the attacker:
    // an enemy victim's +148 value (0x482d90 / 0x482db0) becomes credit.
    void credit_kill(const CreatureActor& victim, std::uint32_t flags);
    int kill_value() const;
    const CreatureDefinition& definition() const {
        return resources_->definition;
    }
    std::uint32_t friendly_exception_mask{}; // +2896, active with definition property 0x4000.
    MatchObject* ritual_building{};          // +1480: damage dealt gains a 1.5 multiplier while bound.
    void set_guard(Match& match, BuildingActor* building); // Offline 0x45e910.
    void run_guard(Match& match);                         // 0x45e980, before damage/order execution.
    BuildingActor* home_altar{};             // Wizard +2972, distinct from the creature ritual link.
    bool computer_wizard{};                  // Wizard +2944, set by 0x4818a0 for computer slots.
    BuildingActor* doctor_altar{};           // Sacdoctor +1492, destination of the current ritual.
    FourCC doctor_action;                    // Context +76, sacc or sacu.
    int doctor_spirit_ticks{}, doctor_beam_ready{}, doctor_permission{}; // +84 / +64 / +88.
    SpellActor* blood_host{}; // Native +2240, cleared by reference loss and pooled activation.
    std::uint32_t permanent_burns{}, temporary_burns{}; // +2224 / +2228; x86 limb shifts wrap at 32.
    int burn_ticks{}; // +2232, longest requested temporary duration.
    SpellActor* flame_host{}; // +2236.
    struct DoctorCircle {
        Vec3 center;
        float minimum{15}, maximum{20}, angle{}, current_angle{}, distance_squared{};
        bool arrived{}, active{};
    } doctor_circle; // Native 0x44a410 behaviour context, separate from the FSM.
    int run_doctor_circle(Match& match, int ticks);
    bool doctor_valid() const;
    int doctor_handler(std::string_view name, int ticks);
    void initialize_doctor(MatchObject* target, BuildingActor* altar, FourCC action, float height);
    void refresh_cast_requirements(Match& match); // 0x4821e0.
    int wizard_handler(std::string_view name, int ticks);
    int request_respawn();                   // 0x4824f0: -1 pending, 0 denied, 1 accepted.
    void run_wizard(Match& match);           // 0x482030 before the creature pre-update.
    void eliminate_wizard(Match& match);     // 0x4824a0.
    bool relocate(Match& match, Vec3 point); // Offline 0x442610; open_point (0x453340) picks the point.
    void kill(Match& match);                 // 0x45a7c0(0, 0), the trigger order 136.
    struct CreditRequest {
        int amount{};
        std::uint32_t flags{};
    };
    std::vector<CreditRequest> credit_requests; // Diagnostic history; experience executes inline.
    WizardProgression progression;
    struct WizardProfile {
        int level{};
        std::uint8_t god{};
    };
    std::optional<WizardProfile> initial_wizard_profile; // Consumed before initial FSM entry.
    // A campaign character's carried definition (0x4a1c30's session wizard): progression, stats, mana
    // and the three learned books replaced on the already placed wizard.
    void adopt_progression(Match& match, const WizardProgression& carried);
    struct LevelRequest {
        int change{}, level{}, event{}; // FSM event executes inline; area audio/effects remain requests.
    };
    std::vector<LevelRequest> level_requests;
    struct GrantRequest {
        int level{}, god{}; // 0x44fb50 boundary, retained only when the match has no spell registry.
    };
    std::vector<GrantRequest> grant_requests;
    void set_experience(int level, int experience);    // Offline 0x483200 mode 0 + 0x44fc90.
    std::int32_t credit_count_80{}, credit_count_20{}; // +2888 / +2892.
    CreatureFsmRunner& fsm() {
        return *fsm_;
    }
    const CreatureFsmRunner& fsm() const {
        return *fsm_;
    }
    const CreatureClip* clip() const {
        return clip_.get();
    }
    std::shared_ptr<const CreatureClip> clip_handle() const { return clip_; }
    const SaxsAsset* body() const {
        return body_.get();
    }
    bool has_model() const {
        return body_ && clip_;
    }
    float frame() const {
        return frame_;
    }
    std::uint64_t pose_revision() const {
        return pose_revision_;
    }
    bool select_animation(int slot, std::uint32_t flags = 0);
    bool select_clip(std::shared_ptr<const CreatureClip> clip, std::uint32_t flags = 0);
    int native_handler(std::string_view name, int ticks);
    // SAXS view preparation updates skeleton/bounds; the following common run consumes model dirtiness.
    void bind_body(std::shared_ptr<const SaxsAsset> body);
    // The spatial world must outlive this binding (including the actor's inactive pool lifetime).
    void bind_spatial(SpatialWorld& world);
    std::uintptr_t spatial_side{};
    const ObjectMotion* spatial_owner{};
    const ObjectMotion* spatial_special{};
    void prepare_model();
    // Creation must publish the initial world/collision bounds before any spell reads the box.
    void initialize_model(Match& match);
    const SaxsPose& model_pose();
    Vec3 model_reference_point(unsigned index, bool animation);
    bool write_reference_point(Vec3& point, unsigned index, bool animation); // 0x44e660, world coordinates.
    double pick_model(Vec3 from, Vec3 ray); // SAXSINSTANCE::CheckIntercept mode 2, world Z-up ray.
    const SaxsBounds* prepared_bounds() const {
        return bounds_ ? &*bounds_ : nullptr;
    }
    // SAXSINSTANCE::GetCameraOffset: radius/height at +340/+344, frozen by the first UpdateBBox.
    Vec2 camera_offset() const { return camera_offset_; }
    bool cast_target(const SpellDefinition& definition, const MatchObject* target) const;
    std::uint32_t model_flags{3}; // SAXSINSTANCE +376: 0x1 dynamic bounds, 0x4 frozen skeleton.
    FourCC texture_override; // SAXSINSTANCE::OverrideTextures(-1), restored when Lethargy ends.
    bool grayscale{};
    CreatureStats stats;
    CreatureRegeneration regeneration;
    CreatureSpellBook spells;
    CreatureOrders orders;
    NavigationPath path;             // +2900, shared route nodes and obstruction/elapsed cache.
    MatchObject* approach_blocker{}; // +2928, cleared by negative reference events.
    int approach_result{};           // +2932, previous arrival/blocked result.
    bool follow_settled{};           // Port repair: a follow/guard order has arrived (execute_orders).
    bool issue_order(const CreatureOrders::Input& input, int insertion = 0, bool marker_mode = false);
    bool issue_spell_order(FourCC id, MatchObject* target, std::optional<Vec3> point = {});
    bool find_summon_point(Match& match, Vec3& point) const;   // 0x458ae0.
    bool summon_creature(Match& match, FourCC id, Vec3 point); // 0x481900, soul payment on release.
    void change_souls(int delta);                              // Offline 0x459200.
    void execute_orders(Match& match); // 0x46de80, invoked before regeneration/animation.
    int terrain_visibility(Match& match, MatchObject* target, std::optional<Vec3> point = {}) const;
    int object_visibility(Match& match, Vec3 point, MatchObject* ignore = nullptr) const;
    std::uint32_t attack_capabilities{6}; // +2868, 0x458fd0 derives spell categories from the common book.
    // Side work (vtable +88, 0x495320): the visibility scan 0x46b150 / 0x4724e0, then attack selection
    // 0x493b70, which may queue a temporary melee (6), ability (66) or flight (2) order.
    bool think(Match& match);
    void select_attack(Match& match);
    // The 'peas' behaviour controller (+2936; 0x4545b0 creates it for CRE8 behaviour FSM 'peas', vtable
    // 0x4bfea8). Peasants follow a friendly wizard, flee near shelter or huddle instead of selecting attacks.
    struct PeasantBehaviour {
        int mode{2};              // +8: 0 flee, 1 follow, 2 huddle, 3 keep the given order.
        std::uint32_t leader{};   // +12: the followed wizard (cleared on its removal, 0x454590).
        int evaluate_ticks{};     // +16: 0x4543c0 re-evaluates every 30 ticks.
        int voice_ticks{};        // +20: 0x454060 voice countdown.
        std::uint32_t shelter{};  // +24: nearest building with property 0x8000.
        float shelter_distance{}; // +28: its squared distance.
    };
    std::optional<PeasantBehaviour> peasant;
    void peasant_evaluate(Match& match, int ticks);         // Vtable +8, 0x4543c0, from the run 0x45a620.
    void peasant_think(Match& match);                       // Vtable +4, 0x4540b0, from 0x493b70.
    void peasant_voice(FourCC group, int spread);           // 0x454060.
    bool flee_point(const Match& match, Vec3& point) const; // 0x4951b0.
    // +744 / +748: the view yaw and pitch the follow camera orbits with (0x47fca0); spawn sets the yaw.
    float view_yaw{}, view_pitch{};
    // Modern addition (disabled by default, 'stfe'): A/D strafes laterally instead of turning,
    // while Q/E apply the classic turn axis (-127..127).
    bool strafe_mode{};
    int strafe_turn{};
    float strafe_visual_yaw{}; // Drawing-only body turn toward the strafe direction.
    float danger{};        // +1468: 0x48f080 known enemy strength against own fighters near the creature.
    float courage{1.2f};   // +1472: danger above this makes a capability-1 creature flee.
    int ground_cost{};     // +1476: path cost at its cell.
    float concealment{1};  // +1500: visibility falloff outside the view cone.
    struct OrderAim {
        float angle{}, horizontal_squared{};
        Vec3 delta;
    };
    float face_order_point(Vec3 point, OrderAim* aim = nullptr); // 0x45b3e0 / 0x45b530.
    float face_order_yaw(float yaw);                             // 0x45b460, signed control in -127..127.
    bool probe_movement(Match& match, float yaw, float pitch, float length, float* height = nullptr,
                        MatchObject** blocker = nullptr); // 0x45bd20.
    int avoid_obstacles(Match& match, unsigned flags, std::optional<Vec3> goal, float length,
                        MatchObject** blocker = nullptr);                   // 0x45be60.
    int steer_order(Match& match, Vec3 goal, float radius, int budget = 0); // 0x45b780.
    int approach_order(Match& match, Vec3 goal, float radius, float yaw = 0, float face_yaw = 0,
                       int budget = 0); // 0x45b620.
    struct OrderRequest {
        bool detach_controller{}, schedule{};
        int countdown{};
    };
    std::vector<OrderRequest> order_requests; // 0x46de40: player-controller and side-work hosts pending.
    std::array<CreatureSpellBook, 3> wizard_spells;
    std::int32_t mana{}, maximum_mana{};
    struct BookSlot {
        unsigned book{};
        std::size_t entry{};
    }; // 0 common, 1..3 wizard books.
    struct CastCheck {
        int code{};
        FourCC reason;
    }; // Native +268: 0 available, 1/2 rejected.
    struct Casting {
        std::optional<BookSlot> requested, active; // +2876 / +2880 native fixed-array addresses.
        FourCC definition;
        std::shared_ptr<const SpellData> data;             // Context definition; immutable per cast.
        Vec3 point;                                        // +464 target position.
        int remaining{}, total{}, cost{}, mana_fraction{}; // +2948/+2952/+2956 and +2860.
        std::array<MatchObject*, 3>
            requirements{}; // +2960 enemy altar, +2964 own altar, +2968 nearby own building.
        FourCC error;
        bool accepted{};
    } casting;
    std::optional<BookSlot> find_spell(FourCC id) const;
    std::pair<int, int> cast_cost(const SpellDefinition& definition) const; // Mana and cast ticks.
    CastCheck cast_check(FourCC id) const;
    bool request_cast(FourCC id, MatchObject* target, std::optional<Vec3> point = {}, bool moving = false);
    bool request_ability(FourCC id, MatchObject* target, std::optional<Vec3> point = {});
    bool start_cast();                 // 0x45c360, called by WizardStartSpell.
    static bool cast_supported(const SpellDefinition& definition); // Spell bodies start_cast executes.
    void tick_cast(int ticks);         // 0x455d10.
    // Definition +132 run spell, assigned by 0x458dd0 at creation, outside the ability book/AI.
    std::shared_ptr<const SpellData> initial_run_spell;
    void change_mana_fixed(int delta); // Shared +2860 fraction, also used by generic heat projectiles.
    void abort_cast();
    void release_cast();
    std::pair<std::uint32_t, float> cast_state() const; // Native +280, used by HealRun.
    struct CastVisualRequest {
        FourCC definition;
        std::optional<Vec3> point; // Immediate effects such as teleport preserve their original position.
    }; // Wizard charge effect; host/RNG pending.
    std::vector<CastVisualRequest> cast_visual_requests;
    bool incantation_requested{}; // 0x4823c0; coalesced across simulation ticks before audio service.
    std::uint64_t voice_handle{};  // +2220: current creature voice, including casting phrases.
    // Optional diagnostic override for SIDE +164; bound actors otherwise read their actual side.
    std::optional<int> side_mode;
    std::uint32_t status_flags{}; // Cached definition properties +2872; map/trigger status is common_status (+568).
    int knockdown_cooldown{};     // +2392: prevents repeated knockdown events, not animation duration.
    int knock_down(int mode, float seconds); // 0x4591a0.
    struct BurnRequest {
        Vec3 point;
        float radius{};
        int ticks{};
    };
    std::vector<BurnRequest> burn_requests;
    void burn_near(Vec3 point, float radius, int ticks);
    void burn_bone(unsigned bone, int ticks); // 0x45cb00, nonpositive ticks make the bone permanent.
    bool write_bone_point(Vec3& point, unsigned bone, CrtRandom* random = nullptr, const Vec3* offset = nullptr);
    std::optional<SaxsAttachment> closest_bone(Vec3 point);
    void request_bone_scale(unsigned bone, Vec3 scale); // 0x459380, averaged/eased by 0x4594e0.
    unsigned bone_count() const;
    unsigned bone_parent(unsigned bone) const;
    float animation_multiplier{1};
    ObjectMotion motion;
    std::array<std::int32_t, 3> controls{}; // +1444: yaw, ground forward / flying pitch, flying throttle.
    std::function<std::array<int, 3>()> input_provider; // Native transient +2204 input, consumed once.
    std::optional<std::array<int, 3>> direct_controls;  // External diagnostic input binding, mapped controls.
    void set_direct_controls(std::array<int, 3> value) {
        direct_controls = value;
        controls = value;
    }
    int previous_turn{}; // 0x45a300 saves previous yaw before resetting controls.
    // Bind after activation. Unbound actors can exercise animation without a world.
    const Terrain* terrain{};
    ObjectMotion::Hooks motion_hooks;
    // MODEL +208 local bounds used by the four terrain probes, when the model host supplies them.
    using ModelBounds = Box3;
    std::optional<ModelBounds> model_bounds;
    std::vector<int> death_requests; // Vtable +204 diagnostics; creature removal executes inline.
    std::vector<int> fall_damage;    // Vtable +124 diagnostics; type 0/source 0 hits execute inline.
    struct Damage {
        std::int32_t type{}, amount{};
        std::uint32_t source{}; // Native watcher word; never a truncated host pointer.
    };
    bool damageable() const;
    bool alive() const {
        return fsm_->state() && !(fsm_->state()->flags & 0x44000);
    }
    // 0x43f610: persistent native HealingRun effect, distinct from the pending HealRun cast animation.
    SpellActor* heal(Match& match, int amount, int rate, int limit = 0);
    bool damage(Match& match, Damage value);
    void accumulate_damage(Damage value, int ticks);
    void die(Match& match, int reason); // Vtable +204: disintegration / wizard boundary recovery.
    void make_corpse(Match& match);     // Vtable +248, called by CreatureMakeMeACorpse.
    // Explicit fixture overrides; normal bound actors resolve native match/side/source inputs.
    std::int32_t damage_multiplier{1000}, network_identity{-1};
    std::uint32_t replication_flags{}; // Object +780, health changed is bit 0x20.
    std::uint8_t friendly_damage_scale{}, gore_level{};
    bool force_gibs{};
    std::function<bool(std::uint32_t)> friendly_source;
    std::function<const ObjectMotion*(std::uint32_t)> damage_source;
    std::function<void(Damage&)> attribute_damage; // 0x454a10 / source's slot 1 may rewrite amount.
    std::int32_t accumulated_damage{}, damage_clock{}, soul_count{};
    struct CombatRequest {
        enum class Kind {
            side_event,
            sound,
            blood,
            gibs,
            release_souls,
            death_notification,
            kill_credit
        } kind;
        std::int32_t value{};
        std::uint32_t source{};
        FourCC group{}, sample{};
        float speed{}; // Blood request speed, native 5 for surviving hits / 10 for death.
    };
    std::vector<CombatRequest> combat_requests;
    CreatureActor* ritual_abort_target{}; // Object +68, +476 used by SACDOC_Abort.
    CreatureActor* ritual_abort_owner{};
    bool ritual_abort_owner_enabled{};        // +80.
    float fade{}, fade_target{}, fade_step{}; // +2208..2216, post-update resets target to zero.
    float attack_seconds{};
    MatchObject* melee_target(float length = 1, unsigned mask = 0x15) const; // 0x46a940.
    int melee_hit(MatchObject& target, float seconds); // 0x4596e0; returns the attempted damage.
    std::shared_ptr<const CreatureClip> requested_animation;
    struct Cue {
        std::uint32_t type{};
        std::int32_t value{};
        FourCC sample; // snd! names it; sfx!/foot select from the creature's tables here (0x45c5b0).
    };
    std::vector<Cue> cues; // Animation sound triggers (0x455460), played by the effect audio host.
    struct SpellRequest {
        FourCC spell;
        int mode{};
        std::uint16_t target_flags{};
        std::uint32_t target_reference{};
    };
    std::vector<SpellRequest> spell_requests;
    std::function<int(CreatureActor&, std::string_view, int)> extra_handler;
    // 0x495530 over the definition's SSET list (first set holding the group): a negative index draws
    // from the shared rand(), otherwise index % count. Used by voices (0x45c5d0) and taunts (0x482650).
    FourCC voice_sample(FourCC group, int index = -1) const;

  protected:
    void activated(Match& match) override;
    void deactivated(Match& match) override;
    void update(Match& match, int ticks) override;
    void sync_model(Match& match) override;

  private:
    int execute_spell_order(Match& match, const SpellDefinition& definition, std::optional<Vec3> point,
                            MatchObject* target);
    void advance_animation(int ticks);
    void animation_trigger(const SaxsSequence::Trigger& trigger);
    void notify_event(int id, MatchObject* target);
    void mitigate(Damage& value) const;
    void attribute(Damage& value);
    bool lose_health(int amount);
    void side_event(int code);
    void lethal(Match& match, int amount);
    void tick_damage(Match& match, int ticks);
    void combat_sound(FourCC group);
    void gib(bool suppress_souls);
    void complete_corpse(Match& match); // Base 0x45a720, also used by wizard elimination.
    void freeze_animation();
    void level_changed(int change);
    void grant_level(WizardProgression& state);
    CreatureSpellBook::Entry* cast_entry(BookSlot slot);
    const CreatureSpellBook::Entry* cast_entry(BookSlot slot) const;
    bool cast_target(const SpellDefinition& definition) const;
    void update_pose();
    void advance_bone_scales(int ticks);
    void copy_bone_scales();
    struct BoneScale {
        Vec3 current{1, 1, 1}, target{1, 1, 1};
        unsigned requests{};
        bool active{};
    };
    std::array<BoneScale, SaxsModel::maximum_bones> bone_scales_;
    std::shared_ptr<const CreatureActorResources> resources_;
    std::unique_ptr<CreatureFsmRunner> fsm_;
    CrtRandom* random_{};
    Match* match_{};
    std::uint32_t accumulated_source_{};
    std::shared_ptr<const CreatureClip> clip_;
    float frame_{}, rate_{};
    std::uint64_t pose_revision_{};
    std::shared_ptr<const SaxsAsset> body_;
    std::shared_ptr<const CreatureClip> posed_clip_;
    unsigned posed_frame_{};
    std::optional<SaxsPose> pose_;
    std::optional<SaxsBounds> bounds_;
    Vec2 camera_offset_{2, 1.75f}; // InitModel defaults, before the initial skeleton bounds exist.
    Orientation bounds_orientation_{};
    bool bounds_dirty_{}, pose_changed_{};
    SpatialWorld* spatial_{};
};
} // namespace sacrifice
