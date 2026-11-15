#include "TogetherEngine/Core/FrameTimer.h"
#include "TogetherEngine/Core/Logger.h"
#include "TogetherEngine/Assets/AssetPipeline.h"
#include "TogetherEngine/Renderer/D3D11Renderer.h"
#include "TogetherEngine/Scene/Scene.h"

#include <Windows.h>
#include <commdlg.h>
#include <shellapi.h>
#include <windowsx.h>
#include <uxtheme.h>

#include <algorithm>
#include <array>
#include <cctype>
#include <chrono>
#include <cmath>
#include <cstring>
#include <cwctype>
#include <cwchar>
#include <filesystem>
#include <exception>
#include <future>
#include <memory>
#include <iterator>
#include <sstream>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#ifndef TWDO_ROOT_DIR
#define TWDO_ROOT_DIR "."
#endif

namespace {

using together::renderer::D3D11Renderer;
using together::renderer::RenderObject;
using together::scene::Scene;
using together::scene::SceneObject;

constexpr wchar_t kMainClass[] = L"TogetherEngineEditorWindow";
constexpr wchar_t kViewportClass[] = L"TogetherEngineEditorViewport";
constexpr UINT_PTR kEditorTimer = 1;
constexpr int kToolbarY = 4;
constexpr int kToolbarHeight = 34;
constexpr int kLeftWidth = 222;
constexpr int kRightWidth = 310;
constexpr int kConsoleHeight = 156;

enum ControlId : int {
    IdViewport = 100,
    IdSearch,
    IdObjectList,
    IdCreate,
    IdDelete,
    IdConsole,
    IdConsoleFilter,
    IdBuild,
    IdVisible,
    IdApply,
    IdSelectTool,
    IdMoveTool,
    IdRotateTool,
    IdScaleTool,
    IdPlay,
    IdPause,
    IdStop,
    IdPositionX = 300,
    IdPositionY,
    IdPositionZ,
    IdRotationX,
    IdRotationY,
    IdRotationZ,
    IdScaleX,
    IdScaleY,
    IdScaleZ,
    IdColorR,
    IdColorG,
    IdColorB,
    IdRoughness,
    IdMetallic,
    IdEmissionR,
    IdEmissionG,
    IdEmissionB,
    IdAssetDirectory = 400, IdAssetGo, IdAssetSearch, IdAssetFilter, IdAssetList,
    IdAssetInfo, IdAssetPreview, IdAssetImport, IdAssetRefresh, IdAssetUp
};

enum MenuId : UINT {
    MenuNew = 1000, MenuOpen, MenuSave, MenuSaveAs, MenuExit,
    MenuDelete = 1020, MenuDuplicate, MenuResetTransform,
    MenuValidate = 1040, MenuCreateCube = 1060,
    MenuOpenSceneFolder = 1080, MenuResetLayout, MenuFocusViewport,
    MenuAbout = 1100
};

enum class LogKind { Info, Build, Warning, Error };
enum class TransformTool { Select, Move, Rotate, Scale };

struct ConsoleEntry { LogKind kind; std::wstring message; };
struct AssetBrowserEntry { std::filesystem::path path; bool directory{}; std::string asset_id; };
enum class PendingGpuAsset { None, Texture, Model };

struct EditorState {
    HWND window{};
    HWND viewport{};
    HWND search{};
    HWND object_list{};
    HWND console{};
    HWND console_filter{};
    HWND visible{};
    HWND inspector_apply{};
    std::array<HWND, 3> position{};
    std::array<HWND, 3> rotation{};
    std::array<HWND, 3> scale{};
    std::array<HWND, 3> color{};
    HWND roughness{};
    HWND metallic{};
    std::array<HWND, 7> inspector_labels{};
    std::array<HWND, 3> emission{};
    HWND console_title{};
    HWND tool_buttons[4]{};
    HWND play_button{};
    HWND pause_button{};
    HWND stop_button{};
    HWND build_button{};
    HWND asset_directory{};
    HWND asset_search{};
    HWND asset_filter{};
    HWND asset_list{};
    HWND asset_info{};
    HWND asset_preview{};
    HWND asset_go{};
    HWND asset_refresh{};
    HWND asset_up{};
    HWND asset_import_button{};
    HBRUSH panel_brush{};
    HBRUSH edit_brush{};
    HBRUSH console_brush{};
    D3D11Renderer renderer;
    Scene scene{"Untitled Scene"};
    std::unique_ptr<together::assets::AssetPipeline> assets;
    std::future<together::assets::AssetImportResult> asset_import;
    std::future<bool> gpu_asset_upload;
    std::filesystem::path asset_import_path;
    std::string latest_material_asset_id;
    std::string latest_texture_asset_id;
    std::string latest_asset_id;
    std::string pending_material_replacement_id;
    PendingGpuAsset pending_gpu_type{PendingGpuAsset::None};
    std::string pending_gpu_asset_id;
    std::string pending_gpu_asset_name;
    Scene play_snapshot;
    std::vector<RenderObject> render_objects;
    std::vector<ConsoleEntry> console_entries;
    std::filesystem::path scene_path;
    std::filesystem::path root_path{TWDO_ROOT_DIR};
    std::filesystem::path asset_browser_directory;
    std::vector<AssetBrowserEntry> asset_browser_entries;
    std::string browser_drag_asset_id;
    together::assets::AssetType browser_drag_asset_type{together::assets::AssetType::Texture};
    bool browser_dragging{};
    std::uint64_t selected_id{};
    bool dirty{};
    bool updating_list{};
    bool playing{};
    bool paused{};
    bool camera_drag{};
    bool object_drag{};
    bool minimized{};
    TransformTool tool{TransformTool::Select};
    POINT last_mouse{};
    together::core::FrameTimer timer;
    double elapsed_seconds{};
    double fps_elapsed{};
    unsigned fps_frames{};
    HANDLE build_process{};
    HANDLE build_thread{};
    HANDLE build_output{};
    std::string build_pending;
};

std::unique_ptr<EditorState> g_app;
WNDPROC g_asset_list_original_proc{};

bool upload_texture_asset(D3D11Renderer& renderer, const std::string& id,
    const std::shared_ptr<const together::assets::AssetPayload>& payload) {
    if (!payload || !std::holds_alternative<together::assets::TextureData>(*payload)) return false;
    const auto& texture = std::get<together::assets::TextureData>(*payload);
    std::vector<together::renderer::TextureMipUpload> levels;
    levels.reserve(texture.mip_levels);
    unsigned width = texture.width, height = texture.height;
    levels.push_back({width, height, std::span<const std::uint8_t>(texture.rgba8)});
    for (const auto& mip : texture.mipmaps) {
        width = (std::max)(1u, width / 2);
        height = (std::max)(1u, height / 2);
        levels.push_back({width, height, std::span<const std::uint8_t>(mip)});
    }
    return renderer.register_texture_rgba8_mipped(id, levels);
}

bool upload_model_asset(D3D11Renderer& renderer, const std::string& id,
    const std::shared_ptr<const together::assets::AssetPayload>& payload) {
    if (!payload || !std::holds_alternative<together::assets::ModelData>(*payload)) return false;
    const auto& model = std::get<together::assets::ModelData>(*payload);
    std::vector<std::vector<together::renderer::ModelVertex>> vertices(model.primitives.size());
    std::vector<together::renderer::ModelPrimitiveUpload> upload;
    upload.reserve(model.primitives.size());
    for (std::size_t i = 0; i < model.primitives.size(); ++i) {
        const auto& primitive = model.primitives[i];
        auto& converted = vertices[i];
        converted.reserve(primitive.vertices.size());
        for (const auto& vertex : primitive.vertices) {
            converted.push_back({{vertex.position[0], vertex.position[1], vertex.position[2]},
                {vertex.normal[0], vertex.normal[1], vertex.normal[2]}, {vertex.uv[0], vertex.uv[1]},
                {vertex.tangent[0], vertex.tangent[1], vertex.tangent[2], vertex.tangent[3]}});
        }
    }
    for (std::size_t i = 0; i < model.primitives.size(); ++i) {
        const auto& primitive = model.primitives[i];
        upload.push_back({std::span<const together::renderer::ModelVertex>(vertices[i]),
            std::span<const std::uint32_t>(primitive.indices),
            {primitive.base_color[0], primitive.base_color[1], primitive.base_color[2], primitive.base_color[3]},
            primitive.roughness, primitive.metallic,
            {primitive.emission[0], primitive.emission[1], primitive.emission[2]},
            {primitive.transform[0], primitive.transform[1], primitive.transform[2], primitive.transform[3],
             primitive.transform[4], primitive.transform[5], primitive.transform[6], primitive.transform[7],
             primitive.transform[8], primitive.transform[9], primitive.transform[10], primitive.transform[11],
             primitive.transform[12], primitive.transform[13], primitive.transform[14], primitive.transform[15]}});
    }
    return renderer.register_model(id, upload);
}

bool asset_operation_busy() {
    return g_app && (g_app->asset_import.valid() || g_app->gpu_asset_upload.valid());
}

std::wstring to_wide(const std::string_view text) {
    if (text.empty()) return {};
    const int required = MultiByteToWideChar(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), nullptr, 0);
    if (required <= 0) return L"<invalid UTF-8>";
    std::wstring result(static_cast<std::size_t>(required), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), result.data(), required);
    return result;
}

std::string to_utf8(const std::wstring_view text) {
    if (text.empty()) return {};
    const int required = WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, text.data(),
        static_cast<int>(text.size()), nullptr, 0, nullptr, nullptr);
    if (required <= 0) return {};
    std::string result(static_cast<std::size_t>(required), '\0');
    WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, text.data(), static_cast<int>(text.size()),
        result.data(), required, nullptr, nullptr);
    return result;
}

HWND make_control(HWND parent, const wchar_t* type, const wchar_t* text, DWORD style,
    int id, DWORD ex_style = 0) {
    HWND control = CreateWindowExW(ex_style, type, text, WS_CHILD | WS_VISIBLE | style,
        0, 0, 10, 10, parent, reinterpret_cast<HMENU>(static_cast<INT_PTR>(id)),
        GetModuleHandleW(nullptr), nullptr);
    if (control) SetWindowTheme(control, L"DarkMode_Explorer", nullptr);
    return control;
}

void append_console(LogKind kind, std::wstring message);
void refresh_hierarchy();
void refresh_inspector();
void sync_renderer_scene();
void refresh_asset_browser();
void update_title();
void layout_controls(int width, int height);
void set_dirty();
bool save_scene(bool choose_path = false);

SceneObject* selected_object() {
    return g_app && g_app->selected_id ? g_app->scene.find_object(g_app->selected_id) : nullptr;
}

std::wstring read_control_text(HWND control) {
    const int length = GetWindowTextLengthW(control);
    std::wstring text(static_cast<std::size_t>(std::max(length, 0)) + 1, L'\0');
    const int copied = GetWindowTextW(control, text.data(), static_cast<int>(text.size()));
    text.resize(static_cast<std::size_t>(std::max(copied, 0)));
    return text;
}

void write_float(HWND control, const float value) {
    wchar_t buffer[48]{};
    swprintf_s(buffer, L"%.4g", static_cast<double>(value));
    SetWindowTextW(control, buffer);
}

bool read_float(HWND control, float& value) {
    const std::wstring text = read_control_text(control);
    if (text.empty()) return false;
    wchar_t* end{};
    const float parsed = std::wcstof(text.c_str(), &end);
    if (end == text.c_str() || *end != L'\0' || !std::isfinite(parsed)) return false;
    value = parsed;
    return true;
}

void set_control_enabled(HWND control, bool enabled) {
    if (control) EnableWindow(control, enabled ? TRUE : FALSE);
}

void append_console(const LogKind kind, std::wstring message) {
    if (!g_app) return;
    g_app->console_entries.push_back({kind, std::move(message)});
    if (g_app->console_entries.size() > 2000) g_app->console_entries.erase(g_app->console_entries.begin(), g_app->console_entries.begin() + 500);
    if (g_app->console) {
        SendMessageW(g_app->console, LB_RESETCONTENT, 0, 0);
        const LRESULT filter = SendMessageW(g_app->console_filter, CB_GETCURSEL, 0, 0);
        for (const auto& entry : g_app->console_entries) {
            const bool show = filter == 0 || filter == CB_ERR ||
                (filter == 1 && entry.kind == LogKind::Build) ||
                (filter == 2 && entry.kind == LogKind::Warning) ||
                (filter == 3 && entry.kind == LogKind::Error);
            if (show) SendMessageW(g_app->console, LB_ADDSTRING, 0, reinterpret_cast<LPARAM>(entry.message.c_str()));
        }
        const LRESULT count = SendMessageW(g_app->console, LB_GETCOUNT, 0, 0);
        if (count > 0) SendMessageW(g_app->console, LB_SETTOPINDEX, static_cast<WPARAM>(count - 1), 0);
    }
}

void refresh_console_filter() {
    if (!g_app) return;
    const LRESULT filter = SendMessageW(g_app->console_filter, CB_GETCURSEL, 0, 0);
    SendMessageW(g_app->console, LB_RESETCONTENT, 0, 0);
    for (const auto& entry : g_app->console_entries) {
        const bool show = filter == 0 || filter == CB_ERR ||
            (filter == 1 && entry.kind == LogKind::Build) ||
            (filter == 2 && entry.kind == LogKind::Warning) ||
            (filter == 3 && entry.kind == LogKind::Error);
        if (show) SendMessageW(g_app->console, LB_ADDSTRING, 0, reinterpret_cast<LPARAM>(entry.message.c_str()));
    }
}

