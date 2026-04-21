# TerraEngine

TerraEngine is a modern C++20 game engine built with Vulkan. It is designed with a focus on modularity, performance (using a task-based thread pool), and a data-driven architecture. The engine is built as a shared library (`TerraEngine-Engine`) which is linked by a Launcher (`TerraEngine-Launcher`) and various offline asset compilation tools.

## Key Technologies
*   **Language:** C++20
*   **Build System:** CMake (3.22.1+)
*   **Graphics:** Vulkan (via `volk`, `VMA`, `Vulkan-Headers`)
*   **Windowing/Input:** GLFW
*   **Math:** GLM
*   **ECS:** EnTT
*   **Reflection:** EnTT Meta (`entt::meta`)
*   **Serialization:** nlohmann/json
*   **Shader Compilation:** DXC (DirectX Shader Compiler)

## Architecture Overview

The project is structured into three main components:

1.  **Engine (`Engine/`)**: The core shared library containing all runtime systems.
    *   **Core**: Main loop, object management, logging, system base classes.
    *   **Renderer**: Vulkan-based rendering backend.
    *   **Resource**: Managers for loading and caching meshes, textures, shaders, and materials.
    *   **Scene**: Handling of `.tescene` and `.teprefab` files (JSON format).
    *   **ThreadPool**: Task-based concurrency system.
2.  **Launcher (`Launcher/`)**: A thin executable wrapper that initializes and runs the engine.
3.  **Editor Tools (`Editor/`)**: Standalone CLI tools for compiling assets.
    *   `TerraEngineMeshCompiler`
    *   `TerraEngineTextureCompiler`
    *   `TerraEngineShaderCompiler`

## Building and Running

### Prerequisites
*   CMake 3.22.1 or newer
*   C++20 compliant compiler (MSVC, Clang, GCC)
*   Vulkan SDK (implicitly required by generic Vulkan usage, though headers are bundled)

### Build Commands
The project **requires** the install step to be executed to function correctly, as it relies on a specific directory structure for runtime assets and binaries (`Build/<Config>/`).

**1. Configure:**
By default, the project is configured for **Debug** builds. This is the recommended mode for development.
```bash
cmake -S . -B out/build_gemini
```

**2. Build & Install (Debug - Default):**
Run the install target to populate the `Build/Debug/` directory.
```bash
cmake --build out/build_gemini --config Debug --target install
```

**Alternative Build Configurations:**
You can build other configurations using the same build directory.

```bash
# Release (Optimized, No Debug Info)
cmake --build out/build_gemini --config Release --target install

# RelWithDebInfo (Optimized + Debug Symbols)
cmake --build out/build_gemini --config RelWithDebInfo --target install

# MinSizeRel (Optimized for Size)
cmake --build out/build_gemini --config MinSizeRel --target install
```

### Running
After installation, the executable will be located in the configuration-specific installation directory.
```bash
# Debug (Default)
./out/build_gemini/Build/Debug/TerraEngine-Launcher.exe

# Alternatives
./out/build_gemini/Build/Release/TerraEngine-Launcher.exe
./out/build_gemini/Build/RelWithDebInfo/TerraEngine-Launcher.exe
./out/build_gemini/Build/MinSizeRel/TerraEngine-Launcher.exe
```
## Development Conventions

Refer to `AGENTS.md` for strict guidelines. Key points include:

*   **Dependency Management:** All dependencies are vendored in `External/`. Do not rely on system-installed libraries.
*   **Shader Management:** Shaders must be kept in separate files within `Engine/Shaders/`, never embedded in C++.
*   **Component System:** Components must use the CRTP pattern inheriting from `Component<Derived>`.
*   **Threading:** Use the `ThreadPool` for async tasks; avoid single-threaded bottlenecks.
*   **Code Style:**
    *   Modern C++20 (RAII, smart pointers).
    *   **Indentation:** Single tab.
    *   **Comments:** Focus on explaining *why* and *intent*.
*   **Build Hygiene:** Always ensure `cmake --install` is run after code changes, especially when moving files or changing build logic.

## Documentation
*   `Docs/`: Detailed documentation on file formats (`.tescene`, `.teprefab`, etc.).
*   `AGENTS.md`: Contributor and AI agent guide.
