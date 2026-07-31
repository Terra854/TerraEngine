# Virtual Geometry benchmark setup

This directory owns the versioned manifests used by feature 045. The manifests
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

The feature 045 benchmark target is built and installed by the normal CMake
workflow. Capture scripts still fail closed when the executable, manifests, or
authoritative runtime adapter is unavailable. Every accepted capture preserves
command inputs, raw samples, execution IDs, revision/settings hashes, and image
evidence beside its output directory.

## Reproducible setup commands

From the repository root, initialise the pinned submodules and inspect the
manifests:

```powershell
git submodule update --init --recursive
Get-Content Tools/VirtualGeometry/workloads.json | ConvertFrom-Json
Get-Content Tools/VirtualGeometry/hardware-matrix.json | ConvertFrom-Json
Get-Content Tools/VirtualGeometry/quality-profiles.json | ConvertFrom-Json
```

The baseline build/test/install commands are documented in
`specs/045-virtual-geometry-alternative/quickstart.md` and are recorded as
instructions (not setup evidence) in `specs/045-virtual-geometry-alternative/evidence/setup-baseline.md`.

Capture and comparison tools accept only measured samples carrying a matching
`executionId`; five or more samples are required for aggregates. Unavailable,
zero-sample, or mismatched evidence is reported as incomplete and never as a
performance claim. Streaming and production soaks likewise require actual
elapsed duration and explicit bounded-counter evidence before becoming measured.

Installed-editor visual summaries can be converted without inventing timings:

```powershell
powershell -File Tools/VirtualGeometry/CaptureVirtualGeometryPerformance.ps1 `
  -VisualSummary out/editor/visual-summary-01.json,out/editor/visual-summary-02.json `
  -Output out/evidence/045/visual-capture.json
```

The normal installed-editor summary supplies
`postCaptureCapabilityEvidence.data.measurements[]`. Only rows with
`availability=measured`, a non-empty `executionEvidenceId`, and a positive
`durationNanoseconds` are accepted. A standalone adapter may instead supply
`rows[]`, but those rows must also set `postCaptureMeasured=true`. Duplicate
evidence does not count, and at least five measured rows are required.

## Phase 0-9 release gate

`RunVirtualGeometryPhaseGate.ps1` is the Vulkan-only installed-tree entry
point. It validates exactly one manifest for each phase 0 through 9, runs the
focused quickstart slices, codec/page experiment, route-image comparisons,
full-reference and million-decision prerequisites, vendor matrix, benchmark
comparison, migration, install/package inventory, and installed-editor MCP
scenarios. Every command record includes the executable, argument, stdout, and
stderr SHA-256 values plus a unique execution identity. JSON inputs without an
accepted status and execution identity remain incomplete.

The complete invocation supplies the artifacts that cannot be derived from a
fallback editor capture:

```powershell
powershell -NoProfile -ExecutionPolicy Bypass -File `
	Tools/VirtualGeometry/RunVirtualGeometryPhaseGate.ps1 `
	-CodecInputMesh <compiled-corpus-mesh.temf> `
	-HardwareImage <hardware.png> `
	-ComputeImage <compute.png> `
	-HybridImage <hybrid.png> `
	-IndexedImage <indexed.png> `
	-FullReferenceEvidence <accepted-full-reference.json> `
	-MillionDecisionEvidence <accepted-million-decision.json> `
	-BenchmarkBaseline <measured-baseline.json> `
	-BenchmarkCandidate <measured-candidate.json> `
	-MigrationInput <legacy-cache-manifest.json>
```

Outputs are rooted at `out/evidence/045/phase-gate/phase-0` through
`phase-9`, with the aggregate `phase-gate.json` and `release-gates.json` beside
them. Missing AMD, NVIDIA, or Intel runner hardware is recorded as
`unavailable`; it is never converted to a parity pass. `-DryRun` validates the
Vulkan-only orchestration without launching tests, tools, or the editor.