void sync_renderer_scene() {
    if (!g_app) return;
    g_app->render_objects.clear();
    g_app->render_objects.reserve(g_app->scene.objects().size());
    for (const SceneObject& object : g_app->scene.objects()) {
        RenderObject render_object;
        render_object.position = {object.transform.position.x, object.transform.position.y, object.transform.position.z};
        render_object.rotation_degrees = {object.transform.rotation.x, object.transform.rotation.y, object.transform.rotation.z};
        render_object.scale = {object.transform.scale.x, object.transform.scale.y, object.transform.scale.z};
        render_object.base_color = {object.material.base_color.x, object.material.base_color.y,
            object.material.base_color.z, 1.0f};
        render_object.emission = {object.material.emission.x, object.material.emission.y, object.material.emission.z};
        render_object.roughness = object.material.roughness;
        render_object.metallic = object.material.metallic;
        render_object.model_asset_id = object.model_asset_id;
        render_object.material_asset_id = object.material_asset_id;
        render_object.base_color_texture_id = object.material.base_color_texture_id;
        render_object.normal_texture_id = object.material.normal_texture_id.empty() ? std::string{} : object.material.normal_texture_id + "#linear";
        render_object.metallic_roughness_texture_id = object.material.metallic_roughness_texture_id.empty() ? std::string{} : object.material.metallic_roughness_texture_id + "#linear";
        render_object.occlusion_texture_id = object.material.occlusion_texture_id.empty() ? std::string{} : object.material.occlusion_texture_id + "#linear";
        render_object.material_override = !object.material_asset_id.empty();
        render_object.visible = object.visible;
        render_object.selected = object.id == g_app->selected_id;
        g_app->render_objects.push_back(render_object);
    }
    g_app->renderer.set_editor_scene(g_app->render_objects);
}

void refresh_hierarchy() {
    if (!g_app || !g_app->object_list) return;
    g_app->updating_list = true;
    SendMessageW(g_app->object_list, LB_RESETCONTENT, 0, 0);
    std::wstring filter = read_control_text(g_app->search);
    const auto lowercase_character = [](wchar_t ch) { return static_cast<wchar_t>(std::towlower(ch)); };
    std::transform(filter.begin(), filter.end(), filter.begin(), lowercase_character);
    int selected_row = -1;
    int row = 0;
    for (const SceneObject& object : g_app->scene.objects()) {
        std::wstring name = to_wide(object.name);
        std::wstring lowercase = name;
        std::transform(lowercase.begin(), lowercase.end(), lowercase.begin(), lowercase_character);
        if (!filter.empty() && lowercase.find(filter) == std::wstring::npos) continue;
        const std::wstring line = (object.visible ? L"  " : L"◌ ") + name;
        const LRESULT added = SendMessageW(g_app->object_list, LB_ADDSTRING, 0, reinterpret_cast<LPARAM>(line.c_str()));
        if (added != LB_ERR && added != LB_ERRSPACE) {
            SendMessageW(g_app->object_list, LB_SETITEMDATA, static_cast<WPARAM>(added), static_cast<LPARAM>(object.id));
            if (object.id == g_app->selected_id) selected_row = row;
            ++row;
        }
    }
    if (selected_row >= 0) SendMessageW(g_app->object_list, LB_SETCURSEL, static_cast<WPARAM>(selected_row), 0);
    g_app->updating_list = false;
}

void refresh_inspector() {
    if (!g_app) return;
    SceneObject* object = selected_object();
    const bool editable = object && !g_app->playing;
    for (HWND field : g_app->position) set_control_enabled(field, editable);
    for (HWND field : g_app->rotation) set_control_enabled(field, editable);
    for (HWND field : g_app->scale) set_control_enabled(field, editable);
    for (HWND field : g_app->color) set_control_enabled(field, editable);
    set_control_enabled(g_app->roughness, editable);
    set_control_enabled(g_app->metallic, editable);
    for (HWND field : g_app->emission) set_control_enabled(field, editable);
    set_control_enabled(g_app->visible, editable);
    set_control_enabled(g_app->inspector_apply, editable);
    if (!object) {
        for (HWND field : g_app->position) SetWindowTextW(field, L"");
        for (HWND field : g_app->rotation) SetWindowTextW(field, L"");
        for (HWND field : g_app->scale) SetWindowTextW(field, L"");
        for (HWND field : g_app->color) SetWindowTextW(field, L"");
        SetWindowTextW(g_app->roughness, L"");
        SetWindowTextW(g_app->metallic, L"");
        for (HWND field : g_app->emission) SetWindowTextW(field, L"");
        SendMessageW(g_app->visible, BM_SETCHECK, BST_UNCHECKED, 0);
        return;
    }
    const std::array<float, 3> position{object->transform.position.x, object->transform.position.y, object->transform.position.z};
    const std::array<float, 3> rotation{object->transform.rotation.x, object->transform.rotation.y, object->transform.rotation.z};
    const std::array<float, 3> scale{object->transform.scale.x, object->transform.scale.y, object->transform.scale.z};
    const std::array<float, 3> color{object->material.base_color.x, object->material.base_color.y, object->material.base_color.z};
    const std::array<float, 3> emission{object->material.emission.x, object->material.emission.y, object->material.emission.z};
    for (std::size_t i = 0; i < 3; ++i) {
        write_float(g_app->position[i], position[i]);
        write_float(g_app->rotation[i], rotation[i]);
        write_float(g_app->scale[i], scale[i]);
        write_float(g_app->color[i], color[i]);
        write_float(g_app->emission[i], emission[i]);
    }
    write_float(g_app->roughness, object->material.roughness);
    write_float(g_app->metallic, object->material.metallic);
    SendMessageW(g_app->visible, BM_SETCHECK, object->visible ? BST_CHECKED : BST_UNCHECKED, 0);
}

void update_title() {
    if (!g_app) return;
    std::wstring scene_name = to_wide(g_app->scene.name());
    std::wstring title = L"Together Engine Editor 0.1 — " + scene_name;
    if (g_app->dirty) title += L" *";
    if (g_app->playing) title += g_app->paused ? L" [Paused]" : L" [Play Preview]";
    SetWindowTextW(g_app->window, title.c_str());
}

void set_dirty() {
    if (!g_app) return;
    g_app->dirty = true;
    update_title();
}

void refresh_scene_ui() {
    refresh_hierarchy();
    refresh_inspector();
    sync_renderer_scene();
    update_title();
}

bool confirm_discard_changes() {
    if (g_app->playing) {
        MessageBoxW(g_app->window, L"Stop Play Preview before replacing or closing the scene.", L"Play Preview Active", MB_OK | MB_ICONINFORMATION);
        return false;
    }
    if (!g_app->dirty) return true;
    const int answer = MessageBoxW(g_app->window,
        L"This scene has unsaved changes. Save them before continuing?",
        L"Unsaved Scene", MB_YESNOCANCEL | MB_ICONWARNING);
    if (answer == IDCANCEL) return false;
    return answer == IDNO || save_scene(false);
}

std::wstring scene_name_from_path(const std::filesystem::path& path) {
    std::wstring name = path.filename().wstring();
    constexpr std::wstring_view extension = L".tws.json";
    if (name.size() >= extension.size() && name.substr(name.size() - extension.size()) == extension)
        name.resize(name.size() - extension.size());
    return name.empty() ? L"Untitled Scene" : name;
}

bool select_scene_path(std::filesystem::path& path, const bool save) {
    std::array<wchar_t, 32768> filename{};
    if (!g_app->scene_path.empty()) wcsncpy_s(filename.data(), filename.size(), g_app->scene_path.c_str(), _TRUNCATE);
    OPENFILENAMEW dialog{sizeof(dialog)};
    dialog.hwndOwner = g_app->window;
    dialog.lpstrFilter = L"Together Engine Scene (*.tws.json)\0*.tws.json\0JSON files (*.json)\0*.json\0All files (*.*)\0*.*\0\0";
    dialog.lpstrFile = filename.data();
    dialog.nMaxFile = static_cast<DWORD>(filename.size());
    dialog.lpstrDefExt = L"tws.json";
    dialog.Flags = OFN_PATHMUSTEXIST | (save ? OFN_OVERWRITEPROMPT : OFN_FILEMUSTEXIST);
    const BOOL chosen = save ? GetSaveFileNameW(&dialog) : GetOpenFileNameW(&dialog);
    if (!chosen) return false;
    path = filename.data();
    return true;
}

bool save_scene(const bool choose_path) {
    if (g_app->playing) {
        append_console(LogKind::Warning, L"Stop Play Preview before saving the scene.");
        return false;
    }
    std::filesystem::path target = g_app->scene_path;
    const bool requested_path = choose_path || target.empty();
    if (requested_path) {
        if (!select_scene_path(target, true)) return false;
    }
    std::string error;
    const std::string previous_name = g_app->scene.name();
    if (requested_path) g_app->scene.set_name(to_utf8(scene_name_from_path(target)));
    if (!g_app->scene.save(target, error)) {
        g_app->scene.set_name(previous_name);
        append_console(LogKind::Error, to_wide(error));
        MessageBoxW(g_app->window, to_wide(error).c_str(), L"Could Not Save Scene", MB_OK | MB_ICONERROR);
        return false;
    }
    g_app->scene_path = target;
    g_app->dirty = false;
    append_console(LogKind::Info, L"Saved scene: " + target.wstring());
    refresh_scene_ui();
    return true;
}

void new_scene() {
    if (!confirm_discard_changes()) return;
    g_app->scene = Scene{};
    g_app->scene_path.clear();
    g_app->selected_id = 0;
    g_app->dirty = false;
    append_console(LogKind::Info, L"Created a new scene.");
    refresh_scene_ui();
}

void open_scene() {
    if (!confirm_discard_changes()) return;
    std::filesystem::path path;
    if (!select_scene_path(path, false)) return;
    Scene loaded;
    std::string error;
    if (!Scene::load(path, loaded, error)) {
        append_console(LogKind::Error, to_wide(error));
        MessageBoxW(g_app->window, to_wide(error).c_str(), L"Could Not Open Scene", MB_OK | MB_ICONERROR);
        return;
    }
    g_app->scene = std::move(loaded);
    g_app->scene_path = path;
    g_app->selected_id = 0;
    g_app->dirty = false;
    append_console(LogKind::Info, L"Opened scene: " + path.wstring());
    for (const SceneObject& object : g_app->scene.objects()) {
        together::assets::AssetMetadata metadata;
        if (!object.model_asset_id.empty() && !g_app->assets->find(object.model_asset_id, metadata))
            append_console(LogKind::Warning, L"Missing model asset reference preserved in scene: " + to_wide(object.model_asset_id));
        if (!object.material_asset_id.empty() && !g_app->assets->find(object.material_asset_id, metadata))
            append_console(LogKind::Warning, L"Missing material asset reference preserved in scene: " + to_wide(object.material_asset_id));
        if (!object.material.base_color_texture_id.empty() && !g_app->assets->find(object.material.base_color_texture_id, metadata))
            append_console(LogKind::Warning, L"Missing texture asset reference preserved in scene: " + to_wide(object.material.base_color_texture_id));
        if (!object.material.normal_texture_id.empty() && !g_app->assets->find(object.material.normal_texture_id, metadata))
            append_console(LogKind::Warning, L"Missing normal texture reference preserved in scene: " + to_wide(object.material.normal_texture_id));
        if (!object.material.metallic_roughness_texture_id.empty() && !g_app->assets->find(object.material.metallic_roughness_texture_id, metadata))
            append_console(LogKind::Warning, L"Missing metallic-roughness texture reference preserved in scene: " + to_wide(object.material.metallic_roughness_texture_id));
        if (!object.material.occlusion_texture_id.empty() && !g_app->assets->find(object.material.occlusion_texture_id, metadata))
            append_console(LogKind::Warning, L"Missing AO texture reference preserved in scene: " + to_wide(object.material.occlusion_texture_id));
    }
    refresh_scene_ui();
}

void create_cube() {
    if (g_app->playing) return;
    SceneObject& object = g_app->scene.create_object();
    g_app->selected_id = object.id;
    set_dirty();
    append_console(LogKind::Info, L"Created " + to_wide(object.name) + L".");
    refresh_scene_ui();
}

void delete_selected() {
    if (g_app->playing || !g_app->selected_id) return;
    const auto id = g_app->selected_id;
    const SceneObject* object = g_app->scene.find_object(id);
    const std::wstring name = object ? to_wide(object->name) : L"object";
    if (!g_app->scene.delete_object(id)) return;
    g_app->selected_id = 0;
    set_dirty();
    append_console(LogKind::Info, L"Deleted " + name + L".");
    refresh_scene_ui();
}

void duplicate_selected() {
    if (g_app->playing || !g_app->selected_id) return;
    SceneObject& object = g_app->scene.duplicate_object(g_app->selected_id);
    g_app->selected_id = object.id;
    set_dirty();
    append_console(LogKind::Info, L"Duplicated object as " + to_wide(object.name) + L".");
    refresh_scene_ui();
}

void reset_transform() {
    SceneObject* object = selected_object();
    if (!object || g_app->playing) return;
    object->transform = {};
    object->transform.position.y = 0.5f;
    set_dirty();
    refresh_scene_ui();
    append_console(LogKind::Info, L"Reset selected object transform.");
}

void apply_inspector() {
    SceneObject* object = selected_object();
    if (!object || g_app->playing) return;
    std::array<float, 3> position{}, rotation{}, scale{}, color{}, emission{};
    for (std::size_t i = 0; i < 3; ++i) {
        if (!read_float(g_app->position[i], position[i]) || !read_float(g_app->rotation[i], rotation[i]) ||
            !read_float(g_app->scale[i], scale[i]) || !read_float(g_app->color[i], color[i]) ||
            !read_float(g_app->emission[i], emission[i])) {
            append_console(LogKind::Warning, L"Inspector values must be valid finite numbers.");
            return;
        }
    }
    float roughness{}, metallic{};
    if (!read_float(g_app->roughness, roughness) || !read_float(g_app->metallic, metallic)) {
        append_console(LogKind::Warning, L"Material values must be valid finite numbers.");
        return;
    }
    const auto old_transform = object->transform;
    const auto old_material = object->material;
    std::vector<std::pair<std::uint64_t, together::scene::Material>> shared_material_snapshots;
    object->transform.position = {position[0], position[1], position[2]};
    object->transform.rotation = {rotation[0], rotation[1], rotation[2]};
    object->transform.scale = {scale[0], scale[1], scale[2]};
    object->material.base_color = {color[0], color[1], color[2]};
    object->material.roughness = roughness;
    object->material.metallic = metallic;
    object->material.emission = {emission[0], emission[1], emission[2]};
    if (!object->material_asset_id.empty()) {
        for (const SceneObject& candidate : g_app->scene.objects()) {
            if (candidate.id == object->id || candidate.material_asset_id != object->material_asset_id) continue;
            SceneObject* shared = g_app->scene.find_object(candidate.id);
            shared_material_snapshots.emplace_back(shared->id, shared->material);
            shared->material.base_color = object->material.base_color;
            shared->material.roughness = roughness;
            shared->material.metallic = metallic;
            shared->material.emission = object->material.emission;
        }
    }
    std::string error;
    if (!g_app->scene.validate(error)) {
        object->transform = old_transform;
        object->material = old_material;
        for (const auto& [id, material] : shared_material_snapshots)
            if (SceneObject* shared = g_app->scene.find_object(id)) shared->material = material;
        append_console(LogKind::Warning, to_wide(error));
        refresh_inspector();
        return;
    }
    set_dirty();
    sync_renderer_scene();
    append_console(LogKind::Info, L"Updated " + to_wide(object->name) + L" properties.");
}

