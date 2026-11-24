# Post-processing and graphics settings

Use `Graphics.DiscoverSettings()` to build an in-game graphics menu from the
active renderer. Each descriptor carries the value type, default, permitted
choices or range, settings scope, activation policy, and availability. Query
again after a renderer restart or camera replacement. A mode present in the C#
enum still needs to be advertised in the target's discovered options.

## Apply a game setting

This example requests FXAA when the running renderer advertises it. Call
`Graphics.ReadRequest` on subsequent updates using the returned request ID.

```csharp
using System.Linq;
using TerraEngine;

var descriptor = Graphics.DiscoverSettings().FirstOrDefault(
	setting => setting.Id == GraphicsSettingId.AntiAliasingMode);
uint fxaa = (uint)GraphicsAntiAliasingMode.FXAA;

if (descriptor is { Available: true } &&
	descriptor.Options.Any(option => option is uint value && value == fxaa))
{
	var request = Graphics.ApplySettings(new GraphicsSettingValue[]
	{
		new(GraphicsSettingId.AntiAliasingMode, fxaa)
	});
	// Keep request.RequestId and poll Graphics.ReadRequest on later updates.
}
```

An `Applied` receipt identifies the effective revision and publication frame.
Pending requests can wait for compatible images and pipelines. If a batch
fails, its previous effective values and resources continue serving the view.
Use `Graphics.ReadSettings` to show effective settings and pending requests.
Supply the last effective revision when submitting an edit that depends on
the copied snapshot. Use `ResetSettings` to return selected settings to their
defaults or inherited values.

Use `Graphics.ForCamera(camera)` for camera-scoped discovery, reads and
requests. The target represents a particular binding lifetime. Resolve it
again when a scene or camera is replaced. Camera overrides apply to the
camera's own output. A runtime Game request affects the Game defaults.

## Current settings and activation

| Area | Controls |
| --- | --- |
| Spatial anti-aliasing | Off, FXAA, SMAA and CMAA2. Choose one mode per supported view. |
| Multisample anti-aliasing | 4× MSAA requires the qualified indexed path and VRS off. Set `GraphicsSettingId.GeometryRasterMode` to `GraphicsGeometryRasterMode.Indexed` and request MSAA in the same batch after discovery. |
| Temporal anti-aliasing | Native TAA and half-extent temporal reconstruction on the indexed path with VRS off. The upscaler takes AA Off and `InternalResolutionScale` 0.5 in the same request. |
| Exposure | Legacy, fixed EV compensation and automatic metering with EV bounds and independent brighten and darken rates. |
| Bloom | Independent enable, threshold, intensity, spread and soft knee. |
| Tone mapping | Legacy, Neutral, Filmic and GT7. Set `GraphicsSettingId.ToneMappingMode` to `(uint)GraphicsToneMappingMode.GT7` to select GT7. |
| Color grading | Independent enable, temperature, tint, contrast, saturation, lift, gain, color curve and LUT. Curve and LUT assets have separate strengths. |
| Screen-space lighting | Separate GTAO, SSR and SSGI controls for qualified opaque indexed views. Screen misses retain the existing environment and indirect lighting. |
| HDR camera effects | Independently configure depth of field, motion blur, bloom lens dirt and lens flare. Depth of field exposes focus distance, focus range and blur strength. Lens dirt uses a texture mask, and lens flare has its own threshold and intensity. |
| Atmosphere | Volumetric height fog exposes density, base height, height falloff and maximum distance. Screen-space sun shafts have a separate toggle and intensity. |
| Presentation lens effects | Independent vignette, film grain, chromatic aberration and lens distortion with adjustable contribution. These run after anti-aliasing and before HUD and editor overlays. |
| Output dithering | Ordered dithering for 8-bit sRGB presentation. Runs on camera color just before HUD and editor overlays. |
| Existing renderer controls | Shadow and light budgets, IBL, LTC, indirect lighting, ambient occlusion, anisotropy, virtual geometry controls and startup options. Discover each control's activation policy and scope. |

