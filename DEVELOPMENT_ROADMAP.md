# Development Roadmap

Milestones are complete only after the code builds and relevant behavior is exercised on the target platform.

1. **Project foundation:** C++20/CMake layout, build scripts, architecture/dependency records, Windows client.
2. **Engine core and renderer:** diagnostics, frame timing, camera, D3D11 scene, materials, textures and render-state management.
3. **Custom editor 0.1:** native editor executable, JSON scene documents, hierarchy, viewport, transform/property editing, validation, console/build output.
4. **Asset pipeline (Milestone 3B partial source):** PNG/JPEG/TGA, WAV and static glTF import; D3D11 model buffers, base-color texture mips, scalar materials, scene asset references and viewport file-drop are present. Remaining: docked browser, previews, automatic asset relinking, broader glTF transforms/instancing, full texture-map shader support and Windows verification. This milestone is not complete.
5. **Physics and player:** Jolt integration, collision, third-person controller, camera and interaction.
6. **Networking:** protocol, authoritative dedicated server, session and player synchronization, local multi-client verification.
7. **Matchmaking:** private room codes and map-specific public matchmaking service.
8. **Communication:** validated text chat and modular low-bandwidth voice transport.
9. **Map and mission framework:** versioned packages and generic authoritative objectives.
10. **First complete map:** cooperative Lost Forest vertical slice, from introduction through extraction.
11. **Additional maps:** Abandoned House and Ancient Temple with distinct mechanics and endings.
12. **Reliability and release:** low-end profiling, security review, Windows packaging and Windows 10/11 compatibility testing.

## Current state

- Milestones 1–2 have an initial implementation: Win32/D3D11 client, camera/frame timer, procedural diagnostic scene, renderer tests, and build scripts.
- Editor 0.1 and the versioned scene model/serializer are implemented in source, including native panels, D3D viewport, CRUD, property editing, preview mode, file dialogs, and asynchronous build output.
- All new C++ source and CTest targets remain **unverified** until built and run with Visual Studio 2022/CMake on Windows. See `ENGINE_STATUS.md`.
- Milestone 3B has a source implementation for static model rendering, simple material assignment and base-color texture sampling. Asset/scene tests are authored but unrun; the editor visual workflow remains partial. See `ASSET_PIPELINE.md` for exact support and remaining gaps.
- Editor usability and behavior should be confirmed on a Windows machine before treating this milestone as complete. Multiplayer has not started.
- Milestones 5 and 6 remain gated until the Milestone 3B asset workflow is complete and verified; do not begin physics or multiplayer from this roadmap yet.
