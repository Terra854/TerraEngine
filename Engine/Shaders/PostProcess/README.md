# Post-Process Shader Inventory

The post-process shader set owns frame-wide compute passes that virtual geometry
can consume through validated renderer state.

- `HiZGenerate.comp.hlsl`: builds the depth pyramid. Virtual geometry may keep
  this pyramid warm, but meshlet occlusion must fail open unless the matching
  view history, target size, projection, and validation state are safe.
- `GTAO.comp.hlsl` and `GTAOBlur.comp.hlsl`: ambient occlusion passes.
- `VRSGenerate.comp.hlsl`: variable-rate shading image generation.

Hi-Z changes for virtual geometry must keep occlusion decisions conservative and
must expose invalidation reasons through renderer diagnostics.
