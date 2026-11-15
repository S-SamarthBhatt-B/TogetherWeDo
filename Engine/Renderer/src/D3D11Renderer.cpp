#include "TogetherEngine/Renderer/D3D11Renderer.h"

#include "TogetherEngine/Core/Logger.h"

#include <d3d11.h>
#include <d3dcompiler.h>
#include <DirectXMath.h>
#include <wrl/client.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <functional>
#include <iterator>
#include <limits>
#include <mutex>
#include <span>
#include <shared_mutex>
#include <sstream>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <vector>

using Microsoft::WRL::ComPtr;

namespace together::renderer {
namespace {

struct Vertex {
    DirectX::XMFLOAT3 position;
    DirectX::XMFLOAT3 normal;
    DirectX::XMFLOAT2 uv;
};

struct alignas(16) SceneConstants {
    DirectX::XMFLOAT4X4 world_view_projection;
    DirectX::XMFLOAT4X4 world;
    DirectX::XMFLOAT4X4 normal_transform;
    DirectX::XMFLOAT4 camera_position;
    DirectX::XMFLOAT4 base_color;
    DirectX::XMFLOAT4 emission;
    DirectX::XMFLOAT4 material; // roughness, metallic, textured, padding
    DirectX::XMFLOAT4 light_direction;
    DirectX::XMFLOAT4 fog_color_density;
};
static_assert(sizeof(SceneConstants) % 16 == 0);

constexpr char kSceneShader[] = R"(
cbuffer SceneConstants : register(b0) {
    row_major float4x4 worldViewProjection;
    row_major float4x4 world;
    row_major float4x4 normalTransform;
    float4 cameraPosition;
    float4 baseColor;
    float4 emission;
    float4 material;
    float4 lightDirection;
    float4 fogColorDensity;
};
Texture2D surfaceTexture : register(t0);
SamplerState surfaceSampler : register(s0);
struct VSInput { float3 position : POSITION; float3 normal : NORMAL; float2 uv : TEXCOORD0; };
struct VSOutput { float4 position : SV_POSITION; float3 worldPosition : POSITION0; float3 normal : NORMAL; float2 uv : TEXCOORD0; };
VSOutput VSMain(VSInput input) {
    VSOutput output;
    const float4 worldPosition = mul(float4(input.position, 1.0), world);
    output.position = mul(float4(input.position, 1.0), worldViewProjection);
    output.worldPosition = worldPosition.xyz;
    output.normal = normalize(mul(float4(input.normal, 0.0), normalTransform).xyz);
    output.uv = input.uv;
    return output;
}
float DistributionGGX(float nDotH, float roughness) {
    const float a = roughness * roughness;
    const float a2 = a * a;
    const float denominator = nDotH * nDotH * (a2 - 1.0) + 1.0;
    return a2 / max(3.14159265 * denominator * denominator, 0.0001);
}
float GeometrySchlick(float nDotV, float roughness) {
    const float k = (roughness + 1.0) * (roughness + 1.0) / 8.0;
    return nDotV / max(nDotV * (1.0 - k) + k, 0.0001);
}
float3 FresnelSchlick(float cosTheta, float3 f0) {
    return f0 + (1.0 - f0) * pow(1.0 - saturate(cosTheta), 5.0);
}
float4 PSMain(VSOutput input) : SV_TARGET {
    float3 albedo = baseColor.rgb;
    if (material.z > 0.5) albedo *= surfaceTexture.Sample(surfaceSampler, input.uv).rgb;
    const float roughness = clamp(material.x, 0.08, 1.0);
    const float metallic = saturate(material.y);
    const float3 n = normalize(input.normal);
    const float3 l = normalize(-lightDirection.xyz);
    const float3 v = normalize(cameraPosition.xyz - input.worldPosition);
    const float3 h = normalize(v + l);
    const float nDotL = saturate(dot(n, l));
    const float nDotV = max(saturate(dot(n, v)), 0.001);
    const float3 f0 = lerp(float3(0.04, 0.04, 0.04), albedo, metallic);
    const float3 f = FresnelSchlick(saturate(dot(h, v)), f0);
    const float d = DistributionGGX(saturate(dot(n, h)), roughness);
    const float g = GeometrySchlick(nDotV, roughness) * GeometrySchlick(nDotL, roughness);
    const float3 specular = (d * g * f) / max(4.0 * nDotV * nDotL, 0.001);
    const float3 diffuse = (1.0 - f) * (1.0 - metallic) * albedo / 3.14159265;
    const float3 direct = (diffuse + specular) * nDotL * float3(3.0, 2.8, 2.5);
    const float3 ambient = albedo * float3(0.14, 0.17, 0.13) * (1.0 - metallic);
    const float3 lit = direct + ambient + emission.rgb;
    const float distanceToCamera = distance(cameraPosition.xyz, input.worldPosition);
    const float fog = saturate(1.0 - exp(-fogColorDensity.a * distanceToCamera));
    return float4(lerp(lit, fogColorDensity.rgb, fog), 1.0);
}
)";

constexpr char kSkyShader[] = R"(
struct SkyOutput { float4 position : SV_POSITION; float2 uv : TEXCOORD0; };
SkyOutput VSMain(uint vertexId : SV_VertexID) {
    SkyOutput output;
    const float2 positions[3] = { float2(-1.0, -1.0), float2(-1.0, 3.0), float2(3.0, -1.0) };
    output.position = float4(positions[vertexId], 1.0, 1.0);
    output.uv = positions[vertexId] * float2(0.5, -0.5) + 0.5;
    return output;
}
float4 PSMain(SkyOutput input) : SV_TARGET {
    const float horizon = saturate(input.uv.y);
    const float3 zenith = float3(0.10, 0.20, 0.29);
    const float3 nearHorizon = float3(0.61, 0.64, 0.52);
    const float3 sky = lerp(zenith, nearHorizon, pow(horizon, 1.4));
    const float2 sunPosition = float2(0.73, 0.22);
    const float sunEdge = saturate((distance(input.uv, sunPosition) - 0.025) / 0.030);
    const float sun = (1.0 - sunEdge) * (1.0 - sunEdge) * (1.0 + 2.0 * sunEdge);
    return float4(lerp(sky, float3(1.0, 0.78, 0.48), sun * 0.8), 1.0);
}
)";

void log_hresult(std::string_view operation, const HRESULT result) {
    std::ostringstream stream;
    stream << operation << " failed (HRESULT 0x" << std::hex << std::uppercase
           << static_cast<unsigned long>(result) << ").";
    core::Logger::instance().write(core::LogLevel::Error, stream.str());
}

bool compile_shader(const char* source, const char* entry, const char* target, ComPtr<ID3DBlob>& bytecode) {
    ComPtr<ID3DBlob> errors;
    const HRESULT result = D3DCompile(source, std::strlen(source), "TogetherEngine", nullptr, nullptr,
        entry, target, D3DCOMPILE_ENABLE_STRICTNESS, 0, &bytecode, &errors);
    if (errors && errors->GetBufferPointer()) {
        core::Logger::instance().write(FAILED(result) ? core::LogLevel::Error : core::LogLevel::Warning,
            std::string_view(static_cast<const char*>(errors->GetBufferPointer()), errors->GetBufferSize()));
    }
    if (FAILED(result)) log_hresult(std::string("D3DCompile ") + entry, result);
    return SUCCEEDED(result);
}

