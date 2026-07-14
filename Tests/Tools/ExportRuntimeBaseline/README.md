# Legacy Export Runtime Baseline Entry Point

`ExportSample.ps1` is retained only for callers that used the historical test
tool path. It validates the `baseline` profile name and forwards to the
canonical installed-tree gate:

```powershell
pwsh -NoProfile -File Tests/Tools/ExportRuntimeBaseline/ExportSample.ps1 `
	-BuildDir out/build/RelWithDebInfo `
	-WorkRoot out/ci/export-runtime-baseline
```

New scripts and CI should invoke `Tools/ExportRuntimeBaseline/RunBaseline.ps1`
directly. See that directory's README for prerequisites, generated paths, and
failure evidence.
