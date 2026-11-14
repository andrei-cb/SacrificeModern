#include "sacrifice/assets.hpp"
#include <algorithm>
#include <cmath>
#include <limits>

namespace sacrifice {
namespace {
Vec3 normalized(Vec3 n) {
    const double x = n.x, y = n.y, z = n.z;
    const double squared = (y * y + z * z) + x * x;
    if (squared > 0) {
        const auto scale = 1.0 / std::sqrt(squared);
        return {static_cast<float>(x * scale), static_cast<float>(y * scale), static_cast<float>(z * scale)};
    }
    return n;
}
bool finite(Vec3 v) {
    return std::isfinite(v.x) && std::isfinite(v.y) && std::isfinite(v.z);
}
float interpolate(float a, float b, float fraction) {
    // Original MorphVertices uses x87 intermediates and stores each result as float.
    return static_cast<float>((static_cast<double>(b) - a) * fraction + a);
}
std::size_t correct_normals(std::vector<Vertex>& vertices) {
    // MRMCLASS::CorrectNormals 0x1601c970: compare positions with squared distance <= 0.0001.
    // Preserve first-anchor order, later reassignment, and each float accumulation store.
    std::vector<std::size_t> links(vertices.size(), std::numeric_limits<std::size_t>::max());
    for (std::size_t i = 0; i < vertices.size(); ++i) {
        if (links[i] != std::numeric_limits<std::size_t>::max())
            continue;
        bool joined = false;
        for (std::size_t j = i + 1; j < vertices.size(); ++j) {
            const double x = static_cast<double>(vertices[i].position.x) - vertices[j].position.x;
            const double y = static_cast<double>(vertices[i].position.y) - vertices[j].position.y;
            const double z = static_cast<double>(vertices[i].position.z) - vertices[j].position.z;
            if ((y * y + z * z) + x * x <= 0.0001) {
                auto& normal = vertices[i].normal;
                const auto other = vertices[j].normal;
                normal.x = normal.x + other.x;
                normal.y = normal.y + other.y;
                normal.z = normal.z + other.z;
                links[j] = i;
                joined = true;
            }
        }
        if (joined)
            vertices[i].normal = normalized(vertices[i].normal);
    }
    std::size_t count = 0;
    for (std::size_t i = 0; i < vertices.size(); ++i)
        if (links[i] != std::numeric_limits<std::size_t>::max()) {
            vertices[i].normal = vertices[links[i]].normal;
            ++count;
        }
    return count;
}
} // namespace
MrmGeometry MrmGeometry::decode(ByteView data) {
    Reader reader(data, "MRMM");
    std::array<std::uint32_t, 49> h{};
    for (auto& value : h)
        value = reader.u32();
    // Runtime faces/levels use 16-bit fields; CreateNormal interprets vertex indices as signed.
    if (h[0] != data.size() || !h[1] || h[1] > 32767 || !h[2] || h[2] > h[1] || !h[3] || h[3] > 65535 ||
        !h[6] || h[6] > 1024 || h[4] > h[3] || h[5] > h[4])
        throw Error("Invalid MRMM header");
    const auto vertex_bytes = 40ull * h[1] * h[6];
    const auto face_offset = 196ull + static_cast<std::uint32_t>(h[47] - h[46]);
    const auto level_offset = 196ull + static_cast<std::uint32_t>(h[48] - h[46]);
    const auto level_count = h[1] - h[2] + 1;
    if (vertex_bytes > data.size() - 196 || face_offset < 196 + vertex_bytes || face_offset > data.size() ||
        36ull * h[3] > data.size() - face_offset || level_offset < face_offset + 36ull * h[3] ||
        level_offset > data.size() || 8ull * level_count > data.size() - level_offset)
        throw Error("MRMM vertex/face/LOD offsets outside resource");
    MrmGeometry geometry;
    const auto box = [&](unsigned at) {
        return Box3{
            {std::bit_cast<float>(h[at]), std::bit_cast<float>(h[at + 1]), std::bit_cast<float>(h[at + 2])},
            {std::bit_cast<float>(h[at + 3]), std::bit_cast<float>(h[at + 4]),
             std::bit_cast<float>(h[at + 5])}};
    };
    geometry.bounds = box(11);
    if (!finite(geometry.bounds.minimum) || !finite(geometry.bounds.maximum) || h[17] > 4)
        throw Error("Invalid MRMM model/collision bounds");
    for (unsigned i = 0; i < h[17]; ++i) {
        const auto mode = h[18 + 7 * i];
        const auto bounds = box(19 + 7 * i);
        if (mode > 2 || !finite(bounds.minimum) || !finite(bounds.maximum))
            throw Error("Invalid MRMM collision record");
        geometry.collisions.push_back({mode, bounds});
    }
    geometry.minimum_vertices = h[2];
    geometry.maximum_faces = h[4];
    geometry.minimum_faces = h[5];
    geometry.trailing_bytes = data.size() - static_cast<std::size_t>(level_offset + 8ull * level_count);
    Reader vertices(data.subspan(196, static_cast<std::size_t>(vertex_bytes)), "MRMM frames");
    geometry.frames.resize(h[6]);
    for (auto& frame : geometry.frames) {
        frame.reserve(h[1]);
        for (unsigned i = 0; i < h[1]; ++i) {
            Vertex vertex;
            const auto x = vertices.f32(), y = vertices.f32(), z = vertices.f32();
            vertex.position = {x, z, y};
            vertex.uv = {vertices.f32(), vertices.f32()};
            const auto color = vertices.u32();
            vertex.color = (color & 0xff00ff00u) | ((color & 255) << 16) | ((color >> 16) & 255);
            (void)vertices.u32();
            const auto nx = vertices.f32(), ny = vertices.f32(), nz = vertices.f32();
            vertex.normal = {nx, nz, ny};
            if (!finite(vertex.position) || !finite(vertex.normal))
                throw Error("Non-finite MRMM vertex position/normal");
            frame.push_back(vertex);
        }
        geometry.normal_links += correct_normals(frame);
        for (const auto& vertex : frame)
            if (!finite(vertex.normal))
                throw Error("MRMM normal correction overflow");
    }
    Reader faces(data.subspan(static_cast<std::size_t>(face_offset), 36ull * h[3]), "MRMM faces");
    for (unsigned i = 0; i < h[3]; ++i) {
        Face face;
        for (auto& index : face.vertices) {
            index = faces.u32();
            if (index >= h[1])
                throw Error("MRMM triangle index outside vertices");
        }
        auto flags = faces.u32();
        FourCC texture{faces.u32()};
        face.normal = {faces.f32(), faces.f32(), faces.f32()};
        face.last_vertex_count = faces.u32();
        if (face.last_vertex_count > 32767)
            throw Error("MRMM LOD threshold exceeds signed runtime field");
        if (flags & 0x2000)
            flags |= 0x12002000u;
        else if (flags & 0x1000)
            flags |= 0x02001000u;
        const Mesh::Material material{flags, texture};
        auto found = std::find(geometry.materials.begin(), geometry.materials.end(), material);
        if (found == geometry.materials.end()) {
            if (geometry.materials.size() == 4)
                throw Error("MRMM exceeds original four-material limit");
            geometry.materials.push_back(material);
            found = geometry.materials.end() - 1;
        }
        face.material = static_cast<std::uint32_t>(found - geometry.materials.begin());
        geometry.faces.push_back(face);
    }
    std::uint32_t combined = 0;
    for (const auto& material : geometry.materials)
        combined |= material.flags;
    for (auto& frame : geometry.frames) {
        if (!(combined & 0x3000))
            for (auto& vertex : frame)
                vertex.color |= 0xff000000u;
        for (const auto& face : geometry.faces)
            if (geometry.materials[face.material].flags & 0x10000)
                for (auto index : face.vertices)
                    if (!std::isfinite(frame[index].uv.x) || !std::isfinite(frame[index].uv.y))
                        throw Error("Non-finite textured MRMM UV");
    }
    Reader levels(data.subspan(static_cast<std::size_t>(level_offset), 8ull * level_count), "MRMM LOD table");
    std::vector<bool> covered(h[3]);
    std::vector<int> changes(level_count + 1);
    for (unsigned level = 0; level < level_count; ++level) {
        Level range{levels.u32(), levels.u32()};
        if (range.count > h[3] || range.first > h[3] - range.count)
            throw Error("MRMM LOD face range outside resource");
        unsigned previous = 32767;
        const auto birth = h[2] + level;
        for (unsigned i = range.first; i < range.first + range.count; ++i) {
            const auto& face = geometry.faces[i];
            if (covered[i])
                throw Error("MRMM LOD table repeats a face");
            covered[i] = true;
            if (face.last_vertex_count < birth || face.last_vertex_count > previous)
                throw Error("Invalid MRMM LOD face lifetime/order");
            previous = face.last_vertex_count;
            for (auto index : face.vertices)
                if (index >= birth)
                    throw Error("MRMM LOD face references an unavailable vertex");
            ++changes[level];
            --changes[std::min(face.last_vertex_count, h[1]) - h[2] + 1];
        }
        geometry.levels.push_back(range);
    }
    if (std::find(covered.begin(), covered.end(), false) != covered.end())
        throw Error("MRMM LOD table omits a face");
    int active = 0;
    for (unsigned i = 0; i < level_count; ++i) {
        active += changes[i];
        if (active < 0 || static_cast<unsigned>(active) > h[4] ||
            (i == 0 && active != static_cast<int>(h[5])) ||
            (i + 1 == level_count && active != static_cast<int>(h[4])))
            throw Error("MRMM LOD active face count differs from header");
    }
    return geometry;
}
std::vector<std::uint32_t> MrmGeometry::active_faces(unsigned vertices) const {
    if (frames.empty() || vertices < minimum_vertices || vertices > frames[0].size())
        throw Error("MRMM detail outside vertex range");
    std::vector<std::uint32_t> result;
    result.reserve(maximum_faces);
    // MRMINSTANCE::UpdateLod 0x1601cf30 visits birth buckets, then stops at first retired face.
    for (unsigned level = 0; level <= vertices - minimum_vertices; ++level) {
        const auto range = levels.at(level);
        for (unsigned i = range.first; i < range.first + range.count; ++i) {
            if (faces[i].last_vertex_count < vertices)
                break;
            result.push_back(i);
        }
    }
    return result;
}
Mesh MrmGeometry::sample(unsigned from, unsigned to, float fraction, unsigned vertices) const {
    if (from >= frames.size() || to >= frames.size() || !std::isfinite(fraction) || fraction < 0 ||
        fraction > 1)
        throw Error("Invalid MRMM morph frames/fraction");
    if (!vertices)
        vertices = static_cast<unsigned>(frames[0].size());
    const auto active = active_faces(vertices);
    Mesh mesh;
    mesh.materials = materials;
    mesh.vertices = frames[from];
    mesh.indices.reserve(active.size() * 3);
    mesh.triangle_materials.reserve(active.size());
    if (from != to)
        for (std::size_t i = 0; i < mesh.vertices.size(); ++i) {
            auto& v = mesh.vertices[i];
            const auto& a = frames[from][i];
            const auto& b = frames[to][i];
            v.position = {interpolate(a.position.x, b.position.x, fraction),
                          interpolate(a.position.y, b.position.y, fraction),
                          interpolate(a.position.z, b.position.z, fraction)};
            v.uv = {interpolate(a.uv.x, b.uv.x, fraction), interpolate(a.uv.y, b.uv.y, fraction)};
            v.normal = normalized({interpolate(a.normal.x, b.normal.x, fraction),
                                   interpolate(a.normal.y, b.normal.y, fraction),
                                   interpolate(a.normal.z, b.normal.z, fraction)});
            // Original MorphVertices copies color from its first input, including at fraction 1.
        }
    for (auto index : active) {
        const auto& face = faces[index];
        mesh.indices.insert(mesh.indices.end(), {face.vertices[0], face.vertices[2], face.vertices[1]});
        mesh.triangle_materials.push_back(face.material);
    }
    return mesh;
}
Mesh decode_mrm(ByteView data) {
    return MrmGeometry::decode(data).sample();
}
} // namespace sacrifice