void append_triangle(std::vector<Vertex>& output, DirectX::XMFLOAT3 a, DirectX::XMFLOAT3 b,
    DirectX::XMFLOAT3 c, DirectX::XMFLOAT2 uv_a = {0, 0}, DirectX::XMFLOAT2 uv_b = {1, 0},
    DirectX::XMFLOAT2 uv_c = {0.5f, 1}) {
    const DirectX::XMVECTOR va = DirectX::XMLoadFloat3(&a);
    const DirectX::XMVECTOR vb = DirectX::XMLoadFloat3(&b);
    const DirectX::XMVECTOR vc = DirectX::XMLoadFloat3(&c);
    DirectX::XMFLOAT3 normal{};
    DirectX::XMStoreFloat3(&normal, DirectX::XMVector3Normalize(
        DirectX::XMVector3Cross(DirectX::XMVectorSubtract(vb, va), DirectX::XMVectorSubtract(vc, va))));
    output.push_back({a, normal, uv_a});
    output.push_back({b, normal, uv_b});
    output.push_back({c, normal, uv_c});
}

struct MeshRange { UINT start{}; UINT count{}; };

struct TransparentStringHash {
    using is_transparent = void;
    std::size_t operator()(std::string_view value) const noexcept { return std::hash<std::string_view>{}(value); }
};

struct DrawObject {
    MeshRange mesh;
    DirectX::XMMATRIX world;
    std::string_view texture;
    DirectX::XMFLOAT4 color;
    float roughness;
    float metallic;
    std::string_view model_asset_id{};
    bool material_override{};
    DirectX::XMFLOAT3 emission{};
};

struct GpuModelPrimitive {
    ComPtr<ID3D11Buffer> vertices;
    ComPtr<ID3D11Buffer> indices;
    UINT index_count{};
    DirectX::XMFLOAT4 base_color{1,1,1,1};
    DirectX::XMFLOAT3 emission{};
    DirectX::XMFLOAT4X4 local_transform{1,0,0,0, 0,1,0,0, 0,0,1,0, 0,0,0,1};
    float roughness{1.0f};
    float metallic{};
};

MeshRange append_cube(std::vector<Vertex>& vertices) {
    const UINT start = static_cast<UINT>(vertices.size());
    constexpr float n = -0.5f;
    constexpr float p = 0.5f;
    const auto quad = [&vertices](DirectX::XMFLOAT3 a, DirectX::XMFLOAT3 b,
        DirectX::XMFLOAT3 c, DirectX::XMFLOAT3 d, DirectX::XMFLOAT3 normal) {
        const std::array<DirectX::XMFLOAT2, 4> uv{{{0, 0}, {1, 0}, {1, 1}, {0, 1}}};
        const std::array<DirectX::XMFLOAT3, 4> points{a, b, c, d};
        for (const unsigned index : {0u, 1u, 2u, 0u, 2u, 3u})
            vertices.push_back({points[index], normal, uv[index]});
    };
    quad({n,n,n}, {p,n,n}, {p,p,n}, {n,p,n}, {0,0,-1});
    quad({p,n,p}, {n,n,p}, {n,p,p}, {p,p,p}, {0,0,1});
    quad({n,n,p}, {n,n,n}, {n,p,n}, {n,p,p}, {-1,0,0});
    quad({p,n,n}, {p,n,p}, {p,p,p}, {p,p,n}, {1,0,0});
    quad({n,p,n}, {p,p,n}, {p,p,p}, {n,p,p}, {0,1,0});
    quad({n,n,p}, {p,n,p}, {p,n,n}, {n,n,n}, {0,-1,0});
    return {start, static_cast<UINT>(vertices.size()) - start};
}

MeshRange append_ground(std::vector<Vertex>& vertices) {
    const UINT start = static_cast<UINT>(vertices.size());
    constexpr float extent = 55.0f;
    const Vertex a{{-extent, 0, -extent}, {0,1,0}, {0,0}};
    const Vertex b{{extent, 0, -extent}, {0,1,0}, {16,0}};
    const Vertex c{{extent, 0, extent}, {0,1,0}, {16,16}};
    const Vertex d{{-extent, 0, extent}, {0,1,0}, {0,16}};
    vertices.insert(vertices.end(), {a, c, b, a, d, c});
    return {start, static_cast<UINT>(vertices.size()) - start};
}

MeshRange append_trunk(std::vector<Vertex>& vertices) {
    const UINT start = static_cast<UINT>(vertices.size());
    constexpr unsigned segments = 8;
    constexpr float pi = 3.14159265f;
    for (unsigned i = 0; i < segments; ++i) {
        const float a0 = 2.0f * pi * static_cast<float>(i) / segments;
        const float a1 = 2.0f * pi * static_cast<float>(i + 1) / segments;
        const DirectX::XMFLOAT3 p0{0.17f * std::cos(a0), 0.0f, 0.17f * std::sin(a0)};
        const DirectX::XMFLOAT3 p1{0.17f * std::cos(a1), 0.0f, 0.17f * std::sin(a1)};
        const DirectX::XMFLOAT3 p2{p1.x, 2.0f, p1.z};
        const DirectX::XMFLOAT3 p3{p0.x, 2.0f, p0.z};
        const DirectX::XMFLOAT3 normal{std::cos((a0+a1)*0.5f), 0, std::sin((a0+a1)*0.5f)};
        for (const unsigned index : {0u,1u,2u,0u,2u,3u}) {
            const std::array<DirectX::XMFLOAT3,4> points{p0,p1,p2,p3};
            const std::array<DirectX::XMFLOAT2,4> uv{{{0,0},{1,0},{1,1},{0,1}}};
            vertices.push_back({points[index], normal, uv[index]});
        }
    }
    return {start, static_cast<UINT>(vertices.size()) - start};
}

MeshRange append_foliage(std::vector<Vertex>& vertices) {
    const UINT start = static_cast<UINT>(vertices.size());
    constexpr unsigned segments = 8;
    constexpr float pi = 3.14159265f;
    for (unsigned layer = 0; layer < 2; ++layer) {
        const float bottom_y = 0.75f + layer * 0.82f;
        const float height = 1.55f;
        const float radius = layer == 0 ? 0.9f : 0.72f;
        const DirectX::XMFLOAT3 tip{0, bottom_y + height, 0};
        for (unsigned i = 0; i < segments; ++i) {
            const float a0 = 2.0f * pi * static_cast<float>(i) / segments;
            const float a1 = 2.0f * pi * static_cast<float>(i + 1) / segments;
            const DirectX::XMFLOAT3 p0{radius * std::cos(a0), bottom_y, radius * std::sin(a0)};
            const DirectX::XMFLOAT3 p1{radius * std::cos(a1), bottom_y, radius * std::sin(a1)};
            append_triangle(vertices, p0, tip, p1, {0,0}, {0.5f,1}, {1,0});
        }
    }
    return {start, static_cast<UINT>(vertices.size()) - start};
}

