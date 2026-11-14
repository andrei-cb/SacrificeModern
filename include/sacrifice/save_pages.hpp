#pragma once
#include "world_save.hpp"
#include <functional>

namespace sacrifice {
// The saved-game pages FORM sgam / lgam and their dialogs sav?, del?, nohd, lerr and progress pages
// sprg / lprg (0x4a5830 list, 0x4a5ba0 sgam, 0x4a5d40 sav?, 0x4a5db0 del?, 0x4a5e70 lgam). Both front
// ends drive this one state machine; they only supply how a page is shown and how a world is saved
// or loaded.
class SavePages {
  public:
    struct Host {
        std::filesystem::path root;
        std::function<bool()> can_save;
        // Writes the named save; an exception opens nohd.
        std::function<void(const std::string& name)> save;
        // Starts loading the named save; an exception opens lerr.
        std::function<void(const std::string& name)> load;
        // Shows a page; {} closes the save pages and resumes the game (in game only).
        std::function<void(FourCC page, bool in_game)> show;
        std::function<std::string(FourCC record)> text;
        std::function<void(const std::string& message)> log;
    };
    explicit SavePages(Host host, std::filesystem::path folder = WorldSave::directory());
    static bool page(FourCC id);

    // 0x4b9590 + the list's '1' event: fill from saves/games and select the last saved name.
    void open(FourCC list_page, bool in_game);
    void select(int row);
    void rename(std::string typed); // The sgam edit box.
    // Whether a command on a save page is available; empty for other pages.
    std::optional<bool> enabled(FourCC page, std::string_view command) const;
    void action(FourCC page, FourCC command);
    void quick_save(); // F1 (0x4a56a0): "Quicksave" through sprg.
    void quick_load(); // F2 (0x4a5710): load it when it is valid.
    void load_failed(); // A load that started but did not finish: lerr (0x4a5e70).
    void service();     // sprg's 'prog' event a frame after the page shows: write the file.
    // 0x4a5830 'desc': profile, game type, map, then the time as "%x  %I:%M %p".
    std::vector<std::string> description() const;

    std::vector<std::string> names;
    int row{-1}, scroll{}, description_scroll{};
    std::string name;      // Destination (0x4f3d24): the selected or typed name.
    std::string last_save; // byte_4F3D34: the last saved name, selected when a list opens.
    FourCC parent;         // The list page under a sav? / del? dialog.
    bool in_game{};        // Opened from the game menu (or F1/F2) rather than the title.
    std::optional<WorldSave::Header> preview; // Empty when the selected file is not a loadable save.
    std::optional<Image> thumbnail;
    unsigned thumbnail_revision{}; // Changes whenever `thumbnail` does, so hosts re-upload it.

  private:
    Host host_;
    std::filesystem::path folder_;
    std::string preview_name_;
    int pending_frames_{};
    void show(FourCC page);
    void refresh();
    void request_save();
    void begin_save();
    void request_load();
    void close();
};
} // namespace sacrifice
