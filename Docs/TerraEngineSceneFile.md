# TerraEngine Scene and Prefab Files

TerraEngine uses JSON-authored data for both scenes (`.tescene`) and prefabs (`.teprefab`). The scene manager centralizes loading, caching, and lookups so gameplay systems can request authored content without performing redundant disk I/O.

## Source Authority and Derived Scene Cache

Files under `Assets/` remain the only authored authority. TerraEngine may create
disposable binary envelopes under `Library/DerivedScenes/`, but those files are
never edited as gameplay content, never exported, and may be deleted at any
time. A clean checkout is expected to contain no derived cache; the first open
loads the authored JSON and regenerates the cache after source preparation
succeeds.

Each derived envelope records its format version, source content identity,
dependency identity, component and reflection schema identities, engine and
platform compatibility, byte order, bounded section table, item counts, and
checksums. A hit must validate every identity and checksum before worker decode.
Missing, stale, corrupt, truncated, incompatible, or cancelled data falls back
to the authored `.tescene` without exposing a partial live scene. Successful
fallback regeneration writes a same-directory temporary file, closes and
validates it, then atomically replaces the prior cache.

Worker preparation produces parent-first immutable object records with exact
object/component/hierarchy reserve counts. The editor publishes the complete
packet at one protected scene boundary. Project close or replacement advances a
generation token, so late decode or regeneration work is rejected before it can
mutate `ObjectManager`. Play Mode uses the same generation-safe bulk packet
shape to restore authored state; it does not save and reparse a temporary scene
text document.

Do not add `Library/DerivedScenes`, managed readiness manifests, editor marker
preferences, or other `Library/`/`UserSettings/` state to source packages or
TEPAK exports. Only authored assets and explicitly cooked runtime products are
gameplay input.

## File Types
- `.tescene` – Full scene documents that describe world state (entities, components, environment settings, etc.).
- `.teprefab` – Reusable authored snippets meant to be instantiated inside scenes (e.g., an enemy archetype).
- Both formats are plain UTF-8 JSON. Files are validated with the vendored [`nlohmann::json`](https://github.com/nlohmann/json) library before being cached so syntax errors are caught early. A minimal starting point is an object root, for example:
  ```json
  {
  	"name": "ExampleScene",
  	"entities": []
  }
  ```

## Manager Usage
- Configure the scene root so relative paths resolve correctly (mirrors `ResourceManager::SetAssetRoot`):
  ```cpp
  SceneManager::Instance().SetSceneRoot("Assets/Scenes");
  ```
- Load synchronously when you need the data immediately:
  ```cpp
  TerraEngine::UUID sceneId = SceneManager::Instance().LoadScene("demo.tescene");
  const SceneDocument* doc = SceneManager::Instance().GetScene(sceneId);
  ```
- Load asynchronously to avoid blocking:
  ```cpp
  auto futureId = SceneManager::Instance().LoadSceneAsync("demo.tescene");
  TerraEngine::UUID sceneId = futureId.get();
  ```
- Prefabs mirror the same APIs via `LoadPrefab`/`LoadPrefabAsync` and `GetPrefab`.
- To drive gameplay, set the active scene after it is loaded:
  ```cpp
  SceneManager::Instance().SetActiveScene(sceneId);
  ```

## Scene Settings And Sky Components
- `sceneSettings` is reserved for scene-level render metadata that is not owned by a component, such as indirect lighting and ambient occlusion values.
- Sky authoring data is saved only on scene objects through `components.SkyComponent`. This keeps sky data on the same object/component path used by the inspector, prefabs, undo/redo, and normal scene saves.
- Older files may contain `sceneSettings.sky`. TerraEngine treats that field as unsupported old input and ignores it as if it were absent. Loading it does not create a `SkyComponent`, does not migrate data, and does not affect active sky selection.
- When more than one active object owns `SkyComponent`, the active sky source is selected by the deterministic scene traversal order: root objects and children are visited by UUID string order.

Example sky object:
```json
{
	"name": "Sky",
	"uuid": "01010101010101010101010101010101",
	"components": {
		"SkyComponent": {
			"active": true,
			"sourceAsset": "Textures/Sky/Default.tetf",
			"exposure": 1.0,
			"rotationOffsetDegrees": 0.0,
			"horizonTint": [0.92, 0.96, 1.0],
			"zenithTint": [0.2, 0.38, 0.8],
			"groundTint": [0.1, 0.12, 0.18]
		},
		"LightComponent": {
			"type": 0,
			"unit": 1,
			"color": [1.0, 1.0, 1.0],
			"intensity": 100000.0,
			"castsShadows": false
		}
	}
}
```

## Build and Install
Run the standard RelWithDebInfo configure, full build/test, and install steps
from a clean checkout. The installed tree is authoritative for live runs:

```powershell
cmake --preset RelWithDebInfo
cmake --build --preset RelWithDebInfo --target TerraEngine TerraEngine-Editor TerraEnginePhysicsColliderCompiler TerraEngineModelPackageCompiler TerraEngine-EngineTests TerraEngine-EditorTests TerraEngine-Export
ctest --test-dir out/build/RelWithDebInfo -C RelWithDebInfo --output-on-failure
cmake --build --preset RelWithDebInfo --target install
```

Use `out/build/RelWithDebInfo/Build` for installed-editor/export commands; do
not launch intermediate target binaries for acceptance. No build command passes
`--parallel`, and no worker cap is implied by the scene format. Derived scene
cache and managed-readiness files remain disposable `Library/` state and are
excluded from authored/export content.

The installed export parity check uses the same disposable work root as the
performance quickstart:

```powershell
powershell.exe -NoProfile -ExecutionPolicy Bypass -File Tests/Tools/ExportRuntimeBaseline/RunBaseline.ps1 `
	-InstallRoot out/build/RelWithDebInfo/Build `
	-WorkRoot out/ci/export-runtime-baseline
```
