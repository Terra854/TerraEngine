# Frame publication performance tools

These scripts validate and capture comparable engine/editor measurements. They
are intentionally dependency-free PowerShell and preserve raw artifacts.

`CaptureFramePublicationPerformance.ps1 -DryRun` validates the project, fixed
scene/camera hash, one-editor invariant, manifest, installed build and timing
configuration without inventing samples.
Comparisons are fail-closed: every identity field must match. Native contracts
and the five-minute soak can be run independently of the editor.

After `cmake --install`, the native contract runner is installed at
`Tools/FramePublication/TerraEngine-FramePublicationContracts(.exe)`. Invoke it
through `RunFramePublicationContracts.ps1` so the manifest and output paths are
validated consistently. Capture requires one editor, an explicit camera hash,
30 seconds/120 frames of warmup, and five 60-second runs. Soak requires one
editor and five minutes of one-second samples. Comparison refuses mismatched
identity, incomplete run sets, or unavailable placeholder evidence.

## Reproducible verification

From a clean checkout, configure/build the RelWithDebInfo tree, run focused
publication/profiler/renderer tests, and install the exact tree used by tools:

```powershell
cmake --preset RelWithDebInfo
cmake --build --preset RelWithDebInfo --target TerraEngine-EngineTests TerraEngine-EditorTests
ctest --test-dir out/build/RelWithDebInfo -R "FramePublication|Profiler|Renderer"
cmake --build --preset RelWithDebInfo --target install
```

Validate the installed contract runner and capture inputs before live work:

```powershell
& .\Tools\FramePublication\RunFramePublicationContracts.ps1 -InstallRoot .\out\install\RelWithDebInfo -Manifest .\Tools\FramePublication\workloads.json -OutputDirectory .\out\evidence\frame-publication\contracts
& .\Tools\FramePublication\CaptureFramePublicationPerformance.ps1 -Project <project> -Scene <scene> -Manifest .\Tools\FramePublication\workloads.json -InstallRoot .\out\install\RelWithDebInfo -CameraHash <read-only-camera-hash> -OutputDirectory .\out\evidence\frame-publication\candidate -DryRun
```

Use the installed editor for five comparable 60-second captures, then run the
comparison and five-minute one-second-sample soak. Live visual validation uses
the authored camera and one editor process; disposable scenes are cleaned only
beneath `Assets/Scenes`.

Interrupted captures can resume safely after the editor is stopped. `-Resume`
reuses a scenario only when its identity, workload fields, raw summaries,
timing streams, and exactly five run metrics match the current invocation;
partial or stale scenarios are rerun in full:

```powershell
& .\Tools\FramePublication\CaptureFramePublicationPerformance.ps1 -Project <project> -Scene <scene> -Manifest .\Tools\FramePublication\workloads.json -InstallRoot .\out\install\RelWithDebInfo -EditorExecutable <installed-editor> -CameraHash <read-only-camera-hash> -OutputDirectory .\out\evidence\frame-publication\candidate -Resume
```

Live summaries label renderer statistics as `renderer-diagnostic-readback` and
PNG captures as `dedicated-offscreen-png-readback` in `diagnosticProvenance`.
Their frame IDs remain separate so offscreen output cannot be mistaken for the
source of renderer counters.