MeshRange append_rock(std::vector<Vertex>& vertices) {
    const UINT start = static_cast<UINT>(vertices.size());
    constexpr float y = 0.55f;
    const DirectX::XMFLOAT3 top{0.0f, y, 0.03f};
    const DirectX::XMFLOAT3 bottom{0.04f, -y * 0.72f, -0.02f};
    const std::array<DirectX::XMFLOAT3, 5> ring{{
        {-0.56f,-0.06f,-0.23f}, {0.36f,0.03f,-0.51f}, {0.58f,-0.02f,0.25f},
        {-0.21f,0.04f,0.56f}, {-0.57f,-0.02f,0.26f}}};
    for (unsigned i = 0; i < ring.size(); ++i) {
        const auto& a = ring[i];
        const auto& b = ring[(i + 1) % ring.size()];
        append_triangle(vertices, top, a, b);
        append_triangle(vertices, bottom, b, a);
    }
    return {start, static_cast<UINT>(vertices.size()) - start};
}

std::vector<std::uint32_t> make_texture(const unsigned width, const unsigned height, const unsigned kind) {
    std::vector<std::uint32_t> pixels(static_cast<std::size_t>(width) * height);
    const auto rgba = [](const unsigned red, const unsigned green, const unsigned blue) {
        return 0xFF000000u | (blue << 16u) | (green << 8u) | red;
    };
    for (unsigned y = 0; y < height; ++y) {
        for (unsigned x = 0; x < width; ++x) {
            std::uint32_t color{};
            if (kind == 0) {
                const bool light = ((x / 8) + (y / 8)) % 2 == 0;
                color = light ? rgba(83, 109, 74) : rgba(50, 73, 54);
            } else if (kind == 4) {
                const bool line = (x % 8u == 0u) || (y % 8u == 0u);
                color = line ? rgba(100, 120, 131) : rgba(39, 48, 54);
            } else if (kind == 1) {
                const unsigned grain = ((x * 17u + y * 7u) % 31u);
                const unsigned red = 75u + grain;
                const unsigned green = 45u + grain / 2u;
                const unsigned blue = 27u + grain / 3u;
                color = rgba(red, green, blue);
            } else if (kind == 2) {
                const unsigned speckle = ((x * 11u + y * 23u + x * y) % 34u);
                const unsigned red = 35u + speckle / 2u;
                const unsigned green = 79u + speckle;
                const unsigned blue = 42u + speckle / 2u;
                color = rgba(red, green, blue);
            } else {
                const unsigned noise = ((x * 19u + y * 29u + x * y * 3u) % 47u);
                const unsigned shade = 87u + noise;
                color = rgba(shade, shade, shade);
            }
            pixels[static_cast<std::size_t>(y) * width + x] = color;
        }
    }
    return pixels;
}

} // namespace

struct D3D11Renderer::Impl {
    HWND window{};
    unsigned width{};
    unsigned height{};
    ComPtr<ID3D11Device> device;
    ComPtr<ID3D11DeviceContext> context;
    ComPtr<IDXGISwapChain> swap_chain;
    ComPtr<ID3D11RenderTargetView> render_target;
    ComPtr<ID3D11Texture2D> depth_texture;
    ComPtr<ID3D11DepthStencilView> depth_view;
    ComPtr<ID3D11VertexShader> vertex_shader;
    ComPtr<ID3D11PixelShader> pixel_shader;
    ComPtr<ID3D11VertexShader> sky_vertex_shader;
    ComPtr<ID3D11PixelShader> sky_pixel_shader;
    ComPtr<ID3D11InputLayout> input_layout;
    ComPtr<ID3D11Buffer> vertices;
    ComPtr<ID3D11Buffer> constants;
    ComPtr<ID3D11SamplerState> sampler;
    ComPtr<ID3D11RasterizerState> scene_rasterizer;
    ComPtr<ID3D11RasterizerState> sky_rasterizer;
    ComPtr<ID3D11DepthStencilState> scene_depth_state;
    ComPtr<ID3D11DepthStencilState> sky_depth_state;
    std::unordered_map<std::string, ComPtr<ID3D11ShaderResourceView>, TransparentStringHash, std::equal_to<>> textures;
    std::unordered_map<std::string, std::vector<GpuModelPrimitive>, TransparentStringHash, std::equal_to<>> models;
    mutable std::shared_mutex textures_mutex;
    mutable std::shared_mutex models_mutex;
    std::vector<DrawObject> objects;
    std::vector<DrawObject> editor_objects;
    DirectX::XMFLOAT3 selected_position{};
    bool editor_has_selection{};
    bool editor_mode{};
    MeshRange ground;
    MeshRange cube;
    MeshRange trunk;
    MeshRange foliage;
    MeshRange rock;
    std::uint64_t frame_count{};
    std::uint64_t resize_count{};
    UINT draw_calls{};
    bool initialized{};
    bool device_failed{};
    double last_frame_ms{};

    bool create_targets() {
        if (!width || !height) return true;
        ComPtr<ID3D11Texture2D> back_buffer;
        HRESULT hr = swap_chain->GetBuffer(0, IID_PPV_ARGS(&back_buffer));
        if (FAILED(hr)) { log_hresult("IDXGISwapChain::GetBuffer", hr); return false; }
        hr = device->CreateRenderTargetView(back_buffer.Get(), nullptr, &render_target);
        if (FAILED(hr)) { log_hresult("CreateRenderTargetView", hr); return false; }
        D3D11_TEXTURE2D_DESC depth_desc{};
        depth_desc.Width = width;
        depth_desc.Height = height;
        depth_desc.MipLevels = 1;
        depth_desc.ArraySize = 1;
        depth_desc.Format = DXGI_FORMAT_D24_UNORM_S8_UINT;
        depth_desc.SampleDesc.Count = 1;
        depth_desc.Usage = D3D11_USAGE_DEFAULT;
        depth_desc.BindFlags = D3D11_BIND_DEPTH_STENCIL;
        hr = device->CreateTexture2D(&depth_desc, nullptr, &depth_texture);
        if (FAILED(hr)) { log_hresult("CreateTexture2D(depth)", hr); return false; }
        hr = device->CreateDepthStencilView(depth_texture.Get(), nullptr, &depth_view);
        if (FAILED(hr)) { log_hresult("CreateDepthStencilView", hr); return false; }
        context->OMSetRenderTargets(1, render_target.GetAddressOf(), depth_view.Get());
        D3D11_VIEWPORT viewport{};
        viewport.Width = static_cast<float>(width);
        viewport.Height = static_cast<float>(height);
        viewport.MaxDepth = 1.0f;
        context->RSSetViewports(1, &viewport);
        return true;
    }

