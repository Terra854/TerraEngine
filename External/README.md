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

The engine links RTTR and GLFW dynamically and consumes the other projects as header-only dependencies.
