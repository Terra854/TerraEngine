# Physics Stress and Lifecycle Fixtures

`stress-fixtures.json` is the committed, provider-neutral workload manifest for
the final physics hardening tests. It fixes the workload size and seed so a
failure can be replayed on a clean checkout without any locally installed
physics SDK.

The fixture requires the repository copy of Jolt Physics **5.6.0** at revision
`e77f175595e64cb44218cc9d9d56fc365ad0e36a`. Tests use TerraEngine-owned
backend values only; Jolt headers, IDs, and worker pools must not enter fixture
or test contracts.

The deterministic stress lane creates 10,000 mixed bodies, including 1,000
active dynamic bodies. It runs the same seeded scene from fresh backend
lifetimes at 1, 2, 4, and the host's maximum shared-pool worker limit, then
compares a backend-state hash and a sorted query-packet hash. The query lane
issues 10,000 rays from the identical state 100 times and checks that peak
provider allocation count stops growing after the warm-up pass.

The remaining fixture records dense contacts, a 32-link constraint chain, one
character/platform support case, and the bounded query, event, and debug
history limits. Their focused provider and editor tests exercise the specialised
records; keeping their counts here makes the acceptance workload auditable.

The lifecycle lane repeats stale-world races 100 times and editor Play Mode
enter/exit 1,000 times. It covers scene unload, remove/recreate, script reload,
renderer-unavailable diagnostics, project close, and shutdown. Every case must
leave no active world, late publication, or stale identity able to affect its
replacement.

To run these focused tests after they are registered by the shared test target:

```powershell
$env:TERRAENGINE_TEST_FILTER = 'Physics.DeterminismStress;Physics.LifecycleStress;Physics.Editor.Soak'
& .\out\build\Debug\Tests\Engine\TerraEngine-EngineTests.exe
& .\out\build\Debug\Tests\Editor\TerraEngine-EditorTests.exe
```

The final integrated build, CTest run, install-tree verification, and
installed-editor loopback remain orchestrator-owned acceptance work.
