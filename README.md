# Lumin Engine

Lumin Engine is a small Vulkan renderer scaffold using CMake and vcpkg manifest mode.

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
The Vulkan SDK must provide `slangc` so CMake can compile `shaders/blinn_phong.slang` to SPIR-V.

## Build

```powershell
cmake -S . -B out/build/debug -G Ninja -DCMAKE_BUILD_TYPE=Debug
cmake --build out/build/debug
```

Run the sandbox:

```powershell
.\out\build\debug\LuminEngine.exe .\assets\models\your_model.obj
```

The Debug configure step also generates `out/build/debug/compile_commands.json`. The local VS Code settings pass that directory to clangd automatically. Refresh it after changing CMake files or dependencies with:

```powershell
cmake -S . -B out/build/debug -G Ninja -DCMAKE_BUILD_TYPE=Debug
```

The sandbox renders OBJ geometry with a Slang-authored Blinn-Phong shader and shows an ImGui overlay for camera, light, material, and smooth-shading controls. If no OBJ path is supplied, it renders a built-in cube.
The project includes `assets/models/stanford-bunny.obj`; if no OBJ path is supplied and that file exists, the sandbox loads it by default.
See `docs/rendering-architecture.md` for the current FrameGraph, Dynamic Rendering, pipeline, shader, resource, and ImGui split.
