#include "sacrifice/campaign.hpp"
#include "sacrifice/character.hpp"
#include "sacrifice/compat.hpp"
#include <algorithm>
#include <cctype>
#include <charconv>
#include <sstream>

namespace sacrifice {
std::string Campaign::decode(ByteView data) {
    if (data.size() < 2)
        throw Error("Campaign descriptor has no key bytes");
    std::uint8_t key = std::to_integer<std::uint8_t>(data[data.size() - 2]);
    const auto increment = std::to_integer<std::uint8_t>(data.back());
    std::string text;
    text.reserve(data.size() - 2);
    for (auto byte : data.first(data.size() - 2)) {
        auto value = static_cast<std::uint8_t>(std::to_integer<std::uint8_t>(byte) - key);
        text.push_back(static_cast<char>(value));
        key = static_cast<std::uint8_t>(key + increment);
    }
    return text;
}
Campaign Campaign::load(const std::filesystem::path& path) {
    return parse(decode(read_file(path, 4 * 1024 * 1024)));
}
Campaign Campaign::parse(std::string text) {
    Campaign result;
    result.text = std::move(text);
    CampaignLevel* current{};
    std::istringstream lines(result.text);
    std::string line;
    unsigned line_number{};
    while (std::getline(lines, line)) {
        ++line_number;
        bool quoted = false;
        std::vector<std::string> tokens;
        std::string token;
        for (char c : line) {
            if (c == '"') {
                quoted = !quoted;
                continue;
            }
            if (c == ';' && !quoted)
                break;
            if (std::isspace(static_cast<unsigned char>(c)) && !quoted) {
                if (!token.empty()) {
                    tokens.push_back(token);
                    token.clear();
                }
            } else
                token += c;
        }
        if (quoted)
            throw Error("Campaign line " + std::to_string(line_number) + ": unterminated quote");
        if (!token.empty())
            tokens.push_back(token);
        if (tokens.empty())
            continue;
        const auto& keyword = tokens[0];
        auto value = [&]() -> const std::string& {
            if (tokens.size() != 2)
                throw Error("Campaign line " + std::to_string(line_number) + ": expected one value");
            return tokens[1];
        };
        if (keyword == "NAME") {
            if (current)
                throw Error("NAME inside campaign level");
            result.name = value();
        } else if (keyword == "WIZARDTYPE") {
            if (current)
                throw Error("WIZARDTYPE inside campaign level");
            result.wizard_types.push_back(value());
        } else if (keyword == "LEVEL") {
            if (current)
                throw Error("Nested campaign levels");
            unsigned n{};
            const auto& str = value();
            auto [end, ec] = std::from_chars(str.data(), str.data() + str.size(), n);
            if (ec != std::errc{} || end != str.data() + str.size() || n > 13 || result.levels.contains(n))
                throw Error("Invalid or duplicate campaign level");
            current = &result.levels.emplace(n, CampaignLevel{n, {}}).first->second;
        } else if (keyword == "ENDLEVEL") {
            if (!current || tokens.size() != 1)
                throw Error("Unmatched ENDLEVEL");
            current = nullptr;
        } else if (keyword == "DEFAULT" || keyword == "LIFE" || keyword == "FIRE" || keyword == "DEATH" ||
                   keyword == "EARTH" || keyword == "AIR" || keyword == "MAP7" || keyword == "MAP8") {
            if (!current)
                throw Error("Mission outside campaign level");
            if (!current->missions.emplace(keyword, value()).second)
                throw Error("Duplicate campaign mission choice");
        } else
            throw Error("Campaign line " + std::to_string(line_number) + ": unknown keyword " + keyword);
    }
    if (current)
        throw Error("Unclosed campaign level");
    if (result.name.empty() || result.levels.empty())
        throw Error("Empty campaign descriptor");
    return result;
}
CampaignHost::CampaignHost(const std::filesystem::path& root, std::string name)
    : root_(root / "maps" / "campaign"), name_(std::move(name)) {
    const auto encoded = root_ / (name_ + ".cmp");
    campaign_ = std::filesystem::exists(encoded) ? Campaign::load(encoded)
                                                 : Campaign::parse([&] {
                                                       const auto bytes = read_file(root_ / (name_ + ".txt"));
                                                       return std::string(reinterpret_cast<const char*>(bytes.data()),
                                                                          bytes.size());
                                                   }());
}
std::vector<FourCC> CampaignHost::wizards() const {
    std::vector<FourCC> result;
    for (const auto& type : campaign_.wizard_types)
        for (const auto& choice : wizard_choices)
            if (ascii_iequals(choice.name, type)) {
                result.push_back(choice.id);
                break;
            }
    return result;
}
const CampaignLevel* CampaignHost::node(int level) const {
    for (int n = std::max(level, 0); n < 14; ++n)
        if (const auto found = campaign_.levels.find(static_cast<unsigned>(n)); found != campaign_.levels.end())
            return &found->second;
    return nullptr;
}
std::filesystem::path CampaignHost::mission_path(const std::string& file) const {
    return root_ / name_ / game_path(file); // "%s\\%s\\%s": Dir, the game type's name, the mission file.
}
std::optional<std::filesystem::path> CampaignHost::start(Character& character) const {
    const auto* level = node(static_cast<int>(character.campaign_node));
    if (!level || !level->missions.contains("DEFAULT"))
        return std::nullopt;
    character.campaign_node = level->number;
    return mission_path(level->missions.at("DEFAULT"));
}
bool CampaignHost::choose(const Character& character, unsigned choice) {
    const auto found = campaign_.levels.find(character.campaign_node);
    if (choice >= choices.size() || found == campaign_.levels.end())
        return false;
    const auto mission = found->second.missions.find(choices[choice]);
    if (mission == found->second.missions.end())
        return false;
    pending_ = mission->second;
    return true;
}
std::optional<std::filesystem::path> CampaignHost::victory(Character& character) {
    if (pending_) {
        const auto file = std::exchange(pending_, std::nullopt);
        return mission_path(*file);
    }
    ++character.campaign_node;
    return start(character);
}
} // namespace sacrifice
