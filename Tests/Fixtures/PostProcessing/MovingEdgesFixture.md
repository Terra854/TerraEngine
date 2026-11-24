# Moving-edge image-quality fixture

`moving-edges-v1` defines the generic native AA image comparison described below.
The [offline scorer](../../Tools/PostProcessing/TestPostProcessImageQuality.ps1)
consumes externally collected native captures. It does not create a project,
launch an engine, implement an AA algorithm, or manufacture missing captures.

The checked-in scorer has numerical self-tests. Actual GPU captures and the
Play/export, disocclusion, invalid-motion, and final acceptance runs remain
separate validation work. A successful measured subset reports
`qualificationComplete: false` to preserve that distinction.

## Source dependencies and commands

The scorer requires Windows and PowerShell 7.4 or newer, including that
PowerShell installation's .NET runtime and `Add-Type` compiler. Its typed C#
kernels use standard .NET APIs and Windows' built-in `gdiplus.dll` PNG decoder.
There is no package download, Python requirement, managed drawing-package
dependency, or shader compilation in this utility. The only source asset is
[MovingEdgesPoses.json](MovingEdgesPoses.json), also installed under
`Tests/Fixtures/PostProcessing/`.

From an x64 Visual Studio developer shell, the repository build/install workflow
is:

```powershell
cmake --preset RelWithDebInfo -DBUILD_TESTING=ON
cmake --build --preset RelWithDebInfo --target install --parallel 2
```

From a clean checkout, run the source scorer's numerical checks in a fresh
PowerShell process. Use a fresh report path:

```powershell
pwsh -NoProfile -File Tests/Tools/PostProcessing/TestPostProcessImageQuality.ps1 `
  -SelfTest -SelfTestOutput out/validation/post-processing/quality-selftest.json
```

The same command works with the installed script path after installation. The
script finds the frozen asset through the preserved `Tests/Tools` and
`Tests/Fixtures` directory layout. Generated decoder images go beside the
explicit report in a unique work directory, or into the OS temporary directory
when no report path is supplied. The script never overwrites an existing report.

After an independent native collector has written a complete manifest, score all
required modes, or score one available mode:

```powershell
pwsh -NoProfile -File Tests/Tools/PostProcessing/TestPostProcessImageQuality.ps1 `
  -Manifest out/validation/aa-captures/manifest.json -Output out/validation/aa-score.json

pwsh -NoProfile -File Tests/Tools/PostProcessing/TestPostProcessImageQuality.ps1 `
  -Manifest out/validation/aa-captures/manifest.json -Output out/validation/aa-fxaa-score.json -Modes FXAA
