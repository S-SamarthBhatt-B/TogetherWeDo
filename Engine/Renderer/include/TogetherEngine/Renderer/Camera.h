#pragma once

#include <DirectXMath.h>

namespace together::renderer {

class Camera final {
public:
    Camera() = default;

    DirectX::XMMATRIX view() const noexcept;
    DirectX::XMMATRIX projection(float aspect_ratio) const noexcept;
    DirectX::XMVECTOR position() const noexcept;

    void move(float forward, float right, float up, float delta_seconds) noexcept;
    void rotate(float delta_yaw, float delta_pitch) noexcept;
    void zoom(float amount) noexcept;

private:
    DirectX::XMFLOAT3 position_{0.0f, 3.2f, -13.0f};
    float yaw_{};
    float pitch_{-0.12f};
    float field_of_view_{DirectX::XMConvertToRadians(60.0f)};
};

} // namespace together::renderer
