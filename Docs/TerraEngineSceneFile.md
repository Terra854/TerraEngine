# TerraEngine Scene and Prefab Files

TerraEngine uses JSON-authored data for both scenes (`.tescene`) and prefabs (`.teprefab`). The scene manager centralizes loading, caching, and lookups so gameplay systems can request authored content without performing redundant disk I/O.

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
Run the standard CMake configure, build, and install steps from a clean checkout:
```bash
cmake -S . -B out/build/make-debug -DCMAKE_BUILD_TYPE=Debug
cmake --build out/build/make-debug
cmake --build out/build/make-debug --target install
```
The install step populates the `Build/` tree under the build directory so downstream launchers can consume the engine binaries alongside their authored `.tescene` and `.teprefab` files.
