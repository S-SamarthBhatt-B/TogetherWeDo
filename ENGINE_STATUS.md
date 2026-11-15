# Together Engine Status

This status separates code that exists from behavior that has actually been verified. Current environment inspection found no CMake, MSVC `cl`, `msbuild`, or `ctest` commands. Consequently no native build, C++ unit test, GPU run, or FPS measurement has been performed here.

## Implemented in source

- C++20 CMake targets for engine core, D3D11 renderer, Windows client, and CTest integration executable.
- Win32 window lifecycle and user-facing startup errors/logging.
- D3D11 hardware initialization with WARP fallback; optional Debug-layer attempt.
- RAII ownership of D3D resources through WRL `ComPtr`.
- Runtime HLSL compilation with compiler diagnostics and HRESULT logs.
- Explicit scene depth testing and back-face culling, resize-aware swap chain targets, and VSync.
- Perspective free camera with keyboard/mouse controls.
- GGX metallic/roughness-compatible directional-light shader, ambient term, and distance fog.
- Validated RGBA8 texture registration interface with sRGB shader views.
- Procedural sky, ground, central moving object, low-poly trees and rocks with generated textures.
- Steady-clock timing, frame/draw/resize counters, and rolling FPS/frame-time display in the title bar.
- Tests authored for finite camera matrices and controls, frame timing, hidden Win32 window creation, renderer initialization, render counters, resize dimensions/count, and renderer scope cleanup.
- A separate `TogetherEngineEditor` Windows executable with dark native panels, D3D11 scene viewport, hierarchy search, selection, cube creation/deletion/duplication, transform/material/visibility inspector, transform tools, play/pause/stop preview, scene file dialogs, validation, console filters, and asynchronous CMake build output.
- `TogetherEngineScene` with versioned JSON parsing/serialization, validation, object CRUD, atomic save replacement, and `Documentation/SceneFormat.md`.
- Scene tests authored for object creation/edit/delete, save/load round-trip, malformed JSON, invalid scene data, and preserving loaded state after a failed load.
- `TogetherEngineAssets` source target with pinned cgltf, stb_image, dr_wav and nlohmann/json dependencies; SHA-256 IDs, in-memory registry/cache, generation handles and asynchronous import worker.
- PNG/JPEG/TGA RGBA8 decoding with dimension/source-format metadata and configurable downscaling; static glTF 2.0 triangle mesh/bounds/PBR factor/node-transform import; WAV float decode; versioned JSON material load/save.
- Async asset import command in the editor, import results/errors in the console, and registered asset inventory listing in the console.
- Asset tests authored for texture metadata, mip generation, material round trip/validation, registration/deduplication, handle validity, cache invalidation, invalid assets, and valid/invalid glTF import.
- Scene v1 optional asset-reference fields: model ID, material ID, base-color texture ID and emission. Older v1 scenes remain loadable; missing references remain saved and are reported on open.
- D3D11 static glTF model upload and indexed drawing for multiple primitives, including supported single-node transforms, per-primitive base color/roughness/metallic/emission factors, and resource release.
- Imported RGBA8 texture plus generated mip-chain upload for base-color sampling; material assignment commands, scalar/emission inspector values, shared-reference scalar propagation, and viewport OS file drop for static glTF.

## Partially implemented

