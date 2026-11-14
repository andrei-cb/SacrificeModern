#include "sacrifice/saxs.hpp"
#include <algorithm>
#include <cmath>
#include <format>
#include <map>

namespace sacrifice {
namespace {
// Quantization constants from game3d.dll .data1 (original VAs in comments).
constexpr float root_unit = 0.1f;                      // 0x161293c8..d0
constexpr float rotation_unit = 6.103515625e-05f;      // 0x161293d4..e0
constexpr float weight_unit = 6.103515625e-05f;        // 0x16129388
constexpr float normal_unit = 3.0518509447574615e-05f; // 0x1612938c..94
constexpr float ring_v_unit = 6.103515625e-05f;        // 0x16129398
constexpr float texel_unit = 0.00390625f;              // 0x16129384, 0x161293a8, 0x161293bc, 0x16056d80

void need(bool condition, std::string_view message) {
    if (!condition)
        throw Error(std::string(message));
}
std::uint8_t byte_at(ByteView data, std::size_t offset) {
    need(offset < data.size(), "SXMD reference outside chunk");
    return std::to_integer<std::uint8_t>(data[offset]);
}
std::uint16_t u16_at(ByteView data, std::size_t offset) {
    need(offset <= data.size() && data.size() - offset >= 2, "SXMD reference outside chunk");
    return static_cast<std::uint16_t>(byte_at(data, offset) | (byte_at(data, offset + 1) << 8));
}
std::int16_t i16_at(ByteView data, std::size_t offset) {
    return std::bit_cast<std::int16_t>(u16_at(data, offset));
}
std::uint32_t u32_at(ByteView data, std::size_t offset) {
    return u16_at(data, offset) | (static_cast<std::uint32_t>(u16_at(data, offset + 2)) << 16);
}
float f32_at(ByteView data, std::size_t offset) {
    return std::bit_cast<float>(u32_at(data, offset));
}
ByteView span_at(ByteView data, std::size_t offset, std::size_t count, std::string_view what) {
    if (offset > data.size() || count > data.size() - offset)
        throw Error(std::format("{} outside chunk", what));
    return data.subspan(offset, count);
}
bool finite(float value) {
    return std::isfinite(value);
}
// The legacy double-to-integer helper truncates toward zero; callers keep the low byte.
std::uint8_t truncate_byte(double value) {
    need(std::isfinite(value) && std::abs(value) < 2147483648.0, "SXMD texture coordinate overflow");
    return static_cast<std::uint8_t>(static_cast<std::int32_t>(value) & 255);
}
std::string c_string(ByteView data) {
    std::string text;
    for (auto value : data) {
        if (value == std::byte{})
            break;
        text.push_back(static_cast<char>(value));
    }
    return text;
}
SaxsMatrix multiply(const SaxsMatrix& a, const SaxsMatrix& b) {
    // 0x16030320: out = a * b for row vectors.
    SaxsMatrix r{};
    for (int row = 0; row < 3; ++row)
        for (int column = 0; column < 3; ++column)
            r[row * 3 + column] =
                a[row * 3 + 2] * b[6 + column] + a[row * 3 + 1] * b[3 + column] + a[row * 3] * b[column];
    for (int column = 0; column < 3; ++column)
        r[9 + column] = a[11] * b[6 + column] + a[10] * b[3 + column] + a[9] * b[column] + b[9 + column];
    return r;
}
SaxsMatrix rotation(const std::array<float, 4>& q) {
    // 0x16030770: quaternion (x,y,z,w) to the SXCore row-vector rotation.
    const double s = 2.0 / (q[3] * q[3] + q[2] * q[2] + q[0] * q[0] + q[1] * q[1]);
    const double x = q[0], y = q[1], z = q[2], w = q[3];
    SaxsMatrix m{};
    m[0] = static_cast<float>(1.0 - s * (z * z + y * y));
    m[1] = static_cast<float>(s * (x * y) - s * (z * w));
    m[2] = static_cast<float>(s * (y * w) + s * (x * z));
    m[3] = static_cast<float>(s * (x * y) + s * (z * w));
    m[4] = static_cast<float>(1.0 - s * (z * z + x * x));
    m[5] = static_cast<float>(s * (y * z) - s * (x * w));
    m[6] = static_cast<float>(s * (x * z) - s * (y * w));
    m[7] = static_cast<float>(s * (x * w) + s * (y * z));
    m[8] = static_cast<float>(1.0 - s * (y * y + x * x));
    return m;
}

// Mesh generation for actor flags 0x400 (the UpdateLOD vertex path). Original globals:
// base = 0x1610c244 (first render vertex of the current ring), vertices = 0x1610c24c,
// triangles = 0x1610c250 (16-bit indices). The original works on the relocated chunk in memory:
// ring byte 1 bit 2 marks visited rings and point flag 0x100 marks emitted points. Patch seams can
// address records past a ring's end, so the builder reads the loader-transformed chunk image.
class Builder {
  public:
    Builder(const SaxsModel& model, std::uint32_t flags, std::int32_t point_step, std::int32_t ring_step)
        : m_(model), mem_(model.image), flags_(flags), point_step_(point_step), ring_step_(ring_step) {
        need(point_step > 0 && ring_step > 0, "SAXS detail step must be positive");
    }
    SaxsTopology build() {
        for (unsigned p = 0; p < m_.parts.size(); ++p)
            build_part(p);
        need(out_.vertices.size() <= 0x10000, "SAXS mesh exceeds 16-bit vertex indices");
        for (const auto& triangle : out_.triangles)
            for (auto index : triangle.vertices)
                need(index < out_.vertices.size(), "SAXS triangle references an unbuilt vertex");
        return std::move(out_);
    }

  private:
    const SaxsModel& m_;
    Bytes mem_;
    std::uint32_t flags_;
    std::int32_t point_step_, ring_step_;
    SaxsTopology out_;
    std::int64_t base_{};
    std::size_t guard_{};

