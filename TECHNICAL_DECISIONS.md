# Technical Decisions

## Foundation and renderer

- **Language/build:** C++20 and CMake 3.24+, with Visual Studio 2022 x64 as the supported build generator.
- **Window/input:** Native Win32 remains in use for this foundation. It avoids adding a package before the renderer and game loop interfaces are stable.
- **Graphics:** Direct3D 11 with hardware device creation and WARP fallback, feature level 11.0, flip-independent two-buffer swap chain, depth buffer, explicit back-face culling, and VSync.
- **Shader path:** Runtime HLSL compilation through D3DCompiler. Shader diagnostics are logged; Debug builds request the optional D3D11 debug layer and retry without it.
- **Scene:** Procedural low-poly geometry and generated textures only. They are diagnostic assets, not licensed production content or finished art.
- **Materials:** A small Cook–Torrance/GGX-compatible metallic/roughness shader with base color, texture modulation, directional light, ambient term, and distance fog. This is a renderer foundation, not a complete calibrated PBR pipeline.
- **Resources:** D3D objects are owned with WRL `ComPtr`; the current texture registration interface accepts validated RGBA8 pixel buffers and owns immutable D3D resources.
- **Camera/timing:** Camera math is separated from the D3D renderer. A steady-clock frame timer supplies delta and instantaneous FPS; client UI reports a rolling FPS and frame time.
- **Editor 0.1:** A separate native Win32 executable uses standard controls for panels and the existing D3D11 renderer in a child viewport. This avoids adding an unverified GUI package and keeps the editor target separate from the game client.
- **Scene data:** `TogetherEngineScene` owns scene objects and version 1 JSON read/write/validation. The format is documented in `Documentation/SceneFormat.md`; save writes a temporary sibling and uses `MoveFileExW` for replacement.
- **Editor build output:** CMake is launched as a separate process with redirected pipes and polled from the UI timer. Scene file operations are local and size-limited; renderer presentation remains on the UI thread.

## Deferred integrations

- SDL3 is still a candidate for window/input abstraction after an explicit migration decision.
- Jolt Physics, ENet, Dear ImGui, miniaudio, SQLite, and Lua are candidates for later milestones only. No online service or multiplayer implementation is part of the current engine slice.
- Image-file and glTF import, mip generation, texture streaming, PBR image maps, shadows, LOD, and production asset licensing/attribution remain future work.
- The initial editor viewport currently renders scene objects as cubes. Transform gizmo dragging applies screen-plane translation, Euler rotation, or uniform scaling; it does not yet provide axis-constrained handles or mesh selection.

## Verification discipline

Renderer integration tests exercise hidden-window creation, D3D11 initialization, frame submissions, resize diagnostics, and C++ resource scope teardown. GPU/window tests must run on Windows; a source-only or configure-only check is not reported as runtime verification.