void set_tool(const TransformTool tool) {
    g_app->tool = tool;
    const std::array<TransformTool, 4> tools{TransformTool::Select, TransformTool::Move, TransformTool::Rotate, TransformTool::Scale};
    constexpr std::array<const wchar_t*, 4> names{L"Select", L"Move", L"Rotate", L"Scale"};
    for (std::size_t i = 0; i < tools.size(); ++i)
        SetWindowTextW(g_app->tool_buttons[i], tools[i] == tool ? (std::wstring(names[i]) + L" *").c_str() : names[i]);
    SetFocus(g_app->viewport);
}

void start_play_preview() {
    if (g_app->playing) return;
    std::string error;
    if (!g_app->scene.validate(error)) {
        append_console(LogKind::Error, to_wide(error));
        return;
    }
    g_app->play_snapshot = g_app->scene;
    g_app->playing = true;
    g_app->paused = false;
    refresh_inspector();
    update_title();
    append_console(LogKind::Info, L"Play Preview started. The selected object rotates until paused or stopped.");
}

void pause_preview() {
    if (!g_app->playing) {
        append_console(LogKind::Warning, L"Start Play Preview before pausing.");
        return;
    }
    g_app->paused = !g_app->paused;
    update_title();
    append_console(LogKind::Info, g_app->paused ? L"Play Preview paused." : L"Play Preview resumed.");
}

void stop_preview() {
    if (!g_app->playing) return;
    g_app->scene = g_app->play_snapshot;
    g_app->playing = false;
    g_app->paused = false;
    refresh_scene_ui();
    append_console(LogKind::Info, L"Play Preview stopped; editor scene restored.");
}

void append_build_output(const char* data, const DWORD size) {
    if (!size) return;
    g_app->build_pending.append(data, size);
    for (;;) {
        const std::size_t newline = g_app->build_pending.find('\n');
        if (newline == std::string::npos) break;
        std::string line = g_app->build_pending.substr(0, newline);
        if (!line.empty() && line.back() == '\r') line.pop_back();
        const int wide_size = MultiByteToWideChar(CP_ACP, 0, line.data(), static_cast<int>(line.size()), nullptr, 0);
        std::wstring wide_line;
        if (wide_size > 0) {
            wide_line.resize(static_cast<std::size_t>(wide_size));
            MultiByteToWideChar(CP_ACP, 0, line.data(), static_cast<int>(line.size()), wide_line.data(), wide_size);
        }
        std::string lowercase = line;
        std::transform(lowercase.begin(), lowercase.end(), lowercase.begin(), [](unsigned char ch) { return static_cast<char>(std::tolower(ch)); });
        const LogKind kind = lowercase.find("fatal error") != std::string::npos || lowercase.find("error") != std::string::npos
            ? LogKind::Error : lowercase.find("warning") != std::string::npos ? LogKind::Warning : LogKind::Build;
        append_console(kind, std::move(wide_line));
        g_app->build_pending.erase(0, newline + 1);
    }
}

void poll_build_process() {
    if (!g_app->build_process) return;
    if (g_app->build_output) {
        for (;;) {
            DWORD available{};
            if (!PeekNamedPipe(g_app->build_output, nullptr, 0, nullptr, &available, nullptr) || !available) break;
            std::array<char, 4096> buffer{};
            DWORD read{};
            if (!ReadFile(g_app->build_output, buffer.data(), (std::min)(available, static_cast<DWORD>(buffer.size())), &read, nullptr) || !read) break;
            append_build_output(buffer.data(), read);
        }
    }
    if (WaitForSingleObject(g_app->build_process, 0) != WAIT_OBJECT_0) return;
    DWORD exit_code = 1;
    GetExitCodeProcess(g_app->build_process, &exit_code);
    if (!g_app->build_pending.empty()) {
        append_console(LogKind::Build, to_wide(g_app->build_pending));
        g_app->build_pending.clear();
    }
    append_console(exit_code == 0 ? LogKind::Info : LogKind::Error,
        exit_code == 0 ? L"Editor build finished successfully." : L"Editor build failed with exit code " + std::to_wstring(exit_code) + L".");
    CloseHandle(g_app->build_process);
    CloseHandle(g_app->build_thread);
    if (g_app->build_output) CloseHandle(g_app->build_output);
    g_app->build_process = g_app->build_thread = g_app->build_output = nullptr;
    EnableWindow(g_app->build_button, TRUE);
}

void start_build() {
    if (g_app->build_process) return;
    std::array<wchar_t, 32768> cmake_path{};
    if (!SearchPathW(nullptr, L"cmake.exe", nullptr, static_cast<DWORD>(cmake_path.size()), cmake_path.data(), nullptr)) {
        append_console(LogKind::Error, L"CMake was not found on PATH. Install CMake 3.24+ before using Build Editor.");
        return;
    }
    SECURITY_ATTRIBUTES security{sizeof(security), nullptr, TRUE};
    HANDLE read_pipe{}, write_pipe{};
    if (!CreatePipe(&read_pipe, &write_pipe, &security, 0)) {
        append_console(LogKind::Error, L"Could not create a pipe for build output.");
        return;
    }
    SetHandleInformation(read_pipe, HANDLE_FLAG_INHERIT, 0);
    STARTUPINFOW startup{sizeof(startup)};
    startup.dwFlags = STARTF_USESTDHANDLES | STARTF_USESHOWWINDOW;
    startup.wShowWindow = SW_HIDE;
    startup.hStdOutput = write_pipe;
    startup.hStdError = write_pipe;
    startup.hStdInput = GetStdHandle(STD_INPUT_HANDLE);
    PROCESS_INFORMATION process{};
    std::wstring command = L"\"" + std::wstring(cmake_path.data()) + L"\" --build \"" +
        (g_app->root_path / L"build" / L"windows").wstring() +
        L"\" --config Debug --target TogetherEngineEditor --parallel";
    std::vector<wchar_t> mutable_command(command.begin(), command.end());
    mutable_command.push_back(L'\0');
    const BOOL created = CreateProcessW(cmake_path.data(), mutable_command.data(), nullptr, nullptr, TRUE,
        CREATE_NO_WINDOW, nullptr, g_app->root_path.c_str(), &startup, &process);
    CloseHandle(write_pipe);
    if (!created) {
        const DWORD error = GetLastError();
        CloseHandle(read_pipe);
        append_console(LogKind::Error, L"Could not start CMake build (Windows error " + std::to_wstring(error) + L").");
        return;
    }
    g_app->build_process = process.hProcess;
    g_app->build_thread = process.hThread;
    g_app->build_output = read_pipe;
    g_app->build_pending.clear();
    EnableWindow(g_app->build_button, FALSE);
    append_console(LogKind::Build, L"Started asynchronous CMake build for TogetherEngineEditor.");
}

void validate_scene() {
    std::string error;
    if (g_app->scene.validate(error)) append_console(LogKind::Info, L"Scene validation passed.");
    else append_console(LogKind::Error, to_wide(error));
}

void open_scene_folder() {
    const std::filesystem::path folder = g_app->scene_path.empty() ? g_app->root_path : g_app->scene_path.parent_path();
    const auto result = reinterpret_cast<INT_PTR>(ShellExecuteW(g_app->window, L"open", folder.c_str(), nullptr, nullptr, SW_SHOWNORMAL));
    if (result <= 32) append_console(LogKind::Error, L"Could not open scene folder (Shell error " + std::to_wstring(result) + L").");
}

void import_asset() {
    wchar_t path[MAX_PATH]{};
    OPENFILENAMEW dialog{};
    dialog.lStructSize = sizeof(dialog);
    dialog.hwndOwner = g_app->window;
    dialog.lpstrFilter = L"Supported assets\0Images (*.png;*.jpg;*.jpeg;*.tga), Models (*.gltf;*.glb), Audio (*.wav), Materials (*.twmat.json)\0*.png;*.jpg;*.jpeg;*.tga;*.gltf;*.glb;*.wav;*.twmat.json\0All files\0*.*\0\0";
    dialog.lpstrFile = path;
    dialog.nMaxFile = MAX_PATH;
    dialog.Flags = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST | OFN_NOCHANGEDIR;
    if (!GetOpenFileNameW(&dialog)) return;
    if (asset_operation_busy()) {
        append_console(LogKind::Warning, L"An asset import is already in progress.");
        return;
    }
    g_app->asset_import_path = path;
    g_app->asset_import = g_app->assets->import_async(path);
    append_console(LogKind::Info, L"Import queued: " + std::filesystem::path(path).filename().wstring());
}

void show_asset_list() {
    const auto entries = g_app->assets->assets();
    if (entries.empty()) { append_console(LogKind::Info, L"No registered assets. Use Assets > Import Asset to add files."); return; }
    append_console(LogKind::Info, L"Registered assets: " + std::to_wstring(entries.size()));
    for (const auto& entry : entries) {
        append_console(LogKind::Info, to_wide(entry.name) + L"  [" + to_wide(together::assets::asset_type_name(entry.type)) + L"]  " + to_wide(entry.id));
    }
}

void refresh_asset_browser() {
    if (!g_app || !g_app->asset_list || !g_app->assets) return;
    g_app->updating_list = true;
    SendMessageW(g_app->asset_list, LB_RESETCONTENT, 0, 0);
    g_app->asset_browser_entries.clear();
    const std::wstring directory_text = read_control_text(g_app->asset_directory);
    if (!directory_text.empty()) g_app->asset_browser_directory = directory_text;
    std::error_code ec;
    if (!std::filesystem::is_directory(g_app->asset_browser_directory, ec)) {
        SetWindowTextW(g_app->asset_info, L"Choose an existing folder and press Go.");
        InvalidateRect(g_app->asset_preview, nullptr, TRUE);
        g_app->updating_list = false;
        return;
    }
    SetWindowTextW(g_app->asset_directory, g_app->asset_browser_directory.wstring().c_str());
    const int filter_index = static_cast<int>(SendMessageW(g_app->asset_filter, CB_GETCURSEL, 0, 0));
    std::wstring search = read_control_text(g_app->asset_search);
    std::transform(search.begin(), search.end(), search.begin(), [](wchar_t ch) { return static_cast<wchar_t>(std::towlower(ch)); });
    const auto allowed_type = [filter_index](const std::filesystem::path& path) {
        if (filter_index == 0) return true;
        std::wstring name = path.filename().wstring();
        std::transform(name.begin(), name.end(), name.begin(), [](wchar_t ch) { return static_cast<wchar_t>(std::towlower(ch)); });
        if (filter_index == 1) return name.ends_with(L".gltf") || name.ends_with(L".glb");
        if (filter_index == 2) return name.ends_with(L".png") || name.ends_with(L".jpg") || name.ends_with(L".jpeg") || name.ends_with(L".tga");
        if (filter_index == 3) return name.ends_with(L".twmat.json");
        return name.ends_with(L".wav");
    };
    const auto add_entry = [&](const std::filesystem::path& path, const bool is_directory, const std::string& id = {}) {
        const std::wstring name = path.filename().wstring();
        std::wstring folded = name;
        std::transform(folded.begin(), folded.end(), folded.begin(), [](wchar_t ch) { return static_cast<wchar_t>(std::towlower(ch)); });
        if (!search.empty() && folded.find(search) == std::wstring::npos) return;
        if (!is_directory && !allowed_type(path)) return;
        const std::wstring prefix = is_directory ? L"[Folder]  " : (id.empty() ? L"[File]  " : L"[Asset]  ");
        const LRESULT row = SendMessageW(g_app->asset_list, LB_ADDSTRING, 0, reinterpret_cast<LPARAM>((prefix + name).c_str()));
        if (row != LB_ERR && row != LB_ERRSPACE) {
            g_app->asset_browser_entries.push_back({path, is_directory, id});
            SendMessageW(g_app->asset_list, LB_SETITEMDATA, static_cast<WPARAM>(row), static_cast<LPARAM>(g_app->asset_browser_entries.size() - 1));
        }
    };
    const auto registered_assets = g_app->assets->assets();
    for (std::filesystem::directory_iterator it(g_app->asset_browser_directory, ec), end; !ec && it != end; it.increment(ec)) {
        std::error_code item_error;
        if (it->is_directory(item_error)) add_entry(it->path(), true);
        else if (it->is_regular_file(item_error)) {
            const std::wstring filename = it->path().filename().wstring();
            std::wstring folded = filename;
            std::transform(folded.begin(), folded.end(), folded.begin(), [](wchar_t ch) { return static_cast<wchar_t>(std::towlower(ch)); });
            if (folded.ends_with(L".gltf") || folded.ends_with(L".glb") || folded.ends_with(L".png") || folded.ends_with(L".jpg") ||
                folded.ends_with(L".jpeg") || folded.ends_with(L".tga") || folded.ends_with(L".wav") || folded.ends_with(L".twmat.json")) {
                std::string id;
                for (const auto& asset : registered_assets) {
                    if (asset.source_path.lexically_normal() == it->path().lexically_normal()) { id = asset.id; break; }
                }
                add_entry(it->path(), false, id);
            }
        }
    }
    if (ec) SetWindowTextW(g_app->asset_info, (L"Directory read failed: " + std::filesystem::path(g_app->asset_browser_directory).wstring()).c_str());
    else SetWindowTextW(g_app->asset_info, L"Select an asset to inspect it. Double-click a file to import it.");
    InvalidateRect(g_app->asset_preview, nullptr, TRUE);
    g_app->updating_list = false;
}