    void tick() {
        need(++guard_ < 50'000'000, "SAXS mesh generation did not terminate");
    }
    std::uint8_t byte(std::size_t offset) const {
        need(offset < mem_.size(), "SAXS mesh generation read outside SXMD");
        return std::to_integer<std::uint8_t>(mem_[offset]);
    }
    std::uint16_t word(std::size_t offset) const {
        return static_cast<std::uint16_t>(byte(offset) | (byte(offset + 1) << 8));
    }
    void set_byte(std::size_t offset, std::uint8_t value) {
        need(offset < mem_.size(), "SAXS mesh generation wrote outside SXMD");
        mem_[offset] = std::byte{value};
    }
    void set_word(std::size_t offset, std::uint16_t value) {
        set_byte(offset, static_cast<std::uint8_t>(value));
        set_byte(offset + 1, static_cast<std::uint8_t>(value >> 8));
    }
    std::size_t point(unsigned p, unsigned r, std::size_t i) const {
        return m_.parts[p].rings.at(r).points_offset + i * 10;
    }
    std::uint16_t point_flags(std::size_t record) const {
        return word(record + 8);
    }
    void mark(std::size_t record, bool included) {
        const auto flags = point_flags(record);
        set_word(record + 8, included ? static_cast<std::uint16_t>(flags | 0x100) : flags & 0xfeff);
    }
    std::size_t ring_flags(unsigned p, unsigned r) const {
        return m_.parts[p].rings.at(r).offset + 1;
    }
    std::uint32_t index(std::int64_t value) const {
        return static_cast<std::uint32_t>(value & 0xffff);
    }
    void triangle(std::int64_t a, std::int64_t b, std::int64_t c, unsigned part) {
        out_.triangles.push_back({{index(a), index(b), index(c)}, part});
    }
    void emit(unsigned part, std::size_t record) {
        SaxsTopology::Vertex vertex;
        for (unsigned s = 0; s < 3; ++s)
            vertex.sources[s] = std::bit_cast<std::int16_t>(word(record + s * 2));
        vertex.u = byte(record + 6) * texel_unit;
        vertex.v = byte(record + 7) * texel_unit;
        vertex.part = part;
        out_.vertices.push_back(vertex);
    }
    // 0x16033e60.
    std::int32_t next_ring(unsigned p, std::int32_t position, std::int32_t limit) {
        if (flags_ & 0x100)
            return ring_step_ + position;
        auto next = ring_step_ + position;
        auto end = next >> 16;
        for (auto r = (position >> 16) + 1; r < end; ++r) {
            if (r >= static_cast<std::int16_t>(limit >> 16))
                break;
            const auto flags = ring_flags(p, static_cast<unsigned>(r));
            if (byte(flags) & 1) {
                next = r << 16;
                end = r;
            } else
                set_byte(flags, byte(flags) & 0xfd);
        }
        return next;
    }
    // 0x16033ee0: join the previous ring to the current ring, advancing by texture U.
    void stitch(unsigned p, unsigned current, unsigned previous, std::int32_t current_count_fx,
                std::int32_t previous_count, std::int32_t previous_emitted) {
        const auto current_count = static_cast<std::int16_t>(current_count_fx >> 16);
        std::int32_t prev_i = 0, cur_i = 0, prev_n = 0, cur_n = 0;
        std::int64_t prev_v = -1, cur_v = -1, last = -1;
        bool prev_wrapped = false, cur_wrapped = false, current_side = false;
        std::int32_t state = 0;
        while (true) {
            tick();
            if ((state >> 24) != 0 && cur_wrapped)
                break;
            if (prev_v < 0) {
                current_side = false;
                while (!(point_flags(point(p, previous, prev_i)) & 0x100)) {
                    tick();
                    if (++prev_i >= previous_count) {
                        prev_i = 0;
                        prev_wrapped = true;
                    }
                }
                prev_v = prev_n++ + base_;
            }
            if (cur_v < 0) {
                current_side = true;
                while (!(point_flags(point(p, current, cur_i)) & 0x100)) {
                    tick();
                    if (++cur_i >= current_count) {
                        cur_i = 0;
                        cur_wrapped = true;
                    }
                }
                cur_v = base_ + previous_emitted + cur_n++;
            }
            if (last >= 0) {
                if (current_side)
                    triangle(last, prev_v, cur_v, p);
                else
                    triangle(cur_v, last, prev_v, p);
            }
            const auto prev_record = point(p, previous, prev_i), cur_record = point(p, current, cur_i);
            bool take_previous =
                (byte(prev_record + 6) < byte(cur_record + 6) || cur_wrapped) && !prev_wrapped;
            // The current-ring mark is evaluated only when the previous point is unmarked. The
            // look-ahead reads the following serialized record, even after a ring's last point.
            if (point_flags(prev_record) & 0x60) {
                if (take_previous && (point_flags(cur_record + 10) & 0x60))
                    take_previous = false;
            } else if ((point_flags(cur_record) & 0x60) && !take_previous)
                take_previous = (point_flags(prev_record + 10) & 0x60) != 0;
            if (take_previous) {
                last = prev_v;
                ++prev_i;
                prev_v = -1;
                if (prev_i < previous_count)
                    state = prev_wrapped ? 0x1000000 : 0;
                else {
                    if (cur_wrapped)
                        triangle(cur_v, last, base_ + prev_n, p);
                    else {
                        prev_v = base_ + prev_n;
                        current_side = !current_side;
                    }
                    prev_i = 0;
                    prev_n = 0;
                    prev_wrapped = true;
                    state = 0x1000000;
                }
            } else {
                ++cur_i;
                last = cur_v;
                cur_v = -1;
                state = prev_wrapped ? 0x1000000 : 0;
                if (cur_i >= current_count) {
                    if (prev_wrapped)
                        triangle(last, prev_v, base_ + previous_emitted + cur_n, p);
                    else {
                        cur_v = base_ + previous_emitted + cur_n;
                        current_side = !current_side;
                    }
                    cur_i = 0;
                    cur_n = 0;
                    cur_wrapped = true;
                }
            }
        }
        base_ += previous_emitted;
    }
    // 0x16034640: seam between a limb ring and its target part. With part flag 0x80 every self
    // link uses one fixed ring, while the point index still comes from the link.
    void patch(unsigned p, std::int32_t start_fx, std::int32_t last_ring) {
        const auto& part = m_.parts[p];
        const auto& data = *part.patch;
        const auto& links = data.links;
        const std::size_t end = links.size();
        const unsigned target = data.target;
        std::size_t self = 0, other = 0;
        bool result = false, has_previous = false, self_pending = false, other_pending = false;
        bool fixed_ring = false, self_wrapped = false, other_wrapped = false, check = false;
        std::int32_t ring_override = 0;
        std::int64_t self_vertex = 0, other_vertex = 0, previous_vertex = 0;
        auto wrap = [&](std::size_t& link, bool& wrapped) {
            if (++link == end) {
                wrapped = true;
                link = 0;
                if (result)
                    return true;
                result = true;
            }
            return false;
        };
        if (part.flags & 0x80) {
            fixed_ring = true;
            ring_override = (part.flags & 0x100) ? (start_fx >> 16) : last_ring;
            if (ring_override >= static_cast<std::int32_t>(part.rings.size()))
                return;
        }
    label7:
        if (check && other_wrapped)
            return;
    label9:
        result = false;
        if (!self_pending) {
            auto wrapped = self_wrapped;
            auto link = self;
            while (true) {
                tick();
                while (links[link].part != p)
                    if (wrap(link, wrapped))
                        return;
                const unsigned r = fixed_ring ? static_cast<unsigned>(ring_override) : links[link].ring;
                if (!(byte(ring_flags(p, r)) & 2)) {
                    if (wrap(link, wrapped))
                        return;
                    continue;
                }
                const auto record = point(p, r, links[link].point);
                self_pending = true;
                if (point_flags(record) & 0x100) {
                    self_vertex = base_;
                    emit(p, record);
                    if (wrapped)
                        out_.vertices.back().u = data.v * texel_unit;
                    ++base_;
                    self = link;
                    self_wrapped = wrapped;
                    break;
                }
                if (wrap(link, wrapped))
                    return;
                self_pending = false;
            }
        }
        result = false;
        if (!other_pending) {
            auto wrapped = other_wrapped;
            auto link = other;
            while (true) {
                tick();
                while (links[link].part != target)
                    if (wrap(link, wrapped))
                        return;
                const auto r = links[link].ring;
                if (byte(ring_flags(target, r)) & 2) {
                    const auto record = point(target, r, links[link].point);
                    other_pending = true;
                    if (!(point_flags(record) & 0x100)) {
                        if (wrap(link, wrapped))
                            return;
                        other_pending = false;
                        continue;
                    }
                    other_vertex = base_;
                    emit(p, record);
                    auto u = wrapped ? data.v : links[link].u;
                    auto v = links[link].v;
                    if (fixed_ring) {
                        const auto& own = links[self];
                        v = byte(point(p, own.ring, own.point) + 7);
                        self_pending = true;
                    }
                    out_.vertices.back().u = u * texel_unit;
                    out_.vertices.back().v = v * texel_unit;
                    ++base_;
                } else if (wrap(link, wrapped))
                    return;
                if (other_pending) {
                    other = link;
                    other_wrapped = wrapped;
                    break;
                }
            }
        }
        if (has_previous) {
            triangle(self_vertex, previous_vertex, other_vertex, p);
            if (self_wrapped && other_wrapped)
                return;
        }
        if ((self >= other || self_wrapped) && !other_wrapped) {
            previous_vertex = other_vertex;
            ++other;
            has_previous = other_pending;
            other_pending = false;
            if (other == end) {
                other_wrapped = true;
                other = 0;
                goto label9;
            }
            check = self_wrapped;
            goto label7;
        }
        previous_vertex = self_vertex;
        ++self;
        has_previous = self_pending;
        self_pending = false;
        if (self == end) {
            self_wrapped = true;
            self = 0;
        }
        check = self_wrapped;
        if (!self_wrapped)
            goto label7;
        goto label9;
    }
    void build_part(unsigned p) {
        const auto& part = m_.parts[p];
        const auto rings = static_cast<std::int32_t>(part.rings.size());
        const auto all_rings = rings << 16;
        auto limit = all_rings;
        std::int32_t start = 0;
        if ((part.flags & 0x80) && (flags_ & 0x1400) == 0x1400) {
            if (part.flags & 0x100)
                start = next_ring(p, 0, all_rings);
            else if (part.flags & 0x200) {
                limit = all_rings - ring_step_;
                if (limit < 0x20000)
                    limit = all_rings;
            }
        }
        std::int32_t position = start, processed = 0, ring_index = 0;
        std::int32_t previous_count = 0, previous_emitted = 0, emitted = 0;
        unsigned previous = 0;
        while (true) {
            tick();
            auto current = position;
            if (position >= limit) {
                if (limit != all_rings)
                    break;
                const auto last = rings - 1;
                if (((position - ring_step_) >> 16) == last)
                    break;
                current = last << 16;
                position = current;
            }
            const auto r = static_cast<unsigned>(current >> 16);
            need(r < part.rings.size(), "SAXS ring position outside part");
            ring_index = static_cast<std::int32_t>(r);
            const auto& ring = part.rings[r];
            set_byte(ring.offset + 1, byte(ring.offset + 1) | 2);
            const auto count = static_cast<std::int32_t>(part.point_count(ring));
            const auto count_fx = count << 16;
            auto step = part.faces.empty() ? point_step_ : 0x10000;
            if (count_fx / step < 3)
                step = count_fx / 3 + 1;
            emitted = 0;
            const auto ring_start = out_.vertices.size();
            for (std::int32_t at = 0; at < count_fx;) {
                tick();
                const auto i = at >> 16;
                mark(point(p, r, i), true);
                emit(p, point(p, r, i));
                auto skipped = i + 1;
                auto skipped_fx = at + 0x10000;
                at += step;
                for (; skipped < (at >> 16) && skipped_fx < count_fx; ++skipped, skipped_fx += 0x10000) {
                    const auto record = point(p, r, skipped);
                    if (point_flags(record) & 0x80) {
                        mark(record, true);
                        emit(p, record);
                        ++emitted;
                    } else
                        mark(record, false);
                }
                ++emitted;
            }
            if (position == 0 && (part.flags & 1) && (flags_ & 0x402) && (flags_ & 0x800))
                for (auto k = 0; k < emitted - 2; ++k)
                    triangle(base_, base_ + 2 + k, base_ + 1 + k, p);
            // Close the ring with a copy of its first vertex at U = ring V (256 after UV remapping).
            need(ring_start < out_.vertices.size(), "SAXS ring emitted no vertices");
            auto seam = out_.vertices[ring_start];
            seam.u = static_cast<float>(word(ring.offset + 2)) * texel_unit;
            out_.vertices.push_back(seam);
            ++emitted;
            if (processed != 0)
                stitch(p, r, previous, count_fx, previous_count, previous_emitted);
            if (!part.faces.empty()) {
                for (const auto& face : part.faces)
                    triangle(face[0] + base_, face[2] + base_, face[1] + base_, p);
                // Literal legacy increment; it vanishes in the 16-bit triangle indices.
                base_ += count_fx;
            }
            position = next_ring(p, position, limit);
            ++processed;
            previous_count = count;
            previous = r;
            previous_emitted = emitted;
        }
        if ((part.flags & 2) && (flags_ & 0x402) && (flags_ & 0x800))
            for (auto k = 0; k < emitted - 2; ++k)
                triangle(base_, base_ + 1 + k, base_ + 2 + k, p);
        base_ += emitted;
        if ((flags_ & 0x1000) && part.patch && (flags_ & 0x400))
            patch(p, start, ring_index);
    }
};
} // namespace

SaxsDescriptor SaxsDescriptor::decode(ByteView data) {
    // Callback 0x408f20 requests 480 bytes; stock chunks contain 268-277 bytes, ending in the
    // source path at offset 224.
    need(data.size() >= 224, "SAXS descriptor is truncated");
    SaxsDescriptor d;
    d.scale = f32_at(data, 0);
    d.root_adjust = f32_at(data, 4) * d.scale;
    d.quality = {f32_at(data, 8), f32_at(data, 12)};
    for (unsigned i = 0; i < d.references.size(); ++i) {
        const auto at = 24 + 16 * i;
        d.references[i] = {i16_at(data, at),
                           u16_at(data, at + 2),
                           {f32_at(data, at + 4), f32_at(data, at + 8), f32_at(data, at + 12)}};
    }
    need(finite(d.scale) && d.scale > 0 && finite(d.root_adjust), "Invalid SAXS scale");
    need(finite(d.quality[0]) && finite(d.quality[1]) && d.quality[0] > 0 && d.quality[1] > 0,
         "Invalid SAXS quality");
    for (unsigned i = 0; i < d.limbs.size(); ++i)
        d.limbs[i] = {byte_at(data, 88 + i * 6), u32_at(data, 90 + i * 6)};
    for (unsigned i = 0; i < d.bone_flags.size(); ++i)
        d.bone_flags[i] = byte_at(data, 184 + i);
    d.source = c_string(data.subspan(224));
    return d;
}
std::uint32_t SaxsDescriptor::limb_flags(unsigned part) const {
    if (part >= limbs.size())
        return 0;
    const auto flags = limbs[part].flags;
    return ((flags & 1) ? 8u : 0u) | ((flags & 2) ? 1u : 0u) | ((flags & 4) ? 4u : 0u);
}
std::uint32_t SaxsDescriptor::material_flags(unsigned part) const {
    const auto flags = limb_flags(part);
    return (flags & 1) ? 0x12010008u : (flags & 4) ? 0x11009u : 0x10001u;
}

SaxsModel SaxsModel::decode(ByteView data, float scale, std::span<const unsigned> texture_heights) {
    need(finite(scale) && scale > 0, "Invalid SXMD scale");
    need(data.size() >= 40, "SXMD header is truncated");
    SaxsModel model;
    model.image.assign(data.begin(), data.end());
    auto& image = model.image;
    auto set_byte = [&](std::size_t offset, std::uint8_t value) {
        need(offset < image.size(), "SXMD write outside chunk");
        image[offset] = std::byte{value};
    };
    auto set_word = [&](std::size_t offset, std::uint16_t value) {
        set_byte(offset, static_cast<std::uint8_t>(value));
        set_byte(offset + 1, static_cast<std::uint8_t>(value >> 8));
    };
    model.scale = scale;
    model.animation_scale = f32_at(data, 8);
    need(finite(model.animation_scale) && model.animation_scale != 0, "Invalid SXMD animation scale");
    const unsigned part_count = byte_at(data, 0);
    const unsigned bone_count = u16_at(data, 28);
    need(part_count > 0, "SXMD has no body parts");
    need(bone_count > 0 && bone_count <= maximum_bones, "SXMD bone count outside 1..40");
    need(texture_heights.size() == part_count, "SXMD texture count differs from body parts");
    const auto bone_offset = u32_at(data, 32), vertex_offset = u32_at(data, 36);
    // 0x16030af0 scales serialized bone floats 0..2 and 4..27 and the square-rooted radius.
    auto bones = span_at(data, bone_offset, static_cast<std::size_t>(bone_count) * 140, "SXMD bones");
    for (unsigned b = 0; b < bone_count; ++b) {
        const auto o = static_cast<std::size_t>(b) * 140;
        Bone bone;
        bone.parent = u16_at(bones, o + 12);
        need(bone.parent == 0xffff || bone.parent < b, "SXMD bone parent is not an earlier bone");
        for (unsigned i = 0; i < 3; ++i)
            bone.offset[i] = scale * f32_at(bones, o + i * 4);
        for (unsigned i = 0; i < 24; ++i)
            bone.box[i] = scale * f32_at(bones, o + 16 + i * 4);
        const auto squared = f32_at(bones, o + 136);
        need(finite(squared) && squared >= 0, "Invalid SXMD bone radius");
        bone.radius = static_cast<float>(std::sqrt(static_cast<double>(squared)) * scale);
        for (auto value : bone.offset)
            need(finite(value), "Non-finite SXMD bone offset");
        model.bones.push_back(bone);
    }
    // 0x16030cd0 relocates body parts, rotates patch links, applies the legacy count swap,
    // negates normals of explicit-face parts, and derives the vertex count from point sources.
    std::map<std::int32_t, bool> negated;
    std::int32_t highest = 0;
    bool swapped = false;
    unsigned swap_count = 0;
    for (unsigned p = 0; p < part_count; ++p) {
        const auto po = u32_at(data, 40 + p * 4);
        Part part;
        const unsigned ring_count = byte_at(data, po);
        part.count_high = byte_at(data, po + 1);
        const unsigned face_count = u16_at(data, po + 2);
        part.flags = u16_at(data, po + 20);
        if (const auto patch_offset = u32_at(data, po + 28)) {
            const unsigned link_count = u16_at(data, patch_offset + 2);
            if (link_count) {
                Patch patch{byte_at(data, patch_offset), byte_at(data, patch_offset + 1), patch_offset, {}};
                auto links = span_at(data, patch_offset + 8, link_count * 8u, "SXMD patch links");
                for (unsigned i = 0; i < link_count; ++i)
                    patch.links.push_back({u16_at(links, i * 8), u16_at(links, i * 8 + 2),
                                           u16_at(links, i * 8 + 4), byte_at(links, i * 8 + 6),
                                           byte_at(links, i * 8 + 7)});
                if (part.flags & 0x80) {
                    auto first = std::find_if(patch.links.begin(), patch.links.end(), [&](const Link& link) {
                        return link.u == 0 && link.part == p;
                    });
                    if (first != patch.links.end()) {
                        std::rotate(patch.links.begin(), first, patch.links.end());
                        part.flags |= patch.links.front().ring ? 0x200 : 0x100;
                        set_word(po + 20, part.flags);
                    }
                }
                part.patch = std::move(patch);
            }
        }
        if (part.count_high >= 3 && (face_count == 0 || part_count == 1))
            swapped = true;
        if (swapped)
            swap_count = part.count_high;
        for (unsigned r = 0; r < ring_count; ++r) {
            const auto ro = u32_at(data, po + 36 + r * 4);
            Ring ring;
            ring.offset = ro;
            ring.count = byte_at(data, ro);
            ring.flags = byte_at(data, ro + 1);
            ring.v = u16_at(data, ro + 2);
            ring.points_offset = u32_at(data, ro + 4);
            if (swapped) {
                part.count_high = ring.count;
                ring.count = static_cast<std::uint8_t>(swap_count);
                set_byte(po + 1, part.count_high);
                set_byte(ro, ring.count);
            }
            const auto count = part.point_count(ring);
            need(count > 0, "SXMD ring has no points");
            auto points = span_at(data, ring.points_offset, count * 10u, "SXMD ring points");
            for (unsigned i = 0; i < count; ++i)
                for (unsigned s = 0; s < 3; ++s) {
                    const auto source = i16_at(points, i * 10 + s * 2);
                    need(source >= (s ? -1 : 0), "Invalid SXMD point source");
                    highest = std::max<std::int32_t>(highest, source);
                    if (s == 0 && face_count)
                        negated[source] = !negated[source];
                }
            part.rings.push_back(ring);
        }
        need(!part.rings.empty(), "SXMD body part has no rings");
        if (face_count) {
            auto faces = span_at(data, u32_at(data, po + 24), face_count * 6u, "SXMD faces");
            for (unsigned f = 0; f < face_count; ++f) {
                std::array<std::uint16_t, 3> face{u16_at(faces, f * 6), u16_at(faces, f * 6 + 2),
                                                  u16_at(faces, f * 6 + 4)};
                for (auto v : face)
                    need(v <= part.point_count(part.rings.back()), "SXMD face outside its ring");
                part.faces.push_back(face);
            }
        }
        model.parts.push_back(std::move(part));
    }
    // Links must address real body parts and rings; a patch target that no link names is unused.
    for (const auto& part : model.parts)
        if (part.patch)
            for (const auto& link : part.patch->links) {
                need(link.part < part_count, "SXMD patch link outside body parts");
                need(link.ring < model.parts[link.part].rings.size(), "SXMD patch link outside rings");
            }
    // 0x160310e0 converts 16-byte serialized vertices to floats.
    const auto vertex_count = static_cast<std::size_t>(highest) + 1;
    auto vertices = span_at(data, vertex_offset, vertex_count * 16, "SXMD vertices");
    for (std::size_t v = 0; v < vertex_count; ++v) {
        const auto o = v * 16;
        SourceVertex vertex;
        for (unsigned i = 0; i < 3; ++i) {
            vertex.position[i] = static_cast<float>(scale * static_cast<double>(i16_at(vertices, o + i * 2)));
            auto normal = i16_at(vertices, o + 10 + i * 2);
            if (negated[static_cast<std::int32_t>(v)])
                normal = static_cast<std::int16_t>(-normal);
            vertex.normal[i] = static_cast<float>(static_cast<double>(normal) * normal_unit);
        }
        vertex.bone = i16_at(vertices, o + 6);
        need(vertex.bone >= 0 && static_cast<unsigned>(vertex.bone) < bone_count,
             "SXMD vertex bone outside skeleton");
        vertex.weight = static_cast<float>(static_cast<double>(u16_at(vertices, o + 8)) * weight_unit);
        model.vertices.push_back(vertex);
    }
    // 0x160313f0 with texture flags 0x400: one 256x256 texture page per part.
    for (unsigned p = 0; p < part_count; ++p) {
        auto& part = model.parts[p];
        const auto height = texture_heights[p];
        need(height > 0 && height <= 0xffff, "Invalid SXTX height");
        for (unsigned r = 0; r < part.rings.size(); ++r) {
            auto& ring = part.rings[r];
            const auto count = part.point_count(ring);
            for (unsigned i = 0; i < count; ++i) {
                const auto record = ring.points_offset + i * 10;
                auto u = std::to_integer<std::uint8_t>(image[record + 6]);
                auto v = std::to_integer<std::uint8_t>(image[record + 7]);
                if (!part.faces.empty()) {
                    u = truncate_byte(u * (256.0 * static_cast<double>(texel_unit)));
                    v = truncate_byte(v * (1.0 / ((static_cast<int>(height) - 1) / 256.0 + 0.001)));
                } else {
                    double row = (256.0 - 1.0) * ((ring.v + 1) * static_cast<double>(ring_v_unit)) + 0.5;
                    if (r + 1 == part.rings.size())
                        row = 255.0;
                    u = truncate_byte(u * static_cast<double>(texel_unit) * 256.0);
                    v = truncate_byte(row);
                }
                set_byte(record + 6, u);
                set_byte(record + 7, v);
            }
            ring.v = 256;
            set_word(ring.offset + 2, ring.v);
        }
        if (part.patch) {
            part.patch->v = 255;
            set_byte(part.patch->offset + 1, part.patch->v);
            for (auto& link : part.patch->links) {
                link.u = truncate_byte(link.u * (256.0 * static_cast<double>(texel_unit)));
                link.v = truncate_byte(link.v * (256.0 * static_cast<double>(texel_unit)));
            }
        }
    }
    for (auto& part : model.parts)
        for (auto& ring : part.rings)
            for (unsigned i = 0; i < part.point_count(ring); ++i) {
                const auto record = ring.points_offset + i * 10;
                Point point;
                for (unsigned s = 0; s < 3; ++s)
                    point.sources[s] = i16_at(image, record + s * 2);
                point.u = byte_at(image, record + 6);
                point.v = byte_at(image, record + 7);
                point.flags = u16_at(image, record + 8);
                ring.points.push_back(point);
            }
    return model;
}

SaxsAnimation SaxsAnimation::decode(ByteView data) {
    need(data.size() >= 12, "SXSK header is truncated");
    SaxsAnimation a;
    a.declared_frames = u16_at(data, 0);
    const unsigned keys = u16_at(data, 2);
    a.scale = f32_at(data, 4);
    a.bones = u16_at(data, 8);
    // Two stock clips store +inf; the root-motion ratio model/animation scale then becomes zero.
    need(!std::isnan(a.scale) && a.scale != 0, "Invalid SXSK scale");
    need(keys > 0, "SXSK has no keys");
    need(a.bones > 0 && a.bones <= SaxsModel::maximum_bones, "SXSK bone count outside 1..40");
    // SAXSANIM::Load reports a frame/key mismatch and continues with the key count.
    auto records = span_at(data, 12, keys * 12u, "SXSK frame records");
    for (unsigned k = 0; k < keys; ++k) {
        Frame frame;
        frame.tag = u16_at(records, k * 12);
        for (unsigned i = 0; i < 3; ++i)
            frame.root[i] =
                static_cast<float>(i16_at(records, k * 12 + 2 + i * 2) * static_cast<double>(root_unit));
        auto rotations = span_at(data, u32_at(records, k * 12 + 8), a.bones * 8u, "SXSK rotations");
        for (unsigned b = 0; b < a.bones; ++b) {
            std::array<float, 4> q{};
            for (unsigned i = 0; i < 4; ++i)
                q[i] =
                    static_cast<float>(i16_at(rotations, b * 8 + i * 2) * static_cast<double>(rotation_unit));
            // Stock clips contain zero quaternions; 0x16030770 then divides by zero (NaN bones).
            a.zero_rotations += q[0] == 0 && q[1] == 0 && q[2] == 0 && q[3] == 0;
            frame.rotations.push_back(q);
        }
        a.frames.push_back(std::move(frame));
    }
    return a;
}

SaxsSequence SaxsSequence::decode(ByteView data) {
    // Callback 0x409110 requests 396 bytes; stock chunks end with the source path at offset 140.
    need(data.size() >= 140, "SKEL descriptor is truncated");
    SaxsSequence s;
    s.framerate = std::bit_cast<std::int32_t>(u32_at(data, 4));
    const auto count = std::bit_cast<std::int32_t>(u32_at(data, 8));
    need(s.framerate > 0, "Invalid SKEL frame rate");
    // At most eight triggers are retained.
    for (std::int32_t i = 0; i < count && i < 8; ++i)
        s.triggers.push_back({std::bit_cast<std::int32_t>(u32_at(data, 12 + i * 12)),
                              u32_at(data, 16 + i * 12),
                              std::bit_cast<std::int32_t>(u32_at(data, 20 + i * 12))});
    for (unsigned i = 0; i < s.references.size(); ++i) {
        const auto at = 108 + 16 * i;
        s.references[i] = {i16_at(data, at),
                           u16_at(data, at + 2),
                           {f32_at(data, at + 4), f32_at(data, at + 8), f32_at(data, at + 12)}};
    }
    s.source = c_string(data.subspan(140));
    return s;
}
void SaxsSequence::animate(float& frame, float frames, unsigned count,
                           const std::function<void(const Trigger&)>& callback) const {
    const auto start = frame;
    const auto end = start + frames;
    frame = end;
    if (callback)
        for (const auto& trigger : triggers)
            if (static_cast<std::int64_t>(start) <= trigger.frame &&
                static_cast<std::int64_t>(end) > trigger.frame) {
                if (trigger.type == loop)
                    frame = static_cast<float>(trigger.value);
                callback(trigger);
            }
    if (static_cast<float>(count) <= frame)
        frame = 0;
}
float SaxsSequence::advance(float frame, float frames, unsigned count) const {
    animate(frame, frames, count, [](const Trigger&) {});
    return frame;
}

SaxsPose saxs_pose(const SaxsModel& model, const SaxsAnimation& animation, float frame) {
    need(animation.bones == model.bones.size(), "SXSK skeleton does not match SXMD bones");
    need(std::isfinite(frame) && frame >= 0, "Invalid SAXS animation frame");
    // 0x16032750 with a 16.16 frame position. Rotations use the integer frame; only the root
    // position interpolates (extrapolating from frames 0..1 after the last frame).
    const auto count = static_cast<unsigned>(animation.frames.size());
    auto f = static_cast<unsigned>(frame);
    double fraction = frame - f;
    if (f >= count) {
        f = count - 1;
        fraction = 0;
    }
    fraction = static_cast<std::uint16_t>(fraction * 65536.0) * (1.0 / 65536.0);
    const auto& current = animation.frames[f];
    SaxsPose pose;
    std::array<double, 3> root{};
    for (unsigned i = 0; i < 3; ++i) {
        double next = current.root[i];
        if (f + 1 < count)
            next = animation.frames[f + 1].root[i];
        else if (count > 1)
            next = animation.frames[1].root[i] - animation.frames[0].root[i] + current.root[i];
        root[i] = current.root[i] * (1.0 - fraction) + next * fraction;
    }
    // Root motion is consumed by the instance in world units: raw * scale * 10 / animation scale.
    const double ratio = model.animation_scale / animation.scale;
    for (unsigned i = 0; i < 3; ++i)
        pose.root[i] = static_cast<float>(root[i] * ratio * model.scale * 10.0);
    for (unsigned b = 0; b < model.bones.size(); ++b) {
        auto local = rotation(current.rotations[b]);
        const auto& bone = model.bones[b];
        if (bone.parent == 0xffff)
            pose.bones.push_back(local);
        else {
            for (unsigned i = 0; i < 3; ++i)
                local[9 + i] = bone.offset[i];
            pose.bones.push_back(multiply(local, pose.bones[bone.parent]));
        }
    }
    return pose;
}

SaxsPose saxs_bind_pose(const SaxsModel& model) {
    SaxsPose pose;
    for (const auto& bone : model.bones) {
        SaxsMatrix local{1, 0, 0, 0, 1, 0, 0, 0, 1, 0, 0, 0};
        if (bone.parent == 0xffff)
            pose.bones.push_back(local);
        else {
            for (unsigned i = 0; i < 3; ++i)
                local[9 + i] = bone.offset[i];
            pose.bones.push_back(multiply(local, pose.bones[bone.parent]));
        }
    }
    pose.root = model.bones.front().offset;
    return pose;
}

namespace {
SaxsMatrix world_bone(const SaxsMatrix& bone) {
    // 0x16032750: bone * SAXS::worldmat, stored before UpdateBBox transforms points.
    return multiply(bone, {-1, 0, 0, 0, 0, 1, 0, -1, 0, 0, 0, 0});
}
SaxsMatrix draw_bone(const SaxsPose& pose, unsigned bone) {
    auto matrix = pose.bones.at(bone);
    if (bone < pose.bone_scales.size()) {
        const auto& scale = pose.bone_scales[bone];
        // Native scales the Z-up world columns without moving joints. SXCore's
        // columns map to world (-x,-z,y), so the Y/Z factors exchange places here.
        for (unsigned row = 0; row < 9; row += 3) {
            matrix[row] *= scale.x;
            matrix[row + 1] *= scale.z;
            matrix[row + 2] *= scale.y;
        }
    }
    return matrix;
}
Vec3 transform_point(const SaxsMatrix& m, Vec3 p) {
    return {static_cast<float>(static_cast<double>(p.x) * m[0] + static_cast<double>(p.y) * m[3] +
                               static_cast<double>(p.z) * m[6] + m[9]),
            static_cast<float>(static_cast<double>(p.x) * m[1] + static_cast<double>(p.y) * m[4] +
                               static_cast<double>(p.z) * m[7] + m[10]),
            static_cast<float>(static_cast<double>(p.x) * m[2] + static_cast<double>(p.y) * m[5] +
                               static_cast<double>(p.z) * m[8] + m[11])};
}
void include_point(Box3& box, Vec3 p) {
    // Preserve the original ordered comparisons, including zero-quaternion NaNs.
    const auto include = [](float& low, float& high, float value) {
        if (value >= low) {
            if (value > high)
                high = value;
        } else
            low = value;
    };
    include(box.minimum.x, box.maximum.x, p.x);
    include(box.minimum.y, box.maximum.y, p.y);
    include(box.minimum.z, box.maximum.z, p.z);
}
void translate_box(Box3& box, Vec3 delta) {
    box.minimum = {box.minimum.x + delta.x, box.minimum.y + delta.y, box.minimum.z + delta.z};
    box.maximum = {box.maximum.x + delta.x, box.maximum.y + delta.y, box.maximum.z + delta.z};
}
} // namespace
SaxsBounds saxs_bounds(const SaxsModel& model, const SaxsDescriptor& descriptor, const SaxsPose& pose) {
    need(pose.bones.size() == model.bones.size(), "SAXS bounds pose does not match bones");
    constexpr Box3 empty{{10000000, 10000000, 10000000}, {-10000000, -10000000, -10000000}};
    SaxsBounds result{empty, empty, false};
    // Six offsets into the original 140-byte bone record, relative to the start of its 24-float box.
    constexpr std::array<unsigned, 6> endpoints{0, 3, 1, 7, 20, 17};
    for (unsigned b = 0; b < model.bones.size(); ++b) {
        const auto& box = model.bones[b].box;
        const auto m = world_bone(draw_bone(pose, b));
        for (unsigned corner = 0; corner < 8; ++corner)
            include_point(result.visual,
                          transform_point(m, {box[corner * 3], box[corner * 3 + 1], box[corner * 3 + 2]}));
        if (!(descriptor.bone_flags.at(b) & 1))
            continue;
        result.collidable = true;
        for (unsigned i = 0; i < endpoints.size(); ++i) {
            const double distance = box[endpoints[i]];
            const auto axis = i >> 1;
            // Literal column access in UpdateBBox: m[axis], m[axis+3], m[axis+6].
            include_point(result.collision, {static_cast<float>(distance * m[axis] + m[9]),
                                             static_cast<float>(distance * m[axis + 3] + m[10]),
                                             static_cast<float>(distance * m[axis + 6] + m[11])});
        }
    }
    const Vec3 root{-pose.root[0], -pose.root[2], pose.root[1]};
    translate_box(result.visual, root);
    translate_box(result.collision, root);
    return result;
}
double saxs_intercept(const SaxsModel& model, const SaxsPose& pose, Vec3 from, Vec3 ray) {
    need(pose.bones.size() == model.bones.size(), "SAXS picking pose does not match bones");
    // CheckIntercept subtracts the instance translation before inverse-transforming each bone.
    from = {from.x + pose.root[0], from.y + pose.root[2], from.z - pose.root[1]};
    double closest = -1;
    for (unsigned bone = 0; bone < model.bones.size(); ++bone) {
        const auto m = world_bone(draw_bone(pose, bone));
        const Vec3 d{from.x - m[9], from.y - m[10], from.z - m[11]};
        const auto inverse = [&](Vec3 p) {
            return Vec3{p.x * m[0] + p.y * m[1] + p.z * m[2],
                        p.x * m[3] + p.y * m[4] + p.z * m[5],
                        p.x * m[6] + p.y * m[7] + p.z * m[8]};
        };
        const auto p = inverse(d), v = inverse(ray);
        const auto& box = model.bones[bone].box;
        const std::array<double, 3> start{p.x, p.y, p.z}, change{v.x, v.y, v.z},
            low{box[0], box[1], box[20]}, high{box[3], box[7], box[17]};
        double near = 0, far = 1.0e30;
        bool hit = true;
        for (unsigned axis = 0; axis < 3; ++axis) {
            if (change[axis] == 0) {
                if (start[axis] < low[axis] || start[axis] > high[axis])
                    hit = false;
            } else {
                const double a = (low[axis] - start[axis]) / change[axis],
                             b = (high[axis] - start[axis]) / change[axis];
                near = std::max(near, std::min(a, b));
                far = std::min(far, std::max(a, b));
            }
        }
        if (hit && near <= far && (closest < 0 || near < closest))
            closest = near;
    }
    return closest;
}
Vec3 saxs_reference_point(const SaxsPose& pose, const SaxsReferencePoint& ref) {
    need(ref.bone >= 0 && static_cast<std::size_t>(ref.bone) < pose.bones.size(),
         "SAXS reference point bone outside pose");
    const auto m = world_bone(pose.bones[ref.bone]);
    Vec3 point;
    if (ref.flags & 1)
        point = {m[9] + ref.offset.x, m[10] + ref.offset.y, m[11] + ref.offset.z};
    else
        point = transform_point(m, {ref.offset.x, -ref.offset.z, ref.offset.y});
    return {point.x - pose.root[0], point.y - pose.root[2], point.z + pose.root[1]};
}

std::vector<Vec3> saxs_shadow(const SaxsModel& model, const SaxsPose& pose, Vec3 normal, double distance) {
    need(pose.bones.size() == model.bones.size(), "SAXS shadow pose does not match bones");
    std::vector<Vec3> result;
    if (std::abs(normal.z) < 1e-6f)
        return result;
    result.reserve(model.bones.size() * 8);
    const Vec3 root{-pose.root[0], -pose.root[2], pose.root[1]};
    const auto ring = [&](unsigned bone, std::array<unsigned, 4> corners) {
        const auto m = world_bone(draw_bone(pose, bone));
        const auto& box = model.bones[bone].box;
        for (const auto k : corners) {
            auto p = transform_point(m, {box[k * 3], box[k * 3 + 1], box[k * 3 + 2]});
            p = {p.x + root.x, p.y + root.y, p.z + root.z};
            // 0x160331c0 along (0,0,-1): p + dir * (distance - n.p) / (n.dir).
            const double t = (distance - (double(normal.x) * p.x + double(normal.y) * p.y + double(normal.z) * p.z)) /
                             -double(normal.z);
            result.push_back({p.x, p.y, static_cast<float>(p.z - t)});
        }
    };
    for (unsigned b = 0; b < model.bones.size(); ++b) {
        const auto parent = model.bones[b].parent;
        if (parent == 0xffff || model.bones[parent].radius == 0)
            ring(b, {0, 4, 1, 5});
        else
            ring(parent, {2, 6, 3, 7});
        ring(b, {2, 6, 3, 7});
    }
    return result;
}
Vec3 saxs_bone_point(const SaxsModel& model, const SaxsPose& pose, unsigned bone, CrtRandom* random) {
    need(bone < model.bones.size() && bone < pose.bones.size(), "SAXS bone point outside model/pose");
    SaxsReferencePoint reference{static_cast<std::int16_t>(bone), 1, {}};
    if (random) { // 0x16029c40 samples three specific pairs of the serialized eight box corners.
        const auto& box = model.bones[bone].box;
        const auto sample = [&](float low, float high) {
            return static_cast<float>(random->next() * (static_cast<double>(high) - low) / 32768 + low);
        };
        reference.flags = 0;
        reference.offset = {sample(box[0], box[3]), sample(box[1], box[7]), sample(box[20], box[17])};
    }
    return saxs_reference_point(pose, reference);
}
SaxsAttachment saxs_closest_bone(const SaxsModel& model, const SaxsPose& pose, Vec3 point) {
    // GetClosestBone 0x1602a010: use the original attachment matrices, ignore empty
    // bones, let later bones win ties, and project outside points onto the spheroid.
    point = {point.x + pose.root[0], point.y + pose.root[2], point.z - pose.root[1]};
    SaxsAttachment closest;
    double best = -1;
    for (unsigned bone = 0; bone < model.bones.size(); ++bone) {
        const auto& box = model.bones[bone].box;
        const Vec3 low{box[0], box[1], box[20]}, high{box[3], box[7], box[17]};
        if (!(low.x < high.x && low.y < high.y && low.z < high.z))
            continue;
        const auto m = world_bone(pose.bones.at(bone));
        const Vec3 d{point.x - m[9], point.y - m[10], point.z - m[11]};
        Vec3 offset{d.x * m[0] + d.y * m[1] + d.z * m[2], d.x * m[6] + d.y * m[7] + d.z * m[8],
                    -(d.x * m[3] + d.y * m[4] + d.z * m[5])};
        const Vec3 center{(low.x + high.x) * .5f, (low.y + high.y) * .5f, (low.z + high.z) * .5f},
            radii{(high.x - low.x) * .5f, (high.y - low.y) * .5f, (high.z - low.z) * .5f};
        const auto excess = [](float value, float center, float radius) {
            return std::max(std::abs((value - center) / radius) - 1.0, 0.0) * radius;
        };
        const double x = excess(offset.x, center.x, radii.x), y = excess(offset.y, center.y, radii.y),
                     z = excess(offset.z, center.z, radii.z), distance = x * x + y * y + z * z;
        if (best >= 0 && distance > best)
            continue;
        best = distance;
        if (distance != 0) {
            const double nx = (offset.x - center.x) / radii.x, ny = (offset.y - center.y) / radii.y,
                         nz = (offset.z - center.z) / radii.z,
                         length = std::sqrt(nx * nx + ny * ny + nz * nz);
            offset = {static_cast<float>(nx / length * radii.x + center.x),
                      static_cast<float>(ny / length * radii.y + center.y),
                      static_cast<float>(nz / length * radii.z + center.z)};
        }
        closest = {bone, offset};
    }
    return closest;
}
SaxsTopology saxs_topology(const SaxsModel& model, const SaxsDescriptor& descriptor, float factor) {
    need(std::isfinite(factor) && factor >= 1 && factor <= 100, "SAXS detail factor outside 1..100");
    // UpdateLOD 0x16029510: steps are factor * SAXS quality in 16.16; caps and patches below 2,
    // forced rings ignored above 4.
    std::uint32_t flags = 0x414;
    if (factor < 2)
        flags |= 0x1800;
    if (factor > 4)
        flags |= 0x100;
    const auto point_step =
        static_cast<std::int64_t>(static_cast<float>(factor * descriptor.quality[0]) * 65536.0);
    const auto ring_step =
        static_cast<std::int64_t>(static_cast<float>(factor * descriptor.quality[1]) * 65536.0);
    need(point_step < 0x7fffffff && ring_step < 0x7fffffff, "SAXS detail step overflow");
    return Builder(model, flags, static_cast<std::int32_t>(point_step), static_cast<std::int32_t>(ring_step))
        .build();
}

Mesh saxs_mesh(const SaxsModel& model, const SaxsDescriptor& descriptor, const SaxsTopology& topology,
               const SaxsPose& pose) {
    need(pose.bones.size() == model.bones.size(), "SAXS pose does not match bones");
    Mesh mesh;
    std::vector<SaxsMatrix> bones;
    bones.reserve(pose.bones.size());
    for (unsigned bone = 0; bone < pose.bones.size(); ++bone)
        bones.push_back(draw_bone(pose, bone));
    for (unsigned p = 0; p < model.parts.size(); ++p)
        mesh.materials.push_back(
            {descriptor.material_flags(p), FourCC::from_text(std::format(".{:03}", p + 1))});
    mesh.vertices.reserve(topology.vertices.size());
    for (const auto& v : topology.vertices) {
        // 0x16033070: weighted sum of up to three bone-local source points.
        std::array<double, 3> position{};
        for (auto source : v.sources) {
            if (source < 0)
                continue;
            need(static_cast<std::size_t>(source) < model.vertices.size(),
                 "SAXS source vertex outside model");
            const auto& s = model.vertices[source];
            const auto& m = bones[s.bone];
            for (unsigned i = 0; i < 3; ++i)
                position[i] += (static_cast<double>(s.position[2]) * m[6 + i] + s.position[1] * m[3 + i] +
                                s.position[0] * m[i] + m[9 + i]) *
                               s.weight;
        }
        // Development lighting: the first source normal rotated by its bone.
        const auto& first = model.vertices.at(v.sources[0]);
        const auto& m = bones[first.bone];
        std::array<double, 3> normal{};
        for (unsigned i = 0; i < 3; ++i)
            normal[i] = first.normal[2] * m[6 + i] + first.normal[1] * m[3 + i] + first.normal[0] * m[i];
        auto length = std::sqrt(normal[0] * normal[0] + normal[1] * normal[1] + normal[2] * normal[2]);
        if (length > 0)
            for (auto& n : normal)
                n /= length;
        // SAXS::worldmat maps SXCore (x,y,z) to Z-up world (-x,-z,y); renderer space is (x,z,y).
        Vertex out;
        // SAXSINSTANCE::Draw translates the instance by +188 before CreateMesh. Keep all three
        // components, as UpdateBBox/GetRefPnt/CheckIntercept do, so rendered limbs stay pickable.
        out.position = {static_cast<float>(-position[0] - pose.root[0]),
                        static_cast<float>(position[1] + pose.root[1]),
                        static_cast<float>(-position[2] - pose.root[2])};
        out.normal = {static_cast<float>(-normal[0]), static_cast<float>(normal[1]),
                      static_cast<float>(-normal[2])};
        out.uv = {v.u, v.v};
        mesh.vertices.push_back(out);
    }
    // NaN bones from zero quaternions leave non-finite vertices; their triangles are not drawn.
    std::vector<bool> valid;
    for (auto& vertex : mesh.vertices) {
        const bool ok = finite(vertex.position.x) && finite(vertex.position.y) && finite(vertex.position.z);
        valid.push_back(ok);
        if (!ok)
            vertex.position = {};
        if (!finite(vertex.normal.x) || !finite(vertex.normal.y) || !finite(vertex.normal.z))
            vertex.normal = {0, 1, 0};
    }
    for (const auto& triangle : topology.triangles) {
        if (!valid.at(triangle.vertices[0]) || !valid.at(triangle.vertices[1]) ||
            !valid.at(triangle.vertices[2]))
            continue;
        mesh.indices.insert(mesh.indices.end(), triangle.vertices.begin(), triangle.vertices.end());
        mesh.triangle_materials.push_back(triangle.part);
    }
    return mesh;
}

SaxsAsset SaxsAsset::load(const ResourceCatalog& catalog, ResourceLocation location) {
    const auto& archive = catalog.archive(location);
    const auto& records = archive.records();
    auto folder = location.record;
    if (records.at(folder).tag != FourCC::from_text("SAXC"))
        folder = records[folder].parent;
    need(folder < records.size() && records[folder].tag == FourCC::from_text("SAXC"),
         "SAXS resource is not inside a SAXC folder");
    const auto context = std::string(archive.label()) + records[folder].path;
    try {
        std::optional<std::size_t> descriptor, geometry;
        std::map<std::uint32_t, std::size_t> textures;
        for (auto i = folder + 1; i < records[folder].subtree_end; ++i) {
            if (records[i].parent != folder)
                continue;
            if (records[i].tag == FourCC::from_text("SAXS") && !descriptor)
                descriptor = i;
            else if (records[i].tag == FourCC::from_text("SXMD") && !geometry)
                geometry = i;
            else if (records[i].tag == FourCC::from_text("SXTX"))
                textures.emplace(records[i].name.value, i);
        }
        need(descriptor && geometry, "SAXC folder lacks SAXS or SXMD");
        SaxsAsset asset;
        asset.descriptor = SaxsDescriptor::decode(archive.read(*descriptor));
        const auto bytes = archive.read(*geometry);
        need(!bytes.empty(), "Empty SXMD");
        const unsigned parts = std::to_integer<unsigned>(bytes[0]);
        // 0x160319a0 loads "<name>.001" onward, one TGA per body part.
        std::vector<unsigned> heights;
        for (unsigned p = 0; p < parts; ++p) {
            const auto name = FourCC::from_text(std::format(".{:03}", p + 1));
            auto it = textures.find(name.value);
            need(it != textures.end(), "missing SXTX:" + name.text());
            auto data = archive.read(it->second);
            need(data.size() >= 18 && data[0] == std::byte{} && data[1] == std::byte{} &&
                     data[2] == std::byte{2} && data[16] == std::byte{24},
                 "SXTX:" + name.text() + " is not an uncompressed 24-bit TGA");
            ModelAsset::Texture texture;
            texture.image = decode_tga(data);
            texture.header.width = texture.image.width;
            texture.header.height = texture.image.height;
            texture.source = {location.archive, it->second};
            // SAXS::SetChromaKey: exact RGB key for limbs with descriptor flag 1.
            if (asset.descriptor.limb_flags(p) & 8) {
                const auto key = asset.descriptor.limbs[p].key & 0xffffff;
                const auto rgba = ((key >> 16) & 255) | (key & 0xff00) | ((key & 255) << 16);
                for (auto& pixel : texture.image.rgba)
                    if ((pixel & 0xffffff) == rgba)
                        pixel &= 0xffffff;
            }
            heights.push_back(texture.image.height);
            asset.textures.push_back(std::move(texture));
        }
        asset.model = SaxsModel::decode(bytes, asset.descriptor.scale, heights);
        const auto group = records[folder].parent;
        if (group < records.size())
            for (auto i = group + 1; i < records[group].subtree_end; ++i) {
                if (records[i].parent != group || records[i].tag != FourCC::from_text("SXSK"))
                    continue;
                auto data = archive.read(i);
                if (data.size() < 12 || u16_at(data, 8) != asset.model.bones.size())
                    continue;
                Clip clip{records[i].name.text(), {location.archive, i}, {}};
                if (auto sequence = archive.find_chunk(FourCC::from_text("SKEL"), records[i].name, group))
                    clip.sequence = ResourceLocation{location.archive, *sequence};
                asset.clips.push_back(std::move(clip));
            }
        return asset;
    } catch (const std::exception& e) {
        throw Error(context + ": " + e.what());
    }
}
std::vector<Mesh::Material> SaxsAsset::materials() const {
    std::vector<Mesh::Material> result;
    for (unsigned p = 0; p < model.parts.size(); ++p)
        result.push_back({descriptor.material_flags(p), FourCC::from_text(std::format(".{:03}", p + 1))});
    return result;
}
} // namespace sacrifice
