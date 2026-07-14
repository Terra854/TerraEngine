# US5 installed-editor walkthrough evidence

## Reproducible RelWithDebInfo command

```powershell
powershell.exe -NoProfile -ExecutionPolicy Bypass `
	-File Tools/Physics/RunInstalledEditorPhysicsStress.ps1 `
	-InstallRoot out/build/RelWithDebInfo/Build `
	-WorkRoot out/validation/t113-installed-editor-walkthrough-final `
	-ObjectCount 10000 -ExplicitStepCount 100 -WorkerCount max `
	-McpPort 43152 -RunAuthoringWalkthrough -AllowLiveStress
```

Result: exit code 0. The disposable clean-project workflow drove the installed
`RelWithDebInfo` editor through its public loopback MCP contract, resolved 32
workers, and completed in 25,175.307 ms (25.175 s), well below the ten-minute
limit.

Primary artifacts:

- `out/validation/t113-installed-editor-walkthrough-final/evidence/summary.json`
- `out/validation/t113-installed-editor-walkthrough-final/evidence/mcp.jsonl`
- `out/validation/t113-installed-editor-walkthrough-final/project/Profiler/project-20260714-075155.teprof`

## Live walkthrough and timing

| Requirement | Installed-editor result |
| --- | --- |
| Clean scene composition | Generated 10,000 physics bodies: 7,000 dynamic falling boxes, 1,000 kinematic boxes, and 2,000 static bodies including the floor |
| Authoring | Public `object.create` plus one two-operation metadata transaction completed; source UUID `fd9d397afd9a46f7d662373971b7a141` remained stable |
| Undo/redo | `history.undo` restored the exact original name; `history.redo` restored both committed name and tag under the same object UUID |
| Prefab | Created `Assets/Prefabs/T113PhysicsWalkthrough.teprefab` as `prefab-1`, instantiated UUID `eca5b845d287fbd9cf4318edc43b2614`, applied one structured override, authored a second real override, and reverted it to zero remaining overrides |
| Authoring duration | 1,565.547 ms, including serializer normalization, object/history checks, prefab creation/compilation, instantiate, apply, and revert |
| Play Mode live state | First observed live tick 25; immutable physics state and diagnostics reported at least 10,000 bodies |
| Pause and Next | Pause completed in 110.432 ms; 100 explicit paused requests each advanced exactly one published tick; final paused tick 135 |
| Explicit-step performance | 10,324.220 ms total; publication p50/p95/p99 104.096/126.584/130.030 ms |
| Debug and profiling | Live and paused `physics.diagnostics.get` snapshots succeeded; the profiler persistence job produced `project-20260714-075155.teprof` |
| Play restoration | `play.exit` reported complete restoration with no partial failures and `play.state` returned `stopped` |
| Walkthrough cleanup | Five bounded history undos removed both temporary objects; `scene.save` cleared the dirty state |
| Authored bytes | Before/after SHA-256 matched: `E9F4AE46CC44BD5DE0B9927929BBF2A55F98AE9B55BC7C519DD26AD061109C5A` |
| Normal shutdown | `forcedTermination=false`; editor shutdown completed in 389.380 ms |

Other recorded stages were project/scene generation 1,830.632 ms, MCP
readiness 593.052 ms, initial managed build/reload 4,553.751 ms, Play Mode entry
437.468 ms, first live physics observation 606.265 ms, and Play exit/restoration
plus cleanup 1,854.319 ms. The MCP log contains 359 independently readable
request/response records.

## Renderer-unavailable acceptance

Renderer shutdown is intentionally not exposed as a production MCP mutation.
The exact condition was therefore run in the focused RelWithDebInfo editor test
executable with this bounded filter:

```powershell
$env:TERRAENGINE_TEST_FILTER = `
	'Physics.SceneView.RendererUnavailableStateKeepsDiagnosticsReadable,' + `
	'Physics.Editor.Performance.TenThousandObjectCreationLoadPlayModeAndPausedNextRemainObservableWithoutRenderer'
out/build/RelWithDebInfo/TerraEngine-EditorTests.exe
```

Result: 2/2 passed. The performance case explicitly calls
`RenderSystem::Shutdown()` before debug submission and proves diagnostics stay
readable and bounded, while the installed-editor run proves the same public
debug and Play Mode surfaces with the renderer available. Together these avoid
adding an unsafe test-only renderer-kill capability to the live editor.
