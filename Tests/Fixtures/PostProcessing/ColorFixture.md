# Color fixture inputs and acceptance protocol

This protocol defines reproducible inputs and acceptance checks. It does not record GPU or installed-renderer acceptance results. Keep generated projects and captures outside the source tree. Use the same installed build, device, extents, material inputs and camera poses in each comparison.

## Two complementary paths

1. **Authored scene and managed controls:** materialize a disposable project using `Tests/Tools/PostProcessing/TestInstalledGraphicsSettings.ps1` conventions: clone the installed canonical `ManagedExportProject` scene, retain two separately identified CameraA/CameraB objects, attach `ColorSettingsFixture` to a third controller object, and stage the installed managed assembly and export generator. Import ordinary chart meshes using the maintained glTF conventions in `Tests/Editor/AssetPipeline/Fixtures/Gltf/one-material.gltf` and `Tests/Fixtures/ExportRuntimeBaseline/Assets/Models/Baseline.gltf`. Never substitute the native probe for this Play Mode/export test.
2. **Calibrated native HDR probe:** use the same production color shaders and recorder with uploaded linear RGBA16F inputs. Reuse `VulkanReviewFixture::CreateImage`, `UploadImage`, `Submit`, `CopyImageToReadback` and `Read`; wait for a real completed submission before decoding little-endian binary16 channels. Explicitly transition from the actual prior image layout. The current public PNG capture is insufficient for testing retained values above 1. The probe must exercise production effects, not a shader rewritten inside the test.

The existing `PostProcessFoundationFixture.cpp` provides real publication sealing, installed pipeline preparation, stage capture, transfer metadata, HUD and completed GPU timing patterns. It currently renders a sky/known clear, not calibrated chart geometry. New chart input integration is required.

## Deterministic inputs

Use 640 x 360 native internal/output extent, one sample, VRS/AA and unrelated effects Off. Top-left row origin. The validation runner must produce three RGBA16F images with alpha 1:

- Gray/HDR ramp: ten 64 x 120 cells across rows [0,120), with linear RGB values `0, 1/1024, 1/256, 0.018, 0.18, 0.5, 1, 2, 4, 16`.
- Chart: ten 64 x 120 cells across rows [120,240): red, green, blue, yellow, cyan, magenta, and low-luminance RGB-biased patches `(0.01,0.002,0.001)`, `(0.001,0.01,0.002)`, `(0.002,0.001,0.01)`, plus neutral 0.18.
- Localized emitter: rows [240,360) have background RGB 0.002; rectangle x=[304,336), y=[260,292) has RGB `(16,8,2)`. Reserve background annuli outside the emitter for bloom spread measurements.
- `neutral`, `bright` and `dark` inputs multiply every RGB value by 1, 4 and 1/16 respectively. Alpha remains unchanged.

The runner must write `inputs.json` with exact extents, layout, values, byte lengths and SHA-256 hashes. Use a ten-quad `ColorChart.gltf` ordinary material import input with separate mesh/material names, float position/normal/UV accessors and uint16 indexed triangles. Its base colors/emission are <=1: do not assume imported materials retain HDR strength without verifying the importer and actual raw pixels. Calibrated HDR values come from the raw probe until a supported authored-emission path is confirmed. Validate the generated glTF through the installed importer before image acceptance.

## Effect matrix

Change one effect at a time through the real C# API; wait for the terminal receipt and matching published snapshot before scoring. Record all effective values and compare against a captured disabled baseline.

