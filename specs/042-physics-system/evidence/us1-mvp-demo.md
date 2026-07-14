# US1 Installed MVP Demonstration

Date: 2026-07-14  
Configuration: RelWithDebInfo only  
Provider: Jolt Physics 5.6.0 at `e77f175595e64cb44218cc9d9d56fc365ad0e36a`

## Installed-editor command

```powershell
powershell.exe -NoProfile -ExecutionPolicy Bypass -File Tools/Physics/RunInstalledEditorPhysicsStress.ps1 `
	-InstallRoot out/build/RelWithDebInfo/Build `
	-WorkRoot out/validation/t111-bounded-acquire-max `
	-ObjectCount 10000 `
	-ExplicitStepCount 100 `
	-McpPort 43147 `
	-WorkerCount max `
	-AllowLiveStress
```

Result: pass. The installed editor completed the disposable clean-project workflow in
22.484 seconds and shut down normally without forced termination.

## MVP observations

- The authored fixture contained 10,000 primitive box bodies: 7,000 dynamic,
  1,000 kinematic, and 2,000 static, including a wide static floor.
- The installed Jolt world published 10,000 bodies and 10,000 shapes. Its live
  tick advanced from zero to 26 before pause, proving that the provider world and
  gravity-enabled dynamic simulation were running.
- While paused, all 100 Next requests advanced exactly one published tick and
  returned to paused. The final paused tick was 133; no request overshot.
- The no-camera/renderer-unavailable path was non-fatal. Physics state, diagnostics,
  Play/Pause/Next automation, and normal editor shutdown remained available.
- The authored scene SHA-256 before and after Play Mode was identical:
  `A3BDD762256B74E9FD1F62CFA2B7B769EF4EAC35134C0AE0AE672E40062AF647`.
  Runtime Transform publication therefore remained visible during Play without
  corrupting the authored scene restored on exit.

The live fixture intentionally emphasizes large-scene lifecycle and fixed-step
publication. Current RelWithDebInfo behavioral tests provide the deterministic
gravity/collision assertions for the same installed implementation:

```powershell
ctest --test-dir out/build/RelWithDebInfo -C RelWithDebInfo `
	-R "^(Engine\.PhysicsDependencyContract|Engine\.PhysicsFoundation|Engine\.PhysicsUS1|Editor\.PhysicsUS1)$" `
	--output-on-failure --no-tests=error
```

Result: 4/4 passed. These groups cover gravity and `useGravity`, primitive
collision/material/filter behavior, sleeping and wake-up, kinematic targets,
fixed-step bounds, transactional transform visibility, rejected-write rollback,
and exact Play Mode restoration.

## Evidence artifacts

- `out/validation/t111-bounded-acquire-max/evidence/summary.json`
- `out/validation/t111-bounded-acquire-max/evidence/mcp.jsonl`
- `out/validation/t111-bounded-acquire-max/project/Assets/Scenes/PhysicsStress.tescene`
- `out/validation/t111-bounded-acquire-max/project/Profiler/project-20260714-073626.teprof`

T048 is accepted by the combined installed-editor and current RelWithDebInfo
behavioral evidence above.
