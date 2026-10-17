# Readback receipt protocol fixtures

These files are synthetic parser inputs. They do not contain GPU, editor,
watchdog-launch, or independent-decoder acceptance evidence. The tiny PNG is a
real checksum-valid 3×2 RGBA control; a second 2×3 control permits alternating
extents. Their surrounding process receipts are mocks.

Run the scoped protocol suite from the repository root with bundled PowerShell 7:

```powershell
pwsh -NoProfile -File Tests/Tools/ReadbackLifetime/TestReadbackLifetimeRunner.ps1 `
	-WorkRoot C:/Temp/readback-protocol-fresh
```

The output directory must not exist. The test creates explicitly fake installed
files beneath that directory, pins their exact bytes, and retains `result.json`.
It launches no native fixture or editor. A successful result sets
`nativeAcceptance=false`, `editorAcceptance=false`, and `realProcessesLaunched=false`.
Failures count only when the expected named protocol assertion fires; import,
missing-command and unrelated runtime errors fail the suite.

## Public validator interfaces

`ReadbackLifetimeProtocol.psm1` exports:

- `Test-ReadbackNativeReceipt $Receipt $Context [-ProtocolOnly]`
- `Test-ReadbackEditorReceipt $Receipt $Context [-ProtocolOnly]`

The context contains `protocolContext`, a unique runner-generated `runNonce`,
absolute disjoint `installRoot`/`workRoot`, `manifestPin`, `requiredArtifacts`,
`iterations`, `executablePath`, and `decoderExecutablePath`. Protocol contexts
cannot be promoted to real acceptance by removing a fixture label. The later
installed runner must construct the real context from its own process launch,
required installed artifact list and fresh work directory, never from an
untrusted receipt. These validators do not launch or attest a process themselves.

Pins have absolute `path`, lowercase SHA-256 `sha256`, and integer `bytes`.
Installed manifest `readback-lifetime-artifacts-v1` contains relative-path file
pins with the same SHA-256/byte fields. The manifest itself is separately pinned;
receipt `installManifestSha256` must match it. Every file is rehashed. Escaping,
duplicate, redirected, missing, changed or uninstalled evidence fails validation.

Both receipt schemas contain `status`, `protocolFixture`, `runNonce`,
`installManifestSha256`, `executablePath`, `processId`, `watchdogPin`,
`rawObservationsPin`, `iterations`, and complete `observations`. Status must be
`Passed`; a missing prerequisite remains `Incomplete` in the future runner and
cannot pass this validator. The caller must map incomplete runs to exit code 2,
failed runs to 1, and verified requested lanes to 0.

The pinned raw JSON contains `runNonce` and `observations`; its observations must
exactly equal the receipt rows. The pinned watchdog JSON must identify the same
PID, numeric exit code 0, and `timedOut=false`. Exact iteration identities cover
the full requested count; duplicate or absent iterations fail. Native context and
receipt additionally contain `iterationsPerScenario`, at least 100. The public
PowerShell runner's `-Iterations 100` means 100 of each native scenario and 300
native observations. The native fixture's `--iterations 300` means total
operations. Editor keeps 100 actual captures in one owned session.

## Native schema

`readback-lifetime-native-v1` requires actual adapter/driver/API identity and
enabled `VK_LAYER_KHRONOS_validation`, an installed callback, zero error records
and counts, and callback coverage through context shutdown. `evidenceKind` must
be `RealVulkan`; deterministic policy/device-loss simulations cannot satisfy it.

Pinned native process output uses `readback-lifetime-native-observations-v1`.
Its process identity, counts, origin, status, validation, adapter, completed
context teardown and gate semaphore counts must exactly match the final receipt.
The runner adds manifest/watchdog provenance after process exit; it cannot
replace native metadata with requested flags or copied fixture claims.

Each uniquely identified operation/session records these ordered observations:

1. `Submitted`: actual `vkQueueSubmit`, result `VK_SUCCESS` (0).
2. `NotReady`: actual `vkGetFenceStatus`, result `VK_NOT_READY` (1).
3. `ClientTerminal`: timeout/cancellation delivered exactly once.
4. `HostRelease`: actual `vkSignalSemaphore`, result `VK_SUCCESS`.
5. `Completed`: actual fence success for timeout/resize, and specifically actual
   successful `vkDeviceWaitIdle` for orderly shutdown after releasing the event.
6. `Retired`: exactly-once per-operation destruction and blocker retirement.

The gate is `kind=HostTimelineSemaphore`: an actual enabled timeline semaphore
object with `initialValue=0`, `waitValue=1`, and `signalValue=1`. Its actual
`vkQueueSubmit` wait uses `waitStageMask=4096` (TRANSFER), so readback copy cannot
execute until the host calls `vkSignalSemaphore`. Both
`timelineSemaphoreEnabled` and `timelineSemaphore` must be observed true. A
release guard precedes idle waits/unwind; the signal is not sent before submit.
The previous VkEvent wait/late host-signal design is rejected, including old
`vkCmdWaitEvents`/`vkSetEvent` receipts. There is no HOST source-stage claim in
this queue-semaphore gate contract.

Staging buffer, allocation, command buffer and fence stay retained
and undestroyed after terminal delivery, then retire once after native proof.
The source-image blocker stays active until retirement. The shared command pool
is a live context dependency, not a per-request object to destroy repeatedly.
`createdGateSemaphores`/`destroyedGateSemaphores` balance at their owning teardown.
Detailed native snapshots name these counters `gateCreated`, `gateSignaled` and
`gateDestroyed`; old event counter names cannot satisfy this schema.

The matrix contains exactly the trusted per-scenario count of timeout, resize
and orderly shutdown; one sample of each cannot satisfy the campaign. Resize follows gated
readback completion/retirement; attempting synchronous full resize while the
submission remains blocked can deadlock. No source image may die prematurely.

## Editor schema

`readback-lifetime-editor-v1` requires owned endpoint/project, unchanged authored
objects/scene/dirty state, matching before/after scene-file pins and normal close.
Both `game_view.capture` and `camera.capture_custom` must occur.

A separate `warmup` record uses the same complete capture/PNG/RPC/decoder shape
as an observation. It precedes the 100 measured captures and is not counted as a
campaign iteration. `resizeCount` equals the trusted 100 capture count;
`normalCloseCount` is exactly one, and `capturesBeforeNormalClose` equals 100.
These session facts and the warmup record must match pinned raw loopback output.
This establishes one owned normal close after all captures, not 100 editor
shutdowns or editor timeout-retention evidence.

Every capture and the warmup retain the actual native admission observation
`backendTarget`, joined exactly to the pinned MCP text envelope's
`data.backendTarget`. It has positive `rendererSessionGeneration` and
`targetGeneration`, positive `sourceWidth`/`sourceHeight`, and
`dedicatedCaptureTarget=true`. All captures keep the same renderer session and
strictly advance target generation. Each source extent differs from its prior
capture/warmup and equals the requested and independently decoded extent.
Game View requests use `freshness=nextFrame` so both operations address the
same dedicated target. Requested dimensions alone cannot prove a real resize.

Every row retains a pinned JSON-RPC request/response, exact request/response and
correlation identities, exactly one response, one PNG image item and one text
envelope. Actual transmitted Base64 bytes must hash to `imagePin`; request and
envelope extents agree. Both receipt and native text envelope require
`backendReadback=true` and `deterministicFallback=false`.

A separate pinned independent decoder process receipt matches its installed
executable, successful process exit, PNG hash, checksum verification and exact
requested dimensions. Its bytes must match the copied decoder fields. Parser
fixtures simulate this shape only; the later runner must invoke the installed
fixture's real bundled PNG inspection mode.

`invalid-receipts.json` supplies explicit mutations and expected assertion codes.
The suite also corrupts pinned RPC/decoder content to verify joins, checks killed
watchdogs and missing closure, and proves fixture data cannot pass real lanes.