| Case | Inputs and assertions |
| --- | --- |
| Legacy/disabled | All effects Off and legacy tone behavior. Same-build decoded output matches the legacy baseline. No disabled pass/timing is presented as active. |
| Fixed exposure | -2, 0, +2 EV; native HDR samples scale by 1/4, 1, 4 before tone and alpha remains unchanged. Include middle-gray values that distinguish single exposure from double multiplication. |
| Tone mapping | Legacy bypass, neutral and filmic independently. Nonnegative ramp remains finite and monotonic; configured highlight roll-off is measured on 1/2/4/16 inputs. Fix operator-specific reference values once the production formulas are approved. |
| Automatic exposure | Meter before bloom; bright/dark transitions respect limits and independent adaptation speeds. Repeat with bloom toggled to prove metering does not follow bloom energy. Fixed mode holds through the identical transition. |
| Grading | Identity settings first; then white balance, a known channel adjustment, curve, and LUT separately. Identity LUT/curves preserve baseline within declared precision. Known nonidentity transforms change intended patches while preserving alpha. |
| Bloom | Below/exactly/above threshold; zero and nonzero intensity; narrow/wide spread. Measure contribution outside the emitter, not only its clipped center. Off restores baseline; metering remains independent. |
| Camera isolation | A=+1 EV, B=-1 EV; change/reset only A and prove B's effective values and output remain stable. Reset inherits A's recorded underlying value. Do not destroy B as the graphics-settings fixture does. |

## Animation and reset

Drive the native probe at exactly 1/60 second per accepted rendered frame. Each sequence starts from a recreated view/history identity and a recorded reset: 120 neutral warmup frames, 120 bright frames, then 120 dark frames. Record every phase/frame and measured exposure, including frame zero after reset; score transition samples at offsets 0, 1, 15, 30, 60 and 119. Repeat with reversed bright/dark order. Split one 1/30 step into two 1/60 steps for a nontrivial adaptation integration test. Real installed playback must record its actual timestep rather than claim a fixed step it cannot enforce.

Run another camera with a different luminance sequence at the same time, then recreate only A. A must use the approved reset exposure and B must retain its own adaptation. A zero-luminance image, single bright outlier and bimodal distribution are separate histogram cases. Histogram binning, percentile/rejection rules, target gray and reset exposure must be recorded before expected values are finalized.

## Captures and comparisons

Capture Raw, Processed and Composited from the accepted camera publication. Require matching camera/view lifetime, settings revision, publication frame, target generation, extent and stage. Save raw bytes/float probe statistics plus lossless decoded PNGs and their metadata. Retain image hashes and immutable phase-tagged effective snapshots.

HDR probe comparisons use linear floating RGB with explicit absolute/relative tolerance derived from binary16 quantization and the production operator. Never score unclamped HDR retention from an SDR PNG. PNG checks expect declared source Linear or Srgb encoding and delivered Srgb: apply the display transfer exactly once; decode to display-referred linear RGB before cross-route comparison. Test the sRGB knee on both sides of 0.0031308, 0.18 gray, saturated channels and nontrivial alpha. Legacy gamma behavior remains a separate exact baseline.

Use interior cell crops with an 8-pixel inset, plus emitter/background masks fixed in the input manifest. No image registration or per-mode crop adjustment. Missing/nonfinite pixels, missing publication metadata, wrong phase or unsupported effect invalidate the case. Set explicit per-effect image tolerances after approved operator reference values; a merely visible difference is not acceptance.

Compare equivalent camera/settings in raw/processed Scene View, Game View, desktop export and native capture. Overlay a fixed output-space HUD quad RGB `(0.2,0.4,0.1)`, matching the existing native fixture. It must be absent from Raw/Processed and unchanged in Composited while exposure, grade and bloom change the camera image. Keep chart crops away from HUD coverage. Check Scene View grid/gizmos separately without contaminating chart measurements.

## Reproduction and outstanding decisions

Input generation must validate dimensions, finite binary16 data, HDR sentinel pixels and glTF accessor/index bounds, then repeat generation and compare hashes. Generation alone performs no import or GPU validation. A draft generator and validated generated inputs are retained under `out/validation/post-processing/color-draft/fixtures/`; that scratch directory is supporting evidence, not an installed prerequisite.

Before implementation, approve: tone formulas and grading domains/order; LUT layout/interpolation/import contract; auto-exposure histogram/target/limits/reset rules; bloom threshold domain/knee/spread units; a production recorder input seam for RGBA16F native tests; and tolerances/reference samples. HDR output-display/PQ/EXR transport is not claimed by this fixture. Installed authored geometry import, shaders, actual raw probe, C# control phases and editor/export parity remain part of the installed color-processing acceptance gate.

