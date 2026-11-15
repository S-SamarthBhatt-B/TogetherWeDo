# Dependencies

## Current build

| Component | Source | License / terms | Status |
|---|---|---|---|
| C++20 standard library and MSVC | Visual Studio 2022 | Microsoft toolchain terms | Required |
| Win32, Direct3D 11, D3DCompiler, DirectXMath | Windows SDK | Microsoft SDK terms | Required |
| CMake 3.24+ | CMake distribution | BSD 3-Clause | Required build tool |
| C++20 scene JSON reader/writer | `Engine/Scene` source | Project code | Required by editor and scene tests |
| cgltf v1.15 | `ThirdParty/cgltf/cgltf.h` | MIT | glTF 2.0 parsing and validation |
| stb_image (pinned revision) | `ThirdParty/stb/stb_image.h` | Public domain / MIT option | PNG, JPEG, TGA decoding |
| dr_wav (pinned revision) | `ThirdParty/dr_libs/dr_wav.h` | Public domain / MIT-0 | WAV decoding |
| nlohmann/json v3.12.0 | `ThirdParty/nlohmann/json.hpp` | MIT | Material JSON serialization |

No third-party source is fetched by the project configure/build. Dependency revisions, upstream links and license notices are recorded in `ThirdParty/THIRD_PARTY_NOTICES.md`. The asset target links Windows BCrypt for SHA-256 content IDs. The editor uses native Win32 common dialogs/controls. Direct3D debug-layer diagnostics are optional and require the Windows Graphics Tools feature; the application retries without that layer.

## Candidates for later milestones

Pin and document each selected version and license before integration.

| Component | Intended use | Status |
|---|---|---|
| SDL3 | Window/input abstraction | Candidate, not integrated |
| Jolt Physics | Physics and character movement | Candidate, not integrated |
| ENet | UDP transport | Candidate, not integrated |
| Dear ImGui | Editor UI widgets | Candidate, not integrated |
| miniaudio | Audio device and playback | Candidate, not integrated |
| SQLite | Local profile/settings persistence | Candidate, not integrated |
| Lua | Map scripting | Candidate, not integrated |
