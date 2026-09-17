# Collision-safe navigation verification — 2026-09-17

Verified the installed RelWithDebInfo editor using
`NavigationCollisionLiveTest.cs`, terrain, a kinematic box, and two controller-only
duck objects. The active test project is
`out/build/RelWithDebInfo/Build/Editor/ProjectCreationRetest-20260916`, scene
`Assets/Scenes/Scene.tescene`.

## Final live results

Two Play runs completed in the same editor process, with Stop/Play between them.

| Check | Run 1 | Run 2 |
| --- | --- | --- |
| Held before moving box | x=19.5800 | x=19.5800 |
| Held before stationary character | x=27.1600 | x=27.1600 |
| Resumed and reached target | x=34.9047, z=19.9999 | x=34.9099, z=20.0000 |
| `NAVCOLL FAIL` | None | None |
| Physics rejected / stale operations | 0 / 0 | 0 / 0 |

Both runs reported two rigid bodies and two characters. The stationary character
remained at x=28 until its scripted movement cleared the route. Scene and physics
pose diagnostics agreed. The editor was left stopped with authored state restored
and saved.

These were live runtime checks through the editor's local MCP bridge, not a
visual desktop inspection: Windows was locked during this verification.

## Build and automated checks

- Build/install succeeded: `out/navigation-final-incremental-install.log`.
- Focused CTest passed all 26 groups in 22.45 seconds:
  `out/navigation-final-tests.log`.
- Live output: `out/editor-navigation-final.stdout.log`.
- `git diff --check` passed (normal Windows line-ending warnings only).

The native regressions cover fixed-tick movement independent of command rate,
character blocking and removal, collision masks, deferred character transform
publication, stale-packet atomic rejection, and managed command admission.

See `Docs/Testing.md` for replay instructions and `Docs/Navigation.md` for the
`NavigationRouteFollower` API. Movement uses controller intent, not transform
assignments or teleports. The fixture explicitly stops the kinematic box after
each target-over-duration command so the obstacle stays in the test corridor.

## Follow-up: ducks disappearing on Play (2026-09-17)

Visual reproduction found physics publication replacing authored scale `0.01`
with the rigid pose's default scale `1`. Physics now preserves scale at the
protected transactional publication boundary for bodies and characters. The live
fixture checks both ducks' scale every update, alongside the navigation checks.

- Rebuilt and installed successfully: `out/navigation-scale-build.log`.
- All 28 navigation, physics, transaction, mutation, and bulk-publication CTest
  groups passed in 31.09 seconds: `out/navigation-scale-tests.log`.
- Two consecutive live Play runs each held at the moving box (`x=19.58`), held
  at the blocking duck (`x=27.16`), and arrived (`x=34.90988`), with no FAIL logs.
- Desktop inspection confirmed duck meshes remained visible during Play and
  after Stop; the selected moving duck retained Inspector scale `0.01`.
- First-run diagnostics reported two bodies, two characters, zero rejected or
  stale commands, and zero rejected publication packets.
- Live output: `out/editor-navigation-scale.stdout.log`. Editor left stopped.

This follow-up includes visual inspection; the earlier locked-desktop run above
only verified runtime positions and did not catch the scale regression.
