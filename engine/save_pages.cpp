#include "sacrifice/save_pages.hpp"
#include "sacrifice/compat.hpp"
#include <ctime>

namespace sacrifice {
namespace {
FourCC id(const char* text) {
    return FourCC::from_text(text);
}
} // namespace
SavePages::SavePages(Host host, std::filesystem::path folder) : host_(std::move(host)), folder_(std::move(folder)) {}
bool SavePages::page(FourCC value) {
    for (const auto* each : {"sgam", "lgam", "sav?", "del?", "nohd", "lerr", "sprg", "lprg"})
        if (value == id(each))
            return true;
    return false;
}
void SavePages::show(FourCC value) {
    if (host_.show)
        host_.show(value, in_game);
}
void SavePages::open(FourCC list_page, bool game) {
    in_game = game;
    parent = {};
    try {
        names = WorldSave::names(folder_);
    } catch (const std::exception& e) {
        names.clear();
        if (host_.log)
            host_.log(std::string("Saved games cannot be listed: ") + e.what());
    }
    row = -1;
    for (std::size_t i = 0; i < names.size(); ++i)
        if (!last_save.empty() && ascii_iequals(names[i], last_save))
            row = static_cast<int>(i);
    if (row >= 0)
        name = names[static_cast<std::size_t>(row)];
    scroll = std::max(0, row - 4);
    refresh();
    show(list_page);
}
void SavePages::select(int index) {
    if (index < 0 || index >= static_cast<int>(names.size()))
        return;
    row = index;
    name = names[static_cast<std::size_t>(index)];
    refresh();
}
void SavePages::rename(std::string typed) {
    name = std::move(typed);
    row = -1;
    for (std::size_t i = 0; i < names.size(); ++i)
        if (ascii_iequals(names[i], name))
            row = static_cast<int>(i);
    refresh();
}
// 0x4a7860 / 0x4a7b20: the header must name an installed map.
void SavePages::refresh() {
    if (preview_name_ == name && (preview || name.empty()))
        return;
    preview_name_ = name;
    preview.reset();
    thumbnail.reset();
    ++thumbnail_revision;
    description_scroll = 0;
    if (!WorldSave::exists(name, folder_))
        return;
    try {
        if (const auto saved = WorldSave::read(name, folder_)) {
            saved->mission(host_.root);
            preview = saved->header();
            thumbnail = saved->thumbnail();
        }
    } catch (const std::exception& e) {
        if (host_.log)
            host_.log("Saved game " + name + " is not loadable: " + e.what());
    }
}
std::vector<std::string> SavePages::description() const {
    const auto text = [&](const char* record) { return host_.text ? host_.text(id(record)) : std::string(record); };
    if (name.empty() || !WorldSave::exists(name, folder_))
        return {text("nofl")};
    if (!preview)
        return {text("invf")};
    std::vector<std::string> lines{preview->profile, preview->game_type, preview->map};
    const auto time = static_cast<std::time_t>(preview->time);
    std::tm local{};
#ifdef _WIN32
    const bool converted = localtime_s(&local, &time) == 0;
#else
    const bool converted = localtime_r(&time, &local) != nullptr;
#endif
    char stamp[32]{};
    if (converted && std::strftime(stamp, sizeof stamp, "%x  %I:%M %p", &local))
        lines.emplace_back(stamp);
    return lines;
}
std::optional<bool> SavePages::enabled(FourCC value, std::string_view command) const {
    const bool can_save = host_.can_save && host_.can_save();
    if (value == id("sgam")) {
        if (command == "sgam" || command == "name")
            return WorldSave::valid_name(name) && can_save;
        if (command == "dgam")
            return row >= 0 && WorldSave::exists(name, folder_);
        return true;
    }
    if (value == id("lgam")) {
        if (command == "lgam") // 0x4a5e70 'lgam' '1': a selected row with a valid header.
            return row >= 0 && preview.has_value();
        if (command == "dgam")
            return row >= 0 && WorldSave::exists(name, folder_);
        return true;
    }
    if (page(value))
        return true;
    return {};
}
// 0x4a5ba0 (LABEL_16): a valid name asks before overwriting, otherwise sprg saves.
void SavePages::request_save() {
    if (!WorldSave::valid_name(name) || !host_.can_save || !host_.can_save())
        return;
    if (WorldSave::exists(name, folder_)) {
        parent = id("sgam");
        show(id("sav?"));
        return;
    }
    begin_save();
}
void SavePages::begin_save() {
    parent = {};
    last_save = name;
    pending_frames_ = 2;
    show(id("sprg"));
}
void SavePages::service() {
    if (!pending_frames_ || --pending_frames_)
        return;
    try {
        host_.save(name);
    } catch (const std::exception& e) {
        if (host_.log)
            host_.log("World save failed: " + std::string(e.what()));
        parent = {};
        show(id("nohd"));
        return;
    }
    close();
}
// sub_451D10(0, 0, 1): close the menus and resume; from the title, return to it.
void SavePages::close() {
    parent = {};
    show(in_game ? FourCC{} : id("titl"));
}
void SavePages::request_load() {
    if (row < 0 || !preview)
        return;
    last_save = name;
    parent = {};
    try {
        show(id("lprg"));
        host_.load(name);
    } catch (const std::exception& e) {
        if (host_.log)
            host_.log("World load failed: " + std::string(e.what()));
        load_failed();
    }
}
void SavePages::load_failed() {
    in_game = false;
    parent = {};
    show(id("lerr"));
}
void SavePages::action(FourCC value, FourCC command) {
    const auto text = command.text();
    if (const auto allowed = enabled(value, text); allowed && !*allowed)
        return;
    if (value == id("sgam") || value == id("lgam")) {
        if (value == id("sgam") && (text == "sgam" || text == "name"))
            request_save();
        else if (value == id("lgam") && text == "lgam")
            request_load();
        else if (text == "dgam") {
            parent = value;
            show(id("del?"));
        } else if (text == "cncl") // 0x4a5e70 'cncl': the title, else back to the game.
            close();
    } else if (value == id("sav?")) {
        if (text == "ok__")
            begin_save();
        else if (text == "cncl") {
            parent = {};
            show(id("sgam"));
        }
    } else if (value == id("del?")) { // 0x4a5db0: delete, then refill the list page.
        const auto list = parent.value ? parent : id("lgam");
        if (text == "ok__") {
            WorldSave::remove(name, folder_);
            if (ascii_iequals(last_save, name))
                last_save.clear();
            name.clear();
            open(list, in_game);
        } else if (text == "cncl") {
            parent = {};
            show(list);
        }
    } else if (value == id("nohd")) {
        if (text == "ok__" || text == "hdok" || text == "cncl")
            close();
    } else if (value == id("lerr")) { // Back to the title.
        if (text == "lerr" || text == "ok__") {
            in_game = false;
            show(id("titl"));
        }
    }
}
void SavePages::quick_save() {
    if (!host_.can_save || !host_.can_save())
        return;
    in_game = true;
    name = std::string(WorldSave::quick_name);
    begin_save();
}
void SavePages::quick_load() {
    in_game = true;
    name = std::string(WorldSave::quick_name);
    row = 0;
    preview_name_.clear();
    refresh();
    request_load();
}
} // namespace sacrifice
