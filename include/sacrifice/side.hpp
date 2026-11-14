#pragma once
#include "core.hpp"
#include <functional>
#include <list>
#include <memory>

namespace sacrifice {
class MatchObject;
class Match;
class MatchSides;
class SideController;
class FogOfWar;
// 0x456330 / 0x471bf0: signed SIDS count, then 20-byte records; nonpositive counts are empty.
struct SideDefinition {
    std::uint32_t index{}, allies{}, enemies{}, flags{}, color{};
    static std::vector<SideDefinition> decode(ByteView bytes);
};
struct MatchSide {
    explicit MatchSide(SideDefinition definition);
    ~MatchSide();
    SideDefinition definition;
    std::uint32_t identity{}, allies{}, enemies{}, flags{};
    std::int32_t damage_multiplier{1000}; // +220, initialized by 0x471bf0.
    std::int32_t outcome{};               // +164, SIDE::EndGame result: 1 won, -1 lost, zero in play.
    std::array<std::list<MatchObject*>, 5> objects;  // +320, head insertion by native object group.
    std::uint32_t incoming_mask{};                   // +208, sides that damaged an object on this side.
    std::int32_t soul_count{}, summoned_creatures{}; // +480 and +504, side accounting.
    void change_souls(int delta);                    // 0x472900, marks side statistics dirty.
    // FORM:summ statistics (0x471400 reads them): 0x472920 counts codes 9 (+492 souls harvested), 24
    // (+588 banishments), 14 (+516 / +528 manaliths / shrines created) and 25 (+540 / +552 destroyed);
    // 0x4721c0 keeps the peak wizard kills (+604, credit counts +2888 + +2892) and level (+568).
    std::int32_t souls_harvested{}, banishments{};
    std::int32_t manaliths_created{}, shrines_created{}, manaliths_destroyed{}, shrines_destroyed{};
    std::int32_t peak_kills{}, peak_level{};
    void tally(); // 0x4721c0, run by 0x471fb0 while flag 0x10000 is set and by SIDE::EndGame.
    std::function<void(MatchObject&, MatchObject*, int, int)>
        event_handler; // Controller +28, before bookkeeping.
    Match* match{};    // Bound by SceneMatch: the local side and music requests of 0x472b80.
    std::unique_ptr<SideController> controller; // +216, 0x471f60 for flag 0x40; released by 0x471f90.
    void event(MatchObject& target, MatchObject* source, int code, int value = 0);
    // The side's own NTTLIST (+0, head +12): objects it has seen, stamped with the period clock +200
    // (buildings and souls +1000000). 0x466cc0 forgets stamps older than 80 periods of 32 ticks.
    struct Known {
        MatchObject* object{};
        std::uint32_t reference{};
        int stamp{};
    };
    std::vector<Known> known;
    int period{}, stamp{}; // +204 (32-tick countdown) / +200.
    std::unique_ptr<FogOfWar> fog; // +316, allocated lazily for the local side (0x471fb0).
    std::list<MatchObject*> work; // +344: 0x473060 orders it by the objects' +868 countdowns.
    void see(MatchObject& object);                         // 0x472820 / 0x493a40.
    void schedule(MatchObject& object, int delay);         // 0x473060.
    void tick(Match& match, int ticks);                    // 0x471fb0.
    MatchObject* wizard() const;
    void attach(MatchObject& object);
    // 0x471c90 at map finalization, for each listed side by index: a held allied or enemy bit is made
    // mutual (and clears the opposite relation both ways), a missing one is cleared both ways; bits of
    // absent indices are cleared. Its controller events (19) await that host.
    void normalize(const MatchSides& sides);
    // 0x4717d0 offline (mode 1, no network slot): negative results become -1. A loss marks a side with a
    // wizard by wizard flag 0x40000, otherwise 0x471dc0 dissolves the side's relations and notifies its
    // objects (code 14); a win makes every enemy side lose. Its wizards, creatures and buildings then
    // drop their orders (on a win also common status 7, on a wizard side's loss flag 0x40000).
    // The controller is then released (0x471f90). Statistics (0x4721c0), sounds and end screens await
    // their hosts.
    int end_game(Match& match, int result, int mode = 1);
    void detach(MatchObject& object);
};
class MatchSides {
  public:
    explicit MatchSides(std::span<const SideDefinition> definitions);
    MatchSide* get(int index) const; // Native index clamp; missing entries alias side 31.
    const std::vector<std::unique_ptr<MatchSide>>& instances() const {
        return instances_;
    }

  private:
    std::vector<std::unique_ptr<MatchSide>> instances_; // Duplicate records retain their list entry.
    std::array<MatchSide*, 32> table_{};
};
} // namespace sacrifice
