#include "sacrifice/mesh_import.hpp"
#include <algorithm>
#include <array>
#include <cctype>
#include <cmath>
#include <cstring>
#include <fstream>
#include <limits>
#include <map>
#include <nlohmann/json.hpp>
#include <sstream>
#include <unordered_map>

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
                // Right-handed CCW outward faces are already CW in left-handed D3D11 view space.
                asset.mesh.indices.push_back(base_vertex + i0);
                asset.mesh.indices.push_back(base_vertex + i1);
                asset.mesh.indices.push_back(base_vertex + i2);
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

namespace {

std::string to_lower_ascii(std::string s) {
    for (char& c : s)
        c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return s;
}

std::string trim_ws(std::string s) {
    const auto first = s.find_first_not_of(" \t\r\n");
    if (first == std::string::npos)
        return {};
    const auto last = s.find_last_not_of(" \t\r\n");
    return s.substr(first, last - first + 1);
}

std::uint32_t pack_rgba(float r, float g, float b, float a = 1.0f) {
    const auto ch = [](float v) { return static_cast<std::uint32_t>(std::clamp(v, 0.0f, 1.0f) * 255.0f + 0.5f); };
    return ch(r) | (ch(g) << 8) | (ch(b) << 16) | (ch(a) << 24);
}

float hash2d(int x, int y) {
    std::uint32_t h = static_cast<std::uint32_t>(x) * 374761393u + static_cast<std::uint32_t>(y) * 668265263u;
    h = (h ^ (h >> 13)) * 1274126177u;
    return static_cast<float>((h ^ (h >> 16)) & 0xffffffu) / 16777215.0f;
}

float value_noise(float x, float y) {
    const int xi = static_cast<int>(std::floor(x));
    const int yi = static_cast<int>(std::floor(y));
    const float fx = x - xi, fy = y - yi;
    const float ux = fx * fx * (3.0f - 2.0f * fx);
    const float uy = fy * fy * (3.0f - 2.0f * fy);
    const float a = hash2d(xi, yi), b = hash2d(xi + 1, yi);
    const float c = hash2d(xi, yi + 1), d = hash2d(xi + 1, yi + 1);
    return (a + (b - a) * ux) + ((c + (d - c) * ux) - (a + (b - a) * ux)) * uy;
}

Image make_procedural_bark() {
    constexpr unsigned sz = 256;
    Image img{sz, sz, std::vector<std::uint32_t>(sz * sz)};
    for (unsigned y = 0; y < sz; ++y) {
        for (unsigned x = 0; x < sz; ++x) {
            const float u = static_cast<float>(x) / sz;
            const float v = static_cast<float>(y) / sz;
            const float ridge = value_noise(u * 28.0f, v * 5.0f) * 0.65f +
                                value_noise(u * 64.0f, v * 14.0f) * 0.35f;
            const float grain = 0.55f + 0.65f * ridge;
            const float r = 0.28f * grain;
            const float g = 0.19f * grain;
            const float b = 0.12f * grain;
            img.rgba[y * sz + x] = pack_rgba(r, g, b, 1.0f);
        }
    }
    return img;
}

// Procedural alpha-cutout pine bough with radiating needles for fallback when Leaf_Pine_C.png is absent.
Image make_procedural_pine_leaves() {
    constexpr unsigned sz = 256;
    Image img{sz, sz, std::vector<std::uint32_t>(sz * sz, 0u)};
    auto draw_seg = [&](float x0, float y0, float x1, float y1, float half_w, std::uint32_t col) {
        const float min_x = std::max(0.0f, std::floor(std::min(x0, x1) - half_w - 1));
        const float max_x = std::min(static_cast<float>(sz - 1), std::ceil(std::max(x0, x1) + half_w + 1));
        const float min_y = std::max(0.0f, std::floor(std::min(y0, y1) - half_w - 1));
        const float max_y = std::min(static_cast<float>(sz - 1), std::ceil(std::max(y0, y1) + half_w + 1));
        const float dx = x1 - x0, dy = y1 - y0;
        const float len2 = std::max(dx * dx + dy * dy, 1e-4f);
        for (int py = static_cast<int>(min_y); py <= static_cast<int>(max_y); ++py) {
            for (int px = static_cast<int>(min_x); px <= static_cast<int>(max_x); ++px) {
                const float t = std::clamp(((px - x0) * dx + (py - y0) * dy) / len2, 0.0f, 1.0f);
                const float nx = x0 + dx * t - px, ny = y0 + dy * t - py;
                if (nx * nx + ny * ny <= half_w * half_w)
                    img.rgba[static_cast<std::size_t>(py) * sz + px] = col;
            }
        }
    };
    auto draw_twig = [&](float bx, float by, float tx, float ty, float spread, int needles, int seed) {
        const float dx = tx - bx, dy = ty - by;
        const float len = std::hypot(dx, dy);
        if (len < 1.0f)
            return;
        const float ux = dx / len, uy = dy / len;
        const float px = -uy, py = ux;
        // Central woody twig.
        draw_seg(bx, by, tx, ty, 2.2f, pack_rgba(0.24f, 0.16f, 0.09f));
        // Radiating pine needles along both sides.
        for (int i = 0; i < needles; ++i) {
            const float t = (i + 0.5f) / needles;
            const float ox = bx + dx * t, oy = by + dy * t;
            const float envelope = std::sin(t * 3.14159265f * 0.92f + 0.15f);
            for (int side : {-1, 1}) {
                const float jitter = (hash2d(i * 7 + seed, side * 13 + seed) - 0.5f) * 0.35f;
                const float nlen = spread * envelope * (0.75f + 0.45f * hash2d(i + seed, side + 31));
                const float dirx = ux * (0.58f + jitter) + px * side * 0.82f;
                const float diry = uy * (0.58f + jitter) + py * side * 0.82f;
                const float shade = 0.72f + 0.45f * t + 0.18f * (hash2d(i * 3 + seed, side * 19) - 0.5f);
                const auto col = pack_rgba(0.11f * shade, 0.29f * shade, 0.10f * shade);
                draw_seg(ox, oy, ox + dirx * nlen, oy + diry * nlen, 1.85f, col);
            }
        }
    };
    // Main central bough + 4 side shoots fanning out to fill the leaf card naturally.
    draw_twig(128, 250, 128, 14, 52.0f, 38, 1);
    draw_twig(128, 210, 42, 118, 38.0f, 26, 2);
    draw_twig(128, 210, 214, 118, 38.0f, 26, 3);
    draw_twig(128, 142, 46, 48, 34.0f, 24, 4);
    draw_twig(128, 142, 210, 48, 34.0f, 24, 5);
    return img;
}

std::optional<Image> try_load_image_file(const std::filesystem::path& path) {
    std::error_code ec;
    if (!std::filesystem::is_regular_file(path, ec))
        return std::nullopt;
    std::ifstream file(path, std::ios::binary | std::ios::ate);
    if (!file)
        return std::nullopt;
    const auto sz = static_cast<std::size_t>(file.tellg());
    if (!sz)
        return std::nullopt;
    std::vector<std::uint8_t> bytes(sz);
    file.seekg(0);
    if (!file.read(reinterpret_cast<char*>(bytes.data()), static_cast<std::streamsize>(sz)))
        return std::nullopt;
    try {
        return decode_encoded_image(bytes.data(), bytes.size(), 1024);
    } catch (const std::exception&) {
        return std::nullopt;
    }
}

std::filesystem::path obj_exe_directory() {
#ifdef _WIN32
    wchar_t buf[MAX_PATH]{};
    const DWORD len = GetModuleFileNameW(nullptr, buf, MAX_PATH);
    if (len > 0 && len < MAX_PATH)
        return std::filesystem::path(buf).parent_path();
#endif
    return {};
}

struct MtlInfo {
    Vec3 kd{0.8f, 0.8f, 0.8f};
    std::string map_kd;
    bool alpha_cutout{};
};

std::map<std::string, MtlInfo> parse_mtl(const std::filesystem::path& path) {
    std::map<std::string, MtlInfo> out;
    std::ifstream file(path);
    if (!file)
        return out;
    std::string line, current;
    while (std::getline(file, line)) {
        line = trim_ws(line);
        if (line.empty() || line[0] == '#')
            continue;
        std::istringstream ss(line);
        std::string key;
        ss >> key;
        if (key == "newmtl") {
            ss >> current;
            out.try_emplace(current);
        } else if (!current.empty()) {
            if (key == "Kd") {
                ss >> out[current].kd.x >> out[current].kd.y >> out[current].kd.z;
            } else if (key == "map_Kd") {
                std::string rest;
                std::getline(ss, rest);
                out[current].map_kd = trim_ws(rest);
            } else if (key == "map_d") {
                out[current].alpha_cutout = true;
            }
        }
    }
    return out;
}

} // namespace

