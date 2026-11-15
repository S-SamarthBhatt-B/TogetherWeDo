# Together Engine Asset Pipeline (Milestone 3B status)

## Status and supported inputs

`TogetherEngineAssets` is a C++20 library used by the Windows editor. It identifies content by a type-qualified SHA-256 digest and keeps a process-local registry, stable index/generation handles, and decoded payload cache. Re-importing identical content returns the existing handle. `import_async` queues work on a worker thread and reports errors through `AssetImportResult`.

| Input | Current behavior | Limits / notes |
|---|---|---|
| PNG, JPEG, TGA | Decode to RGBA8, generate a CPU box-filter mip chain, retain source dimensions/channels/format metadata | 64 MiB file, 16 MP image; quality setting downsizes to 512/1024/2048/original maximum dimension |
| glTF 2.0 `.gltf` / `.glb` | Structural validation, triangle vertex/index extraction, bounds, supported node world transform, primitive PBR factors, texture URI references, and D3D11 vertex/index buffer upload | 128 MiB model; external buffers are read by cgltf; external/embedded images are not automatically imported; multiple nodes instancing one mesh, morph targets, non-triangle topology, skinned meshes, animations and skin deformation are rejected |
| WAV | Decode PCM frames to interleaved float samples | 32 MiB file, 1–8 channels, 8–384 kHz, 16 Mi sample limit; playback is not implemented |
| `.twmat.json` | Version 1 PBR material JSON load/save with field validation | Base color, metallic, roughness, emission and base-color texture are rendered; normal, metallic-roughness and AO maps are data-only |

## Texture memory and GPU path

Decoded texture memory is reported as RGBA8 bytes including generated CPU box-filter mip levels. The base image and mip chain are retained by the asset cache and uploaded to D3D11 as an immutable sRGB texture. Texture quality defaults to Medium (1024 maximum dimension) to constrain working memory on 4 GB systems. Original quality can use up to the 16 MP import ceiling and should be selected deliberately. The process cache rejects additional assets if decoded payloads exceed 768 MiB. GPU texture memory is not measured.

GPU block compression is not performed. Texture uploads currently use RGBA8; no BC encoder or format-selection fallback exists. All imported images are uploaded as sRGB and are currently intended for base color. Normal, roughness, metallic, occlusion, and emission texture slots can be serialized but their shader sampling is not implemented.

## Material and model behavior

Materials serialize base color, metallic, roughness, emission and texture asset IDs. The renderer evaluates those scalar values for static model primitives and supports base-color texture sampling, metallic/roughness factors and emission. The editor offers create, save-as/rename, assign-latest-material, assign-latest-texture, and remove-assignment commands; its existing object inspector edits scalar values, and edits propagate to objects carrying the same material asset ID. Textures, models and materials are registered asynchronously. Importing a model creates a scene instance; dropping a `.gltf` or `.glb` file onto the viewport also queues import. The existing procedural diagnostic scene and scene object rendering remain in place.

The asset listing is currently printed into the editor console. A docked asset browser with directory tree, grid/list modes, filtering/search, asset metadata panel, thumbnails, in-browser rename/delete, and real texture/model/material preview is not implemented. Dragging models from the operating system into the viewport works; dragging material or texture assets from an engine browser does not exist. Material assets are saved as separate `.twmat.json` files; saving edits uses Save/Rename As to a new file and replaces matching scene references after the asynchronous import completes. Removing an asset invalidates the process registry/cache and releases renderer buffers/views, but preserves the source file on disk.

Scene files preserve model/material/texture content IDs. The registry is process-local; on reopening a scene, missing references are reported and remain serialized. Import the referenced source files again to resolve them. Source paths are not embedded in the scene or copied into a project asset directory. A base-color texture will fall back to untextured material output if its ID is missing.

glTF PBR texture URIs are retained as source strings, not converted to asset IDs or automatically loaded. Per-primitive glTF factors render; base-color map sampling requires importing that image separately and assigning it through the editor's latest-texture command. Normal maps, metallic-roughness maps, occlusion maps, alpha blending/masking and double-sided materials are not rendered. Primitive factors default safely when material data is absent.

Skeletal meshes and animated glTF documents are rejected with a clear static-only import error. The importer does not claim skeletal animation support.

## Format and ownership details

The asset ID is `type:sha256(source-file-bytes)`, so different asset classes do not collide. For glTF files, external buffer contents are not incorporated into that source-file digest; modifying a sidecar buffer while keeping the `.gltf` unchanged can therefore produce a stale duplicate identity. Handles contain a registry slot and generation. Registries/caches are currently in-memory for the process lifetime; no disk cache, eviction, hot reload, or cancellation is implemented. Payloads are immutable shared objects after registration. Import dependencies are pinned and included in `ThirdParty/THIRD_PARTY_NOTICES.md`.

## Tests and verification

`TogetherEngineAssetTests` covers a tiny TGA's metadata/decode/mip chain, material JSON round trip and validation, handle resolution/stale handles, duplicate registration/import, registry/cache invalidation, unsupported types, invalid texture data, invalid glTF rejection, a valid embedded-buffer triangle glTF import, and decoded cache accounting. `TogetherEngineSceneTests` covers shared asset reference round trips. These test sources have not run: this environment does not have CMake, MSVC, or CTest available. The D3D11 model-buffer path also has not run. Follow the Windows steps in `ENGINE_STATUS.md` before treating the implementation as verified.
