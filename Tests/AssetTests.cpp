#include "TogetherEngine/Assets/AssetPipeline.h"

#include <filesystem>
#include <algorithm>
#include <fstream>
#include <iostream>
#include <array>
#include <cstdint>
#include <cstring>
#include <string>
#include <vector>

using namespace together::assets;

namespace {
bool check(bool condition, const char* message) {
    if (!condition) std::cerr << "FAIL: " << message << '\n';
    return condition;
}

bool write_tga(const std::filesystem::path& path) {
    // Uncompressed 2x2, 24-bit TGA. Four BGR pixels follow the 18-byte header.
    const unsigned char bytes[] = {0,0,2, 0,0,0,0,0, 0,0,0,0, 2,0,2,0,24,0,
        20,80,180, 40,90,190, 60,100,200, 80,110,210};
    std::ofstream file(path, std::ios::binary);
    file.write(reinterpret_cast<const char*>(bytes), sizeof(bytes));
    return static_cast<bool>(file);
}

std::string base64(const std::vector<std::uint8_t>& bytes) {
    constexpr char alphabet[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    std::string result;
    for (std::size_t i = 0; i < bytes.size(); i += 3) {
        const std::uint32_t a = bytes[i];
        const std::uint32_t b = i + 1 < bytes.size() ? bytes[i + 1] : 0;
        const std::uint32_t c = i + 2 < bytes.size() ? bytes[i + 2] : 0;
        const std::uint32_t value = (a << 16) | (b << 8) | c;
        result += alphabet[(value >> 18) & 63];
        result += alphabet[(value >> 12) & 63];
        result += i + 1 < bytes.size() ? alphabet[(value >> 6) & 63] : '=';
        result += i + 2 < bytes.size() ? alphabet[value & 63] : '=';
    }
    return result;
}

bool write_triangle_gltf(const std::filesystem::path& path) {
    std::vector<std::uint8_t> binary(42);
    const std::array<float, 9> positions{0,0,0, 1,0,0, 0,1,0};
    const std::array<std::uint16_t, 3> indices{0,1,2};
    std::memcpy(binary.data(), positions.data(), sizeof(positions));
    std::memcpy(binary.data() + 36, indices.data(), sizeof(indices));
    const std::string json = std::string(R"({"asset":{"version":"2.0"},"buffers":[{"byteLength":42,"uri":"data:application/octet-stream;base64,)") +
        base64(binary) + R"("}],"bufferViews":[{"buffer":0,"byteOffset":0,"byteLength":36},{"buffer":0,"byteOffset":36,"byteLength":6}],"accessors":[{"bufferView":0,"componentType":5126,"count":3,"type":"VEC3","min":[0,0,0],"max":[1,1,0]},{"bufferView":1,"componentType":5123,"count":3,"type":"SCALAR"}],"meshes":[{"primitives":[{"attributes":{"POSITION":0},"indices":1}]}]})";
    std::ofstream file(path, std::ios::binary);
    file << json;
    return static_cast<bool>(file);
}
}

