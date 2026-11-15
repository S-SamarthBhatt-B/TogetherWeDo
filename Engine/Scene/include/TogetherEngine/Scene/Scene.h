#pragma once

#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

namespace together::scene {

struct Vec3 {
    float x{};
    float y{};
    float z{};
};

struct Transform {
    Vec3 position{};
    Vec3 rotation{}; // Euler angles in degrees.
    Vec3 scale{1.0f, 1.0f, 1.0f};
};

struct Material {
    Vec3 base_color{0.55f, 0.62f, 0.48f};
    Vec3 emission{};
    float roughness{0.72f};
    float metallic{};
    std::string base_color_texture_id;
    std::string normal_texture_id;
    std::string metallic_roughness_texture_id;
    std::string occlusion_texture_id;
};

struct SceneObject {
    std::uint64_t id{};
    std::string name;
    Transform transform;
    Material material;
    // Stable content IDs; resolved against the editor asset registry at load time.
    std::string model_asset_id;
    std::string material_asset_id;
    bool visible{true};
};

class Scene final {
public:
    static constexpr std::uint32_t format_version = 1;

    explicit Scene(std::string name = "Untitled Scene");

    const std::string& name() const noexcept { return name_; }
    void set_name(std::string name);
    const std::vector<SceneObject>& objects() const noexcept { return objects_; }

    SceneObject& create_object(std::string name = {});
    SceneObject& duplicate_object(std::uint64_t id);
    bool delete_object(std::uint64_t id);
    SceneObject* find_object(std::uint64_t id) noexcept;
    const SceneObject* find_object(std::uint64_t id) const noexcept;

    bool validate(std::string& error) const;
    bool save(const std::filesystem::path& path, std::string& error) const;
    static bool load(const std::filesystem::path& path, Scene& output, std::string& error);

private:
    std::string name_;
    std::vector<SceneObject> objects_;
    std::uint64_t next_id_{1};
};

} // namespace together::scene
