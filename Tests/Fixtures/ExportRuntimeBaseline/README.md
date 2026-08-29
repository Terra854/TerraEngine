# Export Runtime Baseline Source Fixture

This project is the minimal known-good export sample for TerraEngine. It is
kept deliberately small so failures point to one of the exported runtime
contracts instead of to complex demo content.

Only authored source belongs in this directory. Never check in `Build/`,
`Builds/`, `obj/`, generated package children, import results, or evidence.

## Coverage

- Startup scene
- Embedded grid mesh, PBR material, and one-pixel PNG source in
  `Assets/Models/Baseline.gltf`
- UI
- Script
- Explicit project render settings
- Boot summary reporting

## Source recipe

`Assets/Models/Baseline.gltf.meta` gives the package the stable GUID
`11111111222222223333333344444444`. The source grid contains 578 triangles,
which intentionally clears the production virtual-geometry cooker's
512-triangle threshold. The compiler uses the package GUID and source roles to
derive stable generated child GUIDs:

- mesh `c8ff317261139cf962f2cedc60952f64` at
  `Models/Baseline_Generated/BaselineTriangle.temf`
- material `5cb8d797dc72ae521a7dd2fa784b0f9f` at
  `Models/Baseline_Generated/BaselineMaterial.tmat`
- texture `803d94e355222334826da82c660ecda8` at
  `Models/Baseline_Generated/BaselineTexture.tetf`

`BuildProfiles/baseline.json` is the only build-profile source. `game.json`
intentionally contains no duplicate embedded profile. The managed project
expects the baseline helper to stage the installed TerraEngine managed
reference and export analyzer into `Build/Assembly-CSharp/References`.

## Expected result

The exported runtime should launch into `Scenes/Startup.tescene`, emit a boot
summary, and display a simple scene with one visible object, one UI signal, and
one script-controlled runtime signal.

Run the installed-tree gate from the repository root:

```powershell
pwsh -NoProfile -File Tests/Tools/ExportRuntimeBaseline/RunBaseline.ps1 `
	-InstallRoot out/build/Debug/Build `
	-WorkRoot out/ci/export-runtime-baseline
```

The compiler creates TEMF/TMAT/TETF children only in the disposable work
project. Success requires valid nonempty headers and no placeholder marker in
any required compiled payload or export inventory entry.

Expected baseline signals:

- Project metadata: `Export Runtime Baseline` version `1.0.0`
- Startup scene: `Scenes/Startup.tescene`
- Render settings source: `ProjectSettings/RenderSettings.json`
- UI text: `Export Runtime Baseline Ready`
- Script log: `ExportRuntimeBaseline.ScriptStarted`
