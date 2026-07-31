# Jolt Physics dependency

This page records the approved Jolt Physics dependency boundary for TerraEngine. implementation check passed its Windows, Linux, float, double, sanitizer, determinism, lifecycle, query-bound, dependency-closure, and performance gates. implementation check then added the pristine approved submodule.

## Approved source and rationale

- **Project:** Jolt Physics
- **Version/tag:** `5.6.0` / `v5.6.0`
- **Exact commit:** `e77f175595e64cb44218cc9d9d56fc365ad0e36a`
- **Approved repository location:** `External/JoltPhysics`
- **License:** MIT (copyright Jorrit Rouwe, 2021; see the upstream `LICENSE` file)

Jolt is the conditional provider because its job-system and barrier interfaces can run on TerraEngine's shared `ThreadPool`, it provides the required rigid-body and query capabilities, and its focused source tree is MIT-licensed. Public engine, scene, scripting, automation, and export contracts must remain provider-neutral; Jolt types and layouts stay inside the backend adapter.

The permanent dependency is the pristine submodule at `External/JoltPhysics`. Disposable evidence checkouts remain under `out/physics-spike/sources`; they are not build inputs. Jolt 5.5 results are retained only as comparison evidence and Jolt 5.5 source is not added to the project.

## Core-only build profile

TerraEngine consumes the Jolt library target only. The following options describe the intended core profile in Jolt's `Build/CMakeLists.txt`:

```text
-DJPH_BUILD_SHARED_LIBS=OFF
-DENABLE_INSTALL=OFF
-DTARGET_UNIT_TESTS=OFF
-DTARGET_HELLO_WORLD=OFF
-DTARGET_PERFORMANCE_TEST=OFF
-DTARGET_SAMPLES=OFF
-DTARGET_VIEWER=OFF
-DENABLE_OBJECT_STREAM=OFF
-DJPH_USE_DX12=OFF
-DJPH_USE_VK=OFF
-DJPH_USE_MTL=OFF
-DJPH_USE_CPU_COMPUTE=OFF
-DJPH_USE_EXTERNAL_PROFILE=OFF
-DDISABLE_CUSTOM_ALLOCATOR=OFF
-DDEBUG_RENDERER_IN_DEBUG_AND_RELEASE=OFF
-DDEBUG_RENDERER_IN_DISTRIBUTION=OFF
-DPROFILER_IN_DEBUG_AND_RELEASE=OFF
-DPROFILER_IN_DISTRIBUTION=OFF
-DOVERRIDE_CXX_FLAGS=OFF
-DENABLE_ALL_WARNINGS=OFF
```

`TARGET_*` switches are only created when Jolt is configured as the top-level project; the TerraEngine integration must link the core `Jolt` target and must not add Jolt samples, viewer, tests, or performance tools. `ENABLE_OBJECT_STREAM=OFF` excludes the optional example/RTTI serialization surface. All four Jolt 5.6 compute backends are disabled because GPU hair/compute is outside this feature and the Vulkan/DX12/Metal integrations would broaden the self-contained dependency and runtime surface. The embedded dependency keeps Jolt's own install rules disabled because TerraEngine owns its installed runtime, headers, exports, and third-party notices. The disposable standalone spike uses `ENABLE_INSTALL=ON` and enables the Release debug renderer solely to audit the upstream package and exercise actual debug callbacks; those spike choices are not the production profile. Precision, SIMD, assertions, and ABI choices must remain consistent between the library and adapter.

Jolt work is lowered through engine-owned adapters:

- **Jobs/barriers:** implement Jolt's `JobSystem` and barrier interfaces on TerraEngine's scheduler/`ThreadPool`; no recurring provider-private worker pool.
- **Allocation:** keep the custom allocator enabled and route allocation policy/telemetry through an engine-owned adapter. Do not call live engine storage from worker callbacks.
- **Logging:** route Jolt diagnostics through an engine-owned logger with world/tick/correlation context.
- **Profiling:** keep provider profiling disabled for normal distribution builds unless a planned diagnostic configuration enables it; expose measurements through an engine-owned profiler adapter.

These adapters and all CMake choices belong in TerraEngine-owned files. **Do not modify source files under `External/JoltPhysics`.**

## MIT redistribution and installation obligations

The MIT license permits use, modification, and redistribution, provided that the copyright notice and permission notice are included in all copies or substantial portions of the software. Any source or binary redistribution of TerraEngine that includes Jolt must therefore ship the unmodified Jolt `LICENSE` text in the corresponding third-party notices/license bundle. Preserve attribution in source archives and installed documentation.

Jolt's standalone `ENABLE_INSTALL=ON` profile installs the `Jolt` library, public headers, and CMake package export, which the disposable spike audits. The embedded TerraEngine build uses `ENABLE_INSTALL=OFF`; TerraEngine's own install/package rules must include only the core runtime artifacts and required license/notice text. They must exclude Jolt samples, viewer, tests, source-only spike material, and unrelated tools. implementation check verifies the resulting installed tree before implementation stage closes.

## Upgrade and source-integrity policy

The pin is exact: changing the tag, commit, patches, provider, build surface, or source contents requires renewed planning and explicit approval. A future Jolt upgrade beyond 5.6.0 must repeat the provider comparison/spike, record fresh evidence, update this document, and obtain approval before implementation. Never silently substitute a downloaded package or a system-installed Jolt copy.

## Reproducible verification

Run these read-only checks against the permanent submodule:

```powershell
$src = "External/JoltPhysics"
git -C $src rev-parse HEAD
Get-Content "$src/LICENSE"
rg -n "project\(JoltPhysics VERSION|JPH_BUILD_SHARED_LIBS|ENABLE_INSTALL|TARGET_UNIT_TESTS|TARGET_HELLO_WORLD|TARGET_PERFORMANCE_TEST|TARGET_SAMPLES|TARGET_VIEWER|ENABLE_OBJECT_STREAM|DISABLE_CUSTOM_ALLOCATOR|JPH_USE_EXTERNAL_PROFILE|JPH_USE_CPU_COMPUTE" "$src/Build/CMakeLists.txt"
```

The first command must print `e77f175595e64cb44218cc9d9d56fc365ad0e36a`; the license check must contain the MIT copyright, permission, and warranty clauses; and the CMake search must show the options documented above. The planning archive retains the complete measured backend-selection record.
