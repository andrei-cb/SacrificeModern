#pragma once
#include "core.hpp"
#include <array>
#include <map>
#include <optional>

namespace sacrifice {
struct CampaignLevel {
    unsigned number{};
    std::map<std::string, std::string> missions;
};
struct Campaign {
    std::string name;
    std::vector<std::string> wizard_types;
    std::map<unsigned, CampaignLevel> levels;
    std::string text;
    static Campaign load(const std::filesystem::path& path);
    static Campaign parse(std::string text);
    static std::string decode(ByteView bytes);
};
struct Character;
// The campaign game type (type 1, vtable 0x4c12e0) over maps\campaign\<name>.cmp. Its level nodes hold a
// mask of mission choices in the 0x4d38c8 keyword order; the character's +84 is the current level.
class CampaignHost {
  public:
    static constexpr std::array<const char*, 8> choices{"DEFAULT", "LIFE",  "FIRE", "EARTH",
                                                        "AIR",     "DEATH", "MAP7", "MAP8"};
    // 0x49ec50 / 0x49ed90 (0x49f400 decodes the .cmp, 0x49efc0 reads a plain .txt).
    CampaignHost(const std::filesystem::path& root, std::string name);
    [[nodiscard]] const std::string& name() const { return name_; }
    [[nodiscard]] const Campaign& campaign() const { return campaign_; }
    // +24 (this+100): the WIZARDTYPE names resolved through the 0x4d3838 table.
    [[nodiscard]] std::vector<FourCC> wizards() const;
    // 0x49ed40: the first defined level at or after `level` (0..13), or null.
    [[nodiscard]] const CampaignLevel* node(int level) const;
    // 0x49ee20: moves the character to its node and returns that node's DEFAULT mission; empty when the
    // campaign is over (no node, or one without DEFAULT), which ends at the title (+88).
    std::optional<std::filesystem::path> start(Character& character) const;
    // +52 (0x49e960): a fresh mission forgets the previous portal selection.
    void begin_mission() { pending_.reset(); }
    // +120 (0x49ee90): trigger action 172 records the current level's mission for `choice`, if listed.
    bool choose(const Character& character, unsigned choice);
    void choose_filename(std::string file) { pending_ = std::move(file); }
    // +84 (0x49ef20) after a victory: a chosen mission plays next without advancing; otherwise the
    // character moves on a level (0x49ee20 then finds the next node).
    std::optional<std::filesystem::path> victory(Character& character);
    [[nodiscard]] std::filesystem::path mission_path(const std::string& file) const;

  private:
    std::filesystem::path root_;
    std::string name_;
    Campaign campaign_;
    std::optional<std::string> pending_; // this+92.
};
} // namespace sacrifice
