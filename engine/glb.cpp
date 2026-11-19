#include "sacrifice/glb.hpp"
#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>
#include <fstream>
#include <limits>
#include <map>
#include <nlohmann/json.hpp>

#ifdef _WIN32
#include <windows.h>
#include <objbase.h>
#include <wincodec.h>
#include <wrl/client.h>
#endif

namespace sacrifice {
namespace {

using Mat4 = std::array<float, 16>; // Column-major, as in glTF.

Mat4 identity_mat4() {
    return {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1};
}

Mat4 mul_mat4(const Mat4& a, const Mat4& b) {
    Mat4 r{};
    for (int col = 0; col < 4; ++col)
        for (int row = 0; row < 4; ++row) {
            float sum = 0;
            for (int k = 0; k < 4; ++k)
                sum += a[k * 4 + row] * b[col * 4 + k];
            r[col * 4 + row] = sum;
        }
    return r;
}

Mat4 trs_mat4(const std::array<float, 3>& t, const std::array<float, 4>& q, const std::array<float, 3>& s) {
    const float x = q[0], y = q[1], z = q[2], w = q[3];
    const float xx = x * x, yy = y * y, zz = z * z;
    const float xy = x * y, xz = x * z, yz = y * z;
    const float wx = w * x, wy = w * y, wz = w * z;
    Mat4 m{};
    m[0] = (1.0f - 2.0f * (yy + zz)) * s[0];
    m[1] = (2.0f * (xy + wz)) * s[0];
    m[2] = (2.0f * (xz - wy)) * s[0];
    m[3] = 0.0f;
    m[4] = (2.0f * (xy - wz)) * s[1];
    m[5] = (1.0f - 2.0f * (xx + zz)) * s[1];
    m[6] = (2.0f * (yz + wx)) * s[1];
    m[7] = 0.0f;
    m[8] = (2.0f * (xz + wy)) * s[2];
    m[9] = (2.0f * (yz - wx)) * s[2];
    m[10] = (1.0f - 2.0f * (xx + yy)) * s[2];
    m[11] = 0.0f;
    m[12] = t[0];
    m[13] = t[1];
    m[14] = t[2];
    m[15] = 1.0f;
    return m;
}

Vec3 transform_point(const Mat4& m, Vec3 p) {
    return {
        m[0] * p.x + m[4] * p.y + m[8] * p.z + m[12],
        m[1] * p.x + m[5] * p.y + m[9] * p.z + m[13],
        m[2] * p.x + m[6] * p.y + m[10] * p.z + m[14],
    };
}

Vec3 transform_normal(const Mat4& m, Vec3 n) {
    Vec3 r{
        m[0] * n.x + m[4] * n.y + m[8] * n.z,
        m[1] * n.x + m[5] * n.y + m[9] * n.z,
        m[2] * n.x + m[6] * n.y + m[10] * n.z,
    };
    const float len = std::sqrt(r.x * r.x + r.y * r.y + r.z * r.z);
    if (len > 1e-6f)
        return {r.x / len, r.y / len, r.z / len};
    return {0, 1, 0};
}

std::uint32_t read_u32_le(const std::uint8_t* p) {
    std::uint32_t v{};
    std::memcpy(&v, p, sizeof(v));
    return v;
}

Image decode_encoded_image(const std::uint8_t* data, std::size_t size, unsigned max_dim = 2048) {
#ifdef _WIN32
    using Microsoft::WRL::ComPtr;
    const HRESULT init_hr = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    const bool uninit = SUCCEEDED(init_hr);
    struct CoGuard {
        bool active;
        ~CoGuard() {
            if (active)
                CoUninitialize();
        }
    } guard{uninit};

    ComPtr<IWICImagingFactory> factory;
    if (FAILED(CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER,
                                IID_PPV_ARGS(&factory))))
        throw Error("GLB: failed to create WIC imaging factory");

    ComPtr<IWICStream> stream;
    if (FAILED(factory->CreateStream(&stream)))
        throw Error("GLB: failed to create WIC stream");
    if (size > std::numeric_limits<DWORD>::max() ||
        FAILED(stream->InitializeFromMemory(const_cast<BYTE*>(data), static_cast<DWORD>(size))))
        throw Error("GLB: failed to initialize WIC memory stream");

