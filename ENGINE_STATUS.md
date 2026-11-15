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
- Scene v1 optional asset-reference fields: model/material IDs and base-color, normal, metallic-roughness and AO texture IDs. Older v1 scenes remain loadable; missing references remain saved and are reported on open.
- D3D11 static glTF model upload and indexed drawing for multiple primitives, including supported single-node transforms, per-primitive base color/roughness/metallic/emission factors, and resource release.
- Imported RGBA8 texture plus generated mip-chain upload with shared sRGB/linear GPU views; material assignment commands, scalar/emission inspector values, shared-reference scalar propagation, and viewport OS file drop for static glTF.
- Milestone 3B continuation source: persisted asset registry index under `.together/asset_registry.json`, source validation/reimport on editor startup, and D3D texture/model resource rebuild for restored assets.
- Milestone 3B continuation source: docked editor Asset Browser with directory/folder navigation, search, type filters, import/refresh, metadata, texture pixel previews, model geometry thumbnails, and software-lit material sphere previews.
- Milestone 3B continuation source: browser drag/drop for models into the viewport, materials onto the selected object, and textures into base-color/normal/metallic-roughness/AO slots.
- D3D11 shader source now samples tangent-space normal, glTF G/B metallic-roughness, and R-channel ambient occlusion textures; imports create sRGB and linear texture views, and scene v1 optional fields preserve the additional map IDs.
- Tangent extraction from glTF and tangent derivation from triangle positions/UVs are present for normal mapping.
- Authored restart-persistence, malformed-registry, PBR texture-slot serialization, scene map-reference, and additional asset tests. None have run in this environment.

## Partially implemented

- **Renderer diagnostics:** HRESULT/shader logs and CPU-side counters exist. No D3D11 InfoQueue collection, GPU timing, memory accounting, or on-screen diagnostics panel yet.
- **Asset browser:** Folder navigation, filtering/search, file list, metadata, source previews, and drag/drop are source implemented but unverified. This is a native list browser rather than a full tree/grid browser; asset rename/delete, thumbnail caching, and portable project-relative storage are not implemented.
- **Material system:** Reusable `.twmat.json` data and rendered base-color, normal, metallic-roughness and AO maps are source implemented but unverified. The material sphere is a CPU preview approximation, not the D3D renderer's exact shading path. Material edit/revert controls remain limited to the existing scalar inspector and save-as workflow.
- **Texture system:** Quality downscaling, CPU mip generation, separate sRGB/linear D3D11 views, and the four material map slots are source implemented but unverified. GPU BC compression, texture memory accounting, preview caching, and eviction are not implemented.
- **Model system:** Static triangle primitives upload to immutable D3D11 vertex/index buffers and render with per-primitive glTF scalar factors. A single node world transform per mesh is supported; multiple node instancing, morph targets, skinned meshes and animations are rejected. Models with external buffers are loaded, but content hashes cover the `.gltf`/`.glb` source only, not sidecar buffer bytes.
- **Resource management:** D3D lifetime is RAII-managed. The JSON asset index persists IDs and absolute source paths, and the editor reimports sources on startup. It does not copy assets into the project; path changes or moving the project can leave references unresolved. The decoded cache remains process-local, with no cache eviction, cancellation, or streaming.
- **Performance intent:** Low-polygon diagnostics, one compact vertex buffer, VSync, and no per-frame scene allocations are used. Intel UHD hardware profiling and 720p frame pacing remain unmeasured.
- **Resize tests:** The integration test checks dimensions and resize counters. It does not simulate repeated OS resize/minimize/restore events or injected DXGI failures.
- **Cleanup test:** Renderer destruction is exercised by scope and host-window survival is checked. COM live-object reporting is not enabled/verified.
- **Editor:** Existing import/model paths plus the new browser controls, CPU previews, browser drag/drop, registry restore, and map assignments are source implemented. No build or runtime verification is available; interaction, shader compilation, D3D resource correctness, scene restart/relink behavior, and GPU rendering are unverified.
- **Scene parser:** It is a project-local JSON reader for scene documents, not a general-purpose public JSON library. Parser and filesystem tests have not run.

## Unimplemented

- Third-person player/gameplay, physics, animation playback, audio playback, editor project manager/terrain/map authoring, save/progression, maps/objectives, NPCs, and production art.
- Dedicated server, networking, public/private matchmaking, voice/text chat, accounts, hosted backend, and multiplayer security.
- Production-ready renderer features such as shadow maps, LOD, particles, terrain, emissive texture sampling, and alternate backends.

## Milestone 3B completion

**Not complete.** The feature branch now has source implementations for the docked browser, previews, browser drag/drop, normal/metallic-roughness/AO sampling, persistent registry restore, and scene-reference re-resolution. These changes have not been built or run. The registry stores absolute paths rather than portable project-relative assets; asset rename/delete and a renderer-matched GPU material preview are still absent. Do not mark the milestone complete or start physics/multiplayer until Windows builds, tests, and editor restart/render workflows pass.

## Unverified / next verification steps

The current environment has no discoverable CMake, MSVC `cl`, MSBuild, CTest, or Visual Studio installation, so source compatibility and all C++ tests are still unverified. On a Windows 10/11 x64 development machine:

1. Install Visual Studio 2022 with Desktop development with C++, MSVC 19.30+, and a Windows 10 or 11 SDK.
2. Install CMake 3.24+ and ensure `cmake` is on `PATH`.
3. From the repository root, run `./Scripts/build-windows.ps1 -NoRun` in PowerShell. This configures, builds Debug and runs the CTest targets.
4. Confirm CTest reports `TogetherEngineTests`, `TogetherEngineSceneTests`, and `TogetherEngineAssetTests` passed.
5. Build and test Release with `cmake --build build/windows --config Release --parallel` followed by `ctest --test-dir build/windows -C Release --output-on-failure`.
6. Run `./build/windows/Debug/TogetherWeDo.exe`; inspect resize/minimize/restore and camera movement.
7. Run `./build/windows/Debug/TogetherEngineEditor.exe`; navigate directories, search/filter, preview all asset kinds, import supported files, drag models/materials/textures, assign each material texture slot, and inspect metadata/errors.
8. Save a scene with model/material/map references, close and restart the editor, reopen the scene, confirm registry reimport and GPU resource creation, then test missing, changed, and invalid source paths.
9. Repeat renderer profiling on Intel UHD-class hardware at 1280×720 before making a compatibility claim.

No test is marked passed until these steps have actually run.
