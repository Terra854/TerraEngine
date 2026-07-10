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

## Preferences

Open `Edit > Preferences...` and select `MCP Bridge`. The category exposes
enablement, write mode, current state, pending request/approval counts, approval
buttons, export activity, resources, and recent activity. The bridge no longer
uses a separate editor window.

The listen port is also configured in this category and defaults to `8765`.
Disable the bridge before changing the port, enter a value from `1024` through
`65535`, then enable the bridge again. Valid edits save automatically. The
selected port is stored in the global editor preferences and reused on every
launch. If another process owns that port, the bridge remains disabled and
reports the bind error instead of choosing a random port.

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
`Models/Props/asset.glb` or `Models/Props/asset.fbx`, plus an optional `force`
flag. For `.gltf`, `.glb`, and `.fbx` model packages, the response includes
`data.importResult` with generated mesh, material, texture, primitive
assignment, bounds, package kind, and diagnostic arrays. Missing or unsupported
package content remains machine-readable through diagnostics so MCP clients can
decide whether to place partial imports or ask for human review.

Texture compression triggered by MCP package imports uses a conservative worker
cap by default so generated package textures and file-watcher reimports do not
spawn multiple all-core BasisU pools. Set `TERRAENGINE_TEXTURE_COMPILE_THREADS`
before launching the editor only when a workstation can safely tolerate more
concurrent texture-compression work.

## Validation

Use the Visual Studio developer environment on Windows so MSVC can locate the
standard library headers:

```powershell
cmd /c "C:\PROGRA~1\MIB055~1\18\COMMUN~1\Common7\Tools\VsDevCmd.bat -arch=x64 -host_arch=x64 && cmake --build --preset Debug --target TerraEngine-EditorTests TerraEngine-Editor"
.\out\build\Debug\TerraEngine-EditorTests.exe
ctest --test-dir out\build\Debug --output-on-failure
cmd /c "C:\PROGRA~1\MIB055~1\18\COMMUN~1\Common7\Tools\VsDevCmd.bat -arch=x64 -host_arch=x64 && cmake --build --preset Debug --target install"
```

Manual validation should use the installed editor tree. Open a project, open
`Edit > Preferences... > MCP Bridge`, confirm disabled/read-only startup, enable
the bridge, switch write mode from Preferences, inspect status/resources,
confirm no `bridge.set_write_mode` MCP tool is advertised, exercise
object/component tools, import a glTF or FBX package with `asset.import`, verify
the returned generated asset identifiers and diagnostics, then disable the
bridge from Preferences and confirm local client requests are rejected.

## Limitations

This implementation keeps transport deliberately local-only and loopback-bound.
It does not persist sessions across editor restarts or execute export/import work
through a background job runner. Those remain separate approval points because
they expand the bridge from editor automation policy into longer-running workflow
orchestration.