    ComPtr<IWICBitmapDecoder> decoder;
    if (FAILED(factory->CreateDecoderFromStream(stream.Get(), nullptr, WICDecodeMetadataCacheOnDemand,
                                                &decoder)))
        throw Error("GLB: failed to decode embedded image");

    ComPtr<IWICBitmapFrameDecode> frame;
    if (FAILED(decoder->GetFrame(0, &frame)))
        throw Error("GLB: failed to get image frame");

    UINT width = 0, height = 0;
    if (FAILED(frame->GetSize(&width, &height)) || !width || !height)
        throw Error("GLB: invalid embedded image size");

    ComPtr<IWICBitmapSource> source = frame;
    if (max_dim > 0 && (width > max_dim || height > max_dim)) {
        const double scale =
            static_cast<double>(max_dim) / static_cast<double>(std::max(width, height));
        const UINT new_w = std::max<UINT>(1, static_cast<UINT>(std::lround(width * scale)));
        const UINT new_h = std::max<UINT>(1, static_cast<UINT>(std::lround(height * scale)));
        ComPtr<IWICBitmapScaler> scaler;
        if (SUCCEEDED(factory->CreateBitmapScaler(&scaler)) &&
            SUCCEEDED(scaler->Initialize(source.Get(), new_w, new_h, WICBitmapInterpolationModeFant))) {
            source = scaler;
            width = new_w;
            height = new_h;
        }
    }

    ComPtr<IWICFormatConverter> converter;
    if (FAILED(factory->CreateFormatConverter(&converter)) ||
        FAILED(converter->Initialize(source.Get(), GUID_WICPixelFormat32bppRGBA, WICBitmapDitherTypeNone,
                                     nullptr, 0.0, WICBitmapPaletteTypeCustom)))
        throw Error("GLB: failed to convert embedded image to RGBA");

    Image image{width, height, std::vector<std::uint32_t>(static_cast<std::size_t>(width) * height)};
    if (FAILED(converter->CopyPixels(nullptr, width * 4,
                                     static_cast<UINT>(image.rgba.size() * sizeof(std::uint32_t)),
                                     reinterpret_cast<BYTE*>(image.rgba.data()))))
        throw Error("GLB: failed to copy decoded pixels");
    return image;
#else
    (void)data;
    (void)size;
    (void)max_dim;
    throw Error("GLB image decoding is only implemented on Windows");
#endif
}

struct AccessorView {
    const std::uint8_t* ptr{};
    std::size_t count{};
    std::size_t stride{};
    int component_type{};
    std::string type;
};

AccessorView resolve_accessor(const nlohmann::json& gltf, const std::uint8_t* bin, std::size_t bin_size,
                              std::size_t accessor_index) {
    const auto& acc = gltf.at("accessors").at(accessor_index);
    const auto bv_index = acc.at("bufferView").get<std::size_t>();
    const auto& bv = gltf.at("bufferViews").at(bv_index);
    const std::size_t bv_offset = bv.value("byteOffset", std::size_t{0});
    const std::size_t acc_offset = acc.value("byteOffset", std::size_t{0});
    const std::size_t count = acc.at("count").get<std::size_t>();
    const int comp_type = acc.at("componentType").get<int>();
    const std::string type = acc.at("type").get<std::string>();

    std::size_t comp_size = 0;
    switch (comp_type) {
    case 5120:
    case 5121:
        comp_size = 1;
        break;
    case 5122:
    case 5123:
        comp_size = 2;
        break;
    case 5125:
    case 5126:
        comp_size = 4;
        break;
    default:
        throw Error("GLB: unsupported accessor componentType");
    }
    std::size_t num_comps = 1;
    if (type == "VEC2")
        num_comps = 2;
    else if (type == "VEC3")
        num_comps = 3;
    else if (type == "VEC4")
        num_comps = 4;
    else if (type != "SCALAR")
        throw Error("GLB: unsupported accessor type " + type);

    const std::size_t elem_size = comp_size * num_comps;
    const std::size_t stride = bv.value("byteStride", elem_size);
    const std::size_t start = bv_offset + acc_offset;
    if (count > 0 && (start > bin_size || (count - 1) * stride + elem_size > bin_size - start))
        throw Error("GLB: accessor data outside BIN chunk");
    return {bin + start, count, stride, comp_type, type};
}

Mat4 node_local_matrix(const nlohmann::json& node) {
    if (node.contains("matrix") && node["matrix"].is_array() && node["matrix"].size() == 16) {
        Mat4 m{};
        for (std::size_t i = 0; i < 16; ++i)
            m[i] = node["matrix"][i].get<float>();
        return m;
    }
    std::array<float, 3> t{0, 0, 0};
    std::array<float, 4> r{0, 0, 0, 1};
    std::array<float, 3> s{1, 1, 1};
    if (node.contains("translation"))
        for (std::size_t i = 0; i < 3; ++i)
            t[i] = node["translation"][i].get<float>();
    if (node.contains("rotation"))
        for (std::size_t i = 0; i < 4; ++i)
            r[i] = node["rotation"][i].get<float>();
    if (node.contains("scale"))
        for (std::size_t i = 0; i < 3; ++i)
            s[i] = node["scale"][i].get<float>();
    return trs_mat4(t, r, s);
}

} // namespace