ModelAsset load_obj(const std::filesystem::path& path, float target_height) {
    std::ifstream file(path);
    if (!file)
        throw Error("OBJ: cannot open " + path.string());

    std::vector<Vec3> positions;
    std::vector<Vec2> uvs;
    std::vector<Vec3> normals;
    std::map<std::string, MtlInfo> mtls;

    ModelAsset asset;
    std::map<std::string, std::uint32_t> mat_indices;

    const auto resolve_material = [&](const std::string& name) -> std::uint32_t {
        if (auto it = mat_indices.find(name); it != mat_indices.end())
            return it->second;
        const auto index = static_cast<std::uint32_t>(asset.mesh.materials.size());
        const auto lower = to_lower_ascii(name);
        const auto mtl_it = mtls.find(name);
        const bool is_leaf = (mtl_it != mtls.end() && mtl_it->second.alpha_cutout) ||
                             lower.find("leaf") != std::string::npos ||
                             lower.find("leaves") != std::string::npos ||
                             lower.find("foliage") != std::string::npos ||
                             lower.find("pine") != std::string::npos;
        const std::uint32_t flags = is_leaf ? 0x08010001u : 0x00010001u;

        std::optional<Image> decoded;
        if (mtl_it != mtls.end() && !mtl_it->second.map_kd.empty()) {
            const std::filesystem::path tex_ref(mtl_it->second.map_kd);
            const auto fname = tex_ref.filename();
            const auto dir = path.parent_path();
            for (const auto& candidate : {dir / fname, dir / "textures" / fname, tex_ref}) {
                decoded = try_load_image_file(candidate);
                if (decoded)
                    break;
            }
        }
        if (!decoded)
            decoded = is_leaf ? make_procedural_pine_leaves() : make_procedural_bark();

        if (is_leaf) {
            // Keep original RGB on opaque pixels, threshold alpha cleanly, and bleed opaque RGB into
            // transparent pixels so bilinear filtering and mipmaps never create dark outlines.
            const unsigned w = decoded->width, h = decoded->height;
            std::vector<std::uint8_t> solid(static_cast<std::size_t>(w) * h, 0);
            for (std::size_t i = 0; i < decoded->rgba.size(); ++i) {
                if ((decoded->rgba[i] >> 24) >= 128) {
                    decoded->rgba[i] |= 0xff000000u;
                    solid[i] = 1;
                } else {
                    decoded->rgba[i] &= 0x00ffffffu;
                }
            }
            // Dilate opaque RGB outward into transparent pixels (6 passes covers mipmap & bilinear fringes).
            std::vector<std::uint32_t> next_rgba = decoded->rgba;
            std::vector<std::uint8_t> next_solid = solid;
            for (int pass = 0; pass < 6; ++pass) {
                bool changed = false;
                for (unsigned y = 0; y < h; ++y) {
                    for (unsigned x = 0; x < w; ++x) {
                        const std::size_t idx = static_cast<std::size_t>(y) * w + x;
                        if (solid[idx])
                            continue;
                        unsigned sr = 0, sg = 0, sb = 0, cnt = 0;
                        for (int dy = -1; dy <= 1; ++dy) {
                            const int ny = static_cast<int>(y) + dy;
                            if (ny < 0 || ny >= static_cast<int>(h))
                                continue;
                            for (int dx = -1; dx <= 1; ++dx) {
                                const int nx = static_cast<int>(x) + dx;
                                if (nx < 0 || nx >= static_cast<int>(w))
                                    continue;
                                const std::size_t nidx = static_cast<std::size_t>(ny) * w + nx;
                                if (!solid[nidx])
                                    continue;
                                const std::uint32_t c = decoded->rgba[nidx];
                                sr += c & 0xffu;
                                sg += (c >> 8) & 0xffu;
                                sb += (c >> 16) & 0xffu;
                                ++cnt;
                            }
                        }
                        if (cnt > 0) {
                            next_rgba[idx] = (sr / cnt) | ((sg / cnt) << 8) | ((sb / cnt) << 16);
                            next_solid[idx] = 1;
                            changed = true;
                        }
                    }
                }
                decoded->rgba.swap(next_rgba);
                solid.swap(next_solid);
                next_rgba = decoded->rgba;
                next_solid = solid;
                if (!changed)
                    break;
            }
            // Any remaining untouched transparent pixels get average solid leaf color (never black).
            std::uint64_t ar = 0, ag = 0, ab = 0, ac = 0;
            for (std::size_t i = 0; i < decoded->rgba.size(); ++i) {
                if ((decoded->rgba[i] >> 24) >= 128) {
                    ar += decoded->rgba[i] & 0xffu;
                    ag += (decoded->rgba[i] >> 8) & 0xffu;
                    ab += (decoded->rgba[i] >> 16) & 0xffu;
                    ++ac;
                }
            }
            const std::uint32_t fallback_rgb =
                ac ? (static_cast<std::uint32_t>(ar / ac) | (static_cast<std::uint32_t>(ag / ac) << 8) |
                      (static_cast<std::uint32_t>(ab / ac) << 16))
                   : 0x001a441au;
            for (std::size_t i = 0; i < decoded->rgba.size(); ++i)
                if ((decoded->rgba[i] >> 24) == 0 && !solid[i])
                    decoded->rgba[i] = fallback_rgb;
        }

        TextureHeader header{};
        header.width = decoded->width;
        header.height = decoded->height;
        header.frames = 1;
        header.frame_width = decoded->width;
        header.frame_height = decoded->height;
        asset.mesh.materials.push_back({flags, FourCC::from_text(is_leaf ? "leaf" : "bark")});
        asset.textures.push_back(ModelAsset::Texture{header, std::move(*decoded), {}});
        mat_indices.emplace(name, index);
        return index;
    };

    struct VertKey {
        int v{}, vt{}, vn{};
        bool operator==(const VertKey&) const = default;
    };
    struct VertKeyHash {
        std::size_t operator()(const VertKey& k) const noexcept {
            return (static_cast<std::size_t>(k.v) * 73856093u) ^
                   (static_cast<std::size_t>(k.vt) * 19349663u) ^
                   (static_cast<std::size_t>(k.vn) * 83492791u);
        }
    };
    std::unordered_map<VertKey, std::uint32_t, VertKeyHash> vert_cache;
    vert_cache.reserve(4096);

    const auto fix_index = [](int idx, std::size_t count) -> int {
        if (idx > 0)
            return idx - 1;
        if (idx < 0)
            return static_cast<int>(count) + idx;
        return -1;
    };

    const auto get_vertex = [&](const std::string& token) -> std::uint32_t {
        int vi = 0, ti = 0, ni = 0;
        const auto s1 = token.find('/');
        if (s1 == std::string::npos) {
            vi = std::stoi(token);
        } else {
            vi = std::stoi(token.substr(0, s1));
            const auto s2 = token.find('/', s1 + 1);
            if (s2 == std::string::npos) {
                if (s1 + 1 < token.size())
                    ti = std::stoi(token.substr(s1 + 1));
            } else {
                if (s2 > s1 + 1)
                    ti = std::stoi(token.substr(s1 + 1, s2 - s1 - 1));
                if (s2 + 1 < token.size())
                    ni = std::stoi(token.substr(s2 + 1));
            }
        }
        const VertKey key{fix_index(vi, positions.size()), fix_index(ti, uvs.size()),
                          fix_index(ni, normals.size())};
        if (auto it = vert_cache.find(key); it != vert_cache.end())
            return it->second;

        Vertex v;
        if (key.v >= 0 && static_cast<std::size_t>(key.v) < positions.size())
            v.position = positions[static_cast<std::size_t>(key.v)];
        if (key.vt >= 0 && static_cast<std::size_t>(key.vt) < uvs.size())
            v.uv = uvs[static_cast<std::size_t>(key.vt)];
        if (key.vn >= 0 && static_cast<std::size_t>(key.vn) < normals.size())
            v.normal = normals[static_cast<std::size_t>(key.vn)];
        else
            v.normal = {0, 1, 0};
        v.color = 0xffffffffu;

        const auto idx = static_cast<std::uint32_t>(asset.mesh.vertices.size());
        asset.mesh.vertices.push_back(v);
        vert_cache.emplace(key, idx);
        return idx;
    };

    std::uint32_t active_mat = UINT32_MAX;
    std::string line;
    std::vector<std::uint32_t> face;
    while (std::getline(file, line)) {
        if (line.empty() || line[0] == '#')
            continue;
        std::istringstream ss(line);
        std::string key;
        ss >> key;
        if (key == "v") {
            Vec3 p;
            ss >> p.x >> p.y >> p.z;
            positions.push_back(p);
        } else if (key == "vt") {
            Vec2 uv;
            ss >> uv.x >> uv.y;
            uv.y = 1.0f - uv.y; // OBJ bottom-left -> D3D11 top-left.
            uvs.push_back(uv);
        } else if (key == "vn") {
            Vec3 n;
            ss >> n.x >> n.y >> n.z;
            const float l = std::sqrt(n.x * n.x + n.y * n.y + n.z * n.z);
            normals.push_back(l > 1e-6f ? Vec3{n.x / l, n.y / l, n.z / l} : Vec3{0, 1, 0});
        } else if (key == "mtllib") {
            std::string rest;
            std::getline(ss, rest);
            rest = trim_ws(rest);
            if (!rest.empty())
                for (auto& [k, v] : parse_mtl(path.parent_path() / rest))
                    mtls.emplace(k, std::move(v));
        } else if (key == "usemtl") {
            std::string mat_name;
            ss >> mat_name;
            active_mat = resolve_material(mat_name);
        } else if (key == "f") {
            if (active_mat == UINT32_MAX)
                active_mat = resolve_material("default");
            face.clear();
            std::string tok;
            while (ss >> tok)
                face.push_back(get_vertex(tok));
            for (std::size_t i = 1; i + 1 < face.size(); ++i) {
                // Right-handed CCW outward faces are already CW in left-handed D3D11 view space.
                asset.mesh.indices.push_back(face[0]);
                asset.mesh.indices.push_back(face[i]);
                asset.mesh.indices.push_back(face[i + 1]);
                asset.mesh.triangle_materials.push_back(active_mat);
            }
        }
    }

    if (asset.mesh.vertices.empty() || asset.mesh.indices.empty())
        throw Error("OBJ: no triangle geometry found in " + path.string());

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
    // Sink root slightly (-0.03 * height) so trunks sit firmly on sloped terrain.
    const float sink = 0.03f * height * scale;
    const float max_r = std::max(0.5f * std::hypot(high.x - low.x, high.z - low.z) * scale, 1e-3f);
    const float norm_h = std::max(height * scale, 1e-3f);
    for (auto& v : asset.mesh.vertices) {
        v.position.x = (v.position.x - cx) * scale;
        v.position.y = (v.position.y - low.y) * scale - sink;
        v.position.z = (v.position.z - cz) * scale;
        // Soft canopy AO: darken inner/lower parts and gently tone down bright stylized textures
        // so the trees blend naturally with Sacrifice's painterly terrain.
        const float h_frac = std::clamp((v.position.y + sink) / norm_h, 0.0f, 1.0f);
        const float r_frac = std::clamp(std::hypot(v.position.x, v.position.z) / max_r, 0.0f, 1.0f);
        const float up_frac = std::clamp(v.normal.y * 0.5f + 0.5f, 0.0f, 1.0f);
        const float ao = (0.52f + 0.30f * h_frac + 0.18f * r_frac) * (0.82f + 0.18f * up_frac);
        v.color = pack_rgba(ao * 0.86f, ao * 0.88f, ao * 0.80f, 1.0f);
    }

    log("Loaded OBJ " + path.filename().string() + ": " + std::to_string(asset.mesh.vertices.size()) +
        " verts, " + std::to_string(asset.mesh.indices.size() / 3) + " tris");
    return asset;
}

