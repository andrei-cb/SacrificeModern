#include "sacrifice/character.hpp"
#include <algorithm>
#include <cctype>
#include <cstring>
#include <fstream>

namespace sacrifice {
const std::array<WizardChoice, 15> wizard_choices{{
    {FourCC::from_text("cwe2"), "Eldred"},    {FourCC::from_text("cwl0"), "Shakti"},
    {FourCC::from_text("cwf0"), "Ambassador Buta"}, {FourCC::from_text("cwe0"), "Abraxus"},
    {FourCC::from_text("cwa0"), "Jadugarr"},  {FourCC::from_text("cwd0"), "The Ragman"},
    {FourCC::from_text("cwl1"), "Yogo"},      {FourCC::from_text("cwf1"), "Grakkus"},
    {FourCC::from_text("cwa1"), "Seerix"},    {FourCC::from_text("cwd1"), "Acheron"},
    {FourCC::from_text("cwl2"), "Hachimen"},  {FourCC::from_text("cwf2"), "Charlotte"},
    {FourCC::from_text("cwd2"), "Sorcha"},    {FourCC::from_text("cwe1"), "Mithras"},
    {FourCC::from_text("cwa2"), "Marduk"},
}};
namespace {
bool same_text(std::string_view a, std::string_view b) {
    return std::ranges::equal(a, b, [](char x, char y) {
        return std::tolower(static_cast<unsigned char>(x)) == std::tolower(static_cast<unsigned char>(y));
    });
}
void put_u32(Bytes& out, std::size_t at, std::uint32_t value) {
    for (int i = 0; i < 4; ++i)
        out[at + i] = static_cast<std::byte>(value >> (8 * i));
}
void put_text(Bytes& out, std::size_t at, std::size_t size, std::string_view text) {
    // strncpy(.., size - 1) followed by an explicit terminator.
    const auto count = std::min(text.size(), size - 1);
    std::memcpy(out.data() + at, text.data(), count);
}
std::string text_at(ByteView bytes, std::size_t at, std::size_t size) {
    const auto* begin = reinterpret_cast<const char*>(bytes.data() + at);
    return {begin, strnlen(begin, size)};
}
} // namespace
Character Character::decode(ByteView bytes) {
    // 0x4a02b0 accepts only a complete record; the size word is not checked, the 1000 at +116 (stat 12)
    // is forced after loading.
    if (bytes.size() != file_size)
        throw Error("Error reading CHARACTER file: wrong size");
    Reader r(bytes, "CHARACTER");
    Character c;
    r.u16();
    if (r.u16() != version)
        throw Error("Unsupported CHARACTER version");
    c.name = text_at(bytes, 4, 32);
    c.game_type = text_at(bytes, 36, 32);
    r.take(64);
    c.campaign_node = r.u32();
    c.wizard = FourCC{r.u32()};
    for (auto& stat : c.stats)
        stat = r.i16();
    c.stats[12] = 1000;
    c.maximum_mana = static_cast<std::int32_t>(r.u32());
    c.auxiliary = static_cast<std::int32_t>(r.u32());
    for (auto& god : c.gods)
        god = r.u8();
    c.level = static_cast<std::int32_t>(r.u32());
    c.experience = static_cast<std::int32_t>(r.u32());
    const auto learned = r.u32();
    if (learned > learned_limit)
        throw Error("CHARACTER learned list exceeds 64 entries");
    for (std::size_t i = 0; i < learned_limit; ++i) {
        const FourCC id{r.u32()};
        if (i < learned)
            c.learned.push_back(id);
    }
    std::ranges::copy(r.take(8), c.reserved.begin());
    c.credit_count_80 = static_cast<std::int32_t>(r.u32());
    c.credit_count_20 = static_cast<std::int32_t>(r.u32());
    const auto count = r.u32();
    if (count > r.remaining() / 32)
        throw Error("CHARACTER flag table exceeds the record");
    for (std::uint32_t i = 0; i < count; ++i) {
        const auto at = r.position();
        r.take(28);
        c.flags.push_back({text_at(bytes, at, 28), static_cast<std::int32_t>(r.u32())});
    }
    // Early port versions left file +124 at zero and reconstructed mana only in memory.
    // Preserve real stored values (including boons), but recover that missing base for old saves.
    if (c.maximum_mana == 0) {
        c.maximum_mana = 1000 + 100 * std::clamp(c.level, 0, 14);
        log("Recovered maximum mana for legacy character " + c.name + ": " +
            std::to_string(c.maximum_mana));
    }
    return c;
}
Bytes Character::encode() const {
    Bytes out(file_size);
    out[0] = static_cast<std::byte>(file_size & 255);
    out[1] = static_cast<std::byte>(file_size >> 8);
    out[2] = static_cast<std::byte>(version);
    put_text(out, 4, 32, name);
    put_text(out, 36, 32, game_type);
    put_u32(out, 68, campaign_node);
    put_u32(out, 72, wizard.value);
    for (std::size_t i = 0; i < stats.size(); ++i) {
        out[76 + 2 * i] = static_cast<std::byte>(static_cast<std::uint16_t>(stats[i]) & 255);
        out[77 + 2 * i] = static_cast<std::byte>(static_cast<std::uint16_t>(stats[i]) >> 8);
    }
    put_u32(out, 124, static_cast<std::uint32_t>(maximum_mana));
    put_u32(out, 128, static_cast<std::uint32_t>(auxiliary));
    for (std::size_t i = 0; i < gods.size(); ++i)
        out[132 + i] = static_cast<std::byte>(gods[i]);
    put_u32(out, 152, static_cast<std::uint32_t>(level));
    put_u32(out, 156, static_cast<std::uint32_t>(experience));
    const auto learned_count = std::min(learned.size(), learned_limit);
    put_u32(out, 160, static_cast<std::uint32_t>(learned_count));
    for (std::size_t i = 0; i < learned_count; ++i)
        put_u32(out, 164 + 4 * i, learned[i].value);
    std::ranges::copy(reserved, out.begin() + 420);
    put_u32(out, 428, static_cast<std::uint32_t>(credit_count_80));
    put_u32(out, 432, static_cast<std::uint32_t>(credit_count_20));
    const auto flag_count = std::min(flags.size(), (file_size - 440) / 32);
    put_u32(out, 436, static_cast<std::uint32_t>(flag_count));
    for (std::size_t i = 0; i < flag_count; ++i) {
        put_text(out, 440 + 32 * i, 28, flags[i].name);
        put_u32(out, 440 + 32 * i + 28, static_cast<std::uint32_t>(flags[i].value));
    }
    return out;
}
std::int32_t Character::flag(std::string_view key) const {
    for (const auto& entry : flags)
        if (same_text(entry.name, key))
            return entry.value;
    return 0;
}
void Character::set_flag(std::string_view key, std::int32_t value) {
    for (auto& entry : flags)
        if (same_text(entry.name, key)) {
            entry.value = value;
            return;
        }
    flags.push_back({std::string(key.substr(0, 27)), value});
}
void Character::absorb(const WizardProgression& p) {
    stats = p.stats;
    maximum_mana = p.maximum_mana;
    auxiliary = p.auxiliary;
    std::copy_n(p.gods.begin(), gods.size(), gods.begin());
    level = p.level;
    experience = p.experience;
    learned.assign(p.learned.begin(), p.learned.begin() + std::min(p.learned.size(), learned_limit));
    credit_count_80 += p.credit_count_80;
    credit_count_20 += p.credit_count_20;
}
std::uint8_t Character::enter_mission(std::uint32_t god, int start, CrtRandom& random) {
    const auto at = static_cast<std::size_t>(std::clamp(start, 0, static_cast<int>(gods.size())));
    if (!god)
        god = at < gods.size() && gods[at] ? gods[at] : static_cast<std::uint32_t>(random.next() % 5 + 1);
    const auto chosen = static_cast<std::uint8_t>(god);
    stats[18] = static_cast<std::int16_t>(god & 0xffff);
    for (std::size_t i = 0; i < at; ++i)
        if (!gods[i])
            gods[i] = chosen;
    std::fill(gods.begin() + static_cast<std::ptrdiff_t>(at), gods.end(), chosen);
    return chosen;
}
void Character::reset_wizard(FourCC id) {
    // 0x44f900 loads the WIZD with god zero (every gods byte and stat 18), then 0x44fc90 sets level zero.
    wizard = id;
    WizardProgression initial;
    initial.stats[18] = 0;
    absorb(initial);
}
WizardProgression Character::progression() const {
    WizardProgression p;
    p.stats = stats;
    p.auxiliary = auxiliary;
    std::ranges::copy(gods, p.gods.begin());
    p.level = level;
    p.experience = experience;
    p.learned = learned;
    // 0x4a04d0 stores WIZD +144 at CHARACTER +140 (file +124), including mission boons.
    p.maximum_mana = maximum_mana;
    return p;
}
int Character::validate_name(std::string_view name) {
    static constexpr std::string_view allowed = "!@#$^&*()_-+=|/?[]{}';:<>,.~";
    int counted = 0, errors = 0;
    for (const char ch : name) {
        const auto c = static_cast<unsigned char>(ch);
        if (std::isalnum(c) || (c && allowed.find(ch) != std::string_view::npos))
            ++counted;
        else
            ++errors;
        if (counted >= 31)
            break;
    }
    if (counted == 31 || counted == 0 || counted > 16)
        ++errors;
    if (name.empty() || !std::isalpha(static_cast<unsigned char>(name.front())))
        ++errors;
    return errors ? (counted >= 4 ? 1 : -1) : 0;
}
void CharacterRoster::import(const std::filesystem::path& installation, std::string_view game_type) const {
    const auto target = root_ / game_type;
    if (std::filesystem::exists(target))
        return;
    std::filesystem::create_directories(target);
    const auto source = installation / "saves" / "Characters" / game_type;
    if (!std::filesystem::is_directory(source))
        return;
    for (const auto& entry : std::filesystem::directory_iterator(source))
        if (entry.is_regular_file() && same_text(utf8(entry.path().extension()), ".wiz"))
            std::filesystem::copy_file(entry.path(), target / entry.path().filename(),
                                       std::filesystem::copy_options::skip_existing);
}
std::vector<Character> CharacterRoster::list(std::string_view game_type) const {
    std::vector<Character> result;
    const auto directory = root_ / game_type;
    if (!std::filesystem::is_directory(directory))
        return result;
    for (const auto& entry : std::filesystem::directory_iterator(directory)) {
        if (!entry.is_regular_file() || !same_text(utf8(entry.path().extension()), ".wiz"))
            continue;
        try {
            auto character = Character::decode(read_file(entry.path(), Character::file_size + 1));
            // 0x4a02b0 rejects a record whose name or game type does not match its path.
            if (same_text(character.name, utf8(entry.path().stem())) && same_text(character.game_type, game_type))
                result.push_back(std::move(character));
            else
                log("Error reading CHARACTER file " + utf8(entry.path()));
        } catch (const std::exception& error) {
            log(std::string(error.what()) + ": " + utf8(entry.path()));
        }
    }
    std::ranges::sort(result, [](const Character& a, const Character& b) {
        return std::lexicographical_compare(a.name.begin(), a.name.end(), b.name.begin(), b.name.end(),
                                            [](char x, char y) {
                                                return std::tolower(static_cast<unsigned char>(x)) <
                                                       std::tolower(static_cast<unsigned char>(y));
                                            });
    });
    return result;
}
std::vector<Character> CharacterRoster::list_all() const {
    std::vector<Character> result;
    if (!std::filesystem::is_directory(root_))
        return result;
    for (const auto& entry : std::filesystem::directory_iterator(root_))
        if (entry.is_directory())
            for (auto& character : list(utf8(entry.path().filename())))
                result.push_back(std::move(character));
    return result;
}
std::filesystem::path CharacterRoster::path(const Character& character) const {
    return root_ / character.game_type / (character.name + ".wiz");
}
void CharacterRoster::save(const Character& character) const {
    const auto file = path(character);
    std::filesystem::create_directories(file.parent_path());
    const auto bytes = character.encode();
    std::ofstream out(file, std::ios::binary | std::ios::trunc);
    out.write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
    if (!out)
        throw Error("Error writing CHARACTER file " + utf8(file));
}
void CharacterRoster::remove(const Character& character) const {
    std::error_code ignored;
    std::filesystem::remove(path(character), ignored);
}
void CharacterRoster::rename(Character& character, std::string_view name) const {
    if (same_text(character.name, name))
        return;
    remove(character);
    character.name = std::string(name.substr(0, 31));
    save(character);
}
bool CharacterRoster::wizard_available(FourCC id) const {
    const int needed = id == FourCC::from_text("cwe1") ? 1 : id == FourCC::from_text("cwa2") ? 2 : 0;
    if (!needed)
        return true;
    return std::ranges::any_of(list_all(), [&](const Character& c) { return c.flag("BeatCampaign") == needed; });
}
bool CharacterRoster::custom_path_available() const {
    return std::ranges::any_of(list_all(), [](const Character& c) { return c.flag("BeatCampaign") != 0; });
}
std::vector<WizardChoice> CharacterRoster::wizards() const {
    std::vector<WizardChoice> result;
    for (const auto& choice : wizard_choices)
        if (wizard_available(choice.id))
            result.push_back(choice);
    return result;
}
} // namespace sacrifice
