# Virtual Geometry benchmark setup

This directory owns the versioned manifests used by the production virtual-geometry workflow. The manifests
are inputs only; they do not claim that a benchmark executable or GPU capture
has already run.

## Current setup

- `workloads.json` names deterministic workload recipes and the existing fixture
  inventory used by later capture gates.
- `hardware-matrix.json` names the required adapter families and records the
  host observations captured during setup. Missing hardware is represented as
  `unavailable`, never as a zero-valued measurement.
- `quality-profiles.json` defines comparable settings for reference and
  accelerated routes.
- `dependency-baseline.json` records the vendored dependency commit hashes.

The production virtual-geometry benchmark target is built and installed by the normal CMake
workflow. Capture scripts still fail closed when the executable, manifests, or
authoritative runtime adapter is unavailable. Every accepted capture preserves
command inputs, raw samples, execution IDs, revision/settings hashes, and image
evidence beside its output directory.

## Reproducible setup commands

From the repository root, initialise the pinned submodules and inspect the
manifests:

```powershell
git submodule update --init --recursive
Get-Content Tests/Tools/VirtualGeometry/workloads.json | ConvertFrom-Json
Get-Content Tests/Tools/VirtualGeometry/hardware-matrix.json | ConvertFrom-Json
Get-Content Tests/Tools/VirtualGeometry/quality-profiles.json | ConvertFrom-Json
```

The baseline build, test, and install commands are documented in
`Docs/Testing.md`; this directory contains only installed-product inputs and
evidence contracts.

Capture and comparison tools accept only measured samples carrying a matching
`executionId`; five or more samples are required for aggregates. Unavailable,
zero-sample, or mismatched evidence is reported as incomplete and never as a
performance claim. Streaming and production soaks likewise require actual
elapsed duration and explicit bounded-counter evidence before becoming measured.

Installed-editor visual summaries can be converted without inventing timings:

```powershell
powershell -File Tests/Tools/VirtualGeometry/CaptureVirtualGeometryPerformance.ps1 `
  -VisualSummary out/editor/visual-summary-01.json,out/editor/visual-summary-02.json `
  -Output out/evidence/virtual-geometry/visual-capture.json
```

The normal installed-editor summary supplies
`postCaptureCapabilityEvidence.data.measurements[]`. Only rows with
`availability=measured`, a non-empty `executionEvidenceId`, and a positive
`durationNanoseconds` are accepted. A standalone adapter may instead supply
`rows[]`, but those rows must also set `postCaptureMeasured=true`. Duplicate
evidence does not count, and at least five measured rows are required.

## Production capability gate

`RunVirtualGeometryCapabilityGate.ps1` is the Vulkan-only installed-tree entry
point. It validates exactly one manifest for each production capability, runs the
focused quickstart slices, codec/page experiment, route-image comparisons,
full-reference and million-decision prerequisites, vendor matrix, benchmark
comparison, install/package inventory, and installed-editor MCP
scenarios. Every command record includes the executable, argument, stdout, and
stderr SHA-256 values plus a unique execution identity. JSON inputs without an
accepted status and execution identity remain incomplete.

The complete invocation supplies the artifacts that cannot be derived from a
fallback editor capture:

```powershell
powershell -NoProfile -ExecutionPolicy Bypass -File `
	Tests/Tools/VirtualGeometry/RunVirtualGeometryCapabilityGate.ps1 `
	-CodecInputMesh <compiled-corpus-mesh.temf> `
	-HardwareImage <hardware.png> `
	-ComputeImage <compute.png> `
	-HybridImage <hybrid.png> `
	-IndexedImage <indexed.png> `
	-FullReferenceEvidence <accepted-full-reference.json> `
	-MillionDecisionEvidence <accepted-million-decision.json> `
	-BenchmarkBaseline <measured-baseline.json> `
	-BenchmarkCandidate <measured-candidate.json>
```

Outputs are rooted at `out/evidence/virtual-geometry/production-gates/`, with
the aggregate `production-gate.json` and `release-gates.json` beside them.
Missing runner hardware is recorded as `unavailable`; it is never converted to
a parity pass. `-DryRun` validates the
Vulkan-only orchestration without launching tests, tools, or the editor.

For the heterogeneous-static 4,000/4,000 material-shape workload, run the
installed typed renderer-topology benchmark. It measures CPU time in
`BuildPersistentRenderTopology` and `BuildDynamicRenderFrameState` using
exactly 4,000 deterministic material identities. Because this headless tool
does not submit a live frame, its GPU timing field remains explicitly
unavailable rather than inventing a GPU number:

```powershell
powershell -File Tests/Tools/VirtualGeometry/CaptureHeterogeneousStaticNative.ps1 `
  -DurationSeconds 60 `
  -Output Tests/Fixtures/RendererPerformanceBaseline/results/final/heterogeneous-static-native-60s.json
```

The six-workload final matrix uses `CaptureVirtualGeometryPerformance.ps1` for
`existing-4000-animated`, `repeated-static`, `ten-renderable`, and the
installed-editor scene summaries, while `CaptureHeterogeneousStaticNative.ps1`
uses the engine topology benchmark and reports its headless GPU state. For each installed-editor row,
run five times with distinct `-Output` paths and MCP ports; never pass
`-CreateValidationScene` against the authored `Scene.tescene`.

The raw direct-static rows use the external project's `Assets/viking_room.obj`
fixture (mesh `87f68c2d68360cb2318f0a77fa3eaf0b`): 3,828 source triangles,
approximately 1.33 x 1.46 x 1.03 units, and transient scale 1.0. The repeated
row uses material `fa653ebd8b3742d77f6b2386ca8a4694`; the ten-renderable row
cycles that key with `60c9af18ea51c6ee9ac8f3e1da50557e` and
`81139f23f318c10dc998212f5b92814b`. It is a MeshImporter source without a
generated virtual-geometry payload, but the accepted live capture still must
prove an authoritative direct route, zero streaming counters, a nonblack frame,
and authoritative picking before a 4,000-instance run is allowed.
