# Scene Graph Runtime

TerraEngine resolves authored hierarchy through `Engine/Scene/SceneGraphRuntime.*`.
The graph API gives runtime, editor, tests, and export startup one deterministic
surface for object identity, parent/child relationships, world transforms,
diagnostics, and comparable scene summaries.

## Prefab Workflow

- Create/open/save/delete prefab operations are represented by
  `PrefabAuthoringSubsystem` and resolved through `PrefabComposition`.
- Deleting a prefab source cancels by default when live instances still
  reference it. Explicit conversion is required to keep those instances as
  ordinary scene objects.
- Instance overrides can be applied back to prefab defaults or reset to source
  defaults.

## Mutation Timing

Structural scene edits should be queued in `SceneMutationQueue`. The engine
applies queued structural changes after systems finish their update pass, so
component iteration is not invalidated halfway through a frame.

## Manual Validation Notes

- No new dependency was added under `External/`.
- No shader source was added or embedded.
- Manual validation should use the fixture files in
  `Tests/Fixtures/SceneGraphRuntime/` plus the quickstart flow in
  `Docs/Testing.md`.
- Automated configure, focused build, CTest, and install validation passed
  through the Visual Studio Developer Command Prompt environment. Interactive
  editor clicking was not run in this headless pass.
