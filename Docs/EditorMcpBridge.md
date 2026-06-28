# Editor MCP Bridge

The editor MCP bridge is an opt-in local automation surface for assistant-driven
editor work. It is disabled by default, accepts only loopback clients, and keeps
all mutating work behind the editor-owned `EditorAutomationService` so transport
code never edits scene or project state directly.

## Safety Model

- The bridge starts disabled and read-only.
- Local peer checks accept `localhost`, `127.0.0.1`, and `::1`; non-local peers
  are rejected and recorded in activity history.
- Enablement and write mode can only be changed in the editor UI. MCP clients
  cannot expose, hide, upgrade, or downgrade their own permission state.
- Read-only mode allows resources and read-only tools such as diagnostics,
  activity, capabilities, scene listing, asset listing, and export validation.
- Write mode is required for scene, object, component, asset, and export
  mutations.
- Destructive or high-impact tools create approval prompts before execution.
- Activity history is bounded to the newest 256 records so long sessions do not
  grow memory indefinitely.

## Editor Window

Open `Windows > MCP Bridge` from the native editor menu. The window exposes
enablement, write mode, current state, pending request/approval counts, approval
buttons, and recent activity.

## Resources

- `terraengine://bridge/status`
- `terraengine://bridge/capabilities`
- `terraengine://project/state`
- `terraengine://scene/summary`
- `terraengine://diagnostics/summary`
- `terraengine://activity/recent`

## Tools

The current bridge registers the contract surface for bridge control, activity,
scene/object/component editing, asset operations, export operations, and
diagnostics. It provides a local loopback HTTP/JSON-RPC listener, capability and
resource discovery, activity/status reporting, scene object metadata operations,
reflected component access through `ComponentAccessRegistry`, safe empty-state
diagnostics, project-relative asset path validation, and export job response
envelopes.

`asset.import` accepts a project-relative asset path, such as
`Models/Props/asset.glb`, plus an optional `force` flag. For `.gltf` and `.glb`
packages, the response includes `data.importResult` with generated mesh,
material, texture, primitive assignment, bounds, and diagnostic arrays. Missing
or unsupported package content remains machine-readable through diagnostics so
MCP clients can decide whether to place partial imports or ask for human review.

## Validation

Use the Visual Studio developer environment on Windows so MSVC can locate the
standard library headers:

```powershell
cmd /c "C:\PROGRA~1\MIB055~1\18\COMMUN~1\Common7\Tools\VsDevCmd.bat -arch=x64 -host_arch=x64 && cmake --build --preset Debug --target TerraEngine-EditorTests TerraEngine-Editor"
.\out\build\Debug\TerraEngine-EditorTests.exe
ctest --test-dir out\build\Debug --output-on-failure
cmd /c "C:\PROGRA~1\MIB055~1\18\COMMUN~1\Common7\Tools\VsDevCmd.bat -arch=x64 -host_arch=x64 && cmake --build --preset Debug --target install"
```

Manual validation should use the installed editor tree. Open a project, show the
MCP Bridge window, confirm disabled/read-only startup, enable the bridge, switch
write mode from the editor window, inspect status/capabilities/resources,
confirm no `bridge.set_write_mode` MCP tool is advertised, exercise
object/component tools, import a glTF package with `asset.import`, verify the
returned generated asset identifiers and diagnostics, then disable the bridge
from the editor window and confirm local client requests are rejected.

## Limitations

This implementation keeps transport deliberately local-only and loopback-bound.
It does not persist sessions across editor restarts or execute export/import work
through a background job runner. Those remain separate approval points because
they expand the bridge from editor automation policy into longer-running workflow
orchestration.
