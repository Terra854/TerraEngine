# Installed Editor Physics Stress Harness

`RunInstalledEditorPhysicsStress.ps1` is the opt-in live test for T142/T145.
It creates a disposable project below `out/`, writes 10,000 physics objects
(70% dynamic, 10% kinematic, and 20% static), starts the installed editor with isolated MCP
preferences, and drives Play Mode through the loopback JSON-RPC endpoint.

Run it only after build, CTest, and install succeed:

```powershell
powershell.exe -NoProfile -ExecutionPolicy Bypass -File Tools/Physics/RunInstalledEditorPhysicsStress.ps1 `
	-InstallRoot out/build/Debug/Build -AllowLiveStress
```

The script requires a free explicit MCP port (default `43127`) and writes its
disposable project plus JSON evidence below
`out/validation/installed-editor-physics-stress`. It verifies live tick/body
diagnostics, 100 exact paused Next ticks, Play Mode restoration, unchanged
authored scene bytes, and editor process health. It attempts a normal editor
shutdown first and records any last-resort forced termination in `summary.json`.
