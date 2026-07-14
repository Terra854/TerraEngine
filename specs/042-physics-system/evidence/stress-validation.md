# Phase 10 Stress and Soak Validation (T143-T145)

Validation ran from the repository `RelWithDebInfo` tree on 2026-07-14. The
committed manifest fixes 10,000 mixed bodies, 1,000 active dynamics, 10,000
rays repeated 100 times, dense contacts, a 32-link constraint chain, one
character/platform case, bounded histories, 100 stale-race repetitions, and
1,000 lifecycle/Play Mode cycles.

```powershell
ctest --test-dir out/build/RelWithDebInfo -C RelWithDebInfo `
  -R "^Engine\.PhysicsStress$" --output-on-failure

ctest --test-dir out/build/RelWithDebInfo -C RelWithDebInfo `
  -R "^Editor\.PhysicsSoak$" --output-on-failure
```

`Engine.PhysicsStress` passed 1/1 in 3.92 seconds. Its parent test
self-reexecuted the engine test binary for the unique 1/2/4/max worker counts,
compared backend-state and sorted-packet hashes, and completed the 100 identical
10,000-ray repetitions within the post-warm-up allocation-growth bound. The
same registration includes 100 stale remove/recreate, unload, reload, and
shutdown races plus 1,000 isolated PhysicsSystem lifecycle cycles.

`Editor.PhysicsSoak` passed 1/1 in 1.23 seconds. It completed 1,000 Play Mode
enter/exit cycles and covered script reload, renderer-unavailable diagnostics,
project close, authored snapshot restoration, and shutdown. Both registrations
are source-controlled through `Tests/Engine/CMakeLists.txt` and
`Tests/Editor/CMakeLists.txt`; fixture values live in
`Tests/Fixtures/Physics/stress-fixtures.json`.
