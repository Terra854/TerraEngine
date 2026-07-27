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

- `terraengine://bridge/status`: enablement, endpoint, permission mode, clients,
  pending requests, approvals, and activity counters.
- `terraengine://bridge/capabilities`: versioned tool descriptors, JSON schemas,
  permissions, approval policy, current availability, and execution boundaries.
- `terraengine://project/state`: the version-one project summary retained for
  compatibility.
- `terraengine://project/session`: project and active-scene identity, prefab
  context, dirty state, generations, selection, Play Mode, and service
  availability.
- `terraengine://scene/summary`: the version-one active-scene summary.
- `terraengine://history/state`: history sequence and current undo/redo labels.
- `terraengine://play/state`: current and pending Play Mode state.
- `terraengine://jobs/recent`: bounded active and recently completed jobs,
  including the oldest retained sequence.
- `terraengine://logs/recent`: bounded structured logs and cursor metadata.
- `terraengine://editor/commands`: registered semantic commands and current
  availability.
- `terraengine://profiler/captures`: profiler artifacts and active capture/job
  state.
- `terraengine://diagnostics/summary`: editor, scripting, asset, export, and
  renderer diagnostics.
- `terraengine://renderer/recovery`: renderer containment and safe-save state.
- `terraengine://capture/availability`: image sources, limits, and renderer
  availability.
- `terraengine://activity/recent`: bounded audit records for requests,
  approvals, commits, cancellation, and terminal jobs.

Resources are read-only snapshots. Their availability does not grant permission
to call a related write tool.

## Tools

`terraengine://bridge/capabilities` is the machine-readable authority for the
current editor state. A tool is advertised only when it has an executable
handler, complete input/result schemas, permission and approval metadata, a
mutation classification, and an availability result. Existing version-one names
and MCP `tools/call` content envelopes remain compatible.

### Project and scenes

- Project: `project.templates.list` and `project.state`. Creating, opening, and closing projects remain native editor actions.
- Scene lifecycle: `scene.list`, `scene.create`, `scene.open`, `scene.save`,
  `scene.save_as`, `scene.rename`, `scene.duplicate`, and
  `scene.prepare_delete`.
- Scene graph: `scene.list_objects`, `object.create`, `object.set_metadata`,
  `object.set_parent`, `object.delete`, `component.list`, `component.get`,
  `component.add`, `component.remove`, and `component.set_property`.
  `scene.list_objects` walks the complete hierarchy in deterministic UUID order;
  each row retains the compatibility `parent` entity and adds `parentUuid` for
  stable hierarchy bindings across scene reloads.

Project creation/opening remain native editor actions and are not advertised by
MCP. Scene and asset paths are normalized against the owner-opened project. Dirty
project transitions require an explicit precondition and, where necessary, an
editor-owned approval. Scene, hierarchy, and component writes enter the typed
transaction/deferred-mutation path instead of modifying live storage from a
worker.

### Transactions and history

- `transaction.execute` accepts an ordered operation array, optional expected
  scene generation, label, correlation identifier, and idempotency key.
- `history.state`, `history.undo`, and `history.redo` expose the editor's normal
  composite history semantics.

An accepted transaction prepares against immutable snapshots, revalidates at
the protected editor boundary, and either applies every operation or applies
none. The result identifies its transaction/history entry and visibility
boundary. One accepted group creates at most one user-visible undo entry.
Selection is an editor-state tail and is committed only after the scene
transaction succeeds.

### Play Mode, capture, and diagnostics

- Play Mode: `play.state`, `play.enter`, `play.pause`, `play.resume`,
  `play.step`, and `play.exit`.
- Image capture: `game_view.capture`, `camera.capture_object`, and
  `camera.capture_custom`.
- Diagnostics: `logs.query`, `diagnostics.get_summary`,
  `renderer.virtualGeometry.stats`, and
  `renderer.recovery.safe_save_project`.

Play transitions accept an expected state and report current and pending state.
`play.step` is valid only while paused and completes exactly one simulation
step. Exiting retains the editor's authored-state restoration rules.

### Jobs

- `jobs.list` and `jobs.get` inspect retained work.
- `jobs.cancel` requests cooperative cancellation when the job advertises
  `cancellable: true`.
- Long-running import, script compilation, export, and profiler operations
  return `status: "accepted"`, a durable `jobId`, initial state, and poll/cancel
  tool names instead of holding one request open.

Job states are `queued`, `preparing`, `running`, `cancel_requested`,
`succeeded`, `failed`, and `canceled`. Progress never decreases. Job snapshots
include stage, timestamps, diagnostics, artifacts, request/correlation IDs, and
the terminal result. Active jobs are not evicted; completed jobs remain
queryable for the current editor session. Cancellation reports whether work
stopped before protected commit, after commit, or was unsupported.

### Assets, prefabs, and scripts

- Assets: `asset.list`, `asset.inspect`, `asset.import`, `asset.move`,
  `asset.rename`, `asset.duplicate`, `asset.prepare_delete`,
  `asset.apply_virtual_geometry_settings`, and
  `asset.rebuild_virtual_geometry`.