const std::vector<ModelAsset>& load_obj_folder(const std::filesystem::path& folder_name, float target_height) {
    static std::map<std::pair<std::string, int>, std::vector<ModelAsset>> cache;
    const auto key = std::pair{folder_name.string(), static_cast<int>(std::lround(target_height * 1000.0f))};
    if (auto it = cache.find(key); it != cache.end())
        return it->second;

    std::vector<ModelAsset> models;
    std::vector<std::filesystem::path> roots;
    std::error_code ec;
    auto cwd = std::filesystem::current_path(ec);
    if (!ec) {
        roots.push_back(cwd);
        roots.push_back(cwd / "source");
        if (cwd.has_parent_path()) {
            roots.push_back(cwd.parent_path());
            if (cwd.parent_path().has_parent_path())
                roots.push_back(cwd.parent_path().parent_path());
        }
    }
    if (auto exe = obj_exe_directory(); !exe.empty()) {
        roots.push_back(exe);
        if (exe.has_parent_path()) {
            roots.push_back(exe.parent_path());
            if (exe.parent_path().has_parent_path())
                roots.push_back(exe.parent_path().parent_path());
        }
    }

    std::filesystem::path found_dir;
    for (const auto& root : roots) {
        const auto dir = root / folder_name;
        if (std::filesystem::is_directory(dir, ec)) {
            found_dir = dir;
            break;
        }
    }
    if (!found_dir.empty()) {
        std::vector<std::filesystem::path> files;
        for (const auto& entry : std::filesystem::directory_iterator(found_dir, ec)) {
            if (!entry.is_regular_file(ec))
                continue;
            if (to_lower_ascii(entry.path().extension().string()) == ".obj")
                files.push_back(entry.path());
        }
        std::ranges::sort(files);
        for (const auto& f : files) {
            try {
                models.push_back(load_obj(f, target_height));
            } catch (const std::exception& e) {
                log("Failed to load OBJ " + f.string() + ": " + e.what());
            }
        }
    }
    return cache.emplace(key, std::move(models)).first->second;
}

} // namespace sacrifice
