#include "TogetherEngine/Renderer/Camera.h"

#include <algorithm>
#include <cmath>

namespace together::renderer {

DirectX::XMVECTOR Camera::position() const noexcept {
    return DirectX::XMLoadFloat3(&position_);
}

DirectX::XMMATRIX Camera::view() const noexcept {
    const float cos_pitch = std::cos(pitch_);
    const DirectX::XMVECTOR direction = DirectX::XMVectorSet(
        std::sin(yaw_) * cos_pitch, std::sin(pitch_), std::cos(yaw_) * cos_pitch, 0.0f);
    return DirectX::XMMatrixLookToLH(position(), direction, DirectX::XMVectorSet(0, 1, 0, 0));
}

DirectX::XMMATRIX Camera::projection(const float aspect_ratio) const noexcept {
    const float safe_aspect = std::max(aspect_ratio, 0.01f);
    return DirectX::XMMatrixPerspectiveFovLH(field_of_view_, safe_aspect, 0.1f, 180.0f);
}

void Camera::move(const float forward, const float right, const float up, const float delta_seconds) noexcept {
    const float speed = 5.0f * std::clamp(delta_seconds, 0.0f, 0.1f);
    const float flat_forward_x = std::sin(yaw_);
    const float flat_forward_z = std::cos(yaw_);
    position_.x += (flat_forward_x * forward + flat_forward_z * right) * speed;
    position_.z += (flat_forward_z * forward - flat_forward_x * right) * speed;
    position_.y += up * speed;
}

void Camera::rotate(const float delta_yaw, const float delta_pitch) noexcept {
    yaw_ += delta_yaw;
    pitch_ = std::clamp(pitch_ + delta_pitch, -1.45f, 1.45f);
}

void Camera::zoom(const float amount) noexcept {
    field_of_view_ = std::clamp(field_of_view_ - amount, DirectX::XMConvertToRadians(35.0f), DirectX::XMConvertToRadians(80.0f));
}

} // namespace together::renderer