void select_asset_browser_entry(const bool activate) {
    if (!g_app || g_app->updating_list) return;
    const LRESULT row = SendMessageW(g_app->asset_list, LB_GETCURSEL, 0, 0);
    if (row == LB_ERR) return;
    const auto index = static_cast<std::size_t>(SendMessageW(g_app->asset_list, LB_GETITEMDATA, static_cast<WPARAM>(row), 0));
    if (index >= g_app->asset_browser_entries.size()) return;
    const AssetBrowserEntry entry = g_app->asset_browser_entries[index];
    if (entry.directory) {
        if (activate) {
            g_app->asset_browser_directory = entry.path;
            SetWindowTextW(g_app->asset_directory, entry.path.wstring().c_str());
            refresh_asset_browser();
        }
        return;
    }
    if (!entry.asset_id.empty()) {
        together::assets::AssetMetadata metadata;
        if (g_app->assets->find(entry.asset_id, metadata)) {
            std::wstring details = to_wide(metadata.name) + L"  |  " +
                to_wide(together::assets::asset_type_name(metadata.type)) + L"  |  " +
                std::to_wstring(metadata.file_size) + L" bytes  |  " + to_wide(metadata.format);
            if (metadata.type == together::assets::AssetType::Texture)
                details += L"  |  " + std::to_wstring(metadata.width) + L" x " + std::to_wstring(metadata.height) +
                    L"  |  " + std::to_wstring(metadata.mip_levels) + L" mips  |  decoded " + std::to_wstring(metadata.decoded_bytes) + L" bytes";
            else if (metadata.type == together::assets::AssetType::Model)
                details += L"  |  " + std::to_wstring(metadata.mesh_count) + L" primitives  |  " +
                    std::to_wstring(metadata.vertex_count) + L" vertices  |  " + std::to_wstring(metadata.index_count) + L" indices";
            else if (metadata.type == together::assets::AssetType::Audio)
                details += L"  |  " + std::to_wstring(metadata.channels) + L" channels  |  decoded " + std::to_wstring(metadata.decoded_bytes) + L" bytes";
            SetWindowTextW(g_app->asset_info, details.c_str());
            g_app->latest_asset_id = metadata.id;
            if (metadata.type == together::assets::AssetType::Texture) g_app->latest_texture_asset_id = metadata.id;
            if (metadata.type == together::assets::AssetType::Material) g_app->latest_material_asset_id = metadata.id;
            InvalidateRect(g_app->asset_preview, nullptr, TRUE);
            return;
        }
    }
    const std::wstring information = entry.path.filename().wstring() + L"  |  Not imported";
    SetWindowTextW(g_app->asset_info, information.c_str());
    InvalidateRect(g_app->asset_preview, nullptr, TRUE);
    if (!activate) return;
    if (asset_operation_busy()) { append_console(LogKind::Warning, L"Wait for the current import before importing another asset."); return; }
    g_app->asset_import_path = entry.path;
    g_app->asset_import = g_app->assets->import_async(entry.path);
    append_console(LogKind::Info, L"Import queued from Asset Browser: " + entry.path.filename().wstring());
}

void paint_asset_preview(const DRAWITEMSTRUCT& draw) {
    RECT area = draw.rcItem;
    FillRect(draw.hDC, &area, g_app->console_brush);
    if (!g_app->assets) return;
    const LRESULT row = SendMessageW(g_app->asset_list, LB_GETCURSEL, 0, 0);
    if (row == LB_ERR) return;
    const auto index = static_cast<std::size_t>(SendMessageW(g_app->asset_list, LB_GETITEMDATA, static_cast<WPARAM>(row), 0));
    if (index >= g_app->asset_browser_entries.size()) return;
    const AssetBrowserEntry& entry = g_app->asset_browser_entries[index];
    if (entry.asset_id.empty()) return;
    together::assets::AssetMetadata metadata;
    if (!g_app->assets->find(entry.asset_id, metadata)) return;
    const auto payload = g_app->assets->payload(metadata.handle);
    if (!payload) return;
    const int width = (std::max)(1, area.right - area.left - 8);
    const int height = (std::max)(1, area.bottom - area.top - 8);
    BITMAPINFO bitmap{};
    bitmap.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    bitmap.bmiHeader.biWidth = 128;
    bitmap.bmiHeader.biHeight = -128;
    bitmap.bmiHeader.biPlanes = 1;
    bitmap.bmiHeader.biBitCount = 32;
    bitmap.bmiHeader.biCompression = BI_RGB;
    std::vector<std::uint8_t> pixels(128u * 128u * 4u, 0);
    if (std::holds_alternative<together::assets::TextureData>(*payload)) {
        const auto& texture = std::get<together::assets::TextureData>(*payload);
        for (unsigned y = 0; y < 128; ++y) for (unsigned x = 0; x < 128; ++x) {
            const unsigned sx = x * texture.width / 128, sy = y * texture.height / 128;
            const std::size_t source = (static_cast<std::size_t>(sy) * texture.width + sx) * 4;
            const std::size_t target = (static_cast<std::size_t>(y) * 128 + x) * 4;
            pixels[target] = texture.rgba8[source + 2]; pixels[target + 1] = texture.rgba8[source + 1];
            pixels[target + 2] = texture.rgba8[source]; pixels[target + 3] = 255;
        }
    } else if (std::holds_alternative<together::assets::MaterialDefinition>(*payload)) {
        const auto& material = std::get<together::assets::MaterialDefinition>(*payload);
        const auto base_texture_id = material.textures[static_cast<std::size_t>(together::assets::TextureSlot::BaseColor)];
        std::shared_ptr<const together::assets::AssetPayload> base_payload;
        together::assets::AssetMetadata base_metadata;
        if (!base_texture_id.empty() && g_app->assets->find(base_texture_id, base_metadata)) base_payload = g_app->assets->payload(base_metadata.handle);
        const together::assets::TextureData* base_texture = base_payload && std::holds_alternative<together::assets::TextureData>(*base_payload)
            ? &std::get<together::assets::TextureData>(*base_payload) : nullptr;
        constexpr float light_x = -0.45f, light_y = 0.62f, light_z = 0.64f;
        const float light_length = std::sqrt(light_x*light_x + light_y*light_y + light_z*light_z);
        for (unsigned y = 0; y < 128; ++y) for (unsigned x = 0; x < 128; ++x) {
            const float nx = (static_cast<float>(x) - 63.5f) / 56.0f;
            const float ny = (63.5f - static_cast<float>(y)) / 56.0f;
            const float radius2 = nx*nx + ny*ny;
            if (radius2 > 1.0f) continue;
            const float nz = std::sqrt(1.0f - radius2);
            float u = std::atan2(nz, nx) / 6.2831853f + 0.5f;
            float v = std::acos(std::clamp(ny, -1.0f, 1.0f)) / 3.14159265f;
            if (base_texture) {
                const unsigned tx = (std::min)(base_texture->width - 1, static_cast<unsigned>(u * base_texture->width));
                const unsigned ty = (std::min)(base_texture->height - 1, static_cast<unsigned>(v * base_texture->height));
                const std::size_t at = (static_cast<std::size_t>(ty) * base_texture->width + tx) * 4;
                const float diffuse_factor = 1.0f - material.metallic;
                const float ndotl = (std::max)(0.0f, (nx*light_x + ny*light_y + nz*light_z) / light_length);
                const float highlight = std::pow(ndotl, 8.0f + (1.0f - material.roughness) * 80.0f) * (0.04f + material.metallic * 0.8f);
                const std::size_t target = (static_cast<std::size_t>(y) * 128 + x) * 4;
                const std::array<float,3> color{base_texture->rgba8[at] / 255.0f * material.base_color[0],
                    base_texture->rgba8[at+1] / 255.0f * material.base_color[1], base_texture->rgba8[at+2] / 255.0f * material.base_color[2]};
                for (unsigned c = 0; c < 3; ++c) {
                    const float shaded = color[c] * (0.16f + ndotl * 0.78f * diffuse_factor) + highlight + material.emission[c];
                    pixels[target + (2 - c)] = static_cast<std::uint8_t>(std::clamp(shaded, 0.0f, 1.0f) * 255.0f);
                }
                pixels[target + 3] = 255;
            } else {
                const float diffuse_factor = 1.0f - material.metallic;
                const float ndotl = (std::max)(0.0f, (nx*light_x + ny*light_y + nz*light_z) / light_length);
                const float highlight = std::pow(ndotl, 8.0f + (1.0f - material.roughness) * 80.0f) * (0.04f + material.metallic * 0.8f);
                const std::size_t target = (static_cast<std::size_t>(y) * 128 + x) * 4;
                for (unsigned c = 0; c < 3; ++c) {
                    const float shaded = material.base_color[c] * (0.16f + ndotl * 0.78f * diffuse_factor) + highlight + material.emission[c];
                    pixels[target + (2 - c)] = static_cast<std::uint8_t>(std::clamp(shaded, 0.0f, 1.0f) * 255.0f);
                }
                pixels[target + 3] = 255;
            }
        }
    } else if (std::holds_alternative<together::assets::ModelData>(*payload)) {
        const auto& model = std::get<together::assets::ModelData>(*payload);
        const float sx = model.bounds.maximum[0] - model.bounds.minimum[0];
        const float sy = model.bounds.maximum[1] - model.bounds.minimum[1];
        const float scale = 104.0f / (std::max)({sx, sy, 0.001f});
        for (const auto& primitive : model.primitives) {
            const COLORREF color = RGB(static_cast<BYTE>(primitive.base_color[0] * 255),
                static_cast<BYTE>(primitive.base_color[1] * 255), static_cast<BYTE>(primitive.base_color[2] * 255));
            HBRUSH brush = CreateSolidBrush(color);
            HGDIOBJ previous = SelectObject(draw.hDC, brush);
            const std::size_t triangles = (std::min)(primitive.indices.size() / 3, std::size_t{1200});
            for (std::size_t tri = 0; tri < triangles; ++tri) {
                POINT points[3]{};
                for (std::size_t corner = 0; corner < 3; ++corner) {
                    const auto& vertex = primitive.vertices[primitive.indices[tri*3+corner]];
                    points[corner].x = area.left + 4 + static_cast<LONG>((vertex.position[0] - model.bounds.minimum[0]) * scale + (128 - sx*scale)*0.5f);
                    points[corner].y = area.top + 4 + static_cast<LONG>((model.bounds.maximum[1] - vertex.position[1]) * scale + (128 - sy*scale)*0.5f);
                }
                Polygon(draw.hDC, points, 3);
            }
            SelectObject(draw.hDC, previous);
            DeleteObject(brush);
        }
        return;
    } else return;
    StretchDIBits(draw.hDC, area.left + (area.right-area.left-width)/2, area.top + (area.bottom-area.top-height)/2,
        width, height, 0, 0, 128, 128, pixels.data(), &bitmap, DIB_RGB_COLORS, SRCCOPY);
}

void create_material_asset() {
    if (asset_operation_busy()) {
        append_console(LogKind::Warning, L"Wait for the current asset import to finish before creating another asset.");
        return;
    }
    wchar_t path[MAX_PATH]{};
    wcscpy_s(path, L"NewMaterial.twmat.json");
    OPENFILENAMEW dialog{};
    dialog.lStructSize = sizeof(dialog);
    dialog.hwndOwner = g_app->window;
    dialog.lpstrFilter = L"Together Engine Material (*.twmat.json)\0*.twmat.json\0All files\0*.*\0\0";
    dialog.lpstrFile = path;
    dialog.nMaxFile = MAX_PATH;
    dialog.lpstrDefExt = L"json";
    dialog.Flags = OFN_PATHMUSTEXIST | OFN_NOCHANGEDIR;
    if (!GetSaveFileNameW(&dialog)) return;
    if (!std::wstring_view(path).ends_with(L".twmat.json")) wcscat_s(path, L".twmat.json");
    if (std::filesystem::exists(path)) {
        append_console(LogKind::Warning, L"Choose a new file name; material sources are not overwritten so existing asset references remain valid.");
        return;
    }
    together::assets::MaterialDefinition material;
    material.name = std::filesystem::path(path).filename().string();
    constexpr std::string_view material_suffix = ".twmat.json";
    if (material.name.ends_with(material_suffix)) material.name.resize(material.name.size() - material_suffix.size());
    std::string error;
    if (!material.save(path, error)) {
        append_console(LogKind::Error, L"Material save failed: " + to_wide(error));
        return;
    }
    g_app->asset_import_path = path;
    g_app->asset_import = g_app->assets->import_async(path);
    append_console(LogKind::Info, L"Material created and queued for registration.");
}

