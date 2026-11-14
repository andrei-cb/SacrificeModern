#include "sacrifice/creature_orders.hpp"
#include <cmath>

namespace sacrifice {
CreatureOrders::CreatureOrders() : mode(FourCC::from_text("flin")) {
    for (int i = 0; i < static_cast<int>(nodes_.size()); ++i) {
        nodes_[i].previous = i - 1;
        nodes_[i].next = i + 1 < static_cast<int>(nodes_.size()) ? i + 1 : -1;
    }
}
void CreatureOrders::bind(MatchObject* owner, const Terrain* terrain) {
    owner_ = owner;
    terrain_ = terrain;
}
int CreatureOrders::tail() const {
    int at = active_;
    if (at != -1)
        while (nodes_[at].next != -1)
            at = nodes_[at].next;
    return at;
}
void CreatureOrders::move(int index, bool active, int after) {
    auto& node = nodes_.at(index);
    auto& old_head = node.active ? active_ : free_;
    if (node.previous != -1)
        nodes_[node.previous].next = node.next;
    else
        old_head = node.next;
    if (node.next != -1)
        nodes_[node.next].previous = node.previous;
    auto& head = active ? active_ : free_;
    node.active = active;
    node.previous = after;
    node.next = after == -1 ? head : nodes_[after].next;
    if (node.next != -1)
        nodes_[node.next].previous = index;
    if (after == -1)
        head = index;
    else
        nodes_[after].next = index;
}
int CreatureOrders::prepend() {
    const int index = free_ != -1 ? free_ : tail();
    if (index == -1)
        throw Error("Native order pool has no records");
    move(index, true);
    return index;
}
void CreatureOrders::fill(int index, const Input& input, unsigned flags) {
    auto& value = nodes_[index].value;
    value.flags = flags;
    value.kind = input.kind;
    value.target = input.target;
    value.formation = input.formation.has_value();
    if (input.formation)
        formation_ = *input.formation;
    if (input.point)
        value.point = *input.point;
    else if (input.target) {
        if (!input.target->source_motion())
            throw Error("Order target has no motion host");
        value.point = input.target->source_motion()->position;
    } else
        value.point = {};
    if (input.yaw)
        value.yaw = *input.yaw;
    else {
        if (!owner_ || !owner_->source_motion())
            throw Error("Order facing requires a bound motion owner");
        const auto position = owner_->source_motion()->position;
        const float dx = value.point.x - position.x, dy = value.point.y - position.y;
        const double length = std::sqrt(static_cast<double>(dx) * dx + static_cast<double>(dy) * dy);
        value.yaw = length == 0 ? 0 : static_cast<float>(std::atan2(-static_cast<double>(dx), dy));
    }
}
void CreatureOrders::notify(bool schedule) {
    if (owner_ && changed)
        changed(schedule);
}
bool CreatureOrders::issue(const Input& input, int insertion, int base_attack, bool marker_mode) {
    if (input.kind >= 64)
        throw Error("This native order can only be issued internally");
    if ((input.kind == 6 || input.kind == 7) && !base_attack)
        return false;
    if (insertion < 0 || insertion > 2)
        throw Error("Invalid native order insertion mode");
    if (marker_mode) {
        if (input.kind != cinematic_marker)
            through_marker(false);
        insertion = 2;
    }
    if (insertion && (active_ == -1 || ((nodes_[active_].value.flags & 0x30) && nodes_[active_].next == -1)))
        insertion = 0;
    int index;
    if (insertion == 1) {
        index = tail();
        if (free_ != -1) {
            const int available = free_;
            move(available, true, index);
            index = available;
        } // A full append overwrites the tail without removing any other record.
    } else {
        if (!insertion)
            clear();
        index = prepend();
    }
    fill(index, input, 0);
    if (insertion != 1)
        notify(true);
    return true;
}
void CreatureOrders::internal(const Input& input, bool temporary) {
    int index = active_;
    if (index == -1 || !(nodes_[index].value.flags & 0x30))
        index = prepend();
    fill(index, input, 0x10);
    notify(false);
    // 0x46da50 sets the temporary bit after 0x46dc00's notification.
    if (temporary)
        nodes_[active_].value.flags |= 0x20;
}
bool CreatureOrders::pop(bool force) {
    if (active_ != -1 && (nodes_[active_].next != -1 || force)) {
        move(active_, false);
        notify(true);
    }
    return active_ == -1;
}
void CreatureOrders::clear() {
    while (active_ != -1)
        move(active_, false);
}
void CreatureOrders::discard_temporary() {
    if (active_ != -1 && (nodes_[active_].value.flags & 0x20))
        move(active_, false);
}
void CreatureOrders::through_marker(bool include_marker) {
    while (active_ != -1 && nodes_[active_].value.kind != cinematic_marker)
        move(active_, false);
    if (active_ != -1 && include_marker)
        move(active_, false);
    notify(true);
}
void CreatureOrders::rewrite(int expected, int replacement) {
    if (active_ != -1 && nodes_[active_].value.kind == expected) {
        nodes_[active_].value.formation = false;
        nodes_[active_].value.kind = replacement;
    }
}
void CreatureOrders::clear_target(const MatchObject& object) {
    for (int at = active_; at != -1; at = nodes_[at].next)
        if (nodes_[at].value.target == &object)
            nodes_[at].value.target = nullptr;
}
CreatureOrders::Value CreatureOrders::current(bool skip_temporary) {
    int at = active_;
    if (skip_temporary)
        while (at != -1 && (nodes_[at].value.flags & 0x20))
            at = nodes_[at].next;
    if (at == -1)
        return {};
    auto& value = nodes_[at].value;
    if (value.formation && value.target) {
        const auto* motion = value.target->source_motion();
        if (!motion || !terrain_)
            throw Error("Formation orders require motion and terrain services");
        const auto& o = motion->orientation;
        const auto& p = formation_.offset;
        value.point = {static_cast<float>(static_cast<double>(p.x) * o[0] + static_cast<double>(p.y) * o[3] +
                                          static_cast<double>(p.z) * o[6] + motion->position.x),
                       static_cast<float>(static_cast<double>(p.x) * o[1] + static_cast<double>(p.y) * o[4] +
                                          static_cast<double>(p.z) * o[7] + motion->position.y),
                       0};
        value.point.z = static_cast<float>(terrain_true_height(*terrain_, value.point.x, value.point.y));
        value.yaw = motion_wrap(static_cast<double>(motion->yaw) + formation_.yaw);
    }
    return value;
}
MatchObject* CreatureOrders::target() const {
    return active_ == -1 ? nullptr : nodes_[active_].value.target;
}
CreatureOrders::Value CreatureOrders::suspended_cinematic_order() const {
    int at = active_;
    while (at != -1 && nodes_[at].value.kind != cinematic_marker)
        at = nodes_[at].next;
    if (at != -1)
        at = nodes_[at].next;
    return at == -1 ? Value{} : nodes_[at].value;
}
int CreatureOrders::kind() const {
    return active_ == -1 ? 0 : nodes_[active_].value.kind;
}
bool CreatureOrders::has_formation() const {
    return active_ != -1 && nodes_[active_].value.formation;
}
bool CreatureOrders::transient() const {
    return active_ != -1 && (nodes_[active_].value.flags & 0x30);
}
std::size_t CreatureOrders::size() const {
    std::size_t count = 0;
    for (int at = active_; at != -1; at = nodes_[at].next)
        ++count;
    return count;
}
} // namespace sacrifice
