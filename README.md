# Together We Do

Together We Do is an original 3D online cooperative game project targeting **2–5 players**. It is being built on **Together Engine**, a custom C++ game engine with a native Win32 and Direct3D 11 foundation.

The repository currently focuses on an engine foundation, diagnostic game scene, standalone editor, scene serialization, and an early asset/material pipeline. Multiplayer networking and production game content are not implemented yet. Procedural scene geometry and generated materials are diagnostic assets, not finished game art.

## Project contents

- **TogetherEngineD3D11** — native Direct3D 11 renderer, camera, diagnostic shaders, and rendering resources.
- **TogetherEngineScene** — scene objects, transforms, validation, and JSON scene persistence.
- **TogetherEngineAssets** — early static glTF, image, WAV, and material import infrastructure.
- **TogetherEngineEditor** — standalone Win32 editor executable, separate from the game client.
- **TogetherWeDo** — Windows game client and renderer diagnostic scene.
- **Tests** — CTest targets for engine, scene, and asset components. The renderer test requires Windows and a desktop graphics device or WARP.

See [ENGINE_STATUS.md](ENGINE_STATUS.md) for implementation and verification status, [DEVELOPMENT_ROADMAP.md](DEVELOPMENT_ROADMAP.md) for planned work, and [PROJECT_ARCHITECTURE.md](PROJECT_ARCHITECTURE.md) for the current module layout.

## Requirements

- Windows 10 or Windows 11, x64.
- Visual Studio 2022 with **Desktop development with C++**, MSVC 19.30 or later, and a Windows 10/11 SDK.
- CMake 3.24 or later.

The current renderer uses Win32, Direct3D 11, DirectXMath, and D3DCompiler. Import dependencies are vendored in `ThirdParty`, and normal CMake configuration does not download packages. Review [DEPENDENCIES.md](DEPENDENCIES.md) and [ThirdParty/THIRD_PARTY_NOTICES.md](ThirdParty/THIRD_PARTY_NOTICES.md) for versions and license terms. Keep those notices with the corresponding source.

## Configure, build, and test

From PowerShell at the repository root, configure the Visual Studio 2022 x64 build, compile Debug, and run CTest:

```powershell
cmake -S . -B build/windows -G "Visual Studio 17 2022" -A x64
cmake --build build/windows --config Debug --parallel
ctest --test-dir build/windows -C Debug --output-on-failure
```

Build the Release configuration with:

```powershell
cmake --build build/windows --config Release --parallel
```

Or use the project helper script, which configures/builds Debug and runs tests by default:

```powershell
.\Scripts\build-windows.ps1 -NoRun
```

To build and launch the client or standalone editor, omit `-NoRun` and select the target:

```powershell
.\Scripts\build-windows.ps1
.\Scripts\build-windows.ps1 -Editor
```

The executables are `build\windows\Debug\TogetherWeDo.exe` and `build\windows\Debug\TogetherEngineEditor.exe`. The hidden-window renderer integration test requires an interactive Windows desktop; it tries hardware D3D11 and then WARP. Debug builds request the optional D3D11 debug layer and retry without it if Windows Graphics Tools is unavailable.

## Current editor and asset workflow

The editor supports scene open/save, hierarchy search, cube creation/deletion, transform and material scalar editing, viewport camera controls, basic selection/gizmos, validation, and asynchronous build-log display. Asset import supports PNG/JPEG/TGA textures, static glTF 2.0 models, WAV audio, and `.twmat.json` materials. The feature-branch continuation adds a docked directory/search/filter browser, texture/model/material previews, browser drag/drop, persistent source-path registry restore, and D3D11 base-color/normal/metallic-roughness/AO sampling. These additions have not been built or runtime tested. Registry paths are absolute and moving a source file requires relinking. Static glTF animation, alpha modes, and multiple-node mesh instancing are unsupported. See [ASSET_PIPELINE.md](ASSET_PIPELINE.md) and [ENGINE_STATUS.md](ENGINE_STATUS.md) for details, limitations and verification steps.

## License and third-party notices

Project source is provided without a project-wide license grant at this stage. Third-party components retain their respective licenses; see `ThirdParty/THIRD_PARTY_NOTICES.md` and `ThirdParty/nlohmann/LICENSE.MIT`. Do not remove or alter those notices when redistributing the vendored files.
