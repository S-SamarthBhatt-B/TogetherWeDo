# Together Engine Asset Pipeline

## Milestone 3B completion work (source status)

This document describes implementation present on `feature/milestone-3b-completion`. The feature branch has not been built or run on Windows in the current environment, so the editor UI, shader changes, persistence behavior, and GPU results remain **unverified**. Milestone 3B must not be called complete until the Windows verification checklist below passes.

## Import and registry

`TogetherEngineAssets` accepts PNG, JPEG, TGA, static glTF 2.0 (`.gltf` and `.glb`), WAV, and `.twmat.json`. Image/model/audio size limits and glTF validation are enforced during import. Images decode to RGBA8, are downscaled according to the import quality, and receive a CPU-generated box-filter mip chain. Static model import extracts triangle primitives, positions, normals, UVs, tangents (or derives tangents from UVs), material factors, and supported node transforms. WAV is decoded to float PCM; playback is not implemented.

Asset IDs are type-qualified SHA-256 hashes. The runtime registry uses generation-checked handles and decoded-payload caching. The editor persists the registry at `.together/asset_registry.json`; version 2 records paths relative to the project root. Successful imports are copied into `Assets/<type>/<sha256>/`. glTF files with external buffers/images retain their URI-relative layout inside that package. The project, registry, and imported sources can therefore move together to another Windows directory. The editor finds the project root by walking up from its executable, with the configured source root as a fallback.

At startup the registry validates its version, ID shape, entry structure, duplicates, path containment, file presence, and imported content. Valid sources are re-imported into runtime payloads and GPU resources are rebuilt by the editor. Missing v2 sources remain in the registry for recovery; browser Refresh retries restoration after a missing file is returned. Changed or invalid content is reported and is not silently accepted. Version 1 absolute entries migrate by importing and staging their source when it still exists. A missing legacy absolute source cannot be made portable automatically and is reported.

Importing from outside the project leaves that original file untouched and creates a project-owned copy. Rename changes the managed filename while retaining the stable content ID. Delete updates the registry and managed package transactionally, invalidates runtime cache state, and is refused while referenced by the current scene, a valid saved `.tws.json` scene, or a registered material. The editor confirms deletion first. A malformed scene or failed project scan blocks deletion conservatively. The registry file is editor-generated and ignored by Git.

## Editor Asset Browser

The native editor has a docked browser in the left panel with:

- Directory path entry, Go and Up navigation, and folder entries for browsing.
- Asset name search and filters for models, textures, materials, and audio.
- File and registered-asset rows, Refresh, Import, Rename and Delete controls. Double-clicking a supported file imports it. Rename stays inside the managed package and preserves the extension; Delete confirms and checks scene/material references.
- A metadata panel showing texture dimensions/mips/decoded bytes, model primitive/vertex/index counts, or audio channels and decoded size.
- A texture preview drawn from decoded image pixels, a mesh preview drawn from imported triangles, and a software-lit material sphere preview using material factors and the base-color texture when available.
- Browser drag-and-drop: drag a registered model into the viewport to create an instance. Drag a material onto a selected object to assign it. Drag a texture onto the viewport to assign base color; hold Shift for normal, Ctrl for metallic/roughness, or Alt for ambient occlusion.

The material preview is a CPU/GDI approximation and does not run the D3D11 material shader. Audio has metadata but no waveform or playback preview. The browser is a native directory/list interface rather than a tree/grid workspace, and preview pixels are generated on demand rather than held in a bounded thumbnail cache.

## Materials and rendered texture maps

Material files use version 1 `.twmat.json` JSON and store base color, metallic, roughness, emission, and texture asset IDs. The editor can assign base-color, normal, metallic-roughness, and AO maps using the Assets menu or browser drop actions. Scenes preserve those references as optional v1 fields (see [SceneFormat.md](Documentation/SceneFormat.md)).

The D3D11 material shader samples base color as sRGB and normal, metallic-roughness, and AO as linear UNORM textures. Imported textures are uploaded in both views so the same content ID can be used in either color space. Normal mapping uses glTF tangents or tangents derived from the triangle positions and UVs. Metallic-roughness follows glTF channel conventions (G roughness, B metallic); AO uses R. Missing/unresolved texture views fall back to the scalar material values.

Emission scalar values are rendered. Emission texture maps are serialized but not sampled. Texture color-space interpretation is selected by the editor's assignment slot; PNG/JPEG/TGA are not automatically tagged as color or data sources. BC compression, GPU memory accounting, cache eviction, LOD, alpha modes, double-sided material handling, and model thumbnail caching are not implemented.

## Static glTF support

Imported mesh primitives are uploaded to immutable D3D11 vertex/index buffers and rendered in the editor scene. The importer supports static triangle primitives, one node transform per mesh, material factors, and bounds. The editor can instantiate registered models by file drop or browser drag-and-drop. Existing model behavior was present before this continuation and has not been GPU-tested here.

External glTF buffers are resolved relative to the source `.gltf`, but the asset content ID is derived from the main `.gltf`/`.glb` file only. Embedded/external glTF images are retained as source URI metadata, not automatically assigned; import those texture files and assign them in the editor. Multiple-node mesh instancing, morph targets, alpha modes, skinned meshes, and animation playback are unsupported and should produce validation errors or remain unused.

## Verification

Tests are authored for material serialization/validation, asset registration and handle generations, duplicate detection, cache invalidation, texture metadata/mips, invalid assets, glTF geometry extraction, registry persistence/reload, malformed and path-escaping registry data, project relocation, project-relative path records, v1 registry migration, rename persistence, referenced-delete protection, physical managed-package deletion, missing-source recovery, and scene material-map/reference queries. They have **not run** on this feature branch because the current environment has no discoverable CMake, MSVC, CTest, or Visual Studio installation. GPU rendering, editor interactions, and runtime migration have likewise not been run.

On a Windows 10/11 x64 machine with Visual Studio 2022 Desktop development with C++, Windows SDK, and CMake 3.24+, run:

```powershell
cmake -S . -B build/windows -G "Visual Studio 17 2022" -A x64
cmake --build build/windows --config Debug --parallel
ctest --test-dir build/windows -C Debug --output-on-failure
cmake --build build/windows --config Release --parallel
ctest --test-dir build/windows -C Release --output-on-failure
```

Then launch `build/windows/Debug/TogetherEngineEditor.exe` and verify: folder navigation and filters; PNG, JPEG, TGA, glTF, WAV, and material import; texture/model/material previews; texture map assignment and rendered response; model browser drag/drop; scene save and close; editor restart; scene reopen and automatic registry restoration; changed/missing/invalid source warnings; and renderer cleanup. Test on an Intel UHD-class GPU before making a performance claim.
