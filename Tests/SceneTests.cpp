#include "TogetherEngine/Scene/Scene.h"

#include <filesystem>
#include <fstream>
#include <iostream>
#include <string_view>

namespace {

bool check(const bool condition, const std::string_view name) {
    std::cout << (condition ? "[PASS] " : "[FAIL] ") << name << '\n';
    return condition;
}

bool write_text(const std::filesystem::path& path, const std::string_view text) {
    std::ofstream file(path, std::ios::binary | std::ios::trunc);
    file.write(text.data(), static_cast<std::streamsize>(text.size()));
    return static_cast<bool>(file);
}

} // namespace

int main() {
    const std::filesystem::path directory = std::filesystem::temp_directory_path() / "TogetherEngineSceneTests";
    std::filesystem::create_directories(directory);
    const auto valid_path = directory / "roundtrip.tws.json";
    const auto invalid_path = directory / "invalid.tws.json";
    bool passed = true;
    std::string error;

    together::scene::Scene scene("Forest \"Test\"");
    auto& first = scene.create_object("Oak\nTree");
    const auto first_id = first.id;
    first.transform.position = {1.25f, -2.0f, 3.5f};
    first.transform.rotation = {0.0f, 45.0f, 0.0f};
    first.transform.scale = {1.0f, 2.0f, 1.0f};
    first.material.base_color = {0.2f, 0.6f, 0.3f};
    first.material.roughness = 0.4f;
    first.material.metallic = 0.1f;
    first.material.emission = {0.2f, 0.05f, 0.01f};
    const float expected_emission = first.material.emission.x;
    first.model_asset_id = "model:0123456789abcdef";
    first.material_asset_id = "material:abcdef0123456789";
    first.material.base_color_texture_id = "texture:fedcba9876543210";
    first.material.normal_texture_id = "texture:normal";
    first.material.metallic_roughness_texture_id = "texture:metalrough";
    first.material.occlusion_texture_id = "texture:occlusion";
    passed &= check(scene.references_asset(first.model_asset_id) &&
        scene.references_asset(first.material_asset_id) &&
        scene.references_asset(first.material.normal_texture_id) &&
        !scene.references_asset("texture:not-referenced"),
        "scene reference queries cover model, material, and texture asset IDs");
    const std::string shared_model_id = first.model_asset_id;
    const std::string shared_material_id = first.material_asset_id;
    const std::string shared_texture_id = first.material.base_color_texture_id;
    auto& second = scene.create_object("Rock");
    const auto second_id = second.id;
    passed &= check(scene.objects().size() == 2 && first_id != second_id, "object creation assigns unique ids");
    auto* edited = scene.find_object(second_id);
    if (edited) edited->transform.position.x = 8.0f;
    passed &= check(edited && edited->transform.position.x == 8.0f, "object transform can be modified by id");
    passed &= check(scene.delete_object(second_id) && !scene.find_object(second_id), "object deletion removes the selected id");
    auto& shared_instance = scene.create_object("Shared Material Instance");
    shared_instance.model_asset_id = shared_model_id;
    shared_instance.material_asset_id = shared_material_id;
    passed &= check(scene.validate(error), "valid scene passes validation");
    passed &= check(scene.save(valid_path, error), "scene serialization writes a JSON scene");

    together::scene::Scene loaded("Existing Scene");
    passed &= check(together::scene::Scene::load(valid_path, loaded, error), "scene loading accepts serialized scene");
    const auto* loaded_object = loaded.find_object(first_id);
    const auto* loaded_shared = loaded.find_object(shared_instance.id);
    passed &= check(loaded.name() == scene.name() && loaded.objects().size() == 2 && loaded_object && loaded_shared &&
        loaded_object->name == "Oak\nTree" && loaded_object->transform.position.y == -2.0f &&
        loaded_object->material.roughness == 0.4f && loaded_object->model_asset_id == shared_model_id &&
        loaded_object->material.emission.x == expected_emission &&
        loaded_object->material_asset_id == shared_material_id &&
        loaded_object->material.base_color_texture_id == shared_texture_id &&
        loaded_object->material.normal_texture_id == "texture:normal" &&
        loaded_object->material.metallic_roughness_texture_id == "texture:metalrough" &&
        loaded_object->material.occlusion_texture_id == "texture:occlusion" &&
        loaded_shared->material_asset_id == shared_material_id,
        "scene JSON round-trip preserves names, transforms, material values, and stable asset references");

    together::scene::Scene invalid_scene;
    auto& invalid_object = invalid_scene.create_object();
    invalid_object.transform.scale.x = 0.0f;
    passed &= check(!invalid_scene.validate(error) && !error.empty(), "invalid scene transform is rejected");
    passed &= check(!invalid_scene.save(directory / "should-not-exist.json", error), "invalid scene cannot be saved");

    const bool malformed_written = write_text(invalid_path, "{\"format\":\"TogetherEngineScene\",\"version\":1,\"name\":\"broken\",\"objects\":[}");
    const auto preserved_name = loaded.name();
    passed &= check(malformed_written && !together::scene::Scene::load(invalid_path, loaded, error) &&
        loaded.name() == preserved_name && !error.empty(),
        "malformed JSON is rejected without replacing the existing scene");
    const bool bad_value_written = write_text(invalid_path,
        "{\"format\":\"TogetherEngineScene\",\"version\":1,\"name\":\"bad\",\"objects\":[{\"id\":1,\"name\":\"Bad\",\"visible\":true,\"transform\":{\"position\":[0,0,0],\"rotation\":[0,0,0],\"scale\":[1,0,1]},\"material\":{\"baseColor\":[1,1,1],\"roughness\":0.5,\"metallic\":0}}]}");
    passed &= check(bad_value_written && !together::scene::Scene::load(invalid_path, loaded, error) && !error.empty(),
        "structurally valid JSON with invalid scene data is rejected");

    std::error_code ignored;
    std::filesystem::remove_all(directory, ignored);
    return passed ? 0 : 1;
}