    bool create_scene_resources() {
        ComPtr<ID3DBlob> vs_blob;
        ComPtr<ID3DBlob> ps_blob;
        ComPtr<ID3DBlob> sky_vs_blob;
        ComPtr<ID3DBlob> sky_ps_blob;
        if (!compile_shader(kSceneShader, "VSMain", "vs_5_0", vs_blob) ||
            !compile_shader(kSceneShader, "PSMain", "ps_5_0", ps_blob) ||
            !compile_shader(kSkyShader, "VSMain", "vs_5_0", sky_vs_blob) ||
            !compile_shader(kSkyShader, "PSMain", "ps_5_0", sky_ps_blob)) return false;
        HRESULT hr = device->CreateVertexShader(vs_blob->GetBufferPointer(), vs_blob->GetBufferSize(), nullptr, &vertex_shader);
        if (FAILED(hr)) { log_hresult("CreateVertexShader(scene)", hr); return false; }
        hr = device->CreatePixelShader(ps_blob->GetBufferPointer(), ps_blob->GetBufferSize(), nullptr, &pixel_shader);
        if (FAILED(hr)) { log_hresult("CreatePixelShader(scene)", hr); return false; }
        hr = device->CreateVertexShader(sky_vs_blob->GetBufferPointer(), sky_vs_blob->GetBufferSize(), nullptr, &sky_vertex_shader);
        if (FAILED(hr)) { log_hresult("CreateVertexShader(sky)", hr); return false; }
        hr = device->CreatePixelShader(sky_ps_blob->GetBufferPointer(), sky_ps_blob->GetBufferSize(), nullptr, &sky_pixel_shader);
        if (FAILED(hr)) { log_hresult("CreatePixelShader(sky)", hr); return false; }

        constexpr std::array<D3D11_INPUT_ELEMENT_DESC, 3> layout{{
            {"POSITION", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0, offsetof(Vertex, position), D3D11_INPUT_PER_VERTEX_DATA, 0},
            {"NORMAL", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0, offsetof(Vertex, normal), D3D11_INPUT_PER_VERTEX_DATA, 0},
            {"TEXCOORD", 0, DXGI_FORMAT_R32G32_FLOAT, 0, offsetof(Vertex, uv), D3D11_INPUT_PER_VERTEX_DATA, 0},
        }};
        hr = device->CreateInputLayout(layout.data(), static_cast<UINT>(layout.size()),
            vs_blob->GetBufferPointer(), vs_blob->GetBufferSize(), &input_layout);
        if (FAILED(hr)) { log_hresult("CreateInputLayout", hr); return false; }

        std::vector<Vertex> all_vertices;
        ground = append_ground(all_vertices);
        cube = append_cube(all_vertices);
        trunk = append_trunk(all_vertices);
        foliage = append_foliage(all_vertices);
        rock = append_rock(all_vertices);
        D3D11_BUFFER_DESC vertex_desc{};
        vertex_desc.ByteWidth = static_cast<UINT>(all_vertices.size() * sizeof(Vertex));
        vertex_desc.Usage = D3D11_USAGE_IMMUTABLE;
        vertex_desc.BindFlags = D3D11_BIND_VERTEX_BUFFER;
        D3D11_SUBRESOURCE_DATA vertex_data{};
        vertex_data.pSysMem = all_vertices.data();
        hr = device->CreateBuffer(&vertex_desc, &vertex_data, &vertices);
        if (FAILED(hr)) { log_hresult("CreateBuffer(scene vertices)", hr); return false; }

        D3D11_BUFFER_DESC constant_desc{};
        constant_desc.ByteWidth = static_cast<UINT>(sizeof(SceneConstants));
        constant_desc.Usage = D3D11_USAGE_DEFAULT;
        constant_desc.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
        hr = device->CreateBuffer(&constant_desc, nullptr, &constants);
        if (FAILED(hr)) { log_hresult("CreateBuffer(scene constants)", hr); return false; }

        D3D11_SAMPLER_DESC sampler_desc{};
        sampler_desc.Filter = D3D11_FILTER_MIN_MAG_MIP_LINEAR;
        sampler_desc.AddressU = D3D11_TEXTURE_ADDRESS_WRAP;
        sampler_desc.AddressV = D3D11_TEXTURE_ADDRESS_WRAP;
        sampler_desc.AddressW = D3D11_TEXTURE_ADDRESS_WRAP;
        sampler_desc.MaxLOD = D3D11_FLOAT32_MAX;
        hr = device->CreateSamplerState(&sampler_desc, &sampler);
        if (FAILED(hr)) { log_hresult("CreateSamplerState", hr); return false; }

        D3D11_RASTERIZER_DESC raster_desc{};
        raster_desc.FillMode = D3D11_FILL_SOLID;
        raster_desc.CullMode = D3D11_CULL_BACK;
        raster_desc.DepthClipEnable = TRUE;
        hr = device->CreateRasterizerState(&raster_desc, &scene_rasterizer);
        if (FAILED(hr)) { log_hresult("CreateRasterizerState(scene)", hr); return false; }
        raster_desc.CullMode = D3D11_CULL_NONE;
        hr = device->CreateRasterizerState(&raster_desc, &sky_rasterizer);
        if (FAILED(hr)) { log_hresult("CreateRasterizerState(sky)", hr); return false; }

        D3D11_DEPTH_STENCIL_DESC depth_desc{};
        depth_desc.DepthEnable = TRUE;
        depth_desc.DepthWriteMask = D3D11_DEPTH_WRITE_MASK_ALL;
        depth_desc.DepthFunc = D3D11_COMPARISON_LESS;
        hr = device->CreateDepthStencilState(&depth_desc, &scene_depth_state);
        if (FAILED(hr)) { log_hresult("CreateDepthStencilState(scene)", hr); return false; }
        depth_desc.DepthEnable = FALSE;
        depth_desc.DepthWriteMask = D3D11_DEPTH_WRITE_MASK_ZERO;
        hr = device->CreateDepthStencilState(&depth_desc, &sky_depth_state);
        if (FAILED(hr)) { log_hresult("CreateDepthStencilState(sky)", hr); return false; }

        const auto ground_pixels = make_texture(64, 64, 0);
        const auto bark_pixels = make_texture(64, 64, 1);
        const auto leaf_pixels = make_texture(64, 64, 2);
        const auto rock_pixels = make_texture(64, 64, 3);
        const auto grid_pixels = make_texture(64, 64, 4);
        if (!owner->register_texture_rgba8("ground", 64, 64, ground_pixels) ||
            !owner->register_texture_rgba8("bark", 64, 64, bark_pixels) ||
            !owner->register_texture_rgba8("leaves", 64, 64, leaf_pixels) ||
            !owner->register_texture_rgba8("stone", 64, 64, rock_pixels) ||
            !owner->register_texture_rgba8("editor_grid", 64, 64, grid_pixels)) return false;

        objects.push_back({ground, DirectX::XMMatrixIdentity(), "ground", {1,1,1,1}, 0.94f, 0.0f});
        constexpr std::array<DirectX::XMFLOAT2, 7> tree_positions{{
            {-7,2}, {-4,7}, {5,5}, {9,1}, {-10,-5}, {4,-8}, {11,-9}}};
        for (std::size_t i = 0; i < tree_positions.size(); ++i) {
            const auto pos = tree_positions[i];
            const float scale = 0.8f + 0.13f * static_cast<float>(i % 4);
            const auto transform = DirectX::XMMatrixScaling(scale, scale, scale) *
                DirectX::XMMatrixTranslation(pos.x, 0, pos.y);
            objects.push_back({trunk, transform, "bark", {1,1,1,1}, 0.88f, 0.0f});
            objects.push_back({foliage, transform, "leaves", {1,1,1,1}, 0.82f, 0.0f});
        }
        constexpr std::array<DirectX::XMFLOAT2, 8> rock_positions{{
            {-3,1}, {3,3}, {7,-3}, {-8,8}, {0,10}, {-12,0}, {12,7}, {-5,-10}}};
        for (std::size_t i = 0; i < rock_positions.size(); ++i) {
            const auto pos = rock_positions[i];
            const float scale = 0.75f + 0.16f * static_cast<float>(i % 3);
            const auto transform = DirectX::XMMatrixScaling(scale, scale, scale) *
                DirectX::XMMatrixRotationY(static_cast<float>(i) * 0.7f) *
                DirectX::XMMatrixTranslation(pos.x, 0.42f * scale, pos.y);
            objects.push_back({rock, transform, "stone", {0.86f,0.87f,0.83f,1}, 0.78f, 0.0f});
        }
        return true;
    }