- **Renderer diagnostics:** HRESULT/shader logs and CPU-side counters exist. No D3D11 InfoQueue collection, GPU timing, memory accounting, or on-screen diagnostics panel yet.
- **Asset browser:** Only a console inventory exists. A docked browser, directory tree, search/filter, grid/list, metadata pane, thumbnails, preview rendering, and browser rename/delete are not implemented.
- **Material system:** Reusable `.twmat.json` data, create/save-as/rename/assign/remove commands, inspector scalar editing, shared-ID scalar propagation, base color textures and emission rendering exist in source. There is no dedicated material inspector/preview sphere. Normal, metallic-roughness and occlusion maps remain stored or referenced but are not sampled by the shader. Material persistence is content-ID based; source files are not copied into a project asset directory.
- **Texture system:** Quality downscaling, CPU mip generation, and D3D11 sRGB RGBA8 mip upload/base-color sampling exist. GPU BC compression, non-color texture formats, thumbnail previews and GPU memory accounting are not implemented.
- **Model system:** Static triangle primitives upload to immutable D3D11 vertex/index buffers and render with per-primitive glTF scalar factors. A single node world transform per mesh is supported; multiple node instancing, morph targets, skinned meshes and animations are rejected. Models with external buffers are loaded, but content hashes cover the `.gltf`/`.glb` source only, not sidecar buffer bytes.
- **Resource management:** D3D lifetime is RAII-managed. Asset cache is process-local, with no disk cache, eviction, reload, cancellation, GPU resource cache, or streaming.
- **Performance intent:** Low-polygon diagnostics, one compact vertex buffer, VSync, and no per-frame scene allocations are used. Intel UHD hardware profiling and 720p frame pacing remain unmeasured.
- **Resize tests:** The integration test checks dimensions and resize counters. It does not simulate repeated OS resize/minimize/restore events or injected DXGI failures.
- **Cleanup test:** Renderer destruction is exercised by scope and host-window survival is checked. COM live-object reporting is not enabled/verified.
- **Editor:** The import/assignment/menu actions and viewport model path are source implemented. No build or runtime verification is available; interaction, D3D buffer correctness, file dialogs, scene reimport/relink behavior and rendering are unverified.
- **Scene parser:** It is a project-local JSON reader for scene documents, not a general-purpose public JSON library. Parser and filesystem tests have not run.

## Unimplemented

- Third-person player/gameplay, physics, animation playback, audio playback, editor project manager/terrain/map authoring, save/progression, maps/objectives, NPCs, and production art.
- Dedicated server, networking, public/private matchmaking, voice/text chat, accounts, hosted backend, and multiplayer security.
- Production-ready renderer features such as shadow maps, normal maps, occlusion/LOD, particles, terrain, and alternate backends.

## Milestone 3B completion

**Not complete.** Static model import/rendering, scene content IDs, scalar material assignment, base-color texture upload, viewport file drop, and source tests are implemented. The requested docked asset browser, material preview/texture slots, browser drag-and-drop, project asset persistence/automatic relinking, and complete reopen-without-reimport workflow are still missing. Do not start physics or multiplayer based on this milestone.

## Unverified / next verification steps

The current environment lacks a discoverable CMake/MSVC toolchain, so source compatibility and all C++ tests are still unverified. On a Windows 10/11 x64 development machine:

1. Install Visual Studio 2022 with Desktop development with C++, MSVC 19.30+, and a Windows 10 or 11 SDK.
2. Install CMake 3.24+ and ensure `cmake` is on `PATH`.
3. From the repository root, run `./Scripts/build-windows.ps1 -NoRun` in PowerShell. This configures, builds Debug and runs the CTest targets.
4. Confirm CTest reports `TogetherEngineTests`, `TogetherEngineSceneTests`, and `TogetherEngineAssetTests` passed.
5. Build and test Release with `cmake --build build/windows --config Release --parallel` followed by `ctest --test-dir build/windows -C Release --output-on-failure`.
6. Run `./build/windows/Debug/TogetherWeDo.exe`; inspect resize/minimize/restore and camera movement.
7. Run `./build/windows/Debug/TogetherEngineEditor.exe`; create/select/edit/delete a cube, Save As a `.tws.json`, close/reopen it, validate, import PNG/JPEG/TGA, WAV, valid glTF and `.twmat.json`, and inspect metadata/errors in the console.
8. Repeat renderer profiling on Intel UHD-class hardware at 1280×720 before making a compatibility claim.

No test is marked passed until these steps have actually run.
