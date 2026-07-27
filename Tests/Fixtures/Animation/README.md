# Animation Fixture Corpus

This directory documents TerraEngine's deterministic, repository-owned
animation fixtures. The corpus is generated offline by
`Tools/Animation/GenerateAnimationFixtures.ps1`; it never downloads a model,
reads a DCC installation, or depends on a machine-specific source asset.

## Generate a disposable corpus

The output root is mandatory. It must be a new or empty directory strictly
below the repository root. The generator does not delete or overwrite existing
content, and every path it creates is checked to remain below that explicit
root.

```powershell
powershell.exe -NoProfile -ExecutionPolicy Bypass `
	-File Tools/Animation/GenerateAnimationFixtures.ps1 `
	-OutputRoot out/validation/animation-fixtures/run-a
```

For a checked-in snapshot, generate into a new child such as
`Tests/Fixtures/Animation/Generated`, review the complete manifest and diff,
then add the files intentionally. Do not point `-OutputRoot` at this directory
while `README.md` is present because non-empty roots are rejected.

## What is generated

The corpus contains:

- `contracts/runtime-limits-v1.json`, which records every operational and hard
  version-1 limit from the frozen validation contract;
- `contracts/validation-tolerances-v1.json`, which records the exact and
  tolerance-based comparison rules used by later fixture tests;
- `valid/*.fixture.json`, a minimal source-independent skeleton pair, clip,
  controller, mask, retarget profile, motion database, ragdoll profile,
  recording, deformable mesh, and atomic runtime-set description;
- `seeds/truncation/`, with named truncated header, dependency-table, and
  payload cases;
- `seeds/range/`, with checked-range, 64-bit arithmetic, and every-hard-limit
  overflow cases;
- `seeds/versioning/`, with bad magic, unsupported major version, unknown
  required chunk, and skippable optional-minor-chunk cases;
- `seeds/dependency/`, with missing, incompatible, cyclic, and corrupt
  transitive dependency cases;
- `seeds/corruption/`, with hash, non-finite transform, hierarchy, key-time,
  and skin-weight corruption cases; and
- `seeds/lifecycle/`, with cancellation, stale source generation, and stale
  runtime completion cases.

`manifest.json` lists every payload file in stable path order with byte count,
SHA-256, fixture identifier, role, and requirement tags. Its
`contentSetSha256` hashes that ordered entry list. The manifest intentionally
does not hash itself, avoiding a circular self-hash.

The operations in `every-hard-limit-overflow.seed.json` are independent test
cases. Consumers apply one operation at a time to the otherwise-valid semantic
set so a rejection proves the named limit rather than an earlier limit in the
same document.

## Production format boundary

The concrete animation envelope, numeric magic values, chunk identifiers, and
binary structures are implementation-owned and are not frozen yet. The valid
fixtures therefore use reviewed semantic JSON and name their intended future
extension, such as `.teskel` or `.teanim`. A later format test must pass the
semantic payload through the real production writer before exercising the real
reader.

Corruption seeds are also layout-independent. They describe symbolic
operations such as `replace-envelope-magic`, `truncate-inside-dependency-table`,
or `advance-source-generation-before-publish`. Production-aware test helpers
resolve those operations only after serialization. Never rename a JSON fixture
to a cooked extension or interpret a symbolic boundary as a byte offset.

This boundary lets the fixture names and expected outcomes remain stable while
the production format is implemented and reviewed. Once a binary contract is
frozen, tests may add hash-locked converted artifacts without changing the
semantic source of truth.

## Verify determinism and hashes

Generate two fresh roots, parse every JSON document, compare relative-file
hashes, and confirm the content-set digest matches:

```powershell
$roots = @(
	"out/validation/animation-fixtures/run-a",
	"out/validation/animation-fixtures/run-b"
)
foreach ($root in $roots) {
	powershell.exe -NoProfile -ExecutionPolicy Bypass `
		-File Tools/Animation/GenerateAnimationFixtures.ps1 `
		-OutputRoot $root
	Get-ChildItem -LiteralPath $root -Filter *.json -File -Recurse |
		ForEach-Object { $null = Get-Content -Raw -LiteralPath $_.FullName | ConvertFrom-Json }
}

$first = Get-Content -Raw -LiteralPath "$($roots[0])/manifest.json" | ConvertFrom-Json
$second = Get-Content -Raw -LiteralPath "$($roots[1])/manifest.json" | ConvertFrom-Json
if ($first.contentSetSha256 -cne $second.contentSetSha256) {
	throw "Animation fixture generation was not deterministic."
}
```

Consumers must additionally recompute each manifest entry's SHA-256 before
using it. A mismatch is a failed fixture setup, not a reason to update the
expected hash automatically. Hash changes require review of the semantic
fixture, generator version, affected requirements, and later acceptance
evidence.

## Safety and boundedness

The generator writes UTF-8 without a byte-order mark, normalizes JSON newlines
to LF, and includes no time, absolute output path, locale-dependent value, or
random identifier. It fails closed above 128 files, 256 KiB per file, or 2 MiB
for the complete generated corpus. It stages files below the requested root and
publishes them only after hashes and size bounds pass.

Cancellation and stale-generation seeds drive the import/publication/runtime
state machines; they are not malformed files. Their required outcome is an
atomic rejection with no partial publication and preservation of the prior
valid generation or documented safe fallback.