void save_selected_material_as() {
    SceneObject* object = selected_object();
    if (!object) {
        append_console(LogKind::Warning, L"Select an object before saving its material.");
        return;
    }
    if (asset_operation_busy()) {
        append_console(LogKind::Warning, L"Wait for the current asset import to finish before saving a material.");
        return;
    }
    together::assets::MaterialDefinition material;
    if (!object->material_asset_id.empty()) {
        together::assets::AssetMetadata existing;
        if (g_app->assets->find(object->material_asset_id, existing)) {
            const auto payload = g_app->assets->payload(existing.handle);
            if (payload && std::holds_alternative<together::assets::MaterialDefinition>(*payload))
                material = std::get<together::assets::MaterialDefinition>(*payload);
        }
    }
    material.base_color = {object->material.base_color.x, object->material.base_color.y,
        object->material.base_color.z, material.base_color[3]};
    material.metallic = object->material.metallic;
    material.roughness = object->material.roughness;
    material.emission = {object->material.emission.x, object->material.emission.y, object->material.emission.z};
    material.textures[static_cast<std::size_t>(together::assets::TextureSlot::BaseColor)] = object->material.base_color_texture_id;
    material.textures[static_cast<std::size_t>(together::assets::TextureSlot::Normal)] = object->material.normal_texture_id;
    material.textures[static_cast<std::size_t>(together::assets::TextureSlot::Roughness)] = object->material.metallic_roughness_texture_id;
    material.textures[static_cast<std::size_t>(together::assets::TextureSlot::Metallic)] = object->material.metallic_roughness_texture_id;
    material.textures[static_cast<std::size_t>(together::assets::TextureSlot::Occlusion)] = object->material.occlusion_texture_id;
    wchar_t path[MAX_PATH]{};
    wcscpy_s(path, L"Material.twmat.json");
    OPENFILENAMEW dialog{};
    dialog.lStructSize = sizeof(dialog);
    dialog.hwndOwner = g_app->window;
    dialog.lpstrFilter = L"Together Engine Material (*.twmat.json)\0*.twmat.json\0All files\0*.*\0\0";
    dialog.lpstrFile = path;
    dialog.nMaxFile = MAX_PATH;
    dialog.Flags = OFN_PATHMUSTEXIST | OFN_NOCHANGEDIR;
    if (!GetSaveFileNameW(&dialog)) return;
    if (!std::wstring_view(path).ends_with(L".twmat.json")) wcscat_s(path, L".twmat.json");
    if (std::filesystem::exists(path)) {
        append_console(LogKind::Warning, L"Choose a new file name; Save/Rename As preserves the prior material asset file.");
        return;
    }
    std::string name = std::filesystem::path(path).filename().string();
    constexpr std::string_view suffix = ".twmat.json";
    if (name.ends_with(suffix)) name.resize(name.size() - suffix.size());
    if (!name.empty()) material.name = name;
    std::string error;
    if (!material.save(path, error)) {
        append_console(LogKind::Error, L"Could not save material: " + to_wide(error));
        return;
    }
    g_app->pending_material_replacement_id = object->material_asset_id;
    g_app->asset_import_path = path;
    g_app->asset_import = g_app->assets->import_async(path);
    append_console(LogKind::Info, L"Saved material; registering its new stable content reference asynchronously.");
}

void remove_latest_material() {
    const std::string id = g_app->latest_material_asset_id;
    if (id.empty()) { append_console(LogKind::Warning, L"No material asset is selected."); return; }
    for (const SceneObject& object : g_app->scene.objects()) {
        if (object.material_asset_id == id) {
            append_console(LogKind::Error, L"Cannot remove a material while scene objects reference it. Clear those assignments first.");
            return;
        }
    }
    if (MessageBoxW(g_app->window, L"Remove this material from the current asset registry? Its source file will remain on disk.",
        L"Remove Material", MB_YESNO | MB_ICONWARNING) != IDYES) return;
    std::string persistence_error;
    if (!g_app->assets->remove_asset(id, &persistence_error)) {
        append_console(LogKind::Error, L"Material was already missing from the registry.");
        return;
    }
    if (!persistence_error.empty()) append_console(LogKind::Error, L"Material removed in memory, but registry update failed: " + to_wide(persistence_error));
    g_app->renderer.release_asset(id);
    g_app->latest_material_asset_id.clear();
    if (g_app->latest_asset_id == id) g_app->latest_asset_id.clear();
    append_console(LogKind::Info, L"Material removed from registry and cache; source file preserved.");
}

void remove_latest_asset() {
    if (asset_operation_busy()) { append_console(LogKind::Warning, L"Wait for the queued asset operation to finish before removing an asset."); return; }
    const std::string id = g_app->latest_asset_id;
    if (id.empty()) { append_console(LogKind::Warning, L"No imported asset is selected."); return; }
    for (const SceneObject& object : g_app->scene.objects()) {
        if (object.model_asset_id == id || object.material_asset_id == id || object.material.base_color_texture_id == id ||
            object.material.normal_texture_id == id || object.material.metallic_roughness_texture_id == id || object.material.occlusion_texture_id == id) {
            append_console(LogKind::Error, L"Cannot remove an asset while a scene object references it. Clear the reference first.");
            return;
        }
    }
    for (const auto& asset : g_app->assets->assets()) {
        if (asset.type != together::assets::AssetType::Material) continue;
        const auto payload = g_app->assets->payload(asset.handle);
        if (!payload || !std::holds_alternative<together::assets::MaterialDefinition>(*payload)) continue;
        const auto& material = std::get<together::assets::MaterialDefinition>(*payload);
        if (std::find(material.textures.begin(), material.textures.end(), id) != material.textures.end()) {
            append_console(LogKind::Error, L"Cannot remove a texture referenced by a registered material asset.");
            return;
        }
    }
    if (MessageBoxW(g_app->window, L"Remove the latest asset from this editor session? The source file on disk will be preserved.",
        L"Remove Asset", MB_YESNO | MB_ICONWARNING) != IDYES) return;
    std::string persistence_error;
    if (!g_app->assets->remove_asset(id, &persistence_error)) {
        append_console(LogKind::Error, L"Asset was already missing from the registry.");
        return;
    }
    if (!persistence_error.empty()) append_console(LogKind::Error, L"Asset removed in memory, but registry update failed: " + to_wide(persistence_error));
    g_app->renderer.release_asset(id);
    if (g_app->latest_material_asset_id == id) g_app->latest_material_asset_id.clear();
    if (g_app->latest_texture_asset_id == id) g_app->latest_texture_asset_id.clear();
    g_app->latest_asset_id.clear();
    append_console(LogKind::Info, L"Removed latest asset from registry/cache; source file preserved.");
}

void assign_latest_material() {
    SceneObject* object = selected_object();
    if (!object || g_app->latest_material_asset_id.empty()) {
        append_console(LogKind::Warning, L"Select an object and import or create a material first.");
        return;
    }
    together::assets::AssetMetadata metadata;
    if (!g_app->assets->find(g_app->latest_material_asset_id, metadata)) {
        append_console(LogKind::Error, L"The selected material reference is missing from the asset registry.");
        return;
    }
    const auto payload = g_app->assets->payload(metadata.handle);
    if (!payload || !std::holds_alternative<together::assets::MaterialDefinition>(*payload)) {
        append_console(LogKind::Error, L"The selected material asset is invalid.");
        return;
    }
    const auto& material = std::get<together::assets::MaterialDefinition>(*payload);
    object->material_asset_id = metadata.id;
    object->material.base_color = {material.base_color[0], material.base_color[1], material.base_color[2]};
    object->material.roughness = material.roughness;
    object->material.metallic = material.metallic;
    object->material.emission = {material.emission[0], material.emission[1], material.emission[2]};
    object->material.base_color_texture_id = material.textures[static_cast<std::size_t>(together::assets::TextureSlot::BaseColor)];
    object->material.normal_texture_id = material.textures[static_cast<std::size_t>(together::assets::TextureSlot::Normal)];
    const auto roughness_map = material.textures[static_cast<std::size_t>(together::assets::TextureSlot::Roughness)];
    const auto metallic_map = material.textures[static_cast<std::size_t>(together::assets::TextureSlot::Metallic)];
    object->material.metallic_roughness_texture_id = roughness_map.empty() ? metallic_map : roughness_map;
    object->material.occlusion_texture_id = material.textures[static_cast<std::size_t>(together::assets::TextureSlot::Occlusion)];
    set_dirty();
    refresh_inspector();
    sync_renderer_scene();
    append_console(LogKind::Info, L"Material assigned to selected object.");
}

void assign_latest_texture(const together::assets::TextureSlot slot = together::assets::TextureSlot::BaseColor) {
    SceneObject* object = selected_object();
    if (!object || g_app->latest_texture_asset_id.empty()) {
        append_console(LogKind::Warning, L"Select an object and import a texture first.");
        return;
    }
    const std::string shared_id = object->material_asset_id;
    const auto set_slot = [slot](SceneObject& target, const std::string& id) {
        if (slot == together::assets::TextureSlot::BaseColor) target.material.base_color_texture_id = id;
        else if (slot == together::assets::TextureSlot::Normal) target.material.normal_texture_id = id;
        else if (slot == together::assets::TextureSlot::Roughness || slot == together::assets::TextureSlot::Metallic)
            target.material.metallic_roughness_texture_id = id;
        else if (slot == together::assets::TextureSlot::Occlusion) target.material.occlusion_texture_id = id;
    };
    set_slot(*object, g_app->latest_texture_asset_id);
    if (!shared_id.empty()) {
        for (const SceneObject& candidate : g_app->scene.objects()) {
            if (candidate.id == object->id || candidate.material_asset_id != shared_id) continue;
            if (SceneObject* shared = g_app->scene.find_object(candidate.id))
                set_slot(*shared, g_app->latest_texture_asset_id);
        }
    }
    set_dirty();
    sync_renderer_scene();
    append_console(LogKind::Info, L"Base color texture assigned.");
}

void perform_browser_drop(const POINT screen_point) {
    if (!g_app || g_app->browser_drag_asset_id.empty()) return;
    RECT viewport_rect{};
    GetWindowRect(g_app->viewport, &viewport_rect);
    if (!PtInRect(&viewport_rect, screen_point)) return;
    if (g_app->playing) { append_console(LogKind::Warning, L"Stop the Play Preview before dropping assets into the scene."); return; }
    if (asset_operation_busy()) {
        append_console(LogKind::Warning, L"Wait for current asset/GPU work to finish before dropping another asset.");
        return;
    }
    together::assets::AssetMetadata metadata;
    if (!g_app->assets->find(g_app->browser_drag_asset_id, metadata)) {
        append_console(LogKind::Error, L"Drop rejected: asset reference is missing from the registry.");
        return;
    }
    if (metadata.type == together::assets::AssetType::Model) {
        SceneObject& object = g_app->scene.create_object(metadata.name);
        object.model_asset_id = metadata.id;
        const int viewport_width = (std::max)(1, viewport_rect.right - viewport_rect.left);
        const int viewport_height = (std::max)(1, viewport_rect.bottom - viewport_rect.top);
        object.transform.position.x = static_cast<float>(screen_point.x - (viewport_rect.left + viewport_width / 2)) * 0.025f;
        object.transform.position.z = static_cast<float>((viewport_rect.top + viewport_height / 2) - screen_point.y) * 0.025f;
        g_app->selected_id = object.id;
        set_dirty(); refresh_scene_ui();
        append_console(LogKind::Info, L"Model instance created from Asset Browser drop.");
        return;
    }
    if (metadata.type == together::assets::AssetType::Material) {
        if (!selected_object()) { append_console(LogKind::Warning, L"Select a scene object before dropping a material onto the viewport."); return; }
        g_app->latest_material_asset_id = metadata.id;
        assign_latest_material();
        return;
    }
    if (metadata.type == together::assets::AssetType::Texture) {
        if (!selected_object()) { append_console(LogKind::Warning, L"Select a scene object before dropping a texture into the viewport."); return; }
        g_app->latest_texture_asset_id = metadata.id;
        const auto slot = (GetKeyState(VK_MENU) & 0x8000) ? together::assets::TextureSlot::Occlusion :
            (GetKeyState(VK_CONTROL) & 0x8000) ? together::assets::TextureSlot::Roughness :
            (GetKeyState(VK_SHIFT) & 0x8000) ? together::assets::TextureSlot::Normal : together::assets::TextureSlot::BaseColor;
        assign_latest_texture(slot);
        return;
    }
    append_console(LogKind::Warning, L"Audio assets cannot be assigned to scene objects.");
}

LRESULT CALLBACK asset_list_proc(HWND window, UINT message, WPARAM wparam, LPARAM lparam) {
    if (!g_asset_list_original_proc) return DefWindowProcW(window, message, wparam, lparam);
    if (message == WM_LBUTTONDOWN) {
        const LRESULT result = CallWindowProcW(g_asset_list_original_proc, window, message, wparam, lparam);
        if (g_app) {
            const LRESULT row = SendMessageW(window, LB_GETCURSEL, 0, 0);
            if (row != LB_ERR) {
                const auto index = static_cast<std::size_t>(SendMessageW(window, LB_GETITEMDATA, static_cast<WPARAM>(row), 0));
                if (index < g_app->asset_browser_entries.size()) {
                    const auto& entry = g_app->asset_browser_entries[index];
                    g_app->browser_drag_asset_id = entry.asset_id;
                    g_app->browser_dragging = !entry.directory && !entry.asset_id.empty();
                    if (g_app->browser_dragging) {
                        together::assets::AssetMetadata metadata;
                        if (g_app->assets->find(entry.asset_id, metadata)) g_app->browser_drag_asset_type = metadata.type;
                        SetCapture(window);
                        SetWindowTextW(g_app->asset_info, L"Drag to viewport. Texture: Shift=Normal, Ctrl=Metal/Roughness, Alt=AO.");
                    }
                }
            }
        }
        return result;
    }
    if (message == WM_MOUSEMOVE && g_app && g_app->browser_dragging && (wparam & MK_LBUTTON)) {
        POINT screen{}; GetCursorPos(&screen);
        RECT viewport{}; GetWindowRect(g_app->viewport, &viewport);
        const bool inside = PtInRect(&viewport, screen) != FALSE;
        const bool accepts = g_app->browser_drag_asset_type == together::assets::AssetType::Model ||
            (selected_object() && (g_app->browser_drag_asset_type == together::assets::AssetType::Material ||
                g_app->browser_drag_asset_type == together::assets::AssetType::Texture));
        SetCursor(LoadCursorW(nullptr, inside && accepts ? IDC_HAND : IDC_NO));
        return 0;
    }
    if (message == WM_LBUTTONUP && g_app && g_app->browser_dragging) {
        POINT screen{}; GetCursorPos(&screen);
        g_app->browser_dragging = false;
        ReleaseCapture();
        perform_browser_drop(screen);
        g_app->browser_drag_asset_id.clear();
        return 0;
    }
    return CallWindowProcW(g_asset_list_original_proc, window, message, wparam, lparam);
}

