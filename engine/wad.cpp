#include "sacrifice/wad.hpp"
#ifdef _WIN32
#include <Windows.h>
#endif
#include "sacrifice/compat.hpp"
#include <algorithm>
#include <format>
#include <fstream>
#include <functional>
#include <limits>

namespace sacrifice {
Archive Archive::open(const std::filesystem::path& path) {
    return parse(read_file(path), utf8(path));
}
Archive Archive::parse(Bytes data, std::string label) {
    Reader probe(data, label);
    (void)probe.take(4);
    const auto offset = probe.u32();
    if (data.size() < 20 || offset > data.size())
        throw Error(label + ": invalid WAD directory offset");
    auto archive = parse_directory(ByteView(data).first(20), ByteView(data).subspan(offset), data.size(),
                                   std::move(label));
    archive.bytes_ = std::move(data);
    return archive;
}
Archive Archive::open_index(const std::filesystem::path& path) {
    auto size = std::filesystem::file_size(path);
    if (size < 20 || size > 512ull * 1024 * 1024)
        throw Error("Invalid archive size: " + utf8(path));
    std::ifstream in(path, std::ios::binary);
    Bytes header(20);
    if (!in.read(reinterpret_cast<char*>(header.data()), 20))
        throw Error("Cannot read archive header: " + utf8(path));
    Reader r(header, utf8(path));
    (void)r.take(4);
    const auto offset = r.u32();
    if (offset < 20 || offset >= size || size - offset > 64 * 1024 * 1024)
        throw Error("Invalid archive directory offset: " + utf8(path));
    Bytes directory(static_cast<std::size_t>(size - offset));
    in.seekg(offset);
    if (!in.read(reinterpret_cast<char*>(directory.data()), directory.size()))
        throw Error("Cannot read archive directory: " + utf8(path));
    auto result = parse_directory(header, directory, static_cast<std::size_t>(size), utf8(path));
    result.backing_file_ = std::filesystem::absolute(path);
    return result;
}
Archive Archive::parse_directory(ByteView header_data, ByteView compressed, std::size_t file_size,
                                 std::string label) {
    Archive archive;
    archive.file_size_ = file_size;
    archive.label_ = std::move(label);
    Reader header(header_data, archive.label_);
    if (header.fixed_string(4) != "WAD>")
        throw Error(archive.label_ + ": expected WAD> signature");
    const auto offset = header.u32(), size = header.u32(), reserved = header.u32(), version = header.u32();
    if (reserved || version > 1)
        throw Error(archive.label_ + ": unsupported WAD header/version");
    if (offset < 20 || offset >= file_size || compressed.size() != file_size - offset || size == 0 ||
        size % 28 || size > 64 * 1024 * 1024)
        throw Error(archive.label_ + ": invalid directory bounds");
    const auto directory = inflate_exact(compressed, size, archive.label_ + " directory");
    Reader reader(directory, archive.label_ + " directory");
    std::size_t payload_offset = 20;
    std::function<void(std::size_t, unsigned)> visit = [&](std::size_t parent, unsigned depth) {
        if (depth > 256)
            throw Error(archive.label_ + ": excessive directory depth");
        ResourceRecord r;
        r.name = {reader.u32()};
        r.tag = {reader.u32()};
        r.stored_size = reader.u32();
        r.size = reader.u32();
        r.children = reader.u32();
        r.flags = reader.u32();
        r.timestamp = reader.u32();
        r.offset = payload_offset;
        r.parent = parent;
        // Flags 4 and 16 also occur in shipped directories; preserve them as metadata.
        if (r.flags & ~0x17u)
            throw Error(archive.label_ + ": unknown resource flags");
        if (r.children && !r.folder())
            throw Error(archive.label_ + ": children on a non-folder resource");
        if (r.children > reader.remaining() / 28)
            throw Error(archive.label_ + ": invalid child count");
        r.path = (parent == std::numeric_limits<std::size_t>::max() ? std::string{}
                                                                    : archive.records_[parent].path) +
                 "/" + r.tag.text() + ":" + r.name.text();
        const auto index = archive.records_.size();
        archive.records_.push_back(r);
        if (r.children) {
            for (std::uint32_t i = 0; i < r.children; ++i)
                visit(index, depth + 1);
        } else {
            if (r.stored_size > offset - payload_offset)
                throw Error(archive.label_ + r.path + ": resource crosses directory boundary");
            if (!r.folder() && !r.compressed() && r.stored_size != r.size)
                throw Error(archive.label_ + r.path + ": raw length mismatch");
            payload_offset += r.stored_size;
        }
        archive.records_[index].subtree_end = archive.records_.size();
    };
    visit(std::numeric_limits<std::size_t>::max(), 0);
    if (reader.remaining() || payload_offset != offset)
        throw Error(archive.label_ + ": directory/payload traversal mismatch");
    return archive;
}
std::size_t Archive::resource_count() const {
    return std::count_if(records_.begin(), records_.end(), [](const auto& r) { return !r.folder(); });
}
Bytes Archive::read(std::size_t index) const {
    if (index >= records_.size())
        throw Error(label_ + ": invalid resource index");
    const auto& r = records_[index];
    if (r.folder())
        throw Error(label_ + r.path + ": cannot read a folder");
    Bytes backing;
    if (!backing_file_.empty()) {
        std::ifstream in(backing_file_, std::ios::binary);
        in.seekg(r.offset);
        backing.resize(r.stored_size);
        if (!in || (!backing.empty() && !in.read(reinterpret_cast<char*>(backing.data()), backing.size())))
            throw Error(label_ + r.path + ": cannot read resource payload");
    }
    auto payload =
        backing_file_.empty() ? ByteView(bytes_).subspan(r.offset, r.stored_size) : ByteView(backing);
    if (r.compressed())
        return inflate_exact(payload, r.size, label_ + r.path);
    return {payload.begin(), payload.end()};
}
std::optional<std::size_t> Archive::find(FourCC tag, std::optional<FourCC> name) const {
    for (std::size_t i = 0; i < records_.size(); ++i)
        if (!records_[i].folder() && records_[i].tag == tag && (!name || records_[i].name == *name))
            return i;
    return {};
}
std::optional<std::size_t> Archive::find_path(std::string_view path) const {
    for (std::size_t i = 0; i < records_.size(); ++i)
        if (records_[i].path == path)
            return i;
    return {};
}
void Archive::validate_payloads() const {
    for (std::size_t i = 0; i < records_.size(); ++i)
        if (!records_[i].folder())
            (void)read(i);
}
std::optional<std::size_t> Archive::find_chunk(FourCC tag, std::optional<FourCC> name,
                                               std::optional<std::size_t> scope) const {
    std::size_t begin = 0, end = records_.size();
    if (scope) {
        if (*scope >= records_.size())
            throw Error(label_ + ": invalid lookup scope");
        begin = *scope + 1;
        end = records_[*scope].subtree_end;
    }
    for (auto i = begin; i < end;) {
        const auto& r = records_[i];
        if (r.tag == tag && (!name || r.name == *name))
            return i;
        i = (r.flags & 4) ? r.subtree_end : i + 1;
    }
    return {};
}
std::vector<std::filesystem::path> container_paths(const std::filesystem::path& root) {
    std::vector<std::filesystem::path> result;
    for (const auto& [dir, extension] : {std::pair{"data", ".wad"}, std::pair{"maps", ".scp"}}) {
        for (const auto& entry : std::filesystem::recursive_directory_iterator(root / dir)) {
            if (!entry.is_regular_file())
                continue;
            auto ext = entry.path().extension().string();
            std::transform(ext.begin(), ext.end(), ext.begin(),
                           [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
            if (ext == extension)
                result.push_back(entry.path());
        }
    }
    std::sort(result.begin(), result.end());
    return result;
}
std::size_t ResourceCatalog::mount(const std::filesystem::path& path) {
    auto absolute = std::filesystem::weakly_canonical(path.is_absolute() ? path : game_dir_ / path);
    for (std::size_t i = 0; i < archives_.size(); ++i)
#ifdef _WIN32
        if (CompareStringOrdinal(widen(archives_[i].label()).c_str(), -1, absolute.c_str(), -1, TRUE) ==
            CSTR_EQUAL)
#else
        if (ascii_iequals(archives_[i].label(), utf8(absolute)))
#endif
            return i;
    archives_.push_back(Archive::open_index(absolute));
    return archives_.size() - 1;
}
void ResourceCatalog::mount_game() {
    auto enumerate = [&](const std::filesystem::path& directory, const wchar_t* wildcard) {
#ifdef _WIN32
        WIN32_FIND_DATAW item{};
        const auto handle = FindFirstFileW((directory / wildcard).c_str(), &item);
        if (handle == INVALID_HANDLE_VALUE) {
            if (GetLastError() == ERROR_FILE_NOT_FOUND)
                return;
            throw Error("Cannot enumerate resource packs in " + utf8(directory));
        }
        struct FindHandle {
            HANDLE value;
            ~FindHandle() {
                FindClose(value);
            }
        } guard{handle};
        do {
            if (!(item.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY))
                mount(directory / item.cFileName);
        } while (FindNextFileW(handle, &item));
        if (GetLastError() != ERROR_NO_MORE_FILES)
            throw Error("Resource enumeration failed in " + utf8(directory));
#else
        std::vector<std::filesystem::path> paths;
        const bool language = std::wstring_view(wildcard) == L"lang_*.wad";
        for (const auto& entry : std::filesystem::directory_iterator(directory)) {
            const auto name = utf8(entry.path().filename());
            if (entry.is_regular_file() && ascii_iequals(utf8(entry.path().extension()), ".wad") &&
                (!language || (name.size() >= 5 && ascii_iequals(std::string_view(name).substr(0, 5), "lang_"))))
                paths.push_back(entry.path());
        }
        const auto folded = [](const auto& path) {
            auto name = utf8(path.filename());
            for (auto& c : name)
                if (c >= 'A' && c <= 'Z') c += 'a' - 'A';
            return name;
        };
        std::sort(paths.begin(), paths.end(), [&](const auto& a, const auto& b) {
            const auto x = folded(a), y = folded(b);
            return x == y ? utf8(a) < utf8(b) : x < y;
        });
        for (const auto& path : paths)
            mount(path);
#endif
    };
    // Original initialization: language files, root packs, remaining locale packs.
    enumerate(game_dir_ / "data/local", L"lang_*.wad");
    enumerate(game_dir_ / "data", L"*.wad");
    enumerate(game_dir_ / "data/local", L"*.wad");
}
std::optional<ResourceLocation> ResourceCatalog::find(FourCC tag, FourCC name) const {
    return find_chunk(tag, name);
}
std::optional<ResourceLocation> ResourceCatalog::find_chunk(FourCC tag, FourCC name,
                                                            std::optional<ResourceLocation> scope) const {
    if (scope) {
        auto r = archive(*scope).find_chunk(tag, name, scope->record);
        if (r)
            return ResourceLocation{scope->archive, *r};
        return {};
    }
    for (std::size_t i = 0; i < archives_.size(); ++i)
        if (auto r = archives_[i].find_chunk(tag, name))
            return ResourceLocation{i, *r};
    return {};
}
const Archive& ResourceCatalog::archive(ResourceLocation l) const {
    if (l.archive >= archives_.size() || l.record >= archives_[l.archive].records().size())
        throw Error("Invalid resource handle");
    return archives_[l.archive];
}
Bytes ResourceCatalog::read(ResourceLocation l) const {
    return archive(l).read(l.record);
}
} // namespace sacrifice
