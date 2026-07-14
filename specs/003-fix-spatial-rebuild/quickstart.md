# Quickstart: Fix Spatial Rebuild

## Configure

From `C:\Users\benti\Desktop\TerraEngine`:

```powershell
cmake -S . -B out/build/spatial-culling -DCMAKE_BUILD_TYPE=Debug -DCMAKE_INSTALL_PREFIX=C:/Users/benti/Desktop/TerraEngine/out/build/spatial-culling/install
```

## Build And Install

```powershell
cmake --build C:\Users\benti\Desktop\TerraEngine\out\build\spatial-culling --config Debug --target install
```

## Run Spatial Validation

```powershell
C:\Users\benti\Desktop\TerraEngine\out\build\spatial-culling\Build\Tools\SpatialValidation\SpatialValidation.exe
```

Expected result:

```text
Spatial validation passed.
```

## Manual Verification

1. Open a large scene in the editor.
2. Let several updates run without moving, adding, deleting, or changing renderable objects.
3. Confirm spatial diagnostics report `refreshMode = Unchanged` and non-zero `unchangedObjectCount` instead of repeated `refreshMode = Full`.
4. Move one renderable object into and out of a camera view.
5. Confirm only bounded incremental changes are reported through `addedObjectCount`, `updatedObjectCount`, and `removedObjectCount`, and visible-object results remain correct.
6. Load or replace the scene.
7. Confirm `fullRefreshObjectCount` is allowed at the lifecycle boundary, then stable updates return to unchanged or incremental refresh behavior.