ModelAsset load_glb(const std::filesystem::path& path, float target_height) {
    std::ifstream file(path, std::ios::binary | std::ios::ate);
    if (!file)
        throw Error("GLB: cannot open " + path.string());
    const auto file_size = static_cast<std::size_t>(file.tellg());
    if (file_size < 20)
        throw Error("GLB: file too small");
    std::vector<std::uint8_t> bytes(file_size);
    file.seekg(0);
    if (!file.read(reinterpret_cast<char*>(bytes.data()), static_cast<std::streamsize>(file_size)))
        throw Error("GLB: failed to read file");

    if (std::memcmp(bytes.data(), "glTF", 4) != 0)
        throw Error("GLB: bad magic");
    const std::uint32_t version = read_u32_le(bytes.data() + 4);
    if (version != 2)
        throw Error("GLB: unsupported glTF version");

    const std::uint8_t* json_ptr = nullptr;
    std::size_t json_len = 0;
    const std::uint8_t* bin_ptr = nullptr;
    std::size_t bin_len = 0;

    std::size_t offset = 12;
    while (offset + 8 <= file_size) {
        const std::uint32_t chunk_len = read_u32_le(bytes.data() + offset);
        const std::uint32_t chunk_type = read_u32_le(bytes.data() + offset + 4);
        offset += 8;
        if (offset + chunk_len > file_size)
            throw Error("GLB: truncated chunk");
        if (chunk_type == 0x4E4F534Au) { // "JSON"
            json_ptr = bytes.data() + offset;
            json_len = chunk_len;
        } else if (chunk_type == 0x004E4942u) { // "BIN\0"
            bin_ptr = bytes.data() + offset;
            bin_len = chunk_len;
        }
        offset += chunk_len;
    }
    if (!json_ptr || !bin_ptr)
        throw Error("GLB: missing JSON or BIN chunk");

    const auto gltf = nlohmann::json::parse(json_ptr, json_ptr + json_len);

    // Decode unique embedded images on demand.
    std::map<std::size_t, Image> decoded_images;
    const auto get_image = [&](std::size_t img_idx) -> const Image& {
        if (auto it = decoded_images.find(img_idx); it != decoded_images.end())
            return it->second;
        const auto& img = gltf.at("images").at(img_idx);
        const auto bv_idx = img.at("bufferView").get<std::size_t>();
        const auto& bv = gltf.at("bufferViews").at(bv_idx);
        const std::size_t bv_off = bv.value("byteOffset", std::size_t{0});
        const std::size_t bv_len = bv.at("byteLength").get<std::size_t>();
        if (bv_off + bv_len > bin_len)
            throw Error("GLB: image bufferView outside BIN chunk");
        return decoded_images.emplace(img_idx, decode_encoded_image(bin_ptr + bv_off, bv_len)).first->second;
    };

    ModelAsset asset;
    std::map<int, std::uint32_t> material_map; // glTF material index (-1 = default) -> asset material index.

    const auto get_material = [&](int gltf_mat) -> std::uint32_t {
        if (auto it = material_map.find(gltf_mat); it != material_map.end())
            return it->second;
        const auto index = static_cast<std::uint32_t>(asset.mesh.materials.size());
        std::uint32_t flags = 0x00010001u; // Lit textured opaque (same base as SAXS).
        std::optional<ModelAsset::Texture> tex;

        if (gltf_mat >= 0 && gltf.contains("materials") &&
            static_cast<std::size_t>(gltf_mat) < gltf["materials"].size()) {
            const auto& mat = gltf["materials"][static_cast<std::size_t>(gltf_mat)];
            if (mat.value("doubleSided", false))
                flags |= 0x08000000u;
            const std::string alpha_mode = mat.value("alphaMode", std::string("OPAQUE"));
            if (alpha_mode == "BLEND")
                flags |= 0x00001008u;

            if (mat.contains("pbrMetallicRoughness") &&
                mat["pbrMetallicRoughness"].contains("baseColorTexture")) {
                const auto tex_idx =
                    mat["pbrMetallicRoughness"]["baseColorTexture"].at("index").get<std::size_t>();
                const auto img_idx = gltf.at("textures").at(tex_idx).at("source").get<std::size_t>();
                const Image& decoded = get_image(img_idx);
                TextureHeader header{};
                header.width = decoded.width;
                header.height = decoded.height;
                header.frames = 1;
                header.frame_width = decoded.width;
                header.frame_height = decoded.height;
                tex = ModelAsset::Texture{header, decoded, {}};
            }
        }
        asset.mesh.materials.push_back({flags, FourCC::from_text("glb0")});
        asset.textures.push_back(std::move(tex));
        material_map.emplace(gltf_mat, index);
        return index;
    };

    const auto append_mesh = [&](std::size_t mesh_index, const Mat4& world) {
        const auto& mesh_json = gltf.at("meshes").at(mesh_index);
        for (const auto& prim : mesh_json.at("primitives")) {
            const int mode = prim.value("mode", 4); // 4 = TRIANGLES
            if (mode != 4)
                continue;
            const auto& attrs = prim.at("attributes");
            if (!attrs.contains("POSITION"))
                continue;

            const auto pos_view =
                resolve_accessor(gltf, bin_ptr, bin_len, attrs.at("POSITION").get<std::size_t>());
            if (pos_view.component_type != 5126 || pos_view.type != "VEC3")
                throw Error("GLB: POSITION must be FLOAT VEC3");

            std::optional<AccessorView> norm_view;
            if (attrs.contains("NORMAL")) {
                auto v = resolve_accessor(gltf, bin_ptr, bin_len, attrs.at("NORMAL").get<std::size_t>());
                if (v.component_type == 5126 && v.type == "VEC3" && v.count == pos_view.count)
                    norm_view = v;
            }
            std::optional<AccessorView> uv_view;
            if (attrs.contains("TEXCOORD_0")) {
                auto v =
                    resolve_accessor(gltf, bin_ptr, bin_len, attrs.at("TEXCOORD_0").get<std::size_t>());
                if (v.component_type == 5126 && v.type == "VEC2" && v.count == pos_view.count)
                    uv_view = v;
            }

            const std::uint32_t mat_index = get_material(prim.value("material", -1));
            const auto base_vertex = static_cast<std::uint32_t>(asset.mesh.vertices.size());
            asset.mesh.vertices.reserve(asset.mesh.vertices.size() + pos_view.count);

            for (std::size_t i = 0; i < pos_view.count; ++i) {
                float p[3]{};
                std::memcpy(p, pos_view.ptr + i * pos_view.stride, sizeof(p));
                Vec3 n{0, 1, 0};
                if (norm_view) {
                    float nf[3]{};
                    std::memcpy(nf, norm_view->ptr + i * norm_view->stride, sizeof(nf));
                    n = transform_normal(world, {nf[0], nf[1], nf[2]});
                }
                Vec2 uv{0, 0};
                if (uv_view) {
                    float uvf[2]{};
                    std::memcpy(uvf, uv_view->ptr + i * uv_view->stride, sizeof(uvf));
                    uv = {uvf[0], uvf[1]};
                }
                Vertex v;
                v.position = transform_point(world, {p[0], p[1], p[2]});
                v.normal = n;
                v.uv = uv;
                v.color = 0xffffffffu;
                asset.mesh.vertices.push_back(v);
            }

            auto emit_tri = [&](std::uint32_t i0, std::uint32_t i1, std::uint32_t i2) {
                // Swap i1/i2 to convert glTF CCW winding to D3D11 CW front-facing winding.
                asset.mesh.indices.push_back(base_vertex + i0);
                asset.mesh.indices.push_back(base_vertex + i2);
                asset.mesh.indices.push_back(base_vertex + i1);
                asset.mesh.triangle_materials.push_back(mat_index);
            };

            if (prim.contains("indices")) {
                const auto idx_view =
                    resolve_accessor(gltf, bin_ptr, bin_len, prim.at("indices").get<std::size_t>());
                const auto read_index = [&](std::size_t k) -> std::uint32_t {
                    const auto* p = idx_view.ptr + k * idx_view.stride;
                    if (idx_view.component_type == 5121)
                        return *p;
                    if (idx_view.component_type == 5123) {
                        std::uint16_t v{};
                        std::memcpy(&v, p, sizeof(v));
                        return v;
                    }
                    if (idx_view.component_type == 5125)
                        return read_u32_le(p);
                    throw Error("GLB: unsupported index componentType");
                };
                const std::size_t tris = idx_view.count / 3;
                asset.mesh.indices.reserve(asset.mesh.indices.size() + tris * 3);
                asset.mesh.triangle_materials.reserve(asset.mesh.triangle_materials.size() + tris);
                for (std::size_t t = 0; t < tris; ++t)
                    emit_tri(read_index(t * 3), read_index(t * 3 + 1), read_index(t * 3 + 2));
            } else {
                const std::size_t tris = pos_view.count / 3;
                asset.mesh.indices.reserve(asset.mesh.indices.size() + tris * 3);
                asset.mesh.triangle_materials.reserve(asset.mesh.triangle_materials.size() + tris);
                for (std::size_t t = 0; t < tris; ++t)
                    emit_tri(static_cast<std::uint32_t>(t * 3), static_cast<std::uint32_t>(t * 3 + 1),
                             static_cast<std::uint32_t>(t * 3 + 2));
            }
        }
    };

    const auto visit_node = [&](auto&& self, std::size_t node_index, const Mat4& parent) -> void {
        const auto& node = gltf.at("nodes").at(node_index);
        const Mat4 world = mul_mat4(parent, node_local_matrix(node));
        if (node.contains("mesh"))
            append_mesh(node["mesh"].get<std::size_t>(), world);
        if (node.contains("children"))
            for (const auto& child : node["children"])
                self(self, child.get<std::size_t>(), world);
    };

    if (gltf.contains("scenes") && !gltf["scenes"].empty()) {
        const std::size_t scene_idx = gltf.value("scene", std::size_t{0});
        const auto& scene = gltf["scenes"].at(scene_idx);
        if (scene.contains("nodes"))
            for (const auto& root : scene["nodes"])
                visit_node(visit_node, root.get<std::size_t>(), identity_mat4());
    } else if (gltf.contains("nodes")) {
        for (std::size_t i = 0; i < gltf["nodes"].size(); ++i)
            visit_node(visit_node, i, identity_mat4());
    }

    if (asset.mesh.vertices.empty() || asset.mesh.indices.empty())
        throw Error("GLB: no triangle geometry found in " + path.string());

    // Center X/Z, sit feet on Y = 0, and optionally scale to match target_height.
    Vec3 low{std::numeric_limits<float>::max(), std::numeric_limits<float>::max(),
             std::numeric_limits<float>::max()};
    Vec3 high{std::numeric_limits<float>::lowest(), std::numeric_limits<float>::lowest(),
              std::numeric_limits<float>::lowest()};
    for (const auto& v : asset.mesh.vertices) {
        low.x = std::min(low.x, v.position.x);
        low.y = std::min(low.y, v.position.y);
        low.z = std::min(low.z, v.position.z);
        high.x = std::max(high.x, v.position.x);
        high.y = std::max(high.y, v.position.y);
        high.z = std::max(high.z, v.position.z);
    }
    const float cx = (low.x + high.x) * 0.5f;
    const float cz = (low.z + high.z) * 0.5f;
    const float height = high.y - low.y;
    const float scale = (target_height > 0.0f && height > 1e-4f) ? (target_height / height) : 1.0f;

    for (auto& v : asset.mesh.vertices) {
        v.position.x = (v.position.x - cx) * scale;
        v.position.y = (v.position.y - low.y) * scale;
        v.position.z = (v.position.z - cz) * scale;
    }

    log("Loaded GLB " + path.filename().string() + ": " + std::to_string(asset.mesh.vertices.size()) +
        " verts, " + std::to_string(asset.mesh.indices.size() / 3) + " tris, scale=" +
        std::to_string(scale));
    return asset;
}


} // namespace sacrifice
