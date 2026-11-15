#include "TogetherEngine/Assets/AssetPipeline.h"

#include <Windows.h>
#include <bcrypt.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <cctype>
#include <cstring>
#include <fstream>
#include <limits>
#include <span>
#include <type_traits>
#include <nlohmann/json.hpp>
#include <system_error>
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
            for (cgltf_size attribute_index = 0; attribute_index < primitive.attributes_count; ++attribute_index) {
                const auto& attribute = primitive.attributes[attribute_index];
                if (attribute.type == cgltf_attribute_type_position) positions = attribute.data;
                else if (attribute.type == cgltf_attribute_type_normal) normals = attribute.data;
                else if (attribute.type == cgltf_attribute_type_texcoord && attribute.index == 0) uvs = attribute.data;
            }
            if (!positions || positions->count == 0 || positions->count > kMaxModelVertices ||
                vertex_total + positions->count > kMaxModelVertices ||
                (normals && normals->count != positions->count) || (uvs && uvs->count != positions->count)) {
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
    : project_root_(std::move(project_root)), worker_([this](const std::stop_token stop) { worker_loop(stop); }) {}

AssetPipeline::~AssetPipeline() {
    worker_.request_stop();
    queue_ready_.notify_all();
}

AssetImportResult AssetPipeline::import_file(const std::filesystem::path& path, const TextureQuality quality) {
    AssetImportResult failed;
    if (path.empty()) { failed.error = "Asset path is empty."; return failed; }
    const std::string ext = lowercase_extension(path);
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
        const auto file_size = std::filesystem::file_size(path, ec);
        if (ec || file_size == 0 || file_size > limit) { failed.error = "Model file is missing, empty, or exceeds 128 MiB."; return failed; }
    } else if (!read_file(path, limit, bytes, failed.error)) return failed;

    std::string content_hash;
    if (type == AssetType::Model) {
        if (!read_file(path, limit, bytes, failed.error)) return failed;
    }
    if (!sha256(bytes, content_hash, failed.error)) return failed;
    const std::string id = std::string(asset_type_name(type)) + ":" + content_hash;
    AssetMetadata existing;
    if (registry_.find_by_id(id, existing)) {
        return {existing, registry_.payload(existing.handle), true, {}};
    }
    {
        std::scoped_lock lock(cache_mutex_);
        const auto cached = cache_.find(id);
        if (cached != cache_.end()) {
            AssetMetadata metadata;
            metadata.type = type; metadata.id = id; metadata.content_hash = content_hash;
            metadata.name = path.stem().string(); metadata.source_path = path; metadata.file_size = bytes.size();
            return registry_.register_asset(std::move(metadata), cached->second);
        }
    }

    AssetMetadata metadata;
    metadata.type = type;
    metadata.id = id;
    metadata.content_hash = content_hash;
    metadata.name = type == AssetType::Material && path.filename().string().ends_with(".twmat.json")
        ? path.filename().string().substr(0, path.filename().string().size() - 11) : path.stem().string();
    metadata.source_path = std::filesystem::absolute(path);
    metadata.file_size = bytes.size();
    std::shared_ptr<AssetPayload> payload_value;
    if (type == AssetType::Texture) {
        TextureData texture;
        if (!import_texture(bytes, ext, quality, texture, metadata, failed.error)) return failed;
        payload_value = std::make_shared<AssetPayload>(std::move(texture));
    } else if (type == AssetType::Model) {
        ModelData model;
        if (!import_model(path, model, metadata, failed.error)) return failed;
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
        if (!MaterialDefinition::load(path, material, failed.error)) return failed;
        if (material.id.empty()) material.id = id;
        metadata.format = "Together Engine Material v1";
        metadata.decoded_bytes = sizeof(MaterialDefinition);
        payload_value = std::make_shared<AssetPayload>(std::move(material));
    }
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
    return registry_.register_asset(std::move(metadata), shared);
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

bool AssetPipeline::remove_asset(const std::string& id) {
    {
        std::scoped_lock lock(cache_mutex_);
        cache_.erase(id);
    }
    return registry_.remove(id);
}

} // namespace together::assets
