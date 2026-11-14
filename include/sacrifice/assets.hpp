#pragma once
#include "wad.hpp"
#include <array>

namespace sacrifice {
struct Vec2 {
    float x{}, y{};
};
struct Vec3 {
    float x{}, y{}, z{};
};
struct Box3 {
    Vec3 minimum, maximum;
};
struct Vertex {
    Vec3 position;
    Vec3 normal;
    Vec2 uv;
    std::uint32_t color{0xffffffff};
    std::uint32_t texture_layer{0xffffffff};
};
struct Image {
    unsigned width{}, height{};
    std::vector<std::uint32_t> rgba;
};
struct Mesh {
    struct Material {
        std::uint32_t flags{};
        FourCC texture;
        bool operator==(const Material&) const = default;
    };
    std::vector<Vertex> vertices;
    std::vector<std::uint32_t> indices;
    std::vector<Material> materials;
    std::vector<std::uint32_t> triangle_materials;
};
struct MrmGeometry {
    struct Collision {
        std::uint32_t mode{}; // MODEL: 0 eight corners, 1 six center-axis endpoints, 2 unchanged.
        Box3 box;
    };
    Box3 bounds;                       // Serialized Z-up frame-zero class bounds, bytes 44..67.
    std::vector<Collision> collisions; // Count at 68, up to four 28-byte records at 72.
    struct Face {
        std::array<std::uint32_t, 3> vertices;
        std::uint32_t material{}, last_vertex_count{};
        Vec3 normal; // Serialized inward-facing plane normal, game Z-up (MRM CheckIntercept).
    };
    struct Level {
        std::uint32_t count{}, first{};
    };
    std::vector<std::vector<Vertex>> frames;
    std::vector<Mesh::Material> materials;
    std::vector<Face> faces;
    std::vector<Level> levels;
    unsigned minimum_vertices{}, maximum_faces{}, minimum_faces{};
    std::size_t trailing_bytes{}, normal_links{};
    static MrmGeometry decode(ByteView data);
    [[nodiscard]] std::vector<std::uint32_t> active_faces(unsigned vertices) const;
    [[nodiscard]] Mesh sample(unsigned from = 0, unsigned to = 0, float fraction = 0,
                              unsigned vertices = 0) const;
};
struct Terrain {
    FourCC tileset;
    unsigned width{}, height{};
    std::vector<std::uint16_t> heights;
    Bytes tile_map;                  // TMAP: 512-byte (mode, index) remap table, then one code per cell.
    std::vector<std::uint8_t> tiles; // Explicit (mode 2) cells until TerrainMaterials::create_tile_map.
    // Per-cell texture layer overrides (empty, or 0 for the TMAP tile); scarred cells use 256 + scar tile.
    std::vector<std::uint32_t> scar_layers;
    std::vector<std::uint32_t> lightmap;
    float minimum{}, maximum{};
    // LEVL dwords 1..9 (version 3), copied by 0x4a9290 to the map info at +1432 (front-end buffer +1292).
    std::array<std::uint32_t, 9> scenario{};
    static Terrain load(const Archive& map);
    // Raw sample as LAND's height buffer holds it: rows are contiguous, so x == width reads the next row's
    // first sample, and SetHeightMap 0x1603dd60 appends a copy of row 0 plus its first sample (x, y <= size).
    [[nodiscard]] std::uint16_t raw(unsigned x, unsigned y) const;
    [[nodiscard]] float at(unsigned x, unsigned y) const;
    // LAND::GetPlane 0x16040730 cell split: true for the (x,y)-(x+1,y+1) diagonal.
    [[nodiscard]] bool main_diagonal(unsigned x, unsigned y) const;
    [[nodiscard]] Mesh mesh(bool tiled = false) const;
    // One vertex of mesh(false): position, light and slope normal of sample (x, y), x < width, y < height.
    [[nodiscard]] Vertex grid_vertex(unsigned x, unsigned y) const;
    // LAND::DentMap 0x160382f0: lower samples inside `radius` by a cosine-like falloff of `depth`.
    // Bit 15 (hole) is preserved. Missing LAND+72 anchors use 128, as GetAnchorMap does.
    void dent_map(Vec3 position, float radius, float depth);
    struct Displacement {
        int x{}, y{}, size{};
        std::vector<std::int16_t> samples;
        float applied{};
        Displacement() = default;
        Displacement(Vec3 center, int size);
    };
    Displacement mound(Vec3 center, int size, float flat, float outer, ByteView profile) const;
    bool apply_displacement(Displacement& patch, float scale);
    // LAND::GetEdge: height bit 15. Coordinates may equal width/height (the extra sample).
    [[nodiscard]] bool edge(int x, int y) const;
    // LAND::SetEdge 0x1603dad0 / SetSingleEdge 0x1603da70. Returns the number of newly written samples.
    int set_edge(int x, int y, int flags = 1, int size = 1);
};
// game3d.dll terrain level of detail for one land view. LAND::UpdateEdgeMipMaps 0x1603cd90 and
// LAND::UpdateVariance 0x1603d420 build per-level hole flags and height variance; the view's patch tree
// (root patch 0x16038bd0, children sub_16039950) splits by eye distance and variance, and each patch draws
// from the finest texture window containing it (LAND::UpdateTextureWindow 0x16046a80), or as near tiles
// once it is a single cell.
struct TerrainLod {
    static constexpr unsigned levels = 8; // Root patch: 256 cells.
    static constexpr std::uint8_t near_tiles = 7;
    unsigned size{};
    std::array<std::vector<std::uint8_t>, levels> flags; // Bit 0: a hole corner below; bit 1: all holes.
    std::array<std::vector<float>, levels> variance;     // Level 0 is never read.
    std::vector<std::uint8_t> holes;                     // Per grid point (size + 1 across): bit 15 set.
    std::vector<float> heights;                          // Per grid point, as the land's height buffer.
    struct Window {
        int x{}, y{};               // Cell origin.
        unsigned cells{}, texels{}; // Cells across, texels per cell.
    };
    struct View {
        std::array<Window, 6> windows; // Five camera windows and the whole-map layer 5.
        struct Triangle {
            std::array<std::uint32_t, 3> points; // Grid points, y x (size + 1) + x.
            std::uint8_t layer;                  // Window 0-5, or near_tiles.
        };
        std::vector<Triangle> triangles;
        std::vector<std::array<std::uint32_t, 2>> edges; // Unshared edges of drawn faces, in face order.
    };
    // Returns an empty grid set for maps other than 256 cells, whose patch indexing the original assumes.
    static TerrainLod build(const Terrain& terrain);
    // Recomputes what grid points [x0, x1] x [y0, y1] (x, y <= size; size is the extra row and column) reach
    // after the land changed there, as build would.
    void update(const Terrain& terrain, int x0, int y0, int x1, int y1);
    // Camera rows in map coordinates (X/Y, 10 units per cell, Z up) and the RENDERWINDOW half-angles in
    // degrees (+24 horizontal, +28 vertical).
    struct Camera {
        std::array<float, 3> right, forward, up, eye;
        float half_width{}, half_height{};
    };
    // `range` is the view's distance clamp(56 x quality + 32, 32, 256) from ILANDVIEW::SetQuality 0x16043330.
    [[nodiscard]] View classify(const Camera& camera, float range) const;
};
struct TerrainTile {
    std::array<Image, 7> mipmaps;
    std::array<std::uint8_t, 12> properties{};
};
TerrainTile decode_terrain_tile(ByteView data, const std::array<std::uint32_t, 256>& palette);
// MAPG "MGnn" record, 60 bytes copied to LAND+4508+60n by LAND::SetMapGroup 0x160404b0. Join groups hold
// the two groups they blend in place of the height trapezoid and 12 corner/edge tiles.
struct TileGroup {
    bool present{};
    std::array<float, 4> height{}, slope{};    // Procedural selection trapezoids (CreateTileMap 0x1603e540).
    bool join{};                               // Byte 32.
    std::array<std::uint32_t, 2> pair{};       // Join groups: bytes 0 and 4.
    std::vector<std::uint8_t> tiles;           // Count at 33, tiles from 34.
    std::array<std::uint8_t, 12> properties{}; // Bytes 48..59: overrides for member tile properties.
};
struct TerrainMaterials {
    FourCC tileset;
    std::array<std::uint32_t, 256> palette{}; // Tileset PALT, RGBA (LAND+0xac in game3d.dll).
    std::array<std::optional<TerrainTile>, 256> tiles;
    // LAND+6940: MAPT properties after ordered SetMapGroup overrides. Absent tiles start zero.
    std::array<std::array<std::uint8_t, 12>, 256> tile_properties{};
    std::vector<TerrainTile> scars; // Per-map scarred copies of tiles (texture layers 256 and up).
    std::vector<std::uint8_t> scar_inverse; // Lazy LAND::MakeScarLUT, shared by map and runtime scars.
    // 'mscr' default 256, range 32..256 (exe option table 0x4c8eb8); LAND::GetFreeScarTexture.
    unsigned scar_limit{256};
    std::uint32_t scar_time{};
    struct ScarSlot {
        std::size_t cell{SIZE_MAX};
        std::uint8_t base{};
        std::int16_t priority{};
        std::uint32_t touched{};
    };
    std::vector<ScarSlot> scar_slots;
    std::vector<unsigned> scar_free, scar_used; // Free stack; used list newest allocation first.
    std::uint32_t scar_layer(Terrain& terrain, std::size_t cell, std::int16_t priority);
    bool release_scar(Terrain& terrain, std::size_t cell); // LAND::ReleaseScarTexture, before SetTile.
    std::array<TileGroup, 16> groups;
    std::array<std::uint8_t, 256> tile_groups{}; // LAND+6428; tiles outside every group stay 0.
    unsigned group_count{};                      // LAND+4504: one past the last non-join group.
    // DT00..DT15 (0x456eb0 -> LAND::SetDetailTexture): 256x256 intensities on the engine "GrAy" palette.
    std::array<std::vector<std::uint8_t>, 16> details;
    // Detail slot each tile draws with (sub_16039E30 reads tile properties byte 7), after SetMapGroup
    // 0x160404b0 replaced it with the group's byte 55 for tiles whose properties byte 0 lacks 0x80.
    std::array<std::uint8_t, 256> tile_details{};
    // Bump height scale (LAND::SetDetailTextureFade's last argument) from the exe table at 0x4cf4d0: 1/64 for
    // JA_A, char and PYRO, otherwise 1/128.
    float detail_bump_scale{1.0f / 128};
    static TerrainMaterials load(const ResourceCatalog& catalog, FourCC tileset);
    // LAND::CreateTileMap 0x1603e540: explicit, group-random and procedural cells, then JoinTiles 0x1603ea80
    // over the whole map. Returns the cells JoinTiles changed.
    unsigned create_tile_map(Terrain& terrain) const;
    unsigned join_tiles(Terrain& terrain) const;
    void validate(const Terrain& terrain) const;
};
struct EntityRecord {
    std::uint32_t type{};
    std::size_t offset{};
    Bytes payload;
};
std::vector<EntityRecord> decode_entities(ByteView data);
Mesh decode_mrm(ByteView data);
Image decode_preview(ByteView data);
Image decode_tga(ByteView data);
struct Sample {
    bool wave_container{};
    float parameter{}, near_range{}, far_range{}; // Raw envelope; sound.dll scales ranges by 10 / 200.
    std::uint16_t format{}, channels{}, bits{}, block_align{};
    std::uint32_t sample_rate{}, byte_rate{};
    Bytes format_extra;
    Bytes audio;
};
Sample decode_sample(ByteView data);
struct TextureHeader {
    unsigned width{}, height{};
    FourCC palette;
    std::uint32_t flags{}, key{}, frames{}, frame_width{}, frame_height{};
    std::int16_t hotspot_x{}, hotspot_y{}; // ICON: packed signed coordinates at serialized +32.
    std::size_t trailing_bytes{};
};
TextureHeader decode_texture_header(ByteView data);
std::array<std::uint32_t, 256> decode_palette(ByteView data);
Image decode_texture(ByteView bytes, const std::array<std::uint32_t, 256>& palette);
Image load_texture(const Archive& archive, std::size_t index);
Image load_texture(const ResourceCatalog& catalog, ResourceLocation location);
Image texture_frame(const Image& image, const TextureHeader& header, unsigned frame);
struct ModelAsset {
    MrmGeometry geometry;
    Mesh mesh;
    struct Texture {
        TextureHeader header;
        Image image;
        ResourceLocation source;
    };
    std::vector<std::optional<Texture>> textures; // Same order as mesh.materials.
    static ModelAsset load(const ResourceCatalog& catalog, ResourceLocation model);
};
} // namespace sacrifice
