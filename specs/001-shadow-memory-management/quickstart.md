# Quickstart: Shadow Memory Management

## Prerequisites

- Work from `C:\Users\benti\Desktop\TerraEngine`.
- Use the existing vendored dependencies under `External/`.
- Keep shader source in `Engine/Shaders/`.
- Use the installed runtime tree for manual validation.

## Configure

```powershell
cmake -S C:\Users\benti\Desktop\TerraEngine -B C:\Users\benti\Desktop\TerraEngine\Build\Debug -DCMAKE_BUILD_TYPE=Debug -DCMAKE_INSTALL_PREFIX=C:\Users\benti\Desktop\TerraEngine\Build\Debug\install
```

## Build

```powershell
cmake --build C:\Users\benti\Desktop\TerraEngine\Build\Debug --config Debug
```

## Install

```powershell
cmake --build C:\Users\benti\Desktop\TerraEngine\Build\Debug --config Debug --target install
```

If the generator does not expose an install target, use:

```powershell
cmake --install C:\Users\benti\Desktop\TerraEngine\Build\Debug --config Debug
```

The local install prefix avoids writing to `C:\Program Files (x86)` during
normal developer validation.

## Validation Scenarios

1. Configure a scene with shadow capacity 32 and zero active shadow-casting lights. Verify diagnostics report zero allocated shadow regions after unused resources are released.
2. Configure a scene with shadow capacity 32 and four active 2D shadow-casting lights. Verify memory remains proportional to the four active shadows and no point regions are allocated.
3. Configure a scene with only spot or area shadows. Verify point shadow memory remains zero.
4. Render a static scene for multiple frames. Verify diagnostics show cache reuse after warm-up.
5. Move a shadow-casting object. Verify affected cache entries invalidate and refresh on the next frame.
6. Use more shadow-eligible lights than the active budget. Verify lower-priority shadows degrade with visible diagnostics while direct lighting remains active.

## Validation Helper

The helper reads JSON captures written by the renderer's cluster validation
capture path. Use the installed or build-tree executable:

```powershell
C:\Users\benti\Desktop\TerraEngine\Build\Debug\Build\Tools\TerraEngineShadowMemoryValidation.exe --scenario capacity32-zero --capture C:\Users\benti\Desktop\TerraEngine\Build\Debug\ShadowMemoryValidationSamples\capacity32-zero.json
C:\Users\benti\Desktop\TerraEngine\Build\Debug\Build\Tools\TerraEngineShadowMemoryValidation.exe --scenario capacity32-four-2d --capture C:\Users\benti\Desktop\TerraEngine\Build\Debug\ShadowMemoryValidationSamples\capacity32-four-2d.json
C:\Users\benti\Desktop\TerraEngine\Build\Debug\Build\Tools\TerraEngineShadowMemoryValidation.exe --scenario static-cache --capture C:\Users\benti\Desktop\TerraEngine\Build\Debug\ShadowMemoryValidationSamples\static-cache.json
C:\Users\benti\Desktop\TerraEngine\Build\Debug\Build\Tools\TerraEngineShadowMemoryValidation.exe --scenario moving-invalidation --capture C:\Users\benti\Desktop\TerraEngine\Build\Debug\ShadowMemoryValidationSamples\moving-invalidation.json
C:\Users\benti\Desktop\TerraEngine\Build\Debug\Build\Tools\TerraEngineShadowMemoryValidation.exe --scenario over-budget --capture C:\Users\benti\Desktop\TerraEngine\Build\Debug\ShadowMemoryValidationSamples\over-budget.json
```

## Completion Criteria

- Build succeeds.
- Install tree is produced and verified.
- Diagnostics cover assignment, memory estimate, cache reuse, refresh, invalidation, and degradation.
- Validation scenarios demonstrate the success criteria from `spec.md`.
