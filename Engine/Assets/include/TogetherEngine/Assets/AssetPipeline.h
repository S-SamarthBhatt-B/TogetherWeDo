#pragma once

#include <array>
#include <condition_variable>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <future>
#include <memory>
#include <mutex>
#include <queue>
#include <string>
#include <thread>
#include <unordered_map>
#include <variant>
#include <vector>

namespace together::assets {

enum class AssetType : std::uint8_t { Texture, Model, Audio, Material };
enum class TextureQuality : std::uint8_t { Low, Medium, High, Original };
enum class TextureSlot : std::uint8_t { BaseColor, Normal, Roughness, Metallic, Occlusion, Emission, Count };

struct AssetHandle {
    std::uint32_t index{UINT32_MAX};
    std::uint32_t generation{};
    bool valid() const noexcept { return index != UINT32_MAX && generation != 0; }
    friend bool operator==(AssetHandle, AssetHandle) = default;
};

struct AssetMetadata {
    AssetHandle handle;
    AssetType type{AssetType::Texture};
    std::string id;             // Type-qualified SHA-256 content id.
    std::string content_hash;   // Lowercase SHA-256 hex.
    std::string name;
    std::filesystem::path source_path;
    std::uint64_t file_size{};
    std::uint64_t decoded_bytes{};
    unsigned width{};
    unsigned height{};
    unsigned channels{};
    unsigned mip_levels{};
    unsigned mesh_count{};
    unsigned vertex_count{};
    unsigned index_count{};
    unsigned joint_count{};
    std::string format;
};

struct AssetVertex {
    float position[3]{};
    float normal[3]{0.0f, 1.0f, 0.0f};
    float uv[2]{};
    float tangent[4]{1.0f, 0.0f, 0.0f, 1.0f};
};

struct Bounds3 { std::array<float, 3> minimum{}; std::array<float, 3> maximum{}; };

struct TextureData {
    unsigned width{};
    unsigned height{};
    unsigned source_channels{};
    unsigned mip_levels{};
    std::string format;
    bool srgb{true};
    std::vector<std::uint8_t> rgba8;
    // Generated CPU mip levels, smallest level last; the base level is rgba8.
    std::vector<std::vector<std::uint8_t>> mipmaps;
};

struct ImportedPrimitive {
    std::string name;
    std::string material_name;
    std::vector<AssetVertex> vertices;
    std::vector<std::uint32_t> indices;
    Bounds3 bounds;
    // glTF node world transform in glTF column-major order.
    std::array<float, 16> transform{1,0,0,0, 0,1,0,0, 0,0,1,0, 0,0,0,1};
    std::array<float, 4> base_color{1,1,1,1};
    float metallic{};
    float roughness{1.0f};
    std::array<float, 3> emission{};
    std::array<std::string, 6> texture_uris{};
};

struct SkeletonDefinition {
    std::vector<std::string> joint_names;
    std::vector<std::int32_t> parent_indices;
    std::vector<std::array<float, 16>> inverse_bind_matrices;
};

struct ModelData {
    std::vector<ImportedPrimitive> primitives;
    std::vector<SkeletonDefinition> skins;
    Bounds3 bounds;
};

struct AudioData {
    unsigned channels{};
    unsigned sample_rate{};
    std::uint64_t frame_count{};
    std::vector<float> interleaved_samples;
};

struct MaterialDefinition {
    std::string id;
    std::string name{"New Material"};
    std::array<float, 4> base_color{0.8f, 0.8f, 0.8f, 1.0f};
    float metallic{};
    float roughness{0.7f};
    std::array<float, 3> emission{};
    std::array<std::string, static_cast<std::size_t>(TextureSlot::Count)> textures{};

    bool validate(std::string& error) const;
    bool save(const std::filesystem::path& path, std::string& error) const;
    static bool load(const std::filesystem::path& path, MaterialDefinition& output, std::string& error);
};

using AssetPayload = std::variant<TextureData, ModelData, AudioData, MaterialDefinition>;

struct AssetImportResult {
    AssetMetadata metadata;
    std::shared_ptr<const AssetPayload> payload;
    bool duplicate{};
    std::string error;
    explicit operator bool() const noexcept { return metadata.handle.valid() && payload && error.empty(); }
};

class AssetRegistry final {
public:
    AssetImportResult register_asset(AssetMetadata metadata, std::shared_ptr<const AssetPayload> payload);
    bool resolve(AssetHandle handle, AssetMetadata& output) const;
    bool find_by_id(const std::string& id, AssetMetadata& output) const;
    bool remove(const std::string& id);
    std::shared_ptr<const AssetPayload> payload(AssetHandle handle) const;
    std::vector<AssetMetadata> snapshot() const;

private:
    struct Slot { std::uint32_t generation{1}; AssetMetadata metadata; std::shared_ptr<const AssetPayload> payload; };
    mutable std::mutex mutex_;
    std::vector<Slot> slots_;
    std::unordered_map<std::string, std::uint32_t> by_id_;
};

class AssetPipeline final {
public:
    explicit AssetPipeline(std::filesystem::path project_root);
    ~AssetPipeline();
    AssetPipeline(const AssetPipeline&) = delete;
    AssetPipeline& operator=(const AssetPipeline&) = delete;

    AssetImportResult import_file(const std::filesystem::path& path, TextureQuality quality = TextureQuality::Medium);
    std::future<AssetImportResult> import_async(std::filesystem::path path, TextureQuality quality = TextureQuality::Medium);
    std::vector<AssetMetadata> assets() const { return registry_.snapshot(); }
    bool resolve(AssetHandle handle, AssetMetadata& output) const { return registry_.resolve(handle, output); }
    bool find(const std::string& id, AssetMetadata& output) const { return registry_.find_by_id(id, output); }
    bool remove_asset(const std::string& id);
    std::shared_ptr<const AssetPayload> payload(AssetHandle handle) const { return registry_.payload(handle); }
    std::uint64_t cached_bytes() const;
    const std::filesystem::path& project_root() const noexcept { return project_root_; }

private:
    std::filesystem::path project_root_;
    AssetRegistry registry_;
    mutable std::mutex cache_mutex_;
    std::unordered_map<std::string, std::shared_ptr<const AssetPayload>> cache_;
    std::mutex queue_mutex_;
    std::condition_variable_any queue_ready_;
    std::queue<std::function<void()>> jobs_;
    std::jthread worker_;

    void worker_loop(std::stop_token stop_token);
};

const char* asset_type_name(AssetType type) noexcept;
const char* texture_slot_name(TextureSlot slot) noexcept;

} // namespace together::assets
