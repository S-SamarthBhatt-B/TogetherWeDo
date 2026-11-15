# Together We Do — Project Architecture

## Repository boundaries

- `Engine/` contains reusable runtime services and the renderer. It has no game-specific map rules.
- `Engine/Scene/` owns scene documents, objects, validation, and the versioned JSON scene format.
- `Game/` contains the client shell and game-specific behavior.
- `Editor/` contains the standalone Together Engine Editor executable and native authoring workflows.
- `Server/` contains authoritative session and matchmaking services; it must not depend on the renderer.
- `Assets/` contains versioned, data-driven content. Map packages are independently loadable and validated.
- `Tools/`, `Tests/`, `Documentation/`, and `Scripts/` contain development support, tests, technical docs, and automation.

## Initial dependency direction

`Game Client -> Engine Core -> Platform/Renderer`. The dedicated server will depend on shared core, protocol, and gameplay libraries, never the client renderer. Editor tools will depend on engine/editor APIs, never on game client internals.

## First vertical slice

Milestones 1–2 establish a C++20 CMake project with a reusable engine core and a Windows D3D11 application. The diagnostic environment has a perspective free camera, depth buffer, explicit back-face culling, directional PBR-compatible lighting, fog, a procedural sky, ground, low-poly trees and rocks, and generated textures. The foundation deliberately uses Win32 and D3D11 directly to keep its dependency footprint small; window/input and renderer interfaces remain separable for later SDL3 adoption and alternate backends. The environment is diagnostic content, not final game art.

The editor uses the same renderer library in a child viewport and operates on `TogetherEngineScene` documents. Editor UI work remains isolated from game-client internals.

## Planned modules

`Engine/Core`, `Engine/Renderer`, `Engine/Graphics`, `Engine/Physics`, `Engine/Audio`, `Engine/Animation`, `Engine/Networking`, `Engine/Scene`, `Engine/Assets`, `Engine/Scripting`, `Engine/UI`, and `Engine/ECS` are added when they have usable implementations. The editor, game client, dedicated server, and matchmaking service build as separate targets.

## Data and authority

Map metadata and editable configuration use versioned JSON. SQLite is reserved for local profile/settings/progression data. Network gameplay decisions are server-authoritative; client-submitted state is input, never proof of objective completion.
