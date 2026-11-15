#include "TogetherEngine/Assets/AssetPipeline.h"

#include <Windows.h>
#include <bcrypt.h>
#include <nlohmann/json.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <cctype>
#include <cstring>
#include <cwchar>
#include <fstream>
#include <iterator>
#include <limits>
#include <span>
#include <type_traits>
#include <system_error>
#include <unordered_set>
#include <utility>

#include "cgltf.h"
#include "dr_wav.h"
#include "stb_image.h"

namespace together::assets {
namespace {

using Json = nlohmann::json;
constexpr std::uint64_t kMaxImageFileBytes = 64ull * 1024 * 1024;
constexpr std::uint64_t kMaxAudioFileBytes = 32ull * 1024 * 1024;
constexpr std::uint64_t kMaxModelFileBytes = 128ull * 1024 * 1024;
constexpr std::uint64_t kMaxTexturePixels = 16ull * 1024 * 1024;
constexpr std::uint64_t kMaxAudioSamples = 16ull * 1024 * 1024;
constexpr std::uint64_t kDecodedCacheBudgetBytes = 768ull * 1024 * 1024;
constexpr std::size_t kMaxModelVertices = 2ull * 1024 * 1024;
constexpr std::uint64_t kMaxModelIndices = 6ull * 1024 * 1024;

std::string lowercase_extension(const std::filesystem::path& path) {
    std::string filename = path.filename().string();
    std::transform(filename.begin(), filename.end(), filename.begin(), [](unsigned char ch) { return static_cast<char>(std::tolower(ch)); });
    if (filename.ends_with(".twmat.json")) return ".twmat.json";
    std::string ext = path.extension().string();
    std::transform(ext.begin(), ext.end(), ext.begin(), [](unsigned char ch) { return static_cast<char>(std::tolower(ch)); });
    return ext;
}

bool read_file(const std::filesystem::path& path, const std::uint64_t limit,
    std::vector<std::uint8_t>& bytes, std::string& error) {
    std::error_code ec;
    const auto size = std::filesystem::file_size(path, ec);
    if (ec) { error = "Cannot read asset file size: " + path.string(); return false; }
    if (size == 0 || size > limit || size > static_cast<std::uint64_t>((std::numeric_limits<int>::max)())) {
        error = "Asset file is empty or exceeds its import size limit.";
        return false;
    }
    std::ifstream file(path, std::ios::binary);
    if (!file) { error = "Cannot open asset file: " + path.string(); return false; }
    bytes.resize(static_cast<std::size_t>(size));
    file.read(reinterpret_cast<char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
    if (!file || static_cast<std::size_t>(file.gcount()) != bytes.size()) {
        error = "Asset file could not be read completely.";
        bytes.clear();
        return false;
    }
    return true;
}

bool sha256(const std::span<const std::uint8_t> bytes, std::string& hex, std::string& error) {
    BCRYPT_ALG_HANDLE algorithm{};
    BCRYPT_HASH_HANDLE hash{};
    DWORD object_size{}, bytes_written{};
    std::vector<std::uint8_t> object;
    std::array<std::uint8_t, 32> digest{};
    NTSTATUS status = BCryptOpenAlgorithmProvider(&algorithm, BCRYPT_SHA256_ALGORITHM, nullptr, 0);
    if (status < 0) { error = "Windows SHA-256 provider initialization failed."; return false; }
    status = BCryptGetProperty(algorithm, BCRYPT_OBJECT_LENGTH,
        reinterpret_cast<PUCHAR>(&object_size), sizeof(object_size), &bytes_written, 0);
    if (status >= 0) {
        object.resize(object_size);
        status = BCryptCreateHash(algorithm, &hash, object.data(), object_size, nullptr, 0, 0);
    }
    if (status >= 0) status = BCryptHashData(hash, const_cast<PUCHAR>(bytes.data()), static_cast<ULONG>(bytes.size()), 0);
    if (status >= 0) status = BCryptFinishHash(hash, digest.data(), static_cast<ULONG>(digest.size()), 0);
    if (hash) BCryptDestroyHash(hash);
    BCryptCloseAlgorithmProvider(algorithm, 0);
    if (status < 0) { error = "Windows SHA-256 hashing failed."; return false; }
    constexpr char digits[] = "0123456789abcdef";
    hex.resize(digest.size() * 2);
    for (std::size_t i = 0; i < digest.size(); ++i) {
        hex[i * 2] = digits[digest[i] >> 4];
        hex[i * 2 + 1] = digits[digest[i] & 0x0F];
    }
    return true;
}

unsigned quality_limit(const TextureQuality quality) {
    switch (quality) {
    case TextureQuality::Low: return 512;
    case TextureQuality::Medium: return 1024;
    case TextureQuality::High: return 2048;
    case TextureQuality::Original: return 16384;
    }
    return 1024;
}

std::string utf8_path(const std::filesystem::path& path) {
    const auto encoded = path.u8string();
    return {reinterpret_cast<const char*>(encoded.data()), encoded.size()};
}

void update_bounds(Bounds3& bounds, const AssetVertex& vertex, const bool initialize) {
    for (unsigned axis = 0; axis < 3; ++axis) {
        const float value = vertex.position[axis];
        if (initialize) bounds.minimum[axis] = bounds.maximum[axis] = value;
        else {
            bounds.minimum[axis] = (std::min)(bounds.minimum[axis], value);
            bounds.maximum[axis] = (std::max)(bounds.maximum[axis], value);
        }
    }
}

std::string texture_uri(const cgltf_texture_view& view) {
    if (!view.texture || !view.texture->image || !view.texture->image->uri) return {};
    return view.texture->image->uri;
}

bool import_texture(const std::vector<std::uint8_t>& bytes, const std::string& ext,
    const TextureQuality quality, TextureData& texture, AssetMetadata& metadata, std::string& error) {
    int width{}, height{}, channels{};
    if (!stbi_info_from_memory(bytes.data(), static_cast<int>(bytes.size()), &width, &height, &channels)) {
        error = std::string("Image header is invalid: ") + stbi_failure_reason();
        return false;
    }
    if (width <= 0 || height <= 0 || width > 32768 || height > 32768 ||
        static_cast<std::uint64_t>(width) * static_cast<std::uint64_t>(height) > kMaxTexturePixels) {
        error = "Image dimensions exceed the 16-megapixel import limit.";
        return false;
    }
    int decoded_width{}, decoded_height{}, decoded_channels{};
    stbi_uc* raw = stbi_load_from_memory(bytes.data(), static_cast<int>(bytes.size()),
        &decoded_width, &decoded_height, &decoded_channels, STBI_rgb_alpha);
    if (!raw) { error = std::string("Image decode failed: ") + stbi_failure_reason(); return false; }
    std::unique_ptr<stbi_uc, decltype(&stbi_image_free)> decoded(raw, stbi_image_free);
    unsigned target_width = static_cast<unsigned>(decoded_width);
    unsigned target_height = static_cast<unsigned>(decoded_height);
    const unsigned limit = quality_limit(quality);
    if (target_width > limit || target_height > limit) {
        const double ratio = (std::min)(static_cast<double>(limit) / target_width, static_cast<double>(limit) / target_height);
        target_width = (std::max)(1u, static_cast<unsigned>(std::floor(target_width * ratio)));
        target_height = (std::max)(1u, static_cast<unsigned>(std::floor(target_height * ratio)));
    }
    texture.width = target_width;
    texture.height = target_height;
    texture.source_channels = static_cast<unsigned>(channels);
    texture.format = ext == ".jpg" || ext == ".jpeg" ? "JPEG" : ext == ".png" ? "PNG" : "TGA";
    texture.mip_levels = 1;
    for (unsigned w = target_width, h = target_height; w > 1 || h > 1; ++texture.mip_levels) {
        w = (std::max)(1u, w / 2);
        h = (std::max)(1u, h / 2);
    }
    texture.rgba8.resize(static_cast<std::size_t>(target_width) * target_height * 4);
    if (target_width == static_cast<unsigned>(decoded_width) && target_height == static_cast<unsigned>(decoded_height)) {
        std::memcpy(texture.rgba8.data(), decoded.get(), texture.rgba8.size());
    } else {
        for (unsigned y = 0; y < target_height; ++y) {
            const unsigned source_y = (std::min)(static_cast<unsigned>(decoded_height - 1),
                static_cast<unsigned>((static_cast<std::uint64_t>(y) * decoded_height) / target_height));
            for (unsigned x = 0; x < target_width; ++x) {
                const unsigned source_x = (std::min)(static_cast<unsigned>(decoded_width - 1),
                    static_cast<unsigned>((static_cast<std::uint64_t>(x) * decoded_width) / target_width));
                const std::size_t source = (static_cast<std::size_t>(source_y) * decoded_width + source_x) * 4;
                const std::size_t destination = (static_cast<std::size_t>(y) * target_width + x) * 4;
                std::memcpy(texture.rgba8.data() + destination, decoded.get() + source, 4);
            }
        }
    }
    unsigned source_width = target_width;
    unsigned source_height = target_height;
    const std::vector<std::uint8_t>* source_pixels = &texture.rgba8;
    while (source_width > 1 || source_height > 1) {
        const unsigned next_width = (std::max)(1u, source_width / 2);
        const unsigned next_height = (std::max)(1u, source_height / 2);
        std::vector<std::uint8_t> next(static_cast<std::size_t>(next_width) * next_height * 4);
        for (unsigned y = 0; y < next_height; ++y) for (unsigned x = 0; x < next_width; ++x) {
            for (unsigned channel = 0; channel < 4; ++channel) {
                unsigned sum = 0, samples = 0;
                for (unsigned dy = 0; dy < 2; ++dy) for (unsigned dx = 0; dx < 2; ++dx) {
                    const unsigned sx = (std::min)(source_width - 1, x * 2 + dx);
                    const unsigned sy = (std::min)(source_height - 1, y * 2 + dy);
                    sum += (*source_pixels)[(static_cast<std::size_t>(sy) * source_width + sx) * 4 + channel];
                    ++samples;
                }
                next[(static_cast<std::size_t>(y) * next_width + x) * 4 + channel] = static_cast<std::uint8_t>(sum / samples);
            }
        }
        texture.mipmaps.push_back(std::move(next));
        source_width = next_width;
        source_height = next_height;
        source_pixels = &texture.mipmaps.back();
    }
    metadata.width = texture.width;
    metadata.height = texture.height;
    metadata.channels = texture.source_channels;
    metadata.mip_levels = texture.mip_levels;
    metadata.format = texture.format;
    metadata.decoded_bytes = texture.rgba8.size();
    for (const auto& mip : texture.mipmaps) metadata.decoded_bytes += mip.size();
    return true;
}

bool import_model(const std::filesystem::path& path, ModelData& model, AssetMetadata& metadata, std::string& error) {
    cgltf_options options{};
    cgltf_data* parsed{};
    const std::string path_utf8 = utf8_path(path);
    cgltf_result result = cgltf_parse_file(&options, path_utf8.c_str(), &parsed);
    if (result != cgltf_result_success || !parsed) {
        error = "glTF parser rejected the file (cgltf result " + std::to_string(result) + ").";
        return false;
    }
    std::unique_ptr<cgltf_data, decltype(&cgltf_free)> data(parsed, cgltf_free);
    result = cgltf_load_buffers(&options, data.get(), path_utf8.c_str());
    if (result != cgltf_result_success) { error = "glTF buffers or external resources could not be loaded."; return false; }
    result = cgltf_validate(data.get());
    if (result != cgltf_result_success) { error = "glTF structural validation failed (cgltf result " + std::to_string(result) + ")."; return false; }
    if (data->skins_count || data->animations_count) {
        error = "This importer renders static glTF only; skinned meshes and animation clips are not supported.";
        return false;
    }

    bool initialized_bounds = false;
    std::uint64_t vertex_total = 0, index_total = 0;
    for (cgltf_size mesh_index = 0; mesh_index < data->meshes_count; ++mesh_index) {
        const cgltf_mesh& mesh = data->meshes[mesh_index];
        std::array<float, 16> mesh_transform{1,0,0,0, 0,1,0,0, 0,0,1,0, 0,0,0,1};
        unsigned mesh_instances = 0;
        for (cgltf_size node_index = 0; node_index < data->nodes_count; ++node_index) {
            if (data->nodes[node_index].mesh == &mesh) {
                if (++mesh_instances > 1) {
                    error = "glTF mesh instancing through multiple nodes is not supported yet.";
                    return false;
                }
                cgltf_node_transform_world(&data->nodes[node_index], mesh_transform.data());
            }
        }
        // Reflect glTF's right-handed Z axis into the renderer's left-handed world.
        for (unsigned column = 0; column < 4; ++column)
            for (unsigned row = 0; row < 4; ++row)
                if ((row == 2) != (column == 2)) mesh_transform[column * 4 + row] = -mesh_transform[column * 4 + row];
        if (!std::all_of(mesh_transform.begin(), mesh_transform.end(), [](float value) { return std::isfinite(value); })) {
            error = "glTF mesh node contains a non-finite world transform.";
            return false;
        }
        const float transform_determinant = mesh_transform[0] * (mesh_transform[5] * mesh_transform[10] - mesh_transform[9] * mesh_transform[6]) -
            mesh_transform[4] * (mesh_transform[1] * mesh_transform[10] - mesh_transform[9] * mesh_transform[2]) +
            mesh_transform[8] * (mesh_transform[1] * mesh_transform[6] - mesh_transform[5] * mesh_transform[2]);
        if (std::abs(transform_determinant) < 1e-8f) {
            error = "glTF mesh node has a singular transform and cannot be rendered safely.";
            return false;
        }
        for (cgltf_size primitive_index = 0; primitive_index < mesh.primitives_count; ++primitive_index) {
            const cgltf_primitive& primitive = mesh.primitives[primitive_index];
            if (primitive.targets_count) {
                error = "glTF morph targets are not supported by the static mesh renderer.";
                return false;
            }
            if (primitive.material && (primitive.material->double_sided || primitive.material->alpha_mode != cgltf_alpha_mode_opaque)) {
                error = "glTF double-sided and alpha-masked/blended materials are not supported by the current opaque renderer.";
                return false;
            }
            if (primitive.type != cgltf_primitive_type_triangles) {
                error = "Only triangle-list glTF primitives are currently supported.";
                return false;
            }
            const cgltf_accessor* positions = nullptr;
            const cgltf_accessor* normals = nullptr;
            const cgltf_accessor* uvs = nullptr;
            const cgltf_accessor* tangents = nullptr;
            for (cgltf_size attribute_index = 0; attribute_index < primitive.attributes_count; ++attribute_index) {
                const auto& attribute = primitive.attributes[attribute_index];
                if (attribute.type == cgltf_attribute_type_position) positions = attribute.data;
                else if (attribute.type == cgltf_attribute_type_normal) normals = attribute.data;
                else if (attribute.type == cgltf_attribute_type_texcoord && attribute.index == 0) uvs = attribute.data;
                else if (attribute.type == cgltf_attribute_type_tangent) tangents = attribute.data;
            }
            if (!positions || positions->count == 0 || positions->count > kMaxModelVertices ||
                vertex_total + positions->count > kMaxModelVertices ||
                (normals && normals->count != positions->count) || (uvs && uvs->count != positions->count) ||
                (tangents && tangents->count != positions->count)) {
                error = "glTF primitive has missing/incompatible attributes or exceeds the 2-million-vertex import limit.";
                return false;
            }
            ImportedPrimitive imported;
            imported.transform = mesh_transform;
            imported.name = mesh.name ? mesh.name : ("Mesh " + std::to_string(mesh_index + 1));
            imported.vertices.resize(positions->count);
            for (cgltf_size i = 0; i < positions->count; ++i) {
                cgltf_float values[4]{};
                if (!cgltf_accessor_read_float(positions, i, values, 3)) { error = "Could not decode glTF POSITION accessor."; return false; }
                AssetVertex& vertex = imported.vertices[i];
                for (unsigned axis = 0; axis < 3; ++axis) vertex.position[axis] = values[axis];
                vertex.position[2] = -vertex.position[2];
                if (normals) {
                    if (!cgltf_accessor_read_float(normals, i, values, 3)) { error = "Could not decode glTF NORMAL accessor."; return false; }
                    for (unsigned axis = 0; axis < 3; ++axis) vertex.normal[axis] = values[axis];
                    vertex.normal[2] = -vertex.normal[2];
                    for (float component : vertex.normal) if (!std::isfinite(component)) { error = "glTF contains non-finite normals."; return false; }
                }
                if (uvs) {
                    if (!cgltf_accessor_read_float(uvs, i, values, 2)) { error = "Could not decode glTF TEXCOORD_0 accessor."; return false; }
                    vertex.uv[0] = values[0]; vertex.uv[1] = values[1];
                    if (!std::isfinite(vertex.uv[0]) || !std::isfinite(vertex.uv[1])) { error = "glTF contains non-finite texture coordinates."; return false; }
                }
                if (tangents) {
                    if (!cgltf_accessor_read_float(tangents, i, values, 4)) { error = "Could not decode glTF TANGENT accessor."; return false; }
                    vertex.tangent[0] = values[0]; vertex.tangent[1] = values[1]; vertex.tangent[2] = -values[2]; vertex.tangent[3] = -values[3];
                    if (!std::all_of(std::begin(vertex.tangent), std::end(vertex.tangent), [](float component) { return std::isfinite(component); })) {
                        error = "glTF contains non-finite tangent vectors."; return false;
                    }
                }
                for (float component : vertex.position) if (!std::isfinite(component)) { error = "glTF contains non-finite vertex positions."; return false; }
                update_bounds(imported.bounds, vertex, i == 0);
                if (!initialized_bounds) { model.bounds = imported.bounds; initialized_bounds = true; }
                else for (unsigned axis = 0; axis < 3; ++axis) {
                    model.bounds.minimum[axis] = (std::min)(model.bounds.minimum[axis], imported.bounds.minimum[axis]);
                    model.bounds.maximum[axis] = (std::max)(model.bounds.maximum[axis], imported.bounds.maximum[axis]);
                }
            }
            if (primitive.indices) {
                if (index_total + primitive.indices->count > kMaxModelIndices) {
                    error = "glTF model exceeds the 6-million-index import limit.";
                    return false;
                }
                imported.indices.resize(primitive.indices->count);
                for (cgltf_size i = 0; i < primitive.indices->count; ++i) {
                    const std::size_t index = cgltf_accessor_read_index(primitive.indices, i);
                    if (index >= imported.vertices.size()) { error = "glTF index references a vertex outside its primitive."; return false; }
                    imported.indices[i] = static_cast<std::uint32_t>(index);
                }
            } else {
                if (index_total + imported.vertices.size() > kMaxModelIndices) {
                    error = "glTF model exceeds the 6-million-index import limit.";
                    return false;
                }
                imported.indices.resize(imported.vertices.size());
                for (std::size_t i = 0; i < imported.indices.size(); ++i) imported.indices[i] = static_cast<std::uint32_t>(i);
            }
            if (imported.indices.empty() || imported.indices.size() % 3 != 0) { error = "glTF triangle primitive has an invalid index count."; return false; }
            for (std::size_t index = 0; index < imported.indices.size(); index += 3)
                std::swap(imported.indices[index + 1], imported.indices[index + 2]);
            if (!normals) {
                for (auto& vertex : imported.vertices) vertex.normal[0] = vertex.normal[1] = vertex.normal[2] = 0;
                for (std::size_t index = 0; index < imported.indices.size(); index += 3) {
                    auto& a = imported.vertices[imported.indices[index]];
                    auto& b = imported.vertices[imported.indices[index + 1]];
                    auto& c = imported.vertices[imported.indices[index + 2]];
                    const float ab[3]{b.position[0] - a.position[0], b.position[1] - a.position[1], b.position[2] - a.position[2]};
                    const float ac[3]{c.position[0] - a.position[0], c.position[1] - a.position[1], c.position[2] - a.position[2]};
                    const float face[3]{ab[1]*ac[2] - ab[2]*ac[1], ab[2]*ac[0] - ab[0]*ac[2], ab[0]*ac[1] - ab[1]*ac[0]};
                    for (auto* vertex : {&a, &b, &c}) for (unsigned axis = 0; axis < 3; ++axis) vertex->normal[axis] += face[axis];
                }
                for (auto& vertex : imported.vertices) {
                    const float length = std::sqrt(vertex.normal[0]*vertex.normal[0] + vertex.normal[1]*vertex.normal[1] + vertex.normal[2]*vertex.normal[2]);
                    if (length > 1e-8f) for (float& component : vertex.normal) component /= length;
                    else { vertex.normal[0] = 0; vertex.normal[1] = 1; vertex.normal[2] = 0; }
                }
            }
            if (!tangents) {
                std::vector<std::array<float, 3>> tangent_sums(imported.vertices.size());
                std::vector<std::array<float, 3>> bitangent_sums(imported.vertices.size());
                for (std::size_t index = 0; index < imported.indices.size(); index += 3) {
                    const std::uint32_t ia = imported.indices[index], ib = imported.indices[index + 1], ic = imported.indices[index + 2];
                    const auto& a = imported.vertices[ia]; const auto& b = imported.vertices[ib]; const auto& c = imported.vertices[ic];
                    const float e1[3]{b.position[0]-a.position[0], b.position[1]-a.position[1], b.position[2]-a.position[2]};
                    const float e2[3]{c.position[0]-a.position[0], c.position[1]-a.position[1], c.position[2]-a.position[2]};
                    const float du1 = b.uv[0]-a.uv[0], dv1 = b.uv[1]-a.uv[1];
                    const float du2 = c.uv[0]-a.uv[0], dv2 = c.uv[1]-a.uv[1];
                    const float determinant = du1*dv2 - du2*dv1;
                    if (std::abs(determinant) < 1e-8f) continue;
                    const float inverse = 1.0f / determinant;
                    const std::array<float,3> tangent{(e1[0]*dv2-e2[0]*dv1)*inverse,
                        (e1[1]*dv2-e2[1]*dv1)*inverse, (e1[2]*dv2-e2[2]*dv1)*inverse};
                    const std::array<float,3> bitangent{(e2[0]*du1-e1[0]*du2)*inverse,
                        (e2[1]*du1-e1[1]*du2)*inverse, (e2[2]*du1-e1[2]*du2)*inverse};
                    for (const std::uint32_t vertex_index : {ia, ib, ic}) for (unsigned axis = 0; axis < 3; ++axis) {
                        tangent_sums[vertex_index][axis] += tangent[axis];
                        bitangent_sums[vertex_index][axis] += bitangent[axis];
                    }
                }
                for (std::size_t i = 0; i < imported.vertices.size(); ++i) {
                    auto& vertex = imported.vertices[i];
                    float projection = 0;
                    for (unsigned axis = 0; axis < 3; ++axis) projection += vertex.normal[axis] * tangent_sums[i][axis];
                    float length = 0;
                    for (unsigned axis = 0; axis < 3; ++axis) {
                        vertex.tangent[axis] = tangent_sums[i][axis] - vertex.normal[axis] * projection;
                        length += vertex.tangent[axis] * vertex.tangent[axis];
                    }
                    length = std::sqrt(length);
                    if (length < 1e-8f) {
                        const float fallback[3]{std::abs(vertex.normal[0]) < 0.9f ? 1.0f : 0.0f,
                            std::abs(vertex.normal[0]) < 0.9f ? 0.0f : 1.0f, 0.0f};
                        float fallback_dot = 0;
                        for (unsigned axis = 0; axis < 3; ++axis) fallback_dot += fallback[axis] * vertex.normal[axis];
                        for (unsigned axis = 0; axis < 3; ++axis) vertex.tangent[axis] = fallback[axis] - fallback_dot * vertex.normal[axis];
                        length = std::sqrt(vertex.tangent[0]*vertex.tangent[0] + vertex.tangent[1]*vertex.tangent[1] + vertex.tangent[2]*vertex.tangent[2]);
                        if (length < 1e-8f) { vertex.tangent[0] = 1; vertex.tangent[1] = 0; vertex.tangent[2] = 0; }
                        else for (unsigned axis = 0; axis < 3; ++axis) vertex.tangent[axis] /= length;
                    } else for (unsigned axis = 0; axis < 3; ++axis) vertex.tangent[axis] /= length;
                    const float cross_x = vertex.normal[1]*vertex.tangent[2] - vertex.normal[2]*vertex.tangent[1];
                    const float cross_y = vertex.normal[2]*vertex.tangent[0] - vertex.normal[0]*vertex.tangent[2];
                    const float cross_z = vertex.normal[0]*vertex.tangent[1] - vertex.normal[1]*vertex.tangent[0];
                    const float orientation = cross_x*bitangent_sums[i][0] + cross_y*bitangent_sums[i][1] + cross_z*bitangent_sums[i][2];
                    vertex.tangent[3] = orientation < 0 ? -1.0f : 1.0f;
                }
            }
            if (primitive.material) {
                const cgltf_material& source = *primitive.material;
                imported.material_name = source.name ? source.name : "Material";
                if (source.has_pbr_metallic_roughness) {
                    for (unsigned i = 0; i < 4; ++i) imported.base_color[i] = source.pbr_metallic_roughness.base_color_factor[i];
                    imported.metallic = source.pbr_metallic_roughness.metallic_factor;
                    imported.roughness = source.pbr_metallic_roughness.roughness_factor;
                    imported.texture_uris[0] = texture_uri(source.pbr_metallic_roughness.base_color_texture);
                    imported.texture_uris[2] = texture_uri(source.pbr_metallic_roughness.roughness_texture);
                    imported.texture_uris[3] = texture_uri(source.pbr_metallic_roughness.metallic_texture);
                }
                imported.texture_uris[1] = texture_uri(source.normal_texture);
                imported.texture_uris[4] = texture_uri(source.occlusion_texture);
                imported.texture_uris[5] = texture_uri(source.emissive_texture);
                for (unsigned i = 0; i < 3; ++i) imported.emission[i] = source.emissive_factor[i];
                for (float component : imported.base_color) if (!std::isfinite(component) || component < 0 || component > 1) {
                    error = "glTF base-color factor must be finite and within [0, 1].";
                    return false;
                }
                if (!std::isfinite(imported.metallic) || imported.metallic < 0 || imported.metallic > 1 ||
                    !std::isfinite(imported.roughness) || imported.roughness < 0 || imported.roughness > 1 ||
                    !std::all_of(imported.emission.begin(), imported.emission.end(), [](float value) { return std::isfinite(value) && value >= 0 && value <= 64; })) {
                    error = "glTF metallic/roughness/emission material factors are outside supported finite ranges.";
                    return false;
                }
            }
            vertex_total += imported.vertices.size();
            index_total += imported.indices.size();
            model.primitives.push_back(std::move(imported));
        }
    }
    if (model.primitives.empty()) { error = "glTF contains no triangle mesh primitives."; return false; }

    for (cgltf_size skin_index = 0; skin_index < data->skins_count; ++skin_index) {
        const cgltf_skin& source = data->skins[skin_index];
        SkeletonDefinition skin;
        skin.joint_names.reserve(source.joints_count);
        skin.parent_indices.assign(source.joints_count, -1);
        skin.inverse_bind_matrices.resize(source.joints_count);
        for (cgltf_size i = 0; i < source.joints_count; ++i) {
            const cgltf_node* joint = source.joints[i];
            skin.joint_names.push_back(joint->name ? joint->name : ("Joint " + std::to_string(i)));
            for (cgltf_size parent_index = 0; parent_index < source.joints_count; ++parent_index) {
                const cgltf_node* parent = source.joints[parent_index];
                for (cgltf_size child_index = 0; child_index < parent->children_count; ++child_index) {
                    if (parent->children[child_index] == joint) skin.parent_indices[i] = static_cast<std::int32_t>(parent_index);
                }
            }
            if (source.inverse_bind_matrices && !cgltf_accessor_read_float(source.inverse_bind_matrices, i,
                skin.inverse_bind_matrices[i].data(), 16)) { error = "Could not decode glTF inverse bind matrices."; return false; }
            if (!source.inverse_bind_matrices) {
                skin.inverse_bind_matrices[i] = {1,0,0,0, 0,1,0,0, 0,0,1,0, 0,0,0,1};
            }
        }
        model.skins.push_back(std::move(skin));
    }
    metadata.mesh_count = static_cast<unsigned>(model.primitives.size());
    metadata.vertex_count = static_cast<unsigned>((std::min)(vertex_total, static_cast<std::uint64_t>(UINT32_MAX)));
    metadata.index_count = static_cast<unsigned>((std::min)(index_total, static_cast<std::uint64_t>(UINT32_MAX)));
    for (const auto& skin : model.skins) metadata.joint_count += static_cast<unsigned>(skin.joint_names.size());
    metadata.format = lowercase_extension(path) == ".glb" ? "glTF 2.0 binary" : "glTF 2.0 JSON";
    metadata.decoded_bytes = vertex_total * sizeof(AssetVertex) + index_total * sizeof(std::uint32_t);
    return true;
}

bool import_audio(const std::vector<std::uint8_t>& bytes, AudioData& audio, std::string& error) {
    drwav wav{};
    if (!drwav_init_memory(&wav, bytes.data(), bytes.size(), nullptr)) { error = "WAV container or format is invalid."; return false; }
    const std::uint64_t samples = wav.totalPCMFrameCount * wav.channels;
    if (wav.channels == 0 || wav.channels > 8 || wav.sampleRate < 8000 || wav.sampleRate > 384000 ||
        samples == 0 || samples > kMaxAudioSamples) {
        drwav_uninit(&wav);
        error = "WAV channels, sample rate, or decoded sample count exceeds supported limits.";
        return false;
    }
    audio.channels = wav.channels;
    audio.sample_rate = wav.sampleRate;
    audio.frame_count = wav.totalPCMFrameCount;
    audio.interleaved_samples.resize(static_cast<std::size_t>(samples));
    const drwav_uint64 read_frames = drwav_read_pcm_frames_f32(&wav, wav.totalPCMFrameCount, audio.interleaved_samples.data());
    drwav_uninit(&wav);
    if (read_frames != audio.frame_count) { error = "WAV decoder did not return all declared frames."; return false; }
    return true;
}

std::size_t payload_size(const AssetPayload& payload) {
    return std::visit([](const auto& value) -> std::size_t {
        using T = std::decay_t<decltype(value)>;
        if constexpr (std::is_same_v<T, TextureData>) {
            std::size_t bytes = value.rgba8.size();
            for (const auto& mip : value.mipmaps) bytes += mip.size();
            return bytes;
        }
        else if constexpr (std::is_same_v<T, AudioData>) return value.interleaved_samples.size() * sizeof(float);
        else if constexpr (std::is_same_v<T, ModelData>) {
            std::size_t bytes = 0;
            for (const auto& primitive : value.primitives) bytes += primitive.vertices.size() * sizeof(AssetVertex) + primitive.indices.size() * sizeof(std::uint32_t);
            return bytes;
        } else return sizeof(MaterialDefinition);
    }, payload);
}

bool path_is_within(const std::filesystem::path& parent, const std::filesystem::path& candidate) {
    const auto parent_normal = parent.lexically_normal();
    const auto candidate_normal = candidate.lexically_normal();
    auto parent_it = parent_normal.begin();
    const auto parent_end = parent_normal.end();
    auto candidate_it = candidate_normal.begin();
    const auto candidate_end = candidate_normal.end();
    for (; parent_it != parent_end; ++parent_it, ++candidate_it) {
        if (candidate_it == candidate_end) return false;
        const std::wstring a = parent_it->native();
        const std::wstring b = candidate_it->native();
        if (CompareStringOrdinal(a.c_str(), static_cast<int>(a.size()), b.c_str(), static_cast<int>(b.size()), TRUE) != CSTR_EQUAL)
            return false;
    }
    return candidate_it != candidate_end;
}

bool decode_uri_path(const std::string_view uri, std::filesystem::path& path, std::string& error) {
    if (uri.empty() || uri.starts_with("data:")) { path.clear(); return true; }
    if (uri.find("://") != std::string_view::npos || uri.starts_with("file:") || uri.find_first_of("?#") != std::string_view::npos) {
        error = "External glTF URI schemes and query/fragment suffixes are unsupported in portable asset packages.";
        return false;
    }
    std::string decoded;
    decoded.reserve(uri.size());
    const auto hex = [](const char ch) -> int {
        if (ch >= '0' && ch <= '9') return ch - '0';
        if (ch >= 'a' && ch <= 'f') return ch - 'a' + 10;
        if (ch >= 'A' && ch <= 'F') return ch - 'A' + 10;
        return -1;
    };
    for (std::size_t i = 0; i < uri.size(); ++i) {
        if (uri[i] == '%') {
            if (i + 2 >= uri.size() || hex(uri[i+1]) < 0 || hex(uri[i+2]) < 0) {
                error = "glTF external resource URI contains invalid percent encoding.";
                return false;
            }
            decoded.push_back(static_cast<char>((hex(uri[i+1]) << 4) | hex(uri[i+2])));
            i += 2;
        } else decoded.push_back(uri[i] == '/' ? '\\' : uri[i]);
    }
    path = std::filesystem::u8path(decoded);
    if (path.is_absolute() || path.has_root_name() || path.has_root_directory()) {
        error = "Absolute glTF resource paths cannot be packaged portably.";
        return false;
    }
    error.clear();
    return true;
}

bool collect_gltf_external_uris(const std::filesystem::path& path, std::vector<std::filesystem::path>& uris, std::string& error) {
    cgltf_options options{};
    cgltf_data* parsed{};
    const std::string path_utf8 = utf8_path(path);
    const cgltf_result result = cgltf_parse_file(&options, path_utf8.c_str(), &parsed);
    if (result != cgltf_result_success || !parsed) { error = "Could not inspect glTF dependencies for portable import."; return false; }
    std::unique_ptr<cgltf_data, decltype(&cgltf_free)> data(parsed, cgltf_free);
    std::unordered_set<std::wstring> unique;
    const auto add = [&](const char* uri) {
        if (!uri || !*uri) return true;
        std::filesystem::path relative;
        if (!decode_uri_path(uri, relative, error)) return false;
        if (relative.empty()) return true;
        const auto normalized = relative.lexically_normal();
        if (normalized == L"." || normalized.empty()) return true;
        if (unique.insert(normalized.native()).second) uris.push_back(normalized);
        return true;
    };
    for (cgltf_size i = 0; i < data->buffers_count; ++i) if (!add(data->buffers[i].uri)) return false;
    for (cgltf_size i = 0; i < data->images_count; ++i) if (!add(data->images[i].uri)) return false;
    return true;
}

bool valid_asset_id(const std::string& id) {
    const auto separator = id.find(':');
    if (separator == std::string::npos || id.size() - separator - 1 != 64) return false;
    const std::string type = id.substr(0, separator);
    if (type != "texture" && type != "model" && type != "audio" && type != "material") return false;
    return std::all_of(id.begin() + static_cast<std::ptrdiff_t>(separator + 1), id.end(), [](const unsigned char ch) {
        return (ch >= '0' && ch <= '9') || (ch >= 'a' && ch <= 'f');
    });
}

} // namespace

const char* asset_type_name(const AssetType type) noexcept {
    switch (type) {
    case AssetType::Texture: return "texture";
    case AssetType::Model: return "model";
    case AssetType::Audio: return "audio";
    case AssetType::Material: return "material";
    }
    return "unknown";
}

const char* texture_slot_name(const TextureSlot slot) noexcept {
    constexpr std::array<const char*, 6> names{"baseColor", "normal", "roughness", "metallic", "occlusion", "emission"};
    const auto index = static_cast<std::size_t>(slot);
    return index < names.size() ? names[index] : "unknown";
}

AssetImportResult AssetRegistry::register_asset(AssetMetadata metadata, std::shared_ptr<const AssetPayload> payload_value) {
    const bool matching_payload = payload_value &&
        ((metadata.type == AssetType::Texture && std::holds_alternative<TextureData>(*payload_value)) ||
         (metadata.type == AssetType::Model && std::holds_alternative<ModelData>(*payload_value)) ||
         (metadata.type == AssetType::Audio && std::holds_alternative<AudioData>(*payload_value)) ||
         (metadata.type == AssetType::Material && std::holds_alternative<MaterialDefinition>(*payload_value)));
    const std::string expected_prefix = std::string(asset_type_name(metadata.type)) + ":";
    if (metadata.id.empty() || !metadata.id.starts_with(expected_prefix) || !matching_payload)
        return {{}, {}, false, "Asset registry rejected an empty/mistyped ID or payload."};
    std::scoped_lock lock(mutex_);
    if (const auto found = by_id_.find(metadata.id); found != by_id_.end()) {
        const Slot& slot = slots_[found->second];
        metadata = slot.metadata;
        return {metadata, slot.payload, true, {}};
    }
    const std::uint32_t index = static_cast<std::uint32_t>(slots_.size());
    metadata.handle = {index, 1};
    slots_.push_back({1, metadata, std::move(payload_value)});
    by_id_.emplace(metadata.id, index);
    return {metadata, slots_.back().payload, false, {}};
}

bool AssetRegistry::resolve(const AssetHandle handle, AssetMetadata& output) const {
    std::scoped_lock lock(mutex_);
    if (!handle.valid() || handle.index >= slots_.size()) return false;
    const Slot& slot = slots_[handle.index];
    if (slot.generation != handle.generation || !slot.metadata.handle.valid()) return false;
    output = slot.metadata;
    return true;
}

bool AssetRegistry::find_by_id(const std::string& id, AssetMetadata& output) const {
    std::scoped_lock lock(mutex_);
    const auto found = by_id_.find(id);
    if (found == by_id_.end()) return false;
    output = slots_[found->second].metadata;
    return true;
}

bool AssetRegistry::update_source(const std::string& id, std::filesystem::path source_path, std::string name) {
    std::scoped_lock lock(mutex_);
    const auto found = by_id_.find(id);
    if (found == by_id_.end()) return false;
    auto& metadata = slots_[found->second].metadata;
    metadata.source_path = std::move(source_path);
    metadata.name = std::move(name);
    return true;
}

bool AssetRegistry::remove(const std::string& id) {
    std::scoped_lock lock(mutex_);
    const auto found = by_id_.find(id);
    if (found == by_id_.end()) return false;
    Slot& slot = slots_[found->second];
    slot.payload.reset();
    slot.metadata = {};
    if (++slot.generation == 0) ++slot.generation;
    by_id_.erase(found);
    return true;
}

std::shared_ptr<const AssetPayload> AssetRegistry::payload(const AssetHandle handle) const {
    std::scoped_lock lock(mutex_);
    if (!handle.valid() || handle.index >= slots_.size()) return {};
    const Slot& slot = slots_[handle.index];
    return slot.generation == handle.generation ? slot.payload : nullptr;
}

std::vector<AssetMetadata> AssetRegistry::snapshot() const {
    std::scoped_lock lock(mutex_);
    std::vector<AssetMetadata> result;
    result.reserve(slots_.size());
    for (const Slot& slot : slots_) if (slot.metadata.handle.valid()) result.push_back(slot.metadata);
    return result;
}

bool MaterialDefinition::validate(std::string& error) const {
    if (name.empty() || name.size() > 256) { error = "Material name must contain 1–256 UTF-8 bytes."; return false; }
    if (id.size() > 256) { error = "Material ID cannot exceed 256 UTF-8 bytes."; return false; }
    for (const auto& texture_id : textures) if (texture_id.size() > 256) { error = "Material texture IDs cannot exceed 256 UTF-8 bytes."; return false; }
    for (float value : base_color) if (!std::isfinite(value) || value < 0 || value > 1) { error = "Material base color components must be within [0, 1]."; return false; }
    for (float value : emission) if (!std::isfinite(value) || value < 0 || value > 64) { error = "Material emission components must be within [0, 64]."; return false; }
    if (!std::isfinite(metallic) || metallic < 0 || metallic > 1 || !std::isfinite(roughness) || roughness < 0 || roughness > 1) {
        error = "Material metallic and roughness must be within [0, 1].";
        return false;
    }
    error.clear();
    return true;
}

bool MaterialDefinition::save(const std::filesystem::path& path, std::string& error) const {
    if (path.empty() || !validate(error)) return false;
    Json json;
    json["format"] = "TogetherEngineMaterial";
    json["version"] = 1;
    json["id"] = id;
    json["name"] = name;
    json["baseColor"] = base_color;
    json["metallic"] = metallic;
    json["roughness"] = roughness;
    json["emission"] = emission;
    json["textures"] = Json::object();
    for (std::size_t i = 0; i < textures.size(); ++i) json["textures"][texture_slot_name(static_cast<TextureSlot>(i))] = textures[i];
    std::filesystem::path temporary = path;
    temporary += L".tmp";
    std::error_code ec;
    std::filesystem::create_directories(path.parent_path(), ec);
    {
        std::ofstream file(temporary, std::ios::binary | std::ios::trunc);
        if (!file) { error = "Cannot create material file."; return false; }
        file << json.dump(2) << '\n';
        file.flush();
        if (!file) { error = "Cannot write complete material file."; return false; }
    }
    if (!MoveFileExW(temporary.c_str(), path.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
        error = "Cannot replace material file (Windows error " + std::to_string(GetLastError()) + ").";
        std::filesystem::remove(temporary, ec);
        return false;
    }
    error.clear();
    return true;
}

bool MaterialDefinition::load(const std::filesystem::path& path, MaterialDefinition& output, std::string& error) {
    std::ifstream file(path, std::ios::binary);
    if (!file) { error = "Cannot open material file."; return false; }
    try {
        Json json = Json::parse(file, nullptr, true, false);
        if (json.is_discarded() || !json.is_object() || json.value("format", "") != "TogetherEngineMaterial" || json.value("version", 0) != 1) {
            error = "Material JSON has an unsupported format or version.";
            return false;
        }
        MaterialDefinition parsed;
        parsed.id = json.value("id", "");
        parsed.name = json.at("name").get<std::string>();
        parsed.base_color = json.at("baseColor").get<std::array<float, 4>>();
        parsed.metallic = json.at("metallic").get<float>();
        parsed.roughness = json.at("roughness").get<float>();
        parsed.emission = json.at("emission").get<std::array<float, 3>>();
        const Json& texture_json = json.at("textures");
        for (std::size_t i = 0; i < parsed.textures.size(); ++i)
            parsed.textures[i] = texture_json.value(texture_slot_name(static_cast<TextureSlot>(i)), std::string{});
        if (!parsed.validate(error)) return false;
        output = std::move(parsed);
        error.clear();
        return true;
    } catch (const std::exception& exception) {
        error = std::string("Material JSON is invalid: ") + exception.what();
        return false;
    }
}

AssetPipeline::AssetPipeline(std::filesystem::path project_root)
    : project_root_(std::filesystem::absolute(std::move(project_root)).lexically_normal()),
      registry_index_path_(project_root_ / L".together" / L"asset_registry.json"),
      worker_([this](const std::stop_token stop) { worker_loop(stop); }) {
    std::error_code root_error;
    const auto canonical_root = std::filesystem::weakly_canonical(project_root_, root_error);
    if (!root_error) project_root_ = canonical_root;
    registry_index_path_ = project_root_ / L".together" / L"asset_registry.json";
    restore_registry_index();
}

AssetPipeline::~AssetPipeline() {
    worker_.request_stop();
    queue_ready_.notify_all();
    if (worker_.joinable()) worker_.join();
}

bool AssetPipeline::make_relative_asset_path(const std::filesystem::path& absolute,
    std::filesystem::path& relative, std::string& error) const {
    std::error_code ec;
    const auto candidate = std::filesystem::weakly_canonical(absolute, ec);
    if (ec) { error = "Cannot canonicalize asset source path: " + ec.message(); return false; }
    const auto root = std::filesystem::weakly_canonical(project_root_, ec);
    if (ec || !path_is_within(root, candidate)) {
        error = "Asset source must be stored inside the current project asset directory.";
        return false;
    }
    relative = candidate.lexically_relative(root);
    if (relative.empty() || relative.is_absolute() || relative.has_root_name()) {
        error = "Could not produce a project-relative asset path.";
        return false;
    }
    for (const auto& component : relative) if (component == L"..") {
        error = "Asset source path escapes the project root.";
        return false;
    }
    error.clear();
    return true;
}

bool AssetPipeline::resolve_relative_asset_path(const std::filesystem::path& relative,
    std::filesystem::path& absolute, std::string& error) const {
    if (relative.empty() || relative.is_absolute() || relative.has_root_name() || relative.has_root_directory()) {
        error = "Registry asset path must be a non-empty project-relative path.";
        return false;
    }
    for (const auto& component : relative) if (component == L"..") {
        error = "Registry asset path attempts to leave the project root.";
        return false;
    }
    std::error_code ec;
    const auto root = std::filesystem::weakly_canonical(project_root_, ec);
    if (ec) { error = "Cannot resolve project root: " + ec.message(); return false; }
    const auto candidate = std::filesystem::weakly_canonical(root / relative, ec);
    if (ec || !path_is_within(root, candidate)) {
        error = "Registry asset path resolves outside the project root.";
        return false;
    }
    absolute = candidate;
    error.clear();
    return true;
}

std::filesystem::path AssetPipeline::stage_asset_source(const std::filesystem::path& source,
    const AssetType type, const std::string& content_hash, std::string& error) const {
    std::error_code ec;
    const auto absolute_source = std::filesystem::weakly_canonical(source, ec);
    if (ec || !std::filesystem::is_regular_file(absolute_source, ec) || ec) {
        error = "Asset source disappeared before it could be copied into the project.";
        return {};
    }
    const auto package = project_root_ / L"Assets" / std::filesystem::u8path(asset_type_name(type)) / std::filesystem::u8path(content_hash);
    const auto existing_parent = absolute_source.parent_path();
    const auto managed_root = project_root_ / L"Assets" / std::filesystem::u8path(asset_type_name(type)) / std::filesystem::u8path(content_hash);
    if (path_is_within(managed_root, absolute_source)) { error.clear(); return absolute_source; }

    std::filesystem::path destination = package / absolute_source.filename();
    std::vector<std::pair<std::filesystem::path, std::filesystem::path>> dependencies;
    if (type == AssetType::Model && (lowercase_extension(source) == ".gltf" || lowercase_extension(source) == ".glb")) {
        std::vector<std::filesystem::path> uris;
        if (!collect_gltf_external_uris(absolute_source, uris, error)) return {};
        unsigned nesting = 0;
        for (const auto& uri : uris) {
            int depth = 0;
            for (const auto& component : uri) {
                if (component == L"..") { --depth; nesting = (std::max)(nesting, static_cast<unsigned>(-depth)); }
                else if (component != L".") ++depth;
            }
        }
        auto model_parent = package;
        for (unsigned i = 0; i < nesting; ++i) model_parent /= L"_relative";
        destination = model_parent / absolute_source.filename();
        for (const auto& uri : uris) {
            const auto source_dependency = (existing_parent / uri).lexically_normal();
            if (!std::filesystem::is_regular_file(source_dependency, ec) || ec) {
                error = "glTF external resource is missing: " + source_dependency.string();
                return {};
            }
            const auto destination_dependency = (model_parent / uri).lexically_normal();
            if (!path_is_within(package, destination_dependency)) {
                error = "glTF external resource cannot be represented safely inside the project asset package.";
                return {};
            }
            dependencies.emplace_back(source_dependency, destination_dependency);
        }
    }
    std::filesystem::create_directories(destination.parent_path(), ec);
    if (ec) { error = "Cannot create project asset folder: " + ec.message(); return {}; }
    std::filesystem::copy_file(absolute_source, destination, std::filesystem::copy_options::overwrite_existing, ec);
    if (ec) { error = "Cannot copy asset into project: " + ec.message(); return {}; }
    for (const auto& [source_dependency, destination_dependency] : dependencies) {
        std::filesystem::create_directories(destination_dependency.parent_path(), ec);
        if (ec) { error = "Cannot create glTF dependency folder: " + ec.message(); return {}; }
        std::filesystem::copy_file(source_dependency, destination_dependency, std::filesystem::copy_options::overwrite_existing, ec);
        if (ec) { error = "Cannot package glTF dependency: " + ec.message(); return {}; }
    }
    error.clear();
    return destination;
}

AssetImportResult AssetPipeline::import_file(const std::filesystem::path& path, const TextureQuality quality) {
    std::scoped_lock operation_lock(operation_mutex_);
    AssetImportResult failed;
    if (path.empty()) { failed.error = "Asset path is empty."; return failed; }
    std::error_code absolute_error;
    const std::filesystem::path absolute_path = std::filesystem::absolute(path, absolute_error).lexically_normal();
    if (absolute_error) { failed.error = "Cannot resolve asset source path: " + absolute_error.message(); return failed; }
    const std::string ext = lowercase_extension(absolute_path);
    AssetType type{};
    std::uint64_t limit{};
    if (ext == ".png" || ext == ".jpg" || ext == ".jpeg" || ext == ".tga") { type = AssetType::Texture; limit = kMaxImageFileBytes; }
    else if (ext == ".gltf" || ext == ".glb") { type = AssetType::Model; limit = kMaxModelFileBytes; }
    else if (ext == ".wav") { type = AssetType::Audio; limit = kMaxAudioFileBytes; }
    else if (ext == ".twmat.json") { type = AssetType::Material; limit = 4ull * 1024 * 1024; }
    else { failed.error = "Unsupported asset extension: " + ext; return failed; }

    std::vector<std::uint8_t> bytes;
    if (type == AssetType::Model) {
        std::error_code ec;
        const auto file_size = std::filesystem::file_size(absolute_path, ec);
        if (ec || file_size == 0 || file_size > limit) { failed.error = "Model file is missing, empty, or exceeds 128 MiB."; return failed; }
    } else if (!read_file(absolute_path, limit, bytes, failed.error)) return failed;

    std::string content_hash;
    if (type == AssetType::Model) {
        if (!read_file(absolute_path, limit, bytes, failed.error)) return failed;
    }
    if (!sha256(bytes, content_hash, failed.error)) return failed;
    const std::string id = std::string(asset_type_name(type)) + ":" + content_hash;
    const auto remember_asset = [&](const std::filesystem::path& source_path, std::string& persistence_error) {
        std::filesystem::path relative;
        std::string relative_error;
        if (!make_relative_asset_path(source_path, relative, relative_error)) {
            persistence_error = std::move(relative_error);
            return false;
        }
        const auto found = std::find_if(registry_records_.begin(), registry_records_.end(), [&](const auto& record) { return record.first == id; });
        if (found == registry_records_.end()) registry_records_.emplace_back(id, relative);
        else found->second = relative;
        if (restoring_registry_) return true;
        if (!save_registry_index(persistence_error)) {
            persistence_errors_.push_back(persistence_error);
            return false;
        }
        return true;
    };
    AssetMetadata existing;
    if (registry_.find_by_id(id, existing)) {
        std::string persistence_error;
        const bool persisted = remember_asset(existing.source_path, persistence_error);
        return {existing, registry_.payload(existing.handle), true, {}, persisted ? std::string{} : std::move(persistence_error)};
    }

    AssetMetadata metadata;
    metadata.type = type;
    metadata.id = id;
    metadata.content_hash = content_hash;
    metadata.name = type == AssetType::Material && absolute_path.filename().string().ends_with(".twmat.json")
        ? absolute_path.filename().string().substr(0, absolute_path.filename().string().size() - 11) : absolute_path.stem().string();
    metadata.source_path = absolute_path;
    metadata.file_size = bytes.size();
    std::shared_ptr<AssetPayload> payload_value;
    if (type == AssetType::Texture) {
        TextureData texture;
        if (!import_texture(bytes, ext, quality, texture, metadata, failed.error)) return failed;
        payload_value = std::make_shared<AssetPayload>(std::move(texture));
    } else if (type == AssetType::Model) {
        ModelData model;
        if (!import_model(absolute_path, model, metadata, failed.error)) return failed;
        payload_value = std::make_shared<AssetPayload>(std::move(model));
    } else if (type == AssetType::Audio) {
        AudioData audio;
        if (!import_audio(bytes, audio, failed.error)) return failed;
        metadata.format = "WAV PCM float decode";
        metadata.channels = audio.channels;
        metadata.decoded_bytes = audio.interleaved_samples.size() * sizeof(float);
        payload_value = std::make_shared<AssetPayload>(std::move(audio));
    } else {
        MaterialDefinition material;
        if (!MaterialDefinition::load(absolute_path, material, failed.error)) return failed;
        if (material.id.empty()) material.id = id;
        metadata.format = "Together Engine Material v1";
        metadata.decoded_bytes = sizeof(MaterialDefinition);
        payload_value = std::make_shared<AssetPayload>(std::move(material));
    }
    metadata.source_path = stage_asset_source(absolute_path, type, content_hash, failed.error);
    if (metadata.source_path.empty()) return failed;
    const std::shared_ptr<const AssetPayload> shared = payload_value;
    {
        std::scoped_lock lock(cache_mutex_);
        std::uint64_t used = 0;
        for (const auto& [cached_id, cached_payload] : cache_) {
            (void)cached_id;
            used += payload_size(*cached_payload);
        }
        const std::uint64_t incoming = payload_size(*shared);
        if (incoming > kDecodedCacheBudgetBytes || used > kDecodedCacheBudgetBytes - incoming) {
            failed.error = "Decoded asset cache budget (768 MiB) would be exceeded; remove unused assets or lower texture quality.";
            return failed;
        }
        cache_.insert_or_assign(id, shared);
    }
    auto registered = registry_.register_asset(std::move(metadata), shared);
    if (registered) {
        std::string persistence_error;
        if (!remember_asset(registered.metadata.source_path, persistence_error)) registered.persistence_error = std::move(persistence_error);
    }
    return registered;
}

bool AssetPipeline::save_registry_index(std::string& error) {
    try {
        std::filesystem::create_directories(registry_index_path_.parent_path());
        nlohmann::json document;
        document["format"] = "TogetherEngineAssetRegistry";
        document["version"] = 2;
        document["assets"] = nlohmann::json::array();
        for (const auto& [id, path] : registry_records_) {
            document["assets"].push_back({{"id", id}, {"sourcePath", utf8_path(path)}});
        }
        const auto temporary = registry_index_path_.wstring() + L".tmp";
        {
            std::ofstream output(temporary, std::ios::binary | std::ios::trunc);
            if (!output) { error = "Cannot create the persistent asset registry index."; return false; }
            const std::string encoded = document.dump(2);
            output.write(encoded.data(), static_cast<std::streamsize>(encoded.size()));
            output.flush();
            if (!output) { error = "Cannot write the persistent asset registry index."; return false; }
        }
        if (!MoveFileExW(temporary.c_str(), registry_index_path_.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
            error = "Cannot atomically replace the asset registry index (Windows error " + std::to_string(GetLastError()) + ").";
            std::error_code ignored;
            std::filesystem::remove(temporary, ignored);
            return false;
        }
        return true;
    } catch (const std::exception& exception) {
        error = std::string("Cannot save the persistent asset registry: ") + exception.what();
        return false;
    }
}

void AssetPipeline::restore_registry_index() {
    std::error_code index_error;
    if (!std::filesystem::exists(registry_index_path_, index_error)) {
        if (index_error) persistence_errors_.push_back("Cannot inspect the persistent asset registry: " + index_error.message());
        return;
    }
    try {
        const auto index_size = std::filesystem::file_size(registry_index_path_, index_error);
        if (index_error || index_size > 4ull * 1024 * 1024) {
            persistence_errors_.push_back(index_error ? "Cannot read persistent asset registry size: " + index_error.message() :
                "Persistent asset registry exceeds the 4 MiB safety limit.");
            return;
        }
        std::ifstream input(registry_index_path_, std::ios::binary);
        if (!input) { persistence_errors_.push_back("Cannot open the persistent asset registry index."); return; }
        const auto document = nlohmann::json::parse(input, nullptr, false);
        const int version = document.is_object() ? document.value("version", 0) : 0;
        if (document.is_discarded() || !document.is_object() ||
            document.value("format", std::string{}) != "TogetherEngineAssetRegistry" || (version != 1 && version != 2) ||
            !document.contains("assets") || !document["assets"].is_array()) {
            persistence_errors_.push_back("Persistent asset registry has an invalid format or version.");
            return;
        }
        if (document["assets"].size() > 10000) {
            persistence_errors_.push_back("Persistent asset registry exceeds the 10,000-entry safety limit.");
            return;
        }
        std::unordered_set<std::string> seen;
        std::vector<std::pair<std::string, std::filesystem::path>> missing;
        restoring_registry_ = true;
        for (const auto& item : document["assets"]) {
            if (!item.is_object() || !item.contains("id") || !item["id"].is_string() ||
                !item.contains("sourcePath") || !item["sourcePath"].is_string()) {
                persistence_errors_.push_back("Persistent asset registry contains a malformed entry.");
                continue;
            }
            const std::string expected_id = item["id"].get<std::string>();
            if (!valid_asset_id(expected_id)) { persistence_errors_.push_back("Persistent asset registry contains an invalid asset ID."); continue; }
            if (!seen.insert(expected_id).second) { persistence_errors_.push_back("Persistent asset registry contains a duplicate ID: " + expected_id); continue; }
            const std::filesystem::path stored_path = std::filesystem::u8path(item["sourcePath"].get<std::string>());
            std::filesystem::path source;
            std::string path_error;
            if (version == 1 && stored_path.is_absolute()) {
                std::error_code ec;
                if (!std::filesystem::is_regular_file(stored_path, ec) || ec) {
                    persistence_errors_.push_back("Legacy absolute asset source is missing and cannot be relocated automatically: " + stored_path.string());
                    continue;
                }
                source = stored_path;
            } else if (!resolve_relative_asset_path(stored_path, source, path_error)) {
                persistence_errors_.push_back("Invalid asset registry path: " + path_error);
                continue;
            }
            std::error_code ec;
            if (!std::filesystem::is_regular_file(source, ec) || ec) {
                persistence_errors_.push_back("Missing asset source: " + source.string());
                if (version == 2) missing.emplace_back(expected_id, stored_path);
                continue;
            }
            const auto imported = import_file(source);
            if (!imported) {
                persistence_errors_.push_back("Could not restore " + source.string() + ": " + imported.error);
                if (version == 2) missing.emplace_back(expected_id, stored_path);
            }
            else if (imported.metadata.id != expected_id) {
                persistence_errors_.push_back("Asset source changed since registration; stale entry was discarded: " + source.string());
                (void)remove_asset(imported.metadata.id);
            }
        }
        restoring_registry_ = false;
        registry_records_.insert(registry_records_.end(), missing.begin(), missing.end());
        std::string save_error;
        // Rewrite migrated paths and any source paths normalized during restoration.
        if (!save_registry_index(save_error)) persistence_errors_.push_back("Could not persist validated asset registry: " + save_error);
    } catch (const std::exception& exception) {
        restoring_registry_ = false;
        persistence_errors_.push_back(std::string("Could not read the persistent asset registry: ") + exception.what());
    }
}

std::vector<std::string> AssetPipeline::restore_missing_assets() {
    std::scoped_lock operation_lock(operation_mutex_);
    std::vector<std::string> messages;
    const auto records_to_check = registry_records_;
    for (const auto& [expected_id, relative] : records_to_check) {
        AssetMetadata restored;
        if (registry_.find_by_id(expected_id, restored)) continue;
        std::filesystem::path source;
        std::string error;
        if (!resolve_relative_asset_path(relative, source, error)) { messages.push_back(error); continue; }
        std::error_code ec;
        if (!std::filesystem::is_regular_file(source, ec) || ec) { messages.push_back("Still missing: " + source.string()); continue; }
        restoring_registry_ = true;
        auto result = import_file(source);
        restoring_registry_ = false;
        if (!result) { messages.push_back("Could not restore " + source.string() + ": " + result.error); continue; }
        if (result.metadata.id != expected_id) {
            (void)remove_asset(result.metadata.id);
            const auto found = std::find_if(registry_records_.begin(), registry_records_.end(), [&](const auto& record) { return record.first == result.metadata.id; });
            if (found != registry_records_.end()) registry_records_.erase(found);
            messages.push_back("Source content no longer matches registered ID: " + source.string());
            continue;
        }
        messages.push_back("Restored asset: " + source.string());
    }
    std::string save_error;
    if (!save_registry_index(save_error)) messages.push_back("Could not save registry after recovery: " + save_error);
    return messages;
}

std::future<AssetImportResult> AssetPipeline::import_async(std::filesystem::path path, const TextureQuality quality) {
    auto promise = std::make_shared<std::promise<AssetImportResult>>();
    auto future = promise->get_future();
    {
        std::scoped_lock lock(queue_mutex_);
        jobs_.push([this, promise, path = std::move(path), quality]() mutable {
            try { promise->set_value(import_file(path, quality)); }
            catch (const std::exception& exception) { promise->set_value({{}, {}, false, exception.what()}); }
        });
    }
    queue_ready_.notify_one();
    return future;
}

void AssetPipeline::worker_loop(const std::stop_token stop_token) {
    while (true) {
        std::function<void()> job;
        {
            std::unique_lock lock(queue_mutex_);
            queue_ready_.wait(lock, stop_token, [this] { return !jobs_.empty(); });
            if (jobs_.empty() && stop_token.stop_requested()) return;
            if (jobs_.empty()) continue;
            job = std::move(jobs_.front());
            jobs_.pop();
        }
        job();
    }
}

std::uint64_t AssetPipeline::cached_bytes() const {
    std::scoped_lock lock(cache_mutex_);
    std::uint64_t total = 0;
    for (const auto& [id, payload] : cache_) { (void)id; total += payload_size(*payload); }
    return total;
}

bool AssetPipeline::rename_asset(const std::string& id, const std::filesystem::path& new_filename, std::string& error) {
    std::scoped_lock operation_lock(operation_mutex_);
    error.clear();
    if (new_filename.empty() || new_filename.has_parent_path() || new_filename.filename() != new_filename ||
        new_filename == L"." || new_filename == L"..") { error = "Choose a valid new filename without a directory."; return false; }
    AssetMetadata metadata;
    if (!registry_.find_by_id(id, metadata)) { error = "Asset is not registered."; return false; }
    const auto old_path = metadata.source_path;
    if (old_path.empty() || old_path.extension() != new_filename.extension() ||
        (metadata.type == AssetType::Material && !new_filename.filename().wstring().ends_with(L".twmat.json"))) {
        error = "Rename must preserve the asset file extension."; return false;
    }
    const auto new_path = old_path.parent_path() / new_filename;
    std::string renamed_name = new_filename.stem().string();
    if (metadata.type == AssetType::Material && renamed_name.ends_with(".twmat")) renamed_name.resize(renamed_name.size() - 6);
    std::error_code ec;
    if (std::filesystem::exists(new_path, ec) || ec) { error = ec ? "Cannot inspect destination: " + ec.message() : "An asset with that filename already exists."; return false; }
    std::filesystem::rename(old_path, new_path, ec);
    if (ec) { error = "Cannot rename asset source: " + ec.message(); return false; }
    std::filesystem::path relative;
    if (!make_relative_asset_path(new_path, relative, error)) { std::filesystem::rename(new_path, old_path, ec); return false; }
    const auto record = std::find_if(registry_records_.begin(), registry_records_.end(), [&](const auto& entry) { return entry.first == id; });
    const bool had_record = record != registry_records_.end();
    const auto old_relative = had_record ? record->second : std::filesystem::path{};
    if (had_record) record->second = relative; else registry_records_.emplace_back(id, relative);
    if (!registry_.update_source(id, new_path, std::move(renamed_name))) {
        std::filesystem::rename(new_path, old_path, ec);
        if (had_record) record->second = old_relative; else std::erase_if(registry_records_, [&](const auto& entry) { return entry.first == id; });
        error = "Asset registry entry disappeared during rename.";
        return false;
    }
    if (!save_registry_index(error)) {
        (void)registry_.update_source(id, old_path, metadata.name);
        if (had_record) record->second = old_relative; else std::erase_if(registry_records_, [&](const auto& entry) { return entry.first == id; });
        std::filesystem::rename(new_path, old_path, ec);
        return false;
    }
    return true;
}

bool AssetPipeline::remove_asset(const std::string& id, const std::span<const std::string> protected_ids, std::string* persistence_error) {
    std::scoped_lock operation_lock(operation_mutex_);
    if (persistence_error) persistence_error->clear();
    if (std::find(protected_ids.begin(), protected_ids.end(), id) != protected_ids.end()) {
        if (persistence_error) *persistence_error = "Asset is referenced and cannot be deleted.";
        return false;
    }
    AssetMetadata metadata;
    if (!registry_.find_by_id(id, metadata)) return false;
    std::filesystem::path relative;
    std::string error;
    if (!make_relative_asset_path(metadata.source_path, relative, error)) { if (persistence_error) *persistence_error = error; return false; }
    const auto assets_root = project_root_ / L"Assets";
    const auto package = assets_root / std::filesystem::u8path(asset_type_name(metadata.type)) /
        std::filesystem::u8path(metadata.content_hash);
    if (!path_is_within(package, metadata.source_path) || !path_is_within(assets_root, package)) {
        if (persistence_error) *persistence_error = "Asset package is outside the managed Assets directory.";
        return false;
    }
    const auto tombstone = package.parent_path() / (package.filename().wstring() + L".deleting");
    std::error_code ec;
    if (std::filesystem::exists(tombstone, ec)) { if (persistence_error) *persistence_error = "A prior asset deletion recovery folder exists."; return false; }
    std::filesystem::rename(package, tombstone, ec);
    if (ec) { if (persistence_error) *persistence_error = "Cannot stage asset deletion: " + ec.message(); return false; }
    const auto records_backup = registry_records_;
    std::erase_if(registry_records_, [&](const auto& record) { return record.first == id; });
    if (!save_registry_index(error)) {
        registry_records_ = records_backup;
        std::filesystem::rename(tombstone, package, ec);
        if (persistence_error) *persistence_error = error;
        persistence_errors_.push_back(error);
        return false;
    }
    if (!registry_.remove(id)) {
        registry_records_ = records_backup;
        std::string ignored;
        (void)save_registry_index(ignored);
        std::filesystem::rename(tombstone, package, ec);
        if (persistence_error) *persistence_error = "Asset registry entry disappeared during deletion.";
        return false;
    }
    {
        std::scoped_lock lock(cache_mutex_);
        cache_.erase(id);
    }
    std::filesystem::remove_all(tombstone, ec);
    if (ec) { if (persistence_error) *persistence_error = "Asset is deleted but its temporary cleanup folder remains: " + ec.message(); }
    return true;
}

} // namespace together::assets
