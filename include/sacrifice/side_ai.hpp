#pragma once
#include "creature_orders.hpp"
#include "match.hpp"
#include "motion.hpp"

namespace sacrifice {
struct MatchSide;
class CreatureActor;
class BuildingActor;
struct SpellDefinition;
struct CreatureDefinition;
class SpellRegistry;

// Computer side controller 0x483780 (vtable 0x4c0a4c), created by 0x471f60 for sides with flag 0x40
// and run by the side pass 0x471fb0 after the triggers. The schedule, agents, squads and soul clusters,
// strategy statistics, the three planners and their assignment, the order filter, wizard decisions and
// spell scorers follow the copied build 0678 executable; remaining gaps are listed in docs/side-ai.md.
class SideController {
  public:
    // 0x48b400 record +8..+24 (allocator 0x48b250) and the 0x48bf90 sum.
    struct Strength {
        std::array<float, 5> v{};
        float total() const;                                   // 0x48bf90: (v2 + v3) / 2 + 2 (v0 + v1).
        void add(float self, const Strength& other, float scale); // 0x48bee0.
        void divide(float value);                              // 0x48bf50: the first four terms.
    };
    static Strength definition_strength(const CreatureDefinition& definition,
                                        const SpellRegistry* registry = nullptr); // 0x48bad0 / 0x48bd80.
    static Strength creature_strength(const CreatureActor& actor,
                                      const SpellRegistry* registry = nullptr); // 0x48b7e0.
    static float compare(const Strength& a, const Strength& b);                 // 0x48bfb0.
    enum class Kind { unit, building, soul };
    struct Task;
    struct Squad;
    struct Agent {
        Kind kind{};
        MatchObject* object{};
        std::uint32_t reference{};
        std::uint32_t flags{};  // +60: 1 own, 2 allied, 4 enemy, 8 soul, 0x10 unit, 0x20 building, 0x40 mobile,
                                // 0x80 seen, 0x100 site or wizard, 0x200 altar.
        std::uint32_t status{}; // +64.
        unsigned clock{}, seen{}; // +68 / +72.
        Vec3 position, predicted, previous, velocity; // +4 / +16 / +28 / +40.
        Strength strength;              // Record +100 (units).
        float power{};                  // +124 units, +108 buildings: strength total.
        float threshold{};              // Unit +96: mana kept back (vtable +88).
        unsigned strength_clock{}, status_clock{}; // +116 units / +100 buildings, +104 buildings.
        Squad* squad{};                 // +104 units, cluster +96 souls.
        Task* task{};                   // +112.
        float danger{};                 // Wizard +108.
        unsigned weigh_clock{}, plan_clock{}, react_clock{}, soul_clock{}; // +172 / +176 / +180 / +184.
        int souls{};                    // +192.
        bool rebuild{true};             // +196 bit 0, set by side event 22.
        FourCC mano, manalith, convert, desecrate; // +200 / +204 / +208 / +212.
        int traced_mode{-1};          // --trace-ai change detection (not native).
        const void* traced_task{}; // The task target agent: task records are rebuilt each plan.
        FourCC traced_manalith;
        struct Summon {
            FourCC id;
            Strength strength;
            const SpellDefinition* definition{};
        };
        std::vector<Summon> summons; // +220.
        // 0x48c1d0 scorers at +136 (0x48c240 selects the function).
        struct Scorer {
            FourCC id;
            int book{}, entry{};
            const SpellDefinition* definition{};
            int function{};
        };
        std::vector<Scorer> scorers;
        struct Plan {
            bool immediate{}; // Record +0 bit 1: sorts first and does not move into range.
            float score{};
            Agent* target{};
            Agent* point{};
            float range{};
            const SpellDefinition* definition{};
        };
        std::vector<Plan> plans; // +244.
        std::vector<Plan> issued_plans; // +228: issued/retired records, reused from the tail by 0x48fa60.
        std::optional<CreatureOrders::Value> cast_approach;
        int category() const;    // 0x489a90: 0 own, 1 allied, 2 neutral, 3 enemy.
        CreatureActor* actor() const;
        BuildingActor* building() const;
        std::pair<int, int> mana() const;  // Vtable +20.
        std::pair<int, int> souls_pair() const; // Vtable +84.
    };
    // 136-byte squad (vtable 0x4c0e6c) and 100-byte soul cluster (0x4c0eac); mode +16 is 1 own, 2 allied,
    // 0 neutral and 4 enemy.
    struct Squad {
        int mode{}, category{}, index{};
        std::vector<Agent*> members;
        int count{};
        std::uint32_t status{};
        Vec3 mean, center, previous, velocity, deviation; // +28 / +40 / +52 / +64 / +80.
        Strength strength;                                // Record +92.
        int mana{}, maximum{}, health{}, health_maximum{}, sources{}, hoars{}, souls{}; // +96..+120.
        bool accepts(const Agent& agent) const; // 0x48e9b0.
    };
    // 184-byte planner task (0x48cd60): kind +20 (1 building, 2 site, 3 unit, 9 altar) is also the
    // required union of member status bits during 0x48e140.
    struct Task {
        float score{};
        Agent* target{};
        Agent* leader{};
        int mode{}, kind{};
        Vec3 point, mean;
        Strength need, assigned; // +76 / +172.
        std::vector<Agent*> members;
    };
    // 0x486870 per-category block at +444 + 44 c.
    struct Statistics {
        int mana{}, maximum{};
        std::array<int, 3> souls{1, 1, 1};
        int manaliths{}, units{};
        std::array<float, 3> ratio{};
        Strength strength;
    };
    SideController(Match& match, MatchSide& side, std::uint32_t flags = 16, float scale = 1);
    ~SideController();
    SideController(const SideController&) = delete;
    SideController& operator=(const SideController&) = delete;
    void tick(int ticks);                                         // 0x485b30.
    void event(MatchObject& target, MatchObject* source, int code); // 0x4859c0.
    Agent* add(MatchObject& object, int mode = 0);                // 0x4847b0.
    void drop(MatchObject& object);                               // 0x4852d0.
    Agent* find(const MatchObject& object) const;                 // 0x484080.
    bool accepts(const MatchObject& object) const;                // 0x48aac0.
    const std::list<std::unique_ptr<Agent>>& agents() const {
        return agents_;
    }
    std::vector<const Task*> tasks() const;
    const std::array<std::vector<std::unique_ptr<Squad>>, 4>& squads() const {
        return squads_;
    }
    const std::array<Statistics, 4>& statistics() const {
        return statistics_;
    }
    std::uint32_t flags, dirty{15};
    float scale;
    unsigned clock() const {
        return clock_;
    }
    int units_maximum{}, fountains{}, enemy_wizards{}; // +640 / +644 / +628.
    float knowledge{};                                  // +620: seen share of enemy altars and free sites.
    Strength friendly_average, enemy_average;           // +632 / +636.