    D3D11Renderer* owner{};
};

D3D11Renderer::D3D11Renderer() : impl_(std::make_unique<Impl>()) { impl_->owner = this; }
D3D11Renderer::~D3D11Renderer() = default;

bool D3D11Renderer::initialize(HWND window, const unsigned width, const unsigned height) {
    if (!window || !width || !height) {
        core::Logger::instance().write(core::LogLevel::Error, "Renderer initialization requires a valid window and nonzero dimensions.");
        return false;
    }
    if (impl_->device) {
        core::Logger::instance().write(core::LogLevel::Error, "Renderer is already initialized.");
        return false;
    }
    impl_->window = window;
    impl_->width = width;
    impl_->height = height;

    DXGI_SWAP_CHAIN_DESC swap_desc{};
    swap_desc.BufferDesc.Width = width;
    swap_desc.BufferDesc.Height = height;
    swap_desc.BufferDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    swap_desc.SampleDesc.Count = 1;
    swap_desc.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    swap_desc.BufferCount = 2;
    swap_desc.OutputWindow = window;
    swap_desc.Windowed = TRUE;
    swap_desc.SwapEffect = DXGI_SWAP_EFFECT_DISCARD;
    constexpr D3D_FEATURE_LEVEL levels[]{D3D_FEATURE_LEVEL_11_0};
    D3D_FEATURE_LEVEL chosen{};
    UINT flags = 0;
#if defined(_DEBUG)
    flags = D3D11_CREATE_DEVICE_DEBUG;
#endif
    HRESULT hr = D3D11CreateDeviceAndSwapChain(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr,
        flags, levels, static_cast<UINT>(std::size(levels)), D3D11_SDK_VERSION, &swap_desc,
        &impl_->swap_chain, &impl_->device, &chosen, &impl_->context);
#if defined(_DEBUG)
    if (FAILED(hr)) {
        impl_->swap_chain.Reset(); impl_->device.Reset(); impl_->context.Reset();
        core::Logger::instance().write(core::LogLevel::Warning, "D3D11 debug layer unavailable; retrying without it.");
        hr = D3D11CreateDeviceAndSwapChain(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr,
            0, levels, static_cast<UINT>(std::size(levels)), D3D11_SDK_VERSION, &swap_desc,
            &impl_->swap_chain, &impl_->device, &chosen, &impl_->context);
    }
#endif
    if (FAILED(hr)) {
        impl_->swap_chain.Reset(); impl_->device.Reset(); impl_->context.Reset();
        hr = D3D11CreateDeviceAndSwapChain(nullptr, D3D_DRIVER_TYPE_WARP, nullptr,
            0, levels, static_cast<UINT>(std::size(levels)), D3D11_SDK_VERSION, &swap_desc,
            &impl_->swap_chain, &impl_->device, &chosen, &impl_->context);
    }
    if (FAILED(hr)) { log_hresult("D3D11CreateDeviceAndSwapChain", hr); return false; }

    std::ostringstream device_message;
    device_message << "D3D11 device created at feature level 0x" << std::hex << static_cast<unsigned>(chosen)
                   << (chosen == D3D_FEATURE_LEVEL_11_0 ? " (11.0)." : ".");
    core::Logger::instance().write(core::LogLevel::Info, device_message.str());
    if (!impl_->create_targets() || !impl_->create_scene_resources()) {
        core::Logger::instance().write(core::LogLevel::Error, "Renderer initialization failed; releasing partially created resources.");
        impl_.reset();
        impl_ = std::make_unique<Impl>();
        impl_->owner = this;
        return false;
    }
    impl_->initialized = true;
    return true;
}

void D3D11Renderer::resize(const unsigned width, const unsigned height) {
    if (!impl_->swap_chain || !width || !height || (width == impl_->width && height == impl_->height)) return;
    impl_->context->OMSetRenderTargets(0, nullptr, nullptr);
    impl_->render_target.Reset();
    impl_->depth_view.Reset();
    impl_->depth_texture.Reset();
    const HRESULT hr = impl_->swap_chain->ResizeBuffers(0, width, height, DXGI_FORMAT_UNKNOWN, 0);
    if (FAILED(hr)) {
        log_hresult("IDXGISwapChain::ResizeBuffers", hr);
        if (!impl_->create_targets()) core::Logger::instance().write(core::LogLevel::Error, "Could not restore old render targets after resize failed.");
        return;
    }
    impl_->width = width;
    impl_->height = height;
    if (!impl_->create_targets()) core::Logger::instance().write(core::LogLevel::Error, "Could not restore render targets after resize.");
    else ++impl_->resize_count;
}

RendererStats D3D11Renderer::stats() const noexcept {
    return {impl_->initialized, impl_->width, impl_->height, impl_->draw_calls,
        impl_->frame_count, impl_->resize_count, impl_->last_frame_ms};
}

void D3D11Renderer::set_editor_scene(const std::span<const RenderObject> objects) {
    impl_->editor_objects.clear();
    impl_->editor_objects.reserve(objects.size());
    impl_->editor_mode = true;
    impl_->selected_position = {};
    impl_->editor_has_selection = false;
    for (const RenderObject& object : objects) {
        if (!object.visible) continue;
        const DirectX::XMMATRIX transform = DirectX::XMMatrixScaling(object.scale.x, object.scale.y, object.scale.z) *
            DirectX::XMMatrixRotationRollPitchYaw(DirectX::XMConvertToRadians(object.rotation_degrees.x),
                DirectX::XMConvertToRadians(object.rotation_degrees.y), DirectX::XMConvertToRadians(object.rotation_degrees.z)) *
            DirectX::XMMatrixTranslation(object.position.x, object.position.y, object.position.z);
        impl_->editor_objects.push_back({impl_->cube, transform, object.base_color_texture_id, object.base_color,
            object.roughness, object.metallic, object.model_asset_id, object.material_override, object.emission});
        if (object.selected) {
            impl_->selected_position = object.position;
            impl_->editor_has_selection = true;
        }
    }
}