HMENU create_menu_bar() {
    HMENU bar = CreateMenu();
    const auto add_menu = [bar](const wchar_t* name, const std::vector<std::pair<const wchar_t*, UINT>>& commands) {
        HMENU menu = CreatePopupMenu();
        for (const auto& [label, id] : commands) {
            if (id == 0) AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
            else AppendMenuW(menu, MF_STRING, id, label);
        }
        AppendMenuW(bar, MF_POPUP, reinterpret_cast<UINT_PTR>(menu), name);
    };
    add_menu(L"File", {{L"New Scene\tCtrl+N", MenuNew}, {L"Open...\tCtrl+O", MenuOpen},
        {L"Save\tCtrl+S", MenuSave}, {L"Save As...", MenuSaveAs}, {nullptr, 0}, {L"Exit", MenuExit}});
    add_menu(L"Edit", {{L"Delete Selected\tDelete", MenuDelete}, {L"Duplicate Selected\tCtrl+D", MenuDuplicate},
        {L"Reset Transform", MenuResetTransform}});
    add_menu(L"Scene", {{L"Validate Scene", MenuValidate}});
    add_menu(L"GameObject", {{L"Create Cube\tCtrl+Shift+N", MenuCreateCube}});
    add_menu(L"Assets", {{L"List Imported Assets in Console", 1081}, {L"Import Asset...", 1082},
        {L"Refresh Asset Browser", IdAssetRefresh},
        {L"Create Material...", 1083}, {L"Save/Rename Object Material As...", 1084},
        {L"Assign Latest Material", 1085}, {L"Assign Latest Texture as Base Color", 1086},
        {L"Assign Latest Texture as Normal Map", 1090}, {L"Assign Latest Texture as Metallic/Roughness", 1091},
        {L"Assign Latest Texture as Ambient Occlusion", 1092},
        {L"Remove Material Assignment", 1087}, {L"Remove Latest Material Asset...", 1088},
        {L"Remove Latest Imported Asset...", 1089},
        {nullptr, 0}, {L"Open Scene Folder", MenuOpenSceneFolder}});
    add_menu(L"Window", {{L"Reset Layout", MenuResetLayout}, {L"Focus Scene View", MenuFocusViewport}});
    add_menu(L"Help", {{L"About Together Engine Editor", MenuAbout}});
    return bar;
}

void create_ui(HWND window) {
    g_app->search = make_control(window, L"EDIT", L"", ES_AUTOHSCROLL | WS_TABSTOP, IdSearch, WS_EX_CLIENTEDGE);
    g_app->object_list = make_control(window, L"LISTBOX", L"", LBS_NOTIFY | LBS_NOINTEGRALHEIGHT | WS_VSCROLL | WS_TABSTOP, IdObjectList, WS_EX_CLIENTEDGE);
    make_control(window, L"BUTTON", L"+ Create Cube", BS_PUSHBUTTON | WS_TABSTOP, IdCreate);
    make_control(window, L"BUTTON", L"Delete", BS_PUSHBUTTON | WS_TABSTOP, IdDelete);
    g_app->asset_directory = make_control(window, L"EDIT", g_app->asset_browser_directory.wstring().c_str(), ES_AUTOHSCROLL | WS_TABSTOP, IdAssetDirectory, WS_EX_CLIENTEDGE);
    g_app->asset_go = make_control(window, L"BUTTON", L"Go", BS_PUSHBUTTON | WS_TABSTOP, IdAssetGo);
    g_app->asset_up = make_control(window, L"BUTTON", L"Up", BS_PUSHBUTTON | WS_TABSTOP, IdAssetUp);
    g_app->asset_search = make_control(window, L"EDIT", L"", ES_AUTOHSCROLL | WS_TABSTOP, IdAssetSearch, WS_EX_CLIENTEDGE);
    g_app->asset_filter = make_control(window, L"COMBOBOX", L"", CBS_DROPDOWNLIST | WS_VSCROLL | WS_TABSTOP, IdAssetFilter);
    for (const wchar_t* filter : {L"All Types", L"Models", L"Textures", L"Materials", L"Audio"})
        SendMessageW(g_app->asset_filter, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(filter));
    SendMessageW(g_app->asset_filter, CB_SETCURSEL, 0, 0);
    g_app->asset_list = make_control(window, L"LISTBOX", L"", LBS_NOTIFY | LBS_NOINTEGRALHEIGHT | WS_VSCROLL | WS_TABSTOP, IdAssetList, WS_EX_CLIENTEDGE);
    g_asset_list_original_proc = reinterpret_cast<WNDPROC>(SetWindowLongPtrW(g_app->asset_list, GWLP_WNDPROC,
        reinterpret_cast<LONG_PTR>(asset_list_proc)));
    g_app->asset_info = make_control(window, L"STATIC", L"Select an asset to inspect it.", SS_LEFT | SS_NOPREFIX, IdAssetInfo);
    g_app->asset_preview = make_control(window, L"STATIC", L"", SS_OWNERDRAW | WS_BORDER, IdAssetPreview, WS_EX_CLIENTEDGE);
    g_app->asset_import_button = make_control(window, L"BUTTON", L"Import...", BS_PUSHBUTTON | WS_TABSTOP, IdAssetImport);
    g_app->asset_refresh = make_control(window, L"BUTTON", L"Refresh", BS_PUSHBUTTON | WS_TABSTOP, IdAssetRefresh);
    g_app->viewport = make_control(window, kViewportClass, L"", WS_CLIPCHILDREN, IdViewport, WS_EX_CLIENTEDGE);

    const auto label = [window](const wchar_t* text) { return make_control(window, L"STATIC", text, SS_LEFT, 0); };
    g_app->inspector_labels[0] = label(L"POSITION");
    for (auto& field : g_app->position) field = make_control(window, L"EDIT", L"", ES_AUTOHSCROLL | WS_TABSTOP, IdPositionX + static_cast<int>(&field - g_app->position.data()), WS_EX_CLIENTEDGE);
    g_app->inspector_labels[1] = label(L"ROTATION");
    for (auto& field : g_app->rotation) field = make_control(window, L"EDIT", L"", ES_AUTOHSCROLL | WS_TABSTOP, IdRotationX + static_cast<int>(&field - g_app->rotation.data()), WS_EX_CLIENTEDGE);
    g_app->inspector_labels[2] = label(L"SCALE");
    for (auto& field : g_app->scale) field = make_control(window, L"EDIT", L"", ES_AUTOHSCROLL | WS_TABSTOP, IdScaleX + static_cast<int>(&field - g_app->scale.data()), WS_EX_CLIENTEDGE);
    g_app->inspector_labels[3] = label(L"BASE COLOR");
    for (auto& field : g_app->color) field = make_control(window, L"EDIT", L"", ES_AUTOHSCROLL | WS_TABSTOP, IdColorR + static_cast<int>(&field - g_app->color.data()), WS_EX_CLIENTEDGE);
    g_app->inspector_labels[4] = label(L"ROUGHNESS");
    g_app->roughness = make_control(window, L"EDIT", L"", ES_AUTOHSCROLL | WS_TABSTOP, IdRoughness, WS_EX_CLIENTEDGE);
    g_app->inspector_labels[5] = label(L"METALLIC");
    g_app->metallic = make_control(window, L"EDIT", L"", ES_AUTOHSCROLL | WS_TABSTOP, IdMetallic, WS_EX_CLIENTEDGE);
    g_app->inspector_labels[6] = label(L"EMISSION");
    for (std::size_t i = 0; i < 3; ++i)
        g_app->emission[i] = make_control(window, L"EDIT", L"", ES_AUTOHSCROLL | WS_TABSTOP,
            IdEmissionR + static_cast<int>(i), WS_EX_CLIENTEDGE);
    g_app->visible = make_control(window, L"BUTTON", L"Visible", BS_AUTOCHECKBOX | WS_TABSTOP, IdVisible);
    g_app->inspector_apply = make_control(window, L"BUTTON", L"Apply Properties", BS_PUSHBUTTON | WS_TABSTOP, IdApply);

    const std::array<const wchar_t*, 7> tool_names{L"Select", L"Move", L"Rotate", L"Scale", L"Play", L"Pause", L"Stop"};
    const std::array<int, 7> tool_ids{IdSelectTool, IdMoveTool, IdRotateTool, IdScaleTool, IdPlay, IdPause, IdStop};
    for (std::size_t i = 0; i < 4; ++i) g_app->tool_buttons[i] = make_control(window, L"BUTTON", tool_names[i], BS_PUSHBUTTON | WS_TABSTOP, tool_ids[i]);
    g_app->play_button = make_control(window, L"BUTTON", tool_names[4], BS_PUSHBUTTON | WS_TABSTOP, tool_ids[4]);
    g_app->pause_button = make_control(window, L"BUTTON", tool_names[5], BS_PUSHBUTTON | WS_TABSTOP, tool_ids[5]);
    g_app->stop_button = make_control(window, L"BUTTON", tool_names[6], BS_PUSHBUTTON | WS_TABSTOP, tool_ids[6]);
    g_app->console_title = make_control(window, L"STATIC", L"Console  |  Build output  |  Warnings  |  Errors", SS_LEFT, 0);
    g_app->console_filter = make_control(window, L"COMBOBOX", L"", CBS_DROPDOWNLIST | WS_VSCROLL | WS_TABSTOP, IdConsoleFilter);
    for (const wchar_t* filter : {L"All", L"Build", L"Warnings", L"Errors"}) SendMessageW(g_app->console_filter, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(filter));
    SendMessageW(g_app->console_filter, CB_SETCURSEL, 0, 0);
    g_app->build_button = make_control(window, L"BUTTON", L"Build Editor", BS_PUSHBUTTON | WS_TABSTOP, IdBuild);
    g_app->console = make_control(window, L"LISTBOX", L"", LBS_NOINTEGRALHEIGHT | WS_VSCROLL | WS_HSCROLL, IdConsole, WS_EX_CLIENTEDGE);
    for (HWND control : {g_app->search, g_app->object_list, g_app->asset_directory, g_app->asset_search, g_app->asset_list,
        g_app->asset_filter, g_app->viewport, g_app->visible, g_app->inspector_apply,
        g_app->roughness, g_app->metallic, g_app->console, g_app->console_filter, g_app->build_button}) {
        if (control) SetWindowTheme(control, L"DarkMode_Explorer", nullptr);
    }
    set_tool(TransformTool::Select);
    refresh_inspector();
    refresh_asset_browser();
}

void layout_controls(const int width, const int height) {
    if (!g_app || !g_app->viewport) return;
    const int console_top = (std::max)(kToolbarHeight + 100, height - kConsoleHeight);
    const int viewport_x = kLeftWidth;
    const int viewport_width = (std::max)(160, width - kLeftWidth - kRightWidth);
    const int inspector_x = width - kRightWidth;
    const int scene_height = (std::max)(80, console_top - kToolbarHeight);
    const auto place = [](HWND hwnd, int x, int y, int w, int h) {
        if (hwnd) MoveWindow(hwnd, x, y, (std::max)(0, w), (std::max)(0, h), TRUE);
    };

    for (std::size_t i = 0; i < 4; ++i) place(g_app->tool_buttons[i], 8 + static_cast<int>(i) * 74, kToolbarY, 68, kToolbarHeight);
    place(g_app->play_button, 318, kToolbarY, 68, kToolbarHeight);
    place(g_app->pause_button, 390, kToolbarY, 68, kToolbarHeight);
    place(g_app->stop_button, 462, kToolbarY, 68, kToolbarHeight);
    place(g_app->search, 8, 50, kLeftWidth - 16, 25);
    const int hierarchy_height = (std::max)(100, scene_height * 38 / 100);
    place(g_app->object_list, 8, 82, kLeftWidth - 16, hierarchy_height);
    place(GetDlgItem(g_app->window, IdCreate), 8, 86 + hierarchy_height, 122, 28);
    place(GetDlgItem(g_app->window, IdDelete), 134, 86 + hierarchy_height, 72, 28);
    const int browser_y = 120 + hierarchy_height;
    place(g_app->asset_directory, 8, browser_y + 16, kLeftWidth - 102, 24);
    place(g_app->asset_go, kLeftWidth - 91, browser_y + 16, 40, 24);
    place(g_app->asset_up, kLeftWidth - 47, browser_y + 16, 39, 24);
    place(g_app->asset_search, 8, browser_y + 45, 92, 24);
    place(g_app->asset_filter, 104, browser_y + 43, kLeftWidth - 112, 120);
    const int browser_footer = console_top - 8;
    place(g_app->asset_import_button, 8, browser_footer - 32, 92, 26);
    place(g_app->asset_refresh, 104, browser_footer - 32, kLeftWidth - 112, 26);
    place(g_app->asset_info, 8, browser_footer - 63, kLeftWidth - 16, 28);
    place(g_app->asset_preview, 8, browser_y + 74, kLeftWidth - 16,
        (std::max)(24, browser_footer - 63 - (browser_y + 74)));
    place(g_app->viewport, viewport_x, kToolbarHeight, viewport_width, scene_height);

    const int label_x = inspector_x + 12;
    const int field_x = inspector_x + 102;
    const int field_width = 58;
    constexpr std::array<int, 7> label_rows{73, 134, 195, 256, 317, 364, 411};
    for (std::size_t i = 0; i < g_app->inspector_labels.size(); ++i)
        place(g_app->inspector_labels[i], label_x, label_rows[i], 88, 22);
    place(g_app->console_title, 8, console_top + 5, 340, 22);
    const auto place_triple = [&](const std::array<HWND,3>& fields, int row_y) {
        for (int i = 0; i < 3; ++i) place(fields[static_cast<std::size_t>(i)], field_x + i * (field_width + 5), row_y, field_width, 25);
    };
    place_triple(g_app->position, 68);
    place_triple(g_app->rotation, 129);
    place_triple(g_app->scale, 190);
    place_triple(g_app->color, 251);
    place(g_app->roughness, field_x, 312, field_width, 25);
    place(g_app->metallic, field_x, 359, field_width, 25);
    place_triple(g_app->emission, 406);
    place(g_app->visible, label_x, 443, 100, 26);
    place(g_app->inspector_apply, label_x, 480, kRightWidth - 24, 30);

    place(g_app->console_filter, width - 230, console_top + 2, 110, 27);
    place(g_app->build_button, width - 112, console_top + 2, 104, 27);
    place(g_app->console, 8, console_top + 32, width - 16, height - console_top - 40);
}

void update_inspector_from_selection() {
    const LRESULT row = SendMessageW(g_app->object_list, LB_GETCURSEL, 0, 0);
    if (row == LB_ERR) g_app->selected_id = 0;
    else g_app->selected_id = static_cast<std::uint64_t>(SendMessageW(g_app->object_list, LB_GETITEMDATA, static_cast<WPARAM>(row), 0));
    refresh_hierarchy();
    refresh_inspector();
    sync_renderer_scene();
}

