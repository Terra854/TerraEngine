# External Dependencies

Initialize TerraEngine's source dependencies before configuring the build.

```bash
git submodule update --init --recursive
```

The parent repository's gitlinks pin exact source revisions. Dear ImGui follows
the recursive cimgui pin because the generated native and C# bindings share
that ABI. The .NET runtime source follows the servicing release used by the
SDK and the offline runtime packs.

| Library | Pinned commit |
|---|---|
| Hexa.NET.ImGui | [`0259312506`](https://github.com/HexaEngine/Hexa.NET.ImGui/tree/02593125062b94b24bf8b8f6c162fa54301f89cd) |
| ImGuiFileDialog | [`d0e97b2adc`](https://github.com/aiekick/ImGuiFileDialog/tree/d0e97b2adc3d3452d72c750c7305dc0291acd052) |
| ImGuizmo | [`5b473bc304`](https://github.com/CedricGuillemet/ImGuizmo/tree/5b473bc30438dc128520b90b688745bca85c4a91) |
| JoltPhysics | [`cb1a624491`](https://github.com/jrouwe/JoltPhysics/tree/cb1a6244912555d12b580265ce7540de262eab44) |
| VMA | [`0f1a757da5`](https://github.com/GPUOpen-LibrariesAndSDKs/VulkanMemoryAllocator/tree/0f1a757da579831283413295cc7f3b47eb521c77) |
| Vulkan-Headers | [`d277a0a3cd`](https://github.com/KhronosGroup/Vulkan-Headers/tree/d277a0a3cda9c682708c93279f0c72cd6b0ae3f3) |
| assimp | [`93fe5d82fb`](https://github.com/assimp/assimp/tree/93fe5d82fb206e850a21809bf45800ae00b17fc7) |
| basisu | [`073eb6a0a3`](https://github.com/BinomialLLC/basis_universal/tree/073eb6a0a34a23d77e75e54f41dcd9653a52da83) |
| cimgui | [`576040c4bd`](https://github.com/JunaMeinhold/cimgui/tree/576040c4bd36894113d1c1c6ae52c4ffd928cf28) |
| dotnet/runtime | [`a3788b7149`](https://github.com/dotnet/runtime/tree/a3788b71495457354b64ca8040b6ded5ff6ca4f9) |
| entt | [`1dcd7aab65`](https://github.com/skypjack/entt/tree/1dcd7aab6548786f7672ff578639188bd2f0d837) |
| fastgltf | [`ac06896bf9`](https://github.com/spnda/fastgltf/tree/ac06896bf98a26a3ce681bb4a07bb9c4c2f3cc3a) |
| glfw | [`92dcf4ce74`](https://github.com/glfw/glfw/tree/92dcf4ce74f2e2554a98fea09be7c705c17daa5a) |
| glm | [`6f14f4792a`](https://github.com/g-truc/glm/tree/6f14f4792a0cde5d0cf2c910506724d61cb95834) |
| json | [`322063822f`](https://github.com/nlohmann/json/tree/322063822f66533bb81ef8e938fd07e5f6394917) |
| meshoptimizer | [`8547fc321b`](https://github.com/zeux/meshoptimizer/tree/8547fc321b782a2235dfc14c65a346a640544cd8) |
| mikktspace | [`3e895b49d0`](https://github.com/mmikk/MikkTSpace/tree/3e895b49d05ea07e4c2133156cfa94369e19e409) |
| miniaudio | [`877ea4a7c2`](https://github.com/mackron/miniaudio/tree/877ea4a7c2601c7b0f2a44e83dfac42e6470fea6) |
| pugixml | [`b6fecd916b`](https://github.com/zeux/pugixml/tree/b6fecd916b5907edcb830b4f392f230e16d07fe1) |
| recastnavigation | [`9f4ce64458`](https://github.com/recastnavigation/recastnavigation/tree/9f4ce64458dfae86e1239c525ddc219c4e9e06f1) |
| simdjson | [`b26f71a0f9`](https://github.com/simdjson/simdjson/tree/b26f71a0f9144360e586d05810f007924f0ffb6b) |
| stb | [`6b5799bae8`](https://github.com/nothings/stb/tree/6b5799bae8a1fe3792691ef4b72ae26434c3e799) |
| volk | [`653b2bfd11`](https://github.com/zeux/volk/tree/653b2bfd1142df0f00f921eb2c1ae9943092340a) |
| xxhash | [`2ec67caeb1`](https://github.com/Cyan4973/xxHash/tree/2ec67caeb18ec15ae60c0853174331089acddfc0) |
| yoga | [`8d20739cf6`](https://github.com/facebook/yoga/tree/8d20739cf68e4fbb31139ccc1823b3ee002443b9) |
| zstd | [`bbcc6389c3`](https://github.com/facebook/zstd/tree/bbcc6389c3403614ee357d1bb2e2863a7f8a3502) |

[simdutf 9.3.3](https://github.com/simdutf/simdutf/releases/tag/v9.3.3) is
vendored as the SHA-256-verified release amalgamation in `External/simdutf`.
fastgltf uses this static library for text validation and Base64 decoding.

The Windows and Linux [DXC v1.9.2609](https://github.com/microsoft/DirectXShaderCompiler/releases/tag/v1.9.2609)
redistributables live in `External/dxc`. TerraEngine uses DXC for SPIR-V.

The `.NET 10.0.404` SDK and the offline NuGet feed are restored from
hash-pinned manifests. The runtime and NativeAOT packs use `10.0.15`, and
the export generator uses Roslyn `5.9.0`, matching the compiler API in the
bundled SDK. See [Testing](../Docs/Testing.md) for build and offline restore
commands.
