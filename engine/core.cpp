#include "sacrifice/core.hpp"
#ifdef _WIN32
#include <Windows.h>
#include <ShlObj.h>
#endif
#include <charconv>
#include <chrono>
#include <cmath>
#include <format>
#include <fstream>
#include <iostream>
#include <limits>
#include <mutex>
#include <zlib.h>
#ifdef __EMSCRIPTEN__
#include <emscripten.h>
#endif

namespace sacrifice {
ByteView Reader::take(std::size_t count) {
    if (count > remaining())
        throw Error(std::format("{}: truncated at byte {}, requested {}, remaining {}", context_, position_,
                                count, remaining()));
    auto result = data_.subspan(position_, count);
    position_ += count;
    return result;
}
std::uint8_t Reader::u8() {
    return std::to_integer<std::uint8_t>(take(1)[0]);
}
std::uint16_t Reader::u16() {
    auto b = take(2);
    return static_cast<std::uint16_t>(std::to_integer<unsigned>(b[0]) |
                                      (std::to_integer<unsigned>(b[1]) << 8));
}
std::uint32_t Reader::u32() {
    auto b = take(4);
    std::uint32_t result{};
    for (unsigned i = 0; i < 4; ++i)
        result |= std::to_integer<std::uint32_t>(b[i]) << (8 * i);
    return result;
}
std::string Reader::fixed_string(std::size_t count) {
    auto b = take(count);
    std::string s;
    s.reserve(count);
    for (auto c : b) {
        if (c == std::byte{})
            break;
        s.push_back(static_cast<char>(c));
    }
    return s;
}
FourCC FourCC::from_text(std::string_view text) {
    if (text.size() != 4)
        throw Error("FourCC identifiers must contain exactly four bytes");
    std::uint32_t v{};
    for (unsigned char c : text)
        v = (v << 8) | c;
    return {v};
}
std::string FourCC::text() const {
    std::string s(4, ' ');
    for (unsigned i = 0; i < 4; ++i)
        s[i] = static_cast<char>((value >> (24 - 8 * i)) & 255);
    return s;
}
Bytes read_file(const std::filesystem::path& path, std::size_t limit) {
    std::error_code ec;
    auto size = std::filesystem::file_size(path, ec);
    if (ec)
        throw Error("Cannot read " + utf8(path) + ": " + ec.message());
    if (size > limit)
        throw Error("File exceeds size limit: " + utf8(path));
    std::ifstream in(path, std::ios::binary);
    if (!in)
        throw Error("Cannot open " + utf8(path));
    Bytes b(static_cast<std::size_t>(size));
    if (!b.empty() && !in.read(reinterpret_cast<char*>(b.data()), static_cast<std::streamsize>(b.size())))
        throw Error("Short read: " + utf8(path));
    return b;
}
Bytes inflate_exact(ByteView input, std::size_t expected, std::string_view context) {
    if (expected > 512ull * 1024 * 1024 || input.size() > std::numeric_limits<uInt>::max())
        throw Error(std::string(context) + ": decompression size limit exceeded");
    Bytes output(expected);
    std::byte dummy{};
    z_stream stream{};
    stream.next_in = reinterpret_cast<Bytef*>(const_cast<std::byte*>(input.data()));
    stream.avail_in = static_cast<uInt>(input.size());
    stream.next_out = reinterpret_cast<Bytef*>(output.empty() ? &dummy : output.data());
    stream.avail_out = static_cast<uInt>(output.empty() ? 1 : output.size());
    if (inflateInit(&stream) != Z_OK)
        throw Error("zlib initialization failed");
    int result = inflate(&stream, Z_FINISH);
    const auto consumed = stream.total_in, produced = stream.total_out;
    inflateEnd(&stream);
    if (result != Z_STREAM_END || produced != expected || consumed != input.size())
        throw Error(std::string(context) + ": invalid zlib stream or length mismatch");
    return output;
}
std::string utf8(const std::filesystem::path& path) {
    auto s = path.u8string();
    return {reinterpret_cast<const char*>(s.data()), s.size()};
}
#ifdef _WIN32
std::wstring widen(std::string_view text) {
    if (text.empty())
        return {};
    int n = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, text.data(), static_cast<int>(text.size()),
                                nullptr, 0);
    if (!n)
        throw Error("Invalid UTF-8 string");
    std::wstring result(n, L'\0');
    MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, text.data(), static_cast<int>(text.size()),
                        result.data(), n);
    return result;
}
#endif
std::filesystem::path game_path(std::string_view text) {
#ifdef _WIN32
    return std::filesystem::path(widen(text));
#else
    std::string normalized(text);
    std::replace(normalized.begin(), normalized.end(), '\\', '/');
    return std::filesystem::path(normalized);
#endif
}
std::filesystem::path application_data() {
#ifdef _WIN32
    PWSTR raw{};
    if (FAILED(SHGetKnownFolderPath(FOLDERID_LocalAppData, 0, nullptr, &raw)))
        throw Error("Cannot locate LocalAppData");
    std::filesystem::path p(raw);
    CoTaskMemFree(raw);
    p /= "SacrificeModern";
#else
    std::filesystem::path p("/persist/SacrificeModern");
#ifdef __EMSCRIPTEN__
    if (EM_ASM_INT({ return typeof process !== 'undefined' && !!process.versions?.node; }))
        p = std::filesystem::current_path() / ".sacrifice-data";
#endif
#endif
    std::filesystem::create_directories(p);
    return p;
}
namespace {
bool trace_ai_enabled = false;
}
bool trace_ai() {
    return trace_ai_enabled;
}
void set_trace_ai(bool enabled) {
    trace_ai_enabled = enabled;
}
void log(std::string_view message) {
    static std::mutex mutex;
    const std::lock_guard lock(mutex);
    std::ofstream out(application_data() / "SacrificeModern.log", std::ios::app);
    auto stamp = std::chrono::system_clock::now();
    out << std::format("{:%F %T} {}\n", stamp, message);
#ifdef _WIN32
    // Logging must never throw: callers log from inside their own catch blocks.
    try {
        OutputDebugStringW(widen(std::string(message) + "\n").c_str());
    } catch (const std::exception&) {
        OutputDebugStringA((std::string(message) + "\n").c_str());
    }
#else
    std::clog << message << '\n';
#endif
}
Arguments Arguments::parse(const std::vector<std::string>& values) {
    Arguments a;
    for (std::size_t i = 0; i < values.size(); ++i) {
        auto v = values[i];
        auto next = [&]() -> const std::string& {
            if (++i == values.size())
                throw Error("Missing argument for " + v);
            return values[i];
        };
        if (v == "--game-dir")
            a.game_dir = game_path(next());
        else if (v == "--map")
            a.map = game_path(next());
        else if (v == "--debug-trial")
            a.map = L"maps/(2) Trial.scp"; // Explicit maps skip the title/lobby and start a playable match.
        else if (v == "--reference")
            a.reference = game_path(next());
        else if (v == "--file")
            a.file = game_path(next());
        else if (v == "--output")
            a.output = game_path(next());
        else if (v == "--resource")
            a.resource = next();
        else if (v == "--campaign-actions")
            a.campaign_actions = next();
        else if (v == "--campaign")
            a.campaign = next();
        else if (v == "--model-frame") {
            const auto& value = next();
            float number{};
            const auto [end, error] = std::from_chars(value.data(), value.data() + value.size(), number);
            if (error != std::errc{} || end != value.data() + value.size() || !std::isfinite(number) ||
                number < 0)
                throw Error("--model-frame requires a finite nonnegative number");
            a.model_frame = number;
        } else if (v == "--model-vertices" || v == "--scene-ticks" || v == "--scene-warmup" || v == "--graphics-detail" ||
                   v == "--scene-weather" || v == "--bench-frames") {
            const auto& value = next();
            unsigned number{};
            const auto [end, error] = std::from_chars(value.data(), value.data() + value.size(), number);
            if (error != std::errc{} || end != value.data() + value.size())
                throw Error(v + " requires an unsigned integer");
            if (v == "--bench-frames") {
                if (number < 1 || number > 10000)
                    throw Error("--bench-frames requires 1 to 10000 frames");
                a.bench_frames = number;
            } else if (v == "--scene-weather") {
                if (number < 1 || number > 6)
                    throw Error("--scene-weather requires a weather kind from 1 (rain) to 6 (insects)");
                a.scene_weather = number;
            } else if (v == "--graphics-detail") {
                if (number > 7)
                    throw Error("--graphics-detail requires a native level from 0 to 7");
                a.graphics_detail = number;
            } else if (v == "--scene-ticks" || v == "--scene-warmup") {
                if (number > 36000)
                    throw Error("--scene-ticks is limited to 36000 (ten minutes)");
                if (v == "--scene-ticks")
                    a.scene_ticks = number;
                else
                    a.scene_warmup = number;
            } else
                a.model_vertices = number;
        } else if (v == "--model-detail") {
            const auto& value = next();
            float number{};
            const auto [end, error] = std::from_chars(value.data(), value.data() + value.size(), number);
            if (error != std::errc{} || end != value.data() + value.size() || !std::isfinite(number) ||
                number < 1 || number > 100)
                throw Error("--model-detail requires a number from 1 to 100");
            a.model_detail = number;
        } else if (v == "--clip")
            a.clip = next();
        else if (v == "--smoke")
            a.smoke = true;
        else if (v == "--trace-ai")
            a.trace_ai = true;
        else if (v == "--no-ui")
            a.hide_ui = true;
        else if (v == "--classic-look")
            a.classic_look = true;
        else if (v == "--no-environment")
            a.hide_environment = true;
        else if (v == "--view-yaw") {
            const auto& value = next();
            float number{};
            const auto [end, error] = std::from_chars(value.data(), value.data() + value.size(), number);
            if (error != std::errc{} || end != value.data() + value.size() || !std::isfinite(number))
                throw Error("--view-yaw requires a finite number of degrees");
            a.view_yaw = number;
        } else if (v == "--camera") {
            const auto& value = next();
            std::array<float, 5> numbers{};
            const char* cursor = value.data();
            const char* const last = value.data() + value.size();
            for (std::size_t n = 0; n < numbers.size(); ++n) {
                const auto [end, error] = std::from_chars(cursor, last, numbers[n]);
                if (error != std::errc{} || !std::isfinite(numbers[n]) ||
                    (n + 1 < numbers.size() ? end == last || *end != ',' : end != last))
                    throw Error("--camera requires X,Y,HEIGHT,YAW,PITCH");
                cursor = n + 1 < numbers.size() ? end + 1 : end;
            }
            a.camera = numbers;
        } else if (v == "--scene-pointer") {
            const auto& value = next();
            std::array<float, 2> numbers{};
            const char* cursor = value.data();
            const auto* last = cursor + value.size();
            for (unsigned n = 0; n < numbers.size(); ++n) {
                const auto [end, error] = std::from_chars(cursor, last, numbers[n]);
                if (error != std::errc{} || !std::isfinite(numbers[n]) ||
                    (n == 0 ? end == last || *end != ',' : end != last))
                    throw Error("--scene-pointer requires X,Y");
                cursor = n == 0 ? end + 1 : end;
            }
            a.scene_pointer = numbers;
        } else if (v == "--scene-target") {
            a.scene_target = FourCC::from_text(next());
        } else if (v == "--scene-select" || v == "--scene-hover") {
            const auto& value = next();
            unsigned number{};
            const auto [end, error] = std::from_chars(value.data(), value.data() + value.size(), number);
            if (error != std::errc{} || end != value.data() + value.size())
                throw Error(v + " requires a character ordinal");
            if (v == "--scene-select")
                a.scene_select = number;
            else
                a.scene_hover = number;
        } else if (v == "--scene-move") {
            const auto& value = next();
            std::array<float, 4> numbers{};
            const char* cursor = value.data();
            const auto* last = cursor + value.size();
            for (unsigned n = 0; n < numbers.size(); ++n) {
                const auto [end, error] = std::from_chars(cursor, last, numbers[n]);
                if (error != std::errc{} || !std::isfinite(numbers[n]) ||
                    (n == 0 &&
                     (numbers[n] < 0 || numbers[n] > 100000 || std::trunc(numbers[n]) != numbers[n])) ||
                    (n + 1 < numbers.size() ? end == last || *end != ',' : end != last))
                    throw Error("--scene-move requires CHARACTER,X,Y,Z");
                cursor = n + 1 < numbers.size() ? end + 1 : end;
            }
            a.scene_move = numbers;
        } else if (v == "--scene-attack" || v == "--scene-souls" || v == "--scene-soul-move") {
            const auto& value = next();
            std::array<int, 2> numbers{};
            const char* cursor = value.data();
            const auto* last = cursor + value.size();
            for (unsigned n = 0; n < 2; ++n) {
                const auto [end, error] = std::from_chars(cursor, last, numbers[n]);
                if (error != std::errc{} || numbers[n] < 0 ||
                    (n == 0 ? end == last || *end != ',' : end != last))
                    throw Error(v + " requires two nonnegative integers separated by a comma");
                cursor = n == 0 ? end + 1 : end;
            }
            if (v == "--scene-attack")
                a.scene_attack = numbers;
            else if (v == "--scene-souls")
                a.scene_souls = numbers;
            else
                a.scene_soul_move = numbers;
        } else if (v == "--scene-cast") {
            const auto& value = next();
            const auto comma = value.find(','),
                       second = value.find(',', comma == std::string::npos ? 0 : comma + 1);
            if (comma == std::string::npos || second == std::string::npos || second - comma != 5)
                throw Error("--scene-cast requires CHARACTER,SPELL,TARGET_CHARACTER");
            unsigned actor{};
            int target{};
            const auto [aend, aerror] = std::from_chars(value.data(), value.data() + comma, actor);
            const auto [tend, terror] =
                std::from_chars(value.data() + second + 1, value.data() + value.size(), target);
            if (aerror != std::errc{} || aend != value.data() + comma || terror != std::errc{} ||
                tend != value.data() + value.size() || target < 0)
                throw Error("--scene-cast requires CHARACTER,SPELL,TARGET_CHARACTER");
            a.scene_cast =
                std::tuple{actor, FourCC::from_text(std::string_view(value).substr(comma + 1, 4)), target};
        } else if (v == "--front-end") {
            const auto& value = next();
            if (value.size() != 4)
                throw Error("--front-end requires a FORM id such as titl, 1ply, loby or cgam");
            a.front_end = FourCC::from_text(value);
        } else if (v == "--scene-ritual") {
            const auto& value = next();
            if (value != "sacc" && value != "sacu")
                throw Error("--scene-ritual requires sacc or sacu");
            a.scene_ritual = FourCC::from_text(value);
        } else if (v == "--scene-control") {
            const auto& value = next();
            std::array<int, 4> numbers{};
            const char* cursor = value.data();
            const char* const last = value.data() + value.size();
            for (std::size_t n = 0; n < numbers.size(); ++n) {
                const auto [end, error] = std::from_chars(cursor, last, numbers[n]);
                if (error != std::errc{} ||
                    (n == 0 ? numbers[n] < 0 : numbers[n] < -128 || numbers[n] > 128) ||
                    (n + 1 < numbers.size() ? end == last || *end != ',' : end != last))
                    throw Error("--scene-control requires CHARACTER,YAW,FORWARD_OR_PITCH,THROTTLE (controls "
                                "-128..128)");
                cursor = n + 1 < numbers.size() ? end + 1 : end;
            }
            a.scene_control = numbers;
        } else if (v == "--scene-altar-ruin") {
            const auto& value = next();
            unsigned ordinal{};
            const auto [end, error] = std::from_chars(value.data(), value.data() + value.size(), ordinal);
            if (error != std::errc{} || end != value.data() + value.size())
                throw Error("--scene-altar-ruin requires a wizard character ordinal");
            a.scene_altar_ruin = ordinal;
        } else if (v == "--scene-building-damage" || v == "--scene-creature-damage" || v == "--scene-burn") {
            const auto& value = next();
            std::array<int, 2> numbers{};
            const char* cursor = value.data();
            const auto* last = value.data() + value.size();
            for (std::size_t n = 0; n < numbers.size(); ++n) {
                const auto [end, error] = std::from_chars(cursor, last, numbers[n]);
                if (error != std::errc{} || numbers[n] < 0 ||
                    (n == 0 ? end == last || *end != ',' : end != last))
                    throw Error(v + " requires ORDINAL,AMOUNT (nonnegative integers)");
                cursor = n == 0 ? end + 1 : end;
            }
            if (v == "--scene-building-damage")
                a.building_damage = numbers;
            else if (v == "--scene-burn")
                a.scene_burn = numbers;
            else
                a.creature_damage = numbers;
        } else if (v == "--scene-building-reveal") {
            const auto& value = next();
            unsigned ordinal{};
            float reveal{};
            const auto* last = value.data() + value.size();
            const auto first = std::from_chars(value.data(), last, ordinal);
            if (first.ec != std::errc{} || first.ptr == last || *first.ptr != ',')
                throw Error("--scene-building-reveal requires BUILDING,FRACTION (0..1)");
            const auto second = std::from_chars(first.ptr + 1, last, reveal);
            if (second.ec != std::errc{} || second.ptr != last || !std::isfinite(reveal) || reveal < 0 ||
                reveal > 1)
                throw Error("--scene-building-reveal requires BUILDING,FRACTION (0..1)");
            a.building_reveal = std::pair{ordinal, reveal};
        } else if (v == "--model-preview")
            a.model_preview = true;
        else if (v.starts_with("--"))
            throw Error("Unknown option: " + v);
        else if (a.command.empty())
            a.command = v;
        else
            throw Error("Unexpected argument: " + v);
    }
    return a;
}
std::filesystem::path discover_game_dir(std::filesystem::path candidate) {
    auto valid = [](const auto& p) {
        return std::filesystem::is_regular_file(p / "data/main.wad") &&
               std::filesystem::is_directory(p / "maps");
    };
    if (!candidate.empty()) {
        candidate = std::filesystem::absolute(candidate).lexically_normal();
        if (valid(candidate))
            return candidate;
        throw Error("Game assets not found in " + utf8(candidate) + " (expected data/main.wad and maps)");
    }
#ifdef _WIN32
    wchar_t executable[32768]{};
    GetModuleFileNameW(nullptr, executable, 32768);
    auto p = std::filesystem::path(executable).parent_path();
#else
    auto p = std::filesystem::path("/game");
#endif
    for (unsigned i = 0; i < 8 && !p.empty(); ++i) {
        if (valid(p))
            return p;
        auto parent = p.parent_path();
        if (parent == p)
            break;
        p = parent;
    }
    throw Error("Cannot locate game assets; pass --game-dir <Sacrifice installation>");
}
} // namespace sacrifice