  private:
    struct Schedule {
        unsigned period, mask;
        void (SideController::*run)(int);
        unsigned next{};
    };
    Match& match_;
    MatchSide& side_;
    unsigned clock_{}, handled_{};
    std::array<Schedule, 6> schedule_; // 0x483590.
    std::list<std::unique_ptr<Agent>> agents_;
    // Category lists: +12 all (link +80), +44 units (+160), +76 buildings (+112), +108 souls.
    std::array<std::vector<Agent*>, 4> all_, units_, buildings_, souls_;
    std::array<std::vector<std::unique_ptr<Squad>>, 4> squads_, clusters_; // +140 / +172.
    std::array<std::vector<std::unique_ptr<Task>>, 3> planners_;          // Types 2, 1, 0 (list +236).
    std::array<Statistics, 4> statistics_;
    void regroup(int ticks);  // 0x485e40.
    void strategy(int ticks); // 0x486870.
    void run_agents(int ticks); // 0x485c90.
    void run_squads(int ticks); // 0x485d10.
    void replan(int ticks);   // 0x486060.
    void run_tasks(int ticks); // 0x485dc0 / 0x48e370.
    void count_units();       // 0x486400.
    void remove(Agent& agent); // 0x485000.
    bool valid(const Agent& agent) const;
    bool alive(const Agent& agent) const; // Unit vtable +32 (0x48a120).
    void run(Agent& agent, int ticks);
    void update_strength(Agent& agent);
    void update(Squad& squad) const;  // 0x48acb0.
    void update_cluster(Squad& squad) const; // 0x4894d0.
    void sort_units(std::vector<Agent*>& list) const; // 0x486060 reordering by vtable +28.
    void build_squads(int category);   // 0x487450.
    void build_clusters(int category); // 0x487ab0.
    void plan_sites();   // 0x48dbf0.
    void plan_guards();  // 0x48d560.
    void plan_idle();    // 0x48e3e0.
    float site_score(float base, const Agent& agent) const; // 0x48db30.
    void finish(std::vector<std::unique_ptr<Task>>& tasks, float total) const; // 0x48d440.
    std::unique_ptr<Task> make_task(Agent* target, int mode) const;          // 0x48cd60.
    void insert(std::vector<std::unique_ptr<Task>>& tasks, std::unique_ptr<Task> task) const; // 0x48d3b0.
    int join(Task& task, std::vector<Agent*>& available, Agent& agent);      // 0x48d490.
    void assign_sites(std::vector<Agent*>& available);  // 0x48e140.
    void assign_guards(std::vector<Agent*>& available); // 0x48d870.
    void assign_idle(std::vector<Agent*>& available);   // 0x48e560.
    float presence(int category, Vec3 point, float inner, float outer, Strength* record) const; // 0x488880.
    float soul_presence(Vec3 point, float inner, float outer) const;                         // 0x488b30.
    float buildings_near(Vec3 point, int category, std::uint32_t mask, float inner, float outer,
                         const Agent* skip) const; // 0x488a20.
    float friendly_buildings(const Agent& agent) const; // 0x488d70.
    std::pair<std::uint32_t, std::uint32_t> abilities(const CreatureActor& unit) const; // 0x48a0b0.
    void decide(Agent& agent, int mode, Task* task);         // Agent vtable +12.
    void execute(Task& task);                                // 0x48ce30.
    int decide_creature(Agent& agent, int mode, Task* task); // 0x48a530.
    int decide_wizard(Agent& agent, int mode, Task* task);   // 0x490b20.
    Agent* choose_victim(const Agent& altar);                // 0x488400.
    int follow(Agent& agent, int mode, Task* task);          // 0x48a8f0.
    bool visit_mana(Agent& agent);                           // 0x48a460.
    void prepare_wizard(Agent& agent);                       // 0x48f460 / 0x48f710 / 0x48f5e0.
    void weigh(Agent& agent);                                // 0x4908e0 danger.
    std::array<float, 3> plan_weights(int mode) const;       // 0x48f800.
    void plan_attack(Agent& agent, const std::array<float, 3>& weights);  // 0x4901a0.
    void plan_support(Agent& agent, const std::array<float, 3>& weights); // 0x490320.
    void plan_convert(Agent& agent, const std::array<float, 3>& weights); // 0x4906d0.
    void plan_summons(Agent& agent, Task* task);             // 0x490410.
    bool plan_manalith(Agent& agent, Agent& site);           // 0x490850.
    float best_spell(Agent& agent, Agent& target, std::uint32_t mask, const SpellDefinition** definition) const; // 0x490020.
    float score(const Agent& agent, const Agent::Scorer& scorer, const Agent& target) const; // 0x48c330.
    void add_plan(Agent& agent, Agent::Plan plan);           // 0x48fa60 / 0x48f9d0.
    bool execute_plans(Agent& agent, bool probe);            // 0x48fc40.
    void stop_cast_approach(Agent& agent);
    Agent* best_soul(const Agent& agent, float range, bool own) const;       // 0x488190.
    Agent* best_mana(Vec3 point, float range) const;                         // 0x4885e0.
    int mana_reserve(const Agent& agent) const;                              // Vtable +88: 0x490ac0 / 0x48a040.
    void order(Agent& agent, int kind, MatchObject* target, std::optional<Vec3> point); // 0x48a6c0.
    bool order_target(Agent& agent, int kind, Agent& target, float range);   // 0x48a2b0.
    bool order_point(Agent& agent, int kind, Vec3 point, float range);       // 0x48a340.
    void order_capture(Agent& agent, Vec3 point, Agent* target, float range); // 0x48a3b0.
};
} // namespace sacrifice
