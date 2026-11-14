#include "renderer_webgpu.hpp"
#include "post_process.hpp"
#include "renderer_shared.hpp"
#include "sacrifice/media.hpp"
#include "sacrifice/player_visuals.hpp"
#include "sacrifice/presentation_audio.hpp"
#include "sacrifice/soul_actor.hpp"
#include "sacrifice/scene_effects.hpp"
#include "sacrifice/scene_match.hpp"
#include "sacrifice/scene_updates.hpp"
#include "sacrifice/spell_actor.hpp"
#include "sacrifice/widgets.hpp"
#include "sacrifice/wizard_control.hpp"
#include "scene_shader.hpp"
#include <DirectXMath.h>
#include <algorithm>
#include <bit>
#include <cmath>
#include <cstring>
#include <map>
#include <utility>

namespace sacrifice {
namespace {
using namespace DirectX;
template <class T, void (*Release)(T)> struct Handle {
    T value{};
    Handle() = default;
    Handle(T v) : value(v) {}
    ~Handle() {
        if (value)
            Release(value);
    }
    Handle(const Handle&) = delete;
    Handle& operator=(const Handle&) = delete;
    Handle(Handle&& other) noexcept : value(std::exchange(other.value, nullptr)) {}
    Handle& operator=(Handle&& other) noexcept {
        if (value)
            Release(value);
        value = std::exchange(other.value, nullptr);
        return *this;
    }
    operator T() const {
        return value;
    }
};
using Buffer = Handle<WGPUBuffer, wgpuBufferRelease>;
using Group = Handle<WGPUBindGroup, wgpuBindGroupRelease>;
using Layout = Handle<WGPUBindGroupLayout, wgpuBindGroupLayoutRelease>;
using Pipeline = Handle<WGPURenderPipeline, wgpuRenderPipelineRelease>;
using Sampler = Handle<WGPUSampler, wgpuSamplerRelease>;
using Texture = std::shared_ptr<WebGpuContext::Texture>;
struct Constants {
    XMFLOAT4X4 view_projection, world;
    XMFLOAT4 options{};
    XMUINT4 material{};
    XMFLOAT4 eye{}, fog{}, fog_color{}, ambient{}, sun_direction{}, sun_color{}, view_dir{};
    XMFLOAT4 bump{}, detail_light{}, windows[6]{}, model_reveal{};
    XMUINT4 point_options{};
    XMFLOAT4 remaster{}; // Remastered Look: enabled, sky layer, mist floor height and falloff.
    XMFLOAT4 weather{};  // Remastered weather: ground wetness, settled snow, storm, lightning flash.
    XMFLOAT4X4 shadow_matrix{}; // Remastered sun shadows: world to shadow map.
    XMFLOAT4 shadow{};          // Enabled, sampling spread, depth bias, model strength.
    XMFLOAT4 wind{};            // Remastered plant wind: time (s), strength, direction X/Z.
};
static_assert(sizeof(Constants) == 560 && offsetof(Constants, windows) == 304);
static_assert(sizeof(Vertex) == 40 && offsetof(Vertex, texture_layer) == 36);
constexpr std::size_t constant_stride = 768; // WebGPU dynamic uniform offsets are multiples of 256.
XMFLOAT4 unpack_color(std::uint32_t color) {
    return {float(color >> 16 & 255) / 255, float(color >> 8 & 255) / 255, float(color & 255) / 255, 1};
}
XMFLOAT4X4 identity() {
    XMFLOAT4X4 result;
    XMStoreFloat4x4(&result, XMMatrixIdentity());
    return result;
}
} // namespace
struct WebSceneRenderer::Impl {
    WebGpuContext& gpu;
    Terrain terrain;
    MapScene scene;
    TerrainMaterials materials;
    const ResourceCatalog* catalog{};
    CrtRandom random;
    std::unique_ptr<SceneMatch> simulation;
    SkyState sky_state;
    SkyTextures sky_textures;
    std::array<std::unique_ptr<VideoClip>, 5> sky_videos;
    std::array<double, 5> sky_video_time{};
    std::uint64_t sky_uploaded{UINT64_MAX};
    std::size_t applied_tiles{}, animated{};
    std::uint64_t land_revision{};
    std::map<FourCC, std::optional<Scar>> scars;
    struct EffectImage {
        TextureHeader header;
        Texture texture;
        Group group;
    };
    std::map<FourCC, EffectImage> effect_images;
    std::vector<ParticleBatch> effect_batches;
    std::vector<PlayerControl::Marker> order_markers;
    std::vector<PointLight> lights;
    renderer_shared::TerrainView land;
    Layout frame_layout, material_layout;
    Handle<WGPUPipelineLayout, wgpuPipelineLayoutRelease> pipeline_layout;
    Handle<WGPUShaderModule, wgpuShaderModuleRelease> shader;
    Sampler clamp, wrap;
    Buffer uniform, slots, land_vertices, fallback_indices;
    // Remastered grass layer (TerrainView::grass), rebuilt each frame into growing buffers.
    Buffer grass_vertices, grass_indices;
    std::size_t grass_vertex_bytes{}, grass_index_bytes{};
    std::vector<Vertex> grass_vertex_data;
    std::vector<std::uint32_t> grass_index_data;
    Buffer point_buffer, effect_vertices, effect_indices;
    std::size_t point_capacity{}, effect_vertex_capacity{}, effect_index_capacity{}, particle_count{};
    bool frame_dirty{};
    unsigned fallback_count{};
    Group frame_group, land_group;
    // Remastered sun shadows: the map, a stand-in bound in its place while the map is drawn, the comparison
    // sampler and the frame group that holds the stand-in. `shadows` is set per frame by prepare.
    Texture shadow_map, shadow_stand_in;
    Sampler shadow_sampler;
    Group shadow_frame_group;
    bool shadows{};
    Texture tiles, details, windows, edge, white, depth;
    std::size_t uniform_capacity{}, land_capacity{};
    unsigned width{}, height{};
    bool has_edge{};
    int texture_filter{2};
    RenderDetail detail;
    RenderDetailControl detail_control;
    // 0x47fbb0 view shake, fed from the match's requests at the last drawn eye (port option 'shak').
    CameraShake shake;
    CrtRandom shake_random;
    bool shake_enabled{true};
    Vec3 shake_eye;
    // Remastered look (post_process.hpp): the half-float scene target, its bloom chain and the passes.
    bool remastered{};
    WeatherLook weather;
    Texture hdr;
    std::vector<Texture> bloom;
    std::vector<std::pair<unsigned, unsigned>> bloom_size;
    Handle<WGPUShaderModule, wgpuShaderModuleRelease> post_shader;
    Layout post_layout;
    Handle<WGPUPipelineLayout, wgpuPipelineLayoutRelease> post_pipeline_layout;
    Pipeline post_down, post_up, post_ao, post_composite;
    Texture ao;
    float view_tan[2]{1, 1}; // The last frame's horizontal and vertical half-angle tangents.
    Sampler post_sampler;
    Buffer post_uniform;
    std::vector<Group> post_groups; // First downsample, downsamples, upsamples, composite.
    static constexpr std::size_t post_stride = 256;
    Texture color_target(unsigned w, unsigned h) {
        auto descriptor = WGPU_TEXTURE_DESCRIPTOR_INIT;
        descriptor.usage = WGPUTextureUsage_RenderAttachment | WGPUTextureUsage_TextureBinding;
        descriptor.size = {w, h, 1};
        descriptor.format = WGPUTextureFormat_RGBA16Float;
        auto result = std::make_shared<WebGpuContext::Texture>();
        result->texture = wgpuDeviceCreateTexture(gpu.device(), &descriptor);
        result->view = wgpuTextureCreateView(result->texture, nullptr);
        return result;
    }
    Pipeline post_pipeline(const char* entry, WGPUTextureFormat format, bool additive) {
        auto blend = WGPU_BLEND_STATE_INIT;
        blend.color.srcFactor = blend.color.dstFactor = WGPUBlendFactor_One;
        blend.alpha.srcFactor = blend.alpha.dstFactor = WGPUBlendFactor_One;
        auto color = WGPU_COLOR_TARGET_STATE_INIT;
        color.format = format;
        color.blend = additive ? &blend : nullptr;
        auto fragment = WGPU_FRAGMENT_STATE_INIT;
        fragment.module = post_shader;
        fragment.entryPoint = {entry, std::strlen(entry)};
        fragment.targetCount = 1;
        fragment.targets = &color;
        auto descriptor = WGPU_RENDER_PIPELINE_DESCRIPTOR_INIT;
        descriptor.layout = post_pipeline_layout;
        descriptor.vertex.module = post_shader;
        descriptor.vertex.entryPoint = {"vs_post", 7};
        descriptor.primitive.topology = WGPUPrimitiveTopology_TriangleList;
        descriptor.fragment = &fragment;
        return Pipeline(wgpuDeviceCreateRenderPipeline(gpu.device(), &descriptor));
    }
    void create_post() {
        auto source = WGPU_SHADER_SOURCE_WGSL_INIT;
        static const auto code = tuning::shader_constants(true) + post::wgsl; // remaster_tuning.hpp values.
        source.code = {code.data(), code.size()};
        auto module = WGPU_SHADER_MODULE_DESCRIPTOR_INIT;
        module.nextInChain = &source.chain;
        post_shader = wgpuDeviceCreateShaderModule(gpu.device(), &module);
        WGPUBindGroupLayoutEntry entries[6]{};
        for (unsigned i = 0; i < 6; ++i) {
            entries[i].binding = i;
            entries[i].visibility = WGPUShaderStage_Fragment;
        }
        entries[0].buffer.type = WGPUBufferBindingType_Uniform;
        entries[0].buffer.minBindingSize = sizeof(post::Constants);
        entries[1].sampler.type = WGPUSamplerBindingType_Filtering;
        for (unsigned i = 2; i < 6; ++i) {
            entries[i].texture.sampleType = i == 4 ? WGPUTextureSampleType_Depth : WGPUTextureSampleType_Float;
            entries[i].texture.viewDimension = WGPUTextureViewDimension_2D;
        }
        auto layout = WGPU_BIND_GROUP_LAYOUT_DESCRIPTOR_INIT;
        layout.entryCount = 6;
        layout.entries = entries;
        post_layout = wgpuDeviceCreateBindGroupLayout(gpu.device(), &layout);
        const WGPUBindGroupLayout layouts[]{post_layout};
        auto pipeline = WGPU_PIPELINE_LAYOUT_DESCRIPTOR_INIT;
        pipeline.bindGroupLayoutCount = 1;
        pipeline.bindGroupLayouts = layouts;
        post_pipeline_layout = wgpuDeviceCreatePipelineLayout(gpu.device(), &pipeline);
        post_down = post_pipeline("ps_down", WGPUTextureFormat_RGBA16Float, false);
        post_up = post_pipeline("ps_up", WGPUTextureFormat_RGBA16Float, true);
        post_ao = post_pipeline("ps_ao", WGPUTextureFormat_RGBA16Float, false);
        post_composite = post_pipeline("ps_composite", gpu.format(), false);
        auto sampler = WGPU_SAMPLER_DESCRIPTOR_INIT;
        sampler.magFilter = sampler.minFilter = WGPUFilterMode_Linear;
        sampler.addressModeU = sampler.addressModeV = sampler.addressModeW = WGPUAddressMode_ClampToEdge;
        post_sampler = wgpuDeviceCreateSampler(gpu.device(), &sampler);
    }
    // Size-dependent targets and the per-pass bind groups (rebuilt on resize).
    void resize_post(unsigned w, unsigned h) {
        hdr = color_target(w, h);
        ao = color_target(w, h);
        const auto levels = post::bloom_levels(w, h);
        bloom.clear();
        bloom_size.clear();
        for (unsigned i = 0; i < levels; ++i) {
            bloom_size.emplace_back(post::level_size(w, i), post::level_size(h, i));
            bloom.push_back(color_target(bloom_size.back().first, bloom_size.back().second));
        }
        const std::size_t passes = levels ? 2 * levels + 1 : 0;
        post_uniform = buffer(std::max<std::size_t>(passes, 1) * post_stride, WGPUBufferUsage_Uniform);
        post_groups.clear();
        const auto group = [&](std::size_t pass, WGPUTextureView a, WGPUTextureView b,
                               WGPUTextureView occluded = nullptr) {
            WGPUBindGroupEntry entries[6]{};
            for (unsigned i = 0; i < 6; ++i)
                entries[i].binding = i;
            entries[0].buffer = post_uniform;
            entries[0].offset = pass * post_stride;
            entries[0].size = sizeof(post::Constants);
            entries[1].sampler = post_sampler;
            entries[2].textureView = a;
            entries[3].textureView = b;
            entries[4].textureView = depth->view;
            entries[5].textureView = occluded ? occluded : white->view;
            auto descriptor = WGPU_BIND_GROUP_DESCRIPTOR_INIT;
            descriptor.layout = post_layout;
            descriptor.entryCount = 6;
            descriptor.entries = entries;
            post_groups.emplace_back(wgpuDeviceCreateBindGroup(gpu.device(), &descriptor));
        };
        if (!levels)
            return;
        group(0, hdr->view, white->view);
        for (unsigned i = 1; i < levels; ++i)
            group(i, bloom[i - 1]->view, white->view);
        for (unsigned i = levels - 1; i > 0; --i)
            group(post_groups.size(), bloom[i]->view, white->view);
        group(post_groups.size(), white->view, white->view);
        group(post_groups.size(), hdr->view, bloom[0]->view, ao->view);
    }
    void write_post(const Environment* environment) {
        const auto levels = static_cast<unsigned>(bloom.size());
        if (!levels)
            return;
        std::vector<std::byte> data((2 * levels + 1) * post_stride);
        std::size_t pass = 0;
        const auto put = [&](const post::Constants& c) {
            std::memcpy(data.data() + pass++ * post_stride, &c, sizeof(c));
        };
        put(post::downsample(width, height, true));
        for (unsigned i = 1; i < levels; ++i)
            put(post::downsample(bloom_size[i - 1].first, bloom_size[i - 1].second, false));
        for (unsigned i = levels - 1; i > 0; --i)
            put(post::upsample(bloom_size[i].first, bloom_size[i].second));
        put(post::occlusion(width, height, view_tan[0], view_tan[1]));
        put(post::look(environment, levels, width, height, weather));
        wgpuQueueWriteBuffer(gpu.queue(), post_uniform, 0, data.data(), data.size());
    }
    struct Material {
        renderer_shared::MaterialRange range;
        Group group;
        Texture texture;
    };
    struct Model {
        Buffer vertices, indices;
        std::vector<Material> materials;
        std::vector<XMFLOAT4X4> worlds;
        std::vector<bool> visible;
        std::vector<float> reveal, opacity;
        std::vector<bool> reveal_blend;
        Texture texture_override;
        bool grayscale{};
        float reveal_min{}, reveal_max{};
        std::uint64_t revision{UINT64_MAX};
        float frame{-1}, lod_frame{-2}, detail_radius{};
        unsigned lod_vertices{};
        std::vector<Mesh::Material> signatures;
        std::vector<std::uint32_t> topology, triangle_materials;
        std::shared_ptr<const WidgetField> field;
        std::uint32_t fade_flags{};
        bool screen_oriented{};
        bool soul{}; // Drawn after the other scene models (see render()).
        std::size_t vertex_bytes{}, index_bytes{};
    };
    std::vector<Model> models;
    std::vector<std::size_t> scene_models;
    std::optional<Model> sky;
    std::map<std::uint32_t, Pipeline> pipelines;
    struct Draw {
        WGPUBuffer vertices{}, indices{};
        WGPUBindGroup group{};
        std::uint32_t flags{};
        unsigned first{}, count{};
        bool indexed{};
        Constants constants;
        bool shadowless{}; // Casts no sun shadow (the remastered grass layer).
    };
    std::vector<Draw> draws;
    std::vector<std::byte> constants;
    // Remastered soft particles: the depth copy the particle draws read, and where those draws start (the
    // scene pass splits there so the copy can be taken).
    std::shared_ptr<WebGpuContext::Texture> depth_copy;
    std::size_t first_particle_draw{};
    explicit Impl(WebGpuContext& context) : gpu(context) {
        auto source = WGPU_SHADER_SOURCE_WGSL_INIT;
        static const auto code = tuning::shader_constants(true) + web_scene_shader;
        source.code = {code.data(), code.size()};
        auto descriptor = WGPU_SHADER_MODULE_DESCRIPTOR_INIT;
        descriptor.nextInChain = &source.chain;
        shader = wgpuDeviceCreateShaderModule(gpu.device(), &descriptor);
        WGPUBindGroupLayoutEntry frame[6]{};
        for (unsigned i = 0; i < 3; ++i) {
            auto& entry = frame[i];
            entry.visibility = WGPUShaderStage_Vertex | WGPUShaderStage_Fragment;
            entry.buffer.type = WGPUBufferBindingType_Uniform;
        }
        frame[0].binding = 0;
        frame[0].buffer.hasDynamicOffset = true;
        frame[0].buffer.minBindingSize = sizeof(Constants);
        frame[1].binding = 1;
        frame[1].buffer.minBindingSize = 1024;
        frame[2].binding = 2;
        frame[2].visibility = WGPUShaderStage_Vertex;
        frame[2].buffer.type = WGPUBufferBindingType_ReadOnlyStorage;
        frame[2].buffer.minBindingSize = 64;
        frame[3].binding = 3;
        frame[3].visibility = WGPUShaderStage_Fragment;
        frame[3].texture.sampleType = WGPUTextureSampleType_Depth;
        frame[3].texture.viewDimension = WGPUTextureViewDimension_2D;
        frame[4] = frame[3];
        frame[4].binding = 4;
        frame[5].binding = 5;
        frame[5].visibility = WGPUShaderStage_Fragment;
        frame[5].sampler.type = WGPUSamplerBindingType_Comparison;
        auto layout = WGPU_BIND_GROUP_LAYOUT_DESCRIPTOR_INIT;
        layout.entryCount = 6;
        layout.entries = frame;
        frame_layout = wgpuDeviceCreateBindGroupLayout(gpu.device(), &layout);
        WGPUBindGroupLayoutEntry material[6]{};
        for (unsigned i = 0; i < 6; ++i) {
            material[i].binding = i;
            material[i].visibility = WGPUShaderStage_Fragment;
        }
        material[0].sampler.type = WGPUSamplerBindingType_Filtering;
        for (unsigned i = 1; i < 6; ++i) {
            material[i].texture.sampleType = WGPUTextureSampleType_Float;
            material[i].texture.viewDimension =
                i >= 2 && i <= 4 ? WGPUTextureViewDimension_2DArray : WGPUTextureViewDimension_2D;
        }
        layout.entryCount = 6;
        layout.entries = material;
        material_layout = wgpuDeviceCreateBindGroupLayout(gpu.device(), &layout);
        const WGPUBindGroupLayout layouts[]{frame_layout, material_layout};
        auto pipeline = WGPU_PIPELINE_LAYOUT_DESCRIPTOR_INIT;
        pipeline.bindGroupLayoutCount = 2;
        pipeline.bindGroupLayouts = layouts;
        pipeline_layout = wgpuDeviceCreatePipelineLayout(gpu.device(), &pipeline);
        const auto depth_texture = [&](unsigned size, WGPUTextureUsage usage) {
            auto descriptor = WGPU_TEXTURE_DESCRIPTOR_INIT;
            descriptor.usage = usage;
            descriptor.size = {size, size, 1};
            descriptor.format = WGPUTextureFormat_Depth32Float;
            auto texture = std::make_shared<WebGpuContext::Texture>();
            texture->texture = wgpuDeviceCreateTexture(gpu.device(), &descriptor);
            texture->view = wgpuTextureCreateView(texture->texture, nullptr);
            return texture;
        };
        shadow_map = depth_texture(static_cast<unsigned>(tuning::shadow_map_size),
                                   WGPUTextureUsage_RenderAttachment | WGPUTextureUsage_TextureBinding);
        shadow_stand_in = depth_texture(1, WGPUTextureUsage_TextureBinding);
        {
            auto compare = WGPU_SAMPLER_DESCRIPTOR_INIT;
            compare.magFilter = compare.minFilter = WGPUFilterMode_Linear;
            compare.addressModeU = compare.addressModeV = compare.addressModeW = WGPUAddressMode_ClampToEdge;
            compare.compare = WGPUCompareFunction_LessEqual;
            shadow_sampler = wgpuDeviceCreateSampler(gpu.device(), &compare);
        }
        auto sampler = WGPU_SAMPLER_DESCRIPTOR_INIT;
        sampler.magFilter = sampler.minFilter = WGPUFilterMode_Linear;
        sampler.mipmapFilter = WGPUMipmapFilterMode_Linear;
        sampler.addressModeU = sampler.addressModeV = sampler.addressModeW = WGPUAddressMode_ClampToEdge;
        clamp = wgpuDeviceCreateSampler(gpu.device(), &sampler);
        sampler.addressModeU = sampler.addressModeV = sampler.addressModeW = WGPUAddressMode_Repeat;
        wrap = wgpuDeviceCreateSampler(gpu.device(), &sampler);
        white = gpu.upload({1, 1, {0xffffffff}});
        create_post();
        point_capacity = 1;
        point_buffer = buffer(64, WGPUBufferUsage_Storage);
    }
    Buffer buffer(std::size_t size, WGPUBufferUsage usage, const void* data = nullptr) {
        auto descriptor = WGPU_BUFFER_DESCRIPTOR_INIT;
        descriptor.size = (std::max(size, std::size_t{4}) + 3) & ~std::size_t{3};
        descriptor.usage = usage | WGPUBufferUsage_CopyDst;
        Buffer result = wgpuDeviceCreateBuffer(gpu.device(), &descriptor);
        if (data && size)
            wgpuQueueWriteBuffer(gpu.queue(), result, 0, data, size);
        return result;
    }
    void write_texture(const Texture& texture, unsigned layer, unsigned mip, unsigned size,
                       const std::vector<std::uint32_t>& pixels) {
        auto destination = WGPU_TEXEL_COPY_TEXTURE_INFO_INIT;
        destination.texture = texture->texture;
        destination.mipLevel = mip;
        destination.origin.z = layer;
        auto layout = WGPU_TEXEL_COPY_BUFFER_LAYOUT_INIT;
        layout.bytesPerRow = size * 4;
        layout.rowsPerImage = size;
        const WGPUExtent3D extent{size, size, 1};
        wgpuQueueWriteTexture(gpu.queue(), &destination, pixels.data(), pixels.size() * 4, &layout, &extent);
    }
    Texture array_texture(unsigned size, unsigned layers, unsigned mips) {
        auto descriptor = WGPU_TEXTURE_DESCRIPTOR_INIT;
        descriptor.usage = WGPUTextureUsage_TextureBinding | WGPUTextureUsage_CopyDst;
        descriptor.dimension = WGPUTextureDimension_2D;
        descriptor.size = {size, size, layers};
        descriptor.format = WGPUTextureFormat_RGBA8Unorm;
        descriptor.mipLevelCount = mips;
        auto result = std::make_shared<WebGpuContext::Texture>();
        result->texture = wgpuDeviceCreateTexture(gpu.device(), &descriptor);
        auto view = WGPU_TEXTURE_VIEW_DESCRIPTOR_INIT;
        view.dimension = WGPUTextureViewDimension_2DArray;
        result->view = wgpuTextureCreateView(result->texture, &view);
        return result;
    }
    Group group(const Texture& image, bool repeat) {
        WGPUBindGroupEntry entries[6]{};
        for (unsigned i = 0; i < 6; ++i)
            entries[i].binding = i;
        entries[0].sampler = repeat ? wrap.value : clamp.value;
        entries[1].textureView = image->view;
        entries[2].textureView = tiles->view;
        entries[3].textureView = details->view;
        entries[4].textureView = windows->view;
        entries[5].textureView = edge->view;
        auto descriptor = WGPU_BIND_GROUP_DESCRIPTOR_INIT;
        descriptor.layout = material_layout;
        descriptor.entryCount = 6;
        descriptor.entries = entries;
        return wgpuDeviceCreateBindGroup(gpu.device(), &descriptor);
    }
    Model model(const ModelAsset& source) {
        Mesh mesh;
        const auto ranges = renderer_shared::group_model(source, mesh);
        Model result;
        result.vertex_bytes = mesh.vertices.size() * sizeof(Vertex);
        result.index_bytes = mesh.indices.size() * 4;
        result.vertices = buffer(result.vertex_bytes, WGPUBufferUsage_Vertex, mesh.vertices.data());
        result.indices = buffer(result.index_bytes, WGPUBufferUsage_Index, mesh.indices.data());
        result.signatures = source.mesh.materials;
        result.topology = source.mesh.indices;
        result.triangle_materials = source.mesh.triangle_materials;
        if (!mesh.vertices.empty()) {
            result.reveal_min = result.reveal_max = mesh.vertices.front().position.y;
            for (const auto& vertex : mesh.vertices) {
                result.reveal_min = std::min(result.reveal_min, vertex.position.y);
                result.reveal_max = std::max(result.reveal_max, vertex.position.y);
            }
        }
        for (const auto& vertex : mesh.vertices)
            result.detail_radius =
                std::max(result.detail_radius, std::sqrt(vertex.position.x * vertex.position.x +
                                                         vertex.position.y * vertex.position.y +
                                                         vertex.position.z * vertex.position.z));
        for (std::size_t i = 0; i < ranges.size(); ++i) {
            auto texture = source.textures[i] ? model_texture(*source.textures[i]) : white;
            result.materials.push_back({ranges[i], group(texture, true), texture});
        }
        return result;
    }
    Texture model_texture(const ModelAsset::Texture& source) {
        const auto& image = source.image;
        if (!image.width || !image.height || image.rgba.size() != std::size_t(image.width) * image.height)
            throw Error("Invalid WebGPU model texture");
        const unsigned mips =
            source.header.flags & 0x18 ? 1u : std::bit_width(std::max(image.width, image.height));
        auto descriptor = WGPU_TEXTURE_DESCRIPTOR_INIT;
        descriptor.usage = WGPUTextureUsage_TextureBinding | WGPUTextureUsage_CopyDst;
        descriptor.dimension = WGPUTextureDimension_2D;
        descriptor.format = WGPUTextureFormat_RGBA8Unorm;
        descriptor.size = {image.width, image.height, 1};
        descriptor.mipLevelCount = mips;
        auto result = std::make_shared<WebGpuContext::Texture>();
        result->texture = wgpuDeviceCreateTexture(gpu.device(), &descriptor);
        Image level = image;
        for (unsigned mip = 0; mip < mips; ++mip) {
            auto destination = WGPU_TEXEL_COPY_TEXTURE_INFO_INIT;
            destination.texture = result->texture;
            destination.mipLevel = mip;
            auto layout = WGPU_TEXEL_COPY_BUFFER_LAYOUT_INIT;
            layout.bytesPerRow = level.width * 4;
            layout.rowsPerImage = level.height;
            const WGPUExtent3D extent{level.width, level.height, 1};
            wgpuQueueWriteTexture(gpu.queue(), &destination, level.rgba.data(), level.rgba.size() * 4,
                                  &layout, &extent);
            if (mip + 1 == mips)
                break;
            Image next{std::max(1u, level.width / 2), std::max(1u, level.height / 2), {}};
            next.rgba.resize(std::size_t(next.width) * next.height);
            for (unsigned y = 0; y < next.height; ++y)
                for (unsigned x = 0; x < next.width; ++x) {
                    std::uint32_t color{};
                    for (unsigned c = 0; c < 32; c += 8) {
                        unsigned sum{};
                        for (unsigned dy = 0; dy < 2; ++dy)
                            for (unsigned dx = 0; dx < 2; ++dx)
                                sum += level.rgba[std::min(2 * y + dy, level.height - 1) * level.width +
                                                  std::min(2 * x + dx, level.width - 1)] >>
                                           c &
                                       255;
                        color |= ((sum + 2) / 4) << c;
                    }
                    next.rgba[y * next.width + x] = color;
                }
            level = std::move(next);
        }
        result->view = wgpuTextureCreateView(result->texture, nullptr);
        return result;
    }
    WGPURenderPipeline pipeline(std::uint32_t flags, bool shadow = false) {
        // Bit 0 keys the remastered half-float target's variant; all ones is the sun shadow pass.
        const auto key = shadow ? 0xffffffffu : (flags & (0x7f00u | 0x0e000000u)) | (remastered ? 1u : 0u);
        if (const auto found = pipelines.find(key); found != pipelines.end())
            return found->second;
        const auto mode = flags & 0x7f00;
        if (mode && mode != 0x400 && mode != 0x1000 && mode != 0x2000)
            throw Error("Unsupported WebGPU material blend mode");
        WGPUVertexAttribute attributes[5]{};
        const WGPUVertexFormat formats[]{WGPUVertexFormat_Float32x3, WGPUVertexFormat_Float32x3,
                                         WGPUVertexFormat_Float32x2, WGPUVertexFormat_Unorm8x4,
                                         WGPUVertexFormat_Uint32};
        const unsigned offsets[]{0, 12, 24, 32, 36};
        for (unsigned i = 0; i < 5; ++i) {
            attributes[i].format = formats[i];
            attributes[i].offset = offsets[i];
            attributes[i].shaderLocation = i;
        }
        auto vertices = WGPU_VERTEX_BUFFER_LAYOUT_INIT;
        vertices.arrayStride = sizeof(Vertex);
        vertices.attributeCount = 5;
        vertices.attributes = attributes;
        auto blend = WGPU_BLEND_STATE_INIT;
        blend.color.srcFactor = WGPUBlendFactor_SrcAlpha;
        blend.color.dstFactor = mode == 0x2000 ? WGPUBlendFactor_One : WGPUBlendFactor_OneMinusSrcAlpha;
        blend.alpha.srcFactor = WGPUBlendFactor_One;
        blend.alpha.dstFactor = blend.color.dstFactor;
        if (mode == 0x400) {
            blend.color.srcFactor = blend.alpha.srcFactor = WGPUBlendFactor_Zero;
            blend.color.dstFactor = WGPUBlendFactor_OneMinusSrc;
            blend.alpha.dstFactor = WGPUBlendFactor_One;
        }
        auto color = WGPU_COLOR_TARGET_STATE_INIT;
        color.format = remastered ? WGPUTextureFormat_RGBA16Float : gpu.format();
        color.blend = mode ? &blend : nullptr;
        auto fragment = WGPU_FRAGMENT_STATE_INIT;
        fragment.module = shader;
        fragment.entryPoint = shadow ? WGPUStringView{"ps_shadow", 9} : WGPUStringView{"ps_main", 7};
        fragment.targetCount = shadow ? 0 : 1;
        fragment.targets = &color;
        auto depth_state = WGPU_DEPTH_STENCIL_STATE_INIT;
        depth_state.format = WGPUTextureFormat_Depth32Float;
        depth_state.depthWriteEnabled = flags & 0x06000000 ? WGPUOptionalBool_False : WGPUOptionalBool_True;
        depth_state.depthCompare =
            flags & 0x04000000 ? WGPUCompareFunction_Always : WGPUCompareFunction_GreaterEqual;
        if (shadow) { // Ordinary depth (near 0) from the sun, biased along steep slopes.
            depth_state.depthWriteEnabled = WGPUOptionalBool_True;
            depth_state.depthCompare = WGPUCompareFunction_LessEqual;
            depth_state.depthBiasSlopeScale = 1.5f;
        }
        auto descriptor = WGPU_RENDER_PIPELINE_DESCRIPTOR_INIT;
        descriptor.layout = pipeline_layout;
        descriptor.vertex.module = shader;
        descriptor.vertex.entryPoint = shadow ? WGPUStringView{"vs_shadow", 9} : WGPUStringView{"vs_main", 7};
        descriptor.vertex.bufferCount = 1;
        descriptor.vertex.buffers = &vertices;
        descriptor.primitive.topology = WGPUPrimitiveTopology_TriangleList;
        // Both backends classify winding in framebuffer coordinates. Match D3D's clockwise fronts.
        descriptor.primitive.frontFace = WGPUFrontFace_CW;
        descriptor.primitive.cullMode = shadow || flags & 0x08000000 ? WGPUCullMode_None : WGPUCullMode_Back;
        descriptor.depthStencil = &depth_state;
        descriptor.fragment = &fragment;
        auto [entry, inserted] =
            pipelines.emplace(key, Pipeline(wgpuDeviceCreateRenderPipeline(gpu.device(), &descriptor)));
        return entry->second;
    }
    void append(Model& model, Constants base, bool unlit = false) {
        for (std::size_t i = 0; i < model.worlds.size(); ++i) {
            if (!model.visible.empty() && !model.visible[i])
                continue;
            base.world = model.worlds[i];
            const float reveal = i < model.reveal.size() ? model.reveal[i] : 1;
            const float opacity = i < model.opacity.size() ? model.opacity[i] : 1;
            model.reveal_blend.resize(model.worlds.size());
            if (reveal != 1)
                model.reveal_blend[i] = true;
            base.model_reveal = {model.reveal_min, model.reveal_max, reveal,
                                 model.reveal_blend[i] ? 1.f : 0.f};
            for (auto& material : model.materials) {
                if (!material.range.count)
                    continue;
                auto flags = material.range.flags;
                if (base.model_reveal.w > 0)
                    flags |= flags & 0x2000 ? 8 : 0x1008;
                if (opacity < 1)
                    flags = (flags & ~0x7f00u) | 0x1008;
                base.material = {flags, 1, unlit ? 1u : 0u, model.field ? 1u : 0u};
                base.options = {model.texture_override || material.texture != white ? 1.f : 0.f,
                                model.grayscale ? 1.f : 0.f, 0, opacity};
                const auto group = model.texture_override ? effect_group(model.texture_override->view)
                                                          : material.group.value;
                draws.push_back({model.vertices, model.indices, material.group, material.range.flags,
                                 material.range.first, material.range.count, true, base});
                draws.back().group = group;
                draws.back().flags = flags;
            }
        }
    }
    EffectImage& effect_image(FourCC id) {
        auto [entry, inserted] = effect_images.try_emplace(id);
        if (inserted && id.value >= effect_land_edge) {
            if (scene.environment)
                if (const auto edge = load_edge_texture(*catalog, *scene.environment, terrain.tileset)) {
                    entry->second.header = edge->header;
                    entry->second.texture = gpu.upload(edge->image);
                    entry->second.group = group(entry->second.texture, true);
                }
        } else if (inserted && id.value >= effect_land_tile) {
            const auto cell = id.value - effect_land_tile;
            if (cell < terrain.tiles.size()) {
                const auto layer = !terrain.scar_layers.empty() && terrain.scar_layers[cell]
                                       ? terrain.scar_layers[cell]
                                       : terrain.tiles[cell];
                const auto* tile = layer < 256 ? (materials.tiles[layer] ? &*materials.tiles[layer] : nullptr)
                                   : layer - 256 < materials.scars.size() ? &materials.scars[layer - 256]
                                                                          : nullptr;
                if (tile) {
                    auto& header = entry->second.header;
                    header.width = header.frame_width = tile->mipmaps[0].width;
                    header.height = header.frame_height = tile->mipmaps[0].height;
                    header.frames = 1;
                    entry->second.texture = gpu.upload(tile->mipmaps[0]);
                    entry->second.group = group(entry->second.texture, true);
                }
            }
        } else if (inserted && id.value) {
            if (const auto location = catalog->find_chunk(FourCC::from_text("TXTR"), id)) {
                entry->second.header = decode_texture_header(catalog->read(*location));
                entry->second.texture = gpu.upload(load_texture(*catalog, *location));
                entry->second.group = group(entry->second.texture, true);
            }
        }
        return entry->second;
    }
    WGPUBindGroup effect_group(WGPUTextureView view) const {
        if (!view)
            return land_group;
        for (const auto& [id, image] : effect_images)
            if (image.texture && image.texture->view == view)
                return image.group;
        throw Error("Effect texture owner is missing");
    }
    static XMFLOAT4X4 world(const ObjectMotion& motion, float scale = 1) {
        const auto& m = motion.orientation;
        const auto& v = motion.position;
        return {m[0] * scale, m[2] * scale, m[1] * scale, 0, m[6] * scale, m[8] * scale, m[7] * scale, 0,
                m[3] * scale, m[5] * scale, m[4] * scale, 0, v.x,          v.z,          v.y,          1};
    }
    void update_mesh(Model& model, const Mesh& mesh) {
        if (mesh.materials != model.signatures)
            throw Error("Dynamic WebGPU model materials changed");
        const auto bytes = mesh.vertices.size() * sizeof(Vertex);
        if (bytes > model.vertex_bytes) {
            model.vertex_bytes = bytes * 2;
            model.vertices = buffer(model.vertex_bytes, WGPUBufferUsage_Vertex);
        }
        if (bytes)
            wgpuQueueWriteBuffer(gpu.queue(), model.vertices, 0, mesh.vertices.data(), bytes);
        if (model.topology != mesh.indices || model.triangle_materials != mesh.triangle_materials) {
            ModelAsset source;
            source.mesh = mesh;
            source.textures.resize(mesh.materials.size());
            Mesh grouped;
            const auto ranges = renderer_shared::group_model(source, grouped);
            const auto index_bytes = grouped.indices.size() * 4;
            if (index_bytes > model.index_bytes) {
                model.index_bytes = index_bytes * 2;
                model.indices = buffer(model.index_bytes, WGPUBufferUsage_Index);
            }
            if (index_bytes)
                wgpuQueueWriteBuffer(gpu.queue(), model.indices, 0, grouped.indices.data(), index_bytes);
            for (std::size_t i = 0; i < ranges.size(); ++i)
                model.materials[i].range = ranges[i];
            model.topology = mesh.indices;
            model.triangle_materials = mesh.triangle_materials;
        }
    }
    void filter(int value) {
        if (texture_filter == value)
            return;
        texture_filter = value;
        auto sampler = WGPU_SAMPLER_DESCRIPTOR_INIT;
        sampler.magFilter = sampler.minFilter = value ? WGPUFilterMode_Linear : WGPUFilterMode_Nearest;
        sampler.mipmapFilter = value == 2 ? WGPUMipmapFilterMode_Linear : WGPUMipmapFilterMode_Nearest;
        sampler.addressModeU = sampler.addressModeV = sampler.addressModeW = WGPUAddressMode_ClampToEdge;
        clamp = wgpuDeviceCreateSampler(gpu.device(), &sampler);
        sampler.addressModeU = sampler.addressModeV = sampler.addressModeW = WGPUAddressMode_Repeat;
        wrap = wgpuDeviceCreateSampler(gpu.device(), &sampler);
        land_group = group(white, false);
        const auto rebind = [&](Model& model) {
            for (auto& material : model.materials)
                material.group = group(material.texture, true);
        };
        for (auto& model : models)
            rebind(model);
        if (sky)
            rebind(*sky);
        for (auto& [id, image] : effect_images)
            if (image.texture)
                image.group = group(image.texture, true);
    }
    void model_detail(const Camera& camera) {
        const Vec3 forward{std::sin(camera.yaw) * std::cos(camera.pitch), std::sin(camera.pitch),
                           std::cos(camera.yaw) * std::cos(camera.pitch)};
        for (std::size_t i = 0; i < scene.models.size(); ++i) {
            const auto& source = scene.models[i];
            const auto& geometry = source.asset.geometry;
            if (geometry.frames.empty() || scene_models[i] == SIZE_MAX)
                continue;
            auto& model = models[scene_models[i]];
            if (model.worlds.empty())
                continue;
            float quality = 0;
            for (const auto& world : model.worlds) {
                const float scale =
                    std::sqrt(world._11 * world._11 + world._12 * world._12 + world._13 * world._13);
                const float depth = (world._41 - camera.position.x) * forward.x +
                                    (world._42 - camera.position.y) * forward.y +
                                    (world._43 - camera.position.z) * forward.z;
                quality = std::max(quality, detail.quality(depth - model.detail_radius * scale,
                                                           model.detail_radius * scale));
            }
            const unsigned maximum = static_cast<unsigned>(geometry.frames[0].size());
            const unsigned vertices = geometry.minimum_vertices +
                                      static_cast<unsigned>((maximum - geometry.minimum_vertices) * quality);
            if (vertices == model.lod_vertices && model.frame == model.lod_frame)
                continue;
            model.lod_vertices = vertices;
            model.lod_frame = model.frame;
            const float frame = std::clamp(model.frame, 0.f, float(geometry.frames.size() - 1));
            const auto from = static_cast<unsigned>(frame);
            update_mesh(model, geometry.sample(from, std::min(from + 1, unsigned(geometry.frames.size() - 1)),
                                               frame - from, vertices));
        }
    }
    void sync_models() {
        simulation->prepare_models();
        animated = 0;
        while (scene_models.size() < scene.models.size())
            scene_models.push_back(SIZE_MAX);
        for (std::size_t i = 0; i < scene.models.size(); ++i) {
            auto& source = scene.models[i];
            if (scene_models[i] == SIZE_MAX) {
                if (source.asset.mesh.indices.empty() && !source.widget_field && !source.character &&
                    !source.summon)
                    continue;
                scene_models[i] = models.size();
                models.push_back(model(source.asset));
            }
            auto& target = models[scene_models[i]];
            target.visible = source.visible;
            target.reveal = source.reveal;
            target.field = source.widget_field;
            target.fade_flags = source.widget_fade_flags;
            target.screen_oriented = source.screen_oriented;
            target.soul = source.soul.has_value();
            target.worlds.clear();
            for (const auto& placement : source.placements) {
                const float k = placement.scale, c = std::cos(placement.yaw) * k,
                            s = std::sin(placement.yaw) * k;
                target.worlds.push_back({c, 0, s, 0, 0, k, 0, 0, -s, 0, c, 0, placement.position.x,
                                         placement.position.y, placement.position.z, 1});
            }
            target.opacity.assign(target.worlds.size(), 1);
            target.texture_override.reset();
            target.grayscale = false;
            if (source.character) {
                auto& character = *source.character;
                auto* actor = character.object;
                target.visible.assign(1, actor && !(actor->state_flags & MatchObject::removed_flag));
                if (actor) {
                    target.worlds.assign(1, world(simulation->match().presented(*actor)));
                    target.opacity.assign(
                        1, static_cast<unsigned>((1.0 - double(actor->fade) * actor->fade) * 255) / 255.f);
                    target.grayscale = actor->grayscale;
                    if (actor->texture_override.value)
                        target.texture_override = effect_image(actor->texture_override).texture;
                    if (actor->clip() && target.revision != actor->pose_revision()) {
                        const auto& body = *character.body;
                        update_mesh(target, saxs_mesh(body.model, body.descriptor, *character.topology,
                                                      actor->model_pose()));
                        target.revision = actor->pose_revision();
                    }
                    ++animated;
                }
            }
            if (source.soul && !source.asset.textures.empty()) {
                // Drawn between ticks like the other objects; a bundle's ring offset rides along.
                if (const auto* soul = source.soul_object; soul && !target.worlds.empty()) {
                    const auto shown = simulation->match().presented(*soul).position;
                    const auto& now = soul->motion.position;
                    auto& w = target.worlds.front();
                    w._41 += shown.x - now.x;
                    w._42 += shown.z - now.z;
                    w._43 += shown.y - now.y;
                }
                auto mesh = soul_mesh(*source.soul, source.asset.textures.front()->header);
                // FLAG generates geometry only; its instance retains the soul material and texture.
                mesh.materials = source.asset.mesh.materials;
                update_mesh(target, mesh);
                ++animated;
            }
            if (source.spell_visual) {
                const auto* spell = source.spell_visual->object;
                target.visible.assign(1, spell && !(spell->state_flags & MatchObject::removed_flag));
                if (spell) {
                    target.worlds.assign(1, world(simulation->match().presented(*spell), spell->model_scale));
                    if (spell->effect == SpellActor::Effect::death)
                        target.opacity.assign(1, std::clamp(spell->cast_alpha * 2, 0.f, 1.f));
                    const auto& geometry = source.asset.geometry;
                    if (!geometry.frames.empty() && target.frame != spell->model_frame) {
                        const float frame =
                            std::clamp(spell->model_frame, 0.f, float(geometry.frames.size() - 1));
                        const auto from = static_cast<unsigned>(frame);
                        update_mesh(target,
                                    geometry.sample(from,
                                                    std::min(from + 1, unsigned(geometry.frames.size() - 1)),
                                                    frame - from));
                        target.frame = spell->model_frame;
                    }
                    ++animated;
                }
            }
            if (source.summon) {
                auto& summon = *source.summon;
                const auto* spell = summon.object;
                const bool echo = spell && spell->effect == SpellActor::Effect::speedup_echo;
                target.visible.assign(1, spell && !(spell->state_flags & MatchObject::removed_flag) &&
                                             (!echo || !spell->echo.hidden));
                if (spell) {
                    target.worlds.assign(1, world(simulation->match().presented(*spell)));
                    target.opacity.assign(1, std::clamp(int(spell->cast_alpha * 255), 0, 255) / 255.f);
                    const auto* clip = echo ? spell->echo.clip.get() : summon.clip.get();
                    const auto revision = echo ? spell->echo.revision : 1;
                    if (clip && target.revision != revision) {
                        const auto& body = *summon.body;
                        update_mesh(target, saxs_mesh(body.model, body.descriptor, *summon.topology,
                                                      saxs_pose(body.model, clip->animation,
                                                                echo ? float(spell->echo.frame) : 0)));
                        target.revision = revision;
                    }
                    ++animated;
                }
            }
        }
        for (const auto& spawn : scene.spawns)
            if (spawn.kind == MapScene::Spawn::Kind::building && spawn.object) {
                const auto& actor = static_cast<const BuildingActor&>(*spawn.object);
                for (unsigned state = 0; state < 3; ++state)
                    if (spawn.state_models[state] != SIZE_MAX) {
                        const auto mapped = scene_models[spawn.state_models[state]];
                        if (mapped != SIZE_MAX)
                            models[mapped].worlds.at(spawn.state_placements[state]) =
                                world(simulation->match().presented(actor), actor.model_scale);
                    }
            }
        if (sky && sky_uploaded != simulation->match().clock().total) {
            const auto mesh = sky_mesh(*scene.environment, sky_textures, sky_state);
            const auto bytes = mesh.vertices.size() * sizeof(Vertex);
            if (bytes <= sky->vertex_bytes && bytes)
                wgpuQueueWriteBuffer(gpu.queue(), sky->vertices, 0, mesh.vertices.data(), bytes);
            sky_uploaded = simulation->match().clock().total;
        }
    }
};
WebSceneRenderer::WebSceneRenderer(WebGpuContext& gpu) : impl_(std::make_unique<Impl>(gpu)) {}
WebSceneRenderer::~WebSceneRenderer() = default;
void WebSceneRenderer::load(const ResourceCatalog& catalog, const Archive& map,
                            std::span<const SkirmishSlot> session, bool campaign,
                            std::optional<std::uint32_t> seed, const Character* character) {
    auto& p = *impl_;
    p.catalog = &catalog;
    p.terrain = Terrain::load(map);
    p.materials = TerrainMaterials::load(catalog, p.terrain.tileset);
    auto& materials = p.materials;
    materials.create_tile_map(p.terrain);
    materials.validate(p.terrain);
    p.scene = MapScene::compose(catalog, map, p.terrain);
    p.land.set_clearings(p.scene.clearings);
    apply_scars(catalog, map, p.terrain, materials, p.scene.unresolved);
    std::vector<SkirmishSlot> players;
    if (campaign)
        players = {{SkirmishSlot::Kind::local, FourCC::from_text("cwe2"), 1}};
    else if (!session.empty())
        players.assign(session.begin(), session.end());
    else if (!p.scene.wizards) {
        for (unsigned slot = 1; slot <= 5; ++slot)
            if (std::ranges::any_of(p.scene.sides, [&](const SideDefinition& side) {
                    const auto nibble = side.flags & 0xf ? side.flags & 0xf : (side.flags & 0x20 ? 1u : 0u);
                    return nibble == slot;
                }))
                players.resize(slot, {SkirmishSlot::Kind::none, FourCC::from_text("cwe2"), 1});
        if (!players.empty())
            players.front().kind = SkirmishSlot::Kind::local;
    }
    if (seed)
        p.random.state = *seed;
    p.simulation = std::make_unique<SceneMatch>(p.scene, p.terrain, p.random, &materials, &catalog, players,
                                                campaign, character);
    for (const auto& change : p.simulation->tile_changes()) {
        materials.release_scar(p.terrain, change.cell);
        p.terrain.tiles.at(change.cell) = change.tile;
    }
    p.applied_tiles = p.simulation->tile_changes().size();
    p.land_revision = p.simulation->match().terrain_revision;
    bake_terrain_lighting(p.terrain, materials, p.scene, map, catalog);
    p.land.update_tiles(materials, {});
    p.land.reset(p.terrain);
    p.fallback_count = 0;
    if (!p.land.lod) {
        const auto mesh = p.terrain.mesh(true);
        p.land_vertices =
            p.buffer(mesh.vertices.size() * sizeof(Vertex), WGPUBufferUsage_Vertex, mesh.vertices.data());
        p.fallback_indices = p.buffer(mesh.indices.size() * 4, WGPUBufferUsage_Index, mesh.indices.data());
        p.fallback_count = static_cast<unsigned>(mesh.indices.size());
    }
    const unsigned layers = 256 + static_cast<unsigned>(materials.scars.size());
    p.tiles = p.array_texture(64, 256 + materials.scar_limit, 7);
    for (unsigned tile = 0; tile < layers; ++tile)
        for (unsigned mip = 0; mip < 7; ++mip) {
            const auto* source = tile < 256 ? (materials.tiles[tile] ? &*materials.tiles[tile] : nullptr)
                                            : &materials.scars[tile - 256];
            const auto size = 64u >> mip;
            p.write_texture(p.tiles, tile, mip, size,
                            source ? source->mipmaps[mip].rgba
                                   : std::vector<std::uint32_t>(size * size, 0xff000000));
        }
    p.details = p.array_texture(256, 16, 9);
    const auto mips = renderer_shared::detail_mips(materials);
    for (unsigned i = 0; i < mips.size(); ++i)
        p.write_texture(p.details, i / 9, i % 9, 256u >> (i % 9), mips[i]);
    std::array<std::uint32_t, 256> slots{};
    std::ranges::copy(materials.tile_details, slots.begin());
    p.slots = p.buffer(sizeof(slots), WGPUBufferUsage_Uniform, slots.data());
    p.windows = p.array_texture(256, 6, 1);
    p.edge = p.white;
    p.has_edge = false;
    if (p.scene.environment)
        if (const auto edge = load_edge_texture(catalog, *p.scene.environment, p.terrain.tileset)) {
            p.edge = p.gpu.upload(edge->image);
            p.has_edge = true;
        }
    p.land_group = p.group(p.white, false);
    p.models.clear();
    p.scene_models.clear();
    p.sky.reset();
    if (p.scene.environment) {
        std::array<Image, 5> frames;
        for (unsigned slot = 0; slot < 5; ++slot)
            if (!p.scene.environment->sky_videos[slot].empty()) {
                p.sky_videos[slot] = std::make_unique<VideoClip>(discover_game_dir({}) /
                                                                 p.scene.environment->sky_videos[slot]);
                p.sky_videos[slot]->next(frames[slot]);
            }
        p.sky_textures = load_sky_textures(catalog, *p.scene.environment, p.terrain.tileset, frames);
        for (unsigned slot = 0; slot < 5; ++slot)
            if (p.sky_videos[slot] && p.sky_textures[slot])
                p.sky_textures[slot]->header.flags |= 0x18;
        auto sky = build_sky(*p.scene.environment, p.sky_textures, p.sky_state);
        if (!sky.mesh.indices.empty()) {
            p.sky = p.model(sky);
            p.sky->worlds.push_back(identity());
        }
    }
    p.sync_models();
}
void WebSceneRenderer::options(const GameOptions& options, float fps) {
    auto& p = *impl_;
    p.detail = p.detail_control.update(fps, options.detail_target(),
                                       options.value(FourCC::from_text("gdet")), ticks() <= 10);
    widgets = options.value(FourCC::from_text("widj")) != 0;
    terrain_detail = options.value(FourCC::from_text("bmap")) ? 2 : options.value(FourCC::from_text("dmap")) ? 1 : 0;
    p.filter(options.value(FourCC::from_text("tfil")));
    auto& match = p.simulation->match();
    match.projectile_friendly_fire = options.value(FourCC::from_text("ffir")) != 0;
    match.interpolate = options.value(FourCC::from_text("smth")) != 0;
    p.shake_enabled = options.value(FourCC::from_text("shak")) != 0;
    p.remastered = options.value(FourCC::from_text("rmst")) != 0;
    match.friendly_damage_scale = static_cast<std::uint8_t>(options.value(FourCC::from_text("fdam")));
}
void WebSceneRenderer::advance(float seconds, bool single_step) {
    auto& p = *impl_;
    if (!p.simulation || (paused && !single_step))
        return;
    auto& match = p.simulation->match();
    const int ticks = single_step ? match.tick(1) : match.advance(seconds * 60);
    p.sky_state.advance(ticks);
    for (unsigned slot = 0; slot < 5; ++slot) {
        auto& clip = p.sky_videos[slot];
        if (!clip)
            continue;
        Image image;
        if (!advance_movie(clip, discover_game_dir({}) / p.scene.environment->sky_videos[slot],
                           p.sky_video_time[slot], seconds, image))
            continue;
        if (p.sky)
            for (unsigned material = 0; material < p.sky->signatures.size(); ++material)
                if (p.sky->signatures[material].texture.value == 0x736b7930u + slot)
                    p.gpu.update(*p.sky->materials[material].texture, image);
    }
    // Audio queues are consumed by the frontend after the tick.
    if (p.shake_enabled)
        for (const auto& request : match.shake_requests)
            p.shake.request(p.shake_eye, request.position, request.amplitude, request.radius);
    match.shake_requests.clear();
    p.shake.update(ticks);
    for (const auto& object : match.objects()) {
        if (auto* actor = dynamic_cast<CreatureActor*>(object.get())) {
            actor->spell_requests.clear();
            actor->death_requests.clear();
            actor->fall_damage.clear();
            std::erase_if(actor->combat_requests, [](const auto& request) {
                return request.kind != CreatureActor::CombatRequest::Kind::sound;
            });
            actor->burn_requests.clear();
            actor->order_requests.clear();
            actor->credit_requests.clear();
            actor->level_requests.clear();
            actor->grant_requests.clear();
            actor->cast_visual_requests.clear();
        }
        if (auto* building = dynamic_cast<BuildingActor*>(object.get())) {
            building->death_requests.clear();
        }
    }
    const bool changed =
        apply_terrain_changes(*p.catalog, *p.simulation, p.terrain, p.materials, p.applied_tiles, p.scars);
    if (changed) {
        // Only the scar layers whose texels changed are written again (the array holds the whole pool).
        for (const auto tile : p.land.update_tiles(p.materials, p.terrain, false))
            for (unsigned mip = 0; mip < 7; ++mip) {
                const auto* source = tile < 256
                                         ? (p.materials.tiles[tile] ? &*p.materials.tiles[tile] : nullptr)
                                         : &p.materials.scars[tile - 256];
                if (source)
                    p.write_texture(p.tiles, static_cast<unsigned>(tile), mip, 64u >> mip, source->mipmaps[mip].rgba);
            }
        // Falling land blocks look their cells' textures up again (scars move layers).
        std::erase_if(p.effect_images, [](const auto& entry) {
            return entry.first.value >= effect_land_tile && entry.first.value < effect_land_edge;
        });
    }
    if (changed || p.land_revision != match.terrain_revision) {
        p.land_revision = match.terrain_revision;
        if (!p.land.refresh(p.terrain))
            p.land.reset(p.terrain);
        if (!p.land.lod) {
            const auto mesh = p.terrain.mesh(true);
            p.land_vertices =
                p.buffer(mesh.vertices.size() * sizeof(Vertex), WGPUBufferUsage_Vertex, mesh.vertices.data());
            p.fallback_indices =
                p.buffer(mesh.indices.size() * 4, WGPUBufferUsage_Index, mesh.indices.data());
            p.fallback_count = static_cast<unsigned>(mesh.indices.size());
        }
    }
}
void WebSceneRenderer::prepare(const Camera& camera, unsigned width, unsigned height) {
    auto& p = *impl_;
    p.draws.clear();
    p.shadows = false;
    if (!width || !height)
        return;
    p.sync_models();
    p.effect_batches.clear();
    p.lights.clear();
    if (effects && !p.remastered) // The Remastered Look's sun shadows replace the native blob shadows.
        append_creature_shadows(p.simulation->match(), camera, p.detail, p.effect_batches);
    if (p.remastered)
        update_weather_look(p.weather, p.simulation->match());
    else
        p.weather = {};
    if (effects)
        append_match_effects(
            p.simulation->match(), camera,
            [&](FourCC id) {
                const auto& image = p.effect_image(id);
                return EffectTexture{image.header, image.texture ? image.texture->view : nullptr};
            },
            p.effect_batches, p.lights, p.remastered);
    if (effects && !p.order_markers.empty()) {
        const auto& image = p.effect_image(FourCC::from_text("aura"));
        if (image.texture)
            for (const auto& marker : p.order_markers) {
                ParticleBatch batch;
                batch.flags = 0x1a012009;
                batch.texture = image.texture->view;
                batch.mesh = order_marker_mesh(marker, image.header,
                                               {camera.position.x, camera.position.z, camera.position.y});
                p.effect_batches.push_back(std::move(batch));
            }
    }
    const auto eye = XMVectorSet(camera.position.x, camera.position.y, camera.position.z, 1);
    const auto direction = XMVectorSet(std::sin(camera.yaw) * std::cos(camera.pitch), std::sin(camera.pitch),
                                       std::cos(camera.yaw) * std::cos(camera.pitch), 0);
    const auto view = XMMatrixLookToLH(eye, direction, XMVectorSet(0, 1, 0, 0));
    const float aspect = float(height) / width, half_width = 45 * std::min(1.f, .75f / aspect),
                half_height = 45 * std::min(aspect, .75f);
    const float near = .1f, tw = std::tan(XMConvertToRadians(half_width)) * near,
                th = std::tan(XMConvertToRadians(half_height)) * near;
    p.view_tan[0] = tw / near;
    p.view_tan[1] = th / near;
    p.shake_eye = {camera.position.x, camera.position.z, camera.position.y};
    auto centre = camera.centre_offset;
    if (p.shake_enabled) {
        const auto shake = p.shake.offset(p.shake_random);
        centre = {centre.x + shake.x, centre.y + shake.y};
    } else
        p.shake = {};
    // The view centre moves by whole pixels, as in the D3D11 renderer.
    const float sx = centre.x * 2 * tw / float(width), sy = centre.y * 2 * th / float(height);
    // Reversed-Z float depth as in the D3D11 renderer (z' = w - z: 1 at the near plane, 0 at the far plane).
    const auto projection = XMMatrixPerspectiveOffCenterLH(-tw - sx, tw - sx, -th + sy, th + sy, near, 30000) *
                            XMMATRIX(1, 0, 0, 0, 0, 1, 0, 0, 0, 0, -1, 0, 0, 0, 1, 1);
    Constants c{};
    // WGSL matrices consume the stored row vectors as columns: M * v equals D3D's v * M.
    XMStoreFloat4x4(&c.view_projection, view * projection);
    c.world = identity();
    c.eye = {camera.position.x, camera.position.y, camera.position.z, 1};
    struct PointSource {
        XMFLOAT4 position_range, color, land_position_range, land_color;
    };
    std::vector<PointSource> points;
    for (const auto& light : p.lights) {
        if (!light.enabled || !(light.outer > 0) || !(light.intensity > 0))
            continue;
        const auto land = light.for_terrain(
            static_cast<float>(terrain_true_height(p.terrain, light.position.x, light.position.y)));
        const auto position = [](const PointLight& l) {
            return XMFLOAT4{l.position.x, l.position.z, l.position.y, l.outer};
        };
        const auto color = [](const PointLight& l) {
            return XMFLOAT4{l.color.x * l.intensity, l.color.y * l.intensity, l.color.z * l.intensity, 0};
        };
        points.push_back({position(light), color(light), position(land), color(land)});
    }
    if (points.size() > p.point_capacity) {
        p.point_capacity = std::max(points.size(), p.point_capacity * 2);
        p.point_buffer = p.buffer(p.point_capacity * sizeof(PointSource), WGPUBufferUsage_Storage);
        p.frame_dirty = true;
    }
    if (!points.empty())
        wgpuQueueWriteBuffer(p.gpu.queue(), p.point_buffer, 0, points.data(),
                             points.size() * sizeof(PointSource));
    c.point_options.x = static_cast<unsigned>(points.size());
    XMStoreFloat4(&c.view_dir, XMVector3Normalize(direction));
    if (environment && p.scene.environment) {
        auto e = *p.scene.environment;
        p.simulation->match().weather.fog(e.fog_start, e.fog_end, e.fog_color);
        c.fog = {e.fog_start, e.fog_end, 1, e.fog_mode ? 1.f : 0.f};
        c.view_dir.w = e.fog_density;
        c.fog_color = unpack_color(e.fog_color);
        c.ambient = {e.ambient.x * e.ambient_intensity, e.ambient.y * e.ambient_intensity,
                     e.ambient.z * e.ambient_intensity, 1};
        XMStoreFloat4(&c.sun_direction, XMVector3Normalize(XMVectorSet(e.sun_direction.x, e.sun_direction.z,
                                                                       e.sun_direction.y, 0)));
        c.sun_color = {e.sun_color.x * e.sun_intensity, e.sun_color.y * e.sun_intensity,
                       e.sun_color.z * e.sun_intensity, 1};
        if (p.remastered) {
            const auto fog = renderer_shared::remaster_constants(&p.terrain);
            c.remaster = {fog.enabled, fog.sky, fog.floor, fog.falloff};
            c.weather = {p.weather.wet, p.weather.snow, p.weather.storm, p.weather.flash};
            c.wind = renderer_shared::wind_constants(p.weather);
            const float ground = terrain_height(p.terrain, camera.position.x, camera.position.z);
            XMStoreFloat4x4(&c.shadow_matrix, renderer_shared::sun_shadow_matrix(c.sun_direction, camera, ground));
            c.shadow = renderer_shared::shadow_constants();
            p.shadows = true;
        }
        if (p.sky) {
            auto sky = c;
            sky.remaster.y = sky.remaster.x; // Sky layers take the remastered haze and sun glow.
            p.append(*p.sky, sky, true);
        }
    }
    p.land.range = p.remastered ? tuning::terrain_detail_range : renderer_shared::detail_range;
    if (p.land.update(camera, half_width, half_height, p.has_edge)) {
        const auto bytes = p.land.lod_faces.size() * sizeof(Vertex);
        if (bytes > p.land_capacity) {
            p.land_capacity = bytes * 2;
            p.land_vertices = p.buffer(p.land_capacity, WGPUBufferUsage_Vertex);
        }
        if (bytes)
            wgpuQueueWriteBuffer(p.gpu.queue(), p.land_vertices, 0, p.land.lod_faces.data(), bytes);
    }
    for (unsigned i = 0; i < 6; ++i) {
        const auto& w = p.land.composed[i];
        c.windows[i] = {w.x * 10.f, w.y * 10.f, w.cells ? 1.f / (w.cells * 10.f) : 0, 0};
        if (p.land.window_dirty[i])
            p.write_texture(p.windows, i, 0, 256, p.land.window_pixels[i]);
    }
    c.windows[0].w = 1;
    c.windows[5].w = float(p.terrain.width);
    // The Remastered Look's wider full-detail mesh keeps the original's detail texture fade.
    const float range = std::min(p.land.land.range, renderer_shared::detail_range);
    c.options = {0, 1, 0, terrain_detail ? range : 0};
    float x = std::cos(camera.yaw) * .5f - std::sin(camera.yaw) * std::cos(camera.pitch);
    float y = -std::sin(camera.yaw) * .5f - std::cos(camera.yaw) * std::cos(camera.pitch);
    if (const float length = std::sqrt(x * x + y * y); length != 0) {
        x *= .70709997f / length;
        y *= .70709997f / length;
    }
    const auto byte = [](float f) { return float(int(f * 127.5f + 128)) / 255; };
    c.bump = {byte(x), byte(y), 218.f / 255, terrain_detail == 2 ? 1.f : 0.f};
    if (c.remaster.x > 0 && c.sun_color.w > 0) // Remastered: the bump relief is lit from the sun.
        c.bump = renderer_shared::sun_bump(c.sun_direction, c.bump.w);
    const float ground = terrain_height(p.terrain, camera.position.x, camera.position.z);
    c.detail_light = {camera.position.x, std::max(camera.position.y, ground + range * .5f), camera.position.z,
                      1};
    if (!p.land.lod_faces.empty())
        p.draws.push_back({p.land_vertices, nullptr, p.land_group, 0x08000000, 0,
                           static_cast<unsigned>(p.land.lod_faces.size()), false, c});
    if (p.fallback_count)
        p.draws.push_back(
            {p.land_vertices, p.fallback_indices, p.land_group, 0x08000000, 0, p.fallback_count, true, c});
    c.options = {};
    p.model_detail(camera);
    // Souls blend without writing depth, so they draw after every model that does; otherwise vegetation
    // or buildings behind a soul, drawn later, would cover it.
    if (models)
        for (const bool souls : {false, true}) {
        // Remastered grass layer: two-sided untextured foliage triangles in the widget pass, drawn with the
        // scene models before the souls. It casts no shadow.
        if (souls && widgets && p.remastered && tuning::grass_density > 0) {
            p.land.grass(p.terrain, camera, p.grass_vertex_data, p.grass_index_data);
            if (!p.grass_index_data.empty()) {
                const auto vb = p.grass_vertex_data.size() * sizeof(Vertex), ib = p.grass_index_data.size() * 4;
                if (vb > p.grass_vertex_bytes) {
                    p.grass_vertex_bytes = vb * 3 / 2;
                    p.grass_vertices = p.buffer(p.grass_vertex_bytes, WGPUBufferUsage_Vertex);
                }
                if (ib > p.grass_index_bytes) {
                    p.grass_index_bytes = ib * 3 / 2;
                    p.grass_indices = p.buffer(p.grass_index_bytes, WGPUBufferUsage_Index);
                }
                wgpuQueueWriteBuffer(p.gpu.queue(), p.grass_vertices, 0, p.grass_vertex_data.data(), vb);
                wgpuQueueWriteBuffer(p.gpu.queue(), p.grass_indices, 0, p.grass_index_data.data(), ib);
                auto base = c;
                base.world = identity();
                base.options = {0, 0, 0, 1};
                base.material = {0x08000000u | 1, 1, 0, 1};
                p.draws.push_back({p.grass_vertices, p.grass_indices, p.land_group, 0x08000000u | 1, 0,
                                   static_cast<unsigned>(p.grass_index_data.size()), true, base, true});
            }
        }
        for (auto& model : p.models) {
            if (model.soul != souls)
                continue;
            if (model.field) {
                if (!widgets)
                    continue;
                const WidgetField::Extras extras{tuning::widget_range_scale,
                                                 static_cast<unsigned>(tuning::widget_copies),
                                                 tuning::widget_copy_max_height, tuning::widget_copy_spread, true,
                                                 true, tuning::widget_min_range};
                const auto frame =
                    model.field->draw(p.terrain, {camera.position.x, camera.position.z, camera.position.y},
                                      {std::sin(camera.yaw) * std::cos(camera.pitch),
                                       std::cos(camera.yaw) * std::cos(camera.pitch), std::sin(camera.pitch)},
                                      p.detail.minimum, p.lights, p.remastered ? &extras : nullptr);
                if (frame.indices.empty() || model.materials.empty())
                    continue;
                const auto vb = frame.vertices.size() * sizeof(Vertex), ib = frame.indices.size() * 4;
                if (vb > model.vertex_bytes) {
                    model.vertex_bytes = vb * 2;
                    model.vertices = p.buffer(model.vertex_bytes, WGPUBufferUsage_Vertex);
                }
                if (ib > model.index_bytes) {
                    model.index_bytes = ib * 2;
                    model.indices = p.buffer(model.index_bytes, WGPUBufferUsage_Index);
                }
                wgpuQueueWriteBuffer(p.gpu.queue(), model.vertices, 0, frame.vertices.data(), vb);
                wgpuQueueWriteBuffer(p.gpu.queue(), model.indices, 0, frame.indices.data(), ib);
                auto base = c;
                base.world = identity();
                base.options = {1, 0, 0, 1};
                const auto push = [&](unsigned first, unsigned count, std::uint32_t flags) {
                    if (!count)
                        return;
                    base.material = {flags, 1, 0, 1};
                    p.draws.push_back({model.vertices, model.indices, model.materials[0].group, flags, first,
                                       count, true, base});
                };
                // Flora models are untextured and opaque, fading out blended.
                push(0, frame.near_indices, frame.flora ? 0x08000001u : model.materials[0].range.flags);
                push(frame.near_indices, static_cast<unsigned>(frame.indices.size()) - frame.near_indices,
                     frame.flora ? 0x08001009u : model.fade_flags);
            } else {
                if (model.screen_oriented) {
                    const auto right = XMVector3Normalize(XMVector3Cross(XMVectorSet(0, 1, 0, 0), direction));
                    const auto up = XMVector3Cross(direction, right);
                    for (auto& world : model.worlds) {
                        const auto old = XMLoadFloat4x4(&world);
                        XMStoreFloat4x4(&world, XMMATRIX(right, up, XMVector3Normalize(direction), old.r[3]));
                    }
                }
                p.append(model, c);
            }
        }
        }
    const auto right = XMVector3Normalize(XMVector3Cross(XMVectorSet(0, 1, 0, 0), direction));
    const auto up = XMVector3Cross(direction, right);
    XMFLOAT3 r{}, u{}, f{};
    XMStoreFloat3(&r, right);
    XMStoreFloat3(&u, up);
    XMStoreFloat3(&f, XMVector3Normalize(direction));
    const auto geometry = renderer_shared::expand_particles(p.effect_batches, camera, {r.x, r.y, r.z},
                                                            {u.x, u.y, u.z}, {f.x, f.y, f.z}, near);
    p.particle_count = geometry.indices.size() / 3;
    const auto vb = geometry.vertices.size() * sizeof(Vertex), ib = geometry.indices.size() * 4;
    if (vb > p.effect_vertex_capacity) {
        p.effect_vertex_capacity = vb * 2;
        p.effect_vertices = p.buffer(p.effect_vertex_capacity, WGPUBufferUsage_Vertex);
    }
    if (ib > p.effect_index_capacity) {
        p.effect_index_capacity = ib * 2;
        p.effect_indices = p.buffer(p.effect_index_capacity, WGPUBufferUsage_Index);
    }
    if (vb)
        wgpuQueueWriteBuffer(p.gpu.queue(), p.effect_vertices, 0, geometry.vertices.data(), vb);
    if (ib)
        wgpuQueueWriteBuffer(p.gpu.queue(), p.effect_indices, 0, geometry.indices.data(), ib);
    c.world = identity();
    c.model_reveal = {};
    p.first_particle_draw = p.draws.size();
    for (const auto& range : geometry.ranges) {
        const auto flags = renderer_shared::effect_flags(range.batch->flags);
        c.options = {range.batch->texture ? 1.f : 0.f, 0,
                     p.remastered && renderer_shared::soft_particles(*range.batch, flags) ? 1.f : 0.f, 1};
        c.material = {flags, 1, 1, 0};
        p.draws.push_back({p.effect_vertices, p.effect_indices,
                           p.effect_group(static_cast<WGPUTextureView>(range.batch->texture)),
                           flags | 0x08000000u, range.first, range.count, true, c});
    }
    if (p.width != width || p.height != height) {
        p.width = width;
        p.height = height;
        auto descriptor = WGPU_TEXTURE_DESCRIPTOR_INIT;
        // Readable by the remaster passes, and copied for the soft particles.
        descriptor.usage =
            WGPUTextureUsage_RenderAttachment | WGPUTextureUsage_TextureBinding | WGPUTextureUsage_CopySrc;
        descriptor.size = {width, height, 1};
        descriptor.format = WGPUTextureFormat_Depth32Float;
        p.depth = std::make_shared<WebGpuContext::Texture>();
        p.depth->texture = wgpuDeviceCreateTexture(p.gpu.device(), &descriptor);
        p.depth->view = wgpuTextureCreateView(p.depth->texture, nullptr);
        descriptor.usage = WGPUTextureUsage_TextureBinding | WGPUTextureUsage_CopyDst;
        p.depth_copy = std::make_shared<WebGpuContext::Texture>();
        p.depth_copy->texture = wgpuDeviceCreateTexture(p.gpu.device(), &descriptor);
        p.depth_copy->view = wgpuTextureCreateView(p.depth_copy->texture, nullptr);
        p.frame_dirty = true; // The frame group holds the depth copy.
        p.resize_post(width, height);
    }
    const auto bytes = p.draws.size() * constant_stride;
    if (bytes > p.uniform_capacity || !p.uniform.value) {
        p.uniform_capacity = std::max(bytes * 2, constant_stride);
        p.uniform = p.buffer(p.uniform_capacity, WGPUBufferUsage_Uniform);
        p.frame_dirty = true;
    }
    if (p.frame_dirty || !p.frame_group.value) {
        WGPUBindGroupEntry entries[6]{};
        entries[0].binding = 0;
        entries[0].buffer = p.uniform;
        entries[0].size = sizeof(Constants);
        entries[1].binding = 1;
        entries[1].buffer = p.slots;
        entries[1].size = 1024;
        entries[2].binding = 2;
        entries[2].buffer = p.point_buffer;
        entries[2].size = p.point_capacity * 64;
        entries[3].binding = 3;
        entries[3].textureView = p.depth_copy->view;
        entries[4].binding = 4;
        entries[4].textureView = p.shadow_map->view;
        entries[5].binding = 5;
        entries[5].sampler = p.shadow_sampler;
        auto descriptor = WGPU_BIND_GROUP_DESCRIPTOR_INIT;
        descriptor.layout = p.frame_layout;
        descriptor.entryCount = 6;
        descriptor.entries = entries;
        p.frame_group = wgpuDeviceCreateBindGroup(p.gpu.device(), &descriptor);
        // The shadow pass cannot bind the map it draws into.
        entries[4].textureView = p.shadow_stand_in->view;
        p.shadow_frame_group = wgpuDeviceCreateBindGroup(p.gpu.device(), &descriptor);
        p.frame_dirty = false;
    }
    p.constants.resize(bytes);
    for (std::size_t i = 0; i < p.draws.size(); ++i)
        std::memcpy(p.constants.data() + i * constant_stride, &p.draws[i].constants, sizeof(Constants));
    if (bytes)
        wgpuQueueWriteBuffer(p.gpu.queue(), p.uniform, 0, p.constants.data(), bytes);
    if (p.remastered)
        p.write_post(environment && p.scene.environment ? &*p.scene.environment : nullptr);
}
void WebSceneRenderer::render(WGPUCommandEncoder encoder, WGPUTextureView target) {
    auto& p = *impl_;
    if (p.shadows) {
        // Remastered sun shadows: the scene's casters (not the sky, unlit layers or particles) from the sun.
        auto shadow_depth = WGPU_RENDER_PASS_DEPTH_STENCIL_ATTACHMENT_INIT;
        shadow_depth.view = p.shadow_map->view;
        shadow_depth.depthLoadOp = WGPULoadOp_Clear;
        shadow_depth.depthStoreOp = WGPUStoreOp_Store;
        shadow_depth.depthClearValue = 1;
        auto shadow_descriptor = WGPU_RENDER_PASS_DESCRIPTOR_INIT;
        shadow_descriptor.depthStencilAttachment = &shadow_depth;
        const auto pass = wgpuCommandEncoderBeginRenderPass(encoder, &shadow_descriptor);
        wgpuRenderPassEncoderSetPipeline(pass, p.pipeline(0, true));
        const auto casters = std::min(p.first_particle_draw, p.draws.size());
        for (std::size_t i = 0; i < casters; ++i) {
            const auto& draw = p.draws[i];
            if (!renderer_shared::casts_shadow(draw.flags) || draw.constants.material.z != 0 || draw.shadowless ||
                (draw.constants.material.y == 0 && tuning::terrain_shadows <= 0)) // Land.
                continue;
            const auto offset = static_cast<std::uint32_t>(i * constant_stride);
            wgpuRenderPassEncoderSetBindGroup(pass, 0, p.shadow_frame_group, 1, &offset);
            wgpuRenderPassEncoderSetBindGroup(pass, 1, draw.group, 0, nullptr);
            wgpuRenderPassEncoderSetVertexBuffer(pass, 0, draw.vertices, 0, WGPU_WHOLE_SIZE);
            if (draw.indexed) {
                wgpuRenderPassEncoderSetIndexBuffer(pass, draw.indices, WGPUIndexFormat_Uint32, 0,
                                                    WGPU_WHOLE_SIZE);
                wgpuRenderPassEncoderDrawIndexed(pass, draw.count, 1, draw.first, 0, 0);
            } else
                wgpuRenderPassEncoderDraw(pass, draw.count, 1, draw.first, 0);
        }
        wgpuRenderPassEncoderEnd(pass);
        wgpuRenderPassEncoderRelease(pass);
    }
    const bool remastered = p.remastered && !p.bloom.empty();
    auto color = WGPU_RENDER_PASS_COLOR_ATTACHMENT_INIT;
    color.view = remastered ? p.hdr->view : target;
    color.loadOp = WGPULoadOp_Clear;
    color.storeOp = WGPUStoreOp_Store;
    const auto clear = environment && p.scene.environment ? unpack_color(p.scene.environment->sky_color)
                                                          : XMFLOAT4(.015, .02, .03, 1);
    color.clearValue = {clear.x, clear.y, clear.z, clear.w};
    auto depth = WGPU_RENDER_PASS_DEPTH_STENCIL_ATTACHMENT_INIT;
    depth.view = p.depth->view;
    depth.depthLoadOp = WGPULoadOp_Clear;
    depth.depthStoreOp = WGPUStoreOp_Store;
    depth.depthClearValue = 0;
    auto descriptor = WGPU_RENDER_PASS_DESCRIPTOR_INIT;
    descriptor.colorAttachmentCount = 1;
    descriptor.colorAttachments = &color;
    descriptor.depthStencilAttachment = &depth;
    // Remastered: the scene pass ends before the particles, the depth is copied for the soft particles to
    // read, and a second pass continues on the same targets.
    const auto split = p.remastered ? std::min(p.first_particle_draw, p.draws.size()) : p.draws.size();
    const auto draw_range = [&](std::size_t from, std::size_t to) {
    const auto pass = wgpuCommandEncoderBeginRenderPass(encoder, &descriptor);
    for (std::size_t i = from; i < to; ++i) {
        const auto& draw = p.draws[i];
        const auto offset = static_cast<std::uint32_t>(i * constant_stride);
        wgpuRenderPassEncoderSetPipeline(pass, p.pipeline(draw.flags));
        wgpuRenderPassEncoderSetBindGroup(pass, 0, p.frame_group, 1, &offset);
        wgpuRenderPassEncoderSetBindGroup(pass, 1, draw.group, 0, nullptr);
        wgpuRenderPassEncoderSetVertexBuffer(pass, 0, draw.vertices, 0, WGPU_WHOLE_SIZE);
        if (draw.indexed) {
            wgpuRenderPassEncoderSetIndexBuffer(pass, draw.indices, WGPUIndexFormat_Uint32, 0,
                                                WGPU_WHOLE_SIZE);
            wgpuRenderPassEncoderDrawIndexed(pass, draw.count, 1, draw.first, 0, 0);
        } else
            wgpuRenderPassEncoderDraw(pass, draw.count, 1, draw.first, 0);
    }
    wgpuRenderPassEncoderEnd(pass);
    wgpuRenderPassEncoderRelease(pass);
    };
    draw_range(0, split);
    if (split < p.draws.size()) {
        auto from = WGPU_TEXEL_COPY_TEXTURE_INFO_INIT;
        from.texture = p.depth->texture;
        auto to = WGPU_TEXEL_COPY_TEXTURE_INFO_INIT;
        to.texture = p.depth_copy->texture;
        const WGPUExtent3D size{p.width, p.height, 1};
        wgpuCommandEncoderCopyTextureToTexture(encoder, &from, &to, &size);
        color.loadOp = WGPULoadOp_Load;
        depth.depthLoadOp = WGPULoadOp_Load;
        draw_range(split, p.draws.size());
    }
    if (!remastered)
        return;
    // Bloom down the chain, back up additively, then the composite onto the canvas.
    const auto post_pass = [&](WGPUTextureView out, WGPURenderPipeline pipeline, std::size_t group, bool load) {
        auto attachment = WGPU_RENDER_PASS_COLOR_ATTACHMENT_INIT;
        attachment.view = out;
        attachment.loadOp = load ? WGPULoadOp_Load : WGPULoadOp_Clear;
        attachment.storeOp = WGPUStoreOp_Store;
        attachment.clearValue = {0, 0, 0, 1};
        auto post_descriptor = WGPU_RENDER_PASS_DESCRIPTOR_INIT;
        post_descriptor.colorAttachmentCount = 1;
        post_descriptor.colorAttachments = &attachment;
        const auto encoder_pass = wgpuCommandEncoderBeginRenderPass(encoder, &post_descriptor);
        wgpuRenderPassEncoderSetPipeline(encoder_pass, pipeline);
        wgpuRenderPassEncoderSetBindGroup(encoder_pass, 0, p.post_groups[group], 0, nullptr);
        wgpuRenderPassEncoderDraw(encoder_pass, 3, 1, 0, 0);
        wgpuRenderPassEncoderEnd(encoder_pass);
        wgpuRenderPassEncoderRelease(encoder_pass);
    };
    const auto levels = p.bloom.size();
    std::size_t group = 0;
    for (std::size_t i = 0; i < levels; ++i)
        post_pass(p.bloom[i]->view, p.post_down, group++, false);
    for (std::size_t i = levels - 1; i > 0; --i)
        post_pass(p.bloom[i - 1]->view, p.post_up, group++, true);
    post_pass(p.ao->view, p.post_ao, group++, false);
    post_pass(target, p.post_composite, group, false);
}
const Terrain& WebSceneRenderer::terrain() const {
    return impl_->terrain;
}
const TerrainMaterials& WebSceneRenderer::materials() const {
    return impl_->materials;
}
void WebSceneRenderer::order_markers(std::span<const PlayerControl::Marker> markers) {
    impl_->order_markers.assign(markers.begin(), markers.end());
}
Camera WebSceneRenderer::initial_camera() const {
    const auto& p = *impl_;
    Vec3 anchor{p.terrain.width * 5.f, p.terrain.height * 5.f, 0};
    if (!p.scene.building_groups.empty())
        anchor = p.scene.building_groups.front().position;
    Camera result;
    result.position = {anchor.x, terrain_height(p.terrain, anchor.x, anchor.y) + 65, anchor.y - 160};
    result.pitch = -.15f;
    return result;
}
std::size_t WebSceneRenderer::model_count() const {
    return impl_->models.size();
}
std::size_t WebSceneRenderer::triangle_count() const {
    std::size_t total{};
    for (const auto& draw : impl_->draws)
        total += draw.count / 3;
    return total;
}
std::size_t WebSceneRenderer::unresolved_count() const {
    return impl_->scene.unresolved.size();
}
SceneMatch* WebSceneRenderer::simulation() {
    return impl_->simulation.get();
}
std::size_t WebSceneRenderer::particle_count() const {
    return impl_->particle_count;
}
std::size_t WebSceneRenderer::animated_count() const {
    return impl_->animated;
}
std::uint32_t WebSceneRenderer::ticks() const {
    return impl_->simulation ? impl_->simulation->match().clock().match : 0;
}
} // namespace sacrifice
