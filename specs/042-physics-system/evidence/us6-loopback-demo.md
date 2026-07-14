# US6 Installed Editor Loopback Demo

Date: 2026-07-14  
Configuration: installed RelWithDebInfo editor, Windows x64, 32 workers  
Scope: T129

## Result

PASS. The authoritative run used only the installed editor and its advertised
loopback MCP tools against a fresh disposable 10,000-body project:

```powershell
powershell.exe -NoProfile -ExecutionPolicy Bypass `
  -File Tools/Physics/RunInstalledEditorPhysicsStress.ps1 `
  -InstallRoot out/build/RelWithDebInfo/Build `
  -WorkRoot out/validation/t129-us6-loopback-pass `
  -McpPort 43142 -WorkerCount max -RunUs6Loopback -AllowLiveStress
```

Primary artifacts:

- `out/validation/t129-us6-loopback-pass/evidence/summary.json`
- `out/validation/t129-us6-loopback-pass/evidence/mcp.jsonl`
- Disposable project profiler artifact `project-20260714-083632.teprof`
  (`profiler-433f3371a6955188`)

## Observed scenario

1. Capability discovery found the required play, script, job, physics read,
   query, command, diagnostics, contact, and profiler tools.
2. The clean project reached managed coordinator `Ready`; an explicit public
   `script.compile` then completed durable job `job-1` with correlation
   `t129-managed-reload` in 1,834.032 ms.
3. Play Mode published 10,000 bodies. A bounded vertical raycast completed
   durable query job `job-2` and identified the dedicated gravity-disabled body
   `0000000000000000000000000000079f` with component generation 4 and world
   generation 1.
4. Immutable body inspection reported zero starting velocity; physics
   diagnostics exposed 7 scheduler declarations.
5. While paused, `physics.body.apply_force` submitted
   `applicationMode=Acceleration`, vector `[0,-9.81,0]`, and
   `applyOnNextStep=true`. The exact retry returned disposition `replay` and the
   same command ID 1.
6. One separate paused Next moved tick 42 to 43. Vertical velocity changed from
   `0` to `-0.1634455174 m/s`, inside the one-application 1/60-second envelope
   and outside both the zero-application and duplicate-application envelopes.
7. The unchanged stress interval then completed 100 consecutive exact paused
   Next requests. It finished in 10,660.552 ms with publication p50/p95/p99 of
   104.364/131.244/139.609 ms.
8. The bounded raw diagnostics history returned 64 retained contacts in stable
   tick order and explicitly reported truncation/overflow. This observation did
   not create or broaden a managed gameplay subscription.
9. Play Mode restoration completed, the normalized authored scene SHA-256
   remained exactly
   `C4ADC25341B6096EFBBCC1DD3890F91E3B7A2B255B0E605160FF2AD3E95DC76B`,
   profiler persistence/summary succeeded, and the editor shut down normally in
   398.777 ms (`forcedTermination=false`).

Total run duration was 25,582.614 ms. The profiler captured 324 frames; its
323 complete Engine CPU samples had mean 33.286 ms and p95 58.931 ms, while
viewport CPU p95 was 0.450 ms and viewport GPU p95 was 0.223 ms.

## Managed reload and event coverage boundary

The live run proves the installed managed assembly crosses a second durable
reload and that the same protected physics publication is observable through
the public automation surface. The focused `Engine.PhysicsManagedIntegration`
gate supplies the complementary in-process proof that pre-reload wrappers fail
safely, managed subscription snapshots are reset, and one throwing event
callback cannot block later callbacks. The live transcript does not claim that
an attachable gameplay script was authored in this disposable project.

## Corrective iterations retained as evidence

Earlier disposable attempts are preserved under
`out/validation/t129-us6-loopback*`. They exposed and then verified fixes for:

- the physics job envelope's immutable `data.data.job` nesting;
- tick-range contact reads that originally consulted only the latest packet;
- the absence of raw diagnostics contacts when no managed subscriber existed;
- serializer normalization needed for a prompt-free normal window shutdown.

