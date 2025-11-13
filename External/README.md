# External Dependencies

The TerraEngine project integrates several third-party libraries that are downloaded at configure time via CMake's `FetchContent` mechanism. The libraries are kept out of this repository to keep the footprint small; use the links below if you need to inspect their sources.

- [RTTR v0.9.6](https://github.com/rttrorg/rttr/tree/v0.9.6)
- [EnTT v3.15.0](https://github.com/skypjack/entt/tree/v3.15.0)
- [GLM 1.0.2](https://github.com/g-truc/glm/tree/1.0.2)
- [GLFW 3.4](https://github.com/glfw/glfw/tree/3.4)
- [Vulkan Headers 1.4.328.1](https://github.com/KhronosGroup/Vulkan-Headers/tree/vulkan-sdk-1.4.328.1)

All dependencies are configured to build (or provide headers) dynamically where available when the engine is configured.
