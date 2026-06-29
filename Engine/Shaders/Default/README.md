# Default Shader Inventory

The default shader set owns the mesh-shader virtual geometry path.

- `Default.task.hlsl`: task shader culling, selected LOD meshlet range dispatch,
  conservative fail-open behavior, and future Hi-Z gate consumption.
- `Default.mesh.hlsl`: meshlet payload decode, remap/index stream reads, and
  selected virtual geometry stream layout.
- `Default.frag.hlsl`: normal material output plus debug visualizer modes such
  as LOD, meshlet/triangle identity, bounds, culled, Hi-Z, residency, and error.

All virtual geometry GPU logic must stay in dedicated shader files under
`Engine/Shaders/`. Do not embed shader source strings in C++ or configuration
files.
