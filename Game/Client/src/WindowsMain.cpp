#include "TogetherEngine/Core/FrameTimer.h"
#include "TogetherEngine/Core/Logger.h"
#include "TogetherEngine/Renderer/D3D11Renderer.h"

#include <Windows.h>
#include <windowsx.h>

#include <array>
#include <filesystem>
#include <iomanip>
#include <memory>
#include <sstream>

namespace {

constexpr wchar_t kWindowClass[] = L"TogetherWeDoWindow";
std::unique_ptr<together::renderer::D3D11Renderer> g_renderer;
bool g_mouse_look{};
bool g_minimized{};
POINT g_last_mouse{};

LRESULT CALLBACK window_proc(HWND window, UINT message, WPARAM wparam, LPARAM lparam) {
    switch (message) {
    case WM_SIZE:
        g_minimized = (wparam == SIZE_MINIMIZED);
        if (g_renderer && !g_minimized) {
            g_renderer->resize(static_cast<unsigned>(LOWORD(lparam)), static_cast<unsigned>(HIWORD(lparam)));
        }
        return 0;
    case WM_RBUTTONDOWN:
        g_mouse_look = true;
        g_last_mouse = {GET_X_LPARAM(lparam), GET_Y_LPARAM(lparam)};
        SetCapture(window);
        return 0;
    case WM_RBUTTONUP:
        g_mouse_look = false;
        ReleaseCapture();
        return 0;
    case WM_CAPTURECHANGED:
        g_mouse_look = false;
        return 0;
    case WM_KILLFOCUS:
        g_mouse_look = false;
        ReleaseCapture();
        return 0;
    case WM_MOUSEMOVE:
        if (g_mouse_look && g_renderer) {
            const POINT current{GET_X_LPARAM(lparam), GET_Y_LPARAM(lparam)};
            const float dx = static_cast<float>(current.x - g_last_mouse.x);
            const float dy = static_cast<float>(current.y - g_last_mouse.y);
            g_renderer->rotate_camera(dx * 0.004f, -dy * 0.004f);
            g_last_mouse = current;
        }
        return 0;
    case WM_MOUSEWHEEL:
        if (g_renderer) g_renderer->zoom_camera(static_cast<float>(GET_WHEEL_DELTA_WPARAM(wparam)) / WHEEL_DELTA * 0.08f);
        return 0;
    case WM_DESTROY:
        PostQuitMessage(0);
        return 0;
    default:
        return DefWindowProcW(window, message, wparam, lparam);
    }
}

bool key_down(const int key) { return (GetAsyncKeyState(key) & 0x8000) != 0; }

} // namespace

int WINAPI wWinMain(HINSTANCE instance, HINSTANCE, PWSTR, int show_command) {
    using together::core::LogLevel;
    using together::core::Logger;
    std::array<wchar_t, 32768> module_path{};
    const DWORD path_length = GetModuleFileNameW(nullptr, module_path.data(), static_cast<DWORD>(module_path.size()));
    if (path_length > 0 && path_length < module_path.size()) {
        Logger::instance().set_file(std::filesystem::path(module_path.data()).parent_path() / L"TogetherWeDo.log");
    } else {
        Logger::instance().set_file(L"TogetherWeDo.log");
    }

    WNDCLASSEXW window_class{sizeof(WNDCLASSEXW)};
    window_class.style = CS_HREDRAW | CS_VREDRAW;
    window_class.lpfnWndProc = window_proc;
    window_class.hInstance = instance;
    window_class.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    window_class.lpszClassName = kWindowClass;
    if (!RegisterClassExW(&window_class)) {
        Logger::instance().write(LogLevel::Error, "Could not register the application window class.");
        return 1;
    }

    RECT bounds{0, 0, 1280, 720};
    AdjustWindowRect(&bounds, WS_OVERLAPPEDWINDOW, FALSE);
    HWND window = CreateWindowExW(0, kWindowClass, L"Together We Do — Together Engine Diagnostic Scene",
        WS_OVERLAPPEDWINDOW, CW_USEDEFAULT, CW_USEDEFAULT,
        bounds.right - bounds.left, bounds.bottom - bounds.top,
        nullptr, nullptr, instance, nullptr);
    if (!window) {
        Logger::instance().write(LogLevel::Error, "Could not create the application window.");
        return 1;
    }

    g_renderer = std::make_unique<together::renderer::D3D11Renderer>();
    RECT client{};
    GetClientRect(window, &client);
    if (!g_renderer->initialize(window, static_cast<unsigned>(client.right), static_cast<unsigned>(client.bottom))) {
        MessageBoxW(window, L"Direct3D 11 initialization failed. See TogetherWeDo.log for details.",
            L"Together We Do", MB_OK | MB_ICONERROR);
        g_renderer.reset();
        DestroyWindow(window);
        UnregisterClassW(kWindowClass, instance);
        return 1;
    }

    ShowWindow(window, show_command);
    Logger::instance().write(LogLevel::Info, "Together Engine diagnostic environment started.");
    together::core::FrameTimer frame_timer;
    double elapsed_seconds = 0.0;
    double fps_window_seconds = 0.0;
    unsigned fps_window_frames = 0;
    MSG message{};
    bool running = true;
    while (running) {
        while (PeekMessageW(&message, nullptr, 0, 0, PM_REMOVE)) {
            if (message.message == WM_QUIT) {
                running = false;
                break;
            }
            TranslateMessage(&message);
            DispatchMessageW(&message);
        }
        if (!running) break;
        if (g_minimized) {
            WaitMessage();
            frame_timer.reset();
            continue;
        }

        const auto frame = frame_timer.tick();
        const float delta = static_cast<float>(frame.delta_seconds);
        const float forward = static_cast<float>(key_down('W')) - static_cast<float>(key_down('S'));
        const float right = static_cast<float>(key_down('D')) - static_cast<float>(key_down('A'));
        const float up = static_cast<float>(key_down(VK_SPACE)) - static_cast<float>(key_down(VK_CONTROL));
        g_renderer->move_camera(forward, right, up, delta);
        elapsed_seconds += frame.delta_seconds;
        fps_window_seconds += frame.delta_seconds;
        ++fps_window_frames;
        g_renderer->render(static_cast<float>(elapsed_seconds), delta);

        if (fps_window_seconds >= 0.5) {
            const double fps = fps_window_frames / fps_window_seconds;
            std::wostringstream title;
            title << L"Together We Do — Diagnostic Scene | " << std::fixed << std::setprecision(1)
                  << fps << L" FPS | " << delta * 1000.0f << L" ms/frame";
            SetWindowTextW(window, title.str().c_str());
            fps_window_seconds = 0.0;
            fps_window_frames = 0;
        }
    }
    g_renderer.reset();
    UnregisterClassW(kWindowClass, instance);
    return static_cast<int>(message.wParam);
}
