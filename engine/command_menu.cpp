#include "sacrifice/command_menu.hpp"
#include <cmath>
#include <set>

namespace sacrifice {
int CommandMenu::index(FourCC id) const {
    const auto it = std::ranges::find(entries_, id, &Entry::id);
    return it == entries_.end() ? -1 : static_cast<int>(it - entries_.begin());
}
const CommandMenu::Entry* CommandMenu::find(FourCC id) const {
    const auto i = index(id);
    return i < 0 ? nullptr : &entries_[i];
}
void CommandMenu::load(const ResourceCatalog& catalog) {
    close();
    entries_.clear();
    std::set<FourCC> seen;
    for (std::size_t archive = 0; archive < catalog.archives().size(); ++archive) {
        const auto& records = catalog.archives()[archive].records();
        for (std::size_t record = 0; record < records.size(); ++record) {
            const auto& r = records[record];
            if (r.tag != FourCC::from_text("XMNU") || r.folder() || !seen.insert(r.name).second)
                continue;
            const auto bytes = catalog.read({archive, record});
            Reader reader(bytes, "XMNU");
            Entry entry;
            entry.id = FourCC{reader.u32()};
            entry.text = FourCC{reader.u32()};
            entry.icon = FourCC{reader.u32()};
            entry.command = FourCC{reader.u32()};
            entry.argument = FourCC{reader.u32()};
            reader.u32(); // Runtime icon cache, zero on disk.
            if (entries_.size() == 128)
                throw Error("Too many xmenu nodes");
            entries_.push_back(entry);
        }
    }
    seen.clear();
    for (std::size_t archive = 0; archive < catalog.archives().size(); ++archive) {
        const auto& records = catalog.archives()[archive].records();
        for (std::size_t record = 0; record < records.size(); ++record) {
            const auto& r = records[record];
            if (r.tag != FourCC::from_text("XMNL") || r.folder() || !seen.insert(r.name).second)
                continue;
            const auto bytes = catalog.read({archive, record});
            Reader reader(bytes, "XMNL");
            const auto count = reader.u32();
            if (count > (bytes.size() - 4) / 12)
                throw Error("Truncated XMNL links");
            for (unsigned i = 0; i < count; ++i) {
                const auto from = index(FourCC{reader.u32()}), to = index(FourCC{reader.u32()});
                const auto direction = reader.u32();
                if (direction > 3)
                    throw Error("Invalid XMNL direction");
                if (from < 0)
                    continue;
                // 0x482940 disconnects both replaced reciprocal edges.
                if (const auto old = entries_[from].links[direction]; old >= 0)
                    entries_[old].links[direction ^ 1] = -1;
                entries_[from].links[direction] = to;
                if (to >= 0) {
                    if (const auto old = entries_[to].links[direction ^ 1]; old >= 0)
                        entries_[old].links[direction] = -1;
                    entries_[to].links[direction ^ 1] = from;
                }
            }
        }
    }
}
bool CommandMenu::open(FourCC root, Vec2 point, int delay) {
    const auto source = index(root), centre = index(FourCC::from_text("1cen"));
    if (source < 0 || centre < 0)
        return false;
    auto& c = entries_[centre];
    const auto& r = entries_[source];
    c.text = r.text;
    c.icon = r.icon;
    c.command = r.command;
    c.argument = r.argument;
    current_ = centre;
    x_ = static_cast<int>(point.x);
    y_ = static_cast<int>(point.y);
    delay_ = remaining_ = delay;
    sound_clock_ = 0;
    return true;
}
void CommandMenu::close() { current_ = -1; }
void CommandMenu::tick(int ticks) {
    if (active()) {
        remaining_ = std::max(remaining_ - ticks, 0);
        sound_clock_ -= ticks;
    }
}
int CommandMenu::hit(Vec2 point, int& column, int& row) const {
    // Native integer division truncates towards zero, including the negative outer edge.
    column = (static_cast<int>(point.x) - x_ + 60) / 40;
    row = (static_cast<int>(point.y) - y_ + 60) / 40;
    if (!active())
        return -1;
    if (column == 1)
        return row == 0 ? entries_[current_].links[0] : row == 1 ? current_
                                                     : row == 2 ? entries_[current_].links[1] : -1;
    if (row == 1)
        return column == 0 ? entries_[current_].links[2]
                           : column == 2 ? entries_[current_].links[3] : -1;
    return -1;
}
FourCC CommandMenu::move(Vec2 point) {
    if (!active())
        return {};
    int next = -1;
    const bool hidden = remaining_ != 0;
    if (hidden) {
        const auto dx = static_cast<int>(point.x) - x_, dy = static_cast<int>(point.y) - y_;
        if (std::max(std::abs(dx), std::abs(dy)) <= 4)
            return {};
        const unsigned direction = std::abs(dx) > std::abs(dy) ? (dx > 0 ? 3 : 2) : (dy > 0 ? 1 : 0);
        next = entries_[current_].links[direction];
        x_ = static_cast<int>(point.x);
        y_ = static_cast<int>(point.y);
        remaining_ = delay_;
    } else {
        int column{}, row{};
        next = hit(point, column, row);
        if (next >= 0) {
            x_ += 40 * column - 40;
            y_ += 40 * row - 40;
        }
    }
    if (next < 0 || next == current_)
        return {};
    current_ = next;
    if (sound_clock_ > 0)
        return {};
    sound_clock_ = 30;
    return FourCC::from_text(hidden ? "invu" : "visu");
}
std::optional<CommandMenu::Entry> CommandMenu::select(Vec2 point) {
    int column{}, row{};
    const auto selected = hit(point, column, row);
    close();
    return selected < 0 ? std::nullopt : std::optional<Entry>{entries_[selected]};
}
std::array<const CommandMenu::Entry*, 5> CommandMenu::displayed() const {
    std::array<const Entry*, 5> result{};
    if (visible()) {
        result[0] = &entries_[current_];
        for (unsigned i = 0; i < 4; ++i)
            if (const auto next = entries_[current_].links[i]; next >= 0)
                result[i + 1] = &entries_[next];
    }
    return result;
}
} // namespace sacrifice