## Managed isolation controller and capture handoff

Stage only `Scripts/ColorSettingsFixture.cs` into the disposable project and bind the separate controller object's script class to `ColorSettingsFixture`. The shared project file compiles staged scripts; source inclusion alone does not attach another controller. Keep CameraA/CameraB alive and do not run the destructive graphics-settings controller concurrently.

Set `TERRAENGINE_COLOR_FIXTURE_ROOT` to a runner-owned existing directory and `TERRAENGINE_COLOR_FIXTURE_RUN_ID` to an ASCII alphanumeric/dash run identity. With neither set, the controller performs no test actions. It writes atomically replaced `color-observation.json` carrying run/boot identity, phase, request receipts, both effective snapshots and a `validationDomain` of `native-settings-only`. `passed` proves the named settings checks only; image and native HDR checks require separate runner results.

At each `Capture` phase, save matched camera images and metadata independently. Initial tags are `baseline`, `camera-a-plus-one`, `camera-b-minus-one`, `camera-a-reset` and `invalid-batch-preserved`; subsequent nonasset control tags and their `-baseline` pairs are listed below. After collecting evidence, atomically write `color-capture-ack.json` containing the exact `runId`, `bootId` and `tag` from the observation. An ACK authorizes the next setting phase; it is not an image success result. Wrong/stale ACKs are ignored. Missing capture acknowledgement or native publication fails by deadline.

## Independent control coverage

Keep these cases distinct in managed requests and capture tags. Asset cases require actual imported, packaged resources; an arbitrary UUID cannot satisfy them.

| Controls | Required capture tags and independent behavior |
| --- | --- |
| ExposureMode 67, compensation 65 | `exposure-fixed`, `exposure-automatic`, `exposure-legacy`; fix mode explicitly for EV comparisons. |
| MinEV 68 / MaxEV 69 | `exposure-limits`; verify adaptation clamps at both configured ends. |
| BrightenSpeed 70 / DarkenSpeed 71 | `exposure-brighten`, `exposure-darken`; use separate transitions and measured exposure traces. |
| ToneMappingMode 72 | `tone-legacy`, `tone-neutral`, `tone-filmic`; hold exposure and grade constant. |
| ColorGradingEnabled 73 | `grade-off`, `grade-identity`; disabled restores baseline. |
| Temperature 74 / Tint 75 | `grade-temperature`, `grade-tint`; neutral defaults are 6500 K and zero. |
| Contrast 76 / Saturation 77 | `grade-contrast`, `grade-saturation`; identity values are one. |
| Lift 78 / Gain 79 | `grade-lift`, `grade-gain`; scalar RGB identity values are zero/one. |
| ColorCurve 80 / Strength 81 | `grade-curve-identity`, `grade-curve-known`, `grade-curve-zero`; validate domain/layout and packaged resource. |
| ColorLut 82 / Strength 83 | `grade-lut-identity`, `grade-lut-known`, `grade-lut-zero`; validate import/interpolation and packaged resource. |
| BloomEnabled 84 | `bloom-off`, `bloom-on`; prove removal/restoration of contribution. |
| Threshold 85 / Intensity 86 | `bloom-threshold`, `bloom-zero-intensity`, `bloom-intensity`; hold spread constant. |
| Spread 87 / SoftKnee 88 | `bloom-spread`, `bloom-knee`; hold intensity/threshold constant. |

For each independent case restore the same recorded baseline first, validate capabilities, apply one controlled patch, wait for a matching effective publication, capture, and restore. The controller executes all 24 nonasset cases above after exposure isolation. For each case it publishes a matching `<tag>-baseline` capture checkpoint, restores the same explicit fixed-zero/neutral/grade-off/bloom-off control baseline, then applies the case patch and publishes `<tag>`. It checks effective values and full CameraB isolation. It finishes at `controls-restored`. Curve and LUT cases remain separate mandatory asset validation work: they are not included in its nonasset pass count, and no image checks are claimed by this controller.
