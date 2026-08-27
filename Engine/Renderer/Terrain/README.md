# Terrain Renderer

This directory owns terrain-specific render derivation: error pyramids,
meshlets, virtual-geometry page payloads, material-control data, seam/stitch
variants, and terrain shader-facing snapshots.

## Public boundary

- Consume immutable terrain render snapshots and generation-tagged invalidation
  packets from `Engine/Terrain`.
- Publish render candidates through the existing virtual-geometry page
  streaming and protected page-publication path.
- Preserve last-known-good pages while replacement work is pending or fails.

## Private boundary

- Meshlet clustering, page packing, LOD selection, seam transitions, and
  renderer backend details remain local to this module.
- Render derivation must never become authoritative terrain storage.

Terrain shaders live under `Engine/Shaders/Terrain`; editor-only preview
shaders belong under `Editor/Shaders/Terrain`.

## Surface publication

Direct and page-backed terrain consume the same complete 1–16 enabled-layer
publication. Each layer provides Base Color, tangent-space Normal, packed ORM
(`R=AO`, `G=roughness`, `B=metallic`), and Emissive with the same contributors,
weights, factor order, and neutral fallbacks. Renderer code must not truncate,
reorder, or independently substitute the accepted layer set.

Physical mapping uses asset-local metres and remains continuous across areas
and large render-origin changes. Terrain-XZ and sign-correct triplanar paths
share the default 4 m by 4 m tile and the layer's metre offsets, rotation, and
sharpness. Control textures keep their separate area-local coordinates.

Nearest, bilinear, trilinear, and anisotropic requests normalize through the
shared sampler policy. Unsupported anisotropy reports trilinear fallback and
excess levels clamp to the device maximum. Vulkan samplers cache by normalized
key; combined descriptors cache by `(texture UUID, SamplerKey)` in the existing
1024-slot heap. All aliases for a texture update and retire safely on view/mip
replacement. Visible-area capture touches control UUIDs plus every layer's four
source UUIDs, deduplicated by UUID rather than descriptor alias.

## Page-backed LOD

The cooker consumes the exact 258×258 halo for each 256×256-cell owned area.
It covers the final row and corner, reconstructs shared boundary normals, and
builds deterministic 256/128/64/32-cell spatial nodes. Every node renders a
fixed 32×32-cell grid; base meshlets cover at most 7×7 cells (64 vertices and
98 triangles), with bounded stitch topology. Versioned, checksummed page
payloads target 64 KiB and include validated ranges, conservative error
(including quantization, descendants, and morph), parent morph endpoints,
16 stitch alternatives, dependencies, and own-ancestry fallback metadata.

Each view selects toward 1.5 pixels with hysteresis, then balances a complete,
non-overlapping cut so adjacent nodes and areas differ by no more than one LOD.
Hole boundaries remain pinned as required. A non-resident desired page resolves
only to the node's nearest resident ancestor. Upload completion is published at
the protected VG/RHI boundary before the page is render-visible.

## Immutable frame use and diagnostics

Protected capture copies one immutable all-area render generation. Worker-side
selection, balancing, demands, streaming touches, and draw packets use only
that capture; replacement or eviction cannot alter an in-flight view.
Diagnostics expose area/node counts, error and hysteresis decisions, balance
repairs, stitch masks, hole pins, desired versus resident pages, fallback depth,
payload bytes/checksum state, halo failures, upload/publication state, sampler
fallback, descriptor pressure, stale rejection, and rollback.

See the normative [surface-runtime](../../../specs/054-terrain-rendering-overhaul/contracts/terrain-surface-runtime.md)
and [LOD-publication](../../../specs/054-terrain-rendering-overhaul/contracts/terrain-lod-publication.md)
contracts.

## Focused verification

Configure, build, run the terrain/VG/RHI lanes, and install from the repository
root. Use the clean target when deliberately resetting generated outputs, then
rebuild and reinstall before collecting evidence.

```powershell
cmake --fresh --preset RelWithDebInfo
cmake --build --preset RelWithDebInfo --target TerraEngine TerraEngine-EngineTests
ctest --test-dir out/build/RelWithDebInfo -C RelWithDebInfo `
	-R "^Engine\.Terrain\.(Surface|Mapping|Sampler|PageCooker|Lod|Residency)|VirtualGeometry|ResourceStreaming" `
	--output-on-failure
cmake --build --preset RelWithDebInfo --target install
cmake --build --preset RelWithDebInfo --target clean
```

See [`Docs/Terrain.md`](../../../Docs/Terrain.md) for the full single-job suite,
installed audit, benchmark, and live seam/residency evidence.