int main() {
    bool ok = true;
    const auto directory = std::filesystem::temp_directory_path() / "TogetherEngineAssetTests";
    std::error_code cleanup_error;
    std::filesystem::remove_all(directory, cleanup_error);
    std::filesystem::create_directories(directory);

    MaterialDefinition material;
    material.name = "Test PBR";
    material.base_color = {0.2f, 0.4f, 0.6f, 1.0f};
    material.metallic = 0.35f;
    material.textures[static_cast<std::size_t>(TextureSlot::Normal)] = "texture:normal";
    material.textures[static_cast<std::size_t>(TextureSlot::Roughness)] = "texture:mr";
    material.textures[static_cast<std::size_t>(TextureSlot::Occlusion)] = "texture:ao";
    std::string error;
    const auto material_path = directory / "test.twmat.json";
    ok &= check(material.save(material_path, error), "material saves as JSON");
    MaterialDefinition loaded;
    ok &= check(MaterialDefinition::load(material_path, loaded, error), "material reloads from JSON");
    ok &= check(loaded.name == material.name && loaded.metallic == material.metallic &&
        loaded.textures[static_cast<std::size_t>(TextureSlot::Normal)] == "texture:normal" &&
        loaded.textures[static_cast<std::size_t>(TextureSlot::Roughness)] == "texture:mr" &&
        loaded.textures[static_cast<std::size_t>(TextureSlot::Occlusion)] == "texture:ao", "material factors and PBR texture slots round trip");
    loaded.roughness = 5.0f;
    ok &= check(!loaded.validate(error), "invalid material values are rejected");

    AssetRegistry registry;
    AssetMetadata invalid_registration;
    const auto rejected = registry.register_asset(invalid_registration, std::make_shared<const AssetPayload>(TextureData{}));
    ok &= check(!rejected && !rejected.error.empty(), "registry rejects invalid IDs instead of minting unusable handles");
    AssetMetadata metadata;
    metadata.type = AssetType::Texture;
    metadata.id = "texture:test-hash";
    auto payload = std::make_shared<const AssetPayload>(TextureData{});
    const auto first = registry.register_asset(metadata, payload);
    const auto duplicate = registry.register_asset(metadata, payload);
    AssetMetadata resolved;
    ok &= check(first && first.metadata.handle.valid(), "asset registration returns a handle");
    ok &= check(duplicate.duplicate && duplicate.metadata.handle == first.metadata.handle, "duplicate content returns the existing handle");
    ok &= check(registry.resolve(first.metadata.handle, resolved), "valid resource handle resolves");
    ok &= check(!registry.resolve({first.metadata.handle.index, first.metadata.handle.generation + 1}, resolved), "stale resource handle is rejected");

    const auto image_path = directory / "tiny.tga";
    ok &= check(write_tga(image_path), "writes TGA fixture");
    AssetPipeline pipeline(directory);
    auto image = pipeline.import_file(image_path, TextureQuality::Original);
    ok &= check(static_cast<bool>(image), "valid TGA imports");
    ok &= check(image.metadata.width == 2 && image.metadata.height == 2 && image.metadata.channels == 3,
        "texture metadata reports dimensions and source channels");
    ok &= check(std::holds_alternative<TextureData>(*image.payload), "TGA import produces decoded texture data");
    const auto& texture = std::get<TextureData>(*image.payload);
    ok &= check(texture.mip_levels == 2 && texture.mipmaps.size() == 1 && texture.mipmaps.front().size() == 4,
        "texture import generates its 1x1 mip level");
    const auto image_duplicate = pipeline.import_file(image_path, TextureQuality::Original);
    ok &= check(image_duplicate && image_duplicate.duplicate, "same texture import is deduplicated");
    {
        AssetPipeline restarted(directory);
        AssetMetadata restored_metadata;
        ok &= check(restarted.find(image.metadata.id, restored_metadata), "asset registry restores imported assets after pipeline restart");
        ok &= check(restored_metadata.source_path == image.metadata.source_path &&
            restored_metadata.source_path.string().find("Assets") != std::string::npos,
            "persistent registry restores the staged, project-owned source path");
    }
    std::string rename_error;
    ok &= check(pipeline.rename_asset(image.metadata.id, "renamed.tga", rename_error), "asset rename succeeds within its managed package");
    AssetMetadata renamed_metadata;
    ok &= check(pipeline.find(image.metadata.id, renamed_metadata) &&
        renamed_metadata.source_path.filename() == "renamed.tga" && std::filesystem::exists(renamed_metadata.source_path),
        "rename preserves the stable asset ID and updates the registered source path");
    {
        AssetPipeline restarted(directory);
        AssetMetadata restored;
        ok &= check(restarted.find(image.metadata.id, restored) && restored.source_path.filename() == "renamed.tga",
            "renamed asset registry survives restart");
    }
    std::filesystem::path registry_path = directory / ".together" / "asset_registry.json";
    std::ifstream registry_file(registry_path, std::ios::binary);
    const std::string registry_text((std::istreambuf_iterator<char>(registry_file)), std::istreambuf_iterator<char>());
    registry_file.close();
    ok &= check(registry_text.find("\"version\": 2") != std::string::npos &&
        registry_text.find(std::filesystem::absolute(directory).string()) == std::string::npos,
        "registry stores version 2 project-relative paths rather than absolute project locations");
    const auto invalid_path = directory / "invalid.asset";
    { std::ofstream file(invalid_path); file << "not an asset"; }
    ok &= check(!pipeline.import_file(invalid_path), "unsupported asset types are rejected");
    const auto invalid_texture = directory / "invalid.png";
    { std::ofstream file(invalid_texture, std::ios::binary); file << "not a PNG"; }
    ok &= check(!pipeline.import_file(invalid_texture), "invalid image data with a supported extension is rejected");
    const auto invalid_model = directory / "invalid.gltf";
    { std::ofstream file(invalid_model); file << "{}"; }
    ok &= check(!pipeline.import_file(invalid_model), "invalid glTF is rejected");
    const auto valid_model = directory / "triangle.gltf";
    ok &= check(write_triangle_gltf(valid_model), "writes a minimal valid glTF fixture");
    const auto model = pipeline.import_file(valid_model);
    ok &= check(static_cast<bool>(model), "valid glTF triangle imports");
    if (model && std::holds_alternative<ModelData>(*model.payload)) {
        const auto& mesh = std::get<ModelData>(*model.payload);
        ok &= check(mesh.primitives.size() == 1 && mesh.primitives[0].vertices.size() == 3 && mesh.primitives[0].indices.size() == 3,
            "glTF geometry and triangle indices are decoded");
    }
    ok &= check(pipeline.cached_bytes() >= 4, "decoded cache memory is tracked");
    const std::string imported_id = image.metadata.id;
    const AssetHandle removed_handle = image.metadata.handle;
    const auto managed_package = renamed_metadata.source_path.parent_path();
    const std::array<std::string, 1> protected_asset{imported_id};
    std::string delete_error;
    ok &= check(!pipeline.remove_asset(imported_id, protected_asset, &delete_error) &&
        pipeline.find(imported_id, resolved), "referenced assets are protected from deletion");
    const bool asset_deleted = pipeline.remove_asset(imported_id, {}, &delete_error);
    if (!asset_deleted) std::cerr << "Asset deletion detail: " << delete_error << '\\n';
    ok &= check(asset_deleted, "unreferenced asset deletion invalidates the registry and cache");
    ok &= check(!std::filesystem::exists(managed_package), "asset deletion removes the managed package directory");
    ok &= check(!pipeline.resolve(removed_handle, resolved) && !pipeline.find(imported_id, resolved),
        "removed asset handles and IDs no longer resolve");
    ok &= check(!pipeline.resolve({removed_handle.index, removed_handle.generation + 1}, resolved),
        "a fabricated handle for an empty slot is also rejected");
    ok &= check(pipeline.cached_bytes() > 0, "removing one asset preserves unrelated cached payloads");
    {
        AssetPipeline after_delete(directory);
        ok &= check(!after_delete.find(imported_id, resolved), "removed asset remains absent after registry reload");
    }

    // Relocate a complete project copy: project-relative registry paths must resolve from the new root.
    const auto relocation_a = directory / "relocation_a";
    const auto relocation_b = directory / "relocation_b";
    std::filesystem::create_directories(relocation_a);
    std::string relocated_id;
    {
        AssetPipeline source_project(relocation_a);
        const auto imported = source_project.import_file(image_path, TextureQuality::Original);
        ok &= check(static_cast<bool>(imported), "asset imports into relocation fixture project");
        relocated_id = imported.metadata.id;
    }
    std::filesystem::copy(relocation_a, relocation_b, std::filesystem::copy_options::recursive);
    std::filesystem::remove_all(relocation_a);
    {
        AssetPipeline moved_project(relocation_b);
        AssetMetadata moved_asset;
        ok &= check(moved_project.find(relocated_id, moved_asset) &&
            moved_asset.source_path.string().find(relocation_b.string()) == 0,
            "project-relative registry resolves after moving the complete project directory");
    }

    // Registry v1 migration from an existing legacy absolute source.
    const auto migration_root = directory / "migration";
    std::filesystem::create_directories(migration_root / ".together");
    {
        AssetPipeline seed(migration_root);
        const auto imported = seed.import_file(image_path, TextureQuality::Original);
        relocated_id = imported.metadata.id;
    }
    const auto legacy_asset = migration_root / "legacy.tga";
    std::filesystem::copy_file(image_path, legacy_asset, std::filesystem::copy_options::overwrite_existing);
    {
        std::ofstream legacy_index(migration_root / ".together" / "asset_registry.json", std::ios::trunc);
        auto escaped = std::filesystem::absolute(legacy_asset).string();
        std::string escaped_json;
        for (const char ch : escaped) { if (ch == '\\') escaped_json.push_back('\\'); escaped_json.push_back(ch); }
        legacy_index << "{\"format\":\"TogetherEngineAssetRegistry\",\"version\":1,\"assets\":[{\"id\":\""
            << relocated_id << "\",\"sourcePath\":\"" << escaped_json << "\"}]}";
    }
    {
        AssetPipeline migrated(migration_root);
        AssetMetadata restored;
        ok &= check(migrated.find(relocated_id, restored) &&
            restored.source_path.string().find((migration_root / "Assets").string()) == 0,
            "legacy absolute registry entry is staged into the project and restored");
    }

    // Temporarily missing sources remain recoverable through the persistent registry.
    const auto recovery_root = directory / "recovery";
    std::filesystem::create_directories(recovery_root);
    std::filesystem::path recovery_source;
    std::string recovery_id;
    {
        AssetPipeline recovery(recovery_root);
        const auto imported = recovery.import_file(image_path, TextureQuality::Original);
        recovery_source = imported.metadata.source_path;
        recovery_id = imported.metadata.id;
    }
    std::filesystem::remove(recovery_source);
    {
        AssetPipeline recovery(recovery_root);
        AssetMetadata absent;
        ok &= check(!recovery.find(recovery_id, absent) && !recovery.persistence_errors().empty(),
            "missing source is reported without registering invalid data");
        std::filesystem::create_directories(recovery_source.parent_path());
        std::filesystem::copy_file(image_path, recovery_source);
        const auto messages = recovery.restore_missing_assets();
        ok &= check(recovery.find(recovery_id, absent) &&
            std::any_of(messages.begin(), messages.end(), [](const auto& message) { return message.find("Restored asset") != std::string::npos; }),
            "restoring the missing relative file recovers its original asset ID");
    }

    const auto broken_root = directory / "broken_registry";
    std::filesystem::create_directories(broken_root / ".together");
    { std::ofstream file(broken_root / ".together" / "asset_registry.json"); file << "{bad registry"; }
    {
        AssetPipeline broken_registry(broken_root);
        ok &= check(!broken_registry.persistence_errors().empty(), "invalid persistent registry data is reported safely");
    }
    const auto unsafe_root = directory / "unsafe_registry";
    std::filesystem::create_directories(unsafe_root / ".together");
    {
        std::ofstream unsafe_index(unsafe_root / ".together" / "asset_registry.json", std::ios::trunc);
        unsafe_index << "{\"format\":\"TogetherEngineAssetRegistry\",\"version\":2,\"assets\":[{\"id\":\""
            << image.metadata.id << "\",\"sourcePath\":\"../../outside.tga\"}]}";
    }
    {
        AssetPipeline unsafe_registry(unsafe_root);
        AssetMetadata outside;
        ok &= check(!unsafe_registry.find(image.metadata.id, outside) && !unsafe_registry.persistence_errors().empty(),
            "registry paths escaping the project root are rejected and reported");
    }

    std::error_code ec;
    std::filesystem::remove_all(directory, ec);
    return ok ? 0 : 1;
}
