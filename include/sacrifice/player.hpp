#pragma once
#include "creature_orders.hpp"
#include "match.hpp"

namespace sacrifice {
struct MatchSide;
struct SpellDefinition;
class CreatureActor;

// Local player interface state (0x4da920) and its command handler 0x461ed0: the selection NTTLIST
// (+296), ten groups (+328 + 16 g), group orders through 0x46d370 with the formation table 0x4cfe98
// and the order table 0x4cff08, and the acknowledgement slots of 0x4610b0. Mouse and widget routing
// ('clik', 'drag' and the GUI handlers) belong to the front end; the host calls these commands.
class PlayerControl {
  public:
    // NTTLIST (0x493690..0x493850): nodes insert at the head; a full list evicts its lowest value.
    class List {
      public:
        struct Entry {
            MatchObject* object{};
            int value{};
        };
        int capacity{}; // +4: 0 is unbounded.
        const std::vector<Entry>& entries() const {
            return entries_;
        }
        std::size_t size() const {
            return entries_.size();
        }
        bool empty() const {
            return entries_.empty();
        }
        bool contains(const MatchObject* object) const;     // 0x493690.
        void add(MatchObject* object, int value);           // 0x4936e0: keeps the larger value.
        void remove(const MatchObject* object);             // 0x493740.
        void clear();                                       // 0x493630.
        void copy(const List& source);                      // 0x493650: head insertion reverses order.
        std::vector<MatchObject*> objects() const;

      private:
        friend class PlayerControl;
        std::vector<Entry> entries_;
        bool insert(MatchObject* object, int value); // 0x493770 / 0x493790.
    };
    // Acknowledgement slot (0x4610b0): 0 interface, 1 the wizard's voice, 2 the creature's reply.
    struct Feedback {
        MatchObject* speaker{};
        FourCC id;
    };
    // 0x4606a0 order marker (object type 35, colour 0x00RRGGBB, 30 ticks in 0x4605c0).
    struct Marker {
        Vec3 point;
        std::uint32_t colour{};
        float age{};
    };

    PlayerControl(Match& match, MatchSide& side);
    MatchSide& side() const {
        return side_;
    }
    CreatureActor* wizard() const; // +164, the side's primary wizard (0x457c30).
    List selection;
    std::array<List, 10> groups;
    int stamp{};          // +636: the value given to new list entries.
    bool append{};        // dword_4dcf88: shift queues orders and toggles selection.
    Vec3 camera_right{1, 0, 0}, camera_forward{0, 1, 0}; // dword_4dafd9 +56 / +68 for local formations.
    std::array<Feedback, 3> feedback;
    // IFAC:snds (loaded by 0x409e20 into the 0x4dcf90 directory): command, result (-1 matches any) and
    // the interface sound group at +8, played from SSET:ifac by 0x44de40; '----' is silent.
    struct SoundRule {
        FourCC command, result, sound;
    };
    static std::vector<SoundRule> decode_sound_rules(ByteView data);
    std::vector<SoundRule> sound_rules;
    std::vector<FourCC> interface_sounds; // Groups requested by 0x461bf0 for the host to play.
    void click(FourCC command, FourCC result = FourCC::from_text("dflt")); // 0x461bf0.
    // 0x460dc0: advisor slot 0 ('invl', 'orng' from SSET:advi); later slots are dropped (0x4610b0).
    void advise(FourCC id);
    std::vector<Marker> markers;
    // 'tgot', 'tatt', 'tgua' and 'prot' without a target wait for a target click ('targ' mode). A
    // targeted spell waits as 'cast' with its id (+44), target mask (+24) and range (+32) (0x461a50).
    FourCC pending;
    FourCC pending_spell;
    std::uint32_t pending_mask{};
    float pending_range{};

