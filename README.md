# Lumin Engine

Lumin Engine is a compact Vulkan 1.3 renderer and scene sandbox built with C++20, SDL3, Slang, and dynamic rendering.

The current sandbox includes:

- A Level-owned Actor system with deferred spawn/destroy and per-frame Tick.
- Procedural height-field terrain with generated normals and height queries.
- A deferred renderer with a position/normal/albedo/motion G-buffer.
- Four-cascade directional shadow maps, SSAO, and a procedural skybox.
- Halton-jittered TAA with previous-camera and previous-model motion vectors.
- ACES tonemapping and an ImGui panel for runtime render settings.

## Layout

- `apps/sandbox`: runnable sample application.
- `include/lumin`: public engine headers.
- `src`: engine implementation.
- `assets/models`: place OBJ files here while experimenting.
- `shaders`: shader sources.
- `cmake`: project CMake helpers.
- `docs`: short architecture notes.

## Dependencies

The project uses `vcpkg.json` to declare:

- SDL3 for window creation, event handling, and Vulkan surface integration.
- GLM for math types.
- Dear ImGui for the renderer overlay.
- tinyobjloader for OBJ parsing.
- Vulkan headers and loader.

Set `VCPKG_ROOT` if vcpkg is not installed in one of the common paths checked by `CMakeLists.txt`.
The Vulkan SDK must provide `slangc` so CMake can compile the files under `shaders` to SPIR-V.

## Build

```powershell
cmake -S . -B out/build/debug -G Ninja -DCMAKE_BUILD_TYPE=Debug
cmake --build out/build/debug
```

Run the sandbox:

```powershell
.\out\build\debug\LuminEngine.exe
```

Optionally replace the default model with an OBJ:

```powershell
.\out\build\debug\LuminEngine.exe .\assets\models\your_model.obj
```

The Debug configure step also generates `out/build/debug/compile_commands.json`. The local VS Code settings pass that directory to clangd automatically. Refresh it after changing CMake files or dependencies with:

```powershell
cmake -S . -B out/build/debug -G Ninja -DCMAKE_BUILD_TYPE=Debug
```

Use `WASD`, `Space`, and `Left Ctrl` to move the camera. The ImGui panel exposes camera speed, CSM, SSAO, TAA, exposure, and sun direction.

If no OBJ path is supplied, the sandbox loads `assets/models/stanford-bunny.obj` when available, otherwise it uses a built-in cube. The scene also creates a procedural terrain Actor and a second built-in mesh.

## Tests

```powershell
ctest --test-dir out\build\debug --output-on-failure
```

The tests cover camera movement, Level/model revisions, Actor lifecycle and deferred changes, terrain generation and height sampling, and renderer batch construction.

See `docs/rendering-architecture.md` for the render-pass order, temporal history contract, and resource ownership model.
