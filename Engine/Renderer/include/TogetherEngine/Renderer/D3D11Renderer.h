#pragma once

#include <Windows.h>

#include <cstdint>
#include <cstddef>
#include <memory>
#include <span>
#include <string>
#include <vector>
#include <string_view>

#include "TogetherEngine/Renderer/Camera.h"

namespace together::renderer {

struct RendererStats {
    bool initialized{};
    unsigned width{};
    unsigned height{};
    unsigned draw_calls{};
    std::uint64_t frame_count{};
    std::uint64_t resize_count{};
    double last_frame_ms{};
};

struct RenderObject {
    DirectX::XMFLOAT3 position{};
    DirectX::XMFLOAT3 rotation_degrees{};
    DirectX::XMFLOAT3 scale{1.0f, 1.0f, 1.0f};
    DirectX::XMFLOAT4 base_color{0.55f, 0.62f, 0.48f, 1.0f};
    DirectX::XMFLOAT3 emission{};
    float roughness{0.72f};
    float metallic{};
    std::string model_asset_id;
    std::string material_asset_id;
    std::string base_color_texture_id;
    std::string normal_texture_id;
    std::string metallic_roughness_texture_id;
    std::string occlusion_texture_id;
    bool material_override{};
    bool visible{true};
    bool selected{};
};

struct ModelVertex {
    DirectX::XMFLOAT3 position{};
    DirectX::XMFLOAT3 normal{0.0f, 1.0f, 0.0f};
    DirectX::XMFLOAT2 uv{};
    DirectX::XMFLOAT4 tangent{1,0,0,1};
};

struct ModelPrimitiveUpload {
    std::span<const ModelVertex> vertices;
    std::span<const std::uint32_t> indices;
    DirectX::XMFLOAT4 base_color{1,1,1,1};
    float roughness{1.0f};
    float metallic{};
    DirectX::XMFLOAT3 emission{};
    DirectX::XMFLOAT4X4 local_transform{1,0,0,0, 0,1,0,0, 0,0,1,0, 0,0,0,1};
};

struct TextureMipUpload {
    unsigned width{};
    unsigned height{};
    std::span<const std::uint8_t> rgba8;
};

class D3D11Renderer final {
public:
    D3D11Renderer();
    ~D3D11Renderer();
    D3D11Renderer(const D3D11Renderer&) = delete;
    D3D11Renderer& operator=(const D3D11Renderer&) = delete;

    bool initialize(HWND window, unsigned width, unsigned height);
    void resize(unsigned width, unsigned height);
    void render(float elapsed_seconds, float delta_seconds);
    void move_camera(float forward, float right, float up, float delta_seconds) noexcept;
    void rotate_camera(float delta_yaw, float delta_pitch) noexcept;
    void zoom_camera(float amount) noexcept;
    bool register_texture_rgba8(std::string_view name, unsigned width, unsigned height,
        std::span<const std::uint32_t> pixels);
    bool register_texture_rgba8_mipped(std::string_view name, std::span<const TextureMipUpload> mip_levels);
    bool register_model(std::string_view asset_id, std::span<const ModelPrimitiveUpload> primitives);
    void release_asset(std::string_view asset_id);
    RendererStats stats() const noexcept;
    void set_editor_scene(std::span<const RenderObject> objects);
    int pick_editor_object(float screen_x, float screen_y, std::span<const RenderObject> objects) const noexcept;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
    Camera camera_;
};

} // namespace together::renderer