```

For a multi-mode subset, preserve the PowerShell array as an array:

```powershell
pwsh -NoProfile -Command '& ./Tests/Tools/PostProcessing/TestPostProcessImageQuality.ps1 -Manifest ./out/validation/aa-captures/manifest.json -Output ./out/validation/aa-spatial-score.json -Modes @("FXAA","SMAA","CMAA2")'
```

Each invocation needs a fresh PowerShell process because `Add-Type` defines its
private test types for that process. Source and installed invocations use the
same decoder and equations. Capture-generation prerequisites include actual
supported Vulkan modes and the native collector; this change supplies neither
GPU captures nor a fallback image generator.

## Frozen authored inputs

The compact pose asset contains 265 explicit pose records, including warmup and
separate temporal sequences. Its exact UTF-8 byte SHA-256 is:

`2ab4d4557f4f9a38069d21dfc314388248c153580bbd7d5662e32190caf60787`

The scorer pins that hash. A changed pose, crop, control, or sequence requires an
explicit fixture revision and review; changing only a manifest hash cannot
retune this fixture after observing results.

| Input | Frozen value |
|---|---|
| Native comparison output | 640 × 360 |
| Simulation | Fixed 60 Hz; phase is the simulation step |
| Warmup | 64 accepted native renders, phases 0–63 |
| Continuous scored segment | 120 frames, phases 64–183 |
| Interior crop | `[48,32,592,328)`, right/bottom exclusive |
| Off reference | 2560 × 1440, sample 1, same base projection/material/exposure |
| Coordinates | Integers in 1/4096 logical 640 × 360 pixel units |
| Projection | Orthographic, 360-world-unit vertical span, constant aspect 16:9 |
| Material | Opaque, double-sided emissive radiance; no additional lighting contribution |
| Unrelated processing | Fixed EV 0, legacy tone, bloom/grade/grain/overlays off |
| Route | Indexed raster; actual mode/sample/extents recorded per frame |

Map a logical vertex `(px,py)` to world `(px-320,180-py,-1-depthRank)`.
The camera looks down negative Z from its recorded X displacement, with native
near/far planes enclosing all four depth ranks. A smaller rank is nearer. This
mapping preserves the same world geometry for the 4× reference and half-size
input; viewport extent changes only the output sampling grid. Use the renderer's
normal projection/depth convention to preserve that declared visibility order.
Triangulate each quad consistently and retain double-sided coverage.

The slanted panel supplies a large high-contrast edge. The rigid foreground rail
has 0.75 logical-pixel horizontal thickness and moves independently from the
camera. The two-joint ribbon has fixed root vertices and animated tip vertices;
its tip follows the frozen continuous triangular-wave deformation. A native
consumer must use actual object and animation/deformation previous-state
producers. It must not substitute a fabricated motion-vector texture.

For every phase after zero, previous camera/object/deformation state is the
previous accepted frozen pose. Phase zero is a genuinely new view with no valid
history. The warmup advances those same authored poses before measurement.
Do not use wall-clock animation or change the simulation step while readback is
pending. Images can be collected slower than real time; throughput is outside
this quality score.

## Separate temporal sequences

| Sequence | Authored operation | Required comparison |
|---|---|---|
| `cut` | Camera jumps by 61 logical pixels; first effective frame is sample index 0 | Fresh same-mode view at the same pose/state/extent and first jitter sample index 0 |
| `resize` | Output becomes 800 × 450, logical generation 2, crop `[60,40,740,410)` | Fresh view at 800 × 450, including 400 × 225 internal extent for temporal upscaling |
| `disocclusion` | Real foreground occluder moves from X255 to X425 at sample index 16 | Inspect newly revealed background against a matching fresh current-frame replay |
| `invalid-motion` | Retire/recreate the ribbon at its current pose at sample index 32 | Inspect the new native owner without previous motion, then observe normal history from sample index 33 |

The cut view's declared logical generation is1. Fresh cut and resize replays use
separate sequence keys `cut-fresh`/`resize-fresh`, with logical generations 1001
and 1002. Actual native sessions, view IDs, and lifetimes are always kept as
recorded; logical fixture tags never replace them.

For disocclusion, derive a fixed visibility mask from previous occluder coverage
minus current coverage. For the new ribbon, use its current visible interior.
Erode those regions by two output pixels and exclude reference edges dilated by
two pixels. Build these masks from the frozen/reference geometry, before looking
at a candidate. These temporal validity checks are separate from the edge/shimmer
mask and need native motion/history evidence. They are not silently treated as
passed by the image scorer. Matching fresh temporal replays must record the same
actual first-frame jitter phase; do not shift or warp images to manufacture it.

## Native capture manifest, version1

The external collector writes a JSON object with these required top-level fields:

| Field | Type and meaning |
|---|---|
| `schemaVersion` | Integer 1 |
| `evidenceKind` | String `NativeBackendReadback` |
| `fixtureFile` | Relative path to an exact copy of the committed pose asset inside the evidence directory |
| `fixtureSha256` | Exact frozen byte hash above |
| `runs` | Object keyed by the run names in the configuration table below |
| `resetPairs` | Array of first/fresh pairs; required for selected temporal modes |

All relative image/fixture paths remain inside the owned manifest directory.
All SHA fields are lowercase hexadecimal. Native IDs are canonical decimal
strings so full-width integers survive JSON and PowerShell. Missing fields,
synthetic/fallback flags, mismatched capture conversion, and incomplete sequences
invalidate the run. No unavailable capture is filled with a zero score.

| Run name | `effective.mode` | Sample count | Internal extent | Captured output |
|---|---|---:|---|---|
| `Reference4x` | `Off` | 1 | 2560 × 1440 | 2560 × 1440 |
| `OffNative` | `Off` | 1 | 640 × 360 | 640 × 360 |
| `OffHalf` | `Off` | 1 | 320 × 180 | 320 × 180 |
| `FXAA`, `SMAA`, `CMAA2` | Same canonical name | 1 | 640 × 360 | 640 × 360 |
| `MSAA` | `MSAA` | 4 | 640 × 360 | 640 × 360 |
| `TAA` | `TAA` | 1 | 640 × 360 | 640 × 360 |
| `TemporalUpscale` | `TemporalUpscale` | 1 | 320 × 180 | 640 × 360 |

Every run contains exactly four keys in `effective`: string `mode`, integer
`sampleCount`, two-integer `internalExtent`, and two-integer `outputExtent`.
These values describe the actual accepted configuration. `controlsSha256` equals
the frozen asset's hash of its `controls` object. The actual collector must verify
those controls; copying their labels without applying them is not valid evidence.
`Reference4x` and `OffNative` are always required. `OffHalf` is required only when
the temporal upscaler is selected.

Each run has 64 `warmupRenderReceipts` and 120 `frames`. The warmup array is in
phase order 0–63. Measured frames may arrive out of order but must contain each
sample index 64–183 exactly once. Both record types carry:

| Field | Required value |
|---|---|
| `sequence` | `continuous` |
| `phase` | Explicit integer phase |
| `viewGeneration` | Explicit logical integer generation 1 |
| `poseSha256` | Matching frozen pose record's SHA |
| `nativeRendererSession` | Original nonzero uint64 decimal string |
| `nativeViewId` | Original uint64 decimal string; physical output zero is valid |
| `nativeViewGeneration` | Original nonzero uint64 decimal string |
| `effective` | Actual four-field configuration object above |

Warmup records additionally require Boolean `nativeRenderAccepted: true`.
Measured frame records additionally require:

| Field | Required value |
|---|---|
| `nativeBackendReadback` | Boolean true |
| `deterministicFallback` | Boolean false |
| `backendFrameId` | Actual nonzero uint64 decimal string |
| `file`, `sha256` | Relative lossless image path and its byte hash |
| `extent` | Two-integer captured output extent |
| `colorSpace` | `BT709` |
| `signalDomain` | `display-referred` |
| `format`, `encoding` | `PNG8` with `srgb` or `linear`, or `RGBA32F` with `linear` |

`PNG8` means noninterlaced RGB/RGBA8 with opaque alpha. The decoder rejects ICC
profiles, conflicting gamma declarations, malformed chunks/CRCs, and translucent
pixels. It uses the non-ICM Windows decoder and explicitly maps BGRA memory to
RGB. `RGBA32F` is tightly packed little-endian float RGBA with alpha 1. Inputs must
be finite normalized display-linear RGB after the declared conversion. HDR scene
values must be resolved into the declared display-referred comparison signal
before this AA protocol; the scorer never guesses a tone or gamma conversion.

The collector must check the actual prepared camera/geometry/material state
against the frozen commands, including their native float32 representation,
before emitting `poseSha256`. That field identifies the canonical frozen pose;
it does not replace native buffer/state provenance. Preserve actual prepared-state
hashes and camera/object/component/binding identities in additional receipt
fields where the native producer exposes them. Do not hash request IDs or frame
numbers into the effective-value hash used for fresh comparisons.

Within each continuous run, one declared logical generation maps consistently to
one actual native identity. Different runs can have different native identities.
The report retains those mappings verbatim. It never overwrites native IDs to
make reference and candidate runs appear to have the same lifetime.

### Reset pair fields

For every selected `TAA` or `TemporalUpscale`, provide one pair with `reason: cut`
and one with `reason: resize`. Each pair contains `mode`, `reason`, `first`, and
`fresh`. Both image records contain the image/native fields above and:

- The exact frozen reset pose SHA, matching `simulationStateSha256` and
  `effectiveSettingsSha256` as 64-character hashes, and integer `jitterPhase: 0`.
- Actual effective mode/sample/internal/output configuration for the reset
  extent. Cut uses 640 × 360; resize uses 800 × 450.
- `first` uses the reset sequence name, sample index 0, its declared generation, and
  Boolean `firstEffectiveFrameAfterReset: true`.
- `fresh` uses the separate fresh sequence/generation described above, sample index 0,
  and integer `acceptedViewFrameIndex: 0`.

The two records need distinct actual native identities and matching pose,
simulation state, effective settings, extent, and first jitter phase. A crop
comparison is not accepted merely because two files look similar.

## Exact scoring and output

1. Decode the 4× Off reference to linear RGB, box-average each 4 × 4 footprint,
   then compute normalized BT.709 luminance `.2126 R + .7152 G + .0722 B`.
2. Inside the frozen crop, seed pixels differing from any of their four axial
   neighbors by at least 0.125. Dilate by a full 3 × 3 square clipped to the crop.
   Build this reference-only mask once per phase and reuse its immutable data.
3. Edge MAE is the global absolute-error sum over every masked measured pixel
   divided by the global masked-sample count. Per-frame means are not averaged.
4. For adjacent phases, subtract each reference before comparing signed errors.
   Average absolute error change over the union of the two reference masks.
   Desired motion is therefore removed from the shimmer score.
5. Every selected mode requires edge MAE ≤ 0.99 times its Off comparator. TAA and
   temporal upscaling also require shimmer ≤ 0.99 times that comparator. Native
   modes use `OffNative`; temporal upscaling uses `OffHalf` bilinearly enlarged
   in linear RGB with pixel-center mapping and clamped edges.
6. Reset pairs compare every crop pixel, including interiors, and require the
   maximum luminance difference to be ≤ 1/255. No registration/warping is allowed.

An empty mask, missing phase, duplicate key, changing continuous native identity,
incompatible transfer/extent, or zero required Off denominator is `Invalid`.
The report contains raw sums/sample counts, mask/reference hashes, actual native
identity mappings, comparator names, improvement ratios, reset maxima, and pass
status. `FailedMeasuredSubset` means valid measurements missed their thresholds;
`PassedMeasuredSubset` means only the selected image checks passed. Invalid and
failed results exit nonzero. All reports retain `qualificationComplete: false`
until the separate native configuration, temporal-validity, and integration gates
are evaluated by the test orchestrator.

The 17 opt-in numerical invariants cover transfer, all five PNG row filters and
orientation, linear reference filtering, pixel-center enlargement, exact masks,
global weighting, signed/union shimmer, desired-motion cancellation, invalid
phase/grid/mask/native data, denominator rejection, the exact 1% boundary,
worst-pixel resets, full-width native IDs, frozen animation, and a synthetic edge.
Their report states `nativeRendererExecuted: false`. No image-quality improvement
for a real AA implementation is claimed by those self-tests.