- Prefabs: `prefab.create`, `prefab.instantiate`,
  `prefab.overrides.inspect`, `prefab.overrides.apply`,
  `prefab.overrides.revert`, `prefab.edit.open`, and `prefab.edit.close`.
- Scripts: `script.templates.list`, `script.create`, `script.types.list`,
  `script.workspace.sync`, and `script.compile`.

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

Asset moves, renames, duplication, and deletion preflight report reference
impact and reject unsafe aliases or destinations. Script creation is limited to
approved templates, valid type names, namespaces, and project-relative
destinations. The bridge does not expose arbitrary filesystem writes or process
execution.

### Export, settings, Scene View, commands, and profiler

- Export compatibility: `export.validate`, `export.start`, and `export.cancel`.
- Build profiles: `build_profile.list`, `build_profile.get`,
  `build_profile.create`, `build_profile.update`, `build_profile.duplicate`,
  and `build_profile.prepare_delete`.
- Settings: `settings.project.get`, `settings.project.set`,
  `settings.editor.get`, and `settings.editor.set`.
- Scene View: `scene_view.state`, `scene_view.camera.set`,
  `scene_view.frame_object`, `scene_view.tool.set`,
  `scene_view.orientation.set`, `scene_view.snapping.set`, and
  `scene_view.visualization.set`.
- Commands: `editor.commands.list` and `editor.commands.invoke`.
- Profiler: `profiler.capture.start`, `profiler.capture.stop`,
  `profiler.capture.list`, `profiler.capture.load`, and
  `profiler.capture.summary`.

Settings use explicit project/editor allowlists. Global editor preference writes
are high impact. Command invocation is limited to explicitly registered semantic
commands; menu-label execution and screen-coordinate input are never accepted.

## Permissions and approvals

Each capability declares one permission level:

- `read` inspects snapshots and can run while the bridge is read-only.
- `project_write` changes the open project, authored content, jobs, or editor
  session and requires **Writes enabled** in Preferences.
- `global_write` changes allowlisted editor-wide preferences and requires
  writes plus high-impact approval.

Approval policy is `none`, `destructive`, or `high_impact`. Required approvals
are created and resolved only by the editor Preferences UI, expire safely, and
are bound to the normalized request fingerprint. Approving a different request,
changing its arguments, or replaying an expired approval does not authorize the
call. The loopback transport closes each HTTP request after its response, so
pending approvals remain bound to the current enabled-listener session across
those ordinary socket closures. Disabling the bridge, re-enabling it, or
restarting the editor starts a new listener session and invalidates every older
prompt. Every approval outcome is retained in bounded activity history.

State-changing calls include a correlation ID and return affected identifiers.
When supplied, an idempotency key is scoped to the client, capability, and
normalized arguments: an exact completed retry replays its result, while reuse
with different arguments returns `conflict` and performs no work.

## Structured logs and errors

`logs.query` filters by severity, category, source, text, time range,
correlation ID, request/job ID, opaque cursor, and bounded page size. Results are
ordered and include `nextCursor`, `oldestRetainedSequence`, `hasMore`, and
`droppedBeforeCursor`; retention loss is therefore explicit rather than silently
skipped.

Failures use a machine-readable category: `validation`, `permission`,
`approval_required`, `conflict`, `stale_target`, `unavailable`, `canceled`,
`timeout`, `unsupported_capability`, or `internal`. Existing version-one capture
errors retain their more specific compatibility categories where applicable.

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

The bridge is an editor-assistance API, not a remote administration endpoint. It
remains fixed-port, loopback-only, disabled and read-only by default, and cannot
enable itself or elevate its permissions. It does not expose arbitrary shell or
process execution, unrestricted filesystem access, source-control operations,
engine-source modification, package installation, arbitrary menu names, or
desktop input emulation.

Jobs, idempotency results, cursors, logs, approvals, and activity are retained
only for the current editor session; clients must rediscover after restart.
Approvals have the narrower lifetime of one continuously enabled listener and
are canceled whenever that listener is disabled.
Project artifacts and saved editor data retain their normal persistence. Active
jobs are cooperative rather than forcibly terminated: an already committed
atomic change remains committed, while an uncommitted transaction remains
unapplied.

Capability availability is state-dependent. Operations that need an open
project, active scene, renderer, stopped Play Mode, or implemented subsystem may
be advertised as unavailable with a structured reason. Future engine features
remain absent until their component or subsystem registers a complete automation
descriptor and executable handler.

## Animation Provider

The optional `animation` provider follows the same read-only, write-mode, and
approval rules as other MCP providers. Its asset and authoring mutations use
expected generations and idempotency keys; long-running work uses bounded
session-retained jobs. It has no authority to open/close projects or control the
application lifecycle. Its runtime residency and profiler reads consume copied,
bounded diagnostics snapshots and do not enable detailed collection when an
automation client merely polls a collapsed surface. See
[Animation automation](Animation/Automation.md) for the operation and recovery
workflow.
