# Default Shader Inventory

The default shader set owns the mesh-shader virtual geometry path.

- `Default.task.hlsl`: task shader culling, selected LOD meshlet range dispatch,
  conservative fail-open behavior, and future Hi-Z gate consumption.
- `Default.mesh.hlsl`: meshlet payload decode, remap/index stream reads, and
  selected virtual geometry stream layout.
- `Default.frag.hlsl`: normal material output plus debug visualizer modes such
  as LOD, meshlet/triangle identity, bounds, culled, Hi-Z, residency, and error.

Adaptive virtual geometry will extend these files with selected-cluster ranges,
page residency/fallback state, parent-child boundary visualization, and richer
debug colors. Keep those payload fields documented beside the shader structs so
CPU mirror tests can validate layout before the renderer dispatches them.

All virtual geometry GPU logic must stay in dedicated shader files under
`Engine/Shaders/`. Do not embed shader source strings in C++ or configuration
files.

## Forward+ validation

From an x64 Visual Studio developer shell, compile and install the default
shader variants before running the renderer tests.

```powershell
cmake --preset RelWithDebInfo -DBUILD_TESTING=ON
cmake --build --preset RelWithDebInfo --target TerraEngine-EngineTests install
ctest --test-dir out/build/RelWithDebInfo -C RelWithDebInfo -R '^(Engine\.(RendererEfficiency|VirtualGeometryRenderer|VirtualGeometryShadow)|Editor\.RendererEfficiency)$' --output-on-failure
```

For a dense-light GPU comparison, keep the camera, viewport size, material and
quality settings fixed. Verify how many lights reach the same cluster with
`TERRAENGINE_CLUSTER_VALIDATION_CAPTURE=1` in a separate run. Turn off readback
during timing. Use GPU timestamps to sum the grid, depth mask, count, prefix
and fill dispatches, and time the material draws separately. Compare native
color captures with a visible, unsaturated receiver under point, spot,
directional and area lights, including shadowed lights and the LTC-off area
fallback.