void start_or_stop_object_drag(const bool begin, const POINT point) {
    if (begin) {
        g_app->last_mouse = point;
        g_app->object_drag = true;
        SetCapture(g_app->viewport);
        return;
    }
    g_app->object_drag = false;
    if (!g_app->camera_drag) ReleaseCapture();
}

void viewport_mouse_move(const POINT point) {
    if (g_app->camera_drag) {
        const float dx = static_cast<float>(point.x - g_app->last_mouse.x);
        const float dy = static_cast<float>(point.y - g_app->last_mouse.y);
        g_app->renderer.rotate_camera(dx * 0.004f, -dy * 0.004f);
        g_app->last_mouse = point;
    } else if (g_app->object_drag && !g_app->playing) {
        SceneObject* object = selected_object();
        if (!object) return;
        const float dx = static_cast<float>(point.x - g_app->last_mouse.x);
        const float dy = static_cast<float>(point.y - g_app->last_mouse.y);
        if (g_app->tool == TransformTool::Move) {
            object->transform.position.x += dx * 0.018f;
            object->transform.position.z -= dy * 0.018f;
        } else if (g_app->tool == TransformTool::Rotate) {
            object->transform.rotation.y += dx * 0.5f;
            object->transform.rotation.x += dy * 0.5f;
        } else if (g_app->tool == TransformTool::Scale) {
            const float factor = std::clamp(1.0f + dx * 0.01f, 0.05f, 20.0f);
            object->transform.scale.x = std::clamp(object->transform.scale.x * factor, 0.05f, 100000.0f);
            object->transform.scale.y = std::clamp(object->transform.scale.y * factor, 0.05f, 100000.0f);
            object->transform.scale.z = std::clamp(object->transform.scale.z * factor, 0.05f, 100000.0f);
        }
        g_app->last_mouse = point;
        set_dirty();
        refresh_inspector();
        sync_renderer_scene();
    }
}

LRESULT CALLBACK viewport_proc(HWND window, UINT message, WPARAM wparam, LPARAM lparam) {
    switch (message) {
    case WM_DROPFILES: {
        HDROP drop = reinterpret_cast<HDROP>(wparam);
        wchar_t path[MAX_PATH]{};
        const UINT count = DragQueryFileW(drop, 0xFFFFFFFF, nullptr, 0);
        bool queued = false;
        for (UINT i = 0; i < count; ++i) {
            if (!DragQueryFileW(drop, i, path, MAX_PATH)) continue;
            const std::filesystem::path candidate(path);
            std::wstring extension = candidate.extension().wstring();
            std::transform(extension.begin(), extension.end(), extension.begin(),
                [](wchar_t ch) { return static_cast<wchar_t>(std::towlower(ch)); });
            if (extension != L".gltf" && extension != L".glb") continue;
            if (asset_operation_busy()) {
                append_console(LogKind::Warning, L"Drop ignored; another asset import is running.");
                break;
            }
            g_app->asset_import_path = candidate;
            g_app->asset_import = g_app->assets->import_async(candidate);
            append_console(LogKind::Info, L"Model import queued from viewport drop.");
            queued = true;
            break;
        }
        DragFinish(drop);
        if (!queued && count) append_console(LogKind::Warning, L"Drop a .gltf or .glb model onto the scene viewport.");
        return 0;
    }
    case WM_LBUTTONDOWN: {
        if (!g_app || g_app->playing) return 0;
        const POINT point{GET_X_LPARAM(lparam), GET_Y_LPARAM(lparam)};
        if (g_app->tool == TransformTool::Select) {
            const int index = g_app->renderer.pick_editor_object(static_cast<float>(point.x), static_cast<float>(point.y), g_app->render_objects);
            g_app->selected_id = index >= 0 && static_cast<std::size_t>(index) < g_app->scene.objects().size()
                ? g_app->scene.objects()[static_cast<std::size_t>(index)].id : 0;
            refresh_hierarchy(); refresh_inspector(); sync_renderer_scene();
        } else if (selected_object()) {
            start_or_stop_object_drag(true, point);
        }
        SetFocus(window);
        return 0;
    }
    case WM_LBUTTONUP:
        if (g_app) start_or_stop_object_drag(false, {});
        return 0;
    case WM_RBUTTONDOWN:
        if (g_app) {
            g_app->camera_drag = true;
            g_app->last_mouse = {GET_X_LPARAM(lparam), GET_Y_LPARAM(lparam)};
            SetCapture(window);
        }
        return 0;
    case WM_RBUTTONUP:
        if (g_app) {
            g_app->camera_drag = false;
            if (!g_app->object_drag) ReleaseCapture();
        }
        return 0;
    case WM_MOUSEMOVE:
        if (g_app) viewport_mouse_move({GET_X_LPARAM(lparam), GET_Y_LPARAM(lparam)});
        return 0;
    case WM_MOUSEWHEEL:
        if (g_app) g_app->renderer.zoom_camera(static_cast<float>(GET_WHEEL_DELTA_WPARAM(wparam)) / WHEEL_DELTA * 0.08f);
        return 0;
    case WM_CAPTURECHANGED:
        if (g_app) { g_app->camera_drag = false; g_app->object_drag = false; }
        return 0;
    case WM_ERASEBKGND:
        return 1;
    default:
        return DefWindowProcW(window, message, wparam, lparam);
    }
}

void handle_menu_command(const UINT id) {
    switch (id) {
    case MenuNew: new_scene(); break;
    case MenuOpen: open_scene(); break;
    case MenuSave: save_scene(); break;
    case MenuSaveAs: save_scene(true); break;
    case MenuExit: SendMessageW(g_app->window, WM_CLOSE, 0, 0); break;
    case MenuDelete: delete_selected(); break;
    case MenuDuplicate: duplicate_selected(); break;
    case MenuResetTransform: reset_transform(); break;
    case MenuValidate: validate_scene(); break;
    case MenuCreateCube: create_cube(); break;
    case MenuOpenSceneFolder: open_scene_folder(); break;
    case 1081: show_asset_list(); break;
    case 1082: import_asset(); break;
    case 1083: create_material_asset(); break;
    case 1084: save_selected_material_as(); break;
    case 1085: assign_latest_material(); break;
    case 1086: assign_latest_texture(); break;
    case 1090: assign_latest_texture(together::assets::TextureSlot::Normal); break;
    case 1091: assign_latest_texture(together::assets::TextureSlot::Roughness); break;
    case 1092: assign_latest_texture(together::assets::TextureSlot::Occlusion); break;
    case IdAssetRefresh: refresh_asset_browser(); break;
    case 1087:
        if (SceneObject* object = selected_object()) {
            object->material_asset_id.clear();
            object->material.base_color_texture_id.clear();
            set_dirty(); sync_renderer_scene(); refresh_inspector();
        }
        break;
    case 1088: remove_latest_material(); break;
    case 1089: remove_latest_asset(); break;
    case MenuResetLayout: { RECT r{}; GetClientRect(g_app->window, &r); layout_controls(r.right, r.bottom); break; }
    case MenuFocusViewport: SetFocus(g_app->viewport); break;
    case MenuAbout:
        MessageBoxW(g_app->window, L"Together Engine Editor 0.1\nA standalone scene editor using the Together Engine D3D11 renderer.",
            L"About", MB_OK | MB_ICONINFORMATION);
        break;
    }
}

void editor_tick() {
    if (!g_app) return;
    poll_build_process();
    if (g_app->asset_import.valid() && g_app->asset_import.wait_for(std::chrono::seconds(0)) == std::future_status::ready) {
        const auto result = g_app->asset_import.get();
        if (result) {
            if (!result.persistence_error.empty())
                append_console(LogKind::Error, L"Asset imported in memory, but registry persistence failed: " + to_wide(result.persistence_error));
            g_app->asset_browser_directory = result.metadata.source_path.parent_path();
            SetWindowTextW(g_app->asset_directory, g_app->asset_browser_directory.wstring().c_str());
            refresh_asset_browser();
            g_app->latest_asset_id = result.metadata.id;
            append_console(LogKind::Info, (result.duplicate ? L"Asset already imported: " : L"Asset imported: ") + to_wide(result.metadata.name));
            if (result.metadata.type == together::assets::AssetType::Texture) {
                append_console(LogKind::Info, L"Texture " + std::to_wstring(result.metadata.width) + L"x" + std::to_wstring(result.metadata.height) + L", " + to_wide(result.metadata.format));
                const auto payload = result.payload;
                const std::string id = result.metadata.id;
                D3D11Renderer* renderer = &g_app->renderer;
                g_app->pending_gpu_type = PendingGpuAsset::Texture;
                g_app->pending_gpu_asset_id = id;
                g_app->pending_gpu_asset_name = result.metadata.name;
                try {
                    g_app->gpu_asset_upload = std::async(std::launch::async, [renderer, id, payload] {
                        try { return upload_texture_asset(*renderer, id, payload); }
                        catch (...) { return false; }
                    });
                    append_console(LogKind::Info, L"Texture GPU upload queued; mip levels are prepared on the asset worker.");
                } catch (const std::exception& exception) {
                    g_app->pending_gpu_type = PendingGpuAsset::None;
                    append_console(LogKind::Error, L"Could not start texture upload worker: " + to_wide(exception.what()));
                }
            } else if (result.metadata.type == together::assets::AssetType::Model) {
                append_console(LogKind::Info, L"Static model validated: " + std::to_wstring(result.metadata.mesh_count) + L" mesh primitives.");
                const auto payload = result.payload;
                const std::string id = result.metadata.id;
                D3D11Renderer* renderer = &g_app->renderer;
                g_app->pending_gpu_type = PendingGpuAsset::Model;
                g_app->pending_gpu_asset_id = id;
                g_app->pending_gpu_asset_name = result.metadata.name;
                try {
                    g_app->gpu_asset_upload = std::async(std::launch::async, [renderer, id, payload] {
                        try { return upload_model_asset(*renderer, id, payload); }
                        catch (...) { return false; }
                    });
                    append_console(LogKind::Info, L"Static model GPU buffer upload queued.");
                } catch (const std::exception& exception) {
                    g_app->pending_gpu_type = PendingGpuAsset::None;
                    append_console(LogKind::Error, L"Could not start model upload worker: " + to_wide(exception.what()));
                }
            } else if (result.metadata.type == together::assets::AssetType::Material) {
                if (!g_app->pending_material_replacement_id.empty()) {
                    for (const SceneObject& candidate : g_app->scene.objects()) {
                        if (candidate.material_asset_id == g_app->pending_material_replacement_id) {
                            if (SceneObject* object = g_app->scene.find_object(candidate.id)) object->material_asset_id = result.metadata.id;
                        }
                    }
                    g_app->pending_material_replacement_id.clear();
                    set_dirty();
                    sync_renderer_scene();
                }
                g_app->latest_material_asset_id = result.metadata.id;
                append_console(LogKind::Info, L"Material registered; use Assets > Assign Latest Material on a selected object.");
            } else if (result.metadata.type == together::assets::AssetType::Audio) {
                const auto& audio = std::get<together::assets::AudioData>(*result.payload);
                append_console(LogKind::Info, L"WAV: " + std::to_wstring(audio.channels) + L" channels, " +
                    std::to_wstring(audio.sample_rate) + L" Hz, " + std::to_wstring(audio.frame_count) + L" frames (playback unavailable).");
            }
        } else append_console(LogKind::Error, L"Asset import failed: " + to_wide(result.error));
    }
    if (g_app->gpu_asset_upload.valid() && g_app->gpu_asset_upload.wait_for(std::chrono::seconds(0)) == std::future_status::ready) {
        const bool uploaded = g_app->gpu_asset_upload.get();
        const PendingGpuAsset type = g_app->pending_gpu_type;
        const std::string id = std::move(g_app->pending_gpu_asset_id);
        const std::string name = std::move(g_app->pending_gpu_asset_name);
        g_app->pending_gpu_type = PendingGpuAsset::None;
        if (!uploaded) append_console(LogKind::Error, L"D3D11 asset upload failed for " + to_wide(name) + L".");
        else if (type == PendingGpuAsset::Texture) {
            g_app->latest_texture_asset_id = id;
            append_console(LogKind::Info, L"RGBA8 base-color texture and mip chain uploaded to D3D11.");
        } else if (type == PendingGpuAsset::Model) {
            SceneObject* restored_instance = nullptr;
            for (const auto& candidate : g_app->scene.objects())
                if (candidate.model_asset_id == id) { restored_instance = g_app->scene.find_object(candidate.id); break; }
            if (!restored_instance) {
                restored_instance = &g_app->scene.create_object(name);
                restored_instance->model_asset_id = id;
                set_dirty();
            }
            g_app->selected_id = restored_instance->id;
            refresh_hierarchy(); refresh_inspector(); sync_renderer_scene();
            append_console(LogKind::Info, L"Static model buffers are ready and the scene instance is visible.");
        }
    }
    if (g_app->minimized) return;
    const auto frame = g_app->timer.tick();
    const float delta = static_cast<float>(frame.delta_seconds);
    if (GetForegroundWindow() == g_app->window && GetFocus() == g_app->viewport && !g_app->playing) {
        const float forward = static_cast<float>((GetAsyncKeyState('W') & 0x8000) != 0) - static_cast<float>((GetAsyncKeyState('S') & 0x8000) != 0);
        const float right = static_cast<float>((GetAsyncKeyState('D') & 0x8000) != 0) - static_cast<float>((GetAsyncKeyState('A') & 0x8000) != 0);
        const float up = static_cast<float>((GetAsyncKeyState(VK_SPACE) & 0x8000) != 0) - static_cast<float>((GetAsyncKeyState(VK_CONTROL) & 0x8000) != 0);
        g_app->renderer.move_camera(forward, right, up, delta);
    }
    g_app->elapsed_seconds += frame.delta_seconds;
    if (g_app->playing && !g_app->paused) {
        if (SceneObject* object = selected_object()) {
            object->transform.rotation.y += delta * 32.0f;
            sync_renderer_scene();
        }
    }
    g_app->renderer.render(static_cast<float>(g_app->elapsed_seconds), delta);
    g_app->fps_elapsed += frame.delta_seconds;
    ++g_app->fps_frames;
    if (g_app->fps_elapsed >= 0.5) {
        const double fps = g_app->fps_frames / g_app->fps_elapsed;
        const auto stats = g_app->renderer.stats();
        std::wostringstream title;
        title << L"Together Engine Editor 0.1 — " << to_wide(g_app->scene.name())
              << (g_app->dirty ? L" *" : L"") << std::fixed << L" | " << fps << L" FPS | "
              << stats.draw_calls << L" draw calls";
        if (g_app->playing) title << (g_app->paused ? L" | Paused" : L" | Play Preview");
        SetWindowTextW(g_app->window, title.str().c_str());
        g_app->fps_elapsed = 0;
        g_app->fps_frames = 0;
    }
}