int D3D11Renderer::pick_editor_object(const float screen_x, const float screen_y,
    const std::span<const RenderObject> objects) const noexcept {
    if (!impl_->width || !impl_->height) return -1;
    const DirectX::XMMATRIX view = camera_.view();
    const DirectX::XMMATRIX projection = camera_.projection(
        static_cast<float>(impl_->width) / static_cast<float>(impl_->height));
    const DirectX::XMMATRIX identity = DirectX::XMMatrixIdentity();
    float nearest_depth = 1.0f;
    int nearest_index = -1;
    for (std::size_t i = 0; i < objects.size(); ++i) {
        const RenderObject& object = objects[i];
        if (!object.visible) continue;
        const DirectX::XMVECTOR world_position = DirectX::XMVectorSet(object.position.x, object.position.y, object.position.z, 1.0f);
        const DirectX::XMVECTOR projected = DirectX::XMVector3Project(world_position, 0, 0,
            static_cast<float>(impl_->width), static_cast<float>(impl_->height), 0, 1, projection, view, identity);
        DirectX::XMFLOAT3 screen{};
        DirectX::XMStoreFloat3(&screen, projected);
        if (screen.z < 0 || screen.z > 1) continue;
        const DirectX::XMVECTOR edge_world = DirectX::XMVectorSet(
            object.position.x + std::max(object.scale.x, object.scale.y) * 0.5f,
            object.position.y, object.position.z, 1.0f);
        const DirectX::XMVECTOR edge_projected = DirectX::XMVector3Project(edge_world, 0, 0,
            static_cast<float>(impl_->width), static_cast<float>(impl_->height), 0, 1, projection, view, identity);
        DirectX::XMFLOAT3 edge{};
        DirectX::XMStoreFloat3(&edge, edge_projected);
        const float radius = std::clamp(std::abs(edge.x - screen.x), 14.0f, 56.0f) + 8.0f;
        const float dx = screen.x - screen_x;
        const float dy = screen.y - screen_y;
        if (dx * dx + dy * dy <= radius * radius && screen.z <= nearest_depth) {
            nearest_depth = screen.z;
            nearest_index = static_cast<int>(i);
        }
    }
    return nearest_index;
}

bool D3D11Renderer::register_texture_rgba8(const std::string_view name, const unsigned width,
    const unsigned height, const std::span<const std::uint32_t> pixels) {
    if (!impl_->device || name.empty() || !width || !height ||
        width > D3D11_REQ_TEXTURE2D_U_OR_V_DIMENSION || height > D3D11_REQ_TEXTURE2D_U_OR_V_DIMENSION ||
        pixels.size() != static_cast<std::size_t>(width) * height) {
        core::Logger::instance().write(core::LogLevel::Error, "Texture registration rejected invalid dimensions or pixel data.");
        return false;
    }
    D3D11_TEXTURE2D_DESC desc{};
    desc.Width = width;
    desc.Height = height;
    desc.MipLevels = 1;
    desc.ArraySize = 1;
    desc.Format = DXGI_FORMAT_R8G8B8A8_UNORM_SRGB;
    desc.SampleDesc.Count = 1;
    desc.Usage = D3D11_USAGE_IMMUTABLE;
    desc.BindFlags = D3D11_BIND_SHADER_RESOURCE;
    D3D11_SUBRESOURCE_DATA data{};
    data.pSysMem = pixels.data();
    data.SysMemPitch = static_cast<UINT>(width * sizeof(std::uint32_t));
    ComPtr<ID3D11Texture2D> texture;
    HRESULT hr = impl_->device->CreateTexture2D(&desc, &data, &texture);
    if (FAILED(hr)) { log_hresult("CreateTexture2D(asset)", hr); return false; }
    ComPtr<ID3D11ShaderResourceView> view;
    hr = impl_->device->CreateShaderResourceView(texture.Get(), nullptr, &view);
    if (FAILED(hr)) { log_hresult("CreateShaderResourceView(asset)", hr); return false; }
    {
        std::unique_lock lock(impl_->textures_mutex);
        impl_->textures.insert_or_assign(std::string(name), std::move(view));
    }
    return true;
}

bool D3D11Renderer::register_model(const std::string_view asset_id,
    const std::span<const ModelPrimitiveUpload> primitives) {
    if (!impl_->device || asset_id.empty() || primitives.empty()) {
        core::Logger::instance().write(core::LogLevel::Error, "Model registration rejected an empty id, primitive list, or uninitialized renderer.");
        return false;
    }
    std::vector<GpuModelPrimitive> uploaded;
    uploaded.reserve(primitives.size());
    for (const ModelPrimitiveUpload& primitive : primitives) {
        const std::array<float, 4> color{primitive.base_color.x, primitive.base_color.y, primitive.base_color.z, primitive.base_color.w};
        const auto& matrix = primitive.local_transform;
        const std::array<float, 16> transform{matrix._11, matrix._12, matrix._13, matrix._14,
            matrix._21, matrix._22, matrix._23, matrix._24, matrix._31, matrix._32, matrix._33, matrix._34,
            matrix._41, matrix._42, matrix._43, matrix._44};
        if (primitive.vertices.empty() || primitive.indices.empty() || primitive.indices.size() % 3 != 0 ||
            primitive.vertices.size() > (std::numeric_limits<UINT>::max)() / sizeof(Vertex) ||
            primitive.indices.size() > (std::numeric_limits<UINT>::max)() / sizeof(UINT) ||
            std::any_of(primitive.indices.begin(), primitive.indices.end(), [&](std::uint32_t index) { return index >= primitive.vertices.size(); }) ||
            !std::isfinite(primitive.roughness) || !std::isfinite(primitive.metallic) ||
            primitive.roughness < 0 || primitive.roughness > 1 || primitive.metallic < 0 || primitive.metallic > 1 ||
            !std::all_of(color.begin(), color.end(), [](float value) { return std::isfinite(value) && value >= 0 && value <= 1; }) ||
            !std::isfinite(primitive.emission.x) || !std::isfinite(primitive.emission.y) || !std::isfinite(primitive.emission.z) ||
            primitive.emission.x < 0 || primitive.emission.y < 0 || primitive.emission.z < 0 ||
            primitive.emission.x > 64 || primitive.emission.y > 64 || primitive.emission.z > 64 ||
            !std::all_of(transform.begin(), transform.end(), [](float value) { return std::isfinite(value); }) ||
            !std::all_of(primitive.vertices.begin(), primitive.vertices.end(), [](const ModelVertex& vertex) {
                return std::isfinite(vertex.position.x) && std::isfinite(vertex.position.y) && std::isfinite(vertex.position.z) &&
                    std::isfinite(vertex.normal.x) && std::isfinite(vertex.normal.y) && std::isfinite(vertex.normal.z) &&
                    std::isfinite(vertex.uv.x) && std::isfinite(vertex.uv.y);
            })) {
            core::Logger::instance().write(core::LogLevel::Error, "Model registration rejected invalid or oversized triangle geometry.");
            return false;
        }
        std::vector<Vertex> vertices;
        vertices.reserve(primitive.vertices.size());
        for (const ModelVertex& vertex : primitive.vertices) vertices.push_back({vertex.position, vertex.normal, vertex.uv});
        D3D11_BUFFER_DESC vertex_desc{};
        vertex_desc.ByteWidth = static_cast<UINT>(vertices.size() * sizeof(Vertex));
        vertex_desc.Usage = D3D11_USAGE_IMMUTABLE;
        vertex_desc.BindFlags = D3D11_BIND_VERTEX_BUFFER;
        D3D11_SUBRESOURCE_DATA vertex_data{};
        vertex_data.pSysMem = vertices.data();
        GpuModelPrimitive gpu;
        HRESULT hr = impl_->device->CreateBuffer(&vertex_desc, &vertex_data, &gpu.vertices);
        if (FAILED(hr)) { log_hresult("CreateBuffer(imported model vertices)", hr); return false; }
        D3D11_BUFFER_DESC index_desc{};
        index_desc.ByteWidth = static_cast<UINT>(primitive.indices.size() * sizeof(UINT));
        index_desc.Usage = D3D11_USAGE_IMMUTABLE;
        index_desc.BindFlags = D3D11_BIND_INDEX_BUFFER;
        D3D11_SUBRESOURCE_DATA index_data{};
        index_data.pSysMem = primitive.indices.data();
        hr = impl_->device->CreateBuffer(&index_desc, &index_data, &gpu.indices);
        if (FAILED(hr)) { log_hresult("CreateBuffer(imported model indices)", hr); return false; }
        gpu.index_count = static_cast<UINT>(primitive.indices.size());
        gpu.base_color = primitive.base_color;
        gpu.emission = primitive.emission;
        gpu.local_transform = primitive.local_transform;
        gpu.roughness = primitive.roughness;
        gpu.metallic = primitive.metallic;
        uploaded.push_back(std::move(gpu));
    }
    {
        std::unique_lock lock(impl_->models_mutex);
        impl_->models.insert_or_assign(std::string(asset_id), std::move(uploaded));
    }
    return true;
}

