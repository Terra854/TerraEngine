# Quickstart: Per-View Buffer Snapshots

## Configure

From `C:\Users\benti\Desktop\TerraEngine`:

```powershell
cmake -S . -B out/build/render-snapshots -DCMAKE_BUILD_TYPE=Debug -DCMAKE_INSTALL_PREFIX=C:/Users/benti/Desktop/TerraEngine/out/build/render-snapshots/install
```

## Build And Install

```powershell
cmake --build C:\Users\benti\Desktop\TerraEngine\out\build\render-snapshots --config Debug --target install
```

## Run Snapshot Validation

```powershell
C:\Users\benti\Desktop\TerraEngine\out\build\render-snapshots\Build\Tools\RenderSnapshotValidation\RenderSnapshotValidation.exe
```

Expected result:

```text
Render snapshot validation passed.
```

## Manual Editor Verification

1. Open the editor with both Scene and Game views visible.
2. Use different camera positions for Scene and Game views.
3. Add at least one shadow-casting directional light and several visible mesh objects.
4. Move the Game camera while keeping the Scene camera stationary.
5. Confirm Scene view lighting, shadows, culling, and visible objects remain stable except for intentional scene changes.
6. Confirm diagnostics report separate Scene and Game snapshot owners.
7. Confirm diagnostics report no broad fallback synchronization during warmed-up normal dual-view frames.

## Future Multi-Camera Verification

1. Run the validation scene that includes a primary camera output and a synthetic offscreen camera output.
2. Confirm both outputs render in one frame with different camera positions or target sizes.
3. Confirm diagnostics report independent snapshot owners for both outputs.
4. Confirm lighting, clustered-lighting, shadow assignment, and indirect draw isolation checks pass for both outputs.
5. Reduce the configured snapshot budget in the validation mode.
6. Confirm required outputs fall back safely and optional outputs are skipped only when diagnostics report the reason.

## Completion Criteria

- Build and install succeeds.
- Snapshot validation executable passes.
- Manual dual-view editor verification passes.
- Future multi-camera validation path reports independent snapshot ownership.

## Validation Results

- 2026-04-24: Configured `out/build/render-snapshots` with the Debug preset command above.
- 2026-04-24: Ran `cmake --build C:\Users\benti\Desktop\TerraEngine\out\build\render-snapshots --config Debug --target install`; build and install completed successfully.
- 2026-04-24: Ran `C:\Users\benti\Desktop\TerraEngine\out\build\render-snapshots\Build\Tools\RenderSnapshotValidation\RenderSnapshotValidation.exe`; result was `Render snapshot validation passed.`
- 2026-04-24: Manual Scene/Game editor verification remains an interactive follow-up because it requires opening editor views and visually comparing camera-specific lighting, shadows, culling, and diagnostics.
