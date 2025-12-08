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
- [nlohmann/json (snapshot)](https://github.com/nlohmann/json) — header-only JSON parser/serializer used for authored `.tescene` and `.teprefab` validation.
- [stb (commit f1c79c0)](https://github.com/nothings/stb/tree/f1c79c0) — header-only texture decoding helpers shipped alongside the engine; pinned to the latest master commit at update time.
- [tinyobjloader (commit a4e519b)](https://github.com/tinyobjloader/tinyobjloader/tree/a4e519b) — header-only OBJ mesh loader kept vendored for offline asset import; pinned to the latest release-branch commit at update time.

The engine links RTTR and GLFW dynamically and consumes the other projects as header-only dependencies.
