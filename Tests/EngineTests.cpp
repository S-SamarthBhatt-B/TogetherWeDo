#include "TogetherEngine/Core/FrameTimer.h"
#include "TogetherEngine/Renderer/Camera.h"
#include "TogetherEngine/Renderer/D3D11Renderer.h"

#include <Windows.h>

#include <array>
#include <chrono>
#include <cmath>
#include <iostream>
#include <string_view>
#include <thread>

namespace {

constexpr wchar_t kTestWindowClass[] = L"TogetherEngineTestWindow";

LRESULT CALLBACK test_window_proc(HWND window, UINT message, WPARAM wparam, LPARAM lparam) {
    return DefWindowProcW(window, message, wparam, lparam);
}

bool check(bool condition, std::string_view name) {
    std::cout << (condition ? "[PASS] " : "[FAIL] ") << name << '\n';
    return condition;
}

bool test_camera() {
    together::renderer::Camera camera;
    const DirectX::XMMATRIX view = camera.view();
    const DirectX::XMMATRIX projection = camera.projection(16.0f / 9.0f);
    DirectX::XMFLOAT4X4 view_values{};
    DirectX::XMFLOAT4X4 projection_values{};
    DirectX::XMStoreFloat4x4(&view_values, view);
    DirectX::XMStoreFloat4x4(&projection_values, projection);
    bool finite = true;
    for (const float value : std::array<float, 16>{
        view_values._11, view_values._12, view_values._13, view_values._14,
        view_values._21, view_values._22, view_values._23, view_values._24,
        view_values._31, view_values._32, view_values._33, view_values._34,
        view_values._41, view_values._42, view_values._43, view_values._44}) finite &= std::isfinite(value);
    for (const float value : std::array<float, 16>{
        projection_values._11, projection_values._12, projection_values._13, projection_values._14,
        projection_values._21, projection_values._22, projection_values._23, projection_values._24,
        projection_values._31, projection_values._32, projection_values._33, projection_values._34,
        projection_values._41, projection_values._42, projection_values._43, projection_values._44}) finite &= std::isfinite(value);
    const float original_z = DirectX::XMVectorGetZ(camera.position());
    camera.move(1.0f, 0.0f, 0.0f, 0.1f);
    camera.rotate(0.15f, 0.08f);
    camera.zoom(0.05f);
    const bool moved = DirectX::XMVectorGetZ(camera.position()) != original_z;
    return check(finite && moved, "camera matrices are finite and camera controls update pose");
}

bool test_frame_timer() {
    together::core::FrameTimer timer;
    std::this_thread::sleep_for(std::chrono::milliseconds(2));
    const auto sample = timer.tick();
    return check(sample.delta_seconds >= 0.0 && sample.frames_per_second > 0.0 &&
        std::isfinite(sample.delta_seconds) && std::isfinite(sample.frames_per_second),
        "frame timer reports finite positive elapsed time and FPS");
}

bool test_window_and_renderer() {
    HINSTANCE instance = GetModuleHandleW(nullptr);
    WNDCLASSEXW window_class{sizeof(WNDCLASSEXW)};
    window_class.lpfnWndProc = test_window_proc;
    window_class.hInstance = instance;
    window_class.lpszClassName = kTestWindowClass;
    if (!RegisterClassExW(&window_class)) {
        return check(false, "hidden Win32 test window creation");
    }
    HWND window = CreateWindowExW(0, kTestWindowClass, L"Together Engine Test",
        WS_OVERLAPPEDWINDOW, 0, 0, 640, 400, nullptr, nullptr, instance, nullptr);
    bool passed = check(window != nullptr, "hidden Win32 test window creation");
    if (window) {
        {
            together::renderer::D3D11Renderer renderer;
            const bool initialized = renderer.initialize(window, 640, 400);
            passed &= check(initialized, "D3D11 renderer initialization (hardware or WARP)");
            if (initialized) {
                renderer.render(0.016f, 0.016f);
                const auto first = renderer.stats();
                renderer.resize(800, 500);
                renderer.render(0.032f, 0.016f);
                const auto second = renderer.stats();
                passed &= check(first.frame_count == 1 && first.draw_calls > 0 &&
                    second.frame_count == 2 && second.resize_count == 1 &&
                    second.width == 800 && second.height == 500 &&
                    second.last_frame_ms > 0.0,
                    "render and resize update renderer diagnostics");
            }
        }
        passed &= check(IsWindow(window) != FALSE, "renderer resource cleanup leaves host window valid");
        DestroyWindow(window);
    }
    UnregisterClassW(kTestWindowClass, instance);
    return passed;
}

} // namespace

int main() {
    bool passed = true;
    passed &= test_camera();
    passed &= test_frame_timer();
    passed &= test_window_and_renderer();
    return passed ? 0 : 1;
}