LRESULT CALLBACK main_proc(HWND window, UINT message, WPARAM wparam, LPARAM lparam) {
    switch (message) {
    case WM_CREATE: {
        g_app->window = window;
        g_app->asset_browser_directory = g_app->root_path;
        SetMenu(window, create_menu_bar());
        create_ui(window);
        g_app->assets = std::make_unique<together::assets::AssetPipeline>(g_app->root_path);
        refresh_asset_browser();
        DragAcceptFiles(g_app->viewport, TRUE);
        RECT rect{};
        GetClientRect(window, &rect);
        layout_controls(rect.right, rect.bottom);
        RECT viewport_rect{};
        GetClientRect(g_app->viewport, &viewport_rect);
        if (!g_app->renderer.initialize(g_app->viewport, static_cast<unsigned>(viewport_rect.right), static_cast<unsigned>(viewport_rect.bottom))) {
            MessageBoxW(window, L"D3D11 scene view could not be initialized. See TogetherWeDo.log for details.", L"Editor Startup Error", MB_OK | MB_ICONERROR);
            return -1;
        }
        for (const auto& persistence_error : g_app->assets->persistence_errors())
            append_console(LogKind::Warning, L"Asset registry: " + to_wide(persistence_error));
        for (const auto& asset : g_app->assets->assets()) {
            const auto payload = g_app->assets->payload(asset.handle);
            bool uploaded = true;
            if (asset.type == together::assets::AssetType::Texture)
                uploaded = upload_texture_asset(g_app->renderer, asset.id, payload);
            else if (asset.type == together::assets::AssetType::Model)
                uploaded = upload_model_asset(g_app->renderer, asset.id, payload);
            if (!uploaded) append_console(LogKind::Error, L"Could not restore GPU resource for " + to_wide(asset.name));
        }
        g_app->scene.create_object();
        g_app->scene.create_object("Second Cube").transform.position = {2.0f, 0.5f, 0.0f};
        g_app->scene.create_object("Metal Cube").transform.position = {-2.0f, 0.5f, 1.0f};
        g_app->scene.find_object(3)->material.metallic = 0.8f;
        g_app->selected_id = 1;
        g_app->dirty = false;
        refresh_scene_ui();
        SetTimer(window, kEditorTimer, 16, nullptr);
        append_console(LogKind::Info, L"Editor ready. Right-drag the viewport to look; WASD moves the camera.");
        return 0;
    }
    case WM_SIZE:
        if (g_app && g_app->viewport) {
            g_app->minimized = (wparam == SIZE_MINIMIZED);
            layout_controls(LOWORD(lparam), HIWORD(lparam));
            RECT rect{};
            GetClientRect(g_app->viewport, &rect);
            g_app->renderer.resize(static_cast<unsigned>(rect.right), static_cast<unsigned>(rect.bottom));
        }
        return 0;
    case WM_COMMAND: {
        const int id = LOWORD(wparam);
        const int notification = HIWORD(wparam);
        if (id >= MenuNew && id <= MenuAbout) { handle_menu_command(static_cast<UINT>(id)); return 0; }
        switch (id) {
        case IdSearch:
            if (notification == EN_CHANGE) refresh_hierarchy();
            return 0;
        case IdAssetSearch:
            if (notification == EN_CHANGE) refresh_asset_browser();
            return 0;
        case IdAssetFilter:
            if (notification == CBN_SELCHANGE) refresh_asset_browser();
            return 0;
        case IdAssetGo:
            refresh_asset_browser();
            return 0;
        case IdAssetUp:
            if (!g_app->asset_browser_directory.parent_path().empty()) {
                g_app->asset_browser_directory = g_app->asset_browser_directory.parent_path();
                SetWindowTextW(g_app->asset_directory, g_app->asset_browser_directory.wstring().c_str());
                refresh_asset_browser();
            }
            return 0;
        case IdAssetRefresh: refresh_asset_browser(); return 0;
        case IdAssetImport: import_asset(); return 0;
        case IdAssetList:
            if (notification == LBN_SELCHANGE) select_asset_browser_entry(false);
            else if (notification == LBN_DBLCLK) select_asset_browser_entry(true);
            return 0;
        case IdObjectList:
            if (notification == LBN_SELCHANGE && !g_app->updating_list) update_inspector_from_selection();
            return 0;
        case IdCreate: create_cube(); return 0;
        case IdDelete: delete_selected(); return 0;
        case IdApply: apply_inspector(); return 0;
        case IdVisible:
            if (notification == BN_CLICKED && selected_object() && !g_app->playing) {
                selected_object()->visible = SendMessageW(g_app->visible, BM_GETCHECK, 0, 0) == BST_CHECKED;
                set_dirty(); refresh_hierarchy(); sync_renderer_scene();
            }
            return 0;
        case IdSelectTool: set_tool(TransformTool::Select); return 0;
        case IdMoveTool: set_tool(TransformTool::Move); return 0;
        case IdRotateTool: set_tool(TransformTool::Rotate); return 0;
        case IdScaleTool: set_tool(TransformTool::Scale); return 0;
        case IdPlay: start_play_preview(); return 0;
        case IdPause: pause_preview(); return 0;
        case IdStop: stop_preview(); return 0;
        case IdBuild: start_build(); return 0;
        case IdConsoleFilter:
            if (notification == CBN_SELCHANGE) refresh_console_filter();
            return 0;
        }
        break;
    }
    case WM_TIMER:
        if (wparam == kEditorTimer) editor_tick();
        return 0;
    case WM_KEYDOWN:
        if (GetFocus() == g_app->asset_directory && wparam == VK_RETURN) refresh_asset_browser();
        else if (wparam == VK_DELETE) delete_selected();
        else if ((GetKeyState(VK_CONTROL) & 0x8000) && (GetKeyState(VK_SHIFT) & 0x8000) && wparam == 'N') create_cube();
        else if ((GetKeyState(VK_CONTROL) & 0x8000) && wparam == 'S') save_scene();
        else if ((GetKeyState(VK_CONTROL) & 0x8000) && wparam == 'N') new_scene();
        else if ((GetKeyState(VK_CONTROL) & 0x8000) && wparam == 'O') open_scene();
        else if ((GetKeyState(VK_CONTROL) & 0x8000) && wparam == 'D') duplicate_selected();
        return 0;
    case WM_CTLCOLORSTATIC: {
        HDC dc = reinterpret_cast<HDC>(wparam);
        SetTextColor(dc, RGB(211, 216, 224));
        SetBkColor(dc, RGB(31, 34, 40));
        return reinterpret_cast<LRESULT>(g_app->panel_brush);
    }
    case WM_CTLCOLOREDIT: case WM_CTLCOLORLISTBOX: {
        HDC dc = reinterpret_cast<HDC>(wparam);
        SetTextColor(dc, RGB(231, 235, 241));
        SetBkColor(dc, RGB(43, 47, 54));
        return reinterpret_cast<LRESULT>(message == WM_CTLCOLORLISTBOX ? g_app->console_brush : g_app->edit_brush);
    }
    case WM_CTLCOLORBTN: {
        HDC dc = reinterpret_cast<HDC>(wparam);
        SetTextColor(dc, RGB(230, 235, 242));
        SetBkColor(dc, RGB(49, 54, 62));
        return reinterpret_cast<LRESULT>(g_app->panel_brush);
    }
    case WM_DRAWITEM:
        if (wparam == IdAssetPreview && lparam) {
            paint_asset_preview(*reinterpret_cast<const DRAWITEMSTRUCT*>(lparam));
            return TRUE;
        }
        break;
    case WM_PAINT: {
        PAINTSTRUCT paint{};
        HDC dc = BeginPaint(window, &paint);
        RECT client{};
        GetClientRect(window, &client);
        FillRect(dc, &client, g_app->panel_brush);
        RECT toolbar{0, 0, client.right, kToolbarHeight};
        FillRect(dc, &toolbar, g_app->edit_brush);
        RECT left{0, kToolbarHeight, kLeftWidth, client.bottom - kConsoleHeight};
        FillRect(dc, &left, g_app->panel_brush);
        RECT right{client.right - kRightWidth, kToolbarHeight, client.right, client.bottom - kConsoleHeight};
        FillRect(dc, &right, g_app->panel_brush);
        RECT console{0, client.bottom - kConsoleHeight, client.right, client.bottom};
        FillRect(dc, &console, g_app->console_brush);
        SetBkMode(dc, TRANSPARENT);
        SetTextColor(dc, RGB(170, 185, 204));
        const int console_top = (std::max)(kToolbarHeight + 100, static_cast<int>(client.bottom) - kConsoleHeight);
        const int scene_height = (std::max)(80, console_top - kToolbarHeight);
        const int hierarchy_height = (std::max)(100, scene_height * 38 / 100);
        TextOutW(dc, 8, 37, L"SCENE HIERARCHY", 15);
        TextOutW(dc, 8, 102 + hierarchy_height, L"ASSET BROWSER", 13);
        EndPaint(window, &paint);
        return 0;
    }
    case WM_CLOSE:
        if (confirm_discard_changes()) DestroyWindow(window);
        return 0;
    case WM_DESTROY:
        KillTimer(window, kEditorTimer);
        if (g_app->build_process && WaitForSingleObject(g_app->build_process, 0) != WAIT_OBJECT_0)
            TerminateProcess(g_app->build_process, 1);
        if (g_app->build_process) CloseHandle(g_app->build_process);
        if (g_app->build_thread) CloseHandle(g_app->build_thread);
        if (g_app->build_output) CloseHandle(g_app->build_output);
        if (g_app->panel_brush) DeleteObject(g_app->panel_brush);
        if (g_app->edit_brush) DeleteObject(g_app->edit_brush);
        if (g_app->console_brush) DeleteObject(g_app->console_brush);
        PostQuitMessage(0);
        return 0;
    default:
        return DefWindowProcW(window, message, wparam, lparam);
    }
    return DefWindowProcW(window, message, wparam, lparam);
}

} // namespace

int WINAPI wWinMain(HINSTANCE instance, HINSTANCE, PWSTR, int show_command) {
    g_app = std::make_unique<EditorState>();
    g_app->panel_brush = CreateSolidBrush(RGB(31, 34, 40));
    g_app->edit_brush = CreateSolidBrush(RGB(43, 47, 54));
    g_app->console_brush = CreateSolidBrush(RGB(24, 27, 32));
    together::core::Logger::instance().set_file(g_app->root_path / L"TogetherWeDoEditor.log");

    WNDCLASSEXW viewport_class{sizeof(WNDCLASSEXW)};
    viewport_class.lpfnWndProc = viewport_proc;
    viewport_class.hInstance = instance;
    viewport_class.hCursor = LoadCursorW(nullptr, IDC_CROSS);
    viewport_class.lpszClassName = kViewportClass;
    if (!RegisterClassExW(&viewport_class)) {
        MessageBoxW(nullptr, L"Could not register the editor viewport window class.", L"Editor Startup Error", MB_OK | MB_ICONERROR);
        return 1;
    }

    WNDCLASSEXW main_class{sizeof(WNDCLASSEXW)};
    main_class.style = CS_HREDRAW | CS_VREDRAW;
    main_class.lpfnWndProc = main_proc;
    main_class.hInstance = instance;
    main_class.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    main_class.hbrBackground = g_app->panel_brush;
    main_class.lpszClassName = kMainClass;
    if (!RegisterClassExW(&main_class)) {
        MessageBoxW(nullptr, L"Could not register the editor window class.", L"Editor Startup Error", MB_OK | MB_ICONERROR);
        UnregisterClassW(kViewportClass, instance);
        return 1;
    }
    RECT bounds{0, 0, 1440, 900};
    AdjustWindowRect(&bounds, WS_OVERLAPPEDWINDOW, TRUE);
    HWND window = CreateWindowExW(0, kMainClass, L"Together Engine Editor 0.1",
        WS_OVERLAPPEDWINDOW | WS_CLIPCHILDREN, CW_USEDEFAULT, CW_USEDEFAULT,
        bounds.right - bounds.left, bounds.bottom - bounds.top,
        nullptr, nullptr, instance, nullptr);
    if (!window) {
        MessageBoxW(nullptr, L"Could not create the editor window.", L"Editor Startup Error", MB_OK | MB_ICONERROR);
        UnregisterClassW(kMainClass, instance);
        UnregisterClassW(kViewportClass, instance);
        return 1;
    }
    ShowWindow(window, show_command);
    UpdateWindow(window);
    constexpr std::array<ACCEL, 6> accelerators{{
        {FVIRTKEY | FCONTROL, 'N', MenuNew},
        {FVIRTKEY | FCONTROL, 'O', MenuOpen},
        {FVIRTKEY | FCONTROL, 'S', MenuSave},
        {FVIRTKEY | FCONTROL | FSHIFT, 'N', MenuCreateCube},
        {FVIRTKEY | FCONTROL, 'D', MenuDuplicate},
        {FVIRTKEY, VK_DELETE, MenuDelete},
    }};
    HACCEL accelerator_table = CreateAcceleratorTableW(const_cast<LPACCEL>(accelerators.data()), static_cast<int>(accelerators.size()));
    MSG message{};
    while (GetMessageW(&message, nullptr, 0, 0) > 0) {
        if (accelerator_table && TranslateAcceleratorW(window, accelerator_table, &message)) continue;
        TranslateMessage(&message);
        DispatchMessageW(&message);
    }
    if (accelerator_table) DestroyAcceleratorTable(accelerator_table);
    UnregisterClassW(kMainClass, instance);
    UnregisterClassW(kViewportClass, instance);
    return static_cast<int>(message.wParam);
}
