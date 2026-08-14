#include "TestHarness.h"

#include <fstream>
#include <sstream>
#include <string>

namespace TerraEngine::EditorTests
{
	namespace
	{
		std::string Read(const char* relativePath)
		{
			std::ifstream input(std::string(TERRAENGINE_SOURCE_DIR) + "/" + relativePath, std::ios::binary);
			std::ostringstream text;
			text << input.rdbuf();
			return text.str();
		}
	}

	void RegisterFramePublicationToolContractTests(TestRegistry& registry)
	{
		registry.Add("Editor.FramePublicationTools.ProfilerBenchmarkPinsIdentity", [](TestContext& context) {
			const std::string source = Read("Tests/Tools/Profiling/CaptureProfilerReworkBenchmark.ps1");
			context.Expect(source.find("projectHash") != std::string::npos
				&& source.find("sceneHash") != std::string::npos
				&& source.find("workloadHash") != std::string::npos
				&& source.find("cameraHash") != std::string::npos
				&& source.find("installedBuildHash") != std::string::npos
				&& source.find("installedPackageHash") != std::string::npos
				&& source.find("TerraEngine.dll") != std::string::npos
				&& source.find("durationSeconds") != std::string::npos,
				"Profiler benchmark identity must pin project, scene, workload, camera, build, and duration.");
			context.Expect(source.find("TerraEngineProfilerJournalV1") != std::string::npos
				&& source.find("CameraHash is required") != std::string::npos,
				"The benchmark must require the unambiguous V1 identity and explicit camera hash.");
			context.Expect(source.find("DurationSeconds -ge 60") != std::string::npos
				&& source.find("schemaVersion = 1") != std::string::npos
				&& source.find("format = 'TerraEngineProfilerJournalV1'") != std::string::npos
				&& source.find("canonicalDurationSeconds = 300") != std::string::npos
				&& source.find("MatchedRecordingComparison is the 60-second-per-leg overhead smoke") != std::string::npos
				&& source.find("frameWindowDeltaPercent = $frameWindowThresholdPercent") != std::string::npos
				&& source.find("recordingOverheadPercent = $recordingOverheadThresholdPercent") != std::string::npos
				&& source.find("openWindowOverheadPercent = $openWindowThresholdPercent") != std::string::npos,
				"The benchmark contract must distinguish the 60-second smoke from the five-minute gate and publish all thresholds.");
			context.Expect(source.find("profilerHealthCheckpoints") != std::string::npos
				&& source.find("collectionCostGrowthPercent") != std::string::npos
				&& source.find("profilerAccumulationPassed") != std::string::npos,
				"Canonical detailed recording must gate protected profiler accumulation cost, not raw workload drift.");
			context.Expect(source.find("MatchedWindowOpen canonical run is 300 seconds with the window opening at 240 seconds") != std::string::npos,
				"Late-window evidence must remain within the five-minute total runtime.");
			context.Expect(source.find("elseif($Mode -eq 'MatchedWindowOpen' -and $Runs -eq 1) { 2 }") == std::string::npos,
				"One requested late-window recording must not expand into two five-minute runs.");
			context.Expect(source.find("Resume refused: benchmark identity differs") != std::string::npos
				&& source.find("identityHash") != std::string::npos,
				"Resume must reject incomparable identity rather than replacing the prior manifest.");
			context.Expect(source.find("RegenerateEvidenceFromRuns") != std::string::npos
				&& source.find("Run summary hash mismatch") != std::string::npos
				&& source.find("Matched comparison requires exact D/R/R/D order") != std::string::npos
				&& source.find("Run identity mismatch") != std::string::npos
				&& source.find("lacks authoritative first/final windows") != std::string::npos
				&& source.find("every late-window run to contain the bounded timed-open measurement") != std::string::npos
				&& source.find("Collections.IDictionary") != std::string::npos,
				"Evidence regeneration must validate saved run identities without launching the editor.");
			context.Expect(source.find("persistenceOperationCount") != std::string::npos
				&& source.find("framesPerPersistenceOperation") != std::string::npos
				&& source.find("compressionRatio") != std::string::npos
				&& source.find("Get-OptionalUInt64") != std::string::npos,
				"Benchmark conversion must retain optional batching counters and leave absent values null.");
			context.Expect(source.find("$durationSlopePassed = $true") != std::string::npos
				&& source.find("$checkpointRows.Count -ge 4") != std::string::npos
				&& source.find("positive frame and collection deltas") != std::string::npos
				&& source.find("$finalDeltaProduced") != std::string::npos
				&& source.find("collectionCostGrowthPercent -gt 1.0") != std::string::npos
				&& source.find("backlogGrowthFrames -gt 0") != std::string::npos
				&& source.find("profilerAccumulationPassed") != std::string::npos
				&& source.find("windowMeasurement") != std::string::npos
				&& source.find("twoPercentGate") != std::string::npos
				&& source.find("finishMcpRoundTripMs") != std::string::npos
				&& source.find("finishInteractiveBlockMs = if($isDisabledMetric)") != std::string::npos
				&& source.find("if($isDisabledMetric)") != std::string::npos
				&& source.find("Disabled run must publish null finish timing values") != std::string::npos
				&& source.find("$timedWindowRows = @($openRows | Where-Object") != std::string::npos
				&& source.find("windowOverheadMeasured = $timedWindowRows.Count -gt 0") != std::string::npos,
				"Canonical stability must use checkpoint accumulation cost while late-window acceptance uses bounded within-run samples.");
			context.Expect(source.find("$paired.Count -eq 5") != std::string::npos
				&& source.find("$windowMeasurement.status = 'measured'") != std::string::npos
				&& source.find("Timed-open regeneration found an incomplete paired window") != std::string::npos
				&& source.find("$windowMeasurement.status = 'unavailable'") != std::string::npos
				&& source.find("MatchedWindowOpen recording row requires measured windowMeasurement") != std::string::npos
				&& source.find("Validate-BenchmarkEvidenceEnvelope") != std::string::npos
				&& source.find("$writtenEvidence = Get-Content") != std::string::npos,
				"Timed-open evidence regeneration must validate all five pairs before normalizing status.");
			const std::string runner = Read("Tests/Tools/FramePublication/CaptureFramePublicationPerformance.ps1");
			const std::string live = Read("Tests/Tools/VirtualGeometry/CaptureVirtualGeometryLiveVisual.ps1");
			const std::string benchmark = Read("Tests/Tools/Profiling/CaptureProfilerReworkBenchmark.ps1");
			const std::string idle = Read("Tests/Tools/Profiling/MeasureProfilerIdleOverhead.ps1");
			context.Expect(runner.find("Get-EngineCpuWindow") != std::string::npos
				&& runner.find("engine-cpu-time") != std::string::npos
				&& runner.find("exact first/final Engine CPU windows") != std::string::npos
				&& runner.find("else { Get-PropertyValue $profilerSummary 'firstWindow' }") != std::string::npos
				&& runner.find("else { Get-PropertyValue $profilerSummary 'finalWindow' }") != std::string::npos
				&& runner.find("Get-PropertyValue $m.frameTiming") == std::string::npos
				&& runner.find("ProfilerMatchedOverheadMeasurement") != std::string::npos
				// Keep this source contract tied to the runner's explicit cadence
				// validation, rather than an older shortened diagnostic fragment.
				&& runner.find("Matched recording overhead samples do not have the required 100 ms cadence.") != std::string::npos
				&& runner.find("Test-BoundedSampleCadence") != std::string::npos
				&& runner.find("Get-BoundedTimingBlocks $overheadEvidence 6") != std::string::npos
				&& runner.find("six complete matched overhead blocks") != std::string::npos
				&& runner.find("comparisonQuartetIndex") != std::string::npos
				&& runner.find("overheadTiming = if($null -ne $overheadTiming)") != std::string::npos
				&& benchmark.find("recordingMinusControlSlopeMedianPercent") != std::string::npos
				&& benchmark.find("recordingMinusControlSlopeP95Percent") != std::string::npos,
				"Disabled controls must report Engine CPU windows and recording-attributable slope fields.");
			context.Expect(benchmark.find("overheadMedianMs") != std::string::npos
				&& benchmark.find("overheadP95Ms") != std::string::npos
				&& benchmark.find("Matched recording row lacks six complete closed 100 ms overhead blocks") != std::string::npos,
				"Matched recording overhead gates must use the bounded closed-window stream.");
			context.Expect(live.find("ProfilerMatchedOverheadMeasurement") != std::string::npos
				&& live.find("profiler-matched-recording-overhead") != std::string::npos
				&& live.find("sampleIntervalMilliseconds = 100") != std::string::npos
				&& live.find("timingSource = 'Engine::Update editor-excluded wall duration'") != std::string::npos,
				"The live adapter must publish the matched closed-window Engine CPU stream while recording.");
			context.Expect(runner.find("Get-PropertyValue $profilerSummary 'batching'") != std::string::npos
				&& runner.find("logicalInputBytes = $logicalInputBytes") != std::string::npos
				&& benchmark.find("summaryBatching") != std::string::npos
				&& benchmark.find("maximumPendingBytes = Get-OptionalUInt64 $health 'pendingBytes'") != std::string::npos,
				"Journal batching counters must fall back to the completed summary while pending bytes remain health-derived.");
			context.Expect(idle.find("passed = $deltaPercent -le 2.0") != std::string::npos
				&& idle.find("$aggregatePassed = $aggregateDeltaPercent -le 2.0") != std::string::npos
				&& idle.find("[Math]::Abs($deltaPercent)") == std::string::npos
				&& idle.find("[Math]::Abs($aggregateDeltaPercent)") == std::string::npos,
				"Open-idle acceptance must use signed one-sided overhead deltas.");
			context.Expect(runner.find("OpenProfilerAtSeconds") != std::string::npos
				&& runner.find("ProfilerWindowOpenAtSeconds") != std::string::npos
				&& runner.find("healthAfter") != std::string::npos
				&& runner.find("window = $windowOpenEvidence") != std::string::npos
				&& runner.find("windowMeasurement =") != std::string::npos
				&& benchmark.find("benchmark-evidence.json") != std::string::npos
				&& benchmark.find("windowOpenLatencyMs") != std::string::npos
				&& live.find("ProfilerWindowOpenAtSeconds") != std::string::npos
				&& live.find("profiler.window.visibility.set") != std::string::npos
				&& live.find("Get-PropertyValue $timing.data 'engineCpuTimeMs'") != std::string::npos
				&& live.find("Get-PropertyValue $timing.data 'engineCpuTimeValid'") != std::string::npos
				&& live.find("$values.Count -lt 30") != std::string::npos
				&& live.find("Start-Sleep -Milliseconds 1000") != std::string::npos
				&& live.find("$windowPair -lt 5") != std::string::npos
				&& live.find("latencyMilliseconds") != std::string::npos
				&& live.find("medianDeltaPercent") != std::string::npos
				&& live.find("p95DeltaPercent") != std::string::npos
				&& live.find("$sceneIncludeComponents = [string]::IsNullOrWhiteSpace($ScenePath)") != std::string::npos
				&& live.find("includeComponents = $sceneIncludeComponents") != std::string::npos
				&& live.find("offset = $sceneOffset") != std::string::npos
				&& live.find("limit = 256") != std::string::npos
				&& live.find("nextOffset") != std::string::npos
				&& live.find("scenePageCount -gt 100000") != std::string::npos
				&& live.find("index remained queued") != std::string::npos
				&& live.find("indexDeadline") != std::string::npos
				&& live.find("pairedWindows") != std::string::npos
				&& live.find("maxVisibilityLatencyMilliseconds") != std::string::npos
				&& live.find("[double]$_.latencyMilliseconds") != std::string::npos
				&& live.find("$closedDeadline") != std::string::npos
				&& live.find("$openDeadline") != std::string::npos
				&& live.find("$idleBlockCount") != std::string::npos
				&& live.find("$idleConditionSamples") != std::string::npos
				&& live.find("AddRange") != std::string::npos
				&& live.find("pairedBlockDeltas") != std::string::npos
				&& live.find("A single-condition idle sample has no matched counterpart") != std::string::npos
				&& live.find("Interleaved profiler idle blocks are missing") != std::string::npos
				&& idle.find("paired-block deltas") != std::string::npos
				&& idle.find("$pairEvidenceForPair") != std::string::npos
				&& idle.find("pairedBlockCount") != std::string::npos
				&& live.find("metric = 'engine-cpu-time'") != std::string::npos,
				"Late-window mode must keep the window closed initially and schedule an explicit visibility operation.");
		});

		registry.Add("Editor.FramePublicationTools.LegacyProfilerAuditIsFailClosed", [](TestContext& context) {
			const std::string source = Read("Tests/Tools/Profiling/CheckProfilerSource.ps1");
			const std::string legacyField = "payload" "Version";
			context.Expect(source.find("ProfilerCapture" "V2") != std::string::npos
				&& source.find(legacyField) != std::string::npos
				&& source.find("throw \"Profiler source audit failed") != std::string::npos,
				"The profiler source audit must reject legacy reader/writer and payload symbols.");
		});

		registry.Add("Editor.FramePublicationTools.CleanV1Documentation", [](TestContext& context) {
			const std::string readme = Read("Tests/Tools/FramePublication/README.md");
			const std::string testing = Read("Docs/Testing.md");
			const std::string legacySelector = "Capture" "Version v2";
			context.Expect(readme.find("TerraEngineProfilerJournalV1") != std::string::npos
				&& readme.find("CheckProfilerSource.ps1") != std::string::npos
				&& testing.find("profiler rework V1") != std::string::npos
				&& testing.find("unsupported and are never migrated") != std::string::npos,
				"Profiler tooling documentation must describe clean V1-only behavior and its audit command.");
			context.Expect(testing.find(legacySelector) == std::string::npos,
				"Testing documentation must not prescribe the removed legacy capture selector.");
		});
	}
}
