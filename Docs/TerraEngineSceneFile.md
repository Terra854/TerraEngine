# TerraEngine Scene and Prefab Files

TerraEngine uses JSON-authored data for both scenes (`.tescene`) and prefabs (`.teprefab`). The scene manager centralizes loading, caching, and lookups so gameplay systems can request authored content without performing redundant disk I/O.

## File Types
- `.tescene` – Full scene documents that describe world state (entities, components, environment settings, etc.).
- `.teprefab` – Reusable authored snippets meant to be instantiated inside scenes (e.g., an enemy archetype).
- Both formats are plain UTF-8 JSON. A minimal starting point is an object root, for example:
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

## Build and Install
Run the standard CMake configure, build, and install steps from a clean checkout:
```bash
cmake -S . -B out/build/make-debug -DCMAKE_BUILD_TYPE=Debug
cmake --build out/build/make-debug
cmake --build out/build/make-debug --target install
```
The install step populates the `Build/` tree under the build directory so downstream launchers can consume the engine binaries alongside their authored `.tescene` and `.teprefab` files.
