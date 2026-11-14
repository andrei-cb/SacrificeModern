#pragma once
#include "match.hpp"
#include "motion.hpp"

namespace sacrifice {
// Native ORDERSYSTEM, actor +1092 (0x46d7f0..0x46de40). Eight reusable 40-byte records.
// Record/formation pointers become stable host references, never truncated x64 addresses.
class CreatureOrders {
  public:
    static constexpr int cinematic_marker = 33;
    struct Formation {
        Vec3 offset;
        float yaw{};
    };
    struct Input {
        int kind{};
        MatchObject* target{};
        std::optional<Vec3> point;
        std::optional<float> yaw;
        std::optional<Formation> formation;
    };
    struct Value {
        int kind{};
        MatchObject* target{};
        Vec3 point;
        float yaw{};
        bool formation{};
        unsigned flags{};
    };
    CreatureOrders();
    CreatureOrders(const CreatureOrders&) = delete;
    CreatureOrders& operator=(const CreatureOrders&) = delete;
    // Native pooling preserves queued records and mode; activation rebinds only the owner.
    void bind(MatchObject* owner, const Terrain* terrain = nullptr);
    FourCC mode; // +1096 formation id set by 'form' (0x46d700); constructor default 'flin'.
    // 0x46de40: false cancels a transient controller when the new kind is nonzero; true also
    // reschedules side work with -15. This hook receives the original call even for an empty queue.
    std::function<void(bool)> changed;
    // 0x46dd00: replace (0), append (1), prepend (2). Melee 6/7 requires nonzero base attack.
    // During cinematics, prepend script orders before the marker while preserving the suspended queue.
    bool issue(const Input& input, int insertion = 0, int base_attack = 1, bool marker_mode = false);
    void internal(const Input& input, bool temporary = false); // 0x46dc00 / 0x46da50, flags 0x10/0x30.
    bool pop(bool force);     // 0x46d8b0: preserve a sole record unless forced; returns whether empty.
    void clear();             // 0x46d8f0: no changed notification.
    void discard_temporary(); // 0x46da80: discard only a leading record with bit 0x20, no notification.
    void through_marker(bool include_marker);     // 0x46dcb0: discard through kind 33, optionally itself.
    void rewrite(int expected, int replacement);  // 0x46db80: also clears formation, retains other fields.
    void clear_target(const MatchObject& target); // 0x46dbe0; host sends reference events with mask 0xA.
    Value current(bool skip_temporary = false);   // 0x46daa0 / 0x46dad0 / 0x46db10; formation refreshes.
    Value suspended_cinematic_order() const;
    int kind() const;                             // 0x46dbb0: no formation refresh.
    MatchObject* target() const;                  // 0x46dbc0: no formation refresh.
    bool has_formation() const;                   // 0x46d870.
    bool transient() const;                       // 0x46d850: flags 0x30.
    std::size_t size() const;                     // 0x46d890.

  private:
    friend class WorldJournal;
    struct Node {
        Value value;
        int previous{-1}, next{-1};
        bool active{};
    };
    std::array<Node, 8> nodes_;
    int active_{-1}, free_{0};
    MatchObject* owner_{};
    const Terrain* terrain_{};
    Formation formation_; // One shared offset: all formation records point at this storage.
    int tail() const;
    void move(int node, bool active, int after = -1);
    int prepend(); // Free head, or steal the last active record when all eight are used.
    void fill(int node, const Input& input, unsigned flags);
    void notify(bool schedule);
};
} // namespace sacrifice
