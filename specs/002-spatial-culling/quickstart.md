# Quickstart: Spatial Partitioning and Object Culling

## Prerequisites

- Windows development environment supported by the TerraEngine CMake project.
- CMake 3.22.1 or newer.
- Repository dependencies already present under `C:/Users/benti/Desktop/TerraEngine/External/`.
- No globally installed runtime dependency should be required by this feature.

## Configure

From `C:/Users/benti/Desktop/TerraEngine`:

```powershell
cmake -S . -B out/build/spatial-culling -DCMAKE_BUILD_TYPE=Debug -DCMAKE_INSTALL_PREFIX=C:/Users/benti/Desktop/TerraEngine/out/build/spatial-culling/install
```

## Build And Install

```powershell
cmake --build out/build/spatial-culling --config Debug --target install
```

The feature is not complete until the install target succeeds and the installed runtime tree is produced.

## Validate Core Behavior

Run the spatial validation target or tool added by implementation:

```powershell
cmake --build out/build/spatial-culling --config Debug --target SpatialValidation
out/build/spatial-culling/Debug/SpatialValidation.exe
```

Expected validation coverage:

- 10,000-object visibility scene.
- Area, sphere, frustum, and ray query correctness.
- Static and dynamic object membership.
- Parent-child transform updates.
- Destroyed and inactive object exclusion.
- Conservative fallback for invalid or missing bounds.

## Validate Rendering Behavior

Run an editor or runtime scene that includes the large-scene validation fixture.

Expected outcomes:

- Visible output remains correct while moving the camera.
- Candidate count drops when most objects are outside the camera-visible region.
- Spatial diagnostics report indexed object count, visible candidate count, and fallback count.
- Existing meshlet and GPU culling diagnostics continue to work downstream.

## Completion Checklist

- Configure succeeds from a clean checkout.
- Build and install succeeds with `cmake --build out/build/spatial-culling --config Debug --target install`.
- Spatial validation tool passes.
- Rendering validation scene shows no incorrectly missing visible objects.
- Documentation under `C:/Users/benti/Desktop/TerraEngine/Docs/` describes how to reproduce the validation.