bool D3D11Renderer::register_texture_rgba8_mipped(const std::string_view name,
    const std::span<const TextureMipUpload> mip_levels) {
    if (!impl_->device || name.empty() || mip_levels.empty() || mip_levels.size() > D3D11_REQ_MIP_LEVELS) return false;
    for (const auto& mip : mip_levels) {
        if (!mip.width || !mip.height || mip.width > D3D11_REQ_TEXTURE2D_U_OR_V_DIMENSION ||
            mip.height > D3D11_REQ_TEXTURE2D_U_OR_V_DIMENSION ||
            mip.rgba8.size() != static_cast<std::size_t>(mip.width) * mip.height * 4) {
            core::Logger::instance().write(core::LogLevel::Error, "Mipped texture registration rejected invalid RGBA8 levels.");
            return false;
        }
    }
    D3D11_TEXTURE2D_DESC desc{};
    desc.Width = mip_levels.front().width;
    desc.Height = mip_levels.front().height;
    desc.MipLevels = static_cast<UINT>(mip_levels.size());
    desc.ArraySize = 1;
    desc.Format = DXGI_FORMAT_R8G8B8A8_UNORM_SRGB;
    desc.SampleDesc.Count = 1;
    desc.Usage = D3D11_USAGE_IMMUTABLE;
    desc.BindFlags = D3D11_BIND_SHADER_RESOURCE;
    std::vector<D3D11_SUBRESOURCE_DATA> initial_data(mip_levels.size());
    for (std::size_t i = 0; i < mip_levels.size(); ++i) {
        if (i && mip_levels[i-1].width == 1 && mip_levels[i-1].height == 1) return false;
        if (i && (mip_levels[i].width != (std::max)(1u, mip_levels[i-1].width / 2) ||
            mip_levels[i].height != (std::max)(1u, mip_levels[i-1].height / 2))) {
            core::Logger::instance().write(core::LogLevel::Error, "Mipped texture levels are not a contiguous mip chain.");
            return false;
        }
        initial_data[i].pSysMem = mip_levels[i].rgba8.data();
        initial_data[i].SysMemPitch = mip_levels[i].width * 4;
        initial_data[i].SysMemSlicePitch = static_cast<UINT>(mip_levels[i].rgba8.size());
    }
    ComPtr<ID3D11Texture2D> texture;
    HRESULT hr = impl_->device->CreateTexture2D(&desc, initial_data.data(), &texture);
    if (FAILED(hr)) { log_hresult("CreateTexture2D(mipped asset)", hr); return false; }
    ComPtr<ID3D11ShaderResourceView> view;
    hr = impl_->device->CreateShaderResourceView(texture.Get(), nullptr, &view);
    if (FAILED(hr)) { log_hresult("CreateShaderResourceView(mipped asset)", hr); return false; }
    {
        std::unique_lock lock(impl_->textures_mutex);
        impl_->textures.insert_or_assign(std::string(name), std::move(view));
    }
    return true;
}

void D3D11Renderer::release_asset(const std::string_view asset_id) {
    if (impl_->context) {
        ID3D11ShaderResourceView* null_view = nullptr;
        ID3D11Buffer* null_buffer = nullptr;
        const UINT zero = 0;
        impl_->context->PSSetShaderResources(0, 1, &null_view);
        impl_->context->IASetVertexBuffers(0, 1, &null_buffer, &zero, &zero);
        impl_->context->IASetIndexBuffer(nullptr, DXGI_FORMAT_UNKNOWN, 0);
    }
    {
        std::unique_lock lock(impl_->models_mutex);
        impl_->models.erase(std::string(asset_id));
    }
    {
        std::unique_lock lock(impl_->textures_mutex);
        impl_->textures.erase(std::string(asset_id));
    }
}

void D3D11Renderer::move_camera(const float forward, const float right, const float up, const float delta_seconds) noexcept {
    camera_.move(forward, right, up, delta_seconds);
}

void D3D11Renderer::rotate_camera(const float delta_yaw, const float delta_pitch) noexcept {
    camera_.rotate(delta_yaw, delta_pitch);
}

void D3D11Renderer::zoom_camera(const float amount) noexcept { camera_.zoom(amount); }

