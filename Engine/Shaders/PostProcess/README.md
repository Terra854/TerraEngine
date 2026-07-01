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

Adaptive virtual geometry validation must keep pyramid generation independent
from trust. The compute pass may keep depth history warm, but culling decisions
must still fail open for first frame, camera cut, projection change, resize,
view mismatch, shadow mismatch, thin-geometry risk, self-occlusion risk, or
explicit validation failure.
