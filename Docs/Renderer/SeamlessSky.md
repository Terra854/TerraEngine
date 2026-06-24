# Seamless Sky

This document describes the scene-owned seamless sky workflow used by TerraEngine runtime and editor scene views.

## Overview

- The sky is stored per scene on an object through `SkyComponent`.
- The renderer draws the sky as an infinite background pass instead of a visible world-space cube.
- New scenes receive default sky values automatically.
- The sky direction comes from a directional `LightComponent` on the same object as the active `SkyComponent`.
- Sky brightness comes from that same directional light's `intensity`.

## Scene Data

The scene serializer persists sky state on a scene object through `SkyComponent`:

```json
{
  "objects": [
    {
      "name": "Sky",
      "components": {
        "SkyComponent": {
          "active": true,
          "sourceAsset": "Assets/Textures/Sky/SunsetSky.hdr",
          "exposure": 1.0,
          "rotationOffsetDegrees": 0.0,
          "horizonTint": [0.92, 0.96, 1.0],
          "zenithTint": [0.2, 0.38, 0.8],
          "groundTint": [0.1, 0.12, 0.18]
        },
        "LightComponent": {
          "type": 0,
          "unit": 1,
          "color": [1.0, 1.0, 1.0],
          "intensity": 100000.0,
          "castsShadows": false
        }
      }
    }
  ]
}
```

Scenes without an active `SkyComponent` use the renderer's no-sky fallback.

## Editor Authoring

Add or edit `SkyComponent` on a scene object through the inspector. Use the
component active checkbox to include or exclude that object from active sky
selection.

- `Enable Scene Sky` toggles whether the scene should use the authored sky source.
- `Sky Source` accepts a typed project-relative asset path or a dragged texture asset.
- `Exposure` and `Rotation Offset` shape the visible background.
- `Horizon Tint`, `Zenith Tint`, and `Ground Tint` adjust the fallback/procedural grading and the textured sky tinting.
- Add a directional `LightComponent` to the same object when the sky should render from an authored sun direction and brightness.

Sky edits dirty the active scene immediately so standard scene save/reload behavior preserves the current configuration.

## Runtime Behavior

- The renderer derives transient `SkyRuntimeState` from the active `SkyComponent` every frame.
- If `sourceAsset` resolves to a valid texture, the sky background samples that texture as an equirectangular environment.
- If the scene has no valid sky texture, the renderer falls back to a procedural gradient background and keeps the scene usable.
- The sun direction comes from the active sky object's own directional `LightComponent`. If that object has no directional light, the background silently falls back to black.
- The sky intensity submitted to the background pass is the active sky object's directional light intensity.

The background pass ignores camera translation so the sky remains visually distant during movement.

## Diagnostics And Recovery

When sky setup is incomplete or invalid, the engine does not stop rendering.

- Missing sky texture: runtime falls back and logs a sky warning once per distinct issue.
- Missing or non-directional sky-object light: runtime falls back to black.
- Editor feedback: the `SkyComponent` inspector path and the scene view surface the current fallback reason so the author can recover quickly.

## Shader Outputs

The install step compiles the sky shaders into the editor build tree:

- `out/build_gemini/Build/Debug/Editor/Shaders/Sky/SkyBackground.vert.spv`
- `out/build_gemini/Build/Debug/Editor/Shaders/Sky/SkyBackground.frag.spv`

These shader files are required for the seamless background pass and should be verified after each install-capable build.