void D3D11Renderer::render(const float elapsed_seconds, const float delta_seconds) {
    if (!impl_->initialized || impl_->device_failed || !impl_->context || !impl_->render_target || !impl_->width || !impl_->height) return;
    impl_->last_frame_ms = static_cast<double>(std::max(delta_seconds, 0.0f)) * 1000.0;
    impl_->frame_count++;
    constexpr float clear[] = {0.12f, 0.20f, 0.28f, 1.0f};
    impl_->context->ClearRenderTargetView(impl_->render_target.Get(), clear);
    impl_->context->ClearDepthStencilView(impl_->depth_view.Get(), D3D11_CLEAR_DEPTH | D3D11_CLEAR_STENCIL, 1.0f, 0);
    impl_->draw_calls = 0;

    impl_->context->OMSetRenderTargets(1, impl_->render_target.GetAddressOf(), impl_->depth_view.Get());
    impl_->context->OMSetDepthStencilState(impl_->sky_depth_state.Get(), 0);
    impl_->context->RSSetState(impl_->sky_rasterizer.Get());
    impl_->context->IASetInputLayout(nullptr);
    impl_->context->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    impl_->context->VSSetShader(impl_->sky_vertex_shader.Get(), nullptr, 0);
    impl_->context->PSSetShader(impl_->sky_pixel_shader.Get(), nullptr, 0);
    impl_->context->Draw(3, 0);
    ++impl_->draw_calls;

    impl_->context->OMSetDepthStencilState(impl_->scene_depth_state.Get(), 0);
    impl_->context->RSSetState(impl_->scene_rasterizer.Get());
    const UINT stride = static_cast<UINT>(sizeof(Vertex));
    constexpr UINT offset = 0;
    impl_->context->IASetVertexBuffers(0, 1, impl_->vertices.GetAddressOf(), &stride, &offset);
    impl_->context->IASetInputLayout(impl_->input_layout.Get());
    impl_->context->VSSetShader(impl_->vertex_shader.Get(), nullptr, 0);
    impl_->context->VSSetConstantBuffers(0, 1, impl_->constants.GetAddressOf());
    impl_->context->PSSetShader(impl_->pixel_shader.Get(), nullptr, 0);
    impl_->context->PSSetConstantBuffers(0, 1, impl_->constants.GetAddressOf());
    impl_->context->PSSetSamplers(0, 1, impl_->sampler.GetAddressOf());

    const DirectX::XMVECTOR camera_position = camera_.position();
    DirectX::XMFLOAT3 camera_pos{};
    DirectX::XMStoreFloat3(&camera_pos, camera_position);
    const DirectX::XMMATRIX view = camera_.view();
    const DirectX::XMMATRIX projection = camera_.projection(
        static_cast<float>(impl_->width) / static_cast<float>(impl_->height));
    const auto draw = [&](const DrawObject& object, const GpuModelPrimitive* model_primitive = nullptr) {
        SceneConstants data{};
        const DirectX::XMMATRIX world = model_primitive
            ? object.world * DirectX::XMLoadFloat4x4(&model_primitive->local_transform) : object.world;
        DirectX::XMStoreFloat4x4(&data.world_view_projection, world * view * projection);
        DirectX::XMStoreFloat4x4(&data.world, world);
        const float world_determinant = DirectX::XMVectorGetX(DirectX::XMMatrixDeterminant(world));
        const DirectX::XMMATRIX normal_transform = std::isfinite(world_determinant) && std::abs(world_determinant) > 1e-8f
            ? DirectX::XMMatrixTranspose(DirectX::XMMatrixInverse(nullptr, world)) : DirectX::XMMatrixIdentity();
        DirectX::XMStoreFloat4x4(&data.normal_transform, normal_transform);
        data.camera_position = {camera_pos.x, camera_pos.y, camera_pos.z, 1};
        data.base_color = model_primitive && !object.material_override ? model_primitive->base_color : object.color;
        const DirectX::XMFLOAT3 emission = model_primitive && !object.material_override ? model_primitive->emission : object.emission;
        data.emission = {emission.x, emission.y, emission.z, 0};
        const float roughness = model_primitive && !object.material_override ? model_primitive->roughness : object.roughness;
        const float metallic = model_primitive && !object.material_override ? model_primitive->metallic : object.metallic;
        data.material = {roughness, metallic, 0.0f, 0};
        data.light_direction = {-0.42f, -1.0f, 0.28f, 0};
        data.fog_color_density = {0.61f, 0.64f, 0.52f, 0.018f};
        ID3D11ShaderResourceView* texture = nullptr;
        std::shared_lock texture_lock(impl_->textures_mutex, std::defer_lock);
        if (!object.texture.empty()) {
            texture_lock.lock();
            const auto found = impl_->textures.find(object.texture);
            if (found != impl_->textures.end()) {
                texture = found->second.Get();
                data.material.z = 1.0f;
            }
        }
        impl_->context->UpdateSubresource(impl_->constants.Get(), 0, nullptr, &data, 0, 0);
        impl_->context->PSSetShaderResources(0, 1, &texture);
        if (texture_lock.owns_lock()) texture_lock.unlock();
        if (model_primitive) {
            const UINT model_stride = static_cast<UINT>(sizeof(Vertex));
            const UINT model_offset = 0;
            impl_->context->IASetVertexBuffers(0, 1, model_primitive->vertices.GetAddressOf(), &model_stride, &model_offset);
            impl_->context->IASetIndexBuffer(model_primitive->indices.Get(), DXGI_FORMAT_R32_UINT, 0);
            impl_->context->DrawIndexed(model_primitive->index_count, 0, 0);
        } else {
            const UINT stride = static_cast<UINT>(sizeof(Vertex));
            const UINT offset = 0;
            impl_->context->IASetVertexBuffers(0, 1, impl_->vertices.GetAddressOf(), &stride, &offset);
            impl_->context->IASetIndexBuffer(nullptr, DXGI_FORMAT_UNKNOWN, 0);
            impl_->context->Draw(object.mesh.count, object.mesh.start);
        }
        ++impl_->draw_calls;
    };
    if (impl_->editor_mode) {
        draw({impl_->ground, DirectX::XMMatrixIdentity(), "editor_grid", {1,1,1,1}, 0.94f, 0.0f});
        for (const DrawObject& object : impl_->editor_objects) {
            std::shared_lock model_lock(impl_->models_mutex);
            const auto model = impl_->models.find(object.model_asset_id);
            if (object.model_asset_id.empty()) draw(object);
            else if (model == impl_->models.end()) continue;
            else for (const auto& primitive : model->second) draw(object, &primitive);
        }
        if (impl_->editor_has_selection) {
            const auto translation = impl_->selected_position;
            draw({impl_->cube, DirectX::XMMatrixScaling(1.2f, 0.035f, 0.035f) *
                DirectX::XMMatrixTranslation(translation.x + 0.6f, translation.y, translation.z),
            "", {1.0f,0.18f,0.16f,1}, 0.45f, 0.0f});
            draw({impl_->cube, DirectX::XMMatrixScaling(0.035f, 1.2f, 0.035f) *
                DirectX::XMMatrixTranslation(translation.x, translation.y + 0.6f, translation.z),
                "", {0.22f,1.0f,0.28f,1}, 0.45f, 0.0f});
            draw({impl_->cube, DirectX::XMMatrixScaling(0.035f, 0.035f, 1.2f) *
                DirectX::XMMatrixTranslation(translation.x, translation.y, translation.z + 0.6f),
                "", {0.22f,0.48f,1.0f,1}, 0.45f, 0.0f});
        }
    } else {
        for (const DrawObject& object : impl_->objects) draw(object);
        DrawObject animated_cube{impl_->cube,
            DirectX::XMMatrixRotationY(elapsed_seconds * 0.35f) * DirectX::XMMatrixTranslation(0, 0.55f, 0),
            "stone", {0.82f,0.87f,0.78f,1}, 0.28f, 0.08f};
        draw(animated_cube);
    }
    const HRESULT present_result = impl_->swap_chain->Present(1, 0);
    if (FAILED(present_result)) {
        log_hresult("IDXGISwapChain::Present", present_result);
        if (present_result == DXGI_ERROR_DEVICE_REMOVED || present_result == DXGI_ERROR_DEVICE_RESET) {
            log_hresult("ID3D11Device::GetDeviceRemovedReason", impl_->device->GetDeviceRemovedReason());
        }
        impl_->device_failed = true;
    }
}

} // namespace together::renderer
