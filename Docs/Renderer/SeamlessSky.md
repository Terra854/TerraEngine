# Seamless Sky

This document describes the scene-owned seamless sky workflow used by TerraEngine runtime and editor scene views.

## Overview

- The sky is stored per scene in `sceneSettings.sky`.
- The renderer draws the sky as an infinite background pass instead of a visible world-space cube.
- New scenes receive default sky values automatically.
- Authors can optionally drive the sky direction from a bound directional light to support a Unity-style day/night workflow.

## Scene Data

The scene serializer persists sky state under the root `sceneSettings` block:

```json
{
  "sceneSettings": {
    "sky": {
      "enabled": true,
      "sourceAsset": "Assets/Textures/Sky/SunsetSky.hdr",
      "intensity": 1.0,
      "exposure": 1.0,
      "rotationOffsetDegrees": 0.0,
      "horizonTint": [0.92, 0.96, 1.0],
      "zenithTint": [0.2, 0.38, 0.8],
      "groundTint": [0.1, 0.12, 0.18],
      "directionalLightObjectId": "00000000000000000000000000000000"
    }
  }
}
```

Legacy scenes that do not contain `sceneSettings.sky` fall back to engine defaults at load time.

## Editor Authoring

Use the `Sky Settings` window to edit the active scene sky.

- `Enable Scene Sky` toggles whether the scene should use the authored sky source.
- `Sky Source` accepts a typed project-relative asset path or a dragged texture asset.
- `Intensity`, `Exposure`, and `Rotation Offset` shape the visible background.
- `Horizon Tint`, `Zenith Tint`, and `Ground Tint` adjust the fallback/procedural grading and the textured sky tinting.
- `Directional Light` lets the author leave the sky on automatic first-light selection or choose a specific directional-light override.

Sky edits dirty the active scene immediately so standard scene save/reload behavior preserves the current configuration.

## Runtime Behavior

- The renderer derives transient `SkyRuntimeState` from the saved scene settings every frame.
- If `sourceAsset` resolves to a valid texture, the sky background samples that texture as an equirectangular environment.
- If the scene has no valid sky texture, the renderer falls back to a procedural gradient background and keeps the scene usable.
- The sun direction always comes from a resolved directional light. When no directional light can be resolved, the background falls back to black.
- `directionalLightObjectId` acts as an optional override. When it is empty, the renderer automatically uses the first directional light found in the active scene.

The background pass ignores camera translation so the sky remains visually distant during movement.

## Diagnostics And Recovery

When sky setup is incomplete or invalid, the engine does not stop rendering.

- Missing sky texture: runtime falls back and logs a sky warning once per distinct issue.
- Missing directional light: runtime falls back to black and reports the issue.
- Invalid directional-light binding: runtime falls back to black and reports the binding issue.
- Editor feedback: `Sky Settings` and the scene view surface the current fallback reason so the author can recover quickly.

## Shader Outputs

The install step compiles the sky shaders into the editor build tree:

- `out/build_gemini/Build/Debug/Editor/Shaders/Sky/SkyBackground.vert.spv`
- `out/build_gemini/Build/Debug/Editor/Shaders/Sky/SkyBackground.frag.spv`

These shader files are required for the seamless background pass and should be verified after each install-capable build.