    bool selectable(const MatchObject& object) const; // Object vtable +72 for the local side.
    // Commands of 0x461ed0.
    bool select(MatchObject* object);            // 'ssel': replace, or toggle while appending.
    bool select_near(Vec3 point);                // 'ssel' 'near': the closest creature within 10000.
    bool select_type(MatchObject& object);       // 'selt': same type within 50 (0x461d40).
    // 0x453120: save the optional Shift selection once, then rebuild the live selection from that
    // baseline and the view's visible candidates on every drag update. Enclosed members get value 1.
    void begin_box();
    void update_box(std::span<MatchObject* const> visible);
    void end_box();
    void cancel_box();
    bool selecting_box() const { return selecting_box_; }
    void clear();                                // 'selz'.
    bool select_group(int group);                // 'selg'.
    bool assign_group(int group);                // 'assg'.
    bool add_group(int group);                   // 'addg'.
    void formation(FourCC id);                   // 'form' (0x46d700): creature +1096.
    bool move(MatchObject* target, Vec3 point);  // 'goto': kind 2.
    bool guard(MatchObject* target, Vec3 point); // 'gurd': 5 on a target, otherwise hold 4.
    bool attack(MatchObject* target, Vec3 point); // 'atck': 6 on a target, otherwise 7.
    bool collect(Vec3 point);                    // 'clct': kind 34.
    bool protect(MatchObject& target);           // 'prot': guard a creature.
    void protect_wizard();                       // 'pwiz': every side creature guards the wizard.
    bool cast(FourCC spell, MatchObject* target, std::optional<Vec3> point); // 'cast' (0x454790).
    // 'Kspl' / 'Xspl' (spell bar click) in 'norm' mode (0x461ed0): the book lookup (+260) opens the
    // spell's book as the bar tab (+272), then 0x454700: a spell or structure with target bits enters
    // target mode; anything else (summons, self spells) is cast at once by the wizard.
    bool spell_key(FourCC spell);
    bool ability_key(); // 'Kabi', 0x47eb20 / 0x47e630: highest-rank selected creature's first ability.
    // 0x461950: a spell or ability icon chosen while a target is pending leaves target mode ('ctrg') and then
    // selects the icon ('seli'), so another spell replaces the pending one. Choosing the pending spell again
    // only leaves target mode (a port convenience: the native re-entered the same mode, which reads as an
    // ignored click). Spell keys take the same path rather than being ignored outside 'norm'.
    bool choose_spell(FourCC spell);
    bool choose_ability();
    // 0x461e70 from the match's reference event (bit 4: death, side change or removal): drop the object
    // from the selection, the box baseline and all ten groups, so pooled objects never come back selected.
    void forget(const MatchObject& object);
    // 0x45d300: add a newly summoned, converted or revived creature controlled by the local wizard
    // to group 0 and the live selection.
    void select_spawned(CreatureActor& actor);
    // 'Kcr?' / 'Ksp?' / 'Ksh?' n (0x4636b0): open book 0/1/2 and cast its nth entry (1-based).
    bool cast_slot(int book, int number);
    // 'Kopn' (0x461ed0): open bar tab 0..2, any other value cycles to the next one.
    void open_tab(unsigned tab);
    int spell_tab{}; // +272: the open spell bar tab, one of the wizard's three books.
    // 'tsel' while a target command is pending; returns false when nothing was waiting. A rejected spell
    // target (0x463890 / 0x463740: wrong kind, or out of range) keeps the target mode.
    bool resolve_target(MatchObject* target, Vec3 point);
    // 0x463740: 0 accepted, -1 illegal kind/rule, -2 beyond the explicit caster's range.
    int target_check(const MatchObject* target, Vec3 point, bool ground = true) const;
    std::uint32_t target_mask() const;
    // 'ctrg' (0x461910): leave target mode for 'norm'; false when no target was pending.
    bool cancel_target();
    // 0x46d370 with the interface flags (1 acknowledge and mark, 2 formations).
    void order(int kind, MatchObject* target, Vec3 point, unsigned mode = 3);
    void tick(int ticks); // Marker lifetime.
    // 0x46d740: the most common formation id among the selection.
    FourCC common_formation() const;
    struct Slot {
        Vec3 offset;
        float yaw{};
    };
    // Formation functions (0x46c390..0x46cb70) for `count` members with radius `radius`.
    static std::vector<Slot> layout(FourCC formation, int count, float radius, const MatchObject* target,
                                    unsigned flags);

  private:
    friend class WorldJournal;
    Match& match_;
    MatchSide& side_;
    MatchObject* last_leader_{};
    int repeats_{};
    List box_baseline_;
    bool selecting_box_{};
    bool selection_spell_{}; // A Kabi cast has no explicit wizard caster (0x454700 / 0x454790).
    MatchObject* toggle(MatchObject* object, int mode, MatchObject* leader); // 0x461c40.
    void acknowledge(MatchObject* leader, FourCC id, bool repeat);            // 0x460e10.
    void voice(FourCC prefix, MatchObject* target, unsigned mask);            // 0x460cd0.
    void mark(Vec3 point, std::uint32_t colour);
    bool request_spell(CreatureActor& own, const SpellDefinition& definition); // 0x454700.
};
} // namespace sacrifice
