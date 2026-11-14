#include "sacrifice/storage.hpp"
#ifdef _WIN32
#include <Windows.h>
#else
#include <fstream>
#include <unistd.h>
#ifdef __EMSCRIPTEN__
#include <emscripten.h>
#endif
#endif
#include <atomic>
#include <limits>

namespace sacrifice {
std::optional<nlohmann::json> DocumentStore::read() const {
    if (!std::filesystem::exists(path_))
        return {};
    const auto bytes = read_file(path_, 128 * 1024 * 1024);
    auto document = nlohmann::json::parse(reinterpret_cast<const char*>(bytes.data()),
                                          reinterpret_cast<const char*>(bytes.data() + bytes.size()));
    if (!document.is_object() || document.value("format", std::string{}) != kind_ ||
        !document.contains("version") || document.at("version") != version_ || !document.contains("payload"))
        throw Error("Unsupported document format/version: " + utf8(path_));
    return document.at("payload");
}
void DocumentStore::write(const nlohmann::json& payload) const {
    const auto text =
        nlohmann::json({{"format", kind_}, {"version", version_}, {"payload", payload}}).dump(2) + "\n";
    if (text.size() > 128ull * 1024 * 1024)
        throw Error("Document exceeds storage limit");
    auto parent = path_.parent_path();
    if (!parent.empty())
        std::filesystem::create_directories(parent);
    static std::atomic<unsigned> sequence{};
    auto temp = path_;
#ifdef _WIN32
    temp += L".tmp." + std::to_wstring(GetCurrentProcessId()) + L"." + std::to_wstring(sequence++);
    struct Pending {
        std::filesystem::path path;
        HANDLE handle{INVALID_HANDLE_VALUE};
        bool created{}, committed{};
        ~Pending() {
            if (handle != INVALID_HANDLE_VALUE)
                CloseHandle(handle);
            if (created && !committed)
                DeleteFileW(path.c_str());
        }
    } pending{temp};
    pending.handle =
        CreateFileW(temp.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_NEW, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (pending.handle == INVALID_HANDLE_VALUE)
        throw Error("Cannot create temporary document: " + utf8(temp));
    pending.created = true;
    DWORD written{};
    if (!WriteFile(pending.handle, text.data(), static_cast<DWORD>(text.size()), &written, nullptr) ||
        written != text.size() || !FlushFileBuffers(pending.handle))
        throw Error("Cannot flush document: " + utf8(temp));
    CloseHandle(pending.handle);
    pending.handle = INVALID_HANDLE_VALUE;
    if (!MoveFileExW(temp.c_str(), path_.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH))
        throw Error("Cannot commit document: " + utf8(path_));
    pending.committed = true;
#else
    temp += ".tmp." + std::to_string(getpid()) + "." + std::to_string(sequence++);
    struct Pending {
        std::filesystem::path path;
        ~Pending() {
            std::error_code ignored;
            std::filesystem::remove(path, ignored);
        }
    } pending{temp};
    {
        std::ofstream out(temp, std::ios::binary | std::ios::trunc);
        out.write(text.data(), static_cast<std::streamsize>(text.size()));
        out.close();
        if (!out)
            throw Error("Cannot flush document: " + utf8(temp));
    }
    std::filesystem::rename(temp, path_);
#ifdef __EMSCRIPTEN__
    // The browser loader mounts /persist as IDBFS. Node checks use NODEFS instead.
    EM_ASM({
        if (typeof IDBFS === 'undefined' || !FS.analyzePath('/persist').exists) return;
        if (FS.lookupPath('/persist').node.mount.type !== IDBFS) return;
        var state = Module['saveSync'] || (Module['saveSync'] = {busy: false, again: false});
        state.again = true;
        if (state.busy) return;
        function flush() {
            state.busy = true;
            state.again = false;
            FS.syncfs(false, function(error) {
                state.busy = false;
                if (error) console.error('Save persistence failed', error);
                if (state.again) flush();
            });
        }
        flush();
    });
#endif
#endif
}
} // namespace sacrifice
