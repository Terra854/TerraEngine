# External Dependencies

TerraEngine vendors its third-party libraries as Git submodules so the required headers and sources are available immediately after cloning the repository. Make sure to initialise the submodules before configuring the project:

```bash
git submodule update --init --recursive
```

The submodules track the following upstream releases:

- [RTTR v0.9.6](https://github.com/rttrorg/rttr/tree/v0.9.6)
- [EnTT v3.15.0](https://github.com/skypjack/entt/tree/v3.15.0)
- [GLM 1.0.2](https://github.com/g-truc/glm/tree/1.0.2)
- [GLFW 3.4](https://github.com/glfw/glfw/tree/3.4)
- [Vulkan Headers 1.4.328.1](https://github.com/KhronosGroup/Vulkan-Headers/tree/vulkan-sdk-1.4.328.1)
- [volk (latest)](https://github.com/zeux/volk) — Vulkan meta-loader to simplify extension loading and multi-platform support.
- [VMA (Vulkan Memory Allocator) v3.1.0](https://github.com/GPUOpen-LibrariesAndSDKs/VulkanMemoryAllocator) — specialized memory management for Vulkan resources.
- [nlohmann/json (snapshot)](https://github.com/nlohmann/json) — header-only JSON parser/serializer used for authored `.tescene` and `.teprefab` validation.
- [stb (commit f1c79c0)](https://github.com/nothings/stb/tree/f1c79c0) — header-only texture decoding helpers shipped alongside the engine.
- [tinyobjloader (commit a4e519b)](https://github.com/tinyobjloader/tinyobjloader) — header-only OBJ mesh loader for offline asset import.
- [meshoptimizer v0.21](https://github.com/zeux/meshoptimizer) — vertex/index optimization and meshlet generation for the asset pipeline.
- [mikktspace (commit 3f5d5e5)](https://github.com/mmikk/MikkTSpace) — industry-standard tangent space generation.
- [cimgui (ImGui v1.90.1)](https://github.com/cimgui/cimgui) — C-api for Dear ImGui used by the editor's UI layer.
- [ImGuizmo (v1.89)](https://github.com/CedricGuillemet/ImGuizmo) — 3D gizmos for the scene editor.
- [ImGuiFileDialog (v0.6.7)](https://github.com/aiekick/ImGuiFileDialog) — file dialogs for asset management.

### Binary Redistributables

The following components are provided as pre-built binaries for runtime and tool support:

- **dotnet/**: .NET 8.0/10.0 runtime redistributables for the C# scripting system.
- **dxc/**: DirectX Shader Compiler (DXC) binaries for SPIR-V shader compilation in the `TerraEngineShaderCompiler` tool.

The engine links RTTR, GLFW, and cimgui dynamically and consumes the other projects as static libraries or header-only dependencies.
