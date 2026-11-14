#pragma once
#include "assets.hpp"
#include <functional>

namespace sacrifice {
// Descriptor/SKEL reference: bone, flags, unscaled XYZ offset. Flag 1 adds the offset in world axes;
// otherwise the bone rotates (x,-z,y) before SAXS::worldmat converts it to Z-up coordinates.
struct SaxsReferencePoint {
    std::int16_t bone{};
    std::uint16_t flags{};
    Vec3 offset;
};
// SAXS chunk read by Sacrifice.exe resource callback 0x408f20.
struct SaxsDescriptor {
    struct Limb {
        std::uint8_t flags{};
        std::uint32_t key{};
    };
    float scale{}, root_adjust{};
    std::array<float, 2> quality{};
    std::array<SaxsReferencePoint, 4> references; // Serialized bytes 24..87.
    std::array<Limb, 16> limbs{};
    std::array<std::uint8_t, 40> bone_flags{};
    std::string source;
    static SaxsDescriptor decode(ByteView data);
    // SAXS::SetLimbFlag and 0x16027210 render properties for a body part.
    [[nodiscard]] std::uint32_t limb_flags(unsigned part) const;
    [[nodiscard]] std::uint32_t material_flags(unsigned part) const;
};
// SXMD chunk after the game3d.dll SXCore loader chain at 0x16032500.
struct SaxsModel {
    static constexpr unsigned maximum_bones = 40;
    struct Bone {
        std::uint16_t parent{0xffff};
        std::array<float, 3> offset{};
        std::array<float, 24> box{};
        float radius{};
    };
    struct SourceVertex {
        std::array<float, 3> position{}, normal{};
        float weight{};
        std::int32_t bone{};
    };
    struct Point {
        std::array<std::int16_t, 3> sources{};
        std::uint8_t u{}, v{};
        std::uint16_t flags{};
    };
    struct Ring {
        std::uint8_t count{}, flags{};
        std::uint16_t v{};
        std::size_t offset{}, points_offset{}; // Serialized record and 10-byte point records.
        std::vector<Point> points;
    };
    struct Link {
        std::uint16_t part{}, ring{}, point{};
        std::uint8_t u{}, v{};
    };
    struct Patch {
        std::uint8_t target{}, v{};
        std::size_t offset{};
        std::vector<Link> links;
    };
    struct Part {
        std::uint8_t count_high{};
        std::uint16_t flags{};
        std::vector<Ring> rings;
        std::vector<std::array<std::uint16_t, 3>> faces;
        std::optional<Patch> patch;
        [[nodiscard]] unsigned point_count(const Ring& ring) const {
            return (static_cast<unsigned>(count_high) << 8) + ring.count;
        }
    };
    float scale{1}, animation_scale{1};
    std::vector<Bone> bones;
    std::vector<SourceVertex> vertices;
    std::vector<Part> parts;
    // The chunk after the loader's in-place UV, flag, and count rewrites. Mesh generation reads
    // point records through it because the original can address records beyond a ring.
    Bytes image;
    // texture_heights holds each part's original SXTX height for the explicit-face UV mapping.
    static SaxsModel decode(ByteView data, float scale, std::span<const unsigned> texture_heights);
};
// SXSK chunk converted by 0x160325b0.
struct SaxsAnimation {
    struct Frame {
        std::uint16_t tag{};
        std::array<float, 3> root{};
        std::vector<std::array<float, 4>> rotations;
    };
    float scale{1};
    unsigned declared_frames{};
    std::size_t zero_rotations{};
    std::uint16_t bones{};
    std::vector<Frame> frames;
    static SaxsAnimation decode(ByteView data);
};
// SKEL chunk read by Sacrifice.exe resource callback 0x409110.
struct SaxsSequence {
    struct Trigger {
        std::int32_t frame{};
        std::uint32_t type{};
        std::int32_t value{};
    };
    static constexpr std::uint32_t loop = 0x6c6f6f70;
    std::int32_t framerate{};
    std::vector<Trigger> triggers;
    std::array<SaxsReferencePoint, 2> references; // Serialized bytes 108..139.
    std::string source;
    static SaxsSequence decode(ByteView data);
    // Trigger crossings use the original start/end even when a loop or callback rewrites frame.
    // The callback can change the creature's clip or frame while the old clip finishes this call.
    void animate(float& frame, float frames, unsigned count,
                 const std::function<void(const Trigger&)>& callback) const;
    // SAXSANIM::Animate: advance, apply crossed loop triggers, and restart after the last frame.
    [[nodiscard]] float advance(float frame, float frames, unsigned count) const;
};
// Row-vector 3x3 rotation followed by translation, as used by the SXCore bone matrices.
using SaxsMatrix = std::array<float, 12>;
struct SaxsPose {
    std::vector<SaxsMatrix> bones;
    std::array<float, 3> root{};
    // SAXSINSTANCE keeps deformed draw/bounds matrices separate from attachment matrices.
    std::vector<Vec3> bone_scales;
};
SaxsPose saxs_pose(const SaxsModel& model, const SaxsAnimation& animation, float frame);
// Inspector-only rest pose: identity keys with the serialized bone offsets.
SaxsPose saxs_bind_pose(const SaxsModel& model);
// UpdateBBox (0x160287c0): all eight corners per bone for visuals; six axis endpoints from flagged
// bones for collision. Both boxes include the instance's XYZ root offset and are local Z-up boxes.
struct SaxsBounds {
    Box3 visual, collision;
    bool collidable{};
};
SaxsBounds saxs_bounds(const SaxsModel& model, const SaxsDescriptor& descriptor, const SaxsPose& pose);
// SAXSINSTANCE::CheckIntercept mode 2: closest posed bone box, with the instance root offset.
// Input is local Z-up; the ray is unbounded. Negative means no hit.
double saxs_intercept(const SaxsModel& model, const SaxsPose& pose, Vec3 from, Vec3 ray);
Vec3 saxs_reference_point(const SaxsPose& pose, const SaxsReferencePoint& reference);
// GetBonePos / GetRandomBonePos: posed local Z-up point, including instance root offset.
Vec3 saxs_bone_point(const SaxsModel& model, const SaxsPose& pose, unsigned bone, CrtRandom* random = nullptr);
// SAXSINSTANCE's planar shadow (SetupShadow 0x16029170, built by 0x16033270): per bone a ring of four box
// corners at its start (the parent's end ring when the parent has a radius) and four at its end, each
// posed point projected along local -Z onto the plane normal . p = distance (all local Z-up, with the
// instance root offset). Eight points per bone; saxs_shadow_band gives the eight side triangles.
std::vector<Vec3> saxs_shadow(const SaxsModel& model, const SaxsPose& pose, Vec3 normal, double distance);
inline constexpr std::array<std::uint32_t, 24> saxs_shadow_band{0, 4, 7, 0, 7, 3, 3, 7, 6, 3, 6, 2,
                                                                2, 6, 5, 2, 5, 1, 1, 5, 4, 1, 4, 0};
struct SaxsAttachment {
    unsigned bone{};
    Vec3 offset;
};
SaxsAttachment saxs_closest_bone(const SaxsModel& model, const SaxsPose& pose, Vec3 point);
struct SaxsTopology {
    struct Vertex {
        std::array<std::int16_t, 3> sources{};
        float u{}, v{};
        std::uint32_t part{};
    };
    struct Triangle {
        std::array<std::uint32_t, 3> vertices{};
        std::uint32_t part{};
    };
    std::vector<Vertex> vertices;
    std::vector<Triangle> triangles;
};
// UpdateLOD/0x16036020 mesh generation for a detail factor (1 is the closest original level).
SaxsTopology saxs_topology(const SaxsModel& model, const SaxsDescriptor& descriptor, float factor = 1);
Mesh saxs_mesh(const SaxsModel& model, const SaxsDescriptor& descriptor, const SaxsTopology& topology,
               const SaxsPose& pose);
struct SaxsAsset {
    struct Clip {
        std::string name;
        ResourceLocation animation;
        std::optional<ResourceLocation> sequence;
    };
    SaxsDescriptor descriptor;
    SaxsModel model;
    std::vector<ModelAsset::Texture> textures; // One per body part.
    std::vector<Clip> clips;                   // Compatible SXSK chunks in the enclosing folder.
    // Accepts the SAXC folder or its SAXS/SXMD chunks.
    static SaxsAsset load(const ResourceCatalog& catalog, ResourceLocation location);
    [[nodiscard]] std::vector<Mesh::Material> materials() const;
};
} // namespace sacrifice