Existing projects start with their legacy appearance. Active effects run on
the camera image before HUD and editor overlays. The editor can also preview
the raw camera image alongside the processed output. Native coverage or
reconstruction modes require a separately qualified renderer configuration.

GT7 uses the published SDR curve and ICtCp highlight chroma recovery. It
maps the 250-nit reference paper white to linear SDR white and uses the
camera's existing exposure and grading controls. Compare `Filmic` and
`GT7` with fixed exposure and identical grading settings to isolate the
tone mapper. [Polyphony Digital's SIGGRAPH 2025 presentation](https://s3.amazonaws.com/gran-turismo.com/pdi_publications/s2025_PBS_Physically_Based_Tone_Mapping_GT7.pdf)

Volumetric fog integrates a global exponential-height medium through the
visible scene depth before exposure and tone mapping. Density is in inverse
metres, base height and maximum distance are in metres, and height falloff
controls how quickly fog thins above its base. The fog uses the authored
sky horizon color. Sun shafts sample visible sky toward the on-screen
procedural sun in perspective views. Atmosphere requires a qualified
single-sample depth attachment. Use AA Off or a spatial AA mode with it.

Enable these effects through `GraphicsSettingId.VolumetricFogEnabled`,
`GraphicsSettingId.SunShaftsEnabled`, and `GraphicsSettingId.OutputDitheringEnabled`.
Each defaults to Off. Dithering is strongest at the native output extent.
Scaling an editor viewport can filter its pattern. Raw previews and dedicated
linear float captures preserve the camera values for inspection.

The Vulkan temporal path uses single-sample, opaque Default surfaces on
the indexed raster route with VRS disabled. Native TAA keeps the viewport
extent. Temporal upscaling renders Game views at half width and height and
composites the HUD at the output extent. Camera cuts and viewport resizes
restart history from phase zero. The 640 × 360 TAA cut and 800 × 450 resize
fixtures produced first images identical to fresh views at the same pose.
The native moving-edge TAA capture met continuous edge-error and shimmer
thresholds. The half-resolution Game reconstruction uses a smaller jitter
footprint and selects the depth and motion of the strongest contributing sample.
In the 120-pose moving-edge run it reduced edge error by 4.5% and shimmer by
12.1% against the bilinear half-resolution Off baseline. Separate disocclusion
tests and the managed Play and export matrix are still pending.

`Graphics.ApplyPreset` submits a preset as a request. `SavePreferences` and
`RestorePreferences` store or apply a named selection. `StageNextLaunch` queues
settings that require startup resources. Track the saved request ID after
restart to confirm its final result. `CancelStaged` cancels a queued startup
request.

## Author a camera profile and control its effects

Import a `.tepostprofile` and select it on the camera in the Inspector. A
profile can supply a settings override mask and an ordered list of
`.teposteffect` assets. Each effect refers to a separately imported fragment
shader and declares its stage, inputs, and typed parameter layout. The current
native adapter supports the source-color input. Set `settings.shaderUse` to
`post-process` in the fragment shader's importer metadata to select the
fullscreen binding contract.

Set the scene default in `sceneSettings.postProcessProfileId` using the profile
asset ID. A matching `PostProcessVolumeComponent` adds a weighted profile above
the scene default and below the camera profile. Its mask uses project layer order
starting with bit zero for Default. Runtime camera edits and restored player
settings apply after the authored layers.

For an already rendered camera, `PostProcessAuthoring.ForCamera(camera).Read()`
returns the available effect descriptors, requested values, and effective values.
Use the returned snapshot as the baseline for `SetProfile`, `ApplyCustomEffects`,
or `ResetCustomEffects`. `SetProfile` submits a scene write. `ApplyCustomEffects`
changes that camera's runtime state. Poll `PostProcessAuthoring.ReadRequest`
and wait for an effective frame before using the result. Use `WithView` to
select an exact output when a camera has multiple rendered views.

See [the installed test commands](Testing.md#post-processing) for CPU and
native Vulkan validation.
