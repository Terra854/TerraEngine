# RunAnimationValidationGate.ps1 - Fail-closed installed animation phase gate.
#
# The gate deliberately keeps every generated project, preference, log, capture,
# and result below out/validation/animation/. It never launches an editor chosen
# by the caller: the executable is resolved only from the supplied install tree.
# A normal run is intentionally opt-in because it performs an integrated build,
# focused CTest pass, install, and live Vulkan editor session.
#
# Static validation without starting a process:
#   powershell.exe -NoProfile -ExecutionPolicy Bypass -File Tools/Animation/RunAnimationValidationGate.ps1 `
#     -Phase setup -GateTask AnimationValidationValidateSetupScriptSchemasBaselineIdentityHashesMissingMetricRejection -DryRun
# Focused camera-content validation without a build or editor launch:
#   powershell.exe -NoProfile -ExecutionPolicy Bypass -File Tools/Animation/TestAnimationValidationGateVisualEvidence.ps1
#
# Live runs provide a fixture, scenario, and acceptance update manifest. An
# intermediate gate supplies explicit deferral authority; a final gate supplies
# both AnimationValidationRunsEveryAccumulatedFeatureWorkflowBenchmarkTRunsEveryUnrelated animation evidence and the AnimationValidationInvokeFinalOnlyTTComparatorEveryPreparedControlIncluding candidate before passing -Execute.

[CmdletBinding()]
param(
	[Parameter(Mandatory = $true)]
	[ValidatePattern("^(setup|foundation|us[0-9]+(?:-[a-z0-9-]+)?|final(?:-[a-z0-9-]+)?)$")]
	[string]$Phase,

	[ValidatePattern("^T[0-9]{3}$")]
	[string]$GateTask = "AnimationValidationTGateOrchestratorInstalledEditorLaunchIsolatedFixtureCopiesMcp",

	[ValidateNotNullOrEmpty()]
	[string]$InstallRoot = "out/build/RelWithDebInfo/Build",

	[ValidateNotNullOrEmpty()]
	[string]$BuildDirectory = "out/build/RelWithDebInfo",

	[string]$FixtureRoot = "",
	[string]$ScenarioManifest = "",
	[string]$PerformanceCandidateManifest = "",
	[string]$FinalAnimationPerformanceEvidenceManifest = "",
	[string]$AcceptanceUpdateManifest = "",
	[switch]$DeferPerformanceExecutionToFinalIntegration,
	[string]$PerformanceDeferralAuthority = "",

	[ValidateNotNullOrEmpty()]
	[string]$EvidenceRoot = "out/validation/animation",

	[ValidatePattern("^[A-Za-z0-9][A-Za-z0-9._-]{0,95}$")]
	[string]$RunId = ([DateTime]::UtcNow.ToString("yyyyMMddTHHmmssfffZ")),

	[ValidateNotNullOrEmpty()]
	[string]$FocusedTestRegex = "Animation|AssetPipeline|SystemScheduler|TransactionalMutation|Scripting|Physics|Renderer|VirtualGeometry|Automation|Export",

	[ValidateRange(1024, 65535)]
	[int]$McpPort = 43143,

	[ValidateRange(60, 14400)]
	[int]$BuildTimeoutSeconds = 7200,

	[ValidateRange(30, 7200)]
	[int]$TestTimeoutSeconds = 3600,

	[ValidateRange(60, 7200)]
	[int]$InstallTimeoutSeconds = 3600,

	[ValidateRange(30, 600)]
	[int]$EditorStartupTimeoutSeconds = 180,

	[ValidateRange(60, 3600)]
	[int]$ManualEvidenceTimeoutSeconds = 900,

	[switch]$ContinueAfterNativeInputException,
	[string]$NativeInputExceptionAuthority = "",

	[switch]$Execute,
	[switch]$DryRun
)

Set-StrictMode -Version Latest
$ErrorActionPreference = "Stop"
if (Test-Path variable:PSNativeCommandUseErrorActionPreference) {
	# Native exit codes are inspected after stdout and stderr are durably saved.
	$PSNativeCommandUseErrorActionPreference = $false
}

$script:FeatureName = "complete-animation-system"
$script:Configuration = "RelWithDebInfo"
$script:ReferenceWidth = 1920
$script:ReferenceHeight = 1080
$script:ReferenceWorkerCount = 31
$script:ExpectedBaselineDigest = "4020949bd881f94f0395853e947e4512fb587c7bdd1ce5592317e75c01b5fcb5"
$script:ExpectedBaselineManifestSha256 = "cf4375934729e1afa745d9446faacfc1a7ba53a6d7db79b3d1e0e8164a5ced8f"
$script:ExpectedBaselineEvidenceLockSha256 = "0605b4d4241bda8ba3fd4743b2ba7a6c6b217aaf1c9d83fe669125493ddb4e41"
$script:ExpectedRuntimeContractSha256 = "a2dd5eee95aa1590a927094006d120d2d67f39d2166605f27902dde85dc3566a"
$script:ExpectedRendererWorkloadManifestSha256 = "3230959fab8c628d9396bd31a7923672c3a1ebf5b51ff4998066a8f10c2b3e4c"
$script:ExpectedPerformanceWorkloadsSha256 = "d99199c796368fa7706af363985ce3e0b75a71fa5fa2c29238bce65343e943a6"
$script:DeferredPerformanceCTestExcludeRegex = "Performance|^Editor\.ExportBaseline$"
# Several broad CTest registrations contain both correctness cases and measured
# workloads. Exact case-name tokens keep correctness/schema coverage active while
# ensuring no wall-clock, throughput, size, or production-load gate runs early.
$script:DeferredPerformanceHarnessExcludeFilter = @(
	'Animation.Performance'
	'Animation.Benchmark'
	# The frozen crowd manifest is part of the deferred AnimationValidationRunsEveryAccumulatedFeatureWorkflowBenchmarkTRunsEveryUnrelated acceptance profile;
	# its reviewed hash must not be rebased while performance work is postponed.
	'Animation.CrowdPerformance.FrozenManifestAndAuthoritiesMatchBytes'
	'Engine.Profiler.EditorWorkIsExcludedFromGameProfiler'
	'Engine.Profiler.ViewportCpuUsesMeasuredProfilerScopes'
	'Engine.ProfilerCapturePerformance.FiveSecondAggregateStaysUnderTenMiB'
	'VirtualGeometry.Batching.DenseSelectedClustersCollapseIntoBoundedBatches'
	'VirtualGeometry.Batching.Regression.PageLocalPacketsShareOneDrawRunAcrossBindings'
	'VirtualGeometry.Batching.Regression.AntiqueCameraLeafCutCollapsesToOneDraw'
	'VirtualGeometry.Batching.GeneratedSubmissionsReduceDenseClusterExpansion'
	'VirtualGeometry.Stress.DenseSceneCameraMotionKeepsMeshletWorkBounded'
	'Physics.DeterminismStress.ProcessLaunchSeedMatchesAcrossOneTwoFourAndMaxWorkers'
	'Physics.Editor.Performance'
	'Editor.ProjectReadiness.LargeSceneColdWarmTimingGate'
) -join ';'
$script:DeferredPerformanceSuccessCriteria = @('animation-outcome-creator-import-representative-animated-gltf-fbx-package-obtain-correctly-referenced-playable-prefab', 'animation-outcome-creator-build-preview-save-reload-run-locomotion-plus-action-controller-nested-states', 'animation-outcome-motion-matching-queries-over-reference-production-size-database-return-eligible-result-within', 'animation-outcome-reference-scene-containing-concurrently-active-joint-characters-two-blended-clips-sustains-least', 'animation-outcome-reference-hero-scene-combining-layered-graphs-morphs-ik-motion-matching-deformable-virtual', 'animation-outcome-optional-animation-profiling-detailed-diagnostics-disabled-versus-enabled-but-collapsed-differ-no', 'animation-outcome-character-reference-scene-gpu-visual-mode-reduces-animation-related-cpu-evaluation-time', 'animation-outcome-instrumented-ordinary-gpu-visual-hybrid-authoritative-frames-perform-zero-complete-pose-gpu', 'animation-outcome-gpu-visual-hybrid-authoritative-poses-match-cpu-reference-poses-within-declared-visual', 'animation-outcome-ten-thousand-forced-transitions-among-gpu-visual-hybrid-authoritative-cpu-deterministic-fallback', 'animation-outcome-gpu-motion-matching-scoring-reduces-animation-related-cpu-search-time-least-reference')
$script:DeferredPerformanceAcceptanceRows = @('animation-contract-avoid-creating-new-persistent-graphics-setup-state-animator-during-steady-state-frames', 'animation-contract-unchanged-animators-avoid-unnecessary-evaluation-publication-upload-visibility-invalidation-detailed-diagnostic-work', 'animation-contract-performance-sensitive-caches-include-pose-morph-graph-view-asset-residency-material-renderer', 'animation-contract-profiling-expose-named-animation-phases-active-execution-modes-cpu-gpu-workload-worker', 'animation-contract-optional-profiling-diagnostics-overlays-recording-evidence-capture-remain-bounded-impose-material-overhead', 'animation-contract-feature-include-reproducible-correctness-corruption-rollback-scheduler-transaction-multithreaded-stress-soak-performance', 'animation-contract-feature-complete-until-required-capabilities-specification-pass-integrated-compilation-full-regression-testing', 'animation-outcome-required-animation-workflows-configured-built-tested-installed-launched-reproduced-clean-checkout-using') + $script:DeferredPerformanceSuccessCriteria
$script:OutstandingFinalPerformanceTasks = @('AnimationValidationRunsEveryAccumulatedFeatureWorkflowBenchmarkTRunsEveryUnrelated', 'AnimationValidationInvokeFinalOnlyTTComparatorEveryPreparedControlIncluding', 'AnimationValidationRepairCapableFinalChecksTTAreCleanFinalBlocking')
# Scenario overlays are deliberately small and bounded. They let a live gate add
# reviewed cooked assets to its disposable project without silently turning the
# scenario manifest into an unrestricted repository copy mechanism.
$script:MaximumFixtureOverlayCount = 16
$script:MaximumFixtureOverlayFiles = 4096
$script:MaximumFixtureOverlayBytes = [UInt64]1073741824
$script:MaximumArgumentBindingsPerOperation = 16
# The complete Animation Automation Validation/Animation Validation lifecycle scenario legitimately binds more than 64
# reviewed scalar values.  Keep a bounded cap while allowing the full, already
# declared phase contract plus its approval-time recording end-tick bindings.
$script:MaximumArgumentBindingsPerScenario = 160
$script:MaximumArgumentBindingPathSegments = 8
# Only reviewed animation mutations use the phase-gate approval marker. Keeping
# this list exact prevents a read/capture or an unknown tool from laundering a
# successful expectation through an unrelated UI approval.
$script:HighImpactScenarioTools = @(
	'animation.asset.import',
	'animation.asset.reimport',
	'animation.motion_database.build',
	'animation.recording.start',
	'animation.recording.stop'
)
$script:ApprovalPollIntervalMilliseconds = 500
# Recording proof is creation-only and deliberately narrow. The project snapshot
# bounds prevent a malformed disposable fixture from turning validation into an
# unbounded filesystem walk or hash workload.
$script:MaximumPlayModeRecordingOutputs = 16
$script:MaximumPlayModeRecordingOutputBytes = [UInt64]536870912
$script:MaximumPlayModeSnapshotFiles = 100000
$script:MaximumPlayModeSnapshotDirectories = 50000
$script:MaximumPlayModeSnapshotBytes = [UInt64]34359738368
$script:PlayModeAuthoredExcludedTopLevelDirectories = @('Build', 'Library', 'Logs', 'Temp', 'UserSettings', 'bin', 'obj', '.git', 'out')
# A successful package import can leave ordinary file-watcher notifications in
# flight briefly after its MCP receipt is returned. The malformed-reimport
# proof must snapshot an already-settled last-known-good generation, otherwise
# one of those earlier notifications can be mistaken for failed-import output.
$script:ScenarioPreservationStableSamples = 3
$script:ScenarioPreservationStableIntervalMilliseconds = 250
$script:ScenarioPreservationStabilizationTimeoutMilliseconds = 10000
$script:Utf8NoBom = [System.Text.UTF8Encoding]::new($false)
$script:CommandRecords = [System.Collections.Generic.List[object]]::new()
$script:StageRecords = [System.Collections.Generic.List[object]]::new()
$script:McpSequence = 0
$script:McpTranscriptPath = $null
$script:McpEndpoint = $null

function Get-AbsolutePath {
	param(
		[Parameter(Mandatory = $true)][string]$Path,
		[Parameter(Mandatory = $true)][string]$BasePath
	)
	if ([System.IO.Path]::IsPathRooted($Path)) {
		return [System.IO.Path]::GetFullPath($Path)
	}
	return [System.IO.Path]::GetFullPath((Join-Path $BasePath $Path))
}

function Assert-PathBelow {
	param(
		[Parameter(Mandatory = $true)][string]$Parent,
		[Parameter(Mandatory = $true)][string]$Child,
		[Parameter(Mandatory = $true)][string]$Description
	)
	$parentPath = [System.IO.Path]::GetFullPath($Parent).TrimEnd('\', '/')
	$childPath = [System.IO.Path]::GetFullPath($Child)
	$prefix = $parentPath + [System.IO.Path]::DirectorySeparatorChar
	if (-not $childPath.StartsWith($prefix, [System.StringComparison]::OrdinalIgnoreCase)) {
		throw "$Description must remain below '$parentPath'; received '$childPath'."
	}
}

function Assert-PathAtOrBelow {
	param(
		[Parameter(Mandatory = $true)][string]$Parent,
		[Parameter(Mandatory = $true)][string]$Child,
		[Parameter(Mandatory = $true)][string]$Description
	)
	$parentPath = [System.IO.Path]::GetFullPath($Parent).TrimEnd('\', '/')
	$childPath = [System.IO.Path]::GetFullPath($Child).TrimEnd('\', '/')
	if ($childPath.Equals($parentPath, [System.StringComparison]::OrdinalIgnoreCase)) { return }
	Assert-PathBelow -Parent $parentPath -Child $childPath -Description $Description
}

function Test-PathBelow {
	param([string]$Parent, [string]$Child)
	try {
		Assert-PathBelow -Parent $Parent -Child $Child -Description "Path"
		return $true
	}
	catch {
		return $false
	}
}

function Get-RelativeForwardPath {
	param([string]$Parent, [string]$Child)
	$parentPath = [System.IO.Path]::GetFullPath($Parent).TrimEnd('\', '/')
	$childPath = [System.IO.Path]::GetFullPath($Child)
	Assert-PathBelow -Parent $parentPath -Child $childPath -Description "Relative evidence path"
	return $childPath.Substring($parentPath.Length + 1).Replace([System.IO.Path]::DirectorySeparatorChar, [char]'/')
}

function Write-Utf8Text {
	param([Parameter(Mandatory = $true)][string]$Path, [Parameter(Mandatory = $true)][AllowEmptyString()][string]$Value)
	$directory = Split-Path -Parent $Path
	if (-not [string]::IsNullOrWhiteSpace($directory)) {
		[System.IO.Directory]::CreateDirectory($directory) | Out-Null
	}
	[System.IO.File]::WriteAllText($Path, $Value, $script:Utf8NoBom)
}

function Write-JsonFile {
	param([Parameter(Mandatory = $true)][string]$Path, [Parameter(Mandatory = $true)]$Value)
	Write-Utf8Text -Path $Path -Value (($Value | ConvertTo-Json -Depth 80) + "`n")
}

function Add-JsonLine {
	param([Parameter(Mandatory = $true)][string]$Path, [Parameter(Mandatory = $true)]$Value)
	$directory = Split-Path -Parent $Path
	if (-not [string]::IsNullOrWhiteSpace($directory)) {
		[System.IO.Directory]::CreateDirectory($directory) | Out-Null
	}
	[System.IO.File]::AppendAllText($Path, (($Value | ConvertTo-Json -Depth 80 -Compress) + "`n"), $script:Utf8NoBom)
}

function Get-OptionalProperty {
	param($Object, [Parameter(Mandatory = $true)][string]$Name)
	if ($null -eq $Object) { return $null }
	$property = $Object.PSObject.Properties[$Name]
	if ($null -eq $property) { return $null }
	return $property.Value
}

function Get-RequiredJsonProperty {
	param(
		[Parameter(Mandatory = $true)]$Object,
		[Parameter(Mandatory = $true)][string]$Name,
		[Parameter(Mandatory = $true)][string]$Description
	)
	# ConvertFrom-Json returns PSCustomObject values. Looking up the property
	# metadata, instead of relying on PowerShell's null coercion, distinguishes a
	# missing required member from a member whose explicit value is null.
	$property = $Object.PSObject.Properties[$Name]
	if ($null -eq $property) { throw "$Description is missing required property '$Name'." }
	return $property.Value
}

function Test-IsJsonArray {
	param($Value)
	# This helper is used with JavaScriptSerializer values. Windows PowerShell's
	# ConvertFrom-Json flattens one-element arrays, so it cannot prove whether a
	# producer actually wrote the required JSON array syntax.
	return $Value -is [System.Array]
}

function Initialize-NativeValidationBridge {
	if ($null -ne ('TerraEngine.Animation.Validation.NativeWindowEvidence' -as [type])) { return }
	if ($PSVersionTable.PSEdition -eq 'Core' -and -not $IsWindows) {
		throw 'Installed-editor native evidence requires Windows.'
	}
	Add-Type -AssemblyName System.Drawing
	Add-Type -ReferencedAssemblies 'System.Drawing' -TypeDefinition @'
using System;
using System.Collections.Generic;
using System.ComponentModel;
using System.Diagnostics;
using System.Drawing;
using System.Drawing.Imaging;
using System.IO;
using System.Runtime.InteropServices;
using System.Text;
using System.Threading;

namespace TerraEngine.Animation.Validation
{
	public sealed class NativeWindowSnapshot
	{
		public int ProcessId { get; set; }
		public long WindowHandle { get; set; }
		public string WindowTitle { get; set; }
		public int Left { get; set; }
		public int Top { get; set; }
		public int PixelWidth { get; set; }
		public int PixelHeight { get; set; }
		public uint Dpi { get; set; }
		public bool DpiAwareCapture { get; set; }
		public string DpiAwarenessContext { get; set; }
		public bool PrintWindowSucceeded { get; set; }
		public int PrintWindowError { get; set; }
		public string CapturedAtUtc { get; set; }
	}

	public sealed class InputDesktopSnapshot
	{
		public bool DesktopOpened { get; set; }
		public string DesktopName { get; set; }
		public bool InputAvailable { get; set; }
		public bool SessionLocked { get; set; }
		public int WindowsError { get; set; }
		public string Reason { get; set; }
		public string ClassifiedAtUtc { get; set; }
	}

	public sealed class NativeClientSnapshot
	{
		public int ProcessId { get; set; }
		public long WindowHandle { get; set; }
		public int ScreenLeft { get; set; }
		public int ScreenTop { get; set; }
		public int PixelWidth { get; set; }
		public int PixelHeight { get; set; }
		public uint Dpi { get; set; }
		public bool DpiAwareGeometry { get; set; }
		public string DpiAwarenessContext { get; set; }
	}

	public sealed class NativeInputReceipt
	{
		public bool Succeeded { get; set; }
		public string Method { get; set; }
		public string RequestKind { get; set; }
		public int EventCountRequested { get; set; }
		public int EventCountSubmitted { get; set; }
		public int WindowsError { get; set; }
		public long ForegroundWindow { get; set; }
		public string DesktopName { get; set; }
		public bool InputAvailable { get; set; }
		public bool ForegroundAcquisitionAttempted { get; set; }
		public bool ForegroundAcquired { get; set; }
		public bool ForegroundAcquiredBeforeInput { get; set; }
		public bool ForegroundAcquiredByDeclaredMouseInput { get; set; }
		public int ForegroundAttempts { get; set; }
		public int PostInputForegroundChecks { get; set; }
		public long PreviousForegroundWindow { get; set; }
		public bool TargetWasMinimized { get; set; }
		public bool RestoreRequested { get; set; }
		public bool ZOrderRaiseAttempted { get; set; }
		public bool ZOrderRaised { get; set; }
		public bool ZOrderRestored { get; set; }
		public bool ThreadInputAttachmentUsed { get; set; }
		public int ThreadInputAttachAttempts { get; set; }
		public int ThreadInputAttachCount { get; set; }
		public int ThreadInputDetachAttempts { get; set; }
		public int ThreadInputDetachCount { get; set; }
		public bool ThreadInputDetached { get; set; }
		public int ThreadInputError { get; set; }
		public bool DpiAwareGeometry { get; set; }
		public string DpiAwarenessContext { get; set; }
		public uint Dpi { get; set; }
		public int ClientPixelWidth { get; set; }
		public int ClientPixelHeight { get; set; }
		public int RequestedClientX { get; set; }
		public int RequestedClientY { get; set; }
		public int ScreenX { get; set; }
		public int ScreenY { get; set; }
		public int PositionedScreenX { get; set; }
		public int PositionedScreenY { get; set; }
		public bool PhysicalCursorPositionVerified { get; set; }
		public string StartedAtUtc { get; set; }
		public string CompletedAtUtc { get; set; }
		public string FailureReason { get; set; }
	}

	public sealed class KillOnCloseProcessJob : IDisposable
	{
		private IntPtr jobHandle;

		internal KillOnCloseProcessJob(int processId)
		{
			jobHandle = NativeWindowEvidence.CreateKillOnCloseJob(processId);
		}

		public bool Terminate(uint exitCode)
		{
			if (jobHandle == IntPtr.Zero)
				return true;
			return NativeWindowEvidence.TerminateJobObject(jobHandle, exitCode);
		}

		public void Dispose()
		{
			if (jobHandle != IntPtr.Zero)
			{
				NativeWindowEvidence.CloseHandle(jobHandle);
				jobHandle = IntPtr.Zero;
			}
			GC.SuppressFinalize(this);
		}

		~KillOnCloseProcessJob()
		{
			Dispose();
		}
	}

	public static class NativeWindowEvidence
	{
		private const uint PW_RENDERFULLCONTENT = 0x00000002;
		private const uint DESKTOP_READOBJECTS = 0x0001;
		private const uint DESKTOP_SWITCHDESKTOP = 0x0100;
		private const int UOI_NAME = 2;
		private const uint INPUT_MOUSE = 0;
		private const uint INPUT_KEYBOARD = 1;
		private const uint MOUSEEVENTF_LEFTDOWN = 0x0002;
		private const uint MOUSEEVENTF_LEFTUP = 0x0004;
		private const uint MOUSEEVENTF_RIGHTDOWN = 0x0008;
		private const uint MOUSEEVENTF_RIGHTUP = 0x0010;
		private const uint KEYEVENTF_KEYUP = 0x0002;
		private const int SW_RESTORE = 9;
		private const uint SWP_NOSIZE = 0x0001;
		private const uint SWP_NOMOVE = 0x0002;
		private const uint SWP_NOACTIVATE = 0x0010;
		private const uint SWP_SHOWWINDOW = 0x0040;
		private const uint JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE = 0x00002000;
		private const int JobObjectExtendedLimitInformation = 9;
		private const uint PROCESS_TERMINATE = 0x0001;
		private const uint PROCESS_SET_QUOTA = 0x0100;
		private const uint PROCESS_QUERY_LIMITED_INFORMATION = 0x1000;
		private static readonly IntPtr DPI_AWARENESS_CONTEXT_UNAWARE = new IntPtr(-1);
		private static readonly IntPtr DPI_AWARENESS_CONTEXT_SYSTEM_AWARE = new IntPtr(-2);
		private static readonly IntPtr DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE = new IntPtr(-3);
		private static readonly IntPtr DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2 = new IntPtr(-4);
		private static readonly IntPtr DPI_AWARENESS_CONTEXT_UNAWARE_GDISCALED = new IntPtr(-5);
		private static readonly IntPtr HWND_TOPMOST = new IntPtr(-1);
		private static readonly IntPtr HWND_NOTOPMOST = new IntPtr(-2);

		[StructLayout(LayoutKind.Sequential)]
		private struct RECT
		{
			public int Left;
			public int Top;
			public int Right;
			public int Bottom;
		}

		[StructLayout(LayoutKind.Sequential)]
		private struct POINT
		{
			public int X;
			public int Y;
		}

		private struct THREAD_INPUT_PAIR
		{
			public uint SourceThread;
			public uint TargetThread;
		}

		[StructLayout(LayoutKind.Sequential)]
		private struct MOUSEINPUT
		{
			public int dx;
			public int dy;
			public uint mouseData;
			public uint dwFlags;
			public uint time;
			public IntPtr dwExtraInfo;
		}

		[StructLayout(LayoutKind.Sequential)]
		private struct KEYBDINPUT
		{
			public ushort wVk;
			public ushort wScan;
			public uint dwFlags;
			public uint time;
			public IntPtr dwExtraInfo;
		}

		[StructLayout(LayoutKind.Explicit)]
		private struct INPUTUNION
		{
			[FieldOffset(0)]
			public MOUSEINPUT mouse;
			[FieldOffset(0)]
			public KEYBDINPUT keyboard;
		}

		[StructLayout(LayoutKind.Sequential)]
		private struct INPUT
		{
			public uint type;
			public INPUTUNION data;
		}

		[StructLayout(LayoutKind.Sequential)]
		private struct JOBOBJECT_BASIC_LIMIT_INFORMATION
		{
			public long PerProcessUserTimeLimit;
			public long PerJobUserTimeLimit;
			public uint LimitFlags;
			public UIntPtr MinimumWorkingSetSize;
			public UIntPtr MaximumWorkingSetSize;
			public uint ActiveProcessLimit;
			public UIntPtr Affinity;
			public uint PriorityClass;
			public uint SchedulingClass;
		}

		[StructLayout(LayoutKind.Sequential)]
		private struct IO_COUNTERS
		{
			public ulong ReadOperationCount;
			public ulong WriteOperationCount;
			public ulong OtherOperationCount;
			public ulong ReadTransferCount;
			public ulong WriteTransferCount;
			public ulong OtherTransferCount;
		}

		[StructLayout(LayoutKind.Sequential)]
		private struct JOBOBJECT_EXTENDED_LIMIT_INFORMATION
		{
			public JOBOBJECT_BASIC_LIMIT_INFORMATION BasicLimitInformation;
			public IO_COUNTERS IoInfo;
			public UIntPtr ProcessMemoryLimit;
			public UIntPtr JobMemoryLimit;
			public UIntPtr PeakProcessMemoryUsed;
			public UIntPtr PeakJobMemoryUsed;
		}

		[DllImport("user32.dll", SetLastError = true)]
		private static extern bool IsWindow(IntPtr window);
		[DllImport("user32.dll", SetLastError = true)]
		private static extern uint GetWindowThreadProcessId(IntPtr window, out uint processId);
		[DllImport("user32.dll", CharSet = CharSet.Unicode, SetLastError = true)]
		private static extern int GetWindowTextW(IntPtr window, StringBuilder text, int maximumCount);
		[DllImport("user32.dll", SetLastError = true)]
		private static extern bool GetWindowRect(IntPtr window, out RECT rectangle);
		[DllImport("user32.dll", SetLastError = true)]
		private static extern bool GetClientRect(IntPtr window, out RECT rectangle);
		[DllImport("user32.dll", SetLastError = true)]
		private static extern bool ClientToScreen(IntPtr window, ref POINT point);
		[DllImport("user32.dll", SetLastError = true)]
		private static extern bool PrintWindow(IntPtr window, IntPtr deviceContext, uint flags);
		[DllImport("user32.dll")]
		private static extern uint GetDpiForWindow(IntPtr window);
		[DllImport("user32.dll", SetLastError = true)]
		private static extern IntPtr SetThreadDpiAwarenessContext(IntPtr dpiContext);
		[DllImport("user32.dll")]
		private static extern IntPtr GetThreadDpiAwarenessContext();
		[DllImport("user32.dll")]
		private static extern bool AreDpiAwarenessContextsEqual(IntPtr first, IntPtr second);
		[DllImport("user32.dll", SetLastError = true)]
		private static extern IntPtr OpenInputDesktop(uint flags, bool inherit, uint desiredAccess);
		[DllImport("user32.dll", SetLastError = true)]
		private static extern bool CloseDesktop(IntPtr desktop);
		[DllImport("user32.dll", SetLastError = true, CharSet = CharSet.Unicode)]
		private static extern bool GetUserObjectInformationW(IntPtr handle, int index, StringBuilder information, uint length, out uint needed);
		[DllImport("user32.dll", SetLastError = true)]
		private static extern bool SetForegroundWindow(IntPtr window);
		[DllImport("user32.dll", SetLastError = true)]
		private static extern bool BringWindowToTop(IntPtr window);
		[DllImport("user32.dll", SetLastError = true)]
		private static extern IntPtr SetActiveWindow(IntPtr window);
		[DllImport("user32.dll", SetLastError = true)]
		private static extern IntPtr SetFocus(IntPtr window);
		[DllImport("user32.dll", SetLastError = true)]
		private static extern bool AttachThreadInput(uint sourceThread, uint targetThread, bool attach);
		[DllImport("user32.dll")]
		private static extern bool IsIconic(IntPtr window);
		[DllImport("user32.dll", SetLastError = true)]
		private static extern bool ShowWindowAsync(IntPtr window, int command);
		[DllImport("user32.dll", SetLastError = true)]
		private static extern bool SetWindowPos(IntPtr window, IntPtr insertAfter, int x, int y, int width, int height, uint flags);
		[DllImport("user32.dll")]
		private static extern IntPtr GetForegroundWindow();
		[DllImport("user32.dll", SetLastError = true)]
		private static extern bool SetPhysicalCursorPos(int x, int y);
		[DllImport("user32.dll", SetLastError = true)]
		private static extern bool GetPhysicalCursorPos(out POINT point);
		[DllImport("user32.dll", SetLastError = true)]
		private static extern uint SendInput(uint count, INPUT[] inputs, int size);
		[DllImport("kernel32.dll", SetLastError = true)]
		private static extern IntPtr CreateJobObjectW(IntPtr attributes, string name);
		[DllImport("kernel32.dll")]
		private static extern uint GetCurrentThreadId();
		[DllImport("kernel32.dll", SetLastError = true)]
		private static extern bool SetInformationJobObject(IntPtr job, int informationClass, ref JOBOBJECT_EXTENDED_LIMIT_INFORMATION information, uint length);
		[DllImport("kernel32.dll", SetLastError = true)]
		private static extern bool AssignProcessToJobObject(IntPtr job, IntPtr process);
		[DllImport("kernel32.dll", SetLastError = true)]
		private static extern IntPtr OpenProcess(uint access, bool inherit, uint processId);
		[DllImport("kernel32.dll", SetLastError = true)]
		internal static extern bool TerminateJobObject(IntPtr job, uint exitCode);
		[DllImport("kernel32.dll", SetLastError = true)]
		internal static extern bool CloseHandle(IntPtr handle);

		// Geometry APIs are virtualized according to the caller's DPI context, not
		// the target HWND's process. PowerShell is commonly DPI-unaware, so every
		// capture/input geometry operation temporarily opts only the calling thread
		// into Per-Monitor-V2 and verifies both activation and exact restoration.
		private sealed class ThreadDpiAwarenessScope : IDisposable
		{
			private IntPtr previousContext;
			private bool restoreRequired;

			public bool IsPerMonitorV2 { get; private set; }
			public string ContextName { get; private set; }

			public ThreadDpiAwarenessScope()
			{
				IntPtr before = GetCurrentThreadDpiContextChecked();
				IntPtr previous = SetThreadDpiAwarenessContextChecked(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
				previousContext = previous;
				restoreRequired = true;
				IntPtr current = GetCurrentThreadDpiContextChecked();
				IsPerMonitorV2 = AreDpiAwarenessContextsEqualChecked(current, DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
				ContextName = DescribeDpiAwarenessContext(current);
				if (!IsPerMonitorV2)
				{
					try
					{
						Restore();
					}
					catch
					{
						throw;
					}
					throw new InvalidOperationException("The native validation thread did not enter the verified Per-Monitor-V2 DPI awareness context.");
				}
				if (!AreDpiAwarenessContextsEqualChecked(previousContext, before))
				{
					try
					{
						Restore();
					}
					catch
					{
						throw;
					}
					throw new InvalidOperationException("SetThreadDpiAwarenessContext did not return the exact prior thread context.");
				}
			}

			public void Dispose()
			{
				Restore();
				GC.SuppressFinalize(this);
			}

			private void Restore()
			{
				if (!restoreRequired)
					return;
				IntPtr contextToRestore = previousContext;
				// Mark the scope consumed only after Windows accepts the restore.
				// A failure remains visible and a repeated Dispose can retry.
				SetThreadDpiAwarenessContextChecked(contextToRestore);
				IntPtr restored = GetCurrentThreadDpiContextChecked();
				if (!AreDpiAwarenessContextsEqualChecked(restored, contextToRestore))
					throw new InvalidOperationException("The native validation thread DPI awareness context was not restored exactly.");
				restoreRequired = false;
			}
		}

		private static IntPtr GetCurrentThreadDpiContextChecked()
		{
			try
			{
				IntPtr context = GetThreadDpiAwarenessContext();
				if (context == IntPtr.Zero)
					throw new Win32Exception(Marshal.GetLastWin32Error(), "GetThreadDpiAwarenessContext failed.");
				return context;
			}
			catch (EntryPointNotFoundException exception)
			{
				throw new PlatformNotSupportedException("Per-Monitor-V2 thread DPI awareness requires Windows 10 version 1607 or later.", exception);
			}
		}

		private static IntPtr SetThreadDpiAwarenessContextChecked(IntPtr context)
		{
			try
			{
				IntPtr previous = SetThreadDpiAwarenessContext(context);
				if (previous == IntPtr.Zero)
					throw new Win32Exception(Marshal.GetLastWin32Error(), "SetThreadDpiAwarenessContext failed.");
				return previous;
			}
			catch (EntryPointNotFoundException exception)
			{
				throw new PlatformNotSupportedException("Per-Monitor-V2 thread DPI awareness requires Windows 10 version 1607 or later.", exception);
			}
		}

		private static bool AreDpiAwarenessContextsEqualChecked(IntPtr first, IntPtr second)
		{
			try
			{
				return AreDpiAwarenessContextsEqual(first, second);
			}
			catch (EntryPointNotFoundException exception)
			{
				throw new PlatformNotSupportedException("Per-Monitor-V2 thread DPI awareness requires Windows 10 version 1607 or later.", exception);
			}
		}

		private static string DescribeDpiAwarenessContext(IntPtr context)
		{
			if (AreDpiAwarenessContextsEqualChecked(context, DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2))
				return "PER_MONITOR_AWARE_V2";
			if (AreDpiAwarenessContextsEqualChecked(context, DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE))
				return "PER_MONITOR_AWARE";
			if (AreDpiAwarenessContextsEqualChecked(context, DPI_AWARENESS_CONTEXT_SYSTEM_AWARE))
				return "SYSTEM_AWARE";
			if (AreDpiAwarenessContextsEqualChecked(context, DPI_AWARENESS_CONTEXT_UNAWARE_GDISCALED))
				return "UNAWARE_GDISCALED";
			if (AreDpiAwarenessContextsEqualChecked(context, DPI_AWARENESS_CONTEXT_UNAWARE))
				return "UNAWARE";
			return "UNKNOWN";
		}

		public static string GetCurrentThreadDpiAwarenessContextName()
		{
			return DescribeDpiAwarenessContext(GetCurrentThreadDpiContextChecked());
		}

		private static IntPtr CheckedWindow(int expectedProcessId, long windowHandle)
		{
			IntPtr window = new IntPtr(windowHandle);
			if (window == IntPtr.Zero || !IsWindow(window))
				throw new InvalidOperationException("The requested native editor HWND is not a live window.");
			uint ownerProcessId;
			if (GetWindowThreadProcessId(window, out ownerProcessId) == 0)
				throw new Win32Exception(Marshal.GetLastWin32Error(), "GetWindowThreadProcessId failed.");
			if (ownerProcessId != (uint)expectedProcessId)
				throw new InvalidOperationException("The requested HWND is not owned by the launched editor PID.");
			return window;
		}

		private static NativeWindowSnapshot ReadWindowSnapshot(
			int expectedProcessId,
			IntPtr window,
			bool dpiAwareCapture,
			string dpiAwarenessContext)
		{
			RECT rectangle;
			if (!GetWindowRect(window, out rectangle))
				throw new Win32Exception(Marshal.GetLastWin32Error(), "GetWindowRect failed.");
			int width = rectangle.Right - rectangle.Left;
			int height = rectangle.Bottom - rectangle.Top;
			if (width <= 0 || height <= 0)
				throw new InvalidOperationException("The native editor window has empty bounds.");
			StringBuilder title = new StringBuilder(2048);
			GetWindowTextW(window, title, title.Capacity);
			uint dpi = 0;
			try { dpi = GetDpiForWindow(window); }
			catch (EntryPointNotFoundException) { dpi = 96; }
			return new NativeWindowSnapshot
			{
				ProcessId = expectedProcessId,
				WindowHandle = window.ToInt64(),
				WindowTitle = title.ToString(),
				Left = rectangle.Left,
				Top = rectangle.Top,
				PixelWidth = width,
				PixelHeight = height,
				Dpi = dpi == 0 ? 96U : dpi,
				DpiAwareCapture = dpiAwareCapture,
				DpiAwarenessContext = dpiAwarenessContext,
				PrintWindowSucceeded = false,
				PrintWindowError = 0,
				CapturedAtUtc = String.Empty
			};
		}

		public static NativeWindowSnapshot GetWindowSnapshot(int expectedProcessId, long windowHandle)
		{
			IntPtr window = CheckedWindow(expectedProcessId, windowHandle);
			using (ThreadDpiAwarenessScope dpiScope = new ThreadDpiAwarenessScope())
			{
				return ReadWindowSnapshot(
					expectedProcessId,
					window,
					dpiScope.IsPerMonitorV2,
					dpiScope.ContextName);
			}
		}

		public static NativeClientSnapshot GetClientSnapshot(int expectedProcessId, long windowHandle)
		{
			IntPtr window = CheckedWindow(expectedProcessId, windowHandle);
			using (ThreadDpiAwarenessScope dpiScope = new ThreadDpiAwarenessScope())
			{
				RECT client;
				if (!GetClientRect(window, out client))
					throw new Win32Exception(Marshal.GetLastWin32Error(), "GetClientRect failed.");
				POINT origin = new POINT { X = client.Left, Y = client.Top };
				if (!ClientToScreen(window, ref origin))
					throw new Win32Exception(Marshal.GetLastWin32Error(), "ClientToScreen failed.");
				int width = client.Right - client.Left;
				int height = client.Bottom - client.Top;
				if (width <= 0 || height <= 0)
					throw new InvalidOperationException("The native editor client area has empty bounds.");
				uint dpi = 0;
				try { dpi = GetDpiForWindow(window); }
				catch (EntryPointNotFoundException) { dpi = 96; }
				return new NativeClientSnapshot
				{
					ProcessId = expectedProcessId,
					WindowHandle = window.ToInt64(),
					ScreenLeft = origin.X,
					ScreenTop = origin.Y,
					PixelWidth = width,
					PixelHeight = height,
					Dpi = dpi == 0 ? 96U : dpi,
					DpiAwareGeometry = dpiScope.IsPerMonitorV2,
					DpiAwarenessContext = dpiScope.ContextName
				};
			}
		}

		public static NativeWindowSnapshot CaptureWindow(int expectedProcessId, long windowHandle, string outputPath)
		{
			if (String.IsNullOrWhiteSpace(outputPath))
				throw new ArgumentException("A native capture output path is required.", "outputPath");
			IntPtr window = CheckedWindow(expectedProcessId, windowHandle);
			using (ThreadDpiAwarenessScope dpiScope = new ThreadDpiAwarenessScope())
			{
				NativeWindowSnapshot snapshot = ReadWindowSnapshot(
					expectedProcessId,
					window,
					dpiScope.IsPerMonitorV2,
					dpiScope.ContextName);
				string parent = Path.GetDirectoryName(Path.GetFullPath(outputPath));
				if (!String.IsNullOrEmpty(parent))
					Directory.CreateDirectory(parent);
				using (Bitmap bitmap = new Bitmap(snapshot.PixelWidth, snapshot.PixelHeight, PixelFormat.Format32bppArgb))
				using (Graphics graphics = Graphics.FromImage(bitmap))
				{
					IntPtr deviceContext = graphics.GetHdc();
					bool printed;
					int printError;
					try
					{
						printed = PrintWindow(window, deviceContext, PW_RENDERFULLCONTENT);
						printError = printed ? 0 : Marshal.GetLastWin32Error();
					}
					finally
					{
						graphics.ReleaseHdc(deviceContext);
					}
					snapshot.PrintWindowSucceeded = printed;
					snapshot.PrintWindowError = printError;
					if (!printed)
						throw new Win32Exception(printError, "PrintWindow failed for the launched editor HWND.");
					bitmap.Save(outputPath, ImageFormat.Png);
				}
				snapshot.CapturedAtUtc = DateTimeOffset.UtcNow.ToString("o");
				return snapshot;
			}
		}

		public static InputDesktopSnapshot GetInputDesktopSnapshot()
		{
			InputDesktopSnapshot snapshot = new InputDesktopSnapshot();
			snapshot.ClassifiedAtUtc = DateTimeOffset.UtcNow.ToString("o");
			IntPtr desktop = OpenInputDesktop(0, false, DESKTOP_READOBJECTS | DESKTOP_SWITCHDESKTOP);
			if (desktop == IntPtr.Zero)
			{
				snapshot.WindowsError = Marshal.GetLastWin32Error();
				snapshot.DesktopOpened = false;
				snapshot.DesktopName = String.Empty;
				snapshot.InputAvailable = false;
				snapshot.SessionLocked = true;
				snapshot.Reason = "OpenInputDesktop failed; SendInput is not accepted as editor evidence.";
				return snapshot;
			}
			try
			{
				uint needed;
				GetUserObjectInformationW(desktop, UOI_NAME, null, 0, out needed);
				StringBuilder name = new StringBuilder((int)(needed / 2) + 2);
				if (!GetUserObjectInformationW(desktop, UOI_NAME, name, (uint)(name.Capacity * 2), out needed))
				{
					snapshot.WindowsError = Marshal.GetLastWin32Error();
					snapshot.DesktopOpened = true;
					snapshot.DesktopName = String.Empty;
					snapshot.InputAvailable = false;
					snapshot.SessionLocked = true;
					snapshot.Reason = "The input desktop name could not be queried.";
					return snapshot;
				}
				snapshot.DesktopOpened = true;
				snapshot.DesktopName = name.ToString();
				snapshot.InputAvailable = String.Equals(snapshot.DesktopName, "Default", StringComparison.OrdinalIgnoreCase);
				snapshot.SessionLocked = !snapshot.InputAvailable;
				snapshot.WindowsError = 0;
				snapshot.Reason = snapshot.InputAvailable
					? "The interactive Default input desktop is available."
					: "The active input desktop is not Default; SendInput is not accepted as editor evidence.";
				return snapshot;
			}
			finally
			{
				CloseDesktop(desktop);
			}
		}

		private static NativeInputReceipt NewInputReceipt(string requestKind, int requested, InputDesktopSnapshot desktop)
		{
			return new NativeInputReceipt
			{
				Succeeded = false,
				Method = "SendInput",
				RequestKind = requestKind,
				EventCountRequested = requested,
				EventCountSubmitted = 0,
				WindowsError = 0,
				ForegroundWindow = GetForegroundWindow().ToInt64(),
				DesktopName = desktop.DesktopName,
				InputAvailable = desktop.InputAvailable,
				ForegroundAcquisitionAttempted = false,
				ForegroundAcquired = false,
				ForegroundAcquiredBeforeInput = false,
				ForegroundAcquiredByDeclaredMouseInput = false,
				ForegroundAttempts = 0,
				PostInputForegroundChecks = 0,
				PreviousForegroundWindow = GetForegroundWindow().ToInt64(),
				TargetWasMinimized = false,
				RestoreRequested = false,
				ZOrderRaiseAttempted = false,
				ZOrderRaised = false,
				ZOrderRestored = true,
				ThreadInputAttachmentUsed = false,
				ThreadInputAttachAttempts = 0,
				ThreadInputAttachCount = 0,
				ThreadInputDetachAttempts = 0,
				ThreadInputDetachCount = 0,
				ThreadInputDetached = true,
				ThreadInputError = 0,
				DpiAwareGeometry = false,
				DpiAwarenessContext = String.Empty,
				Dpi = 0,
				ClientPixelWidth = 0,
				ClientPixelHeight = 0,
				RequestedClientX = 0,
				RequestedClientY = 0,
				ScreenX = 0,
				ScreenY = 0,
				PositionedScreenX = 0,
				PositionedScreenY = 0,
				PhysicalCursorPositionVerified = false,
				StartedAtUtc = DateTimeOffset.UtcNow.ToString("o"),
				CompletedAtUtc = String.Empty,
				FailureReason = String.Empty
			};
		}

		private static void TryAttachInputThreads(uint sourceThread, uint targetThread, List<THREAD_INPUT_PAIR> attached, NativeInputReceipt receipt)
		{
			if (sourceThread == 0 || targetThread == 0 || sourceThread == targetThread)
				return;
			receipt.ThreadInputAttachAttempts++;
			if (AttachThreadInput(sourceThread, targetThread, true))
			{
				attached.Add(new THREAD_INPUT_PAIR { SourceThread = sourceThread, TargetThread = targetThread });
				receipt.ThreadInputAttachCount++;
				receipt.ThreadInputAttachmentUsed = true;
				return;
			}
			int error = Marshal.GetLastWin32Error();
			if (receipt.ThreadInputError == 0)
				receipt.ThreadInputError = error;
		}

		private static bool RestoreInputWindowZOrder(IntPtr window, NativeInputReceipt receipt)
		{
			if (!receipt.ZOrderRaised || receipt.ZOrderRestored)
				return true;
			receipt.ZOrderRestored = SetWindowPos(
				window,
				HWND_NOTOPMOST,
				0,
				0,
				0,
				0,
				SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE | SWP_SHOWWINDOW);
			if (!receipt.ZOrderRestored)
			{
				receipt.WindowsError = Marshal.GetLastWin32Error();
				receipt.Succeeded = false;
				receipt.FailureReason = "The validated editor HWND could not be restored from its temporary topmost z-order.";
				receipt.CompletedAtUtc = DateTimeOffset.UtcNow.ToString("o");
			}
			return receipt.ZOrderRestored;
		}

		private static bool PrepareInputWindow(IntPtr window, NativeInputReceipt receipt, bool retainRaisedForDeclaredMouseActivation)
		{
			receipt.ForegroundAcquisitionAttempted = true;
			IntPtr previousForeground = GetForegroundWindow();
			receipt.PreviousForegroundWindow = previousForeground.ToInt64();
			uint ignoredProcessId;
			uint targetThread = GetWindowThreadProcessId(window, out ignoredProcessId);
			uint currentThread = GetCurrentThreadId();
			uint foregroundThread = previousForeground == IntPtr.Zero
				? 0
				: GetWindowThreadProcessId(previousForeground, out ignoredProcessId);
			List<THREAD_INPUT_PAIR> attached = new List<THREAD_INPUT_PAIR>();
			bool acquired = false;
			bool zOrderRaised = false;
			try
			{
				// Join only the calling, target, and current foreground queues.
				// This is bounded to this call and never installs a global hook.
				TryAttachInputThreads(currentThread, targetThread, attached, receipt);
				TryAttachInputThreads(targetThread, foregroundThread, attached, receipt);
				receipt.TargetWasMinimized = IsIconic(window);
				if (receipt.TargetWasMinimized)
				{
					receipt.RestoreRequested = true;
					ShowWindowAsync(window, SW_RESTORE);
				}
				// Codex and remote-control hosts can legitimately retain foreground
				// policy even after thread queues are joined. Temporarily raising
				// only the already validated editor HWND makes the subsequent
				// activation attempt targetable without moving or resizing it.
				receipt.ZOrderRaiseAttempted = true;
				zOrderRaised = SetWindowPos(
					window,
					HWND_TOPMOST,
					0,
					0,
					0,
					0,
					SWP_NOMOVE | SWP_NOSIZE | SWP_SHOWWINDOW);
				receipt.ZOrderRaised = zOrderRaised;
				receipt.ZOrderRestored = !zOrderRaised;
				for (int attempt = 0; attempt < 12; ++attempt)
				{
					receipt.ForegroundAttempts++;
					BringWindowToTop(window);
					SetActiveWindow(window);
					SetFocus(window);
					SetForegroundWindow(window);
					Thread.Sleep(25);
					if (GetForegroundWindow() == window)
					{
						acquired = true;
						break;
					}
				}
				if (zOrderRaised && (acquired || !retainRaisedForDeclaredMouseActivation))
				{
					RestoreInputWindowZOrder(window, receipt);
				}
			}
			finally
			{
				// Detach the exact successful pairs in reverse order even when
				// focus acquisition throws or Windows policy denies foreground.
				for (int index = attached.Count - 1; index >= 0; --index)
				{
					receipt.ThreadInputDetachAttempts++;
					THREAD_INPUT_PAIR pair = attached[index];
					if (AttachThreadInput(pair.SourceThread, pair.TargetThread, false))
					{
						receipt.ThreadInputDetachCount++;
					}
					else
					{
						receipt.ThreadInputDetached = false;
						int detachError = Marshal.GetLastWin32Error();
						if (receipt.ThreadInputError == 0)
							receipt.ThreadInputError = detachError;
					}
				}
			}
			receipt.ForegroundWindow = GetForegroundWindow().ToInt64();
			receipt.ForegroundAcquired = acquired && receipt.ForegroundWindow == window.ToInt64();
			receipt.ForegroundAcquiredBeforeInput = receipt.ForegroundAcquired;
			if (receipt.ZOrderRaised && !receipt.ZOrderRestored && (acquired || !retainRaisedForDeclaredMouseActivation))
			{
				return false;
			}
			if (!receipt.ThreadInputDetached || receipt.ThreadInputDetachCount != receipt.ThreadInputAttachCount)
			{
				receipt.WindowsError = receipt.ThreadInputError;
				receipt.FailureReason = "Native foreground acquisition did not detach every temporary thread-input attachment.";
				receipt.CompletedAtUtc = DateTimeOffset.UtcNow.ToString("o");
				return false;
			}
			if (!receipt.ForegroundAcquired)
			{
				// SetForegroundWindow policy denial does not define a Win32 last
				// error; report only a real AttachThreadInput error, never stale
				// error state left by an unrelated API call.
				receipt.WindowsError = receipt.ThreadInputError;
				receipt.FailureReason = "The launched editor could not become the foreground input window after bounded thread-input attachment.";
				receipt.CompletedAtUtc = DateTimeOffset.UtcNow.ToString("o");
				return false;
			}
			return true;
		}

		public static NativeInputReceipt SendMouseClick(int expectedProcessId, long windowHandle, int clientX, int clientY, string button)
		{
			IntPtr window = CheckedWindow(expectedProcessId, windowHandle);
			InputDesktopSnapshot desktop = GetInputDesktopSnapshot();
			NativeInputReceipt receipt = NewInputReceipt("mouse-click", 2, desktop);
			if (!desktop.InputAvailable)
			{
				receipt.FailureReason = desktop.Reason;
				receipt.CompletedAtUtc = DateTimeOffset.UtcNow.ToString("o");
				return receipt;
			}
			using (ThreadDpiAwarenessScope dpiScope = new ThreadDpiAwarenessScope())
			{
				receipt.DpiAwareGeometry = dpiScope.IsPerMonitorV2;
				receipt.DpiAwarenessContext = dpiScope.ContextName;
				receipt.RequestedClientX = clientX;
				receipt.RequestedClientY = clientY;
				uint dpi = 0;
				try { dpi = GetDpiForWindow(window); }
				catch (EntryPointNotFoundException) { dpi = 96; }
				receipt.Dpi = dpi == 0 ? 96U : dpi;
				RECT client;
				if (!GetClientRect(window, out client))
				{
					receipt.WindowsError = Marshal.GetLastWin32Error();
					receipt.FailureReason = "The live editor client bounds could not be read under Per-Monitor-V2 DPI awareness.";
					receipt.CompletedAtUtc = DateTimeOffset.UtcNow.ToString("o");
					return receipt;
				}
				receipt.ClientPixelWidth = client.Right - client.Left;
				receipt.ClientPixelHeight = client.Bottom - client.Top;
				if (clientX < client.Left || clientY < client.Top || clientX >= client.Right || clientY >= client.Bottom)
				{
					receipt.FailureReason = "The requested mouse point is outside the live editor client bounds.";
					receipt.CompletedAtUtc = DateTimeOffset.UtcNow.ToString("o");
					return receipt;
				}
				bool right = String.Equals(button, "right", StringComparison.Ordinal);
				if (!right && !String.Equals(button, "left", StringComparison.Ordinal))
				{
					receipt.FailureReason = "Only exact left or right mouse clicks are supported.";
					receipt.CompletedAtUtc = DateTimeOffset.UtcNow.ToString("o");
					return receipt;
				}
				bool preparedForeground = PrepareInputWindow(window, receipt, true);
				if (!preparedForeground)
				{
					if (!receipt.ThreadInputDetached || !receipt.ZOrderRaised || receipt.ZOrderRestored)
						return receipt;
					// SetForegroundWindow can be denied even after bounded thread
					// attachment. The validated editor is still the temporary
					// topmost window at the exact physical point, so the declared
					// click can safely perform normal foreground activation.
					receipt.FailureReason = String.Empty;
					receipt.CompletedAtUtc = String.Empty;
					receipt.WindowsError = 0;
				}
				POINT point = new POINT { X = clientX, Y = clientY };
				if (!ClientToScreen(window, ref point) || !SetPhysicalCursorPos(point.X, point.Y))
				{
					receipt.WindowsError = Marshal.GetLastWin32Error();
					receipt.FailureReason = "The requested editor mouse point could not be resolved on screen under Per-Monitor-V2 DPI awareness.";
					receipt.CompletedAtUtc = DateTimeOffset.UtcNow.ToString("o");
					RestoreInputWindowZOrder(window, receipt);
					return receipt;
				}
				receipt.ScreenX = point.X;
				receipt.ScreenY = point.Y;
				POINT positioned;
				if (!GetPhysicalCursorPos(out positioned))
				{
					receipt.WindowsError = Marshal.GetLastWin32Error();
					receipt.FailureReason = "Windows did not report the physical cursor position selected for the editor mouse event.";
					receipt.CompletedAtUtc = DateTimeOffset.UtcNow.ToString("o");
					RestoreInputWindowZOrder(window, receipt);
					return receipt;
				}
				if (positioned.X != point.X || positioned.Y != point.Y)
				{
					receipt.WindowsError = 0;
					receipt.FailureReason = "Windows did not preserve the exact physical screen pixel selected for the editor mouse event.";
					receipt.CompletedAtUtc = DateTimeOffset.UtcNow.ToString("o");
					RestoreInputWindowZOrder(window, receipt);
					return receipt;
				}
				receipt.PositionedScreenX = positioned.X;
				receipt.PositionedScreenY = positioned.Y;
				receipt.PhysicalCursorPositionVerified = true;
				receipt.ForegroundWindow = GetForegroundWindow().ToInt64();
				receipt.ForegroundAcquiredBeforeInput = receipt.ForegroundWindow == window.ToInt64();
				INPUT[] inputs = new INPUT[2];
				inputs[0].type = INPUT_MOUSE;
				inputs[0].data.mouse.dwFlags = right ? MOUSEEVENTF_RIGHTDOWN : MOUSEEVENTF_LEFTDOWN;
				inputs[1].type = INPUT_MOUSE;
				inputs[1].data.mouse.dwFlags = right ? MOUSEEVENTF_RIGHTUP : MOUSEEVENTF_LEFTUP;
				uint submitted = SendInput((uint)inputs.Length, inputs, Marshal.SizeOf(typeof(INPUT)));
				receipt.EventCountSubmitted = (int)submitted;
				receipt.WindowsError = submitted == inputs.Length ? 0 : Marshal.GetLastWin32Error();
				bool foregroundAfterInput = false;
				for (int check = 0; check < 8; ++check)
				{
					receipt.PostInputForegroundChecks++;
					receipt.ForegroundWindow = GetForegroundWindow().ToInt64();
					if (receipt.ForegroundWindow == window.ToInt64())
					{
						foregroundAfterInput = true;
						break;
					}
					Thread.Sleep(25);
				}
				receipt.ForegroundAcquired = foregroundAfterInput;
				receipt.ForegroundAcquiredByDeclaredMouseInput = !receipt.ForegroundAcquiredBeforeInput && foregroundAfterInput;
				receipt.Succeeded = submitted == inputs.Length && foregroundAfterInput;
				if (submitted != inputs.Length)
					receipt.FailureReason = "SendInput did not submit every requested mouse event.";
				else if (!foregroundAfterInput)
					receipt.FailureReason = "Every declared mouse event was submitted, but the target HWND did not become foreground.";
				else
					receipt.FailureReason = String.Empty;
				receipt.CompletedAtUtc = DateTimeOffset.UtcNow.ToString("o");
				if (!RestoreInputWindowZOrder(window, receipt))
					receipt.Succeeded = false;
				return receipt;
			}
		}

		public static NativeInputReceipt SendKeyChord(int expectedProcessId, long windowHandle, int[] virtualKeys)
		{
			IntPtr window = CheckedWindow(expectedProcessId, windowHandle);
			if (virtualKeys == null || virtualKeys.Length < 1 || virtualKeys.Length > 8)
				throw new ArgumentException("A key chord requires between one and eight virtual keys.", "virtualKeys");
			InputDesktopSnapshot desktop = GetInputDesktopSnapshot();
			NativeInputReceipt receipt = NewInputReceipt("key-chord", virtualKeys.Length * 2, desktop);
			if (!desktop.InputAvailable)
			{
				receipt.FailureReason = desktop.Reason;
				receipt.CompletedAtUtc = DateTimeOffset.UtcNow.ToString("o");
				return receipt;
			}
			if (!PrepareInputWindow(window, receipt, false))
				return receipt;
			receipt.ForegroundWindow = GetForegroundWindow().ToInt64();
			receipt.ForegroundAcquiredBeforeInput = receipt.ForegroundWindow == window.ToInt64();
			receipt.ForegroundAcquired = receipt.ForegroundAcquiredBeforeInput;
			if (!receipt.ForegroundAcquiredBeforeInput)
			{
				receipt.FailureReason = "The target HWND lost foreground before keyboard input could be submitted.";
				receipt.CompletedAtUtc = DateTimeOffset.UtcNow.ToString("o");
				return receipt;
			}
			INPUT[] inputs = new INPUT[virtualKeys.Length * 2];
			for (int index = 0; index < virtualKeys.Length; ++index)
			{
				if (virtualKeys[index] < 1 || virtualKeys[index] > UInt16.MaxValue)
					throw new ArgumentOutOfRangeException("virtualKeys", "Virtual-key values must fit an unsigned 16-bit Windows key code.");
				inputs[index].type = INPUT_KEYBOARD;
				inputs[index].data.keyboard.wVk = (ushort)virtualKeys[index];
			}
			for (int index = 0; index < virtualKeys.Length; ++index)
			{
				int sourceIndex = virtualKeys.Length - index - 1;
				int targetIndex = virtualKeys.Length + index;
				inputs[targetIndex].type = INPUT_KEYBOARD;
				inputs[targetIndex].data.keyboard.wVk = (ushort)virtualKeys[sourceIndex];
				inputs[targetIndex].data.keyboard.dwFlags = KEYEVENTF_KEYUP;
			}
			uint submitted = SendInput((uint)inputs.Length, inputs, Marshal.SizeOf(typeof(INPUT)));
			receipt.EventCountSubmitted = (int)submitted;
			receipt.WindowsError = submitted == inputs.Length ? 0 : Marshal.GetLastWin32Error();
			receipt.Succeeded = submitted == inputs.Length;
			receipt.FailureReason = receipt.Succeeded ? String.Empty : "SendInput did not submit every requested keyboard event.";
			receipt.CompletedAtUtc = DateTimeOffset.UtcNow.ToString("o");
			return receipt;
		}

		internal static IntPtr CreateKillOnCloseJob(int processId)
		{
			IntPtr job = CreateJobObjectW(IntPtr.Zero, null);
			if (job == IntPtr.Zero)
				throw new Win32Exception(Marshal.GetLastWin32Error(), "CreateJobObjectW failed.");
			try
			{
				JOBOBJECT_EXTENDED_LIMIT_INFORMATION information = new JOBOBJECT_EXTENDED_LIMIT_INFORMATION();
				information.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
				if (!SetInformationJobObject(job, JobObjectExtendedLimitInformation, ref information, (uint)Marshal.SizeOf(typeof(JOBOBJECT_EXTENDED_LIMIT_INFORMATION))))
					throw new Win32Exception(Marshal.GetLastWin32Error(), "SetInformationJobObject failed.");
				IntPtr process = OpenProcess(PROCESS_TERMINATE | PROCESS_SET_QUOTA | PROCESS_QUERY_LIMITED_INFORMATION, false, (uint)processId);
				if (process == IntPtr.Zero)
					throw new Win32Exception(Marshal.GetLastWin32Error(), "OpenProcess failed for the installed editor.");
				try
				{
					if (!AssignProcessToJobObject(job, process))
						throw new Win32Exception(Marshal.GetLastWin32Error(), "AssignProcessToJobObject failed for the installed editor.");
				}
				finally
				{
					CloseHandle(process);
				}
				return job;
			}
			catch
			{
				CloseHandle(job);
				throw;
			}
		}

		public static KillOnCloseProcessJob CreateKillOnCloseProcessJob(int processId)
		{
			return new KillOnCloseProcessJob(processId);
		}
	}
}
'@
}

function Assert-NoDuplicateJsonObjectKeys {
	param(
		[Parameter(Mandatory = $true)][AllowEmptyString()][string]$Json,
		[Parameter(Mandatory = $true)][string]$Description
	)
	# Neither Windows PowerShell's ConvertFrom-Json nor JavaScriptSerializer
	# rejects duplicate object members. Compile a tiny dependency-free JSON
	# reader once so a payload such as {"passed":false,"passed":true} cannot
	# become valid evidence merely because the last value silently wins.
	if ($null -eq ('TerraEngine.Animation.Validation.StrictJsonDuplicateKeyValidator' -as [type])) {
		Add-Type -TypeDefinition @'
using System;
using System.Collections.Generic;
using System.Globalization;
using System.Text;

namespace TerraEngine.Animation.Validation
{
	public static class StrictJsonDuplicateKeyValidator
	{
		public static void Validate(string json)
		{
			if (json == null)
				throw new FormatException("JSON text is null.");

			int index = 0;
			ParseValue(json, ref index);
			SkipWhitespace(json, ref index);
			if (index != json.Length)
				throw new FormatException("Unexpected content after the root JSON value.");
		}

		private static void ParseValue(string json, ref int index)
		{
			SkipWhitespace(json, ref index);
			if (index >= json.Length)
				throw new FormatException("Unexpected end of JSON.");

			switch (json[index])
			{
				case '{': ParseObject(json, ref index); return;
				case '[': ParseArray(json, ref index); return;
				case '"': ParseString(json, ref index); return;
				case 't': ParseLiteral(json, ref index, "true"); return;
				case 'f': ParseLiteral(json, ref index, "false"); return;
				case 'n': ParseLiteral(json, ref index, "null"); return;
				default: ParseNumber(json, ref index); return;
			}
		}

		private static void ParseObject(string json, ref int index)
		{
			index++;
			SkipWhitespace(json, ref index);
			if (Consume(json, ref index, '}'))
				return;

			var keys = new HashSet<string>(StringComparer.Ordinal);
			while (true)
			{
				SkipWhitespace(json, ref index);
				if (index >= json.Length || json[index] != '"')
					throw new FormatException("Expected a JSON object member name.");

				string key = ParseString(json, ref index);
				if (!keys.Add(key))
					throw new FormatException("Duplicate JSON object member '" + key + "'.");

				SkipWhitespace(json, ref index);
				if (!Consume(json, ref index, ':'))
					throw new FormatException("Expected ':' after a JSON object member name.");
				ParseValue(json, ref index);
				SkipWhitespace(json, ref index);
				if (Consume(json, ref index, '}'))
					return;
				if (!Consume(json, ref index, ','))
					throw new FormatException("Expected ',' or '}' in a JSON object.");
			}
		}

		private static void ParseArray(string json, ref int index)
		{
			index++;
			SkipWhitespace(json, ref index);
			if (Consume(json, ref index, ']'))
				return;

			while (true)
			{
				ParseValue(json, ref index);
				SkipWhitespace(json, ref index);
				if (Consume(json, ref index, ']'))
					return;
				if (!Consume(json, ref index, ','))
					throw new FormatException("Expected ',' or ']' in a JSON array.");
			}
		}

		private static string ParseString(string json, ref int index)
		{
			if (!Consume(json, ref index, '"'))
				throw new FormatException("Expected a JSON string.");

			var value = new StringBuilder();
			while (index < json.Length)
			{
				char current = json[index++];
				if (current == '"')
					return value.ToString();
				if (current < 0x20)
					throw new FormatException("JSON strings may not contain unescaped control characters.");
				if (current != '\\')
				{
					value.Append(current);
					continue;
				}

				if (index >= json.Length)
					throw new FormatException("Incomplete JSON string escape.");
				char escaped = json[index++];
				switch (escaped)
				{
					case '"': value.Append('"'); break;
					case '\\': value.Append('\\'); break;
					case '/': value.Append('/'); break;
					case 'b': value.Append('\b'); break;
					case 'f': value.Append('\f'); break;
					case 'n': value.Append('\n'); break;
					case 'r': value.Append('\r'); break;
					case 't': value.Append('\t'); break;
					case 'u':
						if (index + 4 > json.Length)
							throw new FormatException("Incomplete JSON Unicode escape.");
						ushort codeUnit;
						if (!ushort.TryParse(json.Substring(index, 4), NumberStyles.AllowHexSpecifier, CultureInfo.InvariantCulture, out codeUnit))
							throw new FormatException("Invalid JSON Unicode escape.");
						value.Append((char)codeUnit);
						index += 4;
						break;
					default:
						throw new FormatException("Invalid JSON string escape.");
				}
			}
			throw new FormatException("Unterminated JSON string.");
		}

		private static void ParseLiteral(string json, ref int index, string literal)
		{
			if (index + literal.Length > json.Length ||
				!string.Equals(json.Substring(index, literal.Length), literal, StringComparison.Ordinal))
				throw new FormatException("Invalid JSON literal.");
			index += literal.Length;
		}

		private static void ParseNumber(string json, ref int index)
		{
			int start = index;
			if (Consume(json, ref index, '-'))
			{
				if (index >= json.Length)
					throw new FormatException("Incomplete JSON number.");
			}

			if (Consume(json, ref index, '0'))
			{
				if (index < json.Length && char.IsDigit(json[index]))
					throw new FormatException("JSON numbers may not contain leading zeroes.");
			}
			else
			{
				int integerStart = index;
				while (index < json.Length && char.IsDigit(json[index]))
					index++;
				if (integerStart == index)
					throw new FormatException("Invalid JSON number.");
			}

			if (Consume(json, ref index, '.'))
			{
				int fractionStart = index;
				while (index < json.Length && char.IsDigit(json[index]))
					index++;
				if (fractionStart == index)
					throw new FormatException("JSON fraction requires at least one digit.");
			}

			if (index < json.Length && (json[index] == 'e' || json[index] == 'E'))
			{
				index++;
				if (index < json.Length && (json[index] == '+' || json[index] == '-'))
					index++;
				int exponentStart = index;
				while (index < json.Length && char.IsDigit(json[index]))
					index++;
				if (exponentStart == index)
					throw new FormatException("JSON exponent requires at least one digit.");
			}

			if (start == index)
				throw new FormatException("Invalid JSON value.");
		}

		private static void SkipWhitespace(string json, ref int index)
		{
			while (index < json.Length)
			{
				char current = json[index];
				if (current != ' ' && current != '\t' && current != '\r' && current != '\n')
					return;
				index++;
			}
		}

		private static bool Consume(string json, ref int index, char expected)
		{
			if (index >= json.Length || json[index] != expected)
				return false;
			index++;
			return true;
		}
	}
}
'@
	}

	try {
		[TerraEngine.Animation.Validation.StrictJsonDuplicateKeyValidator]::Validate($Json)
	}
	catch {
		throw "$Description is not strict JSON: $($_.Exception.Message)"
	}
}

function ConvertFrom-JsonPreservingArrays {
	param(
		[Parameter(Mandatory = $true)][AllowEmptyString()][string]$Json,
		[Parameter(Mandatory = $true)][string]$Description
	)
	# The phase gate runs under Windows PowerShell, where System.Web's serializer
	# preserves empty and one-element arrays. Use it only for schema/type checks;
	# ConvertFrom-Json remains the convenient typed view used by the workflow.
	Add-Type -AssemblyName System.Web.Extensions
	try {
		Assert-NoDuplicateJsonObjectKeys -Json $Json -Description $Description
		$serializer = [System.Web.Script.Serialization.JavaScriptSerializer]::new()
		$serializer.MaxJsonLength = [int]::MaxValue
		return $serializer.DeserializeObject($Json)
	}
	catch {
		throw "$Description is not valid JSON: $($_.Exception.Message)"
	}
}

function Get-RequiredPreservedJsonMember {
	param(
		[Parameter(Mandatory = $true)]$Object,
		[Parameter(Mandatory = $true)][string]$Name,
		[Parameter(Mandatory = $true)][string]$Description
	)
	if ($Object -isnot [System.Collections.IDictionary] -or @($Object.Keys) -cnotcontains $Name) {
		throw "$Description is missing required property '$Name'."
	}
	# -NoEnumerate is essential here: otherwise a one-element JSON array would be
	# flattened again while crossing the PowerShell function boundary.
	Write-Output -NoEnumerate $Object[$Name]
}

function Get-Sha256 {
	param([Parameter(Mandatory = $true)][string]$Path)
	return (Get-FileHash -LiteralPath $Path -Algorithm SHA256).Hash.ToLowerInvariant()
}

function Get-VulkanValidationLogEvidence {
	param(
		[Parameter(Mandatory = $true)][string]$StdoutPath,
		[Parameter(Mandatory = $true)][string]$StderrPath
	)
	# A rendered image can look plausible even when Vulkan consumed an invalid
	# descriptor. Treat every validation-layer error emitted through the engine's
	# Vulkan logger as a correctness failure, while retaining only bounded lines
	# so repetitive driver output cannot make evidence unbounded.
	$errorLines = [System.Collections.Generic.List[string]]::new()
	$vuidIds = [System.Collections.Generic.HashSet[string]]::new([System.StringComparer]::Ordinal)
	$streams = [System.Collections.Generic.List[object]]::new()
	foreach ($entry in @(
		[ordered]@{ name = 'stdout'; path = $StdoutPath },
		[ordered]@{ name = 'stderr'; path = $StderrPath }
	)) {
		if (-not (Test-Path -LiteralPath $entry.path -PathType Leaf)) {
			throw "Editor $($entry.name) log '$($entry.path)' is missing."
		}
		$text = [System.IO.File]::ReadAllText([string]$entry.path)
		$fileInfo = [System.IO.FileInfo]::new([string]$entry.path)
		$streams.Add([ordered]@{
			name = [string]$entry.name
			path = $fileInfo.FullName
			sha256 = Get-Sha256 -Path $fileInfo.FullName
			bytes = $fileInfo.Length
		})
		foreach ($match in [regex]::Matches($text, 'VUID-[A-Za-z0-9_-]+')) {
			[void]$vuidIds.Add([string]$match.Value)
		}
		foreach ($line in @($text -split '\r?\n')) {
			if (($line.Contains('[ERROR]') -and $line.Contains('[Vulkan]')) -or
				($line.Contains('[Vulkan]') -and $line.Contains('Validation Error'))) {
				if ($errorLines.Count -lt 64) {
					$boundedLine = if ($line.Length -le 2048) { $line } else { $line.Substring(0, 2048) }
					$errorLines.Add($boundedLine)
				}
			}
		}
	}
	return [ordered]@{
		schemaVersion = 1
		passed = $errorLines.Count -eq 0
		errorLineCount = $errorLines.Count
		captureLimit = 64
		vuidIds = @($vuidIds | Sort-Object)
		errorLines = @($errorLines)
		streams = @($streams)
	}
}

function Get-StringSha256 {
	param([Parameter(Mandatory = $true)][string]$Value)
	$algorithm = [System.Security.Cryptography.SHA256]::Create()
	try {
		$bytes = $script:Utf8NoBom.GetBytes($Value)
		return ([System.BitConverter]::ToString($algorithm.ComputeHash($bytes))).Replace('-', '').ToLowerInvariant()
	}
	finally {
		$algorithm.Dispose()
	}
}

function Get-FileInventory {
	param(
		[Parameter(Mandatory = $true)][string]$Root,
		[string[]]$ExcludedTopLevelDirectories = @()
	)
	$rootPath = [System.IO.Path]::GetFullPath($Root).TrimEnd('\', '/')
	if (-not (Test-Path -LiteralPath $rootPath -PathType Container)) { return @() }
	$records = [System.Collections.Generic.List[object]]::new()
	foreach ($file in @(Get-ChildItem -LiteralPath $rootPath -File -Recurse -Force -ErrorAction Stop)) {
		$relative = Get-RelativeForwardPath -Parent $rootPath -Child $file.FullName
		$topLevel = ($relative -split '/')[0]
		if ($ExcludedTopLevelDirectories -contains $topLevel) { continue }
		# PSCustomObject preserves the declared JSON field order while also exposing
		# real properties to Sort-Object and Measure-Object during evidence locking.
		$records.Add([pscustomobject][ordered]@{
			path = $relative
			bytes = [UInt64]$file.Length
			sha256 = Get-Sha256 -Path $file.FullName
		})
	}
	return @($records | Sort-Object path)
}

function Get-InventoryDigest {
	param([Parameter(Mandatory = $true)]$Inventory)
	$lines = @($Inventory | ForEach-Object { "$($_.path)`t$($_.bytes)`t$($_.sha256)" } | Sort-Object)
	return Get-StringSha256 -Value (($lines -join "`n") + "`n")
}

function Assert-NoReparsePoints {
	param([Parameter(Mandatory = $true)][string]$Root)
	# Junctions and links could make a seemingly safe recursive copy escape its
	# source tree. Reject them before any destination is created.
	$rootItem = Get-Item -LiteralPath $Root -Force -ErrorAction Stop
	if (($rootItem.Attributes -band [System.IO.FileAttributes]::ReparsePoint) -ne 0) {
		throw "Fixture trees may not be symbolic links or junctions: '$($rootItem.FullName)'."
	}
	foreach ($item in @(Get-ChildItem -LiteralPath $Root -Recurse -Force -ErrorAction Stop)) {
		if (($item.Attributes -band [System.IO.FileAttributes]::ReparsePoint) -ne 0) {
			throw "Fixture trees may not contain symbolic links or junctions: '$($item.FullName)'."
		}
	}
}

function Copy-IsolatedFixture {
	param([Parameter(Mandatory = $true)][string]$Source, [Parameter(Mandatory = $true)][string]$Destination)
	if (-not (Test-Path -LiteralPath $Source -PathType Container)) {
		throw "Animation fixture project was not found: '$Source'."
	}
	if (Test-Path -LiteralPath $Destination) {
		throw "Isolated fixture destination already exists; phase evidence is never overwritten: '$Destination'."
	}
	Assert-NoReparsePoints -Root $Source
	[System.IO.Directory]::CreateDirectory($Destination) | Out-Null
	# Enumerate children first because -LiteralPath intentionally does not expand
	# a wildcard. An empty fixture remains a valid empty directory for copy tests.
	foreach ($child in @(Get-ChildItem -LiteralPath $Source -Force -ErrorAction Stop)) {
		Copy-Item -LiteralPath $child.FullName -Destination $Destination -Recurse -Force
	}
	$sourceInventory = @(Get-FileInventory -Root $Source)
	$destinationInventory = @(Get-FileInventory -Root $Destination)
	$sourceDigest = Get-InventoryDigest -Inventory $sourceInventory
	$destinationDigest = Get-InventoryDigest -Inventory $destinationInventory
	if ($sourceDigest -cne $destinationDigest) {
		throw "The isolated project copy does not match its source fixture."
	}
	return [ordered]@{ sourceSha256 = $sourceDigest; copySha256 = $destinationDigest; files = $destinationInventory.Count }
}

function Assert-FixtureOverlayRelativePath {
	param(
		[Parameter(Mandatory = $true)][AllowEmptyString()][string]$Value,
		[Parameter(Mandatory = $true)][ValidateSet('source', 'destination')][string]$Kind
	)
	$description = "Fixture overlay $Kind"
	if ([string]::IsNullOrWhiteSpace($Value) -or $Value -cne $Value.Trim()) {
		throw "$description must be a nonempty relative path without leading or trailing whitespace."
	}
	# Forward slashes are part of the manifest contract. Rejecting alternate path
	# spellings prevents two declarations from aliasing the same Windows target.
	if ($Value.Contains('\') -or
		$Value.StartsWith('/', [System.StringComparison]::Ordinal) -or
		[System.IO.Path]::IsPathRooted($Value) -or
		$Value -match '^(?i:[a-z]:|//[?.]/|//[^/]+/)' -or
		$Value.IndexOf([char]0) -ge 0) {
		throw "$description '$Value' must be a repo/project-relative forward path, not an absolute, UNC, or device path."
	}
	$segments = @($Value.Split([char]'/'))
	if ($segments.Count -lt 1 -or @($segments | Where-Object { $_ -in @('', '.', '..') }).Count -gt 0) {
		throw "$description '$Value' may not contain empty, current-directory, or parent-directory segments."
	}
	$invalidNameCharacters = [System.IO.Path]::GetInvalidFileNameChars()
	foreach ($segment in $segments) {
		if ($segment.IndexOfAny($invalidNameCharacters) -ge 0 -or
			$segment.EndsWith('.', [System.StringComparison]::Ordinal) -or
			$segment.EndsWith(' ', [System.StringComparison]::Ordinal) -or
			$segment -match '^(?i:con|prn|aux|nul|com[1-9]|lpt[1-9])(?:\..*)?$') {
			throw "$description '$Value' contains a Windows-unsafe path segment '$segment'."
		}
	}
	if ($Kind -eq 'source' -and
		-not $Value.StartsWith('Tests/Fixtures/Animation/', [System.StringComparison]::Ordinal)) {
		throw "Fixture overlay source '$Value' must be repo-relative below 'Tests/Fixtures/Animation/'."
	}
	return $Value
}

function Test-FixtureOverlayPathOverlap {
	param(
		[Parameter(Mandatory = $true)][string]$Left,
		[Parameter(Mandatory = $true)][string]$Right
	)
	$leftPath = $Left.TrimEnd('/')
	$rightPath = $Right.TrimEnd('/')
	return $leftPath.Equals($rightPath, [System.StringComparison]::OrdinalIgnoreCase) -or
		$leftPath.StartsWith(($rightPath + '/'), [System.StringComparison]::OrdinalIgnoreCase) -or
		$rightPath.StartsWith(($leftPath + '/'), [System.StringComparison]::OrdinalIgnoreCase)
}

function Assert-FixtureOverlayDestinationPathSafe {
	param(
		[Parameter(Mandatory = $true)][string]$ProjectRoot,
		[Parameter(Mandatory = $true)][string]$DestinationFile
	)
	$projectPath = [System.IO.Path]::GetFullPath($ProjectRoot).TrimEnd('\', '/')
	$destinationPath = [System.IO.Path]::GetFullPath($DestinationFile)
	Assert-PathBelow -Parent $projectPath -Child $destinationPath -Description 'Fixture overlay destination'
	$current = [System.IO.Path]::GetFullPath((Split-Path -Parent $destinationPath)).TrimEnd('\', '/')
	while ($true) {
		if (Test-Path -LiteralPath $current) {
			$item = Get-Item -LiteralPath $current -Force -ErrorAction Stop
			if (-not $item.PSIsContainer) {
				throw "Fixture overlay destination parent is an existing file: '$current'."
			}
			if (($item.Attributes -band [System.IO.FileAttributes]::ReparsePoint) -ne 0) {
				throw "Fixture overlay destinations may not traverse symbolic links or junctions: '$current'."
			}
		}
		if ($current.Equals($projectPath, [System.StringComparison]::OrdinalIgnoreCase)) { break }
		Assert-PathBelow -Parent $projectPath -Child $current -Description 'Fixture overlay destination parent'
		$parent = [System.IO.Path]::GetFullPath((Split-Path -Parent $current)).TrimEnd('\', '/')
		if ($parent.Equals($current, [System.StringComparison]::OrdinalIgnoreCase)) {
			throw "Fixture overlay destination parent traversal did not reach the isolated project root."
		}
		$current = $parent
	}
}

function Get-FixtureOverlaySourceState {
	param(
		[Parameter(Mandatory = $true)][string]$SourcePath,
		[Parameter(Mandatory = $true)][int]$MaximumFiles,
		[Parameter(Mandatory = $true)][UInt64]$MaximumBytes
	)
	if (-not (Test-Path -LiteralPath $SourcePath)) {
		throw "Fixture overlay source was not found: '$SourcePath'."
	}
	$sourceItem = Get-Item -LiteralPath $SourcePath -Force -ErrorAction Stop
	if (($sourceItem.Attributes -band [System.IO.FileAttributes]::ReparsePoint) -ne 0) {
		throw "Fixture overlay sources may not be symbolic links or junctions: '$($sourceItem.FullName)'."
	}
	$sourceFiles = @()
	$sourceKind = if ($sourceItem.PSIsContainer) { 'directory' } else { 'file' }
	if ($sourceKind -eq 'directory') {
		Assert-NoReparsePoints -Root $sourceItem.FullName
		$sourceFiles = @(Get-ChildItem -LiteralPath $sourceItem.FullName -File -Recurse -Force -ErrorAction Stop | Sort-Object FullName)
	}
	else {
		$sourceFiles = @($sourceItem)
	}
	if ($sourceFiles.Count -lt 1) {
		throw "Fixture overlay source '$($sourceItem.FullName)' is empty."
	}
	if ($sourceFiles.Count -gt $MaximumFiles) {
		throw "Fixture overlays exceed the maximum file count of $MaximumFiles."
	}

	$totalBytes = [UInt64]0
	foreach ($file in $sourceFiles) {
		if ([UInt64]$file.Length -eq 0) {
			throw "Fixture overlay source contains an empty file: '$($file.FullName)'."
		}
		if ([UInt64]$file.Length -gt $MaximumBytes -or $totalBytes -gt ($MaximumBytes - [UInt64]$file.Length)) {
			throw "Fixture overlays exceed the maximum byte count of $MaximumBytes."
		}
		$totalBytes += [UInt64]$file.Length
	}

	$inventory = [System.Collections.Generic.List[object]]::new()
	foreach ($file in $sourceFiles) {
		$relative = if ($sourceKind -eq 'file') { '.' } else { Get-RelativeForwardPath -Parent $sourceItem.FullName -Child $file.FullName }
		$inventory.Add([pscustomobject][ordered]@{
			path = $relative
			bytes = [UInt64]$file.Length
			sha256 = Get-Sha256 -Path $file.FullName
		})
	}
	$sortedInventory = @($inventory | Sort-Object path)
	return [ordered]@{
		absolutePath = [System.IO.Path]::GetFullPath($sourceItem.FullName)
		kind = $sourceKind
		files = $sortedInventory.Count
		bytes = $totalBytes
		inventorySha256 = Get-InventoryDigest -Inventory $sortedInventory
		inventory = $sortedInventory
	}
}

function Copy-FixtureOverlays {
	param(
		[Parameter(Mandatory = $true)]$Overlays,
		[Parameter(Mandatory = $true)][string]$RepoRoot,
		[Parameter(Mandatory = $true)][string]$ProjectRoot
	)
	$overlayItems = @($Overlays)
	if ($overlayItems.Count -eq 0) { return }
	if (-not (Test-Path -LiteralPath $ProjectRoot -PathType Container)) {
		throw "Fixture overlay project root was not found: '$ProjectRoot'."
	}
	$repoPath = [System.IO.Path]::GetFullPath($RepoRoot)
	$projectPath = [System.IO.Path]::GetFullPath($ProjectRoot)
	Assert-NoReparsePoints -Root $projectPath
	$baseInventory = @(Get-FileInventory -Root $projectPath)
	$overlayPlans = [System.Collections.Generic.List[object]]::new()
	$plannedDestinationFiles = [System.Collections.Generic.List[string]]::new()
	$totalFiles = 0
	$totalBytes = [UInt64]0

	# Resolve and validate every overlay before creating one destination. Contract
	# failures therefore leave the disposable base-project copy unchanged.
	for ($overlayIndex = 0; $overlayIndex -lt $overlayItems.Count; ++$overlayIndex) {
		$overlay = $overlayItems[$overlayIndex]
		$source = Assert-FixtureOverlayRelativePath -Value ([string]$overlay.source) -Kind source
		$destination = Assert-FixtureOverlayRelativePath -Value ([string]$overlay.destination) -Kind destination
		$sourcePath = Get-AbsolutePath -Path $source -BasePath $repoPath
		Assert-PathBelow -Parent (Join-Path $repoPath 'Tests/Fixtures/Animation') -Child $sourcePath -Description 'Fixture overlay source'
		$remainingFiles = $script:MaximumFixtureOverlayFiles - $totalFiles
		$remainingBytes = $script:MaximumFixtureOverlayBytes - $totalBytes
		$sourceState = Get-FixtureOverlaySourceState -SourcePath $sourcePath -MaximumFiles $remainingFiles -MaximumBytes $remainingBytes
		$validatedDigest = Get-OptionalProperty -Object $overlay -Name 'validatedSourceSha256'
		if ($null -ne $validatedDigest -and [string]$validatedDigest -cne [string]$sourceState.inventorySha256) {
			throw "Fixture overlay source '$source' changed after scenario validation."
		}
		$totalFiles += [int]$sourceState.files
		$totalBytes += [UInt64]$sourceState.bytes

		foreach ($baseFile in $baseInventory) {
			if (Test-FixtureOverlayPathOverlap -Left $destination -Right ([string]$baseFile.path)) {
				throw "Fixture overlay destination '$destination' overlaps base-project file '$($baseFile.path)'."
			}
		}

		$plannedFiles = [System.Collections.Generic.List[object]]::new()
		foreach ($sourceRecord in @($sourceState.inventory)) {
			$destinationRelative = if ([string]$sourceState.kind -eq 'file') {
				$destination
			}
			else {
				"$destination/$($sourceRecord.path)"
			}
			foreach ($existingDestination in $plannedDestinationFiles) {
				if (Test-FixtureOverlayPathOverlap -Left $destinationRelative -Right $existingDestination) {
					throw "Fixture overlay file '$destinationRelative' conflicts with planned overlay file '$existingDestination'."
				}
			}
			$destinationAbsolute = Get-AbsolutePath -Path $destinationRelative -BasePath $projectPath
			Assert-PathBelow -Parent $projectPath -Child $destinationAbsolute -Description 'Fixture overlay destination'
			if (Test-Path -LiteralPath $destinationAbsolute) {
				throw "Fixture overlay destination already exists and may not be replaced: '$destinationRelative'."
			}
			$sourceAbsolute = if ([string]$sourceState.kind -eq 'file') {
				[string]$sourceState.absolutePath
			}
			else {
				Get-AbsolutePath -Path ([string]$sourceRecord.path) -BasePath ([string]$sourceState.absolutePath)
			}
			if ([string]$sourceState.kind -eq 'directory') {
				Assert-PathBelow -Parent ([string]$sourceState.absolutePath) -Child $sourceAbsolute -Description 'Fixture overlay source file'
			}
			$plannedDestinationFiles.Add($destinationRelative)
			$plannedFiles.Add([ordered]@{
				inventoryPath = [string]$sourceRecord.path
				sourceAbsolute = $sourceAbsolute
				destinationRelative = $destinationRelative
				destinationAbsolute = $destinationAbsolute
				bytes = [UInt64]$sourceRecord.bytes
				sourceSha256 = [string]$sourceRecord.sha256
			})
		}
		$overlayPlans.Add([ordered]@{
			source = $source
			destination = $destination
			sourceState = $sourceState
			plannedFiles = @($plannedFiles)
		})
	}

	$provenance = [System.Collections.Generic.List[object]]::new()
	$createdDestinationFiles = [System.Collections.Generic.List[string]]::new()
	# Recheck after the potentially expensive source hashing pass so a destination
	# junction added during preflight is rejected before the first file is copied.
	Assert-NoReparsePoints -Root $projectPath
	try {
		foreach ($overlayPlan in $overlayPlans) {
			$destinationInventory = [System.Collections.Generic.List[object]]::new()
			$fileEvidence = [System.Collections.Generic.List[object]]::new()
			foreach ($plannedFile in @($overlayPlan.plannedFiles)) {
				$destinationParent = Split-Path -Parent ([string]$plannedFile.destinationAbsolute)
				Assert-FixtureOverlayDestinationPathSafe -ProjectRoot $projectPath -DestinationFile ([string]$plannedFile.destinationAbsolute)
				[System.IO.Directory]::CreateDirectory($destinationParent) | Out-Null
				Assert-FixtureOverlayDestinationPathSafe -ProjectRoot $projectPath -DestinationFile ([string]$plannedFile.destinationAbsolute)
				# File.Copy with overwrite=false is the final no-replacement guard
				# even if another process races the preflight existence checks.
				[System.IO.File]::Copy([string]$plannedFile.sourceAbsolute, [string]$plannedFile.destinationAbsolute, $false)
				$createdDestinationFiles.Add([string]$plannedFile.destinationAbsolute)
				$destinationSha256 = Get-Sha256 -Path ([string]$plannedFile.destinationAbsolute)
				$destinationLength = [UInt64](Get-Item -LiteralPath ([string]$plannedFile.destinationAbsolute) -Force).Length
				if ($destinationLength -ne [UInt64]$plannedFile.bytes -or $destinationSha256 -cne [string]$plannedFile.sourceSha256) {
					throw "Fixture overlay copy is not byte-for-byte identical at '$($plannedFile.destinationRelative)'."
				}
				$destinationInventory.Add([pscustomobject][ordered]@{
					path = [string]$plannedFile.inventoryPath
					bytes = $destinationLength
					sha256 = $destinationSha256
				})
				$fileEvidence.Add([ordered]@{
					sourcePath = if ([string]$overlayPlan.sourceState.kind -eq 'file') { [string]$overlayPlan.source } else { "$($overlayPlan.source)/$($plannedFile.inventoryPath)" }
					destinationPath = [string]$plannedFile.destinationRelative
					bytes = $destinationLength
					sourceSha256 = [string]$plannedFile.sourceSha256
					destinationSha256 = $destinationSha256
				})
			}
			$destinationInventorySorted = @($destinationInventory | Sort-Object path)
			$destinationDigest = Get-InventoryDigest -Inventory $destinationInventorySorted
			if ($destinationDigest -cne [string]$overlayPlan.sourceState.inventorySha256) {
				throw "Fixture overlay destination inventory does not match '$($overlayPlan.source)'."
			}
			$provenance.Add([ordered]@{
				source = [string]$overlayPlan.source
				destination = [string]$overlayPlan.destination
				sourceKind = [string]$overlayPlan.sourceState.kind
				fileCount = [int]$overlayPlan.sourceState.files
				byteCount = [UInt64]$overlayPlan.sourceState.bytes
				sourceInventorySha256 = [string]$overlayPlan.sourceState.inventorySha256
				destinationInventorySha256 = $destinationDigest
				files = @($fileEvidence)
			})
		}
		return @($provenance)
	}
	catch {
		# No destination existed before preflight, so only files whose File.Copy
		# completed are owned by this transaction and safe to roll back.
		for ($createdIndex = $createdDestinationFiles.Count - 1; $createdIndex -ge 0; --$createdIndex) {
			$createdPath = $createdDestinationFiles[$createdIndex]
			if (Test-Path -LiteralPath $createdPath -PathType Leaf) {
				[System.IO.File]::Delete($createdPath)
			}
		}
		throw
	}
}

function Install-VerifiedDefaultEditorLayout {
	param(
		[Parameter(Mandatory = $true)][string]$SourceTemplateRoot,
		[Parameter(Mandatory = $true)][string]$InstalledTemplateRoot,
		[Parameter(Mandatory = $true)][string]$ProjectRoot
	)
	$projectUserSettings = Join-Path $ProjectRoot 'UserSettings'
	[System.IO.Directory]::CreateDirectory($projectUserSettings) | Out-Null
	$records = [System.Collections.Generic.List[object]]::new()
	foreach ($layoutFileName in @('EditorLayout.terra', 'EditorLayout.ini')) {
		$sourcePath = Join-Path $SourceTemplateRoot $layoutFileName
		$installedPath = Join-Path $InstalledTemplateRoot $layoutFileName
		$projectPath = Join-Path $projectUserSettings $layoutFileName
		foreach ($requiredPath in @($sourcePath, $installedPath)) {
			if (-not (Test-Path -LiteralPath $requiredPath -PathType Leaf)) {
				throw "Required default editor layout '$layoutFileName' is missing from '$requiredPath'."
			}
		}
		$sourceSha256 = Get-Sha256 -Path $sourcePath
		$installedSha256 = Get-Sha256 -Path $installedPath
		if ($installedSha256 -cne $sourceSha256) {
			throw "Installed default editor layout '$layoutFileName' does not match the repository source template."
		}
		# The live gate opens an installed editor, so seed its disposable project
		# from the exact installed bytes after install has completed.
		Copy-Item -LiteralPath $installedPath -Destination $projectPath -Force
		$projectSha256 = Get-Sha256 -Path $projectPath
		if ($projectSha256 -cne $installedSha256) {
			throw "Isolated editor layout '$layoutFileName' does not match the installed default."
		}
		$records.Add([ordered]@{
			file = $layoutFileName
			copyDirection = 'installed-to-project'
			source = [ordered]@{ path = $sourcePath; sha256 = $sourceSha256 }
			installed = [ordered]@{ path = $installedPath; sha256 = $installedSha256 }
			project = [ordered]@{ path = $projectPath; sha256 = $projectSha256 }
			sourceMatchesInstalled = $true
			installedMatchesProject = $true
		})
	}
	return @($records)
}

function Get-BaselineLockState {
	param([Parameter(Mandatory = $true)][string]$BaselineRoot)
	$manifestPath = Join-Path $BaselineRoot 'locked-baseline.json'
	$lockPath = Join-Path $BaselineRoot 'evidence-lock.json'
	if (-not (Test-Path -LiteralPath $manifestPath -PathType Leaf) -or -not (Test-Path -LiteralPath $lockPath -PathType Leaf)) {
		throw "The immutable AnimationValidationIsCompletedImmutablePreFeatureReferenceCaptureRemainsSolePre baseline manifest or evidence lock is missing."
	}
	$lock = Get-Content -Raw -LiteralPath $lockPath | ConvertFrom-Json
	$manifest = Get-Content -Raw -LiteralPath $manifestPath | ConvertFrom-Json
	if ([string]$lock.aggregateSha256 -cne $script:ExpectedBaselineDigest) {
		throw "The baseline lock advertises an unexpected aggregate digest. Threshold/baseline substitution is forbidden."
	}
	if ((Get-Sha256 -Path $manifestPath) -cne [string]$lock.lockedBaselineSha256) {
		throw "locked-baseline.json no longer matches its evidence lock."
	}
	if ([string]$manifest.state -ne 'locked' -or -not [bool]$manifest.result.passed -or -not [bool]$manifest.comparisonContract.immutable) {
		throw "The AnimationValidationIsCompletedImmutablePreFeatureReferenceCaptureRemainsSolePre baseline does not describe a passing immutable reference."
	}

	# AnimationValidationIsCompletedImmutablePreFeatureReferenceCaptureRemainsSolePre explicitly excludes generated project subtrees and the lock itself.
	# Recomputing all other file hashes detects same-size evidence tampering.
	$lines = [System.Collections.Generic.List[string]]::new()
	$totalBytes = [UInt64]0
	$fileCount = 0
	foreach ($file in @(Get-ChildItem -LiteralPath $BaselineRoot -Recurse -File -Force)) {
		$relative = Get-RelativeForwardPath -Parent $BaselineRoot -Child $file.FullName
		if ($relative -eq 'evidence-lock.json' -or $relative -match '(^|/)project(/|$)') { continue }
		$lines.Add("$relative`t$(Get-Sha256 -Path $file.FullName)")
		$totalBytes += [UInt64]$file.Length
		$fileCount++
	}
	$canonical = ((@($lines) | Sort-Object) -join "`n") + "`n"
	$observedDigest = Get-StringSha256 -Value $canonical
	if ($observedDigest -cne $script:ExpectedBaselineDigest -or $fileCount -ne [int]$lock.fileCount -or $totalBytes -ne [UInt64]$lock.totalBytes) {
		throw "The protected AnimationValidationIsCompletedImmutablePreFeatureReferenceCaptureRemainsSolePre baseline selection has changed; the phase is incomparable and fails closed."
	}
	return [ordered]@{
		manifest = $manifestPath
		lock = $lockPath
		aggregateSha256 = $observedDigest
		manifestSha256 = Get-Sha256 -Path $manifestPath
		lockSha256 = Get-Sha256 -Path $lockPath
		fileCount = $fileCount
		totalBytes = $totalBytes
		identity = $manifest
	}
}

function Get-LockedPerformanceInputState {
	param(
		[Parameter(Mandatory = $true)][string]$RepoRoot,
		[Parameter(Mandatory = $true)]$Baseline
	)
	# Intermediate gates do not run the comparator, so they repeat its immutable
	# input checks here instead of silently losing baseline/contract protection.
	$lockedInputs = [ordered]@{
		baselineAggregateSha256 = [string]$Baseline.aggregateSha256
		baselineManifestSha256 = [string]$Baseline.manifestSha256
		baselineEvidenceLockSha256 = [string]$Baseline.lockSha256
		runtimeContractSha256 = Get-Sha256 -Path (Join-Path $RepoRoot 'Docs/Animation/README.md')
		rendererWorkloadManifestSha256 = Get-Sha256 -Path (Join-Path $RepoRoot 'Tests/Fixtures/RendererPerformanceBaseline/workloads.json')
		performanceWorkloadsSha256 = Get-Sha256 -Path (Join-Path $RepoRoot 'Tools/Animation/PerformanceWorkloads.json')
	}
	$expected = [ordered]@{
		baselineAggregateSha256 = $script:ExpectedBaselineDigest
		baselineManifestSha256 = $script:ExpectedBaselineManifestSha256
		baselineEvidenceLockSha256 = $script:ExpectedBaselineEvidenceLockSha256
		runtimeContractSha256 = $script:ExpectedRuntimeContractSha256
		rendererWorkloadManifestSha256 = $script:ExpectedRendererWorkloadManifestSha256
		performanceWorkloadsSha256 = $script:ExpectedPerformanceWorkloadsSha256
	}
	foreach ($name in $expected.Keys) {
		if ([string]$lockedInputs[$name] -cne [string]$expected[$name]) {
			throw "Locked performance input '$name' changed; deferral cannot weaken the final comparison contract."
		}
	}
	return $lockedInputs
}

function ConvertTo-NativeArgument {
	param([Parameter(Mandatory = $true)][AllowEmptyString()][string]$Argument)
	if ($Argument.Contains('"')) { throw "Native arguments containing quotes are not supported by this bounded runner." }
	if ($Argument -notmatch '[\s&|<>^]') { return $Argument }
	$trailingSlashes = ([regex]::Match($Argument, '\\+$')).Value
	$escaped = if ($trailingSlashes.Length -gt 0) {
		$Argument.Substring(0, $Argument.Length - $trailingSlashes.Length) + ($trailingSlashes * 2)
	} else { $Argument }
	return '"' + $escaped + '"'
}

function Get-MsvcBuildEnvironment {
	param([Parameter(Mandatory = $true)][string]$BuildDirectory)

	if ($env:OS -cne 'Windows_NT') {
		return [pscustomobject]@{
			environment = @{}
			evidence = [ordered]@{
				schemaVersion = 1
				platform = 'non-windows'
				provider = 'not-required'
				compiler = ''
				vcvars = ''
				vcvarsSha256 = ''
				importedVariableCount = 0
				requiredVariablesPresent = @()
			}
		}
	}

	$cachePath = Join-Path $BuildDirectory 'CMakeCache.txt'
	if (-not (Test-Path -LiteralPath $cachePath -PathType Leaf)) {
		throw "MSVC environment discovery requires the configured cache '$cachePath'."
	}
	$compilerMatch = Select-String -LiteralPath $cachePath -Pattern '^CMAKE_CXX_COMPILER:[^=]+=(.+)$' |
		Select-Object -First 1
	if ($null -eq $compilerMatch) {
		throw "Configured CMake cache '$cachePath' does not identify CMAKE_CXX_COMPILER."
	}
	$compilerPath = [System.IO.Path]::GetFullPath($compilerMatch.Matches[0].Groups[1].Value)

	# Resolve vcvars from the exact compiler selected by CMake first. This avoids
	# importing a different Visual Studio installation when several are present.
	$vcvarsPath = $null
	$cursor = [System.IO.DirectoryInfo]::new((Split-Path -Parent $compilerPath))
	while ($null -ne $cursor) {
		if ($cursor.Name -ceq 'VC') {
			$candidate = Join-Path $cursor.FullName 'Auxiliary/Build/vcvars64.bat'
			if (Test-Path -LiteralPath $candidate -PathType Leaf) {
				$vcvarsPath = [System.IO.Path]::GetFullPath($candidate)
			}
			break
		}
		$cursor = $cursor.Parent
	}

	if ($null -eq $vcvarsPath) {
		$programFilesX86 = [Environment]::GetFolderPath([Environment+SpecialFolder]::ProgramFilesX86)
		$vsWhere = Join-Path $programFilesX86 'Microsoft Visual Studio/Installer/vswhere.exe'
		if (Test-Path -LiteralPath $vsWhere -PathType Leaf) {
			$installationPath = @(& $vsWhere -latest -products '*' -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath 2>$null |
				Where-Object { -not [string]::IsNullOrWhiteSpace([string]$_) } |
				Select-Object -First 1)
			if ($LASTEXITCODE -eq 0 -and $installationPath.Count -eq 1) {
				$candidate = Join-Path ([string]$installationPath[0]) 'VC/Auxiliary/Build/vcvars64.bat'
				if (Test-Path -LiteralPath $candidate -PathType Leaf) {
					$vcvarsPath = [System.IO.Path]::GetFullPath($candidate)
				}
			}
		}
	}
	if ($null -eq $vcvarsPath) {
		throw "The MSVC environment script for configured compiler '$compilerPath' could not be found."
	}

	# Capture `set` from a short-lived developer command prompt and pass those
	# values only to build/test/install children. The caller's shell stays
	# untouched, while nested compiler and linker processes inherit the SDK paths.
	$commandInterpreter = (Get-Command cmd.exe -ErrorAction Stop).Source
	$environmentLines = @(& $commandInterpreter /d /s /c ('call "' + $vcvarsPath + '" >nul && set') 2>&1)
	if ($LASTEXITCODE -ne 0) {
		throw "MSVC environment initialization failed through '$vcvarsPath'."
	}
	$environment = @{}
	foreach ($lineValue in $environmentLines) {
		$line = [string]$lineValue
		if ($line -match '^([^=][^=]*)=(.*)$') {
			$environment[$Matches[1]] = $Matches[2]
		}
	}
	$requiredVariables = @('INCLUDE', 'LIB', 'LIBPATH', 'PATH', 'VCToolsInstallDir', 'WindowsSdkDir')
	$missingVariables = @($requiredVariables | Where-Object {
		-not $environment.ContainsKey($_) -or [string]::IsNullOrWhiteSpace([string]$environment[$_])
	})
	if ($missingVariables.Count -gt 0) {
		throw "MSVC environment initialization omitted required variables: $($missingVariables -join ', ')."
	}

	return [pscustomobject]@{
		environment = $environment
		evidence = [ordered]@{
			schemaVersion = 1
			platform = 'windows'
			provider = 'configured-compiler-vcvars64'
			compiler = $compilerPath
			vcvars = $vcvarsPath
			vcvarsSha256 = Get-Sha256 -Path $vcvarsPath
			importedVariableCount = $environment.Count
			requiredVariablesPresent = @($requiredVariables)
		}
	}
}

function Invoke-BoundedProcess {
	param(
		[Parameter(Mandatory = $true)][string]$Stage,
		[Parameter(Mandatory = $true)][string]$FilePath,
		[Parameter(Mandatory = $true)][string[]]$Arguments,
		[Parameter(Mandatory = $true)][string]$WorkingDirectory,
		[Parameter(Mandatory = $true)][string]$OutputRoot,
		[Parameter(Mandatory = $true)][int]$TimeoutSeconds,
		[int[]]$AllowedExitCodes = @(0),
		[hashtable]$Environment = @{}
	)
	$startedAt = Get-Date
	$stdoutPath = Join-Path $OutputRoot "$Stage.stdout.log"
	$stderrPath = Join-Path $OutputRoot "$Stage.stderr.log"
	$commandPath = Join-Path $OutputRoot "$Stage.command.json"
	[System.IO.Directory]::CreateDirectory($OutputRoot) | Out-Null
	$record = [ordered]@{
		stage = $Stage
		executable = $FilePath
		arguments = @($Arguments)
		workingDirectory = $WorkingDirectory
		timeoutSeconds = $TimeoutSeconds
		startedAt = $startedAt.ToString('o')
		completedAt = $null
		durationSeconds = $null
		exitCode = $null
		timedOut = $false
		stdout = $stdoutPath
		stderr = $stderrPath
	}
	Write-JsonFile -Path $commandPath -Value $record
	$startInfo = [System.Diagnostics.ProcessStartInfo]::new()
	$startInfo.FileName = $FilePath
	$startInfo.Arguments = (@($Arguments | ForEach-Object { ConvertTo-NativeArgument -Argument $_ }) -join ' ')
	$startInfo.WorkingDirectory = $WorkingDirectory
	$startInfo.UseShellExecute = $false
	$startInfo.CreateNoWindow = $true
	$startInfo.RedirectStandardOutput = $true
	$startInfo.RedirectStandardError = $true
	foreach ($key in $Environment.Keys) { $startInfo.EnvironmentVariables[[string]$key] = [string]$Environment[$key] }
	$process = [System.Diagnostics.Process]::Start($startInfo)
	if ($null -eq $process) { throw "Stage '$Stage' could not start '$FilePath'." }
	$stdoutTask = $process.StandardOutput.ReadToEndAsync()
	$stderrTask = $process.StandardError.ReadToEndAsync()
	if (-not $process.WaitForExit($TimeoutSeconds * 1000)) {
		$record.timedOut = $true
		try { $process.Kill() } catch { }
		[void]$process.WaitForExit(10000)
	}
	$record.exitCode = if ($process.HasExited) { $process.ExitCode } else { -1 }
	$record.completedAt = (Get-Date).ToString('o')
	$record.durationSeconds = [Math]::Round(((Get-Date) - $startedAt).TotalSeconds, 3)
	Write-Utf8Text -Path $stdoutPath -Value $stdoutTask.GetAwaiter().GetResult()
	Write-Utf8Text -Path $stderrPath -Value $stderrTask.GetAwaiter().GetResult()
	Write-JsonFile -Path $commandPath -Value $record
	$script:CommandRecords.Add($record)
	$script:StageRecords.Add([ordered]@{ name = $Stage; passed = (-not $record.timedOut -and $AllowedExitCodes -contains [int]$record.exitCode); durationSeconds = $record.durationSeconds; exitCode = $record.exitCode })
	if ($record.timedOut) { throw "Stage '$Stage' exceeded its bounded $TimeoutSeconds-second timeout." }
	if ($AllowedExitCodes -notcontains [int]$record.exitCode) {
		throw "Stage '$Stage' failed with exit code $($record.exitCode). See '$stderrPath'."
	}
	return $record
}

function Resolve-InstalledEditor {
	param([Parameter(Mandatory = $true)][string]$ResolvedInstallRoot)
	$editorDirectory = Join-Path $ResolvedInstallRoot 'Editor'
	foreach ($name in @('TerraEngine-Editor.exe', 'TerraEngine-Editor')) {
		$candidate = Join-Path $editorDirectory $name
		if (Test-Path -LiteralPath $candidate -PathType Leaf) {
			$resolved = (Resolve-Path -LiteralPath $candidate).Path
			Assert-PathBelow -Parent $ResolvedInstallRoot -Child $resolved -Description "Installed editor executable"
			return $resolved
		}
	}
	throw "The installed editor was not found below '$editorDirectory'. The install target must succeed first."
}

function Test-LoopbackPortAvailable {
	param([int]$Port)
	$listener = $null
	try {
		$listener = [System.Net.Sockets.TcpListener]::new([System.Net.IPAddress]::Loopback, $Port)
		$listener.Start()
		return $true
	}
	catch { return $false }
	finally { if ($null -ne $listener) { $listener.Stop() } }
}

function Invoke-McpRpc {
	param(
		[Parameter(Mandatory = $true)][string]$Method,
		$Params = @{},
		[switch]$DisableKeepAlive
	)
	$script:McpSequence++
	$request = [ordered]@{ jsonrpc = '2.0'; id = $script:McpSequence; method = $Method; params = $Params }
	$started = Get-Date
	$transport = [ordered]@{
		disableKeepAlive = [bool]$DisableKeepAlive
		connectionIntent = if ($DisableKeepAlive) { 'close-after-response' } else { 'default-persistent' }
	}
	try {
		$webRequestParameters = @{
			UseBasicParsing = $true
			Uri = $script:McpEndpoint
			Method = 'Post'
			ContentType = 'application/json'
			Body = ($request | ConvertTo-Json -Depth 64 -Compress)
			TimeoutSec = 30
		}
		if ($DisableKeepAlive) {
			# This request deliberately carries Connection: close. The next RPC
			# therefore cannot succeed through the same HTTP connection and is
			# evidence that the loopback listener accepts a fresh client.
			$webRequestParameters.DisableKeepAlive = $true
		}
		$response = Invoke-WebRequest @webRequestParameters
		$parsed = $response.Content | ConvertFrom-Json
		Add-JsonLine -Path $script:McpTranscriptPath -Value ([ordered]@{ sequence = $script:McpSequence; timestamp = (Get-Date).ToString('o'); durationMs = [Math]::Round(((Get-Date) - $started).TotalMilliseconds, 3); transport = $transport; request = $request; response = $parsed })
		$errorProperty = $parsed.PSObject.Properties['error']
		if ($null -ne $errorProperty -and $null -ne $errorProperty.Value) {
			throw "JSON-RPC error $($errorProperty.Value.code): $($errorProperty.Value.message)"
		}
		return $parsed
	}
	catch {
		Add-JsonLine -Path $script:McpTranscriptPath -Value ([ordered]@{ sequence = $script:McpSequence; timestamp = (Get-Date).ToString('o'); durationMs = [Math]::Round(((Get-Date) - $started).TotalMilliseconds, 3); transport = $transport; request = $request; error = $_.Exception.Message })
		throw
	}
}

function Get-ToolEnvelope {
	param([Parameter(Mandatory = $true)]$Response, [Parameter(Mandatory = $true)][string]$Tool)
	$content = @(Get-OptionalProperty -Object (Get-OptionalProperty -Object $Response -Name 'result') -Name 'content')
	$textItems = @($content | Where-Object { [string](Get-OptionalProperty -Object $_ -Name 'type') -eq 'text' })
	if ($textItems.Count -ne 1) { throw "MCP tool '$Tool' did not return exactly one typed text envelope." }
	return ([string]$textItems[0].text | ConvertFrom-Json)
}

function Invoke-McpTool {
	param([Parameter(Mandatory = $true)][string]$Name, $Arguments = @{})
	$response = Invoke-McpRpc -Method 'tools/call' -Params ([ordered]@{ name = $Name; arguments = $Arguments })
	return [ordered]@{ response = $response; envelope = Get-ToolEnvelope -Response $response -Tool $Name }
}

function Assert-ExactRuntimeJsonObjectKeys {
	param(
		[Parameter(Mandatory = $true)]$Object,
		[Parameter(Mandatory = $true)][string[]]$RequiredKeys,
		[Parameter(Mandatory = $true)][string]$Description
	)
	if ($null -eq $Object -or $Object -is [string] -or $Object -is [System.Array]) {
		throw "$Description must be one JSON object."
	}
	$actualKeys = if ($Object -is [System.Collections.IDictionary]) {
		@($Object.Keys | ForEach-Object { [string]$_ })
	}
	else {
		@($Object.PSObject.Properties | ForEach-Object { [string]$_.Name })
	}
	if ($actualKeys.Count -ne $RequiredKeys.Count -or
		@($RequiredKeys | Where-Object { $actualKeys -cnotcontains $_ }).Count -gt 0) {
		throw "$Description must contain exactly: $($RequiredKeys -join ', ')."
	}
}

function Read-McpJsonResource {
	param([Parameter(Mandatory = $true)][string]$Uri)
	$expectedSequence = $script:McpSequence + 1
	$response = Invoke-McpRpc -Method 'resources/read' -Params ([ordered]@{ uri = $Uri })
	if ($script:McpSequence -ne $expectedSequence) {
		throw "MCP resource '$Uri' did not use exactly one transcript sequence."
	}
	$contents = @(Get-NestedValue -Object $response -Path 'result.contents')
	if ($contents.Count -ne 1 -or
		[string](Get-RequiredJsonProperty -Object $contents[0] -Name 'mimeType' -Description "MCP resource '$Uri' content") -cne 'application/json') {
		throw "MCP resource '$Uri' did not return exactly one application/json document."
	}
	$text = [string](Get-RequiredJsonProperty -Object $contents[0] -Name 'text' -Description "MCP resource '$Uri' content")
	if ([string]::IsNullOrWhiteSpace($text)) {
		throw "MCP resource '$Uri' returned an empty JSON document."
	}
	$strict = ConvertFrom-JsonPreservingArrays -Json $text -Description "MCP resource '$Uri'"
	$document = $text | ConvertFrom-Json
	if ($strict -isnot [System.Collections.IDictionary] -or $null -eq $document -or $document -is [System.Array]) {
		throw "MCP resource '$Uri' must contain one JSON object."
	}
	return [ordered]@{
		uri = $Uri
		mcpSequence = $expectedSequence
		documentSha256 = Get-StringSha256 -Value $text
		document = $document
		strict = $strict
		response = $response
	}
}

function Get-PendingApprovalResourceEntry {
	param(
		[Parameter(Mandatory = $true)]$ResourceState,
		[Parameter(Mandatory = $true)][string]$ApprovalId,
		[Parameter(Mandatory = $true)][string]$RequestId,
		[Parameter(Mandatory = $true)][string]$Capability
	)
	Assert-ExactPreservedJsonObjectKeys -Object $ResourceState.strict -RequiredKeys @('ok', 'approvals') -Description 'Pending approvals resource'
	$okStrict = Get-RequiredPreservedJsonMember -Object $ResourceState.strict -Name 'ok' -Description 'Pending approvals resource'
	$approvalsStrict = Get-RequiredPreservedJsonMember -Object $ResourceState.strict -Name 'approvals' -Description 'Pending approvals resource'
	if ($okStrict -isnot [bool] -or -not $okStrict -or -not (Test-IsJsonArray -Value $approvalsStrict)) {
		throw 'Pending approvals resource must contain ok=true and an approvals JSON array.'
	}
	$matches = @($ResourceState.document.approvals | Where-Object {
		[string]$_.approvalId -ceq $ApprovalId
	})
	if ($matches.Count -ne 1) {
		throw "Approval '$ApprovalId' does not identify exactly one pending resource entry."
	}
	$entry = $matches[0]
	Assert-ExactRuntimeJsonObjectKeys -Object $entry -RequiredKeys @(
		'approvalId', 'requestId', 'capability', 'summary', 'state'
	) -Description "Pending approval '$ApprovalId'"
	if ([string]$entry.requestId -cne $RequestId -or [string]$entry.capability -cne $Capability -or
		[string]$entry.state -cne 'pending' -or [string]::IsNullOrWhiteSpace([string]$entry.summary)) {
		throw "Pending approval '$ApprovalId' does not match its exact request, capability, and pending state."
	}
	return $entry
}

function Get-RecentApprovalResourceEntry {
	param(
		[Parameter(Mandatory = $true)]$ResourceState,
		[Parameter(Mandatory = $true)][string]$ApprovalId,
		[Parameter(Mandatory = $true)][string]$RequestId,
		[Parameter(Mandatory = $true)][string]$Capability,
		[Parameter(Mandatory = $true)][string]$ExpectedImpactFingerprint,
		[Parameter(Mandatory = $true)][UInt64]$ExpectedProjectGeneration,
		[Parameter(Mandatory = $true)][UInt64]$ExpectedSceneGeneration,
		[Parameter(Mandatory = $true)][UInt64]$ExpectedWorldGeneration
	)
	Assert-ExactPreservedJsonObjectKeys -Object $ResourceState.strict -RequiredKeys @('ok', 'approvals') -Description 'Recent approvals resource'
	$okStrict = Get-RequiredPreservedJsonMember -Object $ResourceState.strict -Name 'ok' -Description 'Recent approvals resource'
	$approvalsStrict = Get-RequiredPreservedJsonMember -Object $ResourceState.strict -Name 'approvals' -Description 'Recent approvals resource'
	if ($okStrict -isnot [bool] -or -not $okStrict -or -not (Test-IsJsonArray -Value $approvalsStrict)) {
		throw 'Recent approvals resource must contain ok=true and an approvals JSON array.'
	}
	$matches = @($ResourceState.document.approvals | Where-Object {
		[string]$_.approvalId -ceq $ApprovalId
	})
	if ($matches.Count -ne 1) {
		throw "Approval '$ApprovalId' does not identify exactly one retained recent resource entry."
	}
	$entry = $matches[0]
	Assert-ExactRuntimeJsonObjectKeys -Object $entry -RequiredKeys @(
		'approvalId', 'requestId', 'capability', 'summary', 'state', 'approved',
		'dispatchCompleted', 'dispatchResult', 'argumentsFingerprint',
		'impactFingerprint', 'projectGeneration', 'sceneGeneration', 'worldGeneration'
	) -Description "Recent approval '$ApprovalId'"
	$approved = Get-RequiredJsonProperty -Object $entry -Name 'approved' -Description "Recent approval '$ApprovalId'"
	$dispatchCompleted = Get-RequiredJsonProperty -Object $entry -Name 'dispatchCompleted' -Description "Recent approval '$ApprovalId'"
	if ($approved -isnot [bool] -or $dispatchCompleted -isnot [bool] -or
		[string]$entry.requestId -cne $RequestId -or [string]$entry.capability -cne $Capability -or
		[string]::IsNullOrWhiteSpace([string]$entry.summary) -or
		[string]$entry.argumentsFingerprint -notmatch '^[0-9a-f]{1,16}$' -or
		[string]$entry.impactFingerprint -cne $ExpectedImpactFingerprint -or
		[UInt64]$entry.projectGeneration -ne $ExpectedProjectGeneration -or
		[UInt64]$entry.sceneGeneration -ne $ExpectedSceneGeneration -or
		[UInt64]$entry.worldGeneration -ne $ExpectedWorldGeneration -or
		[string]$entry.state -cnotin @('pending', 'dispatching', 'approved', 'denied', 'binding_invalid', 'expired', 'canceled')) {
		throw "Recent approval '$ApprovalId' changed its request binding, fingerprints, generations, or typed state."
	}
	$state = [string]$entry.state
	if (($state -ceq 'approved' -and (-not [bool]$approved -or -not [bool]$dispatchCompleted -or $null -eq $entry.dispatchResult)) -or
		($state -cin @('denied', 'binding_invalid', 'expired', 'canceled') -and
			([bool]$approved -or -not [bool]$dispatchCompleted -or $null -ne $entry.dispatchResult)) -or
		($state -cin @('pending', 'dispatching') -and [bool]$dispatchCompleted)) {
		throw "Recent approval '$ApprovalId' has an inconsistent state, approval flag, dispatch flag, or dispatch result."
	}
	return $entry
}

function Invoke-ApprovedScenarioOperation {
	param(
		[Parameter(Mandatory = $true)]$Definition,
		[Parameter(Mandatory = $true)]$Arguments,
		[Parameter(Mandatory = $true)][string]$ScenarioPhase,
		[Parameter(Mandatory = $true)][string]$RunRoot,
		[Parameter(Mandatory = $true)]$ApprovalRegistry,
		[Parameter(Mandatory = $true)][int]$TimeoutSeconds
	)
	$operationId = [string]$Definition.id
	$capability = [string]$Definition.tool
	$initialSequence = $script:McpSequence + 1
	$initialResult = Invoke-McpTool -Name $capability -Arguments $Arguments
	if ($script:McpSequence -ne $initialSequence) {
		throw "Approved scenario operation '$operationId' initial request did not use exactly one MCP sequence."
	}
	$initialEnvelope = $initialResult.envelope
	Assert-ExactRuntimeJsonObjectKeys -Object $initialEnvelope -RequiredKeys @(
		'ok', 'requestId', 'status', 'message', 'data', 'error'
	) -Description "Approved scenario operation '$operationId' initial envelope"
	$initialOk = Get-RequiredJsonProperty -Object $initialEnvelope -Name 'ok' -Description "Approved scenario operation '$operationId' initial envelope"
	$initialData = Get-RequiredJsonProperty -Object $initialEnvelope -Name 'data' -Description "Approved scenario operation '$operationId' initial envelope"
	Assert-ExactRuntimeJsonObjectKeys -Object $initialData -RequiredKeys @(
		'approvalId', 'capability', 'impactFingerprint', 'projectGeneration',
		'sceneGeneration', 'worldGeneration'
	) -Description "Approved scenario operation '$operationId' initial approval data"
	$approvalId = [string]$initialData.approvalId
	$requestId = [string]$initialEnvelope.requestId
	$impactFingerprint = [string]$initialData.impactFingerprint
	if ($initialOk -isnot [bool] -or $initialOk -or
		[string]$initialEnvelope.status -cne 'waiting_approval' -or
		$null -ne $initialEnvelope.error -or
		$approvalId -notmatch '^approval-[0-9]+$' -or [string]::IsNullOrWhiteSpace($requestId) -or
		[string]$initialData.capability -cne $capability -or
		$impactFingerprint -notmatch '^[0-9a-f]{1,16}$') {
		throw "Approved scenario operation '$operationId' did not enter the exact high-impact waiting_approval boundary."
	}
	if (-not $ApprovalRegistry.Add($approvalId)) {
		throw "Approved scenario operation '$operationId' replayed approval ID '$approvalId'."
	}
	$projectGeneration = [UInt64]$initialData.projectGeneration
	$sceneGeneration = [UInt64]$initialData.sceneGeneration
	$worldGeneration = [UInt64]$initialData.worldGeneration
	$initialEnvelopeSha256 = Get-StringSha256 -Value ($initialEnvelope | ConvertTo-Json -Depth 80 -Compress)
	$pendingState = Read-McpJsonResource -Uri 'terraengine://approvals/pending'
	$pendingEntry = Get-PendingApprovalResourceEntry -ResourceState $pendingState -ApprovalId $approvalId -RequestId $requestId -Capability $capability
	$pendingEntrySha256 = Get-CanonicalJsonSha256 -Value $pendingEntry
	$approvalEvidenceRelative = "editor/approvals/$operationId.json"
	$approvalEvidencePath = Get-AbsolutePath -Path $approvalEvidenceRelative -BasePath $RunRoot
	Assert-PathBelow -Parent $RunRoot -Child $approvalEvidencePath -Description "Scenario approval '$operationId' evidence"
	$recentPolls = [System.Collections.Generic.List[object]]::new()
	$approvalDocument = [ordered]@{
		schemaVersion = 1
		phase = $ScenarioPhase
		operationId = $operationId
		policy = 'high_impact'
		capability = $capability
		approvalId = $approvalId
		requestId = $requestId
		argumentsSha256 = Get-CanonicalJsonSha256 -Value $Arguments
		impactFingerprint = $impactFingerprint
		projectGeneration = $projectGeneration
		sceneGeneration = $sceneGeneration
		worldGeneration = $worldGeneration
		status = 'waiting_approval'
		initial = [ordered]@{
			mcpSequence = $initialSequence
			envelopeSha256 = $initialEnvelopeSha256
			envelope = $initialEnvelope
		}
		pending = [ordered]@{
			mcpSequence = [int]$pendingState.mcpSequence
			resourceUri = [string]$pendingState.uri
			documentSha256 = [string]$pendingState.documentSha256
			entrySha256 = $pendingEntrySha256
			entry = $pendingEntry
		}
		recentPolls = @()
		completion = $null
	}
	# Persist before waiting so an interruption still leaves the exact prompt and
	# its pending-resource proof for diagnosis.
	Write-JsonFile -Path $approvalEvidencePath -Value $approvalDocument
	Write-Host "Approve $capability request $approvalId in Preferences -> Approvals within $TimeoutSeconds seconds."
	$deadline = (Get-Date).AddSeconds($TimeoutSeconds)
	$argumentsFingerprint = $null
	while ((Get-Date) -lt $deadline) {
		$recentState = Read-McpJsonResource -Uri 'terraengine://approvals/recent'
		$recentEntry = Get-RecentApprovalResourceEntry `
			-ResourceState $recentState `
			-ApprovalId $approvalId `
			-RequestId $requestId `
			-Capability $capability `
			-ExpectedImpactFingerprint $impactFingerprint `
			-ExpectedProjectGeneration $projectGeneration `
			-ExpectedSceneGeneration $sceneGeneration `
			-ExpectedWorldGeneration $worldGeneration
		if ($null -eq $argumentsFingerprint) {
			$argumentsFingerprint = [string]$recentEntry.argumentsFingerprint
		}
		elseif ([string]$recentEntry.argumentsFingerprint -cne $argumentsFingerprint) {
			throw "Approval '$approvalId' changed its retained arguments fingerprint while being polled."
		}
		$recentPoll = [ordered]@{
			mcpSequence = [int]$recentState.mcpSequence
			resourceUri = [string]$recentState.uri
			documentSha256 = [string]$recentState.documentSha256
			entrySha256 = Get-CanonicalJsonSha256 -Value $recentEntry
			state = [string]$recentEntry.state
			approved = [bool]$recentEntry.approved
			dispatchCompleted = [bool]$recentEntry.dispatchCompleted
		}
		$recentPolls.Add($recentPoll)
		$approvalDocument.recentPolls = @($recentPolls)
		if ([bool]$recentEntry.dispatchCompleted) {
			$approvalDocument.status = [string]$recentEntry.state
			$dispatchResult = Get-RequiredJsonProperty -Object $recentEntry -Name 'dispatchResult' -Description "Approval '$approvalId' recent entry"
			$dispatchResultSha256 = if ($null -eq $dispatchResult) { $null } else {
				Get-StringSha256 -Value ($dispatchResult | ConvertTo-Json -Depth 80 -Compress)
			}
			$approvalDocument.completion = [ordered]@{
				mcpSequence = [int]$recentState.mcpSequence
				entrySha256 = [string]$recentPoll.entrySha256
				state = [string]$recentEntry.state
				approved = [bool]$recentEntry.approved
				dispatchCompleted = [bool]$recentEntry.dispatchCompleted
				dispatchResultSha256 = $dispatchResultSha256
				dispatchResult = $dispatchResult
			}
			Write-JsonFile -Path $approvalEvidencePath -Value $approvalDocument
			if ([string]$recentEntry.state -cne 'approved' -or -not [bool]$recentEntry.approved -or $null -eq $dispatchResult) {
				throw "Approval '$approvalId' terminated as '$($recentEntry.state)' without an approved dispatch result."
			}
			$dispatchOk = Get-RequiredJsonProperty -Object $dispatchResult -Name 'ok' -Description "Approval '$approvalId' dispatch result"
			if ($dispatchOk -isnot [bool]) {
				throw "Approval '$approvalId' dispatch result omitted a typed ok outcome."
			}
			$approvalEvidenceReference = [ordered]@{
				path = $approvalEvidenceRelative
				sha256 = Get-Sha256 -Path $approvalEvidencePath
			}
			return [ordered]@{
				response = $initialResult.response
				envelope = $dispatchResult
				mcpSequence = $initialSequence
				approval = [ordered]@{
					policy = 'high_impact'
					capability = $capability
					approvalId = $approvalId
					requestId = $requestId
					argumentsSha256 = [string]$approvalDocument.argumentsSha256
					argumentsFingerprint = $argumentsFingerprint
					impactFingerprint = $impactFingerprint
					projectGeneration = $projectGeneration
					sceneGeneration = $sceneGeneration
					worldGeneration = $worldGeneration
					initialMcpSequence = $initialSequence
					initialEnvelopeSha256 = $initialEnvelopeSha256
					pendingMcpSequence = [int]$pendingState.mcpSequence
					pendingDocumentSha256 = [string]$pendingState.documentSha256
					pendingEntrySha256 = $pendingEntrySha256
					completionMcpSequence = [int]$recentState.mcpSequence
					completionEntrySha256 = [string]$recentPoll.entrySha256
					dispatchResultSha256 = $dispatchResultSha256
					evidence = $approvalEvidenceReference
				}
			}
		}
		Write-JsonFile -Path $approvalEvidencePath -Value $approvalDocument
		Start-Sleep -Milliseconds $script:ApprovalPollIntervalMilliseconds
	}
	$approvalDocument.status = 'timed_out'
	Write-JsonFile -Path $approvalEvidencePath -Value $approvalDocument
	throw "Timed out waiting for editor approval '$approvalId' for '$capability'."
}

function Get-NestedValue {
	param($Object, [Parameter(Mandatory = $true)][string]$Path)
	$current = $Object
	foreach ($segment in $Path.Split('.')) {
		$current = Get-OptionalProperty -Object $current -Name $segment
		if ($null -eq $current) { return $null }
	}
	return $current
}

function Test-NestedJsonFieldPresent {
	param($Object, [Parameter(Mandatory = $true)][string]$Path)
	$current = $Object
	foreach ($segment in $Path.Split('.')) {
		if ($null -eq $current) { return $false }
		# Property metadata distinguishes a missing member from an explicitly
		# present empty JSON array without changing Get-NestedValue's established
		# array-enumeration behavior for existing callers.
		$property = $current.PSObject.Properties[$segment]
		if ($null -eq $property -or $null -eq $property.Value) { return $false }
		$current = $property.Value
	}
	return $true
}

function Assert-ArgumentBindingObjectPath {
	param(
		[Parameter(Mandatory = $true)][AllowEmptyString()][string]$Path,
		[Parameter(Mandatory = $true)][string]$Description
	)
	$segments = @($Path.Split([char]'.'))
	if ([string]::IsNullOrWhiteSpace($Path) -or
		$segments.Count -lt 1 -or
		$segments.Count -gt $script:MaximumArgumentBindingPathSegments) {
		throw "$Description must contain one through $($script:MaximumArgumentBindingPathSegments) case-sensitive object-field segments."
	}
	foreach ($segment in $segments) {
		# Binding paths intentionally describe object members only. Numeric array
		# indexes, bracket syntax, templates, and wildcard expansion are excluded.
		if ($segment -cnotmatch '^[A-Za-z_][A-Za-z0-9_]{0,63}$') {
			throw "$Description '$Path' contains unsafe or non-object segment '$segment'."
		}
	}
	return $Path
}

function Get-CaseSensitiveJsonPathValue {
	param(
		$Object,
		[Parameter(Mandatory = $true)][string]$Path
	)
	$current = $Object
	foreach ($segment in $Path.Split([char]'.')) {
		if ($current -is [System.Collections.IDictionary]) {
			if (@($current.Keys) -cnotcontains $segment) {
				return [pscustomobject][ordered]@{ found = $false; value = $null }
			}
			$current = $current[$segment]
			continue
		}
		if ($null -eq $current -or $current -is [string] -or $current -is [System.Array]) {
			return [pscustomobject][ordered]@{ found = $false; value = $null }
		}
		$properties = @($current.PSObject.Properties | Where-Object { $_.Name -ceq $segment })
		if ($properties.Count -ne 1) {
			return [pscustomobject][ordered]@{ found = $false; value = $null }
		}
		$current = $properties[0].Value
	}
	return [pscustomobject][ordered]@{ found = $true; value = $current }
}

function Set-CaseSensitiveJsonNullPathValue {
	param(
		[Parameter(Mandatory = $true)]$Object,
		[Parameter(Mandatory = $true)][string]$Path,
		[Parameter(Mandatory = $true)]$Value
	)
	$segments = @($Path.Split([char]'.'))
	$current = $Object
	for ($segmentIndex = 0; $segmentIndex -lt $segments.Count; ++$segmentIndex) {
		$segment = $segments[$segmentIndex]
		if ($current -isnot [System.Collections.IDictionary] -or @($current.Keys) -cnotcontains $segment) {
			throw "Argument binding destination '$Path' is not an exact case-sensitive object path."
		}
		if ($segmentIndex -eq $segments.Count - 1) {
			if ($null -ne $current[$segment]) {
				throw "Argument binding destination '$Path' is not the required JSON null placeholder."
			}
			$current[$segment] = $Value
			return
		}
		$current = $current[$segment]
		if ($null -eq $current -or $current -is [string] -or $current -is [System.Array]) {
			throw "Argument binding destination '$Path' traverses a non-object value."
		}
	}
}

function Copy-JsonValuePreservingArrays {
	param([Parameter(Mandatory = $true)]$Value)
	$json = ConvertTo-Json -InputObject $Value -Depth 80 -Compress
	$clone = ConvertFrom-JsonPreservingArrays -Json $json -Description 'Scenario invocation arguments'
	Write-Output -NoEnumerate $clone
}

function ConvertTo-CanonicalJsonNode {
	param($Value)
	if ($null -eq $Value) { return $null }
	if ($Value -is [System.Collections.IDictionary]) {
		$keys = [string[]]@($Value.Keys | ForEach-Object { [string]$_ })
		[System.Array]::Sort($keys, [System.StringComparer]::Ordinal)
		$ordered = [ordered]@{}
		foreach ($key in $keys) {
			$ordered[$key] = ConvertTo-CanonicalJsonNode -Value $Value[$key]
		}
		return $ordered
	}
	if ($Value -is [System.Array]) {
		$items = [System.Collections.Generic.List[object]]::new()
		foreach ($item in $Value) {
			$items.Add((ConvertTo-CanonicalJsonNode -Value $item))
		}
		Write-Output -NoEnumerate ([object[]]@($items))
		return
	}
	if ($Value -isnot [string] -and @($Value.PSObject.Properties).Count -gt 0 -and
		[System.Type]::GetTypeCode($Value.GetType()) -eq [System.TypeCode]::Object) {
		$keys = [string[]]@($Value.PSObject.Properties | ForEach-Object { [string]$_.Name })
		[System.Array]::Sort($keys, [System.StringComparer]::Ordinal)
		$ordered = [ordered]@{}
		foreach ($key in $keys) {
			$property = @($Value.PSObject.Properties | Where-Object { $_.Name -ceq $key })[0]
			$ordered[$key] = ConvertTo-CanonicalJsonNode -Value $property.Value
		}
		return $ordered
	}
	return $Value
}

function Get-CanonicalJsonText {
	param($Value)
	$canonical = ConvertTo-CanonicalJsonNode -Value $Value
	return ConvertTo-Json -InputObject $canonical -Depth 80 -Compress
}

function Get-CanonicalJsonSha256 {
	param($Value)
	return Get-StringSha256 -Value (Get-CanonicalJsonText -Value $Value)
}

function Get-OrdinalSortedStrings {
	param([AllowEmptyCollection()]$Values)
	$result = [string[]]@($Values | ForEach-Object { [string]$_ })
	[System.Array]::Sort($result, [System.StringComparer]::Ordinal)
	# Emit individual strings so callers can use @(...); -NoEnumerate would
	# create a nested String[] entry and corrupt canonical snapshot evidence.
	foreach ($value in $result) {
		Write-Output $value
	}
}

function Assert-ExactPreservedJsonObjectKeys {
	param(
		[Parameter(Mandatory = $true)]$Object,
		[Parameter(Mandatory = $true)][string[]]$RequiredKeys,
		[Parameter(Mandatory = $true)][string]$Description
	)
	if ($Object -isnot [System.Collections.IDictionary]) {
		throw "$Description must preserve one JSON object."
	}
	$actualKeys = @($Object.Keys | ForEach-Object { [string]$_ })
	if ($actualKeys.Count -ne $RequiredKeys.Count -or
		@($RequiredKeys | Where-Object { $actualKeys -cnotcontains $_ }).Count -gt 0) {
		throw "$Description must contain exactly: $($RequiredKeys -join ', ')."
	}
}

function Get-JsonScalarType {
	param($Value)
	if ($null -eq $Value) { return 'null' }
	if ($Value -is [string]) { return 'string' }
	if ($Value -is [bool]) { return 'boolean' }
	$typeCode = [System.Type]::GetTypeCode($Value.GetType())
	if ($typeCode -in @(
		[System.TypeCode]::SByte, [System.TypeCode]::Byte,
		[System.TypeCode]::Int16, [System.TypeCode]::UInt16,
		[System.TypeCode]::Int32, [System.TypeCode]::UInt32,
		[System.TypeCode]::Int64, [System.TypeCode]::UInt64
	)) { return 'integer' }
	if ($typeCode -in @([System.TypeCode]::Single, [System.TypeCode]::Double, [System.TypeCode]::Decimal)) {
		return 'number'
	}
	return 'object'
}

function Assert-ArgumentBindingValueType {
	param(
		$Value,
		[Parameter(Mandatory = $true)][string]$ValueType,
		[Parameter(Mandatory = $true)][string]$Description
	)
	$actualType = Get-JsonScalarType -Value $Value
	if ($ValueType -eq 'uuid') {
		# TerraEngine serializes UUIDs with ToHexString/ParseUUID: exactly 32
		# lowercase hexadecimal characters and no Guid-style separators.
		if ($actualType -cne 'string' -or [string]$Value -cnotmatch '^[0-9a-f]{32}$') {
			throw "$Description is not a Terra canonical lowercase 32-hex UUID string."
		}
		return
	}
	$typeMatched = $actualType -ceq $ValueType -or ($ValueType -ceq 'number' -and $actualType -ceq 'integer')
	if (-not $typeMatched) {
		throw "$Description has JSON type '$actualType', expected '$ValueType'."
	}
}

function Get-ScenarioInvocationArguments {
	param(
		[Parameter(Mandatory = $true)]$Definition,
		[Parameter(Mandatory = $true)][ValidateSet('operation', 'capture')][string]$Kind
	)
	$sourceArguments = Get-OptionalProperty -Object $Definition -Name 'arguments'
	if ($null -eq $sourceArguments) { $sourceArguments = [ordered]@{} }
	$arguments = Copy-JsonValuePreservingArrays -Value $sourceArguments
	if ($arguments -isnot [System.Collections.IDictionary]) {
		throw "Scenario item '$($Definition.id)' arguments must contain one JSON object."
	}
	if ($Kind -eq 'capture') {
		$arguments['width'] = $script:ReferenceWidth
		$arguments['height'] = $script:ReferenceHeight
		$arguments['format'] = 'image/png'
		$arguments['freshness'] = 'nextFrame'
		$arguments['allowBackendFallback'] = $false
	}
	Write-Output -NoEnumerate $arguments
}

function Resolve-ScenarioArgumentBindings {
	param(
		[Parameter(Mandatory = $true)]$Definition,
		[Parameter(Mandatory = $true)]$Arguments,
		[Parameter(Mandatory = $true)]$BindingRegistry
	)
	$records = [System.Collections.Generic.List[object]]::new()
	foreach ($binding in @(Get-OptionalProperty -Object $Definition -Name 'argumentBindings')) {
		$sourceId = [string]$binding.sourceOperationId
		$hasSource = if ($null -ne $BindingRegistry.PSObject.Methods['ContainsKey']) {
			[bool]$BindingRegistry.ContainsKey($sourceId)
		}
		else {
			[bool]$BindingRegistry.Contains($sourceId)
		}
		if (-not $hasSource) {
			throw "Argument binding source operation '$sourceId' has not completed successful expectation and evidence validation."
		}
		$sourceRecord = $BindingRegistry[$sourceId]
		$sourcePath = [string]$binding.sourceResponsePath
		$selected = Get-CaseSensitiveJsonPathValue -Object $sourceRecord.envelope -Path $sourcePath
		if (-not [bool]$selected.found -or $null -eq $selected.value) {
			throw "Argument binding source '$sourceId.$sourcePath' is missing or null."
		}
		$valueType = [string]$binding.valueType
		Assert-ArgumentBindingValueType -Value $selected.value -ValueType $valueType -Description "Argument binding source '$sourceId.$sourcePath'"
		$resolvedValue = $selected.value
		# A recording range must begin at the immutable pose tick that was just
		# inspected, while its end must be later than that tick.  Keep this small,
		# explicit integer-only offset instead of accepting arbitrary expressions in
		# reviewed scenario manifests.  That makes the dynamic value reproducible in
		# the evidence while preventing a scenario from smuggling in code execution.
		$integerOffset = $null
		if ($null -ne $binding.PSObject.Properties['integerOffset']) {
			$integerOffset = [Int64]$binding.integerOffset
			try {
				$sourceInteger = [Convert]::ToInt64($selected.value, [Globalization.CultureInfo]::InvariantCulture)
			}
			catch {
				throw "Argument binding source '$sourceId.$sourcePath' cannot apply integerOffset because its value is outside the signed 64-bit range."
			}
			if (($integerOffset -gt 0 -and $sourceInteger -gt [Int64]::MaxValue - $integerOffset) -or
				($integerOffset -lt 0 -and $sourceInteger -lt [Int64]::MinValue - $integerOffset)) {
				throw "Argument binding source '$sourceId.$sourcePath' plus integerOffset overflows the signed 64-bit range."
			}
			$resolvedValue = [Int64]($sourceInteger + $integerOffset)
		}
		Set-CaseSensitiveJsonNullPathValue -Object $Arguments -Path ([string]$binding.destinationArgumentPath) -Value $resolvedValue
		$records.Add([ordered]@{
			sourceOperationId = $sourceId
			sourceResponsePath = $sourcePath
			sourceMcpSequence = [int]$sourceRecord.mcpSequence
			sourceEnvelopeSha256 = [string]$sourceRecord.envelopeSha256
			selectedValueType = Get-JsonScalarType -Value $selected.value
			selectedValueSha256 = Get-CanonicalJsonSha256 -Value $selected.value
			destinationArgumentPath = [string]$binding.destinationArgumentPath
			valueType = $valueType
			integerOffset = $integerOffset
		})
	}
	return [ordered]@{
		argumentsSha256 = Get-CanonicalJsonSha256 -Value $Arguments
		bindings = @($records)
	}
}

function Initialize-ScenarioOperationApprovals {
	param(
		[Parameter(Mandatory = $true)]$Scenario,
		[Parameter(Mandatory = $true)]$ScenarioStrict
	)
	$strictOperations = Get-RequiredPreservedJsonMember -Object $ScenarioStrict -Name 'operations' -Description 'Phase scenario manifest'
	$strictCaptures = Get-RequiredPreservedJsonMember -Object $ScenarioStrict -Name 'captures' -Description 'Phase scenario manifest'
	for ($captureIndex = 0; $captureIndex -lt @($strictCaptures).Count; ++$captureIndex) {
		$strictCapture = @($strictCaptures)[$captureIndex]
		if ($strictCapture -is [System.Collections.IDictionary] -and @($strictCapture.Keys) -ccontains 'approval') {
			throw "Scenario capture '$(@($Scenario.captures)[$captureIndex].id)' may not declare an approval policy."
		}
	}
	for ($operationIndex = 0; $operationIndex -lt @($strictOperations).Count; ++$operationIndex) {
		$strictOperation = @($strictOperations)[$operationIndex]
		$operation = @($Scenario.operations)[$operationIndex]
		$operationId = [string]$operation.id
		$tool = [string]$operation.tool
		if ($strictOperation -is [System.Collections.IDictionary] -and @($strictOperation.Keys) -ccontains 'arguments') {
			$argumentsStrict = Get-RequiredPreservedJsonMember -Object $strictOperation -Name 'arguments' -Description "Scenario operation '$operationId'"
			if ($argumentsStrict -is [System.Collections.IDictionary] -and @($argumentsStrict.Keys) -ccontains 'approval') {
				throw "Scenario operation '$operationId' approval is execution policy metadata and must be an operation-level member, not an MCP argument."
			}
		}
		$hasApproval = $strictOperation -is [System.Collections.IDictionary] -and
			@($strictOperation.Keys) -ccontains 'approval'
		if (-not $hasApproval) {
			if ($script:HighImpactScenarioTools -ccontains $tool) {
				throw "Scenario operation '$operationId' uses high-impact tool '$tool' and requires an exact operation-level approval marker."
			}
			continue
		}
		$approvalStrict = Get-RequiredPreservedJsonMember -Object $strictOperation -Name 'approval' -Description "Scenario operation '$operationId'"
		Assert-ExactPreservedJsonObjectKeys -Object $approvalStrict -RequiredKeys @('policy') -Description "Scenario operation '$operationId' approval"
		if ($approvalStrict['policy'] -isnot [string] -or [string]$approvalStrict['policy'] -cne 'high_impact') {
			throw "Scenario operation '$operationId' approval policy must be exact JSON string 'high_impact'."
		}
		if ($script:HighImpactScenarioTools -cnotcontains $tool) {
			throw "Scenario operation '$operationId' may declare high_impact approval only for a reviewed mutating animation tool."
		}
		if ($null -ne (Get-OptionalProperty -Object $operation -Name 'fileTransition')) {
			throw "Scenario operation '$operationId' may not hold a fixture file transition open while waiting for high-impact approval."
		}
		$expectStrict = Get-RequiredPreservedJsonMember -Object $strictOperation -Name 'expect' -Description "Scenario operation '$operationId'"
		if ($expectStrict -isnot [System.Collections.IDictionary] -or
			@($expectStrict.Keys) -cnotcontains 'ok' -or
			$expectStrict['ok'] -isnot [bool] -or -not [bool]$expectStrict['ok']) {
			throw "Scenario operation '$operationId' high_impact approval requires an explicit expect.ok=true Boolean."
		}
	}
}

function Initialize-ScenarioArgumentBindings {
	param(
		[Parameter(Mandatory = $true)]$Scenario,
		[Parameter(Mandatory = $true)]$ScenarioStrict
	)
	$operationsStrict = Get-RequiredPreservedJsonMember -Object $ScenarioStrict -Name 'operations' -Description 'Phase scenario manifest'
	$capturesStrict = Get-RequiredPreservedJsonMember -Object $ScenarioStrict -Name 'captures' -Description 'Phase scenario manifest'
	$operations = @($Scenario.operations)
	$captures = @($Scenario.captures)
	if (-not (Test-IsJsonArray -Value $operationsStrict) -or @($operationsStrict).Count -ne $operations.Count) {
		throw 'Scenario operations must be a shape-preserving JSON array.'
	}
	if (-not (Test-IsJsonArray -Value $capturesStrict) -or @($capturesStrict).Count -ne $captures.Count) {
		throw 'Scenario captures must be a shape-preserving JSON array.'
	}

	$operationById = [System.Collections.Generic.Dictionary[string, object]]::new([System.StringComparer]::Ordinal)
	$strictOperationById = [System.Collections.Generic.Dictionary[string, object]]::new([System.StringComparer]::Ordinal)
	for ($operationIndex = 0; $operationIndex -lt $operations.Count; ++$operationIndex) {
		$operation = $operations[$operationIndex]
		$strictOperation = @($operationsStrict)[$operationIndex]
		if ($strictOperation -isnot [System.Collections.IDictionary] -or
			$null -eq $operation -or $operation -is [string] -or $operation -is [System.Array]) {
			throw "Scenario operation $($operationIndex + 1) must be one JSON object."
		}
		$id = [string]$operation.id
		$operationById[$id] = $operation
		$strictOperationById[$id] = $strictOperation
	}
	for ($captureIndex = 0; $captureIndex -lt $captures.Count; ++$captureIndex) {
		$strictCapture = @($capturesStrict)[$captureIndex]
		if ($strictCapture -is [System.Collections.IDictionary] -and @($strictCapture.Keys) -ccontains 'argumentBindings') {
			throw "Scenario capture '$($captures[$captureIndex].id)' may not declare operation argumentBindings."
		}
	}

	$totalBindings = 0
	$stageRanks = @{ prePlay = 0; play = 1; postPlay = 2 }
	foreach ($operation in $operations) {
		$operationId = [string]$operation.id
		$strictOperation = $strictOperationById[$operationId]
		$validatedBindings = [System.Collections.Generic.List[object]]::new()
		$destinationPaths = [System.Collections.Generic.List[string]]::new()
		if (@($strictOperation.Keys) -ccontains 'argumentBindings') {
			$bindingsStrict = Get-RequiredPreservedJsonMember -Object $strictOperation -Name 'argumentBindings' -Description "Scenario operation '$operationId'"
			if (-not (Test-IsJsonArray -Value $bindingsStrict)) {
				throw "Scenario operation '$operationId' argumentBindings must be a JSON array."
			}
			if (@($bindingsStrict).Count -gt $script:MaximumArgumentBindingsPerOperation) {
				throw "Scenario operation '$operationId' may declare at most $($script:MaximumArgumentBindingsPerOperation) argument bindings."
			}
			$bindingsTyped = @(Get-OptionalProperty -Object $operation -Name 'argumentBindings')
			if ($bindingsTyped.Count -ne @($bindingsStrict).Count) {
				throw "Scenario operation '$operationId' argumentBindings changed shape during JSON decoding."
			}
			for ($bindingIndex = 0; $bindingIndex -lt $bindingsTyped.Count; ++$bindingIndex) {
				$strictBinding = @($bindingsStrict)[$bindingIndex]
				$binding = $bindingsTyped[$bindingIndex]
				if ($strictBinding -isnot [System.Collections.IDictionary] -or
					$null -eq $binding -or $binding -is [string] -or $binding -is [System.Array]) {
					throw "Scenario operation '$operationId' argument binding $($bindingIndex + 1) must be one JSON object."
				}
				$bindingKeys = @($strictBinding.Keys | ForEach-Object { [string]$_ })
				$requiredBindingKeys = @('sourceOperationId', 'sourceResponsePath', 'destinationArgumentPath', 'valueType')
				$allowedBindingKeys = @($requiredBindingKeys + 'integerOffset')
				if (@($requiredBindingKeys | Where-Object { $bindingKeys -cnotcontains $_ }).Count -gt 0 -or
					@($bindingKeys | Where-Object { $_ -cnotin $allowedBindingKeys }).Count -gt 0) {
					throw "Scenario operation '$operationId' argument binding $($bindingIndex + 1) must contain exactly sourceOperationId, sourceResponsePath, destinationArgumentPath, and valueType, plus only the optional integerOffset field."
				}
				foreach ($bindingKey in $requiredBindingKeys) {
					if ($strictBinding[$bindingKey] -isnot [string]) {
						throw "Scenario operation '$operationId' argument binding $($bindingIndex + 1) property '$bindingKey' must be a JSON string."
					}
				}
				$sourceId = [string]$strictBinding['sourceOperationId']
				$sourcePath = Assert-ArgumentBindingObjectPath -Path ([string]$strictBinding['sourceResponsePath']) -Description "Scenario operation '$operationId' sourceResponsePath"
				$destinationPath = Assert-ArgumentBindingObjectPath -Path ([string]$strictBinding['destinationArgumentPath']) -Description "Scenario operation '$operationId' destinationArgumentPath"
				$valueType = [string]$strictBinding['valueType']
				if (-not $sourcePath.StartsWith('data.', [System.StringComparison]::Ordinal)) {
					throw "Scenario operation '$operationId' argument binding sourceResponsePath must begin with exact 'data.'."
				}
				if ($valueType -cnotin @('uuid', 'string', 'integer', 'number', 'boolean')) {
					throw "Scenario operation '$operationId' argument binding valueType '$valueType' is unsupported."
				}
				if (@($bindingKeys | Where-Object { $_ -ceq 'integerOffset' }).Count -eq 1) {
					$integerOffset = $strictBinding['integerOffset']
					if ((Get-JsonScalarType -Value $integerOffset) -cne 'integer') {
						throw "Scenario operation '$operationId' argument binding $($bindingIndex + 1) integerOffset must be a JSON integer."
					}
					if ($valueType -cne 'integer') {
						throw "Scenario operation '$operationId' argument binding $($bindingIndex + 1) integerOffset requires valueType 'integer'."
					}
					try { $integerOffsetValue = [Convert]::ToInt64($integerOffset, [Globalization.CultureInfo]::InvariantCulture) }
					catch { throw "Scenario operation '$operationId' argument binding $($bindingIndex + 1) integerOffset must fit a signed 64-bit integer." }
					# The recorder's four-hour hard limit is also the largest useful range
					# adjustment, keeping scenario execution bounded and reviewable.
					if ($integerOffsetValue -lt -864000 -or $integerOffsetValue -gt 864000) {
						throw "Scenario operation '$operationId' argument binding $($bindingIndex + 1) integerOffset must stay within the recording four-hour fixed-tick bound."
					}
				}
				if (-not $operationById.ContainsKey($sourceId)) {
					if (@($captures | Where-Object { [string]$_.id -ceq $sourceId }).Count -eq 1) {
						throw "Scenario operation '$operationId' argument binding source '$sourceId' is a capture, not an operation."
					}
					throw "Scenario operation '$operationId' argument binding source operation '$sourceId' does not exist."
				}
				$sourceOperation = $operationById[$sourceId]
				if ($sourceId -ceq $operationId -or
					[int]$sourceOperation.order -ge [int]$operation.order -or
					[int]$stageRanks[[string]$sourceOperation.stage] -gt [int]$stageRanks[[string]$operation.stage]) {
					throw "Scenario operation '$operationId' argument binding source '$sourceId' must be a distinct earlier operation."
				}
				$sourceStrict = $strictOperationById[$sourceId]
				$sourceExpectationStrict = Get-RequiredPreservedJsonMember -Object $sourceStrict -Name 'expect' -Description "Argument binding source operation '$sourceId'"
				if ($sourceExpectationStrict -isnot [System.Collections.IDictionary]) {
					throw "Argument binding source operation '$sourceId' expect must be one JSON object."
				}
				if (@($sourceExpectationStrict.Keys) -ccontains 'ok') {
					$sourceExpectedOk = $sourceExpectationStrict['ok']
					if ($sourceExpectedOk -isnot [bool] -or -not $sourceExpectedOk) {
						throw "Argument binding source operation '$sourceId' must declare a successful ok=true expectation."
					}
				}
				$sourceRequiredFields = Get-RequiredPreservedJsonMember -Object $sourceExpectationStrict -Name 'requiredFields' -Description "Argument binding source operation '$sourceId' expect"
				if (-not (Test-IsJsonArray -Value $sourceRequiredFields) -or
					@($sourceRequiredFields | Where-Object { $_ -isnot [string] }).Count -gt 0 -or
					@($sourceRequiredFields) -cnotcontains $sourcePath) {
					throw "Argument binding source operation '$sourceId' must declare exact required field '$sourcePath'."
				}
				$destinationArgumentsStrict = Get-RequiredPreservedJsonMember -Object $strictOperation -Name 'arguments' -Description "Scenario operation '$operationId'"
				if ($destinationArgumentsStrict -isnot [System.Collections.IDictionary]) {
					throw "Scenario operation '$operationId' arguments must be one JSON object for argument binding."
				}
				foreach ($existingDestination in $destinationPaths) {
					if ($destinationPath.Equals($existingDestination, [System.StringComparison]::Ordinal) -or
						$destinationPath.StartsWith(($existingDestination + '.'), [System.StringComparison]::Ordinal) -or
						$existingDestination.StartsWith(($destinationPath + '.'), [System.StringComparison]::Ordinal)) {
						throw "Scenario operation '$operationId' binding destination '$destinationPath' duplicates or overlaps '$existingDestination'."
					}
				}
				$destinationValue = Get-CaseSensitiveJsonPathValue -Object $destinationArgumentsStrict -Path $destinationPath
				if (-not [bool]$destinationValue.found) {
					throw "Scenario operation '$operationId' binding destination '$destinationPath' does not exist with exact case."
				}
				if ($null -ne $destinationValue.value) {
					throw "Scenario operation '$operationId' binding destination '$destinationPath' must be exact JSON null."
				}
				$destinationPaths.Add($destinationPath)
				$validatedBindings.Add($binding)
				$totalBindings++
				if ($totalBindings -gt $script:MaximumArgumentBindingsPerScenario) {
					throw "Scenario may declare at most $($script:MaximumArgumentBindingsPerScenario) argument bindings."
				}
			}
		}
		$operation | Add-Member -NotePropertyName argumentBindings -NotePropertyValue @($validatedBindings) -Force
	}
}

function Assert-PlayModeRecordingOutputPath {
	param([Parameter(Mandatory = $true)][AllowEmptyString()][string]$Path)
	if ([string]::IsNullOrWhiteSpace($Path) -or $Path -cne $Path.Trim() -or
		$Path.Contains('\') -or [System.IO.Path]::IsPathRooted($Path) -or
		$Path.IndexOf([char]0) -ge 0 -or
		$Path -cnotmatch '^Assets/Captured/[A-Za-z0-9][A-Za-z0-9._-]{0,127}\.teanim(?:\.meta)?$') {
		throw "Play Mode recording output '$Path' must be an exact project-relative Assets/Captured/<safe-name>.teanim path or its separately declared .teanim.meta sidecar."
	}
	$fileName = [System.IO.Path]::GetFileName($Path)
	$assetFileName = if ($fileName.EndsWith('.meta', [System.StringComparison]::Ordinal)) {
		$fileName.Substring(0, $fileName.Length - '.meta'.Length)
	}
	else {
		$fileName
	}
	$baseName = [System.IO.Path]::GetFileNameWithoutExtension($assetFileName)
	if ($fileName.EndsWith('.', [System.StringComparison]::Ordinal) -or
		$fileName.EndsWith(' ', [System.StringComparison]::Ordinal) -or
		$baseName -match '^(?i:con|prn|aux|nul|com[1-9]|lpt[1-9])$') {
		throw "Play Mode recording output '$Path' contains a Windows-unsafe file name."
	}
	return $Path
}

function Initialize-ScenarioPlayModeRecordingOutputs {
	param(
		[Parameter(Mandatory = $true)]$Scenario,
		[Parameter(Mandatory = $true)]$ScenarioStrict
	)
	$outputs = [System.Collections.Generic.List[object]]::new()
	$hasOutputs = $ScenarioStrict -is [System.Collections.IDictionary] -and
		@($ScenarioStrict.Keys) -ccontains 'playModeRecordingOutputs'
	if (-not $hasOutputs) {
		$Scenario | Add-Member -NotePropertyName playModeRecordingOutputs -NotePropertyValue @() -Force
		return
	}
	$outputsStrict = Get-RequiredPreservedJsonMember -Object $ScenarioStrict -Name 'playModeRecordingOutputs' -Description 'Phase scenario manifest'
	if (-not (Test-IsJsonArray -Value $outputsStrict)) {
		throw 'Scenario playModeRecordingOutputs must be a JSON array.'
	}
	if (@($outputsStrict).Count -gt $script:MaximumPlayModeRecordingOutputs) {
		throw "Scenario playModeRecordingOutputs may contain at most $($script:MaximumPlayModeRecordingOutputs) entries."
	}
	$outputsTyped = @(Get-OptionalProperty -Object $Scenario -Name 'playModeRecordingOutputs')
	if ($outputsTyped.Count -ne @($outputsStrict).Count) {
		throw 'Scenario playModeRecordingOutputs changed shape during JSON decoding.'
	}

	$operationById = [System.Collections.Generic.Dictionary[string, object]]::new([System.StringComparer]::Ordinal)
	$strictOperations = Get-RequiredPreservedJsonMember -Object $ScenarioStrict -Name 'operations' -Description 'Phase scenario manifest'
	$strictOperationById = [System.Collections.Generic.Dictionary[string, object]]::new([System.StringComparer]::Ordinal)
	for ($operationIndex = 0; $operationIndex -lt @($Scenario.operations).Count; ++$operationIndex) {
		$operation = @($Scenario.operations)[$operationIndex]
		$operationById[[string]$operation.id] = $operation
		$strictOperationById[[string]$operation.id] = @($strictOperations)[$operationIndex]
	}
	$declaredPaths = [System.Collections.Generic.HashSet[string]]::new([System.StringComparer]::OrdinalIgnoreCase)
	for ($outputIndex = 0; $outputIndex -lt $outputsTyped.Count; ++$outputIndex) {
		$strictOutput = @($outputsStrict)[$outputIndex]
		$output = $outputsTyped[$outputIndex]
		if ($strictOutput -isnot [System.Collections.IDictionary] -or
			$null -eq $output -or $output -is [string] -or $output -is [System.Array]) {
			throw "Scenario Play Mode recording output $($outputIndex + 1) must be one JSON object."
		}
		$requiredKeys = @('path', 'producerOperationId', 'producerResponsePath')
		$outputKeys = @($strictOutput.Keys | ForEach-Object { [string]$_ })
		if ($outputKeys.Count -ne $requiredKeys.Count -or
			@($requiredKeys | Where-Object { $outputKeys -cnotcontains $_ }).Count -gt 0) {
			throw "Scenario Play Mode recording output $($outputIndex + 1) must contain exactly path, producerOperationId, and producerResponsePath."
		}
		foreach ($key in $requiredKeys) {
			if ($strictOutput[$key] -isnot [string]) {
				throw "Scenario Play Mode recording output $($outputIndex + 1) property '$key' must be a JSON string."
			}
		}
		$relativePath = Assert-PlayModeRecordingOutputPath -Path ([string]$strictOutput['path'])
		if (-not $declaredPaths.Add($relativePath)) {
			throw "Scenario Play Mode recording output '$relativePath' is duplicated or aliases another Windows path."
		}
		$producerId = [string]$strictOutput['producerOperationId']
		if (-not $operationById.ContainsKey($producerId)) {
			if (@($Scenario.captures | Where-Object { [string]$_.id -ceq $producerId }).Count -eq 1) {
				throw "Scenario Play Mode recording output producer '$producerId' is a capture, not an operation."
			}
			throw "Scenario Play Mode recording output producer operation '$producerId' does not exist."
		}
		$producer = $operationById[$producerId]
		if ([string]$producer.stage -cne 'play' -or [string]$producer.tool -cne 'animation.recording.stop') {
			throw "Scenario Play Mode recording output producer '$producerId' must be a play-stage animation.recording.stop operation."
		}
		$producerStrict = $strictOperationById[$producerId]
		$expectStrict = Get-RequiredPreservedJsonMember -Object $producerStrict -Name 'expect' -Description "Recording producer operation '$producerId'"
		if ($expectStrict -isnot [System.Collections.IDictionary]) {
			throw "Recording producer operation '$producerId' expect must be one JSON object."
		}
		if (@($expectStrict.Keys) -ccontains 'ok') {
			$expectedOk = $expectStrict['ok']
			if ($expectedOk -isnot [bool] -or -not $expectedOk) {
				throw "Recording producer operation '$producerId' must declare a successful ok=true expectation."
			}
		}
		$responsePath = Assert-ArgumentBindingObjectPath -Path ([string]$strictOutput['producerResponsePath']) -Description "Recording producer '$producerId' producerResponsePath"
		if ($responsePath -cne 'data.destinationPath') {
			throw "Recording producer '$producerId' producerResponsePath must be exact 'data.destinationPath'."
		}
		$requiredFields = Get-RequiredPreservedJsonMember -Object $expectStrict -Name 'requiredFields' -Description "Recording producer operation '$producerId' expect"
		if (-not (Test-IsJsonArray -Value $requiredFields) -or
			@($requiredFields | Where-Object { $_ -isnot [string] }).Count -gt 0 -or
			@($requiredFields) -cnotcontains $responsePath) {
			throw "Recording producer operation '$producerId' must declare exact required field '$responsePath'."
		}
		$outputs.Add($output)
	}
	# A metadata file is a real authored addition made by AssetDatabase, so it
	# must be declared independently. Its declaration is valid only as the exact
	# sidecar of a declared clip and both records must point to the same stop call.
	foreach ($output in $outputs) {
		$relativePath = [string]$output.path
		$isSidecar = $relativePath.EndsWith('.teanim.meta', [System.StringComparison]::Ordinal)
		if ($isSidecar) {
			$clipPath = $relativePath.Substring(0, $relativePath.Length - '.meta'.Length)
			$clipDeclarations = @($outputs | Where-Object { [string]$_.path -ceq $clipPath })
			if ($clipDeclarations.Count -ne 1 -or
				[string]$clipDeclarations[0].producerOperationId -cne [string]$output.producerOperationId -or
				[string]$clipDeclarations[0].producerResponsePath -cne [string]$output.producerResponsePath) {
				throw "Recording sidecar '$relativePath' must pair with exact declared '$clipPath' and the same recording.stop producer."
			}
		}
		$producerOutputs = @($outputs | Where-Object {
			[string]$_.producerOperationId -ceq [string]$output.producerOperationId
		})
		if ($producerOutputs.Count -gt 2) {
			throw "Recording producer '$($output.producerOperationId)' may declare only one .teanim output and its exact paired .teanim.meta sidecar."
		}
		if ($producerOutputs.Count -eq 2) {
			$clipOutputs = @($producerOutputs | Where-Object {
				-not ([string]$_.path).EndsWith('.meta', [System.StringComparison]::Ordinal)
			})
			$sidecarOutputs = @($producerOutputs | Where-Object {
				([string]$_.path).EndsWith('.meta', [System.StringComparison]::Ordinal)
			})
			if ($clipOutputs.Count -ne 1 -or $sidecarOutputs.Count -ne 1 -or
				[string]$sidecarOutputs[0].path -cne ([string]$clipOutputs[0].path + '.meta')) {
				throw "Recording producer '$($output.producerOperationId)' may be shared only by an exact .teanim/.teanim.meta pair."
			}
		}
	}
	$Scenario | Add-Member -NotePropertyName playModeRecordingOutputs -NotePropertyValue @($outputs) -Force
}

function Get-FirstEvidenceValue {
	param([Parameter(Mandatory = $true)]$Object, [Parameter(Mandatory = $true)][string[]]$Paths)
	foreach ($path in $Paths) {
		$value = Get-NestedValue -Object $Object -Path $path
		if ($null -ne $value) { return $value }
	}
	return $null
}

function Read-FinalAnimationPerformanceEvidence {
	param([Parameter(Mandatory = $true)][string]$Path, [Parameter(Mandatory = $true)][string]$OutRoot)
	if (-not (Test-Path -LiteralPath $Path -PathType Leaf)) { throw "Final AnimationValidationRunsEveryAccumulatedFeatureWorkflowBenchmarkTRunsEveryUnrelated animation-performance evidence is missing: '$Path'." }
	Assert-PathBelow -Parent $OutRoot -Child $Path -Description 'Final AnimationValidationRunsEveryAccumulatedFeatureWorkflowBenchmarkTRunsEveryUnrelated animation-performance evidence'
	$manifestText = Get-Content -Raw -LiteralPath $Path
	$manifestStrict = ConvertFrom-JsonPreservingArrays -Json $manifestText -Description 'Final AnimationValidationRunsEveryAccumulatedFeatureWorkflowBenchmarkTRunsEveryUnrelated animation-performance evidence'
	$manifest = $manifestText | ConvertFrom-Json
	if ($manifestStrict -isnot [System.Collections.IDictionary] -or $null -eq $manifest -or $manifest -is [System.Array]) {
		throw 'Final AnimationValidationRunsEveryAccumulatedFeatureWorkflowBenchmarkTRunsEveryUnrelated animation-performance evidence must contain one JSON object.'
	}
	if ([string]$manifest.feature -ne $script:FeatureName) { throw 'Final animation-performance evidence has the wrong feature identity.' }
	$task = [string](Get-FirstEvidenceValue -Object $manifest -Paths @('task', 'gateTask'))
	if ($task -ne 'AnimationValidationRunsEveryAccumulatedFeatureWorkflowBenchmarkTRunsEveryUnrelated') { throw "Final animation-performance evidence must identify task AnimationValidationRunsEveryAccumulatedFeatureWorkflowBenchmarkTRunsEveryUnrelated; received '$task'." }
	$passed = Get-FirstEvidenceValue -Object $manifest -Paths @('passed', 'result.passed', 'overall.passed')
	$status = ([string](Get-FirstEvidenceValue -Object $manifest -Paths @('status', 'outcome', 'result.status', 'result.outcome', 'overall.status'))).ToLowerInvariant()
	if ($passed -isnot [bool] -or -not [bool]$passed -or $status -notin @('pass', 'passed', 'success', 'succeeded')) {
		throw 'Final animation-performance evidence does not explicitly report a passing result.'
	}
	$deferred = Get-FirstEvidenceValue -Object $manifest -Paths @('deferred', 'performanceDeferred', 'authorizedPerformanceDeferral', 'performance.deferred', 'performance.authorizedPerformanceDeferral', 'result.authorizedPerformanceDeferral')
	if ($deferred -isnot [bool] -or [bool]$deferred) { throw 'Final animation-performance evidence must explicitly report deferred=false.' }

	$scopeValues = [System.Collections.Generic.List[string]]::new()
	foreach ($value in @(Get-FirstEvidenceValue -Object $manifest -Paths @('scopes', 'scope', 'coverage.scopes', 'performance.scopes'))) {
		if (-not [string]::IsNullOrWhiteSpace([string]$value)) { $scopeValues.Add(([string]$value).ToLowerInvariant()) }
	}
	$scopeText = $scopeValues -join ';'
	$scopeComplete = $scopeText -match 'final[- ]animation[- ]performance' -or (($scopeValues -contains 'feature benchmarks') -and ($scopeValues -contains 'feature comparisons') -and ($scopeValues -contains 'performance acceptance gates'))
	if (-not $scopeComplete) { throw 'Final animation-performance evidence lacks explicit AnimationValidationRunsEveryAccumulatedFeatureWorkflowBenchmarkTRunsEveryUnrelated feature benchmark/comparison/acceptance scope.' }

	$criteria = [System.Collections.Generic.List[object]]::new()
	foreach ($collection in @(
		(Get-FirstEvidenceValue -Object $manifest -Paths @('successCriteria', 'coveredSuccessCriteria', 'coverage.successCriteria')),
		(Get-OptionalProperty -Object $manifest -Name 'criteria'),
		(Get-NestedValue -Object $manifest -Path 'acceptance.rowsAccepted')
	)) {
		foreach ($entry in @($collection)) {
			# A bare criterion ID proves only that a producer knew the requirement's
			# name. Final acceptance requires a typed object carrying both a passing
			# outcome and the measurements/evidence used to reach that outcome.
			if ($entry -is [string]) {
				if ([string]$entry -match '^SC-[0-9]{3}$') {
					throw "Final animation-performance criterion '$entry' is a bare ID without passing measurement evidence."
				}
				continue
			}
			$id = [string](Get-OptionalProperty -Object $entry -Name 'id')
			if ($id -match '^SC-[0-9]{3}$') { $criteria.Add($entry) }
		}
	}
	$criterionIds = [System.Collections.Generic.HashSet[string]]::new([System.StringComparer]::Ordinal)
	foreach ($criterion in @($criteria)) {
		$criterionId = [string](Get-RequiredJsonProperty -Object $criterion -Name 'id' -Description 'Final animation-performance criterion')
		if (-not $criterionIds.Add($criterionId)) { throw "Final animation-performance criterion '$criterionId' is duplicated." }
	}
	foreach ($requiredId in $script:DeferredPerformanceSuccessCriteria) {
		$matches = @($criteria | Where-Object { [string]$_.id -eq $requiredId })
		if ($matches.Count -ne 1) { throw "Final animation-performance evidence must contain exactly one object for required success criterion '$requiredId'." }
		$match = $matches[0]
		$criterionPassed = Get-RequiredJsonProperty -Object $match -Name 'passed' -Description "Final animation-performance criterion '$requiredId'"
		$criterionStatus = ([string](Get-RequiredJsonProperty -Object $match -Name 'status' -Description "Final animation-performance criterion '$requiredId'")).ToLowerInvariant()
		if ($criterionPassed -isnot [bool] -or -not $criterionPassed -or $criterionStatus -notin @('pass', 'passed', 'success', 'succeeded', 'accepted')) {
			throw "Final animation-performance criterion '$requiredId' is not explicitly passing."
		}
		$criterionProof = Get-FirstEvidenceValue -Object $match -Paths @('metrics', 'measurements', 'evidence', 'result')
		if ($null -eq $criterionProof -or
			($criterionProof -is [string] -and [string]::IsNullOrWhiteSpace([string]$criterionProof)) -or
			($criterionProof -is [System.Array] -and @($criterionProof).Count -lt 1) -or
			($criterionProof -isnot [string] -and $criterionProof -isnot [System.Array] -and @($criterionProof.PSObject.Properties).Count -lt 1)) {
			throw "Final animation-performance criterion '$requiredId' lacks non-empty metrics, measurements, evidence, or result data."
		}
	}
	return [ordered]@{
		manifest = $Path
		sha256 = Get-Sha256 -Path $Path
		task = 'AnimationValidationRunsEveryAccumulatedFeatureWorkflowBenchmarkTRunsEveryUnrelated'
		status = $status
		passed = $true
		deferred = $false
		scopes = @($scopeValues)
		successCriteria = @($script:DeferredPerformanceSuccessCriteria)
		freshnessProof = 'AnimationValidationRunsEveryAccumulatedFeatureWorkflowBenchmarkTRunsEveryUnrelated task identity, non-deferred status, and content hash copied into this live-gate run'
		freshnessLimitation = 'No pre-existing canonical AnimationValidationRunsEveryAccumulatedFeatureWorkflowBenchmarkTRunsEveryUnrelated manifest schema defines a capture timestamp or live-install artifact binding.'
	}
}

function Assert-OperationExpectation {
	param([Parameter(Mandatory = $true)]$Envelope, $Expectation, [Parameter(Mandatory = $true)][string]$Id)
	$expectedOk = $true
	if ($null -ne $Expectation -and $null -ne (Get-OptionalProperty -Object $Expectation -Name 'ok')) { $expectedOk = [bool]$Expectation.ok }
	if ([bool]$Envelope.ok -ne $expectedOk) { throw "Scenario operation '$Id' returned ok=$($Envelope.ok), expected $expectedOk." }
	if ($null -eq $Expectation) { return }
	$expectedStatus = Get-OptionalProperty -Object $Expectation -Name 'status'
	if ($null -ne $expectedStatus -and [string](Get-NestedValue -Object $Envelope -Path 'status') -ne [string]$expectedStatus) {
		throw "Scenario operation '$Id' returned an unexpected status."
	}
	$expectedError = Get-OptionalProperty -Object $Expectation -Name 'errorCategory'
	if ($null -ne $expectedError -and [string](Get-NestedValue -Object $Envelope -Path 'error.category') -ne [string]$expectedError) {
		throw "Scenario operation '$Id' returned an unexpected error category."
	}
	$requiredFields = Get-OptionalProperty -Object $Expectation -Name 'requiredFields'
	# PowerShell turns @($null) into a one-item collection. Guard the optional
	# member explicitly so an expectation that omits requiredFields does not
	# accidentally validate an empty JSON path.
	if ($null -ne $requiredFields) {
		foreach ($field in @($requiredFields)) {
			if (-not (Test-NestedJsonFieldPresent -Object $Envelope -Path ([string]$field))) {
				throw "Scenario operation '$Id' omitted required result field '$field'."
			}
		}
	}
	$equalFields = Get-OptionalProperty -Object $Expectation -Name 'fieldEquals'
	if ($null -ne $equalFields) {
		foreach ($property in $equalFields.PSObject.Properties) {
			$actual = Get-NestedValue -Object $Envelope -Path $property.Name
			$actualJson = $actual | ConvertTo-Json -Depth 20 -Compress
			$expectedJson = $property.Value | ConvertTo-Json -Depth 20 -Compress
			if ($actualJson -cne $expectedJson) {
				throw "Scenario operation '$Id' field '$($property.Name)' did not equal its required value."
			}
		}
	}
	$oneOfFields = Get-OptionalProperty -Object $Expectation -Name 'fieldOneOf'
	if ($null -ne $oneOfFields) {
		foreach ($property in $oneOfFields.PSObject.Properties) {
			$actualJson = (Get-NestedValue -Object $Envelope -Path $property.Name) | ConvertTo-Json -Depth 20 -Compress
			$allowed = @($property.Value | ForEach-Object { $_ | ConvertTo-Json -Depth 20 -Compress })
			if ($allowed -cnotcontains $actualJson) {
				throw "Scenario operation '$Id' field '$($property.Name)' was outside its allowed values."
			}
		}
	}
}

function New-ScenarioExpectationRecord {
	param($Expectation)
	# Persist the exact expectation shape that was evaluated so the later
	# evidence-lock pass can compare the transcript summary with the reviewed
	# scenario rather than trusting a self-authored "ok": true record.
	$expectedOk = $true
	if ($null -ne $Expectation -and $null -ne (Get-OptionalProperty -Object $Expectation -Name 'ok')) {
		$expectedOk = [bool]$Expectation.ok
	}
	return [ordered]@{
		ok = $expectedOk
		status = if ($null -eq $Expectation) { $null } else { Get-OptionalProperty -Object $Expectation -Name 'status' }
		errorCategory = if ($null -eq $Expectation) { $null } else { Get-OptionalProperty -Object $Expectation -Name 'errorCategory' }
		requiredFields = @(if ($null -ne $Expectation) { Get-OptionalProperty -Object $Expectation -Name 'requiredFields' })
		fieldEquals = if ($null -eq $Expectation) { $null } else { Get-OptionalProperty -Object $Expectation -Name 'fieldEquals' }
		fieldOneOf = if ($null -eq $Expectation) { $null } else { Get-OptionalProperty -Object $Expectation -Name 'fieldOneOf' }
	}
}

function Read-McpTranscriptForOperationEvidence {
	param(
		[Parameter(Mandatory = $true)]$Reference,
		[Parameter(Mandatory = $true)][string]$RunRoot
	)
	$relativePath = [string](Get-RequiredJsonProperty -Object $Reference -Name 'path' -Description 'MCP transcript evidence reference')
	if ($relativePath.Replace('\', '/') -cne 'editor/mcp.jsonl') {
		throw 'MCP transcript evidence reference must use editor/mcp.jsonl.'
	}
	$transcriptPath = Get-AbsolutePath -Path $relativePath -BasePath $RunRoot
	Assert-PathBelow -Parent $RunRoot -Child $transcriptPath -Description 'MCP operation transcript'
	if (-not (Test-Path -LiteralPath $transcriptPath -PathType Leaf) -or (Get-Item -LiteralPath $transcriptPath).Length -eq 0) {
		throw 'MCP operation transcript is missing or empty.'
	}
	$expectedHash = [string](Get-RequiredJsonProperty -Object $Reference -Name 'sha256' -Description 'MCP transcript evidence reference')
	if ($expectedHash -notmatch '^[0-9a-f]{64}$' -or (Get-Sha256 -Path $transcriptPath) -cne $expectedHash) {
		throw 'MCP operation transcript hash does not match its evidence reference.'
	}
	$entries = [System.Collections.Generic.List[object]]::new()
	$previousSequence = 0
	$lineNumber = 0
	foreach ($line in @(Get-Content -LiteralPath $transcriptPath | Where-Object { -not [string]::IsNullOrWhiteSpace($_) })) {
		$lineNumber++
		$lineStrict = ConvertFrom-JsonPreservingArrays -Json $line -Description "MCP transcript line $lineNumber"
		$entry = $line | ConvertFrom-Json
		if ($lineStrict -isnot [System.Collections.IDictionary] -or $null -eq $entry -or $entry -is [System.Array]) {
			throw "MCP transcript line $lineNumber must contain one JSON object."
		}
		$strictSequence = Get-RequiredPreservedJsonMember -Object $lineStrict -Name 'sequence' -Description "MCP transcript line $lineNumber"
		if (($strictSequence -isnot [int] -and $strictSequence -isnot [long]) -or
			[int64]$strictSequence -lt 1 -or [int64]$strictSequence -lt $previousSequence) {
			throw 'MCP transcript sequences must be positive and monotonically ordered.'
		}
		$previousSequence = [int64]$strictSequence
		$entries.Add($entry)
	}
	return [ordered]@{
		path = $relativePath
		sha256 = $expectedHash
		entries = @($entries)
	}
}

function Get-TranscriptJsonResourceAtSequence {
	param(
		[Parameter(Mandatory = $true)]$TranscriptState,
		[Parameter(Mandatory = $true)][int]$Sequence,
		[Parameter(Mandatory = $true)][string]$Uri
	)
	$matches = @($TranscriptState.entries | Where-Object {
		[int](Get-OptionalProperty -Object $_ -Name 'sequence') -eq $Sequence -and
		$null -ne (Get-OptionalProperty -Object $_ -Name 'response')
	})
	if ($matches.Count -ne 1) {
		throw "Approval resource '$Uri' sequence $Sequence does not identify exactly one transcript response."
	}
	$entry = $matches[0]
	$request = Get-RequiredJsonProperty -Object $entry -Name 'request' -Description "Approval resource '$Uri' transcript"
	$params = Get-RequiredJsonProperty -Object $request -Name 'params' -Description "Approval resource '$Uri' transcript request"
	$response = Get-RequiredJsonProperty -Object $entry -Name 'response' -Description "Approval resource '$Uri' transcript"
	$requestId = Get-RequiredJsonProperty -Object $request -Name 'id' -Description "Approval resource '$Uri' transcript request"
	$responseId = Get-RequiredJsonProperty -Object $response -Name 'id' -Description "Approval resource '$Uri' transcript response"
	if (($requestId -isnot [int] -and $requestId -isnot [long]) -or
		($responseId -isnot [int] -and $responseId -isnot [long]) -or
		[int64]$requestId -ne $Sequence -or [int64]$responseId -ne $Sequence -or
		[string]$request.method -cne 'resources/read' -or [string]$params.uri -cne $Uri) {
		throw "Approval resource '$Uri' transcript identity does not match sequence $Sequence."
	}
	$contents = @(Get-NestedValue -Object $response -Path 'result.contents')
	if ($contents.Count -ne 1 -or
		[string](Get-RequiredJsonProperty -Object $contents[0] -Name 'mimeType' -Description "Approval resource '$Uri' transcript content") -cne 'application/json') {
		throw "Approval resource '$Uri' transcript did not retain exactly one application/json document."
	}
	$text = [string](Get-RequiredJsonProperty -Object $contents[0] -Name 'text' -Description "Approval resource '$Uri' transcript content")
	$strict = ConvertFrom-JsonPreservingArrays -Json $text -Description "Approval resource '$Uri' transcript document"
	$document = $text | ConvertFrom-Json
	if ($strict -isnot [System.Collections.IDictionary] -or $null -eq $document -or $document -is [System.Array]) {
		throw "Approval resource '$Uri' transcript document must be one JSON object."
	}
	return [ordered]@{
		uri = $Uri
		mcpSequence = $Sequence
		documentSha256 = Get-StringSha256 -Value $text
		document = $document
		strict = $strict
	}
}

function Read-AndValidateScenarioApprovalEvidence {
	param(
		[Parameter(Mandatory = $true)]$Summary,
		[Parameter(Mandatory = $true)]$SummaryStrict,
		[Parameter(Mandatory = $true)]$Definition,
		[Parameter(Mandatory = $true)]$ExpectedArguments,
		[Parameter(Mandatory = $true)]$InitialEnvelope,
		[Parameter(Mandatory = $true)][int]$InitialMcpSequence,
		[Parameter(Mandatory = $true)]$TranscriptState,
		[Parameter(Mandatory = $true)][string]$RunRoot,
		[Parameter(Mandatory = $true)][string]$ScenarioPhase,
		[Parameter(Mandatory = $true)]$ApprovalIds
	)
	$operationId = [string]$Definition.id
	$capability = [string]$Definition.tool
	$summaryKeys = @(
		'policy', 'capability', 'approvalId', 'requestId', 'argumentsSha256',
		'argumentsFingerprint', 'impactFingerprint', 'projectGeneration',
		'sceneGeneration', 'worldGeneration', 'initialMcpSequence',
		'initialEnvelopeSha256', 'pendingMcpSequence', 'pendingDocumentSha256',
		'pendingEntrySha256', 'completionMcpSequence', 'completionEntrySha256',
		'dispatchResultSha256', 'evidence'
	)
	Assert-ExactPreservedJsonObjectKeys -Object $SummaryStrict -RequiredKeys $summaryKeys -Description "Scenario approval '$operationId' summary"
	$approvalId = [string]$Summary.approvalId
	$requestId = [string]$Summary.requestId
	if ([string]$Summary.policy -cne 'high_impact' -or [string]$Summary.capability -cne $capability -or
		$approvalId -notmatch '^approval-[0-9]+$' -or [string]::IsNullOrWhiteSpace($requestId) -or
		[string]$Summary.argumentsSha256 -cne (Get-CanonicalJsonSha256 -Value $ExpectedArguments) -or
		[string]$Summary.argumentsFingerprint -notmatch '^[0-9a-f]{1,16}$' -or
		[string]$Summary.impactFingerprint -notmatch '^[0-9a-f]{1,16}$') {
		throw "Scenario approval '$operationId' summary does not match its reviewed policy, capability, arguments, or identifiers."
	}
	if (-not $ApprovalIds.Add($approvalId)) {
		throw "Scenario approval evidence replays approval ID '$approvalId'."
	}
	foreach ($integerName in @(
		'projectGeneration', 'sceneGeneration', 'worldGeneration',
		'initialMcpSequence', 'pendingMcpSequence', 'completionMcpSequence'
	)) {
		$strictInteger = Get-RequiredPreservedJsonMember -Object $SummaryStrict -Name $integerName -Description "Scenario approval '$operationId' summary"
		# Windows PowerShell's strict JavaScriptSerializer returns ordinary JSON
		# integers as Int32/Int64, but an unsigned 64-bit generation above
		# Int64.MaxValue is represented exactly as Decimal. Accept only that
		# overflow range when it is integral and still fits UInt64; smaller Decimal
		# values remain rejected so fractional JSON values cannot masquerade as the
		# sequence numbers owned by the gate.
		$isSignedJsonInteger = ($strictInteger -is [int] -or $strictInteger -is [long]) -and [decimal]$strictInteger -ge 0
		$isUnsignedGenerationInteger = $strictInteger -is [decimal] -and
			[decimal]$strictInteger -gt [decimal][long]::MaxValue -and
			[decimal]$strictInteger -le [decimal][UInt64]::MaxValue -and
			[decimal]::Truncate([decimal]$strictInteger) -eq [decimal]$strictInteger
		if (-not $isSignedJsonInteger -and -not $isUnsignedGenerationInteger) {
			throw "Scenario approval '$operationId' summary '$integerName' must be a nonnegative JSON integer."
		}
	}
	foreach ($digestName in @(
		'argumentsSha256', 'initialEnvelopeSha256', 'pendingDocumentSha256',
		'pendingEntrySha256', 'completionEntrySha256', 'dispatchResultSha256'
	)) {
		$strictDigest = Get-RequiredPreservedJsonMember -Object $SummaryStrict -Name $digestName -Description "Scenario approval '$operationId' summary"
		if ($strictDigest -isnot [string] -or [string]$strictDigest -notmatch '^[0-9a-f]{64}$') {
			throw "Scenario approval '$operationId' summary '$digestName' must be a lowercase SHA-256 digest."
		}
	}
	if ([int]$Summary.initialMcpSequence -ne $InitialMcpSequence -or
		[int]$Summary.pendingMcpSequence -le $InitialMcpSequence -or
		[int]$Summary.completionMcpSequence -le [int]$Summary.pendingMcpSequence) {
		throw "Scenario approval '$operationId' has missing, aliased, or reordered approval MCP sequences."
	}
	$referenceStrict = Get-RequiredPreservedJsonMember -Object $SummaryStrict -Name 'evidence' -Description "Scenario approval '$operationId' summary"
	Assert-ExactPreservedJsonObjectKeys -Object $referenceStrict -RequiredKeys @('path', 'sha256') -Description "Scenario approval '$operationId' evidence reference"
	$reference = Get-RequiredJsonProperty -Object $Summary -Name 'evidence' -Description "Scenario approval '$operationId' summary"
	$expectedRelativePath = "editor/approvals/$operationId.json"
	if ([string]$reference.path -cne $expectedRelativePath -or [string]$reference.sha256 -notmatch '^[0-9a-f]{64}$') {
		throw "Scenario approval '$operationId' points to an unexpected prompt evidence artifact."
	}
	$evidencePath = Get-AbsolutePath -Path ([string]$reference.path) -BasePath $RunRoot
	Assert-PathBelow -Parent $RunRoot -Child $evidencePath -Description "Scenario approval '$operationId' artifact"
	if (-not (Test-Path -LiteralPath $evidencePath -PathType Leaf) -or
		(Get-Sha256 -Path $evidencePath) -cne [string]$reference.sha256) {
		throw "Scenario approval '$operationId' artifact is missing or its hash does not match."
	}
	$text = Get-Content -Raw -LiteralPath $evidencePath
	$strict = ConvertFrom-JsonPreservingArrays -Json $text -Description "Scenario approval '$operationId' artifact"
	$document = $text | ConvertFrom-Json
	Assert-ExactPreservedJsonObjectKeys -Object $strict -RequiredKeys @(
		'schemaVersion', 'phase', 'operationId', 'policy', 'capability',
		'approvalId', 'requestId', 'argumentsSha256', 'impactFingerprint',
		'projectGeneration', 'sceneGeneration', 'worldGeneration', 'status',
		'initial', 'pending', 'recentPolls', 'completion'
	) -Description "Scenario approval '$operationId' artifact"
	$schemaStrict = Get-RequiredPreservedJsonMember -Object $strict -Name 'schemaVersion' -Description "Scenario approval '$operationId' artifact"
	if (($schemaStrict -isnot [int] -and $schemaStrict -isnot [long]) -or [int]$schemaStrict -ne 1 -or
		[string]$document.phase -cne $ScenarioPhase -or [string]$document.operationId -cne $operationId -or
		[string]$document.policy -cne 'high_impact' -or [string]$document.capability -cne $capability -or
		[string]$document.approvalId -cne $approvalId -or [string]$document.requestId -cne $requestId -or
		[string]$document.argumentsSha256 -cne [string]$Summary.argumentsSha256 -or
		[string]$document.impactFingerprint -cne [string]$Summary.impactFingerprint -or
		[UInt64]$document.projectGeneration -ne [UInt64]$Summary.projectGeneration -or
		[UInt64]$document.sceneGeneration -ne [UInt64]$Summary.sceneGeneration -or
		[UInt64]$document.worldGeneration -ne [UInt64]$Summary.worldGeneration -or
		[string]$document.status -cne 'approved') {
		throw "Scenario approval '$operationId' artifact identity or final state was tampered."
	}
	$initialStrict = Get-RequiredPreservedJsonMember -Object $strict -Name 'initial' -Description "Scenario approval '$operationId' artifact"
	Assert-ExactPreservedJsonObjectKeys -Object $initialStrict -RequiredKeys @(
		'mcpSequence', 'envelopeSha256', 'envelope'
	) -Description "Scenario approval '$operationId' initial proof"
	$initialProof = $document.initial
	$initialEnvelopeSha256 = Get-StringSha256 -Value ($InitialEnvelope | ConvertTo-Json -Depth 80 -Compress)
	if ([int]$initialProof.mcpSequence -ne $InitialMcpSequence -or
		[string]$initialProof.envelopeSha256 -cne $initialEnvelopeSha256 -or
		[string]$Summary.initialEnvelopeSha256 -cne $initialEnvelopeSha256 -or
		(Get-CanonicalJsonText -Value $initialProof.envelope) -cne (Get-CanonicalJsonText -Value $InitialEnvelope)) {
		throw "Scenario approval '$operationId' initial waiting_approval proof does not match its transcript."
	}
	$initialOk = Get-RequiredJsonProperty -Object $InitialEnvelope -Name 'ok' -Description "Scenario approval '$operationId' initial envelope"
	if ($initialOk -isnot [bool] -or $initialOk -or [string]$InitialEnvelope.status -cne 'waiting_approval' -or
		[string]$InitialEnvelope.requestId -cne $requestId -or
		[string]$InitialEnvelope.data.approvalId -cne $approvalId -or
		[string]$InitialEnvelope.data.capability -cne $capability -or
		[string]$InitialEnvelope.data.impactFingerprint -cne [string]$Summary.impactFingerprint) {
		throw "Scenario approval '$operationId' initial envelope is not its exact pending request."
	}
	$pendingStrict = Get-RequiredPreservedJsonMember -Object $strict -Name 'pending' -Description "Scenario approval '$operationId' artifact"
	Assert-ExactPreservedJsonObjectKeys -Object $pendingStrict -RequiredKeys @(
		'mcpSequence', 'resourceUri', 'documentSha256', 'entrySha256', 'entry'
	) -Description "Scenario approval '$operationId' pending proof"
	$pendingTranscript = Get-TranscriptJsonResourceAtSequence -TranscriptState $TranscriptState -Sequence ([int]$Summary.pendingMcpSequence) -Uri 'terraengine://approvals/pending'
	$pendingEntry = Get-PendingApprovalResourceEntry -ResourceState $pendingTranscript -ApprovalId $approvalId -RequestId $requestId -Capability $capability
	if ([int]$document.pending.mcpSequence -ne [int]$Summary.pendingMcpSequence -or
		[string]$document.pending.resourceUri -cne 'terraengine://approvals/pending' -or
		[string]$document.pending.documentSha256 -cne [string]$pendingTranscript.documentSha256 -or
		[string]$Summary.pendingDocumentSha256 -cne [string]$pendingTranscript.documentSha256 -or
		[string]$document.pending.entrySha256 -cne (Get-CanonicalJsonSha256 -Value $pendingEntry) -or
		[string]$Summary.pendingEntrySha256 -cne (Get-CanonicalJsonSha256 -Value $pendingEntry) -or
		(Get-CanonicalJsonText -Value $document.pending.entry) -cne (Get-CanonicalJsonText -Value $pendingEntry)) {
		throw "Scenario approval '$operationId' pending resource proof was tampered."
	}
	$recentStrict = Get-RequiredPreservedJsonMember -Object $strict -Name 'recentPolls' -Description "Scenario approval '$operationId' artifact"
	$recentPolls = @($document.recentPolls)
	if (-not (Test-IsJsonArray -Value $recentStrict) -or $recentPolls.Count -lt 1) {
		throw "Scenario approval '$operationId' must preserve every recent-resource poll in a non-empty JSON array."
	}
	$recordedRecentSequences = [System.Collections.Generic.List[int]]::new()
	$finalEntry = $null
	$previousRecentSequence = [int]$Summary.pendingMcpSequence
	for ($pollIndex = 0; $pollIndex -lt $recentPolls.Count; ++$pollIndex) {
		$pollStrict = @($recentStrict)[$pollIndex]
		Assert-ExactPreservedJsonObjectKeys -Object $pollStrict -RequiredKeys @(
			'mcpSequence', 'resourceUri', 'documentSha256', 'entrySha256',
			'state', 'approved', 'dispatchCompleted'
		) -Description "Scenario approval '$operationId' recent poll $($pollIndex + 1)"
		$poll = $recentPolls[$pollIndex]
		$pollSequence = [int]$poll.mcpSequence
		if ($pollSequence -le $previousRecentSequence) {
			throw "Scenario approval '$operationId' recent-resource sequences are reordered or duplicated."
		}
		$previousRecentSequence = $pollSequence
		$recordedRecentSequences.Add($pollSequence)
		$recentTranscript = Get-TranscriptJsonResourceAtSequence -TranscriptState $TranscriptState -Sequence $pollSequence -Uri 'terraengine://approvals/recent'
		$recentEntry = Get-RecentApprovalResourceEntry `
			-ResourceState $recentTranscript `
			-ApprovalId $approvalId `
			-RequestId $requestId `
			-Capability $capability `
			-ExpectedImpactFingerprint ([string]$Summary.impactFingerprint) `
			-ExpectedProjectGeneration ([UInt64]$Summary.projectGeneration) `
			-ExpectedSceneGeneration ([UInt64]$Summary.sceneGeneration) `
			-ExpectedWorldGeneration ([UInt64]$Summary.worldGeneration)
		if ([string]$poll.resourceUri -cne 'terraengine://approvals/recent' -or
			[string]$poll.documentSha256 -cne [string]$recentTranscript.documentSha256 -or
			[string]$poll.entrySha256 -cne (Get-CanonicalJsonSha256 -Value $recentEntry) -or
			[string]$poll.state -cne [string]$recentEntry.state -or
			[bool]$poll.approved -ne [bool]$recentEntry.approved -or
			[bool]$poll.dispatchCompleted -ne [bool]$recentEntry.dispatchCompleted -or
			[string]$recentEntry.argumentsFingerprint -cne [string]$Summary.argumentsFingerprint) {
			throw "Scenario approval '$operationId' recent poll $($pollIndex + 1) was tampered."
		}
		if ($pollIndex -lt ($recentPolls.Count - 1) -and [bool]$recentEntry.dispatchCompleted) {
			throw "Scenario approval '$operationId' continued polling after a terminal dispatch result."
		}
		$finalEntry = $recentEntry
	}
	$unlistedRecentSequences = @($TranscriptState.entries | Where-Object {
		$sequence = [int](Get-OptionalProperty -Object $_ -Name 'sequence')
		if ($sequence -le [int]$Summary.pendingMcpSequence -or $sequence -gt [int]$Summary.completionMcpSequence) { return $false }
		$request = Get-OptionalProperty -Object $_ -Name 'request'
		$params = Get-OptionalProperty -Object $request -Name 'params'
		return [string](Get-OptionalProperty -Object $request -Name 'method') -ceq 'resources/read' -and
			[string](Get-OptionalProperty -Object $params -Name 'uri') -ceq 'terraengine://approvals/recent' -and
			$null -ne (Get-OptionalProperty -Object $_ -Name 'response')
	} | ForEach-Object { [int]$_.sequence })
	if ((Get-CanonicalJsonText -Value @($recordedRecentSequences)) -cne
		(Get-CanonicalJsonText -Value @($unlistedRecentSequences))) {
		throw "Scenario approval '$operationId' does not preserve every recent-resource MCP sequence."
	}
	$completionStrict = Get-RequiredPreservedJsonMember -Object $strict -Name 'completion' -Description "Scenario approval '$operationId' artifact"
	Assert-ExactPreservedJsonObjectKeys -Object $completionStrict -RequiredKeys @(
		'mcpSequence', 'entrySha256', 'state', 'approved', 'dispatchCompleted',
		'dispatchResultSha256', 'dispatchResult'
	) -Description "Scenario approval '$operationId' completion proof"
	$completion = $document.completion
	$dispatchResult = Get-RequiredJsonProperty -Object $finalEntry -Name 'dispatchResult' -Description "Scenario approval '$operationId' final recent entry"
	$dispatchResultSha256 = Get-StringSha256 -Value ($dispatchResult | ConvertTo-Json -Depth 80 -Compress)
	if ([int]$completion.mcpSequence -ne [int]$Summary.completionMcpSequence -or
		[int]$completion.mcpSequence -ne $recordedRecentSequences[$recordedRecentSequences.Count - 1] -or
		[string]$completion.entrySha256 -cne (Get-CanonicalJsonSha256 -Value $finalEntry) -or
		[string]$Summary.completionEntrySha256 -cne [string]$completion.entrySha256 -or
		[string]$completion.state -cne 'approved' -or -not [bool]$completion.approved -or
		-not [bool]$completion.dispatchCompleted -or [string]$finalEntry.state -cne 'approved' -or
		-not [bool]$finalEntry.approved -or -not [bool]$finalEntry.dispatchCompleted -or
		[string]$completion.dispatchResultSha256 -cne $dispatchResultSha256 -or
		[string]$Summary.dispatchResultSha256 -cne $dispatchResultSha256 -or
		(Get-CanonicalJsonText -Value $completion.dispatchResult) -cne (Get-CanonicalJsonText -Value $dispatchResult)) {
		throw "Scenario approval '$operationId' completion result is denied, incomplete, replayed, or tampered."
	}
	return [ordered]@{
		envelope = $dispatchResult
		summary = $Summary
		initialMcpSequence = $InitialMcpSequence
		completionMcpSequence = [int]$Summary.completionMcpSequence
		evidenceSha256 = [string]$reference.sha256
	}
}

function Read-AndValidateOperationEvidence {
	param(
		[Parameter(Mandatory = $true)][string]$Path,
		[Parameter(Mandatory = $true)]$Scenario,
		[Parameter(Mandatory = $true)][string]$RunRoot,
		[ValidateSet(1, 2)][int]$MinimumSchemaVersion = 1
	)
	if (-not (Test-Path -LiteralPath $Path -PathType Leaf) -or (Get-Item -LiteralPath $Path).Length -eq 0) {
		throw "MCP operation evidence is missing or empty: '$Path'."
	}
	$text = Get-Content -Raw -LiteralPath $Path
	$strict = ConvertFrom-JsonPreservingArrays -Json $text -Description 'MCP operation evidence'
	$evidence = $text | ConvertFrom-Json
	if ($strict -isnot [System.Collections.IDictionary] -or $null -eq $evidence -or $evidence -is [System.Array]) {
		throw 'MCP operation evidence must contain one JSON object.'
	}
	$strictSchemaVersion = Get-RequiredPreservedJsonMember -Object $strict -Name 'schemaVersion' -Description 'MCP operation evidence'
	if (($strictSchemaVersion -isnot [int] -and $strictSchemaVersion -isnot [long]) -or
		[int]$strictSchemaVersion -notin @(1, 2) -or
		[string](Get-RequiredJsonProperty -Object $evidence -Name 'phase' -Description 'MCP operation evidence') -cne [string]$Scenario.phase) {
		throw 'MCP operation evidence schema/phase does not match the reviewed scenario.'
	}
	$schemaVersion = [int]$strictSchemaVersion
	# Schema v1 remains readable for historical no-binding evidence. Live runs
	# pass MinimumSchemaVersion=2 so an edited summary cannot discard transcript,
	# sequence, resolved-argument, or binding provenance after the run.
	if ($schemaVersion -lt $MinimumSchemaVersion) {
		throw "MCP operation evidence requires schemaVersion $MinimumSchemaVersion or later for this validation path."
	}
	$scenarioBindingCount = @($Scenario.operations | ForEach-Object { @(Get-OptionalProperty -Object $_ -Name 'argumentBindings') }).Count
	if ($scenarioBindingCount -gt 0 -and $schemaVersion -ne 2) {
		throw 'Scenario argument bindings require schemaVersion 2 MCP operation evidence.'
	}
	$transcriptState = $null
	if ($schemaVersion -eq 2) {
		$transcriptReference = Get-RequiredJsonProperty -Object $evidence -Name 'transcriptEvidence' -Description 'MCP operation evidence'
		$transcriptState = Read-McpTranscriptForOperationEvidence -Reference $transcriptReference -RunRoot $RunRoot
	}
	$reconnectPassed = Get-RequiredPreservedJsonMember -Object $strict -Name 'reconnectPassed' -Description 'MCP operation evidence'
	if ($reconnectPassed -isnot [bool] -or -not $reconnectPassed) {
		throw "MCP operation evidence property 'reconnectPassed' must be the JSON Boolean true."
	}
	$reconnectReference = Get-RequiredJsonProperty -Object $evidence -Name 'reconnectEvidence' -Description 'MCP operation evidence'
	$reconnectRelativePath = [string](Get-RequiredJsonProperty -Object $reconnectReference -Name 'path' -Description 'MCP reconnect evidence reference')
	if ($reconnectRelativePath.Replace('\', '/') -cne 'editor/mcp-reconnect.json') { throw 'MCP reconnect evidence reference must use editor/mcp-reconnect.json.' }
	$reconnectPath = Get-AbsolutePath -Path $reconnectRelativePath -BasePath $RunRoot
	Assert-PathBelow -Parent $RunRoot -Child $reconnectPath -Description 'MCP reconnect evidence'
	if (-not (Test-Path -LiteralPath $reconnectPath -PathType Leaf) -or (Get-Item -LiteralPath $reconnectPath).Length -eq 0) { throw 'MCP reconnect evidence is missing or empty.' }
	$reconnectHash = [string](Get-RequiredJsonProperty -Object $reconnectReference -Name 'sha256' -Description 'MCP reconnect evidence reference')
	if ($reconnectHash -notmatch '^[0-9a-f]{64}$' -or (Get-Sha256 -Path $reconnectPath) -cne $reconnectHash) { throw 'MCP reconnect evidence hash does not match its summary.' }
	$reconnectText = Get-Content -Raw -LiteralPath $reconnectPath
	$reconnectStrict = ConvertFrom-JsonPreservingArrays -Json $reconnectText -Description 'MCP reconnect evidence'
	$reconnectDocument = $reconnectText | ConvertFrom-Json
	if ($reconnectStrict -isnot [System.Collections.IDictionary] -or [int](Get-RequiredJsonProperty -Object $reconnectDocument -Name 'schemaVersion' -Description 'MCP reconnect evidence') -ne 1) { throw 'MCP reconnect evidence must be one schemaVersion 1 object.' }
	$reconnectDocumentPassed = Get-RequiredJsonProperty -Object $reconnectDocument -Name 'passed' -Description 'MCP reconnect evidence'
	$boundary = Get-RequiredJsonProperty -Object $reconnectDocument -Name 'connectionBoundary' -Description 'MCP reconnect evidence'
	$freshInitialize = Get-RequiredJsonProperty -Object $reconnectDocument -Name 'freshInitialize' -Description 'MCP reconnect evidence'
	$postReconnect = Get-RequiredJsonProperty -Object $reconnectDocument -Name 'postReconnect' -Description 'MCP reconnect evidence'
	$disableKeepAlive = Get-RequiredJsonProperty -Object $boundary -Name 'disableKeepAlive' -Description 'MCP reconnect connection boundary'
	$requiredToolsPresent = Get-RequiredJsonProperty -Object $postReconnect -Name 'requiredToolsPresent' -Description 'MCP reconnect post operations'
	$postOperationPassed = Get-RequiredJsonProperty -Object $postReconnect -Name 'operationPassed' -Description 'MCP reconnect post operations'
	$boundarySequence = [int](Get-RequiredJsonProperty -Object $boundary -Name 'sequence' -Description 'MCP reconnect connection boundary')
	$freshSequence = [int](Get-RequiredJsonProperty -Object $freshInitialize -Name 'sequence' -Description 'MCP reconnect fresh initialize')
	$toolListSequence = [int](Get-RequiredJsonProperty -Object $postReconnect -Name 'toolListSequence' -Description 'MCP reconnect post operations')
	$playStateSequence = [int](Get-RequiredJsonProperty -Object $postReconnect -Name 'playStateSequence' -Description 'MCP reconnect post operations')
	if ($reconnectDocumentPassed -isnot [bool] -or -not $reconnectDocumentPassed -or
		$disableKeepAlive -isnot [bool] -or -not $disableKeepAlive -or
		$requiredToolsPresent -isnot [bool] -or -not $requiredToolsPresent -or
		$postOperationPassed -isnot [bool] -or -not $postOperationPassed -or
		[string](Get-RequiredJsonProperty -Object $boundary -Name 'serverName' -Description 'MCP reconnect connection boundary') -cne 'TerraEngine Editor' -or
		[string](Get-RequiredJsonProperty -Object $freshInitialize -Name 'serverName' -Description 'MCP reconnect fresh initialize') -cne 'TerraEngine Editor' -or
		[string](Get-RequiredJsonProperty -Object $postReconnect -Name 'playState' -Description 'MCP reconnect post operations') -cne 'stopped' -or
		$boundarySequence -lt 1 -or $freshSequence -le $boundarySequence -or $toolListSequence -le $freshSequence -or $playStateSequence -le $toolListSequence) {
		throw 'MCP reconnect evidence does not prove an ordered connection-close boundary, fresh initialize, tool list, and stopped-state operation.'
	}
	$recordsStrict = Get-RequiredPreservedJsonMember -Object $strict -Name 'operations' -Description 'MCP operation evidence'
	$records = @(Get-RequiredJsonProperty -Object $evidence -Name 'operations' -Description 'MCP operation evidence')
	$declared = @(@($Scenario.operations) + @($Scenario.captures))
	if ($schemaVersion -eq 2) {
		$strictDeclaredCount = Get-RequiredPreservedJsonMember -Object $strict -Name 'declaredItemCount' -Description 'MCP operation evidence'
		if (($strictDeclaredCount -isnot [int] -and $strictDeclaredCount -isnot [long]) -or
			[int64]$strictDeclaredCount -ne $declared.Count) {
			throw 'SchemaVersion 2 MCP operation evidence declaredItemCount does not match the reviewed scenario.'
		}
	}
	if (-not (Test-IsJsonArray -Value $recordsStrict) -or $records.Count -lt 1) {
		throw 'MCP operation evidence operations must be a non-empty JSON array.'
	}
	if ($records.Count -ne $declared.Count) {
		throw "MCP operation evidence contains $($records.Count) records for $($declared.Count) declared scenario items."
	}
	$declaredExecutionOrder = [System.Collections.Generic.List[object]]::new()
	foreach ($stageName in @('prePlay', 'play', 'postPlay')) {
		$stageItems = @($declared | Where-Object { [string]$_.stage -ceq $stageName } | Sort-Object { [int]$_.order })
		foreach ($stageItem in $stageItems) { $declaredExecutionOrder.Add($stageItem) }
	}
	$recordIds = [System.Collections.Generic.HashSet[string]]::new([System.StringComparer]::Ordinal)
	$validatedBindingRegistry = [System.Collections.Generic.Dictionary[string, object]]::new([System.StringComparer]::Ordinal)
	$validatedApprovalIds = [System.Collections.Generic.HashSet[string]]::new([System.StringComparer]::Ordinal)
	$previousRecordedMcpSequence = 0
	$approvalCount = 0
	for ($recordIndex = 0; $recordIndex -lt $records.Count; ++$recordIndex) {
		$record = $records[$recordIndex]
		$recordStrict = @($recordsStrict)[$recordIndex]
		if ($null -eq $record -or $record -is [string] -or $record -is [System.Array]) {
			throw 'Every MCP operation evidence entry must be a JSON object.'
		}
		if ($recordStrict -isnot [System.Collections.IDictionary]) {
			throw 'Every MCP operation evidence entry must preserve one JSON object.'
		}
		$id = [string](Get-RequiredJsonProperty -Object $record -Name 'id' -Description 'MCP operation evidence entry')
		if ($schemaVersion -eq 2 -and [string]$declaredExecutionOrder[$recordIndex].id -cne $id) {
			throw "SchemaVersion 2 MCP operation evidence is reordered at record $($recordIndex + 1)."
		}
		if (-not $recordIds.Add($id)) { throw "MCP operation evidence repeats scenario item '$id'." }
		$matching = @($declared | Where-Object { [string]$_.id -ceq $id })
		if ($matching.Count -ne 1) { throw "MCP operation evidence contains undeclared scenario item '$id'." }
		$definition = $matching[0]
		$kind = if (@($Scenario.captures | Where-Object { [string]$_.id -ceq $id }).Count -eq 1) { 'capture' } else { 'operation' }
		foreach ($field in @(
			[ordered]@{ name = 'kind'; expected = $kind },
			[ordered]@{ name = 'stage'; expected = [string]$definition.stage },
			[ordered]@{ name = 'tool'; expected = [string]$definition.tool }
		)) {
			if ([string](Get-RequiredJsonProperty -Object $record -Name $field.name -Description "MCP operation evidence '$id'") -cne [string]$field.expected) {
				throw "MCP operation evidence '$id' has an unexpected $($field.name)."
			}
		}
		if ([int](Get-RequiredJsonProperty -Object $record -Name 'order' -Description "MCP operation evidence '$id'") -ne [int]$definition.order) {
			throw "MCP operation evidence '$id' has an unexpected order."
		}
		$mcpSequence = 0
		$transcriptEnvelope = $null
		$expectedArguments = $null
		$expectedBindingResolution = $null
		if ($schemaVersion -eq 2) {
			$strictMcpSequence = Get-RequiredPreservedJsonMember -Object $recordStrict -Name 'mcpSequence' -Description "MCP operation evidence '$id'"
			if (($strictMcpSequence -isnot [int] -and $strictMcpSequence -isnot [long]) -or
				[int64]$strictMcpSequence -le $previousRecordedMcpSequence) {
				throw "MCP operation evidence '$id' has a missing, invalid, or reordered MCP sequence."
			}
			$mcpSequence = [int]$strictMcpSequence
			$previousRecordedMcpSequence = $mcpSequence
			$expectedArguments = Get-ScenarioInvocationArguments -Definition $definition -Kind $kind
			$expectedBindingResolution = if ($kind -eq 'operation') {
				Resolve-ScenarioArgumentBindings -Definition $definition -Arguments $expectedArguments -BindingRegistry $validatedBindingRegistry
			}
			else {
				[ordered]@{ argumentsSha256 = Get-CanonicalJsonSha256 -Value $expectedArguments; bindings = @() }
			}
			$recordedArguments = Get-RequiredJsonProperty -Object $record -Name 'resolvedArguments' -Description "MCP operation evidence '$id'"
			$recordedArgumentsHash = [string](Get-RequiredJsonProperty -Object $record -Name 'resolvedArgumentsSha256' -Description "MCP operation evidence '$id'")
			$recordedBindingsStrict = Get-RequiredPreservedJsonMember -Object $recordStrict -Name 'argumentBindings' -Description "MCP operation evidence '$id'"
			$recordedBindings = @(Get-RequiredJsonProperty -Object $record -Name 'argumentBindings' -Description "MCP operation evidence '$id'")
			if (-not (Test-IsJsonArray -Value $recordedBindingsStrict) -or
				$recordedArgumentsHash -notmatch '^[0-9a-f]{64}$' -or
				$recordedArgumentsHash -cne [string]$expectedBindingResolution.argumentsSha256 -or
				(Get-CanonicalJsonSha256 -Value $recordedArguments) -cne $recordedArgumentsHash -or
				(Get-CanonicalJsonText -Value $recordedArguments) -cne (Get-CanonicalJsonText -Value $expectedArguments) -or
				(Get-CanonicalJsonText -Value $recordedBindings) -cne (Get-CanonicalJsonText -Value @($expectedBindingResolution.bindings))) {
				throw "MCP operation evidence '$id' does not preserve its exact resolved arguments, hash, and binding provenance."
			}

			$matchingTranscriptEntries = @($transcriptState.entries | Where-Object {
				[int](Get-OptionalProperty -Object $_ -Name 'sequence') -eq $mcpSequence -and
				$null -ne (Get-OptionalProperty -Object $_ -Name 'response')
			})
			if ($matchingTranscriptEntries.Count -ne 1) {
				throw "MCP operation evidence '$id' does not identify exactly one successful transcript response."
			}
			$transcriptEntry = $matchingTranscriptEntries[0]
			$transcriptRequest = Get-RequiredJsonProperty -Object $transcriptEntry -Name 'request' -Description "MCP transcript sequence $mcpSequence"
			$transcriptParams = Get-RequiredJsonProperty -Object $transcriptRequest -Name 'params' -Description "MCP transcript sequence $mcpSequence request"
			$transcriptArguments = Get-RequiredJsonProperty -Object $transcriptParams -Name 'arguments' -Description "MCP transcript sequence $mcpSequence request"
			if ([int](Get-RequiredJsonProperty -Object $transcriptRequest -Name 'id' -Description "MCP transcript sequence $mcpSequence request") -ne $mcpSequence -or
				[string](Get-RequiredJsonProperty -Object $transcriptRequest -Name 'method' -Description "MCP transcript sequence $mcpSequence request") -cne 'tools/call' -or
				[string](Get-RequiredJsonProperty -Object $transcriptParams -Name 'name' -Description "MCP transcript sequence $mcpSequence request") -cne [string]$definition.tool -or
				(Get-CanonicalJsonText -Value $transcriptArguments) -cne (Get-CanonicalJsonText -Value $expectedArguments)) {
				throw "MCP transcript sequence $mcpSequence does not preserve scenario item '$id' and its resolved arguments."
			}
			$transcriptResponse = Get-RequiredJsonProperty -Object $transcriptEntry -Name 'response' -Description "MCP transcript sequence $mcpSequence"
			if ([int](Get-RequiredJsonProperty -Object $transcriptResponse -Name 'id' -Description "MCP transcript sequence $mcpSequence response") -ne $mcpSequence) {
				throw "MCP transcript sequence $mcpSequence response has the wrong request identity."
			}
			$transcriptEnvelope = Get-ToolEnvelope -Response $transcriptResponse -Tool ([string]$definition.tool)
			$definitionApproval = Get-OptionalProperty -Object $definition -Name 'approval'
			$recordApprovalStrict = Get-RequiredPreservedJsonMember -Object $recordStrict -Name 'approval' -Description "MCP operation evidence '$id'"
			$recordApproval = Get-RequiredJsonProperty -Object $record -Name 'approval' -Description "MCP operation evidence '$id'"
			if ($null -eq $definitionApproval) {
				if ($null -ne $recordApprovalStrict -or $null -ne $recordApproval) {
					throw "MCP operation evidence '$id' invents approval provenance for an unapproved scenario item."
				}
			}
			else {
				if ($kind -cne 'operation' -or $recordApprovalStrict -isnot [System.Collections.IDictionary] -or $null -eq $recordApproval) {
					throw "MCP operation evidence '$id' omits its required high-impact approval provenance."
				}
				$approvalValidation = Read-AndValidateScenarioApprovalEvidence `
					-Summary $recordApproval `
					-SummaryStrict $recordApprovalStrict `
					-Definition $definition `
					-ExpectedArguments $expectedArguments `
					-InitialEnvelope $transcriptEnvelope `
					-InitialMcpSequence $mcpSequence `
					-TranscriptState $transcriptState `
					-RunRoot $RunRoot `
					-ScenarioPhase ([string]$Scenario.phase) `
					-ApprovalIds $validatedApprovalIds
				$transcriptEnvelope = $approvalValidation.envelope
				$previousRecordedMcpSequence = [int]$approvalValidation.completionMcpSequence
				$approvalCount++
			}
		}
		$expectationPassed = Get-RequiredJsonProperty -Object $record -Name 'expectationPassed' -Description "MCP operation evidence '$id'"
		if ($expectationPassed -isnot [bool] -or -not $expectationPassed) {
			throw "MCP operation evidence '$id' does not carry a typed passing expectation result."
		}
		$expectedRecord = New-ScenarioExpectationRecord -Expectation (Get-OptionalProperty -Object $definition -Name 'expect')
		$recordedExpected = Get-RequiredJsonProperty -Object $record -Name 'expected' -Description "MCP operation evidence '$id'"
		if (($recordedExpected | ConvertTo-Json -Depth 30 -Compress) -cne ($expectedRecord | ConvertTo-Json -Depth 30 -Compress)) {
			throw "MCP operation evidence '$id' does not preserve its reviewed scenario expectation."
		}
		$observed = Get-RequiredJsonProperty -Object $record -Name 'observed' -Description "MCP operation evidence '$id'"
		$observedOk = Get-RequiredJsonProperty -Object $observed -Name 'ok' -Description "MCP operation evidence '$id' observed result"
		if ($observedOk -isnot [bool] -or [bool]$observedOk -ne [bool]$expectedRecord.ok) {
			throw "MCP operation evidence '$id' observed ok result does not match its reviewed expectation."
		}
		$envelopeHash = [string](Get-RequiredJsonProperty -Object $observed -Name 'envelopeSha256' -Description "MCP operation evidence '$id' observed result")
		if ($envelopeHash -notmatch '^[0-9a-f]{64}$') { throw "MCP operation evidence '$id' lacks a valid response-envelope digest." }
		if ($schemaVersion -eq 2 -and
			(Get-StringSha256 -Value ($transcriptEnvelope | ConvertTo-Json -Depth 80 -Compress)) -cne $envelopeHash) {
			throw "MCP operation evidence '$id' response-envelope digest does not match its transcript response."
		}
		if ($null -ne $expectedRecord.status -and [string](Get-OptionalProperty -Object $observed -Name 'status') -cne [string]$expectedRecord.status) {
			throw "MCP operation evidence '$id' observed status does not match its reviewed expectation."
		}
		if ($null -ne $expectedRecord.errorCategory -and [string](Get-OptionalProperty -Object $observed -Name 'errorCategory') -cne [string]$expectedRecord.errorCategory) {
			throw "MCP operation evidence '$id' observed error category does not match its reviewed expectation."
		}
		if ($kind -eq 'operation') {
			$evidenceReference = Get-RequiredJsonProperty -Object $record -Name 'evidence' -Description "MCP operation evidence '$id'"
			$relativeEvidencePath = [string](Get-RequiredJsonProperty -Object $evidenceReference -Name 'path' -Description "MCP operation evidence '$id' artifact")
			if ($relativeEvidencePath.Replace('\', '/') -cne [string]$definition.evidencePath.Replace('\', '/')) {
				throw "MCP operation evidence '$id' points to an unexpected phase artifact."
			}
			$operationEvidencePath = Get-AbsolutePath -Path $relativeEvidencePath -BasePath $RunRoot
			Assert-PathBelow -Parent $RunRoot -Child $operationEvidencePath -Description "MCP operation evidence '$id' artifact"
			if (-not (Test-Path -LiteralPath $operationEvidencePath -PathType Leaf) -or (Get-Item -LiteralPath $operationEvidencePath).Length -eq 0) {
				throw "MCP operation evidence '$id' artifact is missing or empty."
			}
			$artifactHash = [string](Get-RequiredJsonProperty -Object $evidenceReference -Name 'sha256' -Description "MCP operation evidence '$id' artifact")
			if ($artifactHash -notmatch '^[0-9a-f]{64}$' -or (Get-Sha256 -Path $operationEvidencePath) -cne $artifactHash) {
				throw "MCP operation evidence '$id' artifact hash does not match its summary."
			}
			$artifactText = Get-Content -Raw -LiteralPath $operationEvidencePath
			$artifactStrict = ConvertFrom-JsonPreservingArrays -Json $artifactText -Description "MCP operation artifact '$id'"
			$artifact = $artifactText | ConvertFrom-Json
			if ($artifactStrict -isnot [System.Collections.IDictionary] -or
				[int](Get-RequiredJsonProperty -Object $artifact -Name 'schemaVersion' -Description "MCP operation artifact '$id'") -ne $schemaVersion -or
				[string](Get-RequiredJsonProperty -Object $artifact -Name 'phase' -Description "MCP operation artifact '$id'") -cne [string]$Scenario.phase -or
				[string](Get-RequiredJsonProperty -Object $artifact -Name 'id' -Description "MCP operation artifact '$id'") -cne $id -or
				[string](Get-RequiredJsonProperty -Object $artifact -Name 'tool' -Description "MCP operation artifact '$id'") -cne [string]$definition.tool) {
				throw "MCP operation artifact '$id' does not match its reviewed scenario identity."
			}
			$artifactPassed = Get-RequiredJsonProperty -Object $artifact -Name 'expectationPassed' -Description "MCP operation artifact '$id'"
			$artifactEnvelope = Get-RequiredJsonProperty -Object $artifact -Name 'envelope' -Description "MCP operation artifact '$id'"
			$artifactEnvelopeHash = Get-StringSha256 -Value ($artifactEnvelope | ConvertTo-Json -Depth 80 -Compress)
			if ($artifactPassed -isnot [bool] -or -not $artifactPassed -or
				[string](Get-RequiredJsonProperty -Object $artifact -Name 'envelopeSha256' -Description "MCP operation artifact '$id'") -cne $envelopeHash -or
				$artifactEnvelopeHash -cne $envelopeHash) {
				throw "MCP operation artifact '$id' does not preserve its passing response envelope."
			}
			if ($schemaVersion -eq 2) {
				$artifactBindingsStrict = Get-RequiredPreservedJsonMember -Object $artifactStrict -Name 'argumentBindings' -Description "MCP operation artifact '$id'"
				$artifactBindings = @(Get-RequiredJsonProperty -Object $artifact -Name 'argumentBindings' -Description "MCP operation artifact '$id'")
				$artifactResolvedArguments = Get-RequiredJsonProperty -Object $artifact -Name 'resolvedArguments' -Description "MCP operation artifact '$id'"
				$artifactArguments = Get-RequiredJsonProperty -Object $artifact -Name 'arguments' -Description "MCP operation artifact '$id'"
				$artifactArgumentsHash = [string](Get-RequiredJsonProperty -Object $artifact -Name 'resolvedArgumentsSha256' -Description "MCP operation artifact '$id'")
				if (-not (Test-IsJsonArray -Value $artifactBindingsStrict) -or
					[int](Get-RequiredJsonProperty -Object $artifact -Name 'mcpSequence' -Description "MCP operation artifact '$id'") -ne $mcpSequence -or
					$artifactArgumentsHash -cne [string]$expectedBindingResolution.argumentsSha256 -or
					(Get-CanonicalJsonSha256 -Value $artifactResolvedArguments) -cne $artifactArgumentsHash -or
					(Get-CanonicalJsonText -Value $artifactResolvedArguments) -cne (Get-CanonicalJsonText -Value $expectedArguments) -or
					(Get-CanonicalJsonText -Value $artifactArguments) -cne (Get-CanonicalJsonText -Value $expectedArguments) -or
					(Get-CanonicalJsonText -Value $artifactBindings) -cne (Get-CanonicalJsonText -Value @($expectedBindingResolution.bindings))) {
					throw "MCP operation artifact '$id' does not preserve schema-v2 argument binding provenance."
				}
				$artifactApproval = Get-RequiredJsonProperty -Object $artifact -Name 'approval' -Description "MCP operation artifact '$id'"
				$recordApproval = Get-RequiredJsonProperty -Object $record -Name 'approval' -Description "MCP operation evidence '$id'"
				if ((Get-CanonicalJsonText -Value $artifactApproval) -cne (Get-CanonicalJsonText -Value $recordApproval)) {
					throw "MCP operation artifact '$id' does not preserve its exact approval provenance."
				}
				$artifactOk = Get-RequiredJsonProperty -Object $artifactEnvelope -Name 'ok' -Description "MCP operation artifact '$id' envelope"
				if ($artifactOk -is [bool] -and [bool]$artifactOk) {
					$validatedBindingRegistry[$id] = [ordered]@{
						envelope = $artifactEnvelope
						mcpSequence = $mcpSequence
						envelopeSha256 = $envelopeHash
						evidenceSha256 = $artifactHash
						approval = $recordApproval
					}
				}
			}
		}
	}
	foreach ($definition in $declared) {
		if (-not $recordIds.Contains([string]$definition.id)) { throw "MCP operation evidence omits declared scenario item '$($definition.id)'." }
	}
	return [ordered]@{
		schemaVersion = $schemaVersion
		phase = [string]$Scenario.phase
		operationCount = $records.Count
		argumentBindingCount = $scenarioBindingCount
		approvalCount = $approvalCount
		transcriptEvidence = if ($null -eq $transcriptState) { $null } else { [ordered]@{ path = [string]$transcriptState.path; sha256 = [string]$transcriptState.sha256 } }
		reconnectPassed = $true
		reconnectEvidence = [ordered]@{ path = $reconnectRelativePath; sha256 = $reconnectHash }
		sha256 = Get-Sha256 -Path $Path
	}
}

function Get-PngDimensions {
	param([Parameter(Mandatory = $true)][byte[]]$Bytes)
	$signature = [byte[]](0x89, 0x50, 0x4e, 0x47, 0x0d, 0x0a, 0x1a, 0x0a)
	if ($Bytes.Length -lt 24) { throw "Capture PNG is truncated." }
	for ($index = 0; $index -lt $signature.Length; ++$index) {
		if ($Bytes[$index] -ne $signature[$index]) { throw "Capture did not contain a valid PNG signature." }
	}
	$width = ([UInt32]$Bytes[16] -shl 24) -bor ([UInt32]$Bytes[17] -shl 16) -bor ([UInt32]$Bytes[18] -shl 8) -bor [UInt32]$Bytes[19]
	$height = ([UInt32]$Bytes[20] -shl 24) -bor ([UInt32]$Bytes[21] -shl 16) -bor ([UInt32]$Bytes[22] -shl 8) -bor [UInt32]$Bytes[23]
	return [ordered]@{ width = $width; height = $height }
}

function Get-PngVisualStatistics {
	param([Parameter(Mandatory = $true)][byte[]]$Bytes)

	# PNG headers prove only that an image-shaped payload exists. Decode every
	# pixel into a known BGRA layout so uniform, black, transparent, and duplicate
	# readbacks cannot masquerade as visible scene evidence.
	Add-Type -AssemblyName System.Drawing
	$stream = $null
	$source = $null
	$bitmap = $null
	$graphics = $null
	$bitmapData = $null
	try {
		$stream = [System.IO.MemoryStream]::new($Bytes, $false)
		$source = [System.Drawing.Image]::FromStream($stream, $true, $true)
		$bitmap = [System.Drawing.Bitmap]::new($source.Width, $source.Height, [System.Drawing.Imaging.PixelFormat]::Format32bppArgb)
		$graphics = [System.Drawing.Graphics]::FromImage($bitmap)
		$graphics.CompositingMode = [System.Drawing.Drawing2D.CompositingMode]::SourceCopy
		$graphics.DrawImage($source, 0, 0, $source.Width, $source.Height)
		$graphics.Dispose()
		$graphics = $null

		$rectangle = [System.Drawing.Rectangle]::new(0, 0, $bitmap.Width, $bitmap.Height)
		$bitmapData = $bitmap.LockBits($rectangle, [System.Drawing.Imaging.ImageLockMode]::ReadOnly, [System.Drawing.Imaging.PixelFormat]::Format32bppArgb)
		$rowLength = [int]$bitmap.Width * 4
		$canonicalPixels = [byte[]]::new($rowLength * [int]$bitmap.Height)
		$row = [byte[]]::new($rowLength)
		$pixelCount = [UInt64]$bitmap.Width * [UInt64]$bitmap.Height
		$nonBlackRgbPixels = [UInt64]0
		$visibleNonBlackPixels = [UInt64]0
		$transparentPixels = [UInt64]0
		$opaquePixels = [UInt64]0
		$luminanceTotal = [UInt64]0
		$minimumRed = 255; $minimumGreen = 255; $minimumBlue = 255; $minimumAlpha = 255; $minimumLuminance = 255
		$maximumRed = 0; $maximumGreen = 0; $maximumBlue = 0; $maximumAlpha = 0; $maximumLuminance = 0
		$firstRgb = [UInt32]0
		$firstAlpha = 0
		$haveFirstPixel = $false
		$rgbUniform = $true
		$rgbaUniform = $true
		$uniqueRgb = [System.Collections.Generic.HashSet[UInt32]]::new()
		$uniqueRgbCapped = $false
		$uniqueRgbLimit = 4096

		for ($y = 0; $y -lt $bitmap.Height; ++$y) {
			# LockBits may expose either row orientation. Statistics are independent
			# of orientation, while copying each addressed row keeps the digest stable
			# for captures decoded through the same normalized bitmap path.
			$rowPointer = [System.IntPtr]::Add($bitmapData.Scan0, $y * $bitmapData.Stride)
			[System.Runtime.InteropServices.Marshal]::Copy($rowPointer, $row, 0, $rowLength)
			[System.Buffer]::BlockCopy($row, 0, $canonicalPixels, $y * $rowLength, $rowLength)
			for ($offset = 0; $offset -lt $rowLength; $offset += 4) {
				$blue = [int]$row[$offset]
				$green = [int]$row[$offset + 1]
				$red = [int]$row[$offset + 2]
				$alpha = [int]$row[$offset + 3]
				$rgb = [UInt32](($red -shl 16) -bor ($green -shl 8) -bor $blue)
				$luminance = [int](($red * 2126 + $green * 7152 + $blue * 722) / 10000)

				if (-not $haveFirstPixel) {
					$firstRgb = $rgb
					$firstAlpha = $alpha
					$haveFirstPixel = $true
				}
				elseif ($rgb -ne $firstRgb) {
					$rgbUniform = $false
					$rgbaUniform = $false
				}
				elseif ($alpha -ne $firstAlpha) {
					$rgbaUniform = $false
				}

				if ($rgb -ne 0) {
					$nonBlackRgbPixels++
					if ($alpha -gt 0) { $visibleNonBlackPixels++ }
				}
				if ($alpha -eq 0) { $transparentPixels++ }
				if ($alpha -eq 255) { $opaquePixels++ }
				$luminanceTotal += [UInt64]$luminance
				if ($red -lt $minimumRed) { $minimumRed = $red }; if ($red -gt $maximumRed) { $maximumRed = $red }
				if ($green -lt $minimumGreen) { $minimumGreen = $green }; if ($green -gt $maximumGreen) { $maximumGreen = $green }
				if ($blue -lt $minimumBlue) { $minimumBlue = $blue }; if ($blue -gt $maximumBlue) { $maximumBlue = $blue }
				if ($alpha -lt $minimumAlpha) { $minimumAlpha = $alpha }; if ($alpha -gt $maximumAlpha) { $maximumAlpha = $alpha }
				if ($luminance -lt $minimumLuminance) { $minimumLuminance = $luminance }; if ($luminance -gt $maximumLuminance) { $maximumLuminance = $luminance }
				if (-not $uniqueRgbCapped -and $uniqueRgb.Add($rgb) -and $uniqueRgb.Count -gt $uniqueRgbLimit) { $uniqueRgbCapped = $true }
			}
		}

		$hashAlgorithm = [System.Security.Cryptography.SHA256]::Create()
		try {
			$pixelSha256 = ([System.BitConverter]::ToString($hashAlgorithm.ComputeHash($canonicalPixels))).Replace('-', '').ToLowerInvariant()
		}
		finally {
			$hashAlgorithm.Dispose()
		}
		return [ordered]@{
			pixelCount = $pixelCount
			nonBlackRgbPixelCount = $nonBlackRgbPixels
			visibleNonBlackPixelCount = $visibleNonBlackPixels
			transparentPixelCount = $transparentPixels
			opaquePixelCount = $opaquePixels
			isSolidBlack = $maximumRed -eq 0 -and $maximumGreen -eq 0 -and $maximumBlue -eq 0
			isRgbUniform = $rgbUniform
			isRgbaUniform = $rgbaUniform
			uniqueRgbColorCount = if ($uniqueRgbCapped) { $uniqueRgbLimit + 1 } else { $uniqueRgb.Count }
			uniqueRgbColorCountIsLowerBound = $uniqueRgbCapped
			minimumRgba = @($minimumRed, $minimumGreen, $minimumBlue, $minimumAlpha)
			maximumRgba = @($maximumRed, $maximumGreen, $maximumBlue, $maximumAlpha)
			minimumLuminance = $minimumLuminance
			maximumLuminance = $maximumLuminance
			meanLuminance = [Math]::Round(([double]$luminanceTotal / [double]$pixelCount), 3)
			pixelSha256 = $pixelSha256
		}
	}
	catch {
		throw "Capture PNG pixel decoding failed: $($_.Exception.Message)"
	}
	finally {
		if ($null -ne $bitmapData -and $null -ne $bitmap) { $bitmap.UnlockBits($bitmapData) }
		if ($null -ne $graphics) { $graphics.Dispose() }
		if ($null -ne $bitmap) { $bitmap.Dispose() }
		if ($null -ne $source) { $source.Dispose() }
		if ($null -ne $stream) { $stream.Dispose() }
	}
}

function Get-CaptureVisualAssessment {
	param(
		[Parameter(Mandatory = $true)]$Capture,
		[Parameter(Mandatory = $true)]$Statistics
	)
	$policy = [string]$Capture.visualExpectation.contentPolicy
	$violations = [System.Collections.Generic.List[string]]::new()
	if ($policy -eq 'visible-geometry-required') {
		# Pixel diversity is only a fail-closed sanity check. It can reject vacuous
		# evidence, but it cannot identify a mesh or prove the feature-specific visual
		# claim; the manual checklist and semantic runtime diagnostics remain required.
		if ([bool]$Statistics.isSolidBlack) { $violations.Add('solid-black-frame') }
		if ([bool]$Statistics.isRgbUniform) { $violations.Add('uniform-rgb-frame') }
		if ([UInt64]$Statistics.visibleNonBlackPixelCount -eq 0) { $violations.Add('no-visible-nonblack-pixels') }
	}
	elseif ($policy -ne 'empty-scene-allowed') {
		$violations.Add('unknown-content-policy')
	}
	return [ordered]@{
		contentPolicy = $policy
		automatedCheck = if ($policy -eq 'visible-geometry-required') { 'non-vacuous-pixel-content-sanity' } else { 'decode-and-statistics-only' }
		passed = $violations.Count -eq 0
		violations = @($violations)
		featureCorrectnessInferred = $false
		limitation = 'Pixel statistics reject vacuous evidence but do not, by themselves, prove visible geometry identity or feature correctness.'
	}
}

function Save-CameraCapture {
	param(
		[Parameter(Mandatory = $true)]$Capture,
		[Parameter(Mandatory = $true)]$ToolResult,
		[Parameter(Mandatory = $true)][string]$CameraRoot,
		[Parameter(Mandatory = $true)][string]$RunRoot
	)
	$envelope = $ToolResult.envelope
	if (-not [bool]$envelope.ok) { throw "Required camera capture '$($Capture.id)' failed." }
	if ([int]$envelope.data.width -ne $script:ReferenceWidth -or [int]$envelope.data.height -ne $script:ReferenceHeight) {
		throw "Capture '$($Capture.id)' did not preserve the frozen 1920x1080 resolution."
	}
	if (-not [bool]$envelope.data.backendReadback -or [bool]$envelope.data.deterministicFallback) {
		throw "Capture '$($Capture.id)' is not real installed-editor Vulkan readback evidence."
	}
	$imageItems = @($ToolResult.response.result.content | Where-Object { $_.type -eq 'image' })
	if ($imageItems.Count -ne 1 -or [string]$imageItems[0].mimeType -ne 'image/png' -or [string]::IsNullOrWhiteSpace([string]$imageItems[0].data)) {
		throw "Capture '$($Capture.id)' omitted its required image/png MCP content."
	}
	try { $bytes = [Convert]::FromBase64String([string]$imageItems[0].data) } catch { throw "Capture '$($Capture.id)' contained invalid base64 image data." }
	$dimensions = Get-PngDimensions -Bytes $bytes
	if ($dimensions.width -ne $script:ReferenceWidth -or $dimensions.height -ne $script:ReferenceHeight) {
		throw "Capture '$($Capture.id)' PNG dimensions are not 1920x1080."
	}
	$path = Join-Path $CameraRoot "$($Capture.id).png"
	[System.IO.File]::WriteAllBytes($path, $bytes)
	$statistics = Get-PngVisualStatistics -Bytes $bytes
	$assessment = Get-CaptureVisualAssessment -Capture $Capture -Statistics $statistics
	$visualEvidencePath = Join-Path $CameraRoot "$($Capture.id).visual.json"
	# Write the decoded statistics before enforcing the policy so a failed live
	# gate still leaves actionable evidence instead of only an error string.
	Write-JsonFile -Path $visualEvidencePath -Value ([ordered]@{
		schemaVersion = 1
		captureId = [string]$Capture.id
		expectation = $Capture.visualExpectation
		statistics = $statistics
		assessment = $assessment
	})
	if (-not [bool]$assessment.passed) {
		throw "Capture '$($Capture.id)' failed its '$($assessment.contentPolicy)' visual-content contract: $($assessment.violations -join ', ')."
	}
	return [ordered]@{
		id = [string]$Capture.id
		view = [string]$Capture.view
		stage = [string]$Capture.stage
		tool = [string]$Capture.tool
		path = Get-RelativeForwardPath -Parent $RunRoot -Child $path
		bytes = [UInt64]$bytes.Length
		sha256 = Get-Sha256 -Path $path
		width = [int]$dimensions.width
		height = [int]$dimensions.height
		frameId = Get-OptionalProperty -Object $envelope.data -Name 'frameId'
		sourceKind = [string]$envelope.data.sourceKind
		backendReadback = [bool]$envelope.data.backendReadback
		deterministicFallback = [bool]$envelope.data.deterministicFallback
		visualExpectation = $Capture.visualExpectation
		visualStatistics = $statistics
		visualAssessment = $assessment
		visualEvidence = Get-RelativeForwardPath -Parent $RunRoot -Child $visualEvidencePath
	}
}

function Read-ScenarioManifest {
	param(
		[Parameter(Mandatory = $true)][string]$Path,
		[Parameter(Mandatory = $true)][string]$ExpectedPhase,
		[string]$RepoRoot = ''
	)
	if ([string]::IsNullOrWhiteSpace($RepoRoot)) {
		$repoRootVariable = Get-Variable -Name repoRoot -Scope Script -ErrorAction SilentlyContinue
		if ($null -eq $repoRootVariable -or [string]::IsNullOrWhiteSpace([string]$repoRootVariable.Value)) {
			throw 'Read-ScenarioManifest requires an explicit repository root when no script-scoped repoRoot is available.'
		}
		$RepoRoot = [string]$repoRootVariable.Value
	}
	if (-not (Test-Path -LiteralPath $Path -PathType Leaf)) { throw "Phase scenario manifest was not found: '$Path'." }
	$scenarioText = Get-Content -Raw -LiteralPath $Path
	$scenarioStrict = ConvertFrom-JsonPreservingArrays -Json $scenarioText -Description 'Phase scenario manifest'
	$scenario = $scenarioText | ConvertFrom-Json
	if ([int]$scenario.schemaVersion -ne 2 -or [string]$scenario.phase -ne $ExpectedPhase) {
		throw "Scenario schema/phase does not match '$ExpectedPhase'."
	}
	$fixtureOverlays = [System.Collections.Generic.List[object]]::new()
	$overlayDestinations = [System.Collections.Generic.List[string]]::new()
	$totalOverlayFiles = 0
	$totalOverlayBytes = [UInt64]0
	$hasFixtureOverlays = $scenarioStrict -is [System.Collections.IDictionary] -and
		@($scenarioStrict.Keys) -ccontains 'fixtureOverlays'
	if ($hasFixtureOverlays) {
		$fixtureOverlaysStrict = Get-RequiredPreservedJsonMember -Object $scenarioStrict -Name 'fixtureOverlays' -Description 'Phase scenario manifest'
		if (-not (Test-IsJsonArray -Value $fixtureOverlaysStrict)) {
			throw 'Scenario fixtureOverlays must be a JSON array.'
		}
		if (@($fixtureOverlaysStrict).Count -gt $script:MaximumFixtureOverlayCount) {
			throw "Scenario fixtureOverlays may contain at most $($script:MaximumFixtureOverlayCount) entries."
		}
		$fixtureOverlaysTyped = @(Get-OptionalProperty -Object $scenario -Name 'fixtureOverlays')
		if ($fixtureOverlaysTyped.Count -ne @($fixtureOverlaysStrict).Count) {
			throw 'Scenario fixtureOverlays could not be decoded without changing its array shape.'
		}
		for ($overlayIndex = 0; $overlayIndex -lt $fixtureOverlaysTyped.Count; ++$overlayIndex) {
			$strictOverlay = @($fixtureOverlaysStrict)[$overlayIndex]
			$typedOverlay = $fixtureOverlaysTyped[$overlayIndex]
			if ($strictOverlay -isnot [System.Collections.IDictionary] -or
				$null -eq $typedOverlay -or
				$typedOverlay -is [string] -or
				$typedOverlay -is [System.Array]) {
				throw "Scenario fixture overlay $($overlayIndex + 1) must be one JSON object."
			}
			$overlayKeys = @($strictOverlay.Keys | ForEach-Object { [string]$_ })
			if ($overlayKeys.Count -ne 2 -or
				$overlayKeys -cnotcontains 'source' -or
				$overlayKeys -cnotcontains 'destination') {
				throw "Scenario fixture overlay $($overlayIndex + 1) must contain exactly 'source' and 'destination'."
			}
			$strictSource = Get-RequiredPreservedJsonMember -Object $strictOverlay -Name 'source' -Description "Scenario fixture overlay $($overlayIndex + 1)"
			$strictDestination = Get-RequiredPreservedJsonMember -Object $strictOverlay -Name 'destination' -Description "Scenario fixture overlay $($overlayIndex + 1)"
			if ($strictSource -isnot [string] -or $strictDestination -isnot [string]) {
				throw "Scenario fixture overlay $($overlayIndex + 1) source and destination must be JSON strings."
			}
			$source = Assert-FixtureOverlayRelativePath -Value ([string]$strictSource) -Kind source
			$destination = Assert-FixtureOverlayRelativePath -Value ([string]$strictDestination) -Kind destination
			foreach ($existingDestination in $overlayDestinations) {
				if (Test-FixtureOverlayPathOverlap -Left $destination -Right $existingDestination) {
					throw "Scenario fixture overlay destination '$destination' conflicts with '$existingDestination'."
				}
			}
			$sourcePath = Get-AbsolutePath -Path $source -BasePath $RepoRoot
			Assert-PathBelow -Parent (Join-Path $RepoRoot 'Tests/Fixtures/Animation') -Child $sourcePath -Description 'Fixture overlay source'
			$remainingFiles = $script:MaximumFixtureOverlayFiles - $totalOverlayFiles
			$remainingBytes = $script:MaximumFixtureOverlayBytes - $totalOverlayBytes
			$sourceState = Get-FixtureOverlaySourceState -SourcePath $sourcePath -MaximumFiles $remainingFiles -MaximumBytes $remainingBytes
			$totalOverlayFiles += [int]$sourceState.files
			$totalOverlayBytes += [UInt64]$sourceState.bytes
			# Internal validation fields lock the source between manifest parsing
			# and the later post-fixture copy without expanding the JSON contract.
			$typedOverlay | Add-Member -NotePropertyName validatedSourceSha256 -NotePropertyValue ([string]$sourceState.inventorySha256) -Force
			$typedOverlay | Add-Member -NotePropertyName validatedSourceFiles -NotePropertyValue ([int]$sourceState.files) -Force
			$typedOverlay | Add-Member -NotePropertyName validatedSourceBytes -NotePropertyValue ([UInt64]$sourceState.bytes) -Force
			$typedOverlay | Add-Member -NotePropertyName validatedSourceKind -NotePropertyValue ([string]$sourceState.kind) -Force
			$overlayDestinations.Add($destination)
			$fixtureOverlays.Add($typedOverlay)
		}
	}
	# Every caller can consume fixtureOverlays as an array, including scenarios
	# created before the optional overlay contract existed.
	$scenario | Add-Member -NotePropertyName fixtureOverlays -NotePropertyValue @($fixtureOverlays) -Force
	$ids = [System.Collections.Generic.HashSet[string]]::new([System.StringComparer]::Ordinal)
	$orders = [System.Collections.Generic.HashSet[int]]::new()
	foreach ($item in @(@($scenario.operations) + @($scenario.captures))) {
		$id = [string]$item.id
		if ($id -notmatch '^[A-Za-z0-9][A-Za-z0-9._-]{0,95}$' -or -not $ids.Add($id)) { throw "Scenario operation/capture IDs must be unique path-safe names." }
		if ([string]$item.stage -notin @('prePlay', 'play', 'postPlay')) { throw "Scenario item '$id' has an invalid stage." }
		if ($null -eq (Get-OptionalProperty -Object $item -Name 'order') -or -not $orders.Add([int]$item.order)) { throw "Scenario item '$id' must have a unique integer order." }
	}
	Initialize-ScenarioOperationApprovals -Scenario $scenario -ScenarioStrict $scenarioStrict
	Initialize-ScenarioArgumentBindings -Scenario $scenario -ScenarioStrict $scenarioStrict
	Initialize-ScenarioPlayModeRecordingOutputs -Scenario $scenario -ScenarioStrict $scenarioStrict
	$captures = @($scenario.captures)
	foreach ($view in @('SceneView', 'GameView', 'InGameCamera')) {
		if (@($captures | Where-Object { [string]$_.view -eq $view }).Count -lt 1) { throw "Scenario is missing required $view camera evidence." }
	}
	foreach ($capture in $captures) {
		$arguments = Get-OptionalProperty -Object $capture -Name 'arguments'
		$width = Get-OptionalProperty -Object $arguments -Name 'width'
		$height = Get-OptionalProperty -Object $arguments -Name 'height'
		if (($null -ne $width -and [int]$width -ne $script:ReferenceWidth) -or ($null -ne $height -and [int]$height -ne $script:ReferenceHeight)) {
			throw "Camera capture '$($capture.id)' attempts to weaken the frozen 1920x1080 setting."
		}
		if ([string]$capture.view -eq 'SceneView' -and [string]$capture.tool -ne 'camera.capture_custom') { throw "SceneView evidence must use the semantic custom camera capture." }
		if ([string]$capture.view -eq 'GameView' -and [string]$capture.tool -ne 'game_view.capture') { throw "GameView evidence must use game_view.capture." }
		if ([string]$capture.view -eq 'InGameCamera' -and [string]$capture.tool -ne 'camera.capture_object') { throw "InGameCamera evidence must use an authored camera object." }
	}
	$expectations = @($scenario.visualExpectations)
	if ($expectations.Count -ne $captures.Count) { throw 'Every camera capture must have exactly one explicit visual expectation.' }
	$expectationIds = [System.Collections.Generic.HashSet[string]]::new([System.StringComparer]::Ordinal)
	foreach ($expectation in $expectations) {
		$captureId = [string]$expectation.captureId
		if (-not $expectationIds.Add($captureId)) { throw "Visual expectation '$captureId' is duplicated." }
		$capture = @($captures | Where-Object { [string]$_.id -eq $captureId })
		if ($capture.Count -ne 1) { throw "Visual expectation '$captureId' does not identify exactly one capture." }
		$policy = [string]$expectation.contentPolicy
		if ($policy -notin @('visible-geometry-required', 'empty-scene-allowed')) { throw "Capture '$captureId' has an unsupported visual content policy." }
		$capture[0] | Add-Member -NotePropertyName visualExpectation -NotePropertyValue $expectation -Force
	}
	foreach ($expectation in $expectations) {
		$captureId = [string]$expectation.captureId
		$mustDifferProperty = Get-OptionalProperty -Object $expectation -Name 'mustDifferFrom'
		$mustDifferFrom = @(if ($null -ne $mustDifferProperty) { $mustDifferProperty | ForEach-Object { [string]$_ } })
		if (@($mustDifferFrom | Select-Object -Unique).Count -ne $mustDifferFrom.Count) { throw "Capture '$captureId' repeats a mustDifferFrom target." }
		foreach ($peerId in $mustDifferFrom) {
			if ($peerId -eq $captureId -or $expectationIds -notcontains $peerId) { throw "Capture '$captureId' has an invalid mustDifferFrom target '$peerId'." }
			$peer = @($expectations | Where-Object { [string]$_.captureId -eq $peerId })[0]
			$peerProperty = Get-OptionalProperty -Object $peer -Name 'mustDifferFrom'
			$peerTargets = @(if ($null -ne $peerProperty) { $peerProperty | ForEach-Object { [string]$_ } })
			if ($peerTargets -notcontains $captureId) { throw "mustDifferFrom must be symmetric between '$captureId' and '$peerId'." }
		}
	}
	$identityPairs = [System.Collections.Generic.HashSet[string]]::new([System.StringComparer]::Ordinal)
	foreach ($exception in @($scenario.visualIdentityExceptions)) {
		$captureIds = @($exception.captureIds | ForEach-Object { [string]$_ })
		$reason = [string]$exception.reason
		if ($captureIds.Count -ne 2 -or $captureIds[0] -eq $captureIds[1] -or $expectationIds -notcontains $captureIds[0] -or $expectationIds -notcontains $captureIds[1]) {
			throw 'Each visual identity exception must identify exactly two different known captures.'
		}
		if ($reason.Trim().Length -lt 40) { throw "Visual identity exception '$($captureIds -join ', ')' requires a specific defensive reason of at least 40 characters." }
		$leftCapture = @($captures | Where-Object { [string]$_.id -eq $captureIds[0] })[0]
		$rightCapture = @($captures | Where-Object { [string]$_.id -eq $captureIds[1] })[0]
		if ([string]$leftCapture.view -eq [string]$rightCapture.view) { throw 'Visual identity exceptions are only valid between semantically distinct view types.' }
		$leftExpectation = @($expectations | Where-Object { [string]$_.captureId -eq $captureIds[0] })[0]
		$rightExpectation = @($expectations | Where-Object { [string]$_.captureId -eq $captureIds[1] })[0]
		$leftMustDifferProperty = Get-OptionalProperty -Object $leftExpectation -Name 'mustDifferFrom'
		$rightMustDifferProperty = Get-OptionalProperty -Object $rightExpectation -Name 'mustDifferFrom'
		$leftMustDifferTargets = @(if ($null -ne $leftMustDifferProperty) { $leftMustDifferProperty | ForEach-Object { [string]$_ } })
		$rightMustDifferTargets = @(if ($null -ne $rightMustDifferProperty) { $rightMustDifferProperty | ForEach-Object { [string]$_ } })
		if ($leftMustDifferTargets -contains $captureIds[1] -or $rightMustDifferTargets -contains $captureIds[0]) {
			throw 'A visual identity exception may not weaken a mustDifferFrom requirement.'
		}
		$pairKey = @($captureIds | Sort-Object) -join "`n"
		if (-not $identityPairs.Add($pairKey)) { throw "Visual identity exception '$($captureIds -join ', ')' is duplicated." }
	}
	$requiredEvidenceStrict = Get-RequiredPreservedJsonMember -Object $scenarioStrict -Name 'requiredEvidence' -Description 'Phase scenario manifest'
	$requiredEvidence = Get-OptionalProperty -Object $scenario -Name 'requiredEvidence'
	if (-not (Test-IsJsonArray -Value $requiredEvidenceStrict) -or @($requiredEvidenceStrict).Count -lt 1) {
		throw 'Scenario requiredEvidence must be a non-empty JSON array of explicit run-root-relative files.'
	}
	$requiredEvidencePaths = [System.Collections.Generic.HashSet[string]]::new([System.StringComparer]::OrdinalIgnoreCase)
	foreach ($requiredPathValue in @($requiredEvidenceStrict)) {
		if ($requiredPathValue -isnot [string]) { throw 'Scenario requiredEvidence entries must be JSON strings.' }
		$requiredPath = [string]$requiredPathValue
		if ([string]::IsNullOrWhiteSpace($requiredPath) -or
			[System.IO.Path]::IsPathRooted($requiredPath) -or
			$requiredPath -match '[:*?]' -or
			@($requiredPath -split '[\\/]' | Where-Object { $_ -in @('', '.', '..') }).Count -gt 0) {
			throw "Scenario requiredEvidence path '$requiredPath' must be a safe relative file path without traversal or wildcards."
		}
		if (-not $requiredEvidencePaths.Add($requiredPath.Replace('\', '/'))) { throw "Scenario requiredEvidence path '$requiredPath' is duplicated." }
	}
	$operationEvidencePaths = [System.Collections.Generic.HashSet[string]]::new([System.StringComparer]::OrdinalIgnoreCase)
	foreach ($operation in @($scenario.operations)) {
		$operationId = [string]$operation.id
		$operationEvidencePath = [string](Get-RequiredJsonProperty -Object $operation -Name 'evidencePath' -Description "Scenario operation '$operationId'")
		$normalizedOperationEvidencePath = $operationEvidencePath.Replace('\', '/')
		if ([string]::IsNullOrWhiteSpace($operationEvidencePath) -or
			[System.IO.Path]::IsPathRooted($operationEvidencePath) -or
			$operationEvidencePath -match '[:*?]' -or
			@($operationEvidencePath -split '[\\/]' | Where-Object { $_ -in @('', '.', '..') }).Count -gt 0 -or
			[System.IO.Path]::GetExtension($operationEvidencePath) -cne '.json') {
			throw "Scenario operation '$operationId' evidencePath must be a safe run-root-relative JSON file."
		}
		if (-not $operationEvidencePaths.Add($normalizedOperationEvidencePath)) {
			throw "Scenario operation evidencePath '$operationEvidencePath' is duplicated."
		}
		if (-not $requiredEvidencePaths.Contains($normalizedOperationEvidencePath)) {
			throw "Scenario operation '$operationId' evidencePath must also appear in requiredEvidence."
		}
	}
	$manualStrict = Get-RequiredPreservedJsonMember -Object $scenarioStrict -Name 'manualChecklist' -Description 'Phase scenario manifest'
	$manual = @($scenario.manualChecklist)
	if (-not (Test-IsJsonArray -Value $manualStrict) -or $manual.Count -lt 1) { throw "Scenario must declare the real mouse/keyboard checklist as a non-empty JSON array." }
	$manualIds = [System.Collections.Generic.HashSet[string]]::new([System.StringComparer]::Ordinal)
	for ($manualIndex = 0; $manualIndex -lt $manual.Count; ++$manualIndex) {
		$strictItem = @($manualStrict)[$manualIndex]
		$item = $manual[$manualIndex]
		if ($strictItem -isnot [System.Collections.IDictionary] -or $null -eq $item -or $item -is [string] -or $item -is [System.Array]) {
			throw "Manual checklist item $($manualIndex + 1) must be one JSON object."
		}
		$id = [string](Get-RequiredJsonProperty -Object $item -Name 'id' -Description "Manual checklist item $($manualIndex + 1)")
		if ($id -notmatch '^[A-Za-z0-9][A-Za-z0-9._-]{0,95}$' -or -not $manualIds.Add($id)) {
			throw 'Manual checklist IDs must be unique path-safe names.'
		}
		$input = [string](Get-RequiredJsonProperty -Object $item -Name 'input' -Description "Manual checklist item '$id'")
		if ($input -cnotin @('mouse', 'keyboard')) { throw "Manual checklist item '$id' input must be exactly 'mouse' or 'keyboard'." }
		$required = Get-RequiredPreservedJsonMember -Object $strictItem -Name 'required' -Description "Manual checklist item '$id'"
		if ($required -isnot [bool] -or -not $required) { throw "Manual checklist item '$id' required must be the JSON Boolean true." }
		$instruction = [string](Get-RequiredJsonProperty -Object $item -Name 'instruction' -Description "Manual checklist item '$id'")
		if ([string]::IsNullOrWhiteSpace($instruction)) { throw "Manual checklist item '$id' requires a non-empty instruction." }
		$item | Add-Member -NotePropertyName actionSequence -NotePropertyValue ($manualIndex + 1) -Force
		# Existing scenario prose sometimes groups several clicks/edits into one
		# checklist item. Such prose may never be satisfied by one blanket action
		# claim: a bounded ordered request list must contain at least two atomic
		# events whenever the instruction has an obvious sequence/conjunction.
		$declaredMinimumProperty = $item.PSObject.Properties['minimumInputEvents']
		if ($null -ne $declaredMinimumProperty) {
			$strictMinimum = Get-RequiredPreservedJsonMember -Object $strictItem -Name 'minimumInputEvents' -Description "Manual checklist item '$id'"
			if (($strictMinimum -isnot [int] -and $strictMinimum -isnot [long]) -or [int64]$strictMinimum -lt 1 -or [int64]$strictMinimum -gt 128) {
				throw "Manual checklist item '$id' minimumInputEvents must be an integer from 1 through 128."
			}
			$minimumInputEvents = [int]$strictMinimum
		}
		else {
			$looksMultiAction = $instruction -match '(?i)(,\s|\bthen\b|\bfollowed by\b|\band\b)'
			$minimumInputEvents = if ($looksMultiAction) { 2 } else { 1 }
		}
		$item | Add-Member -NotePropertyName minimumInputEvents -NotePropertyValue $minimumInputEvents -Force
	}
	$inputs = @($manual | ForEach-Object { [string]$_.input })
	if ($inputs -cnotcontains 'mouse' -or $inputs -cnotcontains 'keyboard') {
		throw "Manual checklist must retain both mouse and keyboard interaction proof."
	}
	return $scenario
}

function Get-CameraEvidenceValidation {
	param(
		[Parameter(Mandatory = $true)]$Scenario,
		[Parameter(Mandatory = $true)]$CameraInventory
	)
	$comparisons = [System.Collections.Generic.List[object]]::new()
	$failures = [System.Collections.Generic.List[string]]::new()
	$captures = @($CameraInventory)
	for ($leftIndex = 0; $leftIndex -lt $captures.Count; ++$leftIndex) {
		for ($rightIndex = $leftIndex + 1; $rightIndex -lt $captures.Count; ++$rightIndex) {
			$left = $captures[$leftIndex]
			$right = $captures[$rightIndex]
			$semanticallyDistinctViews = [string]$left.view -cne [string]$right.view
			$leftMustDifferProperty = Get-OptionalProperty -Object $left.visualExpectation -Name 'mustDifferFrom'
			$rightMustDifferProperty = Get-OptionalProperty -Object $right.visualExpectation -Name 'mustDifferFrom'
			$leftMustDifferTargets = @(if ($null -ne $leftMustDifferProperty) { $leftMustDifferProperty | ForEach-Object { [string]$_ } })
			$rightMustDifferTargets = @(if ($null -ne $rightMustDifferProperty) { $rightMustDifferProperty | ForEach-Object { [string]$_ } })
			$leftMustDiffer = $leftMustDifferTargets -contains [string]$right.id
			$rightMustDiffer = $rightMustDifferTargets -contains [string]$left.id
			$mustDiffer = $semanticallyDistinctViews -or $leftMustDiffer -or $rightMustDiffer
			if (-not $mustDiffer) { continue }

			$byteIdentical = [string]$left.sha256 -ceq [string]$right.sha256
			$pixelIdentical = [string]$left.visualStatistics.pixelSha256 -ceq [string]$right.visualStatistics.pixelSha256
			$exception = $null
			foreach ($candidate in @($Scenario.visualIdentityExceptions)) {
				$candidateIds = @($candidate.captureIds | ForEach-Object { [string]$_ })
				if ($candidateIds -contains [string]$left.id -and $candidateIds -contains [string]$right.id) { $exception = $candidate; break }
			}
			$exceptionAllowed = $semanticallyDistinctViews -and $null -ne $exception -and -not $leftMustDiffer -and -not $rightMustDiffer
			$passed = -not ($byteIdentical -or $pixelIdentical) -or $exceptionAllowed
			$record = [ordered]@{
				leftCaptureId = [string]$left.id
				rightCaptureId = [string]$right.id
				semanticallyDistinctViews = $semanticallyDistinctViews
				declaredMustDiffer = $leftMustDiffer -or $rightMustDiffer
				byteIdentical = $byteIdentical
				pixelIdentical = $pixelIdentical
				exceptionAllowed = $exceptionAllowed
				exceptionReason = if ($exceptionAllowed) { [string]$exception.reason } else { '' }
				passed = $passed
			}
			$comparisons.Add($record)
			if (-not $passed) { $failures.Add("'$($left.id)' and '$($right.id)' contain identical required visual evidence") }
		}
	}
	if ($failures.Count -gt 0) { throw "Camera evidence set failed distinct-view validation: $($failures -join '; ')." }
	return [ordered]@{
		passed = $true
		comparisonCount = $comparisons.Count
		comparisons = @($comparisons)
		featureCorrectnessInferredFromPixels = $false
		limitation = 'Content and identity checks reject vacuous evidence; semantic feature correctness still requires the scenario operations and manual evidence.'
	}
}

function Wait-ForEditorMainWindow {
	param(
		[Parameter(Mandatory = $true)]$EditorProcess,
		[Parameter(Mandatory = $true)][int]$TimeoutSeconds
	)
	$deadline = (Get-Date).AddSeconds($TimeoutSeconds)
	while ((Get-Date) -lt $deadline) {
		$EditorProcess.Refresh()
		if ($EditorProcess.HasExited) { throw 'Installed editor exited before its native main window became available.' }
		if ($EditorProcess.MainWindowHandle -ne [IntPtr]::Zero -and -not [string]::IsNullOrWhiteSpace([string]$EditorProcess.MainWindowTitle)) {
			return [ordered]@{
				processId = [int]$EditorProcess.Id
				windowHandle = ('0x{0:x}' -f [UInt64]$EditorProcess.MainWindowHandle.ToInt64())
				windowTitle = [string]$EditorProcess.MainWindowTitle
			}
		}
		Start-Sleep -Milliseconds 250
	}
	throw "Installed editor did not expose a native main window within $TimeoutSeconds seconds."
}

function Read-ManualActionRecords {
	param(
		[Parameter(Mandatory = $true)]$Scenario,
		[Parameter(Mandatory = $true)][string]$ClickPath,
		[switch]$RequireComplete
	)
	if (-not (Test-Path -LiteralPath $ClickPath -PathType Leaf)) {
		if ($RequireComplete) { throw 'Required real-click JSONL transcript is missing.' }
		return @()
	}
	$declaredItems = @($Scenario.manualChecklist)
	$records = [System.Collections.Generic.List[object]]::new()
	$lineNumber = 0
	foreach ($line in @(Get-Content -LiteralPath $ClickPath)) {
		$lineNumber++
		if ([string]::IsNullOrWhiteSpace($line)) { continue }
		if ($records.Count -ge $declaredItems.Count) { throw 'Click transcript contains more actions than the declared manual checklist.' }
		$recordStrict = ConvertFrom-JsonPreservingArrays -Json $line -Description "Click transcript line $lineNumber"
		try { $record = $line | ConvertFrom-Json } catch { throw "Click transcript line $lineNumber is not valid JSON." }
		if ($recordStrict -isnot [System.Collections.IDictionary] -or $null -eq $record -or $record -is [System.Array]) { throw "Click transcript line $lineNumber must contain one JSON object." }
		$declared = $declaredItems[$records.Count]
		$id = [string](Get-RequiredJsonProperty -Object $record -Name 'id' -Description "Click transcript line $lineNumber")
		if ($id -cne [string]$declared.id) { throw "Click transcript action $lineNumber must be '$($declared.id)' so checklist order remains exact." }
		$input = [string](Get-RequiredJsonProperty -Object $record -Name 'input' -Description "Click record '$id'")
		if ($input -cne [string]$declared.input) { throw "Click record '$id' input must exactly match scenario input '$($declared.input)'." }
		$inputDelivery = [string](Get-RequiredJsonProperty -Object $record -Name 'inputDelivery' -Description "Click record '$id'")
		if ($inputDelivery -cnotin @('human', 'Win32SendInput')) { throw "Click record '$id' inputDelivery must be 'human' or 'Win32SendInput'." }
		$passed = Get-RequiredPreservedJsonMember -Object $recordStrict -Name 'passed' -Description "Click record '$id'"
		if ($passed -isnot [bool] -or -not $passed) { throw "Click record '$id' property 'passed' must be the JSON Boolean true." }
		$observed = [string](Get-RequiredJsonProperty -Object $record -Name 'observed' -Description "Click record '$id'")
		if ([string]::IsNullOrWhiteSpace($observed)) { throw "Click record '$id' requires a non-empty observed result." }
		if ($null -ne $record.PSObject.Properties['evidence']) { throw "Click record '$id' may not provide screenshot evidence; the gate owns PrintWindow capture." }
		if ($null -ne $record.PSObject.Properties['inputRequest']) {
			throw "Click record '$id' uses the obsolete singular inputRequest contract; every atomic event must appear in ordered inputRequests."
		}
		$requestsStrict = Get-RequiredPreservedJsonMember -Object $recordStrict -Name 'inputRequests' -Description "Click record '$id'"
		$requests = @(Get-RequiredJsonProperty -Object $record -Name 'inputRequests' -Description "Click record '$id'")
		$minimumInputEvents = [int](Get-OptionalProperty -Object $declared -Name 'minimumInputEvents')
		if ($minimumInputEvents -lt 1) { $minimumInputEvents = 1 }
		if (-not (Test-IsJsonArray -Value $requestsStrict) -or $requests.Count -lt $minimumInputEvents -or $requests.Count -gt 128) {
			throw "Click record '$id' inputRequests must be a JSON array containing $minimumInputEvents to 128 ordered atomic events; one event cannot prove a multi-action narrative."
		}
		$eventIds = [System.Collections.Generic.HashSet[string]]::new([System.StringComparer]::Ordinal)
		$lastHumanEventTime = [DateTimeOffset]::MinValue
		for ($eventIndex = 0; $eventIndex -lt $requests.Count; ++$eventIndex) {
			$requestStrict = @($requestsStrict)[$eventIndex]
			$request = $requests[$eventIndex]
			$eventSequence = $eventIndex + 1
			if ($requestStrict -isnot [System.Collections.IDictionary] -or $null -eq $request -or $request -is [string] -or $request -is [System.Array]) {
				throw "Click record '$id' input event $eventSequence must be one JSON object."
			}
			$eventId = [string](Get-RequiredJsonProperty -Object $request -Name 'eventId' -Description "Click record '$id' input event $eventSequence")
			if ($eventId -notmatch '^[A-Za-z0-9][A-Za-z0-9._-]{0,95}$' -or -not $eventIds.Add($eventId)) {
				throw "Click record '$id' input event IDs must be unique path-safe names."
			}
			$kind = [string](Get-RequiredJsonProperty -Object $request -Name 'kind' -Description "Click record '$id' input event '$eventId'")
			$expectedKind = if ($input -ceq 'mouse') { 'mouse-click' } else { 'key-chord' }
			if ($kind -cne $expectedKind) { throw "Click record '$id' input event '$eventId' kind must be exactly '$expectedKind'." }
			$description = [string](Get-RequiredJsonProperty -Object $request -Name 'description' -Description "Click record '$id' input event '$eventId'")
			if ([string]::IsNullOrWhiteSpace($description)) { throw "Click record '$id' input event '$eventId' requires a non-empty atomic description." }
			if ($inputDelivery -ceq 'human') {
				$performedAtText = [string](Get-RequiredJsonProperty -Object $request -Name 'performedAtUtc' -Description "Click record '$id' human input event '$eventId'")
				$performedAt = [DateTimeOffset]::Parse($performedAtText, [Globalization.CultureInfo]::InvariantCulture, [Globalization.DateTimeStyles]::RoundtripKind).ToUniversalTime()
				if ($performedAt -le $lastHumanEventTime -or $performedAt -gt [DateTimeOffset]::UtcNow.AddSeconds(5)) {
					throw "Click record '$id' human input event '$eventId' must have a fresh strictly increasing performedAtUtc."
				}
				$lastHumanEventTime = $performedAt
			}
			elseif ($null -ne $request.PSObject.Properties['performedAtUtc']) {
				throw "Win32SendInput event '$eventId' may not claim a caller-authored performedAtUtc."
			}
			if ($inputDelivery -ceq 'Win32SendInput' -and $input -ceq 'mouse') {
				foreach ($coordinateName in @('clientX', 'clientY')) {
					$coordinate = Get-RequiredPreservedJsonMember -Object $requestStrict -Name $coordinateName -Description "Click record '$id' input event '$eventId'"
					if ($coordinate -isnot [int] -and $coordinate -isnot [long]) { throw "Win32 mouse event '$eventId' $coordinateName must be an integer." }
				}
				$button = [string](Get-RequiredJsonProperty -Object $request -Name 'button' -Description "Click record '$id' input event '$eventId'")
				if ($button -cnotin @('left', 'right')) { throw "Win32 mouse event '$eventId' button must be exactly 'left' or 'right'." }
			}
			elseif ($inputDelivery -ceq 'Win32SendInput') {
				$virtualKeysStrict = Get-RequiredPreservedJsonMember -Object $requestStrict -Name 'virtualKeys' -Description "Click record '$id' input event '$eventId'"
				$virtualKeys = @(Get-RequiredJsonProperty -Object $request -Name 'virtualKeys' -Description "Click record '$id' input event '$eventId'")
				if (-not (Test-IsJsonArray -Value $virtualKeysStrict) -or $virtualKeys.Count -lt 1 -or $virtualKeys.Count -gt 8) { throw "Win32 keyboard event '$eventId' requires one to eight virtualKeys." }
				foreach ($virtualKey in @($virtualKeysStrict)) {
					if (($virtualKey -isnot [int] -and $virtualKey -isnot [long]) -or [int64]$virtualKey -lt 1 -or [int64]$virtualKey -gt [UInt16]::MaxValue) {
						throw "Win32 keyboard event '$eventId' virtualKeys must be integer Windows key codes."
					}
				}
			}
		}
		$records.Add($record)
	}
	if ($RequireComplete -and $records.Count -ne $declaredItems.Count) {
		throw "Click transcript contains $($records.Count) actions but the scenario requires $($declaredItems.Count) in exact order."
	}
	return @($records)
}

function Get-NativeInputCapability {
	Initialize-NativeValidationBridge
	$capability = [TerraEngine.Animation.Validation.NativeWindowEvidence]::GetInputDesktopSnapshot()
	return [ordered]@{
		desktopOpened = [bool]$capability.DesktopOpened
		desktopName = [string]$capability.DesktopName
		inputAvailable = [bool]$capability.InputAvailable
		sessionLocked = [bool]$capability.SessionLocked
		windowsError = [int]$capability.WindowsError
		reason = [string]$capability.Reason
		classifiedAtUtc = [string]$capability.ClassifiedAtUtc
	}
}

function Invoke-GateOwnedInput {
	param(
		[Parameter(Mandatory = $true)]$Action,
		[Parameter(Mandatory = $true)][int]$EditorProcessId,
		[Parameter(Mandatory = $true)][Int64]$WindowHandle,
		[Parameter(Mandatory = $true)][DateTimeOffset]$ObservedAt
	)
	$delivery = [string]$Action.inputDelivery
	$requests = @($Action.inputRequests)
	$receipts = [System.Collections.Generic.List[object]]::new()
	$capability = if ($delivery -ceq 'human') { Get-NativeInputCapability } else { $null }
	for ($eventIndex = 0; $eventIndex -lt $requests.Count; ++$eventIndex) {
		$request = $requests[$eventIndex]
		$eventSequence = $eventIndex + 1
		$eventId = [string]$request.eventId
		$kind = [string]$request.kind
		if ($delivery -ceq 'human') {
			$performedAt = [DateTimeOffset]::Parse([string]$request.performedAtUtc, [Globalization.CultureInfo]::InvariantCulture, [Globalization.DateTimeStyles]::RoundtripKind).ToUniversalTime()
			if ($performedAt -gt $ObservedAt.ToUniversalTime()) {
				throw "Human input event '$eventId' claims it occurred after the gate observed its completed action record."
			}
			$receipts.Add([ordered]@{
				eventId = $eventId
				eventSequence = $eventSequence
				description = [string]$request.description
				succeeded = $true
				method = 'human'
				requestKind = $kind
				sendInputAttempted = $false
				eventCountRequested = 1
				eventCountSubmitted = 1
				windowsError = 0
				foregroundWindow = ('0x{0:x}' -f [UInt64]$WindowHandle)
				desktopName = [string]$capability.desktopName
				inputAvailable = [bool]$capability.inputAvailable
				sessionLocked = [bool]$capability.sessionLocked
				foregroundAcquisitionAttempted = $false
				foregroundAcquired = $false
				foregroundAcquiredBeforeInput = $false
				foregroundAcquiredByDeclaredMouseInput = $false
				foregroundAttempts = 0
				postInputForegroundChecks = 0
				previousForegroundWindow = ('0x{0:x}' -f [UInt64]$WindowHandle)
				targetWasMinimized = $false
				restoreRequested = $false
				zOrderRaiseAttempted = $false
				zOrderRaised = $false
				zOrderRestored = $true
				threadInputAttachmentUsed = $false
				threadInputAttachAttempts = 0
				threadInputAttachCount = 0
				threadInputDetachAttempts = 0
				threadInputDetachCount = 0
				threadInputDetached = $true
				threadInputError = 0
				startedAtUtc = $performedAt.ToString('o')
				completedAtUtc = $performedAt.ToString('o')
				gateRecordedAtUtc = $ObservedAt.ToUniversalTime().ToString('o')
				failureReason = ''
			})
			continue
		}

		$receipt = $null
		if ([string]$Action.input -ceq 'mouse') {
			$receipt = [TerraEngine.Animation.Validation.NativeWindowEvidence]::SendMouseClick(
				$EditorProcessId,
				$WindowHandle,
				[int]$request.clientX,
				[int]$request.clientY,
				[string]$request.button)
		}
		else {
			$receipt = [TerraEngine.Animation.Validation.NativeWindowEvidence]::SendKeyChord(
				$EditorProcessId,
				$WindowHandle,
				[int[]]@($request.virtualKeys))
		}
		$normalized = [ordered]@{
			eventId = $eventId
			eventSequence = $eventSequence
			description = [string]$request.description
			succeeded = [bool]$receipt.Succeeded
			method = [string]$receipt.Method
			requestKind = [string]$receipt.RequestKind
			sendInputAttempted = $true
			eventCountRequested = [int]$receipt.EventCountRequested
			eventCountSubmitted = [int]$receipt.EventCountSubmitted
			windowsError = [int]$receipt.WindowsError
			foregroundWindow = ('0x{0:x}' -f [UInt64]$receipt.ForegroundWindow)
			desktopName = [string]$receipt.DesktopName
			inputAvailable = [bool]$receipt.InputAvailable
			sessionLocked = -not [bool]$receipt.InputAvailable
			foregroundAcquisitionAttempted = [bool]$receipt.ForegroundAcquisitionAttempted
			foregroundAcquired = [bool]$receipt.ForegroundAcquired
			foregroundAcquiredBeforeInput = [bool]$receipt.ForegroundAcquiredBeforeInput
			foregroundAcquiredByDeclaredMouseInput = [bool]$receipt.ForegroundAcquiredByDeclaredMouseInput
			foregroundAttempts = [int]$receipt.ForegroundAttempts
			postInputForegroundChecks = [int]$receipt.PostInputForegroundChecks
			previousForegroundWindow = ('0x{0:x}' -f [UInt64]$receipt.PreviousForegroundWindow)
			targetWasMinimized = [bool]$receipt.TargetWasMinimized
			restoreRequested = [bool]$receipt.RestoreRequested
			zOrderRaiseAttempted = [bool]$receipt.ZOrderRaiseAttempted
			zOrderRaised = [bool]$receipt.ZOrderRaised
			zOrderRestored = [bool]$receipt.ZOrderRestored
			threadInputAttachmentUsed = [bool]$receipt.ThreadInputAttachmentUsed
			threadInputAttachAttempts = [int]$receipt.ThreadInputAttachAttempts
			threadInputAttachCount = [int]$receipt.ThreadInputAttachCount
			threadInputDetachAttempts = [int]$receipt.ThreadInputDetachAttempts
			threadInputDetachCount = [int]$receipt.ThreadInputDetachCount
			threadInputDetached = [bool]$receipt.ThreadInputDetached
			threadInputError = [int]$receipt.ThreadInputError
			dpiAwareGeometry = [bool]$receipt.DpiAwareGeometry
			dpiAwarenessContext = [string]$receipt.DpiAwarenessContext
			dpi = [int]$receipt.Dpi
			clientPixelWidth = [int]$receipt.ClientPixelWidth
			clientPixelHeight = [int]$receipt.ClientPixelHeight
			requestedClientX = [int]$receipt.RequestedClientX
			requestedClientY = [int]$receipt.RequestedClientY
			screenX = [int]$receipt.ScreenX
			screenY = [int]$receipt.ScreenY
			positionedScreenX = [int]$receipt.PositionedScreenX
			positionedScreenY = [int]$receipt.PositionedScreenY
			physicalCursorPositionVerified = [bool]$receipt.PhysicalCursorPositionVerified
			startedAtUtc = [string]$receipt.StartedAtUtc
			completedAtUtc = [string]$receipt.CompletedAtUtc
			gateRecordedAtUtc = [DateTimeOffset]::UtcNow.ToString('o')
			failureReason = [string]$receipt.FailureReason
		}
		if (-not [bool]$receipt.Succeeded) {
			throw "Win32SendInput event '$eventId' in action '$($Action.id)' failed or is unavailable: $($receipt.FailureReason) Windows error $($receipt.WindowsError)."
		}
		$receipts.Add($normalized)
	}
	return @($receipts)
}

function New-GateNativeActionEvidence {
	param(
		[Parameter(Mandatory = $true)]$Action,
		[Parameter(Mandatory = $true)][int]$ActionSequence,
		[Parameter(Mandatory = $true)][string]$RunRoot,
		[Parameter(Mandatory = $true)]$EditorProcess
	)
	Initialize-NativeValidationBridge
	$EditorProcess.Refresh()
	if ($EditorProcess.HasExited) { throw "Editor exited before native evidence for '$($Action.id)' could be captured." }
	$windowHandle = [Int64]$EditorProcess.MainWindowHandle.ToInt64()
	if ($windowHandle -eq 0) { throw "The launched editor has no native HWND for action '$($Action.id)'." }
	$observedAt = [DateTimeOffset]::UtcNow
	$inputReceipts = @(Invoke-GateOwnedInput -Action $Action -EditorProcessId ([int]$EditorProcess.Id) -WindowHandle $windowHandle -ObservedAt $observedAt)
	if ($inputReceipts.Count -ne @($Action.inputRequests).Count) {
		throw "Gate-owned input receipt count does not match action '$($Action.id)' atomic event count."
	}
	$captureRelativePath = "editor/manual/{0:d2}-{1}-native-window.png" -f $ActionSequence, [string]$Action.id
	$capturePath = Get-AbsolutePath -Path $captureRelativePath -BasePath $RunRoot
	Assert-PathBelow -Parent $RunRoot -Child $capturePath -Description 'Gate-owned native screenshot'
	$capture = [TerraEngine.Animation.Validation.NativeWindowEvidence]::CaptureWindow([int]$EditorProcess.Id, $windowHandle, $capturePath)
	$bytes = [System.IO.File]::ReadAllBytes($capturePath)
	$dimensions = Get-PngDimensions -Bytes $bytes
	if ([int]$capture.PixelWidth -ne [int]$dimensions.width -or [int]$capture.PixelHeight -ne [int]$dimensions.height) {
		throw "Gate-owned screenshot '$($Action.id)' dimensions differ from its live HWND bounds."
	}
	if ([int]$dimensions.width -lt 320 -or [int]$dimensions.height -lt 200) {
		throw "Gate-owned screenshot '$($Action.id)' is too small to be credible editor-window evidence."
	}
	$statistics = Get-PngVisualStatistics -Bytes $bytes
	if ([bool]$statistics.isSolidBlack -or [bool]$statistics.isRgbUniform -or [UInt64]$statistics.visibleNonBlackPixelCount -eq 0) {
		throw "Gate-owned screenshot '$($Action.id)' is black, uniform, or contains no visible non-black pixels."
	}
	$file = Get-Item -LiteralPath $capturePath
	return [ordered]@{
		schemaVersion = 2
		gateOwned = $true
		actionId = [string]$Action.id
		actionSequence = $ActionSequence
		input = [string]$Action.input
		inputDelivery = [string]$Action.inputDelivery
		actionObservedAtUtc = $observedAt.ToUniversalTime().ToString('o')
		inputReceipts = @($inputReceipts)
		evidence = @([ordered]@{
			path = $captureRelativePath
			kind = 'native-window-screenshot'
			actionId = [string]$Action.id
			actionSequence = $ActionSequence
			sha256 = Get-Sha256 -Path $capturePath
			pixelSha256 = [string]$statistics.pixelSha256
			fileLastWriteTimeUtc = $file.LastWriteTimeUtc.ToString('o')
			nativeWindow = [ordered]@{
				processId = [int]$capture.ProcessId
				windowHandle = ('0x{0:x}' -f [UInt64]$capture.WindowHandle)
				windowTitle = [string]$capture.WindowTitle
				ownerValidatedAtCapture = $true
				captureMethod = 'PrintWindow'
				printWindowSucceeded = [bool]$capture.PrintWindowSucceeded
				printWindowError = [int]$capture.PrintWindowError
				capturedAtUtc = [string]$capture.CapturedAtUtc
				dpiAware = [bool]$capture.DpiAwareCapture
				dpiAwarenessContext = [string]$capture.DpiAwarenessContext
				dpi = [int]$capture.Dpi
				left = [int]$capture.Left
				top = [int]$capture.Top
				pixelWidth = [int]$capture.PixelWidth
				pixelHeight = [int]$capture.PixelHeight
			}
			visualStatistics = $statistics
		})
	}
}

function Read-AndValidateManualEvidence {
	param(
		[Parameter(Mandatory = $true)]$Scenario,
		[Parameter(Mandatory = $true)][string]$ClickPath,
		[Parameter(Mandatory = $true)][string]$NativeEvidencePath,
		[Parameter(Mandatory = $true)][string]$CompletionPath,
		[Parameter(Mandatory = $true)][string]$RunRoot,
		[Parameter(Mandatory = $true)]$EditorProcess,
		[Parameter(Mandatory = $true)][DateTimeOffset]$ManualStageStartedAtUtc
	)
	if (-not (Test-Path -LiteralPath $CompletionPath -PathType Leaf)) { throw 'Required manual-complete.json is missing.' }
	$completionText = Get-Content -Raw -LiteralPath $CompletionPath
	$completionStrict = ConvertFrom-JsonPreservingArrays -Json $completionText -Description 'manual-complete.json'
	try { $completion = $completionText | ConvertFrom-Json } catch { throw "manual-complete.json is not valid JSON: $($_.Exception.Message)" }
	if ($completionStrict -isnot [System.Collections.IDictionary] -or $null -eq $completion -or $completion -is [System.Array]) { throw 'manual-complete.json must contain one JSON object.' }
	$completed = Get-RequiredPreservedJsonMember -Object $completionStrict -Name 'completed' -Description 'manual-complete.json'
	if ($completed -isnot [bool] -or -not $completed) { throw "manual-complete.json property 'completed' must be the JSON Boolean true." }
	$defectsStrict = Get-RequiredPreservedJsonMember -Object $completionStrict -Name 'defects' -Description 'manual-complete.json'
	if (-not (Test-IsJsonArray -Value $defectsStrict)) { throw "manual-complete.json property 'defects' must be a JSON array." }
	$defects = @(Get-OptionalProperty -Object $completion -Name 'defects')
	foreach ($defect in @($defects)) {
		if ($null -eq $defect -or $defect -is [string] -or $defect -is [System.Array]) { throw 'Every manual defect must be a JSON object.' }
		$defectId = [string](Get-RequiredJsonProperty -Object $defect -Name 'id' -Description 'Manual defect')
		$observedDefect = [string](Get-RequiredJsonProperty -Object $defect -Name 'observed' -Description "Manual defect '$defectId'")
		$defectStatus = [string](Get-RequiredJsonProperty -Object $defect -Name 'status' -Description "Manual defect '$defectId'")
		if ([string]::IsNullOrWhiteSpace($defectId) -or [string]::IsNullOrWhiteSpace($observedDefect)) { throw 'Every manual defect requires non-empty id and observed fields.' }
		if ($defectStatus -cne 'fixed') { throw "Manual defect '$defectId' remains unresolved; status must be exactly 'fixed'." }
	}

	$actions = @(Read-ManualActionRecords -Scenario $Scenario -ClickPath $ClickPath -RequireComplete)
	if (-not (Test-Path -LiteralPath $NativeEvidencePath -PathType Leaf)) { throw 'Gate-owned native action evidence is missing.' }
	$nativeRecords = [System.Collections.Generic.List[object]]::new()
	$lineNumber = 0
	foreach ($line in @(Get-Content -LiteralPath $NativeEvidencePath)) {
		$lineNumber++
		if ([string]::IsNullOrWhiteSpace($line)) { continue }
		$strict = ConvertFrom-JsonPreservingArrays -Json $line -Description "Gate-owned native evidence line $lineNumber"
		try { $record = $line | ConvertFrom-Json } catch { throw "Gate-owned native evidence line $lineNumber is invalid JSON." }
		if ($strict -isnot [System.Collections.IDictionary] -or $null -eq $record -or $record -is [System.Array]) { throw "Gate-owned native evidence line $lineNumber must contain one JSON object." }
		$gateOwned = Get-RequiredPreservedJsonMember -Object $strict -Name 'gateOwned' -Description "Gate-owned native evidence line $lineNumber"
		if ($gateOwned -isnot [bool] -or -not $gateOwned) { throw "Native evidence line $lineNumber is not marked as gate-owned." }
		$nativeRecords.Add($record)
	}
	if ($nativeRecords.Count -ne $actions.Count) { throw 'Every exact manual action must have one gate-owned native evidence record.' }

	$screenshotPaths = [System.Collections.Generic.HashSet[string]]::new([System.StringComparer]::OrdinalIgnoreCase)
	$pixelHashes = [System.Collections.Generic.HashSet[string]]::new([System.StringComparer]::Ordinal)
	$lastCapture = [DateTimeOffset]::MinValue
	$normalizedRecords = [System.Collections.Generic.List[object]]::new()
	for ($index = 0; $index -lt $actions.Count; ++$index) {
		$action = $actions[$index]
		$native = $nativeRecords[$index]
		$expectedSequence = $index + 1
		if ([string]$native.actionId -cne [string]$action.id -or [int]$native.actionSequence -ne $expectedSequence) { throw "Native evidence action $expectedSequence is not bound to '$($action.id)'." }
		if ([string]$native.input -cne [string]$action.input -or [string]$native.inputDelivery -cne [string]$action.inputDelivery) { throw "Native evidence for '$($action.id)' does not match its declared input contract." }
		$observedAt = [DateTimeOffset]::Parse([string]$native.actionObservedAtUtc, [Globalization.CultureInfo]::InvariantCulture, [Globalization.DateTimeStyles]::RoundtripKind).ToUniversalTime()
		if ($observedAt -lt $ManualStageStartedAtUtc.ToUniversalTime()) { throw "Native evidence for '$($action.id)' predates the manual stage." }
		if ($null -ne $native.PSObject.Properties['inputReceipt']) { throw "Native evidence for '$($action.id)' uses the obsolete singular inputReceipt contract." }
		$requests = @($action.inputRequests)
		$receipts = @($native.inputReceipts)
		if ($receipts.Count -ne $requests.Count) { throw "Native action '$($action.id)' must have one gate-owned receipt for every ordered atomic input event." }
		$inputCompleted = [DateTimeOffset]::MinValue
		for ($eventIndex = 0; $eventIndex -lt $requests.Count; ++$eventIndex) {
			$request = $requests[$eventIndex]
			$receipt = $receipts[$eventIndex]
			$eventSequence = $eventIndex + 1
			if ($null -eq $receipt -or -not [bool]$receipt.succeeded) { throw "Native input receipt $eventSequence for '$($action.id)' did not succeed." }
			if ([string]$receipt.eventId -cne [string]$request.eventId -or [int]$receipt.eventSequence -ne $eventSequence -or [string]$receipt.requestKind -cne [string]$request.kind -or [string]$receipt.description -cne [string]$request.description) {
				throw "Native input receipt $eventSequence for '$($action.id)' is not bound to its exact declared event."
			}
			$inputStarted = [DateTimeOffset]::Parse([string]$receipt.startedAtUtc, [Globalization.CultureInfo]::InvariantCulture, [Globalization.DateTimeStyles]::RoundtripKind).ToUniversalTime()
			$currentInputCompleted = [DateTimeOffset]::Parse([string]$receipt.completedAtUtc, [Globalization.CultureInfo]::InvariantCulture, [Globalization.DateTimeStyles]::RoundtripKind).ToUniversalTime()
			if ($inputStarted -lt $ManualStageStartedAtUtc.ToUniversalTime() -or $inputStarted -lt $inputCompleted -or $currentInputCompleted -lt $inputStarted) {
				throw "Native input receipt $eventSequence for '$($action.id)' has invalid ordered action-bound timestamps."
			}
			if ([string]$action.inputDelivery -ceq 'Win32SendInput') {
				$expectedForegroundWindow = ('0x{0:x}' -f [UInt64]$EditorProcess.MainWindowHandle.ToInt64())
				$foregroundAcquiredBeforeInput = [bool]$receipt.foregroundAcquiredBeforeInput
				$foregroundAcquiredByDeclaredMouseInput = [bool]$receipt.foregroundAcquiredByDeclaredMouseInput
				$foregroundContractPassed = if ([string]$action.input -ceq 'mouse') {
					[bool]$receipt.foregroundAcquired -and
						($foregroundAcquiredBeforeInput -xor $foregroundAcquiredByDeclaredMouseInput) -and
						[int]$receipt.postInputForegroundChecks -ge 1 -and
						[int]$receipt.postInputForegroundChecks -le 8
				}
				else {
					[bool]$receipt.foregroundAcquired -and
						$foregroundAcquiredBeforeInput -and
						-not $foregroundAcquiredByDeclaredMouseInput -and
						[int]$receipt.postInputForegroundChecks -eq 0
				}
				$dpiGeometryContractPassed = if ([string]$action.input -ceq 'mouse') {
					[bool]$receipt.dpiAwareGeometry -and
						[string]$receipt.dpiAwarenessContext -ceq 'PER_MONITOR_AWARE_V2' -and
						[int]$receipt.dpi -ge 48 -and
						[int]$receipt.clientPixelWidth -gt 0 -and
						[int]$receipt.clientPixelHeight -gt 0 -and
						[int]$receipt.requestedClientX -eq [int]$request.clientX -and
						[int]$receipt.requestedClientY -eq [int]$request.clientY -and
						[int]$receipt.requestedClientX -ge 0 -and
						[int]$receipt.requestedClientY -ge 0 -and
						[int]$receipt.requestedClientX -lt [int]$receipt.clientPixelWidth -and
						[int]$receipt.requestedClientY -lt [int]$receipt.clientPixelHeight -and
						[bool]$receipt.physicalCursorPositionVerified -and
						[int]$receipt.screenX -eq [int]$receipt.positionedScreenX -and
						[int]$receipt.screenY -eq [int]$receipt.positionedScreenY
				}
				else {
					$true
				}
				if ($inputStarted -lt $observedAt -or [string]$receipt.method -cne 'SendInput' -or -not [bool]$receipt.sendInputAttempted -or -not [bool]$receipt.inputAvailable -or [bool]$receipt.sessionLocked -or
					-not [bool]$receipt.foregroundAcquisitionAttempted -or -not $foregroundContractPassed -or [int]$receipt.foregroundAttempts -lt 1 -or [int]$receipt.foregroundAttempts -gt 12 -or [string]$receipt.foregroundWindow -cne $expectedForegroundWindow -or
					-not [bool]$receipt.threadInputDetached -or [int]$receipt.threadInputAttachCount -ne [int]$receipt.threadInputDetachCount -or [int]$receipt.threadInputDetachAttempts -ne [int]$receipt.threadInputAttachCount -or
					-not $dpiGeometryContractPassed -or [int]$receipt.eventCountRequested -lt 1 -or [int]$receipt.eventCountSubmitted -ne [int]$receipt.eventCountRequested -or [int]$receipt.windowsError -ne 0) {
					throw "Win32SendInput receipt $eventSequence for '$($action.id)' is incomplete or did not submit every event."
				}
			}
			elseif ($currentInputCompleted -gt $observedAt -or [string]$receipt.method -cne 'human' -or [bool]$receipt.sendInputAttempted -or [int]$receipt.eventCountRequested -ne 1 -or [int]$receipt.eventCountSubmitted -ne 1) {
				throw "Human input receipt $eventSequence for '$($action.id)' is not an ordered pre-observation human event."
			}
			$inputCompleted = $currentInputCompleted
		}
		$evidence = @($native.evidence)
		if ($evidence.Count -ne 1) { throw "Native action '$($action.id)' must have exactly one gate-owned PrintWindow screenshot." }
		$artifactRecord = $evidence[0]
		if ([string]$artifactRecord.kind -cne 'native-window-screenshot' -or [string]$artifactRecord.actionId -cne [string]$action.id -or [int]$artifactRecord.actionSequence -ne $expectedSequence) {
			throw "Native screenshot metadata is not bound to exact action '$($action.id)' sequence $expectedSequence."
		}
		$relativePath = [string]$artifactRecord.path
		if ([string]::IsNullOrWhiteSpace($relativePath) -or [System.IO.Path]::IsPathRooted($relativePath) -or $relativePath -match '[:*?]' -or @($relativePath -split '[\\/]' | Where-Object { $_ -in @('.', '..') }).Count -gt 0 -or [System.IO.Path]::GetExtension($relativePath) -cne '.png') {
			throw "Native screenshot for '$($action.id)' must use a safe run-root-relative PNG path."
		}
		$normalizedPath = $relativePath.Replace('\', '/')
		if (-not $screenshotPaths.Add($normalizedPath)) { throw "Native screenshot path '$relativePath' is reused across checklist actions." }
		$artifact = Get-AbsolutePath -Path $relativePath -BasePath $RunRoot
		Assert-PathBelow -Parent $RunRoot -Child $artifact -Description 'Gate-owned native screenshot'
		if (-not (Test-Path -LiteralPath $artifact -PathType Leaf) -or (Get-Item -LiteralPath $artifact).Length -eq 0) { throw "Gate-owned native screenshot is missing or empty: '$relativePath'." }
		$artifactHash = Get-Sha256 -Path $artifact
		if ($artifactHash -cne [string]$artifactRecord.sha256) { throw "Gate-owned screenshot '$relativePath' changed after capture." }
		$bytes = [System.IO.File]::ReadAllBytes($artifact)
		$dimensions = Get-PngDimensions -Bytes $bytes
		$statistics = Get-PngVisualStatistics -Bytes $bytes
		if ([string]$statistics.pixelSha256 -cne [string]$artifactRecord.pixelSha256) { throw "Gate-owned screenshot '$relativePath' pixel digest changed after capture." }
		if (-not $pixelHashes.Add([string]$statistics.pixelSha256)) { throw "Action '$($action.id)' reuses identical screenshot pixels from an earlier checklist action." }
		if ([bool]$statistics.isSolidBlack -or [bool]$statistics.isRgbUniform -or [UInt64]$statistics.visibleNonBlackPixelCount -eq 0) { throw "Action '$($action.id)' screenshot is black, uniform, or contains no visible non-black pixels." }
		$window = $artifactRecord.nativeWindow
		if ([int]$window.processId -ne [int]$EditorProcess.Id -or -not [bool]$window.ownerValidatedAtCapture) { throw "Action '$($action.id)' screenshot lacks live HWND owner validation." }
		$windowHandleText = [string]$window.windowHandle
		if ($windowHandleText -notmatch '^0x[0-9a-fA-F]+$') { throw "Action '$($action.id)' screenshot has an invalid native HWND." }
		$windowHandle = [Convert]::ToInt64($windowHandleText.Substring(2), 16)
		$EditorProcess.Refresh()
		if ($EditorProcess.HasExited -or $windowHandle -ne [Int64]$EditorProcess.MainWindowHandle.ToInt64()) {
			throw "Action '$($action.id)' screenshot windowHandle does not match the launched editor's live HWND."
		}
		# Re-querying ownership catches a stale, destroyed, or PID-unrelated HWND.
		# Geometry is intentionally evaluated from the atomic capture record below
		# because the operator may move the same live window between actions.
		[void][TerraEngine.Animation.Validation.NativeWindowEvidence]::GetWindowSnapshot([int]$EditorProcess.Id, $windowHandle)
		if ([string]::IsNullOrWhiteSpace([string]$window.windowTitle) -or [string]$window.captureMethod -cne 'PrintWindow' -or -not [bool]$window.printWindowSucceeded -or [int]$window.printWindowError -ne 0 -or -not [bool]$window.dpiAware -or [string]$window.dpiAwarenessContext -cne 'PER_MONITOR_AWARE_V2' -or [int]$window.dpi -lt 48) {
			throw "Action '$($action.id)' screenshot lacks successful runtime-verified Per-Monitor-V2 DPI-aware PrintWindow metadata."
		}
		foreach ($geometryField in @('left', 'top', 'pixelWidth', 'pixelHeight')) {
			[void](Get-RequiredJsonProperty -Object $window -Name $geometryField -Description "Action '$($action.id)' native window")
		}
		if ([int]$window.pixelWidth -ne [int]$dimensions.width -or [int]$window.pixelHeight -ne [int]$dimensions.height -or [int]$window.pixelWidth -lt 320 -or [int]$window.pixelHeight -lt 200) {
			throw "Action '$($action.id)' screenshot dimensions do not match sensible live HWND bounds."
		}
		$capturedAt = [DateTimeOffset]::Parse([string]$window.capturedAtUtc).ToUniversalTime()
		if ($capturedAt -lt $inputCompleted -or $capturedAt -le $lastCapture -or $capturedAt -gt [DateTimeOffset]::UtcNow.AddSeconds(5)) { throw "Action '$($action.id)' capture timestamp is not fresh and action-bound." }
		$lastCapture = $capturedAt
		$file = Get-Item -LiteralPath $artifact
		$declaredFileTime = [DateTimeOffset]::Parse([string]$artifactRecord.fileLastWriteTimeUtc).ToUniversalTime()
		$actualFileTime = [DateTimeOffset]::new($file.LastWriteTimeUtc)
		if ($declaredFileTime -ne $actualFileTime -or [Math]::Abs(($declaredFileTime - $capturedAt).TotalSeconds) -gt 5) { throw "Action '$($action.id)' screenshot file time is not bound to its capture." }
		$normalizedRecords.Add([ordered]@{ id = [string]$action.id; input = [string]$action.input; inputDelivery = [string]$action.inputDelivery; passed = $true; observed = [string]$action.observed; actionObservedAtUtc = $observedAt.ToString('o'); inputReceipts = @($receipts); evidence = @([ordered]@{ path = $normalizedPath; kind = 'native-window-screenshot'; actionId = [string]$action.id; actionSequence = $expectedSequence; sha256 = $artifactHash; pixelSha256 = [string]$statistics.pixelSha256; nativeWindow = $window; visualStatistics = $statistics }) })
	}
	$completionTime = [DateTimeOffset]::new((Get-Item -LiteralPath $CompletionPath).LastWriteTimeUtc)
	if ($completionTime -lt $lastCapture) { throw 'manual-complete.json was written before the final gate-owned native capture completed.' }
	return [ordered]@{
		schemaVersion = 2
		completed = $true
		recordCount = $normalizedRecords.Count
		records = @($normalizedRecords)
		defects = @($defects)
		manualStageStartedAtUtc = $ManualStageStartedAtUtc.ToUniversalTime().ToString('o')
		completedFileTimeUtc = $completionTime.ToString('o')
		editor = [ordered]@{ processId = [int]$EditorProcess.Id }
	}
}

function Wait-AndValidateClickEvidence {
	param(
		[Parameter(Mandatory = $true)]$Scenario,
		[Parameter(Mandatory = $true)][string]$ClickPath,
		[Parameter(Mandatory = $true)][string]$NativeEvidencePath,
		[Parameter(Mandatory = $true)][string]$CompletionPath,
		[Parameter(Mandatory = $true)][string]$RunRoot,
		[Parameter(Mandatory = $true)]$EditorProcess,
		[Parameter(Mandatory = $true)][DateTimeOffset]$ManualStageStartedAtUtc,
		[Parameter(Mandatory = $true)][int]$TimeoutSeconds
	)
	if (Test-Path -LiteralPath $NativeEvidencePath) { throw 'Gate-owned native action evidence path already exists; phase evidence is append-never-overwrite.' }
	Write-Host "Perform each exact checklist action in the installed editor, then append its action request in declared order to: $ClickPath"
	Write-Host "The gate captures PrintWindow evidence itself. Provide every atomic event in ordered inputRequests; the gate emits one receipt per event."
	Write-Host "Only after every native capture is complete write {`"completed`":true,`"defects`":[]} to: $CompletionPath"
	$processed = 0
	$deadline = (Get-Date).AddSeconds($TimeoutSeconds)
	while ((Get-Date) -lt $deadline) {
		$EditorProcess.Refresh()
		if ($EditorProcess.HasExited) { throw 'Editor exited before manual evidence was completed.' }
		$actions = @(Read-ManualActionRecords -Scenario $Scenario -ClickPath $ClickPath)
		while ($processed -lt $actions.Count) {
			$native = New-GateNativeActionEvidence -Action $actions[$processed] -ActionSequence ($processed + 1) -RunRoot $RunRoot -EditorProcess $EditorProcess
			Add-JsonLine -Path $NativeEvidencePath -Value $native
			$processed++
		}
		if (Test-Path -LiteralPath $CompletionPath -PathType Leaf) {
			$completion = Get-Content -Raw -LiteralPath $CompletionPath | ConvertFrom-Json
			$completed = Get-OptionalProperty -Object $completion -Name 'completed'
			if ($completed -is [bool] -and $completed) {
				if ($actions.Count -ne @($Scenario.manualChecklist).Count -or $processed -ne $actions.Count) {
					throw 'manual-complete.json was written before every declared action received gate-owned native evidence.'
				}
				break
			}
		}
		Start-Sleep -Milliseconds 250
	}
	if (-not (Test-Path -LiteralPath $CompletionPath -PathType Leaf)) {
		$capability = Get-NativeInputCapability
		throw "Manual evidence did not complete before the bounded timeout. Input capability: $($capability.reason)"
	}
	return Read-AndValidateManualEvidence -Scenario $Scenario -ClickPath $ClickPath -NativeEvidencePath $NativeEvidencePath -CompletionPath $CompletionPath -RunRoot $RunRoot -EditorProcess $EditorProcess -ManualStageStartedAtUtc $ManualStageStartedAtUtc
}

function Get-ScenarioPreservationDigest {
	param(
		[Parameter(Mandatory = $true)][string]$ProjectRoot,
		[Parameter(Mandatory = $true)][string]$RelativePath
	)
	$path = Get-AbsolutePath -Path $RelativePath -BasePath $ProjectRoot
	Assert-PathBelow -Parent $ProjectRoot -Child $path -Description 'Scenario preservation root'
	if (-not (Test-Path -LiteralPath $path)) {
		throw "Scenario preservation root does not exist: '$RelativePath'."
	}
	if (Test-Path -LiteralPath $path -PathType Leaf) {
		return [ordered]@{ path = $RelativePath; kind = 'file'; files = 1; sha256 = Get-Sha256 -Path $path }
	}
	$inventory = @(Get-FileInventory -Root $path)
	return [ordered]@{ path = $RelativePath; kind = 'directory'; files = $inventory.Count; sha256 = Get-InventoryDigest -Inventory $inventory }
}

function Get-StableScenarioPreservationDigest {
	param(
		[Parameter(Mandatory = $true)][string]$ProjectRoot,
		[Parameter(Mandatory = $true)][string]$RelativePath
	)
	# The normal editor file watcher may still be importing child documents from
	# the preceding valid package import. Require consecutive, byte-identical
	# inventories before replacing the source so this failure probe compares the
	# actual last-known-good package rather than a previous import's tail work.
	$deadline = [DateTimeOffset]::UtcNow.AddMilliseconds($script:ScenarioPreservationStabilizationTimeoutMilliseconds)
	$stableSamples = 0
	$previous = $null
	$observedDigests = [System.Collections.Generic.List[string]]::new()
	while ([DateTimeOffset]::UtcNow -le $deadline) {
		$current = Get-ScenarioPreservationDigest -ProjectRoot $ProjectRoot -RelativePath $RelativePath
		$observedDigests.Add([string]$current.sha256)
		if ($null -ne $previous -and [string]$previous.sha256 -ceq [string]$current.sha256) {
			$stableSamples++
		}
		else {
			$stableSamples = 1
		}
		if ($stableSamples -ge $script:ScenarioPreservationStableSamples) {
			$current['stabilization'] = [ordered]@{
				stableSamples = $stableSamples
				intervalMilliseconds = $script:ScenarioPreservationStableIntervalMilliseconds
				observedDigests = @($observedDigests)
			}
			return $current
		}
		$previous = $current
		Start-Sleep -Milliseconds $script:ScenarioPreservationStableIntervalMilliseconds
	}
	throw "Scenario preservation root '$RelativePath' did not become quiescent before the malformed reimport. Observed digests: $($observedDigests -join ', ')."
}

function Start-ScenarioFileTransition {
	param(
		[Parameter(Mandatory = $true)]$Transition,
		[Parameter(Mandatory = $true)][string]$OperationId,
		[Parameter(Mandatory = $true)][string]$RunRoot
	)
	# A phase may deliberately replace one isolated-project source with a malformed
	# fixture immediately around an MCP reimport. The original source and every
	# declared last-known-good output are hashed so the failure proof is atomic.
	$projectRoot = Join-Path $RunRoot 'project'
	$targetRelative = [string]$Transition.target
	$replacementRelative = [string]$Transition.replacement
	$target = Get-AbsolutePath -Path $targetRelative -BasePath $projectRoot
	$replacement = Get-AbsolutePath -Path $replacementRelative -BasePath $projectRoot
	Assert-PathBelow -Parent $projectRoot -Child $target -Description 'Scenario transition target'
	Assert-PathBelow -Parent $projectRoot -Child $replacement -Description 'Scenario transition replacement'
	if (-not (Test-Path -LiteralPath $target -PathType Leaf) -or -not (Test-Path -LiteralPath $replacement -PathType Leaf)) {
		throw "Scenario file transition '$OperationId' requires existing target and replacement files."
	}
	foreach ($path in @($target, $replacement)) {
		if ((Get-Item -LiteralPath $path -Force).Attributes -band [System.IO.FileAttributes]::ReparsePoint) {
			throw "Scenario file transition '$OperationId' rejects symbolic links and junctions."
		}
	}
	$backup = Join-Path $RunRoot "commands/$OperationId-source-backup.bin"
	Copy-Item -LiteralPath $target -Destination $backup
	$preservedBefore = @()
	foreach ($relative in @(Get-OptionalProperty -Object $Transition -Name 'preserveRoots')) {
		$preservedBefore += Get-StableScenarioPreservationDigest -ProjectRoot $projectRoot -RelativePath ([string]$relative)
	}
	Copy-Item -LiteralPath $replacement -Destination $target -Force
	return [ordered]@{
		projectRoot = $projectRoot
		target = $target
		targetRelative = $targetRelative
		backup = $backup
		originalSha256 = Get-Sha256 -Path $backup
		replacementSha256 = Get-Sha256 -Path $target
		preservedBefore = @($preservedBefore)
		restored = $false
	}
}

function Restore-ScenarioFileTransition {
	param([Parameter(Mandatory = $true)]$State)
	if (-not [bool]$State.restored) {
		Copy-Item -LiteralPath $State.backup -Destination $State.target -Force
		$State.restored = $true
	}
	if ((Get-Sha256 -Path $State.target) -cne [string]$State.originalSha256) {
		throw "Scenario file transition failed to restore '$($State.targetRelative)'."
	}
}

function Complete-ScenarioFileTransition {
	param(
		[Parameter(Mandatory = $true)]$State,
		[Parameter(Mandatory = $true)]$Transition
	)
	$preserved = @()
	try {
		foreach ($before in @($State.preservedBefore)) {
			$after = Get-ScenarioPreservationDigest -ProjectRoot $State.projectRoot -RelativePath ([string]$before.path)
			$matched = [string]$before.sha256 -ceq [string]$after.sha256
			$preserved += [ordered]@{ path = [string]$before.path; beforeSha256 = [string]$before.sha256; afterSha256 = [string]$after.sha256; matched = $matched }
			if (-not $matched) {
				# Include both immutable digests in the terminal error. A failed gate
				# otherwise cannot retain this per-operation evidence because the
				# operation artifact is written only after the transition completes.
				throw "Failed reimport changed protected last-known-good output '$($before.path)' (before=$($before.sha256), after=$($after.sha256))."
			}
		}
	}
	finally {
		Restore-ScenarioFileTransition -State $State
	}
	return [ordered]@{
		target = [string]$State.targetRelative
		originalSha256 = [string]$State.originalSha256
		replacementSha256 = [string]$State.replacementSha256
		restored = [bool]$State.restored
		preservedRoots = @($preserved)
	}
}

function Invoke-ScenarioStage {
	param(
		[Parameter(Mandatory = $true)]$Scenario,
		[Parameter(Mandatory = $true)][string]$Stage,
		[Parameter(Mandatory = $true)][string]$CameraRoot,
		[Parameter(Mandatory = $true)][string]$RunRoot,
		[Parameter(Mandatory = $true)]$CameraInventory,
		[Parameter(Mandatory = $true)]$OperationInventory,
		[Parameter(Mandatory = $true)]$BindingRegistry,
		[Parameter(Mandatory = $true)]$ApprovalRegistry,
		[Parameter(Mandatory = $true)][int]$ApprovalTimeoutSeconds
	)
	$items = [System.Collections.Generic.List[object]]::new()
	foreach ($operation in @($Scenario.operations | Where-Object { [string]$_.stage -eq $Stage })) { $items.Add([ordered]@{ kind = 'operation'; value = $operation; order = [int]$operation.order }) }
	foreach ($capture in @($Scenario.captures | Where-Object { [string]$_.stage -eq $Stage })) { $items.Add([ordered]@{ kind = 'capture'; value = $capture; order = [int]$capture.order }) }
	# The merged records are OrderedDictionary values. Sort-Object's shorthand
	# property adapter does not reliably expose dictionary keys, which previously
	# let destructive fixture transitions run before their prerequisite imports.
	# Read the integer key explicitly so every phase executes its reviewed order.
	foreach ($item in @($items | Sort-Object { [int]$_['order'] })) {
		$definition = $item.value
		$arguments = Get-ScenarioInvocationArguments -Definition $definition -Kind ([string]$item.kind)
		$bindingResolution = [ordered]@{
			argumentsSha256 = Get-CanonicalJsonSha256 -Value $arguments
			bindings = @()
		}
		$transition = Get-OptionalProperty -Object $definition -Name 'fileTransition'
		$transitionState = $null
		$transitionEvidence = $null
		$approvalEvidence = $null
		if ($item.kind -eq 'operation' -and $null -ne $transition) {
			$transitionState = Start-ScenarioFileTransition -Transition $transition -OperationId ([string]$definition.id) -RunRoot $RunRoot
		}
		try {
			# Resolve from already accepted in-memory responses immediately before
			# the one reviewed MCP call. No transcript replay or retry can provide
			# a value to this path.
			if ($item.kind -eq 'operation') {
				$bindingResolution = Resolve-ScenarioArgumentBindings -Definition $definition -Arguments $arguments -BindingRegistry $BindingRegistry
			}
			$expectedMcpSequence = $script:McpSequence + 1
			$approvalPolicy = Get-OptionalProperty -Object $definition -Name 'approval'
			if ($item.kind -eq 'operation' -and $null -ne $approvalPolicy) {
				$result = Invoke-ApprovedScenarioOperation `
					-Definition $definition `
					-Arguments $arguments `
					-ScenarioPhase ([string]$Scenario.phase) `
					-RunRoot $RunRoot `
					-ApprovalRegistry $ApprovalRegistry `
					-TimeoutSeconds $ApprovalTimeoutSeconds
				if ([int]$result.mcpSequence -ne $expectedMcpSequence) {
					throw "Approved scenario item '$($definition.id)' changed its initial MCP request sequence."
				}
				$approvalEvidence = $result.approval
			}
			else {
				$result = Invoke-McpTool -Name ([string]$definition.tool) -Arguments $arguments
				if ($script:McpSequence -ne $expectedMcpSequence) {
					throw "Scenario item '$($definition.id)' did not use exactly one MCP sequence."
				}
			}
			if ($null -ne $transitionState) {
				$transitionEvidence = Complete-ScenarioFileTransition -State $transitionState -Transition $transition
				$transitionState = $null
			}
		}
		finally {
			if ($null -ne $transitionState) { Restore-ScenarioFileTransition -State $transitionState }
		}
		$expectation = Get-OptionalProperty -Object $definition -Name 'expect'
		Assert-OperationExpectation -Envelope $result.envelope -Expectation $expectation -Id ([string]$definition.id)
		$expectedRecord = New-ScenarioExpectationRecord -Expectation $expectation
		$envelopeJson = $result.envelope | ConvertTo-Json -Depth 80 -Compress
		$envelopeHash = Get-StringSha256 -Value $envelopeJson
		$operationEvidenceReference = $null
		if ($item.kind -eq 'operation') {
			$operationEvidenceRelative = [string]$definition.evidencePath
			$operationEvidencePath = Get-AbsolutePath -Path $operationEvidenceRelative -BasePath $RunRoot
			Assert-PathBelow -Parent $RunRoot -Child $operationEvidencePath -Description "Scenario operation '$($definition.id)' evidence"
			Write-JsonFile -Path $operationEvidencePath -Value ([ordered]@{
				schemaVersion = 2
				phase = [string]$Scenario.phase
				id = [string]$definition.id
				stage = $Stage
				order = [int]$definition.order
				tool = [string]$definition.tool
				mcpSequence = $expectedMcpSequence
				arguments = $arguments
				resolvedArguments = $arguments
				resolvedArgumentsSha256 = [string]$bindingResolution.argumentsSha256
				argumentBindings = @($bindingResolution.bindings)
				expected = $expectedRecord
				expectationPassed = $true
				envelopeSha256 = $envelopeHash
				envelope = $result.envelope
				approval = $approvalEvidence
				fileTransition = $transitionEvidence
			})
			$operationEvidenceReference = [ordered]@{
				path = $operationEvidenceRelative.Replace('\', '/')
				sha256 = Get-Sha256 -Path $operationEvidencePath
			}
		}
		$OperationInventory.Add([ordered]@{
			id = [string]$definition.id
			kind = [string]$item.kind
			stage = $Stage
			order = [int]$definition.order
			tool = [string]$definition.tool
			mcpSequence = $expectedMcpSequence
			resolvedArguments = $arguments
			resolvedArgumentsSha256 = [string]$bindingResolution.argumentsSha256
			argumentBindings = @($bindingResolution.bindings)
			expectationPassed = $true
			expected = $expectedRecord
			observed = [ordered]@{
				ok = [bool]$result.envelope.ok
				status = Get-NestedValue -Object $result.envelope -Path 'status'
				errorCategory = Get-NestedValue -Object $result.envelope -Path 'error.category'
				envelopeSha256 = $envelopeHash
			}
			evidence = $operationEvidenceReference
			approval = $approvalEvidence
			fileTransition = $transitionEvidence
		})
		if ($item.kind -eq 'operation') {
			# Publish only after the expectation passed and the operation artifact
			# was durably written and hashed above.
			$operationOk = Get-RequiredJsonProperty -Object $result.envelope -Name 'ok' -Description "Scenario operation '$($definition.id)' envelope"
			if ($operationOk -is [bool] -and [bool]$operationOk) {
				$BindingRegistry[[string]$definition.id] = [ordered]@{
					envelope = $result.envelope
					mcpSequence = $expectedMcpSequence
					envelopeSha256 = $envelopeHash
					evidenceSha256 = [string]$operationEvidenceReference.sha256
					approval = $approvalEvidence
				}
			}
		}
		if ($item.kind -eq 'capture') { $CameraInventory.Add((Save-CameraCapture -Capture $definition -ToolResult $result -CameraRoot $CameraRoot -RunRoot $RunRoot)) }
	}
}

function ConvertTo-ExtendedLengthPath {
	param([Parameter(Mandatory = $true)][string]$Path)
	$fullPath = [System.IO.Path]::GetFullPath($Path)
	if ([System.IO.Path]::DirectorySeparatorChar -ne '\' -or
		$fullPath.StartsWith('\\?\', [System.StringComparison]::Ordinal)) {
		return $fullPath
	}
	if ($fullPath.StartsWith('\\', [System.StringComparison]::Ordinal)) {
		return '\\?\UNC\' + $fullPath.Substring(2)
	}
	return '\\?\' + $fullPath
}

function Get-LongPathFileState {
	param(
		[Parameter(Mandatory = $true)][string]$Path,
		[UInt64]$MaximumBytes = [UInt64]::MaxValue
	)
	$extendedPath = ConvertTo-ExtendedLengthPath -Path $Path
	$attributes = [System.IO.File]::GetAttributes($extendedPath)
	if (($attributes -band [System.IO.FileAttributes]::Directory) -ne 0) {
		throw "Expected a file but found a directory: '$Path'."
	}
	if (($attributes -band [System.IO.FileAttributes]::ReparsePoint) -ne 0) {
		throw "Authored and recording-output files may not be symbolic links or reparse points: '$Path'."
	}
	$stream = [System.IO.File]::Open(
		$extendedPath,
		[System.IO.FileMode]::Open,
		[System.IO.FileAccess]::Read,
		[System.IO.FileShare]::Read
	)
	$algorithm = [System.Security.Cryptography.SHA256]::Create()
	try {
		$bytes = [UInt64]$stream.Length
		if ($bytes -gt $MaximumBytes) {
			throw "File '$Path' exceeds the bounded maximum of $MaximumBytes bytes."
		}
		$hash = ([System.BitConverter]::ToString($algorithm.ComputeHash($stream))).Replace('-', '').ToLowerInvariant()
		return [ordered]@{ bytes = $bytes; sha256 = $hash }
	}
	finally {
		$algorithm.Dispose()
		$stream.Dispose()
	}
}

function Get-PlayModeSnapshotDigest {
	param([Parameter(Mandatory = $true)]$Snapshot)
	$core = [ordered]@{
		excludedTopLevelDirectories = @($Snapshot.excludedTopLevelDirectories)
		excludedTransientState = $Snapshot.excludedTransientState
		limits = $Snapshot.limits
		fileCount = [int]$Snapshot.fileCount
		directoryCount = [int]$Snapshot.directoryCount
		totalBytes = [UInt64]$Snapshot.totalBytes
		filesSha256 = [string]$Snapshot.filesSha256
		directoriesSha256 = [string]$Snapshot.directoriesSha256
		files = @($Snapshot.files)
		directories = @($Snapshot.directories)
	}
	return Get-CanonicalJsonSha256 -Value $core
}

function Get-PlayModeProjectSnapshot {
	param([Parameter(Mandatory = $true)][string]$ProjectRoot)
	$rootPath = [System.IO.Path]::GetFullPath($ProjectRoot).TrimEnd('\', '/')
	$extendedRoot = ConvertTo-ExtendedLengthPath -Path $rootPath
	if (-not [System.IO.Directory]::Exists($extendedRoot)) {
		throw "Play Mode authored-state project root does not exist: '$rootPath'."
	}
	$rootAttributes = [System.IO.File]::GetAttributes($extendedRoot)
	if (($rootAttributes -band [System.IO.FileAttributes]::ReparsePoint) -ne 0) {
		throw 'Play Mode authored-state project root may not be a symbolic link or reparse point.'
	}

	$files = [System.Collections.Generic.List[object]]::new()
	$directories = [System.Collections.Generic.List[string]]::new()
	$pending = [System.Collections.Generic.Stack[object]]::new()
	$pending.Push([ordered]@{ absolute = $extendedRoot; relative = '' })
	$totalBytes = [UInt64]0
	while ($pending.Count -gt 0) {
		$current = $pending.Pop()
		$entries = [string[]]@([System.IO.Directory]::EnumerateFileSystemEntries([string]$current.absolute))
		[System.Array]::Sort($entries, [System.StringComparer]::OrdinalIgnoreCase)
		foreach ($entry in $entries) {
			$trimmedEntry = $entry.TrimEnd([char[]]@('\', '/'))
			$name = [System.IO.Path]::GetFileName($trimmedEntry)
			if ([string]::IsNullOrWhiteSpace($name)) {
				throw "Play Mode authored-state enumeration returned an unnamed entry below '$([string]$current.relative)'."
			}
			$relative = if ([string]::IsNullOrEmpty([string]$current.relative)) { $name } else { "$([string]$current.relative)/$name" }
			if ($relative.Length -gt 32760) {
				throw 'Play Mode authored-state inventory encountered a path beyond the supported extended-length bound.'
			}
			$attributes = [System.IO.File]::GetAttributes($entry)
			if (($attributes -band [System.IO.FileAttributes]::ReparsePoint) -ne 0) {
				throw "Play Mode authored-state inventory rejects symbolic links and reparse points: '$relative'."
			}
			$isDirectory = ($attributes -band [System.IO.FileAttributes]::Directory) -ne 0
			$isTopLevel = [string]::IsNullOrEmpty([string]$current.relative)
			if ($isDirectory -and $isTopLevel -and $script:PlayModeAuthoredExcludedTopLevelDirectories -contains $name) {
				continue
			}
			if ($isDirectory) {
				$directories.Add($relative)
				if ($directories.Count -gt $script:MaximumPlayModeSnapshotDirectories) {
					throw "Play Mode authored-state inventory exceeds $($script:MaximumPlayModeSnapshotDirectories) directories."
				}
				$pending.Push([ordered]@{ absolute = $entry; relative = $relative })
				continue
			}
			$fileState = Get-LongPathFileState -Path $entry
			if ([UInt64]$fileState.bytes -gt $script:MaximumPlayModeSnapshotBytes -or
				$totalBytes -gt ($script:MaximumPlayModeSnapshotBytes - [UInt64]$fileState.bytes)) {
				throw "Play Mode authored-state inventory exceeds $($script:MaximumPlayModeSnapshotBytes) bytes."
			}
			$totalBytes += [UInt64]$fileState.bytes
			$files.Add([pscustomobject][ordered]@{
				path = $relative
				bytes = [UInt64]$fileState.bytes
				sha256 = [string]$fileState.sha256
			})
			if ($files.Count -gt $script:MaximumPlayModeSnapshotFiles) {
				throw "Play Mode authored-state inventory exceeds $($script:MaximumPlayModeSnapshotFiles) files."
			}
		}
	}
	$fileByPath = [System.Collections.Generic.Dictionary[string, object]]::new([System.StringComparer]::Ordinal)
	foreach ($file in $files) { $fileByPath[[string]$file.path] = $file }
	$sortedFilePaths = Get-OrdinalSortedStrings -Values @($fileByPath.Keys)
	$sortedFiles = @($sortedFilePaths | ForEach-Object { $fileByPath[[string]$_] })
	$sortedDirectories = @(Get-OrdinalSortedStrings -Values @($directories))
	$directoryDigest = Get-StringSha256 -Value ((@($sortedDirectories) -join "`n") + "`n")
	$snapshot = [ordered]@{
		excludedTopLevelDirectories = @($script:PlayModeAuthoredExcludedTopLevelDirectories)
		excludedTransientState = [ordered]@{
			path = 'UserSettings'
			reason = 'Per-project editor layout and preferences are transient UI state, not authored restoration state.'
		}
		limits = [ordered]@{
			maximumFiles = $script:MaximumPlayModeSnapshotFiles
			maximumDirectories = $script:MaximumPlayModeSnapshotDirectories
			maximumBytes = $script:MaximumPlayModeSnapshotBytes
		}
		fileCount = $sortedFiles.Count
		directoryCount = $sortedDirectories.Count
		totalBytes = $totalBytes
		filesSha256 = Get-InventoryDigest -Inventory $sortedFiles
		directoriesSha256 = $directoryDigest
		files = $sortedFiles
		directories = $sortedDirectories
	}
	$snapshot['sha256'] = Get-PlayModeSnapshotDigest -Snapshot $snapshot
	return $snapshot
}

function Start-PlayModeRecordingOutputContract {
	param(
		[Parameter(Mandatory = $true)]$Scenario,
		[Parameter(Mandatory = $true)][string]$ProjectRoot,
		[Parameter(Mandatory = $true)]$BeforeSnapshot
	)
	$preflight = [System.Collections.Generic.List[object]]::new()
	foreach ($declaration in @($Scenario.playModeRecordingOutputs)) {
		$relativePath = [string]$declaration.path
		$absolutePath = Get-AbsolutePath -Path $relativePath -BasePath $ProjectRoot
		Assert-PathBelow -Parent $ProjectRoot -Child $absolutePath -Description 'Play Mode recording output'
		$parentPath = [System.IO.Path]::GetFullPath((Split-Path -Parent $absolutePath))
		$extendedParent = ConvertTo-ExtendedLengthPath -Path $parentPath
		if (-not [System.IO.Directory]::Exists($extendedParent)) {
			throw "Play Mode recording output '$relativePath' requires the existing Assets/Captured parent directory."
		}
		$parentAttributes = [System.IO.File]::GetAttributes($extendedParent)
		if (($parentAttributes -band [System.IO.FileAttributes]::ReparsePoint) -ne 0) {
			throw "Play Mode recording output '$relativePath' may not traverse a reparse-point parent."
		}
		if (@($BeforeSnapshot.directories) -cnotcontains 'Assets' -or
			@($BeforeSnapshot.directories) -cnotcontains 'Assets/Captured') {
			throw "Play Mode recording output '$relativePath' requires exact-case existing Assets/Captured directories in the authored snapshot."
		}
		$extendedOutput = ConvertTo-ExtendedLengthPath -Path $absolutePath
		if ([System.IO.File]::Exists($extendedOutput) -or [System.IO.Directory]::Exists($extendedOutput) -or
			@($BeforeSnapshot.files | Where-Object { [string]$_.path -ieq $relativePath }).Count -gt 0) {
			throw "Play Mode recording output '$relativePath' already exists; recording proof is creation-only."
		}
		$preflight.Add([ordered]@{
			path = $relativePath
			producerOperationId = [string]$declaration.producerOperationId
			producerResponsePath = [string]$declaration.producerResponsePath
			parent = 'Assets/Captured'
			parentExisted = $true
			outputExisted = $false
		})
	}
	return @($preflight)
}

function Compare-PlayModeProjectSnapshots {
	param(
		[Parameter(Mandatory = $true)]$Before,
		[Parameter(Mandatory = $true)]$After,
		[Parameter(Mandatory = $true)]$Declarations
	)
	$beforeFiles = [System.Collections.Generic.Dictionary[string, object]]::new([System.StringComparer]::Ordinal)
	$afterFiles = [System.Collections.Generic.Dictionary[string, object]]::new([System.StringComparer]::Ordinal)
	foreach ($record in @($Before.files)) { $beforeFiles[[string]$record.path] = $record }
	foreach ($record in @($After.files)) { $afterFiles[[string]$record.path] = $record }
	foreach ($path in $beforeFiles.Keys) {
		if (-not $afterFiles.ContainsKey($path)) {
			throw "Play Mode deleted or renamed original authored file '$path'."
		}
		$beforeRecord = $beforeFiles[$path]
		$afterRecord = $afterFiles[$path]
		if ([UInt64]$beforeRecord.bytes -ne [UInt64]$afterRecord.bytes -or
			[string]$beforeRecord.sha256 -cne [string]$afterRecord.sha256) {
			throw "Play Mode mutated original authored file '$path'."
		}
	}
	if ((Get-CanonicalJsonText -Value @($Before.directories)) -cne
		(Get-CanonicalJsonText -Value @($After.directories))) {
		throw 'Play Mode added, deleted, renamed, or case-changed an authored directory.'
	}
	$addedPaths = @(Get-OrdinalSortedStrings -Values @($afterFiles.Keys | Where-Object { -not $beforeFiles.ContainsKey($_) }))
	$expectedPaths = @(Get-OrdinalSortedStrings -Values @($Declarations | ForEach-Object { [string]$_.path }))
	if ((Get-CanonicalJsonText -Value $addedPaths) -cne (Get-CanonicalJsonText -Value $expectedPaths)) {
		throw "Play Mode file additions do not exactly match declared recording outputs. Expected [$($expectedPaths -join ', ')], observed [$($addedPaths -join ', ')]."
	}
	$outputs = [System.Collections.Generic.List[object]]::new()
	foreach ($declaration in @($Declarations)) {
		$path = [string]$declaration.path
		if (-not $afterFiles.ContainsKey($path)) {
			throw "Declared Play Mode recording output '$path' was not created."
		}
		$record = $afterFiles[$path]
		if ([UInt64]$record.bytes -lt 1) {
			throw "Declared Play Mode recording output '$path' is empty."
		}
		if ([UInt64]$record.bytes -gt $script:MaximumPlayModeRecordingOutputBytes) {
			throw "Declared Play Mode recording output '$path' exceeds $($script:MaximumPlayModeRecordingOutputBytes) bytes."
		}
		$outputs.Add([ordered]@{
			path = $path
			bytes = [UInt64]$record.bytes
			sha256 = [string]$record.sha256
		})
	}
	return [ordered]@{
		passed = $true
		originalFilesUnchanged = $true
		directoriesUnchanged = $true
		creationOnly = $true
		addedFiles = $addedPaths
		outputs = @($outputs)
	}
}

function Complete-PlayModeRecordingOutputContract {
	param(
		[Parameter(Mandatory = $true)]$Scenario,
		[Parameter(Mandatory = $true)][string]$ProjectRoot,
		[Parameter(Mandatory = $true)]$BeforeSnapshot,
		[Parameter(Mandatory = $true)]$AfterSnapshot,
		[Parameter(Mandatory = $true)]$BindingRegistry,
		[Parameter(Mandatory = $true)]$OperationInventory,
		[Parameter(Mandatory = $true)][int]$PlayEnterSequence,
		[Parameter(Mandatory = $true)][int]$PlayExitSequence
	)
	$comparison = Compare-PlayModeProjectSnapshots -Before $BeforeSnapshot -After $AfterSnapshot -Declarations @($Scenario.playModeRecordingOutputs)
	$proofs = [System.Collections.Generic.List[object]]::new()
	foreach ($declaration in @($Scenario.playModeRecordingOutputs)) {
		$producerId = [string]$declaration.producerOperationId
		if (-not $BindingRegistry.ContainsKey($producerId)) {
			throw "Recording producer '$producerId' did not complete successful expectation and artifact publication."
		}
		$producerState = $BindingRegistry[$producerId]
		$producerRecords = @($OperationInventory | Where-Object {
			[string]$_.id -ceq $producerId -and [string]$_.kind -ceq 'operation'
		})
		if ($producerRecords.Count -ne 1) {
			throw "Recording producer '$producerId' does not have exactly one operation evidence record."
		}
		$producerRecord = $producerRecords[0]
		$producerSequence = [int]$producerRecord.mcpSequence
		# Live operation inventories also use ordered dictionaries. Preserve their
		# approval object instead of silently treating the dictionary key as absent.
		$producerApproval = if ($producerRecord -is [System.Collections.IDictionary] -and $producerRecord.Contains('approval')) {
			$producerRecord['approval']
		}
		else {
			Get-OptionalProperty -Object $producerRecord -Name 'approval'
		}
		$producerCompletionSequence = if ($null -eq $producerApproval) {
			$producerSequence
		}
		elseif ($producerApproval -is [System.Collections.IDictionary] -and $producerApproval.Contains('completionMcpSequence')) {
			[int]$producerApproval['completionMcpSequence']
		}
		else {
			[int](Get-RequiredJsonProperty -Object $producerApproval -Name 'completionMcpSequence' -Description "Recording producer '$producerId' approval")
		}
		# Live binding registries store ordered dictionaries while evidence
		# reconstruction uses parsed JSON objects. Read the optional approval from
		# either representation without treating a dictionary key as a PS property.
		$publishedApproval = if ($producerState -is [System.Collections.IDictionary] -and $producerState.Contains('approval')) {
			$producerState['approval']
		}
		else {
			Get-OptionalProperty -Object $producerState -Name 'approval'
		}
		$publishedApprovalText = Get-CanonicalJsonText -Value $publishedApproval
		$producerApprovalText = Get-CanonicalJsonText -Value $producerApproval
		$approvalProvenanceMatches = $publishedApprovalText -ceq $producerApprovalText
		if ($producerSequence -le $PlayEnterSequence -or $producerSequence -ge $PlayExitSequence -or
			$producerCompletionSequence -lt $producerSequence -or $producerCompletionSequence -ge $PlayExitSequence -or
			[int]$producerState.mcpSequence -ne $producerSequence -or
			-not $approvalProvenanceMatches) {
			throw "Recording producer '$producerId' MCP sequence is not strictly between play.enter and play.exit or its approval provenance changed: enter=$PlayEnterSequence, initial=$producerSequence, completion=$producerCompletionSequence, published=$([int]$producerState.mcpSequence), exit=$PlayExitSequence, approvalMatched=$approvalProvenanceMatches."
		}
		if ([string]$producerRecord.stage -cne 'play' -or
			[string]$producerRecord.tool -cne 'animation.recording.stop' -or
			-not [bool]$producerRecord.expectationPassed) {
			throw "Recording producer '$producerId' is not a successful play-stage animation.recording.stop record."
		}
		$selectedPath = Get-CaseSensitiveJsonPathValue -Object $producerState.envelope -Path ([string]$declaration.producerResponsePath)
		if (-not [bool]$selectedPath.found -or $selectedPath.value -isnot [string] -or
			[string]::IsNullOrWhiteSpace([string]$selectedPath.value) -or
			-not [System.IO.Path]::IsPathRooted([string]$selectedPath.value)) {
			throw "Recording producer '$producerId' did not return an absolute path at '$($declaration.producerResponsePath)'."
		}
		$producerAbsolute = [System.IO.Path]::GetFullPath([string]$selectedPath.value)
		Assert-PathBelow -Parent $ProjectRoot -Child $producerAbsolute -Description "Recording producer '$producerId' destination"
		$producerRelative = Get-RelativeForwardPath -Parent $ProjectRoot -Child $producerAbsolute
		$declaredPath = [string]$declaration.path
		$expectedDeclaredPath = if ($declaredPath.EndsWith('.teanim.meta', [System.StringComparison]::Ordinal)) {
			$producerRelative + '.meta'
		}
		else {
			$producerRelative
		}
		if ($expectedDeclaredPath -cne $declaredPath) {
			throw "Recording producer '$producerId' destination '$producerRelative' does not exactly match declared '$declaredPath' or its exact declared metadata sidecar."
		}
		$outputState = @($comparison.outputs | Where-Object { [string]$_.path -ceq $declaredPath })
		if ($outputState.Count -ne 1) {
			throw "Recording output '$($declaration.path)' is missing its creation-only snapshot proof."
		}
		$artifactReference = if ($producerRecord -is [System.Collections.IDictionary] -and $producerRecord.Contains('evidence')) {
			$producerRecord['evidence']
		}
		else {
			Get-RequiredJsonProperty -Object $producerRecord -Name 'evidence' -Description "Recording producer '$producerId' operation evidence"
		}
		if ([string]$artifactReference.sha256 -cne [string]$producerState.evidenceSha256) {
			throw "Recording producer '$producerId' artifact hash does not match its successful in-memory publication."
		}
		$proofs.Add([ordered]@{
			path = $declaredPath
			bytes = [UInt64]$outputState[0].bytes
			sha256 = [string]$outputState[0].sha256
			producerOperationId = $producerId
			producerResponsePath = [string]$declaration.producerResponsePath
			producerMcpSequence = $producerSequence
			producerCompletionMcpSequence = $producerCompletionSequence
			producerEnvelopeSha256 = [string]$producerState.envelopeSha256
			producerApproval = $producerApproval
			producerArtifact = [ordered]@{
				path = [string]$artifactReference.path
				sha256 = [string]$artifactReference.sha256
			}
		})
	}
	return [ordered]@{
		comparison = $comparison
		outputs = @($proofs)
	}
}

function Get-AuthoredProjectState {
	param([Parameter(Mandatory = $true)][string]$ProjectRoot)
	# Include directory identity as well as file content. This catches empty
	# directory creation, case-only renames, and deletions that a file-only digest
	# could not observe while explicitly excluding transient UserSettings state.
	return Get-PlayModeProjectSnapshot -ProjectRoot $ProjectRoot
}

function Get-StableAuthoredProjectState {
	param(
		[Parameter(Mandatory = $true)][string]$ProjectRoot,
		[ValidateRange(2, 10)][int]$RequiredConsecutiveObservations = 3,
		[ValidateRange(25, 2000)][int]$SampleIntervalMilliseconds = 250,
		[ValidateRange(1, 60)][int]$TimeoutSeconds = 15
	)
	# scene.save can trigger an asynchronous AssetDatabase metadata rewrite. A
	# single inventory may otherwise hash the old or partially rewritten sidecar
	# and falsely attribute its completion to Play Mode. Require the complete
	# authored inventory to remain identical across several bounded observations.
	$samples = [System.Collections.Generic.List[object]]::new()
	$deadline = [DateTimeOffset]::UtcNow.AddSeconds($TimeoutSeconds)
	$previousSha256 = ''
	$consecutiveObservations = 0
	while ([DateTimeOffset]::UtcNow -lt $deadline) {
		try {
			$candidate = Get-AuthoredProjectState -ProjectRoot $ProjectRoot
			$candidateSha256 = [string]$candidate.sha256
			$samples.Add([ordered]@{
				observedAtUtc = [DateTimeOffset]::UtcNow.ToString('o')
				succeeded = $true
				sha256 = $candidateSha256
				fileCount = [int]$candidate.fileCount
				directoryCount = [int]$candidate.directoryCount
				totalBytes = [UInt64]$candidate.totalBytes
			})
			if ($candidateSha256 -ceq $previousSha256) {
				++$consecutiveObservations
			}
			else {
				$previousSha256 = $candidateSha256
				$consecutiveObservations = 1
			}
			if ($consecutiveObservations -ge $RequiredConsecutiveObservations) {
				return [ordered]@{
					snapshot = $candidate
					stabilizedAtUtc = [DateTimeOffset]::UtcNow.ToString('o')
					requiredConsecutiveObservations = $RequiredConsecutiveObservations
					sampleIntervalMilliseconds = $SampleIntervalMilliseconds
					timeoutSeconds = $TimeoutSeconds
					samples = @($samples)
				}
			}
		}
		catch [System.IO.IOException] {
			# An AssetDatabase writer may briefly hold a sidecar while replacing
			# it. Record the transient read failure and retry inside the same
			# bounded deadline instead of accepting or hiding an unstable state.
			$previousSha256 = ''
			$consecutiveObservations = 0
			$samples.Add([ordered]@{
				observedAtUtc = [DateTimeOffset]::UtcNow.ToString('o')
				succeeded = $false
				error = $_.Exception.Message
			})
		}
		Start-Sleep -Milliseconds $SampleIntervalMilliseconds
	}
	$observedHashes = @($samples | Where-Object { [bool]$_.succeeded } | ForEach-Object { [string]$_.sha256 })
	throw "Authored project state did not remain stable for $RequiredConsecutiveObservations observations within $TimeoutSeconds seconds. Observed hashes: [$($observedHashes -join ', ')]."
}

function Assert-PlayModeSnapshotRelativePath {
	param(
		[Parameter(Mandatory = $true)][AllowEmptyString()][string]$Path,
		[Parameter(Mandatory = $true)][string]$Description
	)
	if ([string]::IsNullOrWhiteSpace($Path) -or $Path -cne $Path.Trim() -or
		$Path.Contains('\') -or [System.IO.Path]::IsPathRooted($Path) -or
		$Path.IndexOf([char]0) -ge 0) {
		throw "$Description '$Path' is not a safe project-relative forward path."
	}
	$segments = @($Path.Split([char]'/'))
	if (@($segments | Where-Object { $_ -in @('', '.', '..') }).Count -gt 0) {
		throw "$Description '$Path' contains an empty, current, or parent segment."
	}
	$invalidCharacters = [System.IO.Path]::GetInvalidFileNameChars()
	foreach ($segment in $segments) {
		if ($segment.IndexOfAny($invalidCharacters) -ge 0 -or
			$segment.EndsWith('.', [System.StringComparison]::Ordinal) -or
			$segment.EndsWith(' ', [System.StringComparison]::Ordinal) -or
			$segment -match '^(?i:con|prn|aux|nul|com[1-9]|lpt[1-9])(?:\..*)?$') {
			throw "$Description '$Path' contains an unsafe segment '$segment'."
		}
	}
	if ($script:PlayModeAuthoredExcludedTopLevelDirectories -contains $segments[0]) {
		throw "$Description '$Path' enters excluded transient top-level state."
	}
	return $Path
}

function Assert-PlayModeSnapshotIntegrity {
	param(
		[Parameter(Mandatory = $true)]$Snapshot,
		[Parameter(Mandatory = $true)]$SnapshotStrict,
		[Parameter(Mandatory = $true)][string]$Description
	)
	if ($SnapshotStrict -isnot [System.Collections.IDictionary]) {
		throw "$Description snapshot must preserve one JSON object."
	}
	Assert-ExactPreservedJsonObjectKeys -Object $SnapshotStrict -RequiredKeys @(
		'excludedTopLevelDirectories', 'excludedTransientState', 'limits',
		'fileCount', 'directoryCount', 'totalBytes', 'filesSha256',
		'directoriesSha256', 'files', 'directories', 'sha256'
	) -Description "$Description snapshot"
	$filesStrict = Get-RequiredPreservedJsonMember -Object $SnapshotStrict -Name 'files' -Description "$Description snapshot"
	$directoriesStrict = Get-RequiredPreservedJsonMember -Object $SnapshotStrict -Name 'directories' -Description "$Description snapshot"
	$excludedStrict = Get-RequiredPreservedJsonMember -Object $SnapshotStrict -Name 'excludedTopLevelDirectories' -Description "$Description snapshot"
	$transientStrict = Get-RequiredPreservedJsonMember -Object $SnapshotStrict -Name 'excludedTransientState' -Description "$Description snapshot"
	$limitsStrict = Get-RequiredPreservedJsonMember -Object $SnapshotStrict -Name 'limits' -Description "$Description snapshot"
	if (-not (Test-IsJsonArray -Value $filesStrict) -or
		-not (Test-IsJsonArray -Value $directoriesStrict) -or
		-not (Test-IsJsonArray -Value $excludedStrict) -or
		@($excludedStrict | Where-Object { $_ -isnot [string] }).Count -gt 0) {
		throw "$Description snapshot files, directories, and exclusions must be JSON arrays."
	}
	if ((Get-CanonicalJsonText -Value @($Snapshot.excludedTopLevelDirectories)) -cne
		(Get-CanonicalJsonText -Value @($script:PlayModeAuthoredExcludedTopLevelDirectories))) {
		throw "$Description snapshot changed the authored-state exclusion policy."
	}
	Assert-ExactPreservedJsonObjectKeys -Object $transientStrict -RequiredKeys @('path', 'reason') -Description "$Description snapshot excludedTransientState"
	$transientState = Get-RequiredJsonProperty -Object $Snapshot -Name 'excludedTransientState' -Description "$Description snapshot"
	if ($transientStrict['path'] -isnot [string] -or $transientStrict['reason'] -isnot [string] -or
		[string]$transientState.path -cne 'UserSettings' -or
		[string]$transientState.reason -cne 'Per-project editor layout and preferences are transient UI state, not authored restoration state.') {
		throw "$Description snapshot does not truthfully identify excluded UserSettings transient state."
	}
	Assert-ExactPreservedJsonObjectKeys -Object $limitsStrict -RequiredKeys @(
		'maximumFiles', 'maximumDirectories', 'maximumBytes'
	) -Description "$Description snapshot limits"
	$limits = Get-RequiredJsonProperty -Object $Snapshot -Name 'limits' -Description "$Description snapshot"
	foreach ($limitName in @('maximumFiles', 'maximumDirectories', 'maximumBytes')) {
		$strictLimit = $limitsStrict[$limitName]
		if ($strictLimit -isnot [int] -and $strictLimit -isnot [long]) {
			throw "$Description snapshot limit '$limitName' must be a JSON integer."
		}
	}
	if ([int]$limits.maximumFiles -ne $script:MaximumPlayModeSnapshotFiles -or
		[int]$limits.maximumDirectories -ne $script:MaximumPlayModeSnapshotDirectories -or
		[UInt64]$limits.maximumBytes -ne $script:MaximumPlayModeSnapshotBytes) {
		throw "$Description snapshot does not preserve the bounded inventory limits."
	}
	$strictFileCount = Get-RequiredPreservedJsonMember -Object $SnapshotStrict -Name 'fileCount' -Description "$Description snapshot"
	$strictDirectoryCount = Get-RequiredPreservedJsonMember -Object $SnapshotStrict -Name 'directoryCount' -Description "$Description snapshot"
	$strictTotalBytes = Get-RequiredPreservedJsonMember -Object $SnapshotStrict -Name 'totalBytes' -Description "$Description snapshot"
	foreach ($countValue in @($strictFileCount, $strictDirectoryCount, $strictTotalBytes)) {
		if ($countValue -isnot [int] -and $countValue -isnot [long]) {
			throw "$Description snapshot counts and totalBytes must be JSON integers."
		}
	}
	if ([int64]$strictFileCount -lt 0 -or [int64]$strictDirectoryCount -lt 0 -or [int64]$strictTotalBytes -lt 0) {
		throw "$Description snapshot counts and totalBytes may not be negative."
	}
	foreach ($digestName in @('filesSha256', 'directoriesSha256', 'sha256')) {
		$strictDigest = Get-RequiredPreservedJsonMember -Object $SnapshotStrict -Name $digestName -Description "$Description snapshot"
		if ($strictDigest -isnot [string] -or [string]$strictDigest -notmatch '^[0-9a-f]{64}$') {
			throw "$Description snapshot '$digestName' must be a lowercase SHA-256 digest."
		}
	}
	$files = @(Get-RequiredJsonProperty -Object $Snapshot -Name 'files' -Description "$Description snapshot")
	$directories = @(Get-RequiredJsonProperty -Object $Snapshot -Name 'directories' -Description "$Description snapshot")
	if ($files.Count -ne [int]$strictFileCount -or
		$directories.Count -ne [int]$strictDirectoryCount -or
		$files.Count -gt $script:MaximumPlayModeSnapshotFiles -or
		$directories.Count -gt $script:MaximumPlayModeSnapshotDirectories) {
		throw "$Description snapshot count metadata is invalid."
	}
	$filePaths = [System.Collections.Generic.HashSet[string]]::new([System.StringComparer]::OrdinalIgnoreCase)
	$totalBytes = [UInt64]0
	$previousFilePath = $null
	for ($fileIndex = 0; $fileIndex -lt $files.Count; ++$fileIndex) {
		$strictFile = @($filesStrict)[$fileIndex]
		$file = $files[$fileIndex]
		$requiredFileKeys = @('path', 'bytes', 'sha256')
		if ($strictFile -isnot [System.Collections.IDictionary] -or
			@($strictFile.Keys).Count -ne 3 -or
			@($requiredFileKeys | Where-Object { @($strictFile.Keys) -cnotcontains $_ }).Count -gt 0) {
			throw "$Description snapshot file $($fileIndex + 1) must contain exactly path, bytes, and sha256."
		}
		$path = Assert-PlayModeSnapshotRelativePath -Path ([string]$strictFile['path']) -Description "$Description snapshot file"
		$strictBytes = $strictFile['bytes']
		if (($strictBytes -isnot [int] -and $strictBytes -isnot [long]) -or [int64]$strictBytes -lt 0 -or
			[string]$strictFile['sha256'] -notmatch '^[0-9a-f]{64}$' -or
			-not $filePaths.Add($path)) {
			throw "$Description snapshot file '$path' has invalid bytes, digest, or an aliased duplicate path."
		}
		if ($null -ne $previousFilePath -and
			[System.StringComparer]::Ordinal.Compare($previousFilePath, $path) -ge 0) {
			throw "$Description snapshot files are not in strict ordinal path order."
		}
		$previousFilePath = $path
		if ([UInt64]$strictBytes -gt $script:MaximumPlayModeSnapshotBytes -or
			$totalBytes -gt ($script:MaximumPlayModeSnapshotBytes - [UInt64]$strictBytes)) {
			throw "$Description snapshot exceeds its byte bound."
		}
		$totalBytes += [UInt64]$strictBytes
	}
	$directoryPaths = [System.Collections.Generic.HashSet[string]]::new([System.StringComparer]::OrdinalIgnoreCase)
	$previousDirectoryPath = $null
	for ($directoryIndex = 0; $directoryIndex -lt $directories.Count; ++$directoryIndex) {
		$strictDirectory = @($directoriesStrict)[$directoryIndex]
		if ($strictDirectory -isnot [string]) {
			throw "$Description snapshot directory $($directoryIndex + 1) must be a JSON string."
		}
		$directory = Assert-PlayModeSnapshotRelativePath -Path ([string]$strictDirectory) -Description "$Description snapshot directory"
		if (-not $directoryPaths.Add($directory) -or
			($null -ne $previousDirectoryPath -and [System.StringComparer]::Ordinal.Compare($previousDirectoryPath, $directory) -ge 0)) {
			throw "$Description snapshot directories contain an aliased duplicate or are not in strict ordinal order."
		}
		$previousDirectoryPath = $directory
	}
	foreach ($directory in $directoryPaths) {
		$separator = $directory.LastIndexOf([char]'/')
		if ($separator -gt 0) {
			$parent = $directory.Substring(0, $separator)
			if (-not $directoryPaths.Contains($parent)) {
				throw "$Description snapshot directory '$directory' is missing parent directory '$parent'."
			}
		}
		if ($filePaths.Contains($directory)) {
			throw "$Description snapshot path '$directory' aliases both a file and a directory."
		}
	}
	foreach ($filePath in $filePaths) {
		$separator = $filePath.LastIndexOf([char]'/')
		if ($separator -gt 0) {
			$parent = $filePath.Substring(0, $separator)
			if (-not $directoryPaths.Contains($parent)) {
				throw "$Description snapshot file '$filePath' is missing parent directory '$parent'."
			}
		}
	}
	if ($totalBytes -ne [UInt64]$strictTotalBytes -or
		(Get-InventoryDigest -Inventory $files) -cne [string]$Snapshot.filesSha256 -or
		(Get-StringSha256 -Value ((@($directories) -join "`n") + "`n")) -cne [string]$Snapshot.directoriesSha256 -or
		(Get-PlayModeSnapshotDigest -Snapshot $Snapshot) -cne [string]$Snapshot.sha256) {
		throw "$Description snapshot digest metadata does not match its exact inventory."
	}
	return $true
}

function Read-PlayModeSnapshotEvidence {
	param(
		[Parameter(Mandatory = $true)]$Reference,
		[Parameter(Mandatory = $true)]$ReferenceStrict,
		[Parameter(Mandatory = $true)][string]$ExpectedPath,
		[Parameter(Mandatory = $true)][ValidateSet('before-play', 'after-play')][string]$ExpectedKind,
		[Parameter(Mandatory = $true)]$Scenario,
		[Parameter(Mandatory = $true)][string]$RunRoot
	)
	Assert-ExactPreservedJsonObjectKeys -Object $ReferenceStrict -RequiredKeys @('path', 'sha256') -Description "$ExpectedKind evidence reference"
	if ($ReferenceStrict['path'] -isnot [string] -or $ReferenceStrict['sha256'] -isnot [string]) {
		throw "$ExpectedKind evidence reference path and sha256 must be JSON strings."
	}
	$relativePath = [string](Get-RequiredJsonProperty -Object $Reference -Name 'path' -Description "$ExpectedKind evidence reference")
	if ($relativePath.Replace('\', '/') -cne $ExpectedPath) {
		throw "$ExpectedKind evidence reference must use '$ExpectedPath'."
	}
	$path = Get-AbsolutePath -Path $relativePath -BasePath $RunRoot
	Assert-PathBelow -Parent $RunRoot -Child $path -Description "$ExpectedKind evidence"
	if (-not (Test-Path -LiteralPath $path -PathType Leaf) -or (Get-Item -LiteralPath $path).Length -eq 0) {
		throw "$ExpectedKind evidence is missing or empty."
	}
	$expectedHash = [string](Get-RequiredJsonProperty -Object $Reference -Name 'sha256' -Description "$ExpectedKind evidence reference")
	if ($expectedHash -notmatch '^[0-9a-f]{64}$' -or (Get-Sha256 -Path $path) -cne $expectedHash) {
		throw "$ExpectedKind evidence hash does not match its reference."
	}
	$text = Get-Content -Raw -LiteralPath $path
	$strict = ConvertFrom-JsonPreservingArrays -Json $text -Description "$ExpectedKind evidence"
	$document = $text | ConvertFrom-Json
	$requiredDocumentKeys = if ($ExpectedKind -ceq 'before-play') {
		@('schemaVersion', 'phase', 'kind', 'capturedAtUtc', 'playModeRecordingOutputs', 'creationOnlyPreflight', 'snapshot')
	}
	else {
		@('schemaVersion', 'phase', 'kind', 'capturedAtUtc', 'beforeEvidence', 'operationEvidence',
			'transcriptEvidence', 'playBoundary', 'comparison', 'recordingOutputs', 'snapshot')
	}
	Assert-ExactPreservedJsonObjectKeys -Object $strict -RequiredKeys $requiredDocumentKeys -Description "$ExpectedKind evidence"
	$strictSchemaVersion = Get-RequiredPreservedJsonMember -Object $strict -Name 'schemaVersion' -Description "$ExpectedKind evidence"
	$strictPhase = Get-RequiredPreservedJsonMember -Object $strict -Name 'phase' -Description "$ExpectedKind evidence"
	$strictKind = Get-RequiredPreservedJsonMember -Object $strict -Name 'kind' -Description "$ExpectedKind evidence"
	$strictCapturedAt = Get-RequiredPreservedJsonMember -Object $strict -Name 'capturedAtUtc' -Description "$ExpectedKind evidence"
	$capturedAt = [DateTimeOffset]::MinValue
	if (($strictSchemaVersion -isnot [int] -and $strictSchemaVersion -isnot [long]) -or
		[int]$strictSchemaVersion -ne 1 -or
		$strictPhase -isnot [string] -or $strictKind -isnot [string] -or $strictCapturedAt -isnot [string] -or
		-not [DateTimeOffset]::TryParse(
			[string]$strictCapturedAt,
			[System.Globalization.CultureInfo]::InvariantCulture,
			[System.Globalization.DateTimeStyles]::RoundtripKind,
			[ref]$capturedAt
		) -or
		[string](Get-RequiredJsonProperty -Object $document -Name 'phase' -Description "$ExpectedKind evidence") -cne [string]$Scenario.phase -or
		[string](Get-RequiredJsonProperty -Object $document -Name 'kind' -Description "$ExpectedKind evidence") -cne $ExpectedKind) {
		throw "$ExpectedKind evidence identity does not match the reviewed scenario."
	}
	$snapshotStrict = Get-RequiredPreservedJsonMember -Object $strict -Name 'snapshot' -Description "$ExpectedKind evidence"
	$snapshot = Get-RequiredJsonProperty -Object $document -Name 'snapshot' -Description "$ExpectedKind evidence"
	[void](Assert-PlayModeSnapshotIntegrity -Snapshot $snapshot -SnapshotStrict $snapshotStrict -Description $ExpectedKind)
	return [ordered]@{
		path = $relativePath
		sha256 = $expectedHash
		document = $document
		strict = $strict
		capturedAtUtc = $capturedAt
	}
}

function Get-TranscriptToolEnvelopeAtSequence {
	param(
		[Parameter(Mandatory = $true)]$TranscriptState,
		[Parameter(Mandatory = $true)][int]$Sequence,
		[Parameter(Mandatory = $true)][string]$Tool
	)
	$matches = @($TranscriptState.entries | Where-Object {
		[int](Get-OptionalProperty -Object $_ -Name 'sequence') -eq $Sequence -and
		$null -ne (Get-OptionalProperty -Object $_ -Name 'response')
	})
	if ($matches.Count -ne 1) {
		throw "Play Mode '$Tool' sequence $Sequence does not identify exactly one transcript response."
	}
	$request = Get-RequiredJsonProperty -Object $matches[0] -Name 'request' -Description "Play Mode '$Tool' transcript"
	$params = Get-RequiredJsonProperty -Object $request -Name 'params' -Description "Play Mode '$Tool' transcript request"
	$response = Get-RequiredJsonProperty -Object $matches[0] -Name 'response' -Description "Play Mode '$Tool' transcript"
	$requestId = Get-RequiredJsonProperty -Object $request -Name 'id' -Description "Play Mode '$Tool' transcript request"
	$responseId = Get-RequiredJsonProperty -Object $response -Name 'id' -Description "Play Mode '$Tool' transcript response"
	if (($requestId -isnot [int] -and $requestId -isnot [long]) -or
		($responseId -isnot [int] -and $responseId -isnot [long]) -or
		[int64]$requestId -ne $Sequence -or [string]$request.method -cne 'tools/call' -or
		[string]$params.name -cne $Tool -or [int64]$responseId -ne $Sequence) {
		throw "Play Mode '$Tool' transcript identity does not match sequence $Sequence."
	}
	return Get-ToolEnvelope -Response $response -Tool $Tool
}

function Read-AndValidatePlayModeEvidence {
	param(
		[Parameter(Mandatory = $true)][string]$Path,
		[Parameter(Mandatory = $true)]$Scenario,
		[Parameter(Mandatory = $true)][string]$RunRoot,
		[Parameter(Mandatory = $true)][string]$ProjectRoot
	)
	if (-not (Test-Path -LiteralPath $Path -PathType Leaf) -or (Get-Item -LiteralPath $Path).Length -eq 0) {
		throw 'Play Mode evidence is missing or empty.'
	}
	$text = Get-Content -Raw -LiteralPath $Path
	$strict = ConvertFrom-JsonPreservingArrays -Json $text -Description 'Play Mode evidence'
	$evidence = $text | ConvertFrom-Json
	Assert-ExactPreservedJsonObjectKeys -Object $strict -RequiredKeys @(
		'schemaVersion', 'phase', 'entered', 'exited', 'authoredStateRestored',
		'creationOnly', 'beforeSha256', 'afterSha256', 'excludedTransientState',
		'beforeEvidence', 'afterEvidence', 'operationEvidence', 'transcriptEvidence',
		'playBoundary', 'recordingOutputs', 'enter', 'exit'
	) -Description 'Play Mode evidence'
	$strictSchemaVersion = Get-RequiredPreservedJsonMember -Object $strict -Name 'schemaVersion' -Description 'Play Mode evidence'
	$strictPhase = Get-RequiredPreservedJsonMember -Object $strict -Name 'phase' -Description 'Play Mode evidence'
	if (($strictSchemaVersion -isnot [int] -and $strictSchemaVersion -isnot [long]) -or
		[int]$strictSchemaVersion -ne 2 -or $strictPhase -isnot [string] -or
		[string](Get-RequiredJsonProperty -Object $evidence -Name 'phase' -Description 'Play Mode evidence') -cne [string]$Scenario.phase) {
		throw 'Play Mode evidence schema/phase does not match the reviewed scenario.'
	}
	foreach ($booleanName in @('entered', 'exited', 'authoredStateRestored', 'creationOnly')) {
		$value = Get-RequiredPreservedJsonMember -Object $strict -Name $booleanName -Description 'Play Mode evidence'
		if ($value -isnot [bool] -or -not $value) {
			throw "Play Mode evidence '$booleanName' must be the JSON Boolean true."
		}
	}
	foreach ($snapshotHashName in @('beforeSha256', 'afterSha256')) {
		$strictHash = Get-RequiredPreservedJsonMember -Object $strict -Name $snapshotHashName -Description 'Play Mode evidence'
		if ($strictHash -isnot [string] -or [string]$strictHash -notmatch '^[0-9a-f]{64}$') {
			throw "Play Mode evidence '$snapshotHashName' must be a lowercase SHA-256 digest."
		}
	}
	$beforeReference = Get-RequiredJsonProperty -Object $evidence -Name 'beforeEvidence' -Description 'Play Mode evidence'
	$beforeReferenceStrict = Get-RequiredPreservedJsonMember -Object $strict -Name 'beforeEvidence' -Description 'Play Mode evidence'
	$afterReference = Get-RequiredJsonProperty -Object $evidence -Name 'afterEvidence' -Description 'Play Mode evidence'
	$afterReferenceStrict = Get-RequiredPreservedJsonMember -Object $strict -Name 'afterEvidence' -Description 'Play Mode evidence'
	$beforeState = Read-PlayModeSnapshotEvidence -Reference $beforeReference -ReferenceStrict $beforeReferenceStrict -ExpectedPath 'editor/play-mode-before.json' -ExpectedKind 'before-play' -Scenario $Scenario -RunRoot $RunRoot
	$afterState = Read-PlayModeSnapshotEvidence -Reference $afterReference -ReferenceStrict $afterReferenceStrict -ExpectedPath 'editor/play-mode-after.json' -ExpectedKind 'after-play' -Scenario $Scenario -RunRoot $RunRoot
	if ($afterState.capturedAtUtc -lt $beforeState.capturedAtUtc) {
		throw 'After-play evidence timestamp predates before-play evidence.'
	}
	$declared = @($Scenario.playModeRecordingOutputs)
	$beforeDeclaredStrict = Get-RequiredPreservedJsonMember -Object $beforeState.strict -Name 'playModeRecordingOutputs' -Description 'before-play evidence'
	if (-not (Test-IsJsonArray -Value $beforeDeclaredStrict) -or
		(Get-CanonicalJsonText -Value @($beforeState.document.playModeRecordingOutputs)) -cne (Get-CanonicalJsonText -Value $declared)) {
		throw 'Before-play evidence does not preserve the reviewed recording-output declarations.'
	}
	$preflightStrict = Get-RequiredPreservedJsonMember -Object $beforeState.strict -Name 'creationOnlyPreflight' -Description 'before-play evidence'
	$preflight = @(Get-RequiredJsonProperty -Object $beforeState.document -Name 'creationOnlyPreflight' -Description 'before-play evidence')
	if (-not (Test-IsJsonArray -Value $preflightStrict) -or $preflight.Count -ne $declared.Count) {
		throw 'Before-play evidence does not preserve every creation-only output preflight.'
	}
	$expectedPreflight = [System.Collections.Generic.List[object]]::new()
	foreach ($declaration in $declared) {
		$expectedPreflight.Add([ordered]@{
			path = [string]$declaration.path
			producerOperationId = [string]$declaration.producerOperationId
			producerResponsePath = [string]$declaration.producerResponsePath
			parent = 'Assets/Captured'
			parentExisted = $true
			outputExisted = $false
		})
	}
	if ((Get-CanonicalJsonText -Value @($preflight)) -cne (Get-CanonicalJsonText -Value @($expectedPreflight))) {
		throw 'Recording output preflight does not prove exact absent outputs below the existing parent.'
	}

	$operationReferenceStrict = Get-RequiredPreservedJsonMember -Object $strict -Name 'operationEvidence' -Description 'Play Mode evidence'
	Assert-ExactPreservedJsonObjectKeys -Object $operationReferenceStrict -RequiredKeys @('path', 'sha256') -Description 'Play Mode operation-evidence reference'
	if ($operationReferenceStrict['path'] -isnot [string] -or $operationReferenceStrict['sha256'] -isnot [string]) {
		throw 'Play Mode operation-evidence path and sha256 must be JSON strings.'
	}
	$operationReference = Get-RequiredJsonProperty -Object $evidence -Name 'operationEvidence' -Description 'Play Mode evidence'
	if ([string]$operationReference.path -cne 'editor/mcp-operations.json') {
		throw 'Play Mode evidence must reference editor/mcp-operations.json.'
	}
	$operationPath = Join-Path $RunRoot 'editor/mcp-operations.json'
	if ([string]$operationReference.sha256 -notmatch '^[0-9a-f]{64}$' -or
		(Get-Sha256 -Path $operationPath) -cne [string]$operationReference.sha256) {
		throw 'Play Mode operation-evidence hash does not match.'
	}
	[void](Read-AndValidateOperationEvidence -Path $operationPath -Scenario $Scenario -RunRoot $RunRoot -MinimumSchemaVersion 2)
	$operationDocument = Get-Content -Raw -LiteralPath $operationPath | ConvertFrom-Json
	$transcriptReferenceStrict = Get-RequiredPreservedJsonMember -Object $strict -Name 'transcriptEvidence' -Description 'Play Mode evidence'
	Assert-ExactPreservedJsonObjectKeys -Object $transcriptReferenceStrict -RequiredKeys @('path', 'sha256') -Description 'Play Mode transcript-evidence reference'
	if ($transcriptReferenceStrict['path'] -isnot [string] -or $transcriptReferenceStrict['sha256'] -isnot [string]) {
		throw 'Play Mode transcript-evidence path and sha256 must be JSON strings.'
	}
	$transcriptReference = Get-RequiredJsonProperty -Object $evidence -Name 'transcriptEvidence' -Description 'Play Mode evidence'
	if ((Get-CanonicalJsonText -Value $transcriptReference) -cne
		(Get-CanonicalJsonText -Value $operationDocument.transcriptEvidence)) {
		throw 'Play Mode transcript reference does not match operation evidence.'
	}
	$transcriptState = Read-McpTranscriptForOperationEvidence -Reference $transcriptReference -RunRoot $RunRoot
	$boundary = Get-RequiredJsonProperty -Object $evidence -Name 'playBoundary' -Description 'Play Mode evidence'
	$boundaryStrict = Get-RequiredPreservedJsonMember -Object $strict -Name 'playBoundary' -Description 'Play Mode evidence'
	Assert-ExactPreservedJsonObjectKeys -Object $boundaryStrict -RequiredKeys @(
		'enterMcpSequence', 'exitMcpSequence', 'enterEnvelopeSha256', 'exitEnvelopeSha256'
	) -Description 'Play Mode boundary'
	$strictEnterSequence = Get-RequiredPreservedJsonMember -Object $boundaryStrict -Name 'enterMcpSequence' -Description 'Play Mode boundary'
	$strictExitSequence = Get-RequiredPreservedJsonMember -Object $boundaryStrict -Name 'exitMcpSequence' -Description 'Play Mode boundary'
	$strictEnterHash = Get-RequiredPreservedJsonMember -Object $boundaryStrict -Name 'enterEnvelopeSha256' -Description 'Play Mode boundary'
	$strictExitHash = Get-RequiredPreservedJsonMember -Object $boundaryStrict -Name 'exitEnvelopeSha256' -Description 'Play Mode boundary'
	if (($strictEnterSequence -isnot [int] -and $strictEnterSequence -isnot [long]) -or
		($strictExitSequence -isnot [int] -and $strictExitSequence -isnot [long]) -or
		$strictEnterHash -isnot [string] -or [string]$strictEnterHash -notmatch '^[0-9a-f]{64}$' -or
		$strictExitHash -isnot [string] -or [string]$strictExitHash -notmatch '^[0-9a-f]{64}$') {
		throw 'Play Mode boundary sequences must be JSON integers and envelope hashes must be lowercase SHA-256 digests.'
	}
	$enterSequence = [int]$strictEnterSequence
	$exitSequence = [int]$strictExitSequence
	if ($enterSequence -lt 1 -or $exitSequence -le $enterSequence) {
		throw 'Play Mode boundary sequences are invalid or reordered.'
	}
	$enterEnvelope = Get-TranscriptToolEnvelopeAtSequence -TranscriptState $transcriptState -Sequence $enterSequence -Tool 'play.enter'
	$exitEnvelope = Get-TranscriptToolEnvelopeAtSequence -TranscriptState $transcriptState -Sequence $exitSequence -Tool 'play.exit'
	$enterOk = Get-RequiredJsonProperty -Object $enterEnvelope -Name 'ok' -Description 'play.enter envelope'
	$exitOk = Get-RequiredJsonProperty -Object $exitEnvelope -Name 'ok' -Description 'play.exit envelope'
	if ((Get-StringSha256 -Value ($enterEnvelope | ConvertTo-Json -Depth 80 -Compress)) -cne [string]$boundary.enterEnvelopeSha256 -or
		(Get-StringSha256 -Value ($exitEnvelope | ConvertTo-Json -Depth 80 -Compress)) -cne [string]$boundary.exitEnvelopeSha256 -or
		$enterOk -isnot [bool] -or -not $enterOk -or $exitOk -isnot [bool] -or -not $exitOk) {
		throw 'Play Mode boundary envelope hashes or successful outcomes do not match the transcript.'
	}
	$enterData = Get-RequiredJsonProperty -Object $enterEnvelope -Name 'data' -Description 'play.enter envelope'
	$exitData = Get-RequiredJsonProperty -Object $exitEnvelope -Name 'data' -Description 'play.exit envelope'
	if ((Get-CanonicalJsonText -Value $evidence.enter) -cne (Get-CanonicalJsonText -Value $enterData) -or
		(Get-CanonicalJsonText -Value $evidence.exit) -cne (Get-CanonicalJsonText -Value $exitData)) {
		throw 'Play Mode summary enter/exit data does not match its transcript boundary envelopes.'
	}
	foreach ($crossLink in @(
		[ordered]@{ name = 'before evidence'; actual = $afterState.document.beforeEvidence; expected = $evidence.beforeEvidence },
		[ordered]@{ name = 'operation evidence'; actual = $afterState.document.operationEvidence; expected = $operationReference },
		[ordered]@{ name = 'transcript evidence'; actual = $afterState.document.transcriptEvidence; expected = $transcriptReference },
		[ordered]@{ name = 'Play Mode boundary'; actual = $afterState.document.playBoundary; expected = $boundary }
	)) {
		if ((Get-CanonicalJsonText -Value $crossLink.actual) -cne (Get-CanonicalJsonText -Value $crossLink.expected)) {
			throw "After-play $($crossLink.name) cross-link does not match Play Mode evidence."
		}
	}
	if ((Get-CanonicalJsonText -Value $evidence.excludedTransientState) -cne
		(Get-CanonicalJsonText -Value $beforeState.document.snapshot.excludedTransientState)) {
		throw 'Play Mode summary changed its truthful UserSettings exclusion.'
	}

	$registry = [System.Collections.Generic.Dictionary[string, object]]::new([System.StringComparer]::Ordinal)
	foreach ($declaration in $declared) {
		$producerId = [string]$declaration.producerOperationId
		$record = @($operationDocument.operations | Where-Object { [string]$_.id -ceq $producerId })
		if ($record.Count -ne 1) { throw "Recording producer '$producerId' is missing from operation evidence." }
		$artifactReference = $record[0].evidence
		$artifactPath = Get-AbsolutePath -Path ([string]$artifactReference.path) -BasePath $RunRoot
		$artifact = Get-Content -Raw -LiteralPath $artifactPath | ConvertFrom-Json
		$registry[$producerId] = [ordered]@{
			envelope = $artifact.envelope
			mcpSequence = [int]$record[0].mcpSequence
			envelopeSha256 = [string]$record[0].observed.envelopeSha256
			evidenceSha256 = [string]$artifactReference.sha256
			approval = $record[0].approval
		}
	}
	$reconstructed = Complete-PlayModeRecordingOutputContract -Scenario $Scenario -ProjectRoot $ProjectRoot -BeforeSnapshot $beforeState.document.snapshot -AfterSnapshot $afterState.document.snapshot -BindingRegistry $registry -OperationInventory @($operationDocument.operations) -PlayEnterSequence $enterSequence -PlayExitSequence $exitSequence
	$expectedProofs = [System.Collections.Generic.List[object]]::new()
	foreach ($proof in @($reconstructed.outputs)) {
		$expectedProofs.Add([ordered]@{
			path = [string]$proof.path
			bytes = [UInt64]$proof.bytes
			sha256 = [string]$proof.sha256
			producerOperationId = [string]$proof.producerOperationId
			producerResponsePath = [string]$proof.producerResponsePath
			producerMcpSequence = [int]$proof.producerMcpSequence
			producerCompletionMcpSequence = [int]$proof.producerCompletionMcpSequence
			producerEnvelopeSha256 = [string]$proof.producerEnvelopeSha256
			producerApproval = $proof.producerApproval
			producerArtifact = $proof.producerArtifact
			operationEvidenceSha256 = [string]$operationReference.sha256
			transcriptSha256 = [string]$transcriptReference.sha256
		})
	}
	$afterOutputsStrict = Get-RequiredPreservedJsonMember -Object $afterState.strict -Name 'recordingOutputs' -Description 'after-play evidence'
	$playOutputsStrict = Get-RequiredPreservedJsonMember -Object $strict -Name 'recordingOutputs' -Description 'Play Mode evidence'
	if (-not (Test-IsJsonArray -Value $afterOutputsStrict) -or
		-not (Test-IsJsonArray -Value $playOutputsStrict) -or
		(Get-CanonicalJsonText -Value @($afterState.document.recordingOutputs)) -cne (Get-CanonicalJsonText -Value @($expectedProofs)) -or
		(Get-CanonicalJsonText -Value @($evidence.recordingOutputs)) -cne (Get-CanonicalJsonText -Value @($expectedProofs)) -or
		(Get-CanonicalJsonText -Value $afterState.document.comparison) -cne (Get-CanonicalJsonText -Value $reconstructed.comparison)) {
		throw 'Play Mode recording-output comparison or producer provenance was tampered.'
	}
	foreach ($proof in @($expectedProofs)) {
		$outputPath = Get-AbsolutePath -Path ([string]$proof.path) -BasePath $ProjectRoot
		$currentState = Get-LongPathFileState -Path $outputPath -MaximumBytes $script:MaximumPlayModeRecordingOutputBytes
		if ([UInt64]$currentState.bytes -ne [UInt64]$proof.bytes -or
			[string]$currentState.sha256 -cne [string]$proof.sha256) {
			throw "Recording output '$($proof.path)' changed after its after-play snapshot."
		}
	}
	if ([string]$evidence.beforeSha256 -cne [string]$beforeState.document.snapshot.sha256 -or
		[string]$evidence.afterSha256 -cne [string]$afterState.document.snapshot.sha256) {
		throw 'Play Mode summary snapshot hashes do not match its referenced evidence.'
	}
	return [ordered]@{
		schemaVersion = 2
		phase = [string]$Scenario.phase
		beforeSha256 = [string]$beforeState.document.snapshot.sha256
		afterSha256 = [string]$afterState.document.snapshot.sha256
		recordingOutputCount = $expectedProofs.Count
		creationOnly = $true
		authoredStateRestored = $true
		sha256 = Get-Sha256 -Path $Path
	}
}

function Get-EnvironmentEvidence {
	param([Parameter(Mandatory = $true)]$Baseline)
	$result = [ordered]@{
		host = ''
		operatingSystem = ''
		cpu = ''
		logicalProcessors = 0
		gpu = ''
		gpuDriver = ''
		vulkanLoader = ''
		vulkanDeviceApi = ''
		displayMode = ''
		referenceResolution = '1920x1080'
		vsync = $false
		powerPlan = ''
		acConnected = $false
		thermalSnapshot = @{}
		workerCount = $script:ReferenceWorkerCount
		ambientProcesses = @()
		identityMatched = $false
		mismatchReasons = @()
	}
	try {
		$computer = Get-CimInstance Win32_ComputerSystem -ErrorAction Stop
		$os = Get-CimInstance Win32_OperatingSystem -ErrorAction Stop
		$cpu = @(Get-CimInstance Win32_Processor -ErrorAction Stop)
		$gpu = @(Get-CimInstance Win32_VideoController -ErrorAction Stop | Where-Object { $_.Name -like '*RTX 4080 Laptop GPU*' } | Select-Object -First 1)
		$battery = @(Get-CimInstance Win32_Battery -ErrorAction SilentlyContinue | Select-Object -First 1)
		$result.host = "$($computer.Manufacturer) $($computer.Model)"
		$result.operatingSystem = "$($os.Caption) $($os.Version) build $($os.BuildNumber)"
		$result.cpu = [string]($cpu | Select-Object -First 1 -ExpandProperty Name)
		$result.logicalProcessors = [int]($cpu | Measure-Object -Property NumberOfLogicalProcessors -Sum).Sum
		if ($gpu.Count -eq 1) { $result.gpu = [string]$gpu[0].Name; $result.gpuDriver = [string]$gpu[0].DriverVersion }
		$result.acConnected = $battery.Count -eq 1 -and [int]$battery[0].BatteryStatus -eq 2
		$result.displayMode = "$($Baseline.identity.replacementHost.graphics.physicalDisplay.width)x$($Baseline.identity.replacementHost.graphics.physicalDisplay.height)@$($Baseline.identity.replacementHost.graphics.physicalDisplay.refreshHz)"
		$powerLine = (& powercfg.exe /GETACTIVESCHEME 2>$null | Out-String).Trim()
		$result.powerPlan = $powerLine
		# Windows PowerShell promotes native stderr lines to ErrorRecord objects when
		# streams are merged. vulkaninfo emits harmless loader warnings on stderr, so
		# a direct bounded process probe keeps those warnings from becoming failures.
		$vulkanStartInfo = [System.Diagnostics.ProcessStartInfo]::new()
		$vulkanStartInfo.FileName = (Get-Command vulkaninfo.exe -ErrorAction Stop).Source
		$vulkanStartInfo.Arguments = '--summary'
		$vulkanStartInfo.UseShellExecute = $false
		$vulkanStartInfo.CreateNoWindow = $true
		$vulkanStartInfo.RedirectStandardOutput = $true
		$vulkanStartInfo.RedirectStandardError = $true
		$vulkanProcess = [System.Diagnostics.Process]::Start($vulkanStartInfo)
		if ($null -eq $vulkanProcess) { throw 'vulkaninfo could not be started.' }
		$vulkanStdoutTask = $vulkanProcess.StandardOutput.ReadToEndAsync()
		$vulkanStderrTask = $vulkanProcess.StandardError.ReadToEndAsync()
		if (-not $vulkanProcess.WaitForExit(30000)) {
			try { $vulkanProcess.Kill() } catch { }
			[void]$vulkanProcess.WaitForExit(5000)
			throw 'vulkaninfo timed out after 30 seconds.'
		}
		$vulkan = $vulkanStdoutTask.GetAwaiter().GetResult()
		[void]$vulkanStderrTask.GetAwaiter().GetResult()
		if ($vulkanProcess.ExitCode -eq 0) {
			$result.vulkanLoader = ([regex]::Match($vulkan, 'Vulkan Instance Version:\s*([^\r\n]+)')).Groups[1].Value.Trim()
			# Each GPU block is parsed independently because vulkaninfo lists fields in
			# implementation-defined order. The discrete device's API version must come
			# from that same block so an integrated GPU cannot satisfy the identity lock.
			foreach ($deviceBlock in [regex]::Matches($vulkan, '(?ms)^GPU[0-9]+:\s*(?<body>.*?)(?=^GPU[0-9]+:|\z)')) {
				$body = $deviceBlock.Groups['body'].Value
				$deviceType = [regex]::Match($body, '(?m)^\s*deviceType\s*=\s*([^\r\n]+)').Groups[1].Value.Trim()
				if ($deviceType -ne 'PHYSICAL_DEVICE_TYPE_DISCRETE_GPU') { continue }
				$result.vulkanDeviceApi = [regex]::Match($body, '(?m)^\s*apiVersion\s*=\s*([^\r\n]+)').Groups[1].Value.Trim()
				break
			}
		}
		$result.ambientProcesses = @(Get-Process | Select-Object -ExpandProperty ProcessName -Unique | Sort-Object)
	}
	catch { $result.mismatchReasons += $_.Exception.Message }
	$expected = $Baseline.identity.replacementHost
	if ($result.host -notlike "*$($expected.model)*") { $result.mismatchReasons += 'host model mismatch' }
	if ($result.cpu -ne [string]$expected.processor.name) { $result.mismatchReasons += 'CPU mismatch' }
	if ($result.logicalProcessors -ne [int]$expected.processor.logicalProcessors) { $result.mismatchReasons += 'logical processor mismatch' }
	if ($result.gpu -ne [string]$expected.graphics.discrete.name -or $result.gpuDriver -ne [string]$expected.graphics.discrete.windowsDriverVersion) { $result.mismatchReasons += 'discrete GPU or driver mismatch' }
	if (-not $result.acConnected) { $result.mismatchReasons += 'AC power is not confirmed' }
	if ($result.powerPlan -notmatch 'Performance') { $result.mismatchReasons += 'Performance power plan is not active' }
	if ($result.vulkanLoader -ne [string]$Baseline.identity.replacementHost.graphics.vulkanLoaderVersion -or $result.vulkanDeviceApi -ne [string]$expected.graphics.discrete.vulkanApiVersion) { $result.mismatchReasons += 'Vulkan loader/device API mismatch' }
	$result.identityMatched = $result.mismatchReasons.Count -eq 0
	return $result
}

function New-AcceptanceMatrixUpdate {
	param(
		[Parameter(Mandatory = $true)][string]$MatrixPath,
		[Parameter(Mandatory = $true)][string]$UpdateManifestPath,
		[Parameter(Mandatory = $true)][string]$RepoRoot,
		[Parameter(Mandatory = $true)][string]$RunRoot,
		[Parameter(Mandatory = $true)]$Scenario,
		[Parameter(Mandatory = $true)]$OperationInventory,
		[Parameter(Mandatory = $true)]$CameraInventory,
		[Parameter(Mandatory = $true)]$ManualEvidence,
		[bool]$PerformanceDeferred = $false
	)
	if (-not (Test-Path -LiteralPath $MatrixPath -PathType Leaf)) { throw "Acceptance matrix is missing: '$MatrixPath'." }
	if (-not (Test-Path -LiteralPath $UpdateManifestPath -PathType Leaf)) { throw "Acceptance update manifest is missing: '$UpdateManifestPath'." }
	$updateManifestText = Get-Content -Raw -LiteralPath $UpdateManifestPath
	$updateManifestStrict = ConvertFrom-JsonPreservingArrays -Json $updateManifestText -Description 'Acceptance update manifest'
	$updateManifest = $updateManifestText | ConvertFrom-Json
	if ($updateManifestStrict -isnot [System.Collections.IDictionary] -or $null -eq $updateManifest -or $updateManifest -is [System.Array]) {
		throw 'Acceptance update manifest must contain one JSON object.'
	}
	if ([int]$updateManifest.schemaVersion -ne 1 -or [string]$updateManifest.phase -ne $Phase) { throw "Acceptance update manifest schema/phase mismatch." }
	$updatesStrict = Get-RequiredPreservedJsonMember -Object $updateManifestStrict -Name 'updates' -Description 'Acceptance update manifest'
	$updates = @(Get-RequiredJsonProperty -Object $updateManifest -Name 'updates' -Description 'Acceptance update manifest')
	if (-not (Test-IsJsonArray -Value $updatesStrict) -or $updates.Count -lt 1) {
		throw 'Acceptance update manifest updates must be a non-empty JSON array.'
	}
	$originalBytes = [System.IO.File]::ReadAllBytes($MatrixPath)
	$originalHash = Get-Sha256 -Path $MatrixPath
	$lines = [System.Collections.Generic.List[string]]::new()
	# Windows PowerShell 5.1 treats BOM-less UTF-8 as the active ANSI code page
	# when Get-Content has no explicit encoding. Read through .NET with UTF-8
	# explicitly so punctuation in the matrix cannot turn into mojibake each time
	# an acceptance transaction rewrites the file.
	foreach ($line in @([System.IO.File]::ReadAllLines($MatrixPath, $script:Utf8NoBom))) { $lines.Add($line) }
	$accepted = [System.Collections.Generic.List[string]]::new()
	$blocked = [System.Collections.Generic.List[string]]::new()
	$updateIds = [System.Collections.Generic.HashSet[string]]::new([System.StringComparer]::Ordinal)
	$evidenceReference = (Get-RelativeForwardPath -Parent $RepoRoot -Child (Join-Path $RunRoot 'result.json'))
	foreach ($update in $updates) {
		if ($null -eq $update -or $update -is [string] -or $update -is [System.Array]) { throw 'Every acceptance update must be a JSON object.' }
		$id = [string]$update.id
		if ($id -notmatch '^(FR-(?:00[1-9]|0[1-9][0-9]|1[0-9]{2}|2[0-2][0-9]|23[0-3])|SC-(?:00[1-9]|0[1-2][0-9]|03[0-5]))$') { throw "Invalid acceptance requirement ID '$id'." }
		if (-not $updateIds.Add($id)) { throw "Acceptance update repeats requirement '$id'." }
		# PowerShell's case-insensitive automatic $Matches variable is replaced by
		# every successful -match expression. Use a distinctly named typed list so
		# the regex result cannot overwrite the acceptance-row indices.
		$matchingLineIndices = [System.Collections.Generic.List[int]]::new()
		for ($index = 0; $index -lt $lines.Count; ++$index) {
			if ($lines[$index] -match "^\|\s*$([regex]::Escape($id))\s*\|") {
				$matchingLineIndices.Add($index)
			}
		}
		if ($matchingLineIndices.Count -ne 1) { throw "Acceptance matrix must contain exactly one row for '$id'." }
		$cells = @($lines[$matchingLineIndices[0]].Trim().Trim('|') -split '(?<!\\)\|' | ForEach-Object { $_.Trim() })
		if ($cells.Count -ne 6) { throw "Acceptance row '$id' has an unexpected table shape." }
		$status = [string]$update.status
		if ($status -notin @('Accepted', 'Evidence incomplete', 'Blocked', 'In progress')) { throw "Unsupported acceptance status '$status'." }
		if ($PerformanceDeferred -and $status -eq 'Accepted' -and $script:DeferredPerformanceAcceptanceRows -contains $id) {
			throw "Deferred gate may not accept performance-dependent row '$id'; final AnimationValidationRunsEveryAccumulatedFeatureWorkflowBenchmarkTRunsEveryUnrelated/AnimationValidationInvokeFinalOnlyTTComparatorEveryPreparedControlIncluding/AnimationValidationRepairCapableFinalChecksTTAreCleanFinalBlocking evidence is still outstanding."
		}
		if ($cells[5] -eq 'Accepted' -and $status -ne 'Accepted') { throw "Acceptance update may not weaken already accepted row '$id'." }
		$cells[4] = ($cells[4] + "<br>Gate evidence: ``$evidenceReference``")
		$cells[5] = $status
		$lines[$matchingLineIndices[0]] = '| ' + ($cells -join ' | ') + ' |'
		if ($status -eq 'Accepted') { $accepted.Add($id) } else { $blocked.Add($id) }
	}
	if ($accepted.Count -lt 1) {
		throw 'Acceptance update contains no Accepted rows and therefore cannot unblock the next phase.'
	}
	if ($accepted.Count -gt 0) {
		$proof = Get-RequiredJsonProperty -Object $updateManifest -Name 'acceptedEvidence' -Description 'Acceptance update manifest'
		$proofStrict = Get-RequiredPreservedJsonMember -Object $updateManifestStrict -Name 'acceptedEvidence' -Description 'Acceptance update manifest'
		if ($proofStrict -isnot [System.Collections.IDictionary] -or $null -eq $proof -or $proof -is [string] -or $proof -is [System.Array]) {
			throw 'Acceptance update acceptedEvidence must be one JSON object when rows are accepted.'
		}
		$statement = [string](Get-RequiredJsonProperty -Object $proof -Name 'statement' -Description 'Acceptance acceptedEvidence')
		if ($statement.Trim().Length -lt 80) {
			throw 'Acceptance acceptedEvidence statement must specifically explain the phase evidence binding in at least 80 characters.'
		}
		$proofCollections = [ordered]@{
			requirementIds = @($accepted)
			operationIds = @($Scenario.operations | ForEach-Object { [string]$_.id })
			captureIds = @($Scenario.captures | ForEach-Object { [string]$_.id })
			manualChecklistIds = @($Scenario.manualChecklist | Where-Object { $null -eq (Get-OptionalProperty -Object $_ -Name 'required') -or [bool]$_.required } | ForEach-Object { [string]$_.id })
		}
		foreach ($collectionName in $proofCollections.Keys) {
			$strictCollection = Get-RequiredPreservedJsonMember -Object $proofStrict -Name $collectionName -Description 'Acceptance acceptedEvidence'
			$values = @(Get-RequiredJsonProperty -Object $proof -Name $collectionName -Description 'Acceptance acceptedEvidence' | ForEach-Object { [string]$_ })
			if (-not (Test-IsJsonArray -Value $strictCollection) -or $values.Count -lt 1) {
				throw "Acceptance acceptedEvidence $collectionName must be a non-empty JSON array."
			}
			if (@($values | Select-Object -Unique).Count -ne $values.Count) {
				throw "Acceptance acceptedEvidence $collectionName contains duplicate identifiers."
			}
			$expectedValues = @($proofCollections[$collectionName])
			if (@(Compare-Object -ReferenceObject @($expectedValues | Sort-Object) -DifferenceObject @($values | Sort-Object) -CaseSensitive).Count -ne 0) {
				throw "Acceptance acceptedEvidence $collectionName must exactly bind every applicable reviewed scenario identifier."
			}
		}
		$operationIds = @($OperationInventory | Where-Object { [string]$_.kind -eq 'operation' -and [bool]$_.expectationPassed } | ForEach-Object { [string]$_.id })
		$captureIds = @($CameraInventory | Where-Object { [bool]$_.visualAssessment.passed } | ForEach-Object { [string]$_.id })
		$manualIds = @($ManualEvidence.records | Where-Object { [bool]$_.passed } | ForEach-Object { [string]$_.id })
		foreach ($binding in @(
			[ordered]@{ name = 'operationIds'; expected = $proofCollections.operationIds; actual = $operationIds },
			[ordered]@{ name = 'captureIds'; expected = $proofCollections.captureIds; actual = $captureIds },
			[ordered]@{ name = 'manualChecklistIds'; expected = $proofCollections.manualChecklistIds; actual = $manualIds }
		)) {
			if (@(Compare-Object -ReferenceObject @($binding.expected | Sort-Object) -DifferenceObject @($binding.actual | Sort-Object) -CaseSensitive).Count -ne 0) {
				throw "Accepted rows are not backed by every declared passing $($binding.name) result."
			}
		}
		$artifactValuesStrict = Get-RequiredPreservedJsonMember -Object $proofStrict -Name 'requiredArtifacts' -Description 'Acceptance acceptedEvidence'
		$artifactValues = @(Get-RequiredJsonProperty -Object $proof -Name 'requiredArtifacts' -Description 'Acceptance acceptedEvidence' | ForEach-Object { [string]$_ })
		if (-not (Test-IsJsonArray -Value $artifactValuesStrict) -or $artifactValues.Count -lt 4) {
			throw 'Acceptance acceptedEvidence requiredArtifacts must be a JSON array containing at least four independent proof files.'
		}
		foreach ($relativeArtifact in $artifactValues) {
			if ([string]::IsNullOrWhiteSpace($relativeArtifact) -or [System.IO.Path]::IsPathRooted($relativeArtifact) -or @($relativeArtifact -split '[\\/]' | Where-Object { $_ -in @('', '.', '..') }).Count -gt 0) {
				throw "Acceptance proof artifact '$relativeArtifact' is not a safe run-root-relative path."
			}
			$artifactPath = Get-AbsolutePath -Path $relativeArtifact -BasePath $RunRoot
			Assert-PathBelow -Parent $RunRoot -Child $artifactPath -Description 'Acceptance proof artifact'
			if (-not (Test-Path -LiteralPath $artifactPath -PathType Leaf) -or (Get-Item -LiteralPath $artifactPath).Length -eq 0) {
				throw "Acceptance proof artifact '$relativeArtifact' is missing or empty."
			}
		}
	}
	$updatedText = (($lines -join "`r`n") + "`r`n")
	$updatedHash = Get-StringSha256 -Value $updatedText
	if ($updatedHash -ceq $originalHash) { throw 'Acceptance update is a no-op and cannot unblock the next phase.' }
	return [ordered]@{
		matrixPath = $MatrixPath
		originalBytes = $originalBytes
		updatedText = $updatedText
		rowsEvaluated = @($updates | ForEach-Object { [string]$_.id })
		rowsAccepted = @($accepted)
		rowsBlocked = @($blocked)
		beforeSha256 = $originalHash
		proposedAfterSha256 = $updatedHash
	}
}

function Apply-AcceptanceMatrixUpdate {
	param([Parameter(Mandatory = $true)]$Plan)
	if ((Get-Sha256 -Path ([string]$Plan.matrixPath)) -cne [string]$Plan.beforeSha256) {
		throw 'Acceptance matrix changed concurrently; refusing a stale update.'
	}
	$tempPath = "$($Plan.matrixPath).$RunId.tmp"
	Write-Utf8Text -Path $tempPath -Value ([string]$Plan.updatedText)
	Move-Item -LiteralPath $tempPath -Destination ([string]$Plan.matrixPath) -Force
	$afterHash = Get-Sha256 -Path ([string]$Plan.matrixPath)
	if ($afterHash -cne [string]$Plan.proposedAfterSha256) {
		throw 'Acceptance matrix write did not produce the prepared content hash.'
	}
	return [ordered]@{
		updateApplied = $true
		transactionState = 'committed-after-evidence-lock'
		rowsEvaluated = @($Plan.rowsEvaluated)
		rowsAccepted = @($Plan.rowsAccepted)
		rowsBlocked = @($Plan.rowsBlocked)
		beforeSha256 = [string]$Plan.beforeSha256
		afterSha256 = $afterHash
	}
}

function Restore-AcceptanceMatrixUpdate {
	param([Parameter(Mandatory = $true)]$Plan)
	$currentHash = Get-Sha256 -Path ([string]$Plan.matrixPath)
	if ($currentHash -cne [string]$Plan.proposedAfterSha256) {
		throw 'Acceptance receipt failed and the matrix changed again; refusing an unsafe rollback.'
	}
	$tempPath = "$($Plan.matrixPath).$RunId.rollback.tmp"
	[System.IO.File]::WriteAllBytes($tempPath, [byte[]]$Plan.originalBytes)
	Move-Item -LiteralPath $tempPath -Destination ([string]$Plan.matrixPath) -Force
	if ((Get-Sha256 -Path ([string]$Plan.matrixPath)) -cne [string]$Plan.beforeSha256) {
		throw 'Acceptance matrix rollback did not restore its original content hash.'
	}
}

function Assert-RequiredEvidence {
	param(
		[Parameter(Mandatory = $true)][string]$RunRoot,
		[Parameter(Mandatory = $true)]$Scenario,
		[bool]$PerformanceDeferred = $false,
		[bool]$FinalGate = $false
	)
	$required = @(
		'phase-evidence.json', 'editor/clicks.jsonl', 'editor/mcp.jsonl', 'editor/play-mode.json',
		'editor/play-mode-before.json', 'editor/play-mode-after.json',
		'editor/authored-state-stability-before.json', 'editor/authored-state-stability-after.json',
		'editor/camera-evidence.json', 'editor/tool-inventory.json', 'editor/native-window.json', 'editor/input-capability.json',
		'editor/manual-checklist.json', 'editor/manual-stage.json', 'editor/manual-complete.json', 'editor/manual-evidence.json',
		'editor/native-actions.jsonl', 'editor/mcp-reconnect.json',
		'editor/manual-scene-save.json', 'editor/final-scene-save.json',
		'build/toolchain-environment.json', 'build/integrated-build.command.json',
		'tests/focused-tests.command.json', 'install/install.command.json'
	)
	foreach ($capture in @($Scenario.captures)) {
		$required += "editor/cameras/$($capture.id).png"
		$required += "editor/cameras/$($capture.id).visual.json"
	}
	if ($PerformanceDeferred) {
		$required += 'performance/deferral.json'
	}
	else {
		$required += 'performance/comparison.json'
		if ($FinalGate) { $required += 'inputs/final-animation-performance-evidence.json' }
	}
	foreach ($extra in @($Scenario.requiredEvidence)) { $required += [string]$extra }
	$missing = [System.Collections.Generic.List[string]]::new()
	foreach ($relative in @($required | Select-Object -Unique)) {
		$path = Get-AbsolutePath -Path $relative -BasePath $RunRoot
		Assert-PathBelow -Parent $RunRoot -Child $path -Description "Required evidence"
		if (-not (Test-Path -LiteralPath $path -PathType Leaf) -or (Get-Item -LiteralPath $path).Length -eq 0) { $missing.Add($relative) }
	}
	if ($missing.Count -gt 0) { throw "Required phase evidence is missing or empty: $($missing -join ', ')." }
	# This artifact is required by every installed scenario. Parse it again just
	# before locking so a placeholder, partial rewrite, or mismatched transcript
	# cannot satisfy the evidence contract through file existence alone.
	[void](Read-AndValidateOperationEvidence -Path (Join-Path $RunRoot 'editor/mcp-operations.json') -Scenario $Scenario -RunRoot $RunRoot -MinimumSchemaVersion 2)
	[void](Read-AndValidatePlayModeEvidence -Path (Join-Path $RunRoot 'editor/play-mode.json') -Scenario $Scenario -RunRoot $RunRoot -ProjectRoot (Join-Path $RunRoot 'project'))
	return @($required | Select-Object -Unique)
}

function Write-EvidenceLock {
	param([Parameter(Mandatory = $true)][string]$RunRoot)
	$inventory = @(Get-FileInventory -Root $RunRoot | Where-Object { $_.path -notin @('evidence-manifest.json', 'evidence-lock.json') })
	$manifest = [ordered]@{ schemaVersion = 1; feature = $script:FeatureName; phase = $Phase; runId = $RunId; files = $inventory; fileCount = $inventory.Count; totalBytes = [UInt64](($inventory | Measure-Object bytes -Sum).Sum); aggregateSha256 = Get-InventoryDigest -Inventory $inventory }
	$manifestPath = Join-Path $RunRoot 'evidence-manifest.json'
	Write-JsonFile -Path $manifestPath -Value $manifest
	$lock = [ordered]@{ schemaVersion = 1; lockedAt = (Get-Date).ToString('o'); manifest = 'evidence-manifest.json'; manifestSha256 = Get-Sha256 -Path $manifestPath; aggregateSha256 = $manifest.aggregateSha256; appendNeverOverwrite = $true }
	$lockPath = Join-Path $RunRoot 'evidence-lock.json'
	Write-JsonFile -Path $lockPath -Value $lock
	(Get-Item -LiteralPath $manifestPath).IsReadOnly = $true
	(Get-Item -LiteralPath $lockPath).IsReadOnly = $true
	return $lock
}

$repoRoot = [System.IO.Path]::GetFullPath((Join-Path $PSScriptRoot '../..'))
$outRoot = Join-Path $repoRoot 'out'
$animationEvidenceRoot = Join-Path $outRoot 'validation/animation'
$baselineRoot = Join-Path $animationEvidenceRoot 'baseline'
$resolvedEvidenceRoot = Get-AbsolutePath -Path $EvidenceRoot -BasePath $repoRoot
$resolvedInstallRoot = Get-AbsolutePath -Path $InstallRoot -BasePath $repoRoot
$resolvedBuildDirectory = Get-AbsolutePath -Path $BuildDirectory -BasePath $repoRoot
$phaseRoot = Join-Path $resolvedEvidenceRoot $Phase
$runRoot = Join-Path $phaseRoot $RunId
$resultPath = Join-Path $runRoot 'result.json'
$startedAt = Get-Date
$runCreated = $false
$editorProcess = $null
$editorJob = $null
$editorJobCreated = $false
$editorStdoutTask = $null
$editorStderrTask = $null
$forcedTermination = $false
$gracefulShutdownAttempted = $false
$editorExited = $false
$editorExitCode = $null
$mcpPortReleased = $null
$cleanupFailures = [System.Collections.Generic.List[string]]::new()
$failure = $null
$nativeInputException = $null
$acceptanceResult = [ordered]@{ matrix = 'specs/complete-animation-system/acceptance-matrix.md'; rowsEvaluated = @(); rowsAccepted = @(); rowsBlocked = @(); updateApplied = $false }
$acceptancePlan = $null
$acceptanceReceiptPath = Join-Path $phaseRoot "$RunId-acceptance-commit.json"
$cameraInventory = [System.Collections.Generic.List[object]]::new()
$operationInventory = [System.Collections.Generic.List[object]]::new()
$argumentBindingRegistry = [System.Collections.Generic.Dictionary[string, object]]::new([System.StringComparer]::Ordinal)
$approvalRegistry = [System.Collections.Generic.HashSet[string]]::new([System.StringComparer]::Ordinal)
$phaseEvidence = $null
$gateTaskNumber = [int]$GateTask.Substring(1)
$isFinalGate = $Phase -match '^final(?:-|$)' -or $gateTaskNumber -ge 272
$performanceDeferred = [bool]$DeferPerformanceExecutionToFinalIntegration
$finalAnimationPerformanceEvidence = $null
$currentOutstandingPerformanceTasks = [System.Collections.Generic.List[string]]::new()
if ($performanceDeferred) { foreach ($task in $script:OutstandingFinalPerformanceTasks) { $currentOutstandingPerformanceTasks.Add($task) } }

try {
	if ($Execute -and $DryRun) { throw '-Execute and -DryRun are mutually exclusive.' }
	if ($ContinueAfterNativeInputException -and [string]::IsNullOrWhiteSpace($NativeInputExceptionAuthority)) { throw '-NativeInputExceptionAuthority must be nonempty with -ContinueAfterNativeInputException.' }
	if (-not $ContinueAfterNativeInputException -and -not [string]::IsNullOrWhiteSpace($NativeInputExceptionAuthority)) { throw '-NativeInputExceptionAuthority is valid only with -ContinueAfterNativeInputException.' }
	if ($performanceDeferred -and [string]::IsNullOrWhiteSpace($PerformanceDeferralAuthority)) { throw '-PerformanceDeferralAuthority must be nonempty with -DeferPerformanceExecutionToFinalIntegration.' }
	if (-not $performanceDeferred -and -not [string]::IsNullOrWhiteSpace($PerformanceDeferralAuthority)) { throw '-PerformanceDeferralAuthority is valid only with -DeferPerformanceExecutionToFinalIntegration.' }
	if ($performanceDeferred -and $isFinalGate) { throw 'Final gates (Phase final* or GateTask AnimationValidationAuditEveryFrFrScScAgainstIncrementallyMaintainedImplementation and later) reject performance deferral.' }
	if ($performanceDeferred -and (-not [string]::IsNullOrWhiteSpace($PerformanceCandidateManifest) -or -not [string]::IsNullOrWhiteSpace($FinalAnimationPerformanceEvidenceManifest))) { throw 'Deferred gates reject performance candidate and final AnimationValidationRunsEveryAccumulatedFeatureWorkflowBenchmarkTRunsEveryUnrelated evidence manifests.' }
	if ($Execute -and -not $isFinalGate -and -not $performanceDeferred) { throw 'Intermediate execution requires -DeferPerformanceExecutionToFinalIntegration so no performance workload or comparison can run before implementation stage.' }
	if (-not $isFinalGate -and -not [string]::IsNullOrWhiteSpace($PerformanceCandidateManifest)) { throw 'Intermediate gates reject performance candidate manifests; AnimationValidationInvokeFinalOnlyTTComparatorEveryPreparedControlIncluding owns the final paired comparison.' }
	if ($isFinalGate -and [string]::IsNullOrWhiteSpace($PerformanceCandidateManifest)) { throw 'Final gates require -PerformanceCandidateManifest for the fresh AnimationValidationInvokeFinalOnlyTTComparatorEveryPreparedControlIncluding engine-wide comparison.' }
	if ($isFinalGate -and [string]::IsNullOrWhiteSpace($FinalAnimationPerformanceEvidenceManifest)) { throw 'Final gates require -FinalAnimationPerformanceEvidenceManifest for passing non-deferred AnimationValidationRunsEveryAccumulatedFeatureWorkflowBenchmarkTRunsEveryUnrelated evidence.' }
	if (-not $isFinalGate -and -not [string]::IsNullOrWhiteSpace($FinalAnimationPerformanceEvidenceManifest)) { throw 'Final AnimationValidationRunsEveryAccumulatedFeatureWorkflowBenchmarkTRunsEveryUnrelated animation-performance evidence may be supplied only to a final gate.' }
	Assert-PathAtOrBelow -Parent $animationEvidenceRoot -Child $resolvedEvidenceRoot -Description 'Animation evidence root'
	Assert-PathBelow -Parent $resolvedEvidenceRoot -Child $runRoot -Description 'Phase run root'
	Assert-PathBelow -Parent $outRoot -Child $resolvedInstallRoot -Description 'Install root'
	Assert-PathBelow -Parent $outRoot -Child $resolvedBuildDirectory -Description 'Build directory'
	$buildOwnedInstallRoot = [System.IO.Path]::GetFullPath((Join-Path $resolvedBuildDirectory 'Build')).TrimEnd('\', '/')
	if (-not $resolvedInstallRoot.TrimEnd('\', '/').Equals($buildOwnedInstallRoot, [System.StringComparison]::OrdinalIgnoreCase)) {
		throw "InstallRoot must be the selected BuildDirectory's CMake-owned 'Build' tree ('$buildOwnedInstallRoot')."
	}
	if (Test-PathBelow -Parent $baselineRoot -Child $runRoot) { throw 'Phase evidence may never be created below the immutable baseline.' }
	if ($script:Configuration -ne 'RelWithDebInfo' -or $script:ReferenceWorkerCount -ne 31 -or $script:ReferenceWidth -ne 1920 -or $script:ReferenceHeight -ne 1080) { throw 'Frozen validation constants were changed.' }
	$baselineBefore = Get-BaselineLockState -BaselineRoot $baselineRoot
	$lockedPerformanceInputs = Get-LockedPerformanceInputState -RepoRoot $repoRoot -Baseline $baselineBefore
	$environmentEvidence = Get-EnvironmentEvidence -Baseline $baselineBefore
	# Correctness-only intermediate gates retain every frozen host check except the
	# active power-plan label when the caller explicitly defers all performance
	# execution. This records the user-authorized waiver without making a timed
	# result appear comparable or weakening any hardware/driver identity check.
	$waivedEnvironmentMismatchReasons = @($environmentEvidence.mismatchReasons | Where-Object {
		$performanceDeferred -and [string]$_ -eq 'Performance power plan is not active'
	})
	$blockingEnvironmentMismatchReasons = @($environmentEvidence.mismatchReasons | Where-Object {
		-not ($performanceDeferred -and [string]$_ -eq 'Performance power plan is not active')
	})
	$gateEnvironmentIdentityMatched = $blockingEnvironmentMismatchReasons.Count -eq 0
	$environmentEvidence['performanceDeferralAuthorized'] = $performanceDeferred
	$environmentEvidence['gateIdentityMatched'] = $gateEnvironmentIdentityMatched
	$environmentEvidence['waivedMismatchReasons'] = @($waivedEnvironmentMismatchReasons)
	$environmentEvidence['blockingMismatchReasons'] = @($blockingEnvironmentMismatchReasons)

	$resolvedFixture = if ([string]::IsNullOrWhiteSpace($FixtureRoot)) { $null } else { Get-AbsolutePath -Path $FixtureRoot -BasePath $repoRoot }
	$resolvedScenario = if ([string]::IsNullOrWhiteSpace($ScenarioManifest)) { $null } else { Get-AbsolutePath -Path $ScenarioManifest -BasePath $repoRoot }
	$resolvedCandidate = if ([string]::IsNullOrWhiteSpace($PerformanceCandidateManifest)) { $null } else { Get-AbsolutePath -Path $PerformanceCandidateManifest -BasePath $repoRoot }
	$resolvedFinalAnimationPerformanceEvidence = if ([string]::IsNullOrWhiteSpace($FinalAnimationPerformanceEvidenceManifest)) { $null } else { Get-AbsolutePath -Path $FinalAnimationPerformanceEvidenceManifest -BasePath $repoRoot }
	$resolvedAcceptance = if ([string]::IsNullOrWhiteSpace($AcceptanceUpdateManifest)) { $null } else { Get-AbsolutePath -Path $AcceptanceUpdateManifest -BasePath $repoRoot }
	$scenario = if ($null -eq $resolvedScenario) { $null } else { Read-ScenarioManifest -Path $resolvedScenario -ExpectedPhase $Phase -RepoRoot $repoRoot }
	if ($isFinalGate) { $finalAnimationPerformanceEvidence = Read-FinalAnimationPerformanceEvidence -Path $resolvedFinalAnimationPerformanceEvidence -OutRoot $outRoot }

	$plan = [ordered]@{
		schemaVersion = 1
		tool = 'RunAnimationValidationGate'
		feature = $script:FeatureName
		phase = $Phase
		gateTask = $GateTask
		mode = if ($Execute) { 'execute' } else { 'dry-run' }
		passed = $true
		outcome = if ($Execute) { 'validated-for-execution' } else { 'dry-run-validated' }
		authorizedPerformanceDeferral = $performanceDeferred
		finalIntegrationBlockedOnPerformance = $performanceDeferred
		outstandingPerformanceTasks = @($currentOutstandingPerformanceTasks)
		fixedProfile = [ordered]@{ configuration = $script:Configuration; workerCount = $script:ReferenceWorkerCount; resolution = '1920x1080'; vsync = $false; minimumPerformanceRuns = 5; missingEvidence = 'reject'; thresholdWeakening = 'reject' }
		baseline = [ordered]@{ manifest = $baselineBefore.manifest; aggregateSha256 = $baselineBefore.aggregateSha256; digestMatched = $true; lockedHashes = $lockedPerformanceInputs }
		commands = @(
			"cmake --build $BuildDirectory",
			$(if ($performanceDeferred) { "ctest --test-dir $BuildDirectory -C RelWithDebInfo -R <focused-regex> -E $($script:DeferredPerformanceCTestExcludeRegex) --output-on-failure (TERRAENGINE_TEST_EXCLUDE_FILTER=$($script:DeferredPerformanceHarnessExcludeFilter))" } else { "ctest --test-dir $BuildDirectory -C RelWithDebInfo -R <focused-regex> --output-on-failure" }),
			"cmake --build $BuildDirectory --target install"
		)
		installedEditor = Join-Path $resolvedInstallRoot 'Editor/TerraEngine-Editor.exe'
		isolatedProject = Join-Path $runRoot 'project'
		evidenceRoot = $runRoot
		inputs = [ordered]@{
			fixture = $resolvedFixture
			fixtureOverlays = @(if ($null -ne $scenario) {
				$scenario.fixtureOverlays | ForEach-Object {
					[ordered]@{
						source = [string]$_.source
						destination = [string]$_.destination
						sourceFiles = [int]$_.validatedSourceFiles
						sourceBytes = [UInt64]$_.validatedSourceBytes
						sourceSha256 = [string]$_.validatedSourceSha256
					}
				}
			})
			scenario = $resolvedScenario
			performanceCandidate = $resolvedCandidate
			finalAnimationPerformanceEvidence = $resolvedFinalAnimationPerformanceEvidence
			acceptanceUpdate = $resolvedAcceptance
			performanceDeferralAuthority = $PerformanceDeferralAuthority
		}
		environmentIdentityMatched = $gateEnvironmentIdentityMatched
		environmentObservedIdentityMatched = $environmentEvidence.identityMatched
		environmentMismatchReasons = @($environmentEvidence.mismatchReasons)
		environmentWaivedMismatchReasons = @($waivedEnvironmentMismatchReasons)
		environmentBlockingMismatchReasons = @($blockingEnvironmentMismatchReasons)
	}
	if (-not $Execute) {
		$plan | ConvertTo-Json -Depth 20 -Compress | Write-Output
		return
	}

	$requiredInputs = @([ordered]@{ name = 'FixtureRoot'; value = $resolvedFixture }, [ordered]@{ name = 'ScenarioManifest'; value = $resolvedScenario }, [ordered]@{ name = 'AcceptanceUpdateManifest'; value = $resolvedAcceptance })
	if (-not $performanceDeferred) { $requiredInputs += [ordered]@{ name = 'PerformanceCandidateManifest'; value = $resolvedCandidate } }
	if ($isFinalGate) { $requiredInputs += [ordered]@{ name = 'FinalAnimationPerformanceEvidenceManifest'; value = $resolvedFinalAnimationPerformanceEvidence } }
	foreach ($requiredInput in $requiredInputs) {
		if ($null -eq $requiredInput.value) { throw "-$($requiredInput.name) is required with -Execute." }
	}
	if (-not $gateEnvironmentIdentityMatched) { throw "Frozen replacement-host identity does not match: $($blockingEnvironmentMismatchReasons -join '; ')." }
	if (-not (Test-LoopbackPortAvailable -Port $McpPort)) { throw "MCP port $McpPort is occupied." }
	if (Test-Path -LiteralPath $runRoot) { throw "Phase run evidence already exists and will not be overwritten: '$runRoot'." }

	[System.IO.Directory]::CreateDirectory($runRoot) | Out-Null
	$runCreated = $true
	foreach ($directory in @('commands', 'tests', 'build', 'install', 'editor', 'editor/cameras', 'editor/manual', 'performance', 'defects', 'inputs')) { [System.IO.Directory]::CreateDirectory((Join-Path $runRoot $directory)) | Out-Null }
	$templatePath = Join-Path $repoRoot 'Tools/Animation/AnimationEvidenceTemplate.json'
	if (-not (Test-Path -LiteralPath $templatePath -PathType Leaf)) { throw "Canonical AnimationEvidenceTemplate.json is missing." }
	$phaseEvidence = Get-Content -Raw -LiteralPath $templatePath | ConvertFrom-Json
	$phaseEvidence.phase = $Phase
	$phaseEvidence.gateTask = $GateTask
	$phaseEvidence.status = 'running'
	$phaseEvidence.authorizedPerformanceDeferral = $performanceDeferred
	$phaseEvidence.finalIntegrationBlockedOnPerformance = $performanceDeferred
	$phaseEvidence.outstandingPerformanceTasks = @($currentOutstandingPerformanceTasks)
	$phaseEvidence.result.authorizedPerformanceDeferral = $performanceDeferred
	$phaseEvidence.result.finalIntegrationBlockedOnPerformance = $performanceDeferred
	$phaseEvidence.result.outstandingPerformanceTasks = @($currentOutstandingPerformanceTasks)
	$phaseEvidence.startedAt = $startedAt.ToString('o')
	$phaseEvidence.lockedBaseline.identityMatched = $true
	$phaseEvidence.lockedBaseline.digestMatched = $true
	$phaseEvidence.lockedBaseline.aggregateSha256 = $baselineBefore.aggregateSha256
	$phaseEvidence.lockedBaseline.runtimeContractSha256 = $lockedPerformanceInputs.runtimeContractSha256
	$phaseEvidence.lockedBaseline.rendererWorkloadManifestSha256 = $lockedPerformanceInputs.rendererWorkloadManifestSha256
	$phaseEvidence.lockedBaseline.performanceWorkloadsSha256 = $lockedPerformanceInputs.performanceWorkloadsSha256
	$phaseEvidence.environment = $environmentEvidence
	$phaseEvidence | Add-Member -NotePropertyName runId -NotePropertyValue $RunId -Force
	Write-JsonFile -Path (Join-Path $runRoot 'phase-evidence.json') -Value $phaseEvidence
	Write-JsonFile -Path (Join-Path $runRoot 'inputs/baseline-reference.json') -Value ([ordered]@{ manifest = $baselineBefore.manifest; lock = $baselineBefore.lock; aggregateSha256 = $baselineBefore.aggregateSha256; manifestSha256 = $baselineBefore.manifestSha256; lockSha256 = $baselineBefore.lockSha256 })
	Copy-Item -LiteralPath $resolvedScenario -Destination (Join-Path $runRoot 'inputs/scenario.json')
	Copy-Item -LiteralPath $resolvedAcceptance -Destination (Join-Path $runRoot 'inputs/acceptance-update.json')
	if ($isFinalGate) { Copy-Item -LiteralPath $resolvedFinalAnimationPerformanceEvidence -Destination (Join-Path $runRoot 'inputs/final-animation-performance-evidence.json') }
	Write-JsonFile -Path (Join-Path $runRoot 'environment.json') -Value $environmentEvidence

	$fixtureCopy = Copy-IsolatedFixture -Source $resolvedFixture -Destination (Join-Path $runRoot 'project')
	$projectRoot = Join-Path $runRoot 'project'
	$fixtureOverlayEvidence = @(Copy-FixtureOverlays -Overlays @($scenario.fixtureOverlays) -RepoRoot $repoRoot -ProjectRoot $projectRoot)
	# Gate projects are disposable and must start from the repository's default
	# layout. Copying only while the editor is closed avoids the layout-reset race
	# observed during the earlier foundation gate.
	$layoutTemplateRoot = Join-Path $repoRoot 'Editor/TerraEngine-Editor/Project/Templates/UserSettings'
	$projectUserSettings = Join-Path $projectRoot 'UserSettings'
	if (-not (Test-Path -LiteralPath $layoutTemplateRoot -PathType Container)) { throw 'Default editor UserSettings template is missing.' }
	[System.IO.Directory]::CreateDirectory($projectUserSettings) | Out-Null
	$preferencesPath = Join-Path $runRoot 'editor/preferences.json'
	Write-JsonFile -Path $preferencesPath -Value ([ordered]@{
		recentProjects = @(); theme = 'Dark'; externalScriptEditorCommand = ''; autoBuildScripts = $false
		editorMcpEnabled = $true; editorMcpWriteModeDefault = 'writes_enabled'; editorMcpRecentSession = "animation-$Phase-$RunId"; editorMcpListenPort = $McpPort
		vulkanValidationLayers = $true; renderDebugDiagnostics = $true; rendererDiagnosticsPreferencesInitialized = $true
	})
	$phaseEvidence.inputs.isolatedProjectRoot = $projectRoot
	$phaseEvidence.inputs.editorPreferencesSha256 = Get-Sha256 -Path $preferencesPath
	$phaseEvidence.inputs.fixtureManifestSha256 = $fixtureCopy.sourceSha256
	$phaseEvidence.inputs | Add-Member -NotePropertyName fixtureOverlays -NotePropertyValue @($fixtureOverlayEvidence) -Force
	$phaseEvidence.inputs.performanceCandidateManifestSha256 = if ($null -eq $resolvedCandidate) { '' } else { Get-Sha256 -Path $resolvedCandidate }
	$phaseEvidence.inputs.finalAnimationPerformanceEvidenceManifestSha256 = if ($null -eq $finalAnimationPerformanceEvidence) { '' } else { [string]$finalAnimationPerformanceEvidence.sha256 }
	Write-JsonFile -Path (Join-Path $runRoot 'phase-evidence.json') -Value $phaseEvidence

	$cmake = (Get-Command cmake.exe -ErrorAction Stop).Source
	$ctest = (Get-Command ctest.exe -ErrorAction Stop).Source
	$toolchainEnvironment = Get-MsvcBuildEnvironment -BuildDirectory $resolvedBuildDirectory
	Write-JsonFile -Path (Join-Path $runRoot 'build/toolchain-environment.json') -Value $toolchainEnvironment.evidence
	$buildRecord = Invoke-BoundedProcess -Stage 'integrated-build' -FilePath $cmake -Arguments @('--build', $resolvedBuildDirectory) -WorkingDirectory $repoRoot -OutputRoot (Join-Path $runRoot 'build') -TimeoutSeconds $BuildTimeoutSeconds -Environment $toolchainEnvironment.environment
	$phaseEvidence.integratedBuild.log = $buildRecord.stdout; $phaseEvidence.integratedBuild.exitCode = $buildRecord.exitCode; $phaseEvidence.integratedBuild.passed = $true
	$discoveryArguments = @('--test-dir', $resolvedBuildDirectory, '-C', 'RelWithDebInfo', '-N', '-R', $FocusedTestRegex)
	$testArguments = @('--test-dir', $resolvedBuildDirectory, '-C', 'RelWithDebInfo', '-R', $FocusedTestRegex, '--output-on-failure')
	$testEnvironment = @{}
	foreach ($key in $toolchainEnvironment.environment.Keys) {
		$testEnvironment[[string]$key] = [string]$toolchainEnvironment.environment[$key]
	}
	if ($performanceDeferred) {
		$discoveryArguments += @('-E', $script:DeferredPerformanceCTestExcludeRegex)
		$testArguments += @('-E', $script:DeferredPerformanceCTestExcludeRegex)
		# Broad Engine.Animation/Editor.Animation CTest entries need a second,
		# case-name-level exclusion while retaining Animation.Dormant.* invariants.
		$testEnvironment['TERRAENGINE_TEST_EXCLUDE_FILTER'] = $script:DeferredPerformanceHarnessExcludeFilter
	}
	$discovery = Invoke-BoundedProcess -Stage 'focused-test-discovery' -FilePath $ctest -Arguments $discoveryArguments -WorkingDirectory $repoRoot -OutputRoot (Join-Path $runRoot 'tests') -TimeoutSeconds $TestTimeoutSeconds
	$discoveryText = Get-Content -Raw -LiteralPath $discovery.stdout
	$testCountMatch = [regex]::Match($discoveryText, 'Total Tests:\s*([0-9]+)')
	if (-not $testCountMatch.Success -or [int]$testCountMatch.Groups[1].Value -lt 1) { throw "Focused CTest discovery found no tests; missing test evidence fails closed." }
	$testRecord = Invoke-BoundedProcess -Stage 'focused-tests' -FilePath $ctest -Arguments $testArguments -WorkingDirectory $repoRoot -OutputRoot (Join-Path $runRoot 'tests') -TimeoutSeconds $TestTimeoutSeconds -Environment $testEnvironment
	$phaseEvidence.focusedValidation.tests = @([ordered]@{ regex = $FocusedTestRegex; excludedCTestRegex = if ($performanceDeferred) { $script:DeferredPerformanceCTestExcludeRegex } else { '' }; excludedHarnessFilter = if ($performanceDeferred) { $script:DeferredPerformanceHarnessExcludeFilter } else { '' }; dormantStructuralCasesRetained = $performanceDeferred; discovered = [int]$testCountMatch.Groups[1].Value; log = $testRecord.stdout; exitCode = $testRecord.exitCode; passed = $true })
	# Each configured build owns its Build/ install tree. Honoring BuildDirectory
	# here lets a live gate use a separate tree while an earlier installed
	# soak/runtime is legitimately executing and Windows holds its image open.
	$installRecord = Invoke-BoundedProcess -Stage 'install' -FilePath $cmake -Arguments @('--build', $resolvedBuildDirectory, '--target', 'install') -WorkingDirectory $repoRoot -OutputRoot (Join-Path $runRoot 'install') -TimeoutSeconds $InstallTimeoutSeconds -Environment $toolchainEnvironment.environment
	$phaseEvidence.install.log = $installRecord.stdout; $phaseEvidence.install.exitCode = $installRecord.exitCode; $phaseEvidence.install.passed = $true; $phaseEvidence.install.root = $resolvedInstallRoot
	$editorExecutable = Resolve-InstalledEditor -ResolvedInstallRoot $resolvedInstallRoot
	$installedLayoutRoot = Join-Path (Split-Path -Parent $editorExecutable) 'Project/Templates/UserSettings'
	$layoutEvidence = @(Install-VerifiedDefaultEditorLayout -SourceTemplateRoot $layoutTemplateRoot -InstalledTemplateRoot $installedLayoutRoot -ProjectRoot $projectRoot)
	$phaseEvidence.inputs | Add-Member -NotePropertyName editorLayouts -NotePropertyValue @($layoutEvidence) -Force
	$installInventory = @(Get-FileInventory -Root $resolvedInstallRoot)
	if ($installInventory.Count -lt 1) { throw "Install tree inventory is empty." }
	$phaseEvidence.install.inventory = $installInventory
	$phaseEvidence.installedEditor.executable = $editorExecutable
	$phaseEvidence.installedEditor.executableSha256 = Get-Sha256 -Path $editorExecutable
	$phaseEvidence.installedEditor.installedTreeOnly = $true

	$script:McpTranscriptPath = Join-Path $runRoot 'editor/mcp.jsonl'
	$script:McpEndpoint = "http://127.0.0.1:$McpPort/mcp"
	$editorStdoutPath = Join-Path $runRoot 'editor/editor.stdout.log'
	$editorStderrPath = Join-Path $runRoot 'editor/editor.stderr.log'
	$startInfo = [System.Diagnostics.ProcessStartInfo]::new()
	$startInfo.FileName = $editorExecutable
	# Quote each launch argument independently so spaces in the isolated project
	# path cannot change the installed editor's command-line meaning.
	$startInfo.Arguments = "$(ConvertTo-NativeArgument -Argument '--project') $(ConvertTo-NativeArgument -Argument $projectRoot)"
	$startInfo.WorkingDirectory = Split-Path -Parent $editorExecutable
	$startInfo.UseShellExecute = $false; $startInfo.RedirectStandardOutput = $true; $startInfo.RedirectStandardError = $true
	$startInfo.EnvironmentVariables['TERRAENGINE_EDITOR_PREFS_PATH'] = $preferencesPath
	$startInfo.EnvironmentVariables['TERRAENGINE_SUPPRESS_PROJECT_RECENTS'] = '1'
	$startInfo.EnvironmentVariables['TERRAENGINE_THREADPOOL_WORKERS'] = [string]$script:ReferenceWorkerCount
	$editorProcess = [System.Diagnostics.Process]::Start($startInfo)
	if ($null -eq $editorProcess) { throw 'Installed editor failed to start.' }
	Initialize-NativeValidationBridge
	# Keep the complete editor/compiler process tree under one kill-on-close
	# boundary. Assignment happens immediately after launch, before MCP readiness.
	$editorJob = [TerraEngine.Animation.Validation.NativeWindowEvidence]::CreateKillOnCloseProcessJob([int]$editorProcess.Id)
	$editorJobCreated = $true
	$editorStdoutTask = $editorProcess.StandardOutput.ReadToEndAsync(); $editorStderrTask = $editorProcess.StandardError.ReadToEndAsync()
	$phaseEvidence.installedEditor.launchArguments = @('--project', $projectRoot)
	$ready = $false
	$deadline = (Get-Date).AddSeconds($EditorStartupTimeoutSeconds)
	while ((Get-Date) -lt $deadline) {
		$editorProcess.Refresh(); if ($editorProcess.HasExited) { throw "Installed editor exited before MCP readiness with code $($editorProcess.ExitCode)." }
		try {
			$initialize = Invoke-McpRpc -Method 'initialize' -Params ([ordered]@{ protocolVersion = '2025-03-26'; clientInfo = [ordered]@{ name = 'animation-phase-gate'; version = '1' } })
			if ([string]$initialize.result.serverInfo.name -eq 'TerraEngine Editor') { $ready = $true; break }
		}
		catch { Start-Sleep -Milliseconds 500 }
	}
	if (-not $ready) { throw "Installed editor MCP did not become ready within $EditorStartupTimeoutSeconds seconds." }
	$nativeWindow = Wait-ForEditorMainWindow -EditorProcess $editorProcess -TimeoutSeconds $EditorStartupTimeoutSeconds
	$inputCapability = Get-NativeInputCapability
	Write-JsonFile -Path (Join-Path $runRoot 'editor/input-capability.json') -Value ([ordered]@{
		schemaVersion = 1
		processId = [int]$editorProcess.Id
		windowHandle = [string]$nativeWindow.windowHandle
		classification = $inputCapability
		sendInputAcceptedOnlyWhenAvailable = $true
		printWindowDoesNotRequireInteractiveDesktop = $true
	})
	Write-JsonFile -Path (Join-Path $runRoot 'editor/native-window.json') -Value ([ordered]@{
		schemaVersion = 1
		processId = $nativeWindow.processId
		windowHandle = $nativeWindow.windowHandle
		windowTitle = $nativeWindow.windowTitle
		validatedAtUtc = [DateTimeOffset]::UtcNow.ToString('o')
	})
	$phaseEvidence.installedEditor | Add-Member -NotePropertyName nativeWindow -NotePropertyValue $nativeWindow -Force
	$tools = Invoke-McpRpc -Method 'tools/list'
	$toolNames = @($tools.result.tools | ForEach-Object { [string]$_.name })
	$requiredTools = @('play.state', 'play.enter', 'play.exit', 'scene.list_objects', 'scene.save', 'scene_view.state', 'game_view.capture', 'camera.capture_custom', 'camera.capture_object') + @($scenario.requiredTools)
	foreach ($requiredTool in @($requiredTools | Select-Object -Unique)) { if ($toolNames -notcontains $requiredTool) { throw "Installed editor MCP does not advertise required tool '$requiredTool'." } }
	Write-JsonFile -Path (Join-Path $runRoot 'editor/tool-inventory.json') -Value ([ordered]@{ required = @($requiredTools | Select-Object -Unique); advertised = @($toolNames | Sort-Object) })

	$clickPath = Join-Path $runRoot 'editor/clicks.jsonl'
	$nativeEvidencePath = Join-Path $runRoot 'editor/native-actions.jsonl'
	$completionPath = Join-Path $runRoot 'editor/manual-complete.json'
	Write-JsonFile -Path (Join-Path $runRoot 'editor/manual-checklist.json') -Value ([ordered]@{
		schemaVersion = 4
		phase = $Phase
		instructions = 'Perform each declared item in exact array order, then append one action record to clicks.jsonl. Every record requires an ordered inputRequests JSON array with one atomic event per real interaction (maximum 128 and at least each item''s minimumInputEvents); one event cannot prove multi-action prose. Each event requires unique eventId, exact kind mouse-click or key-chord, and nonempty description. Human events also require strictly increasing performedAtUtc. Win32SendInput events omit performedAtUtc and provide clientX/clientY/button or virtualKeys; clientX/clientY are unscaled physical client pixels from the live editor surface, never DPI-scaled logical coordinates. The gate executes and receipts every event only when input-capability.json reports inputAvailable:true. Do not provide evidence metadata or screenshots: the gate owns PrintWindow capture, HWND ownership, dimensions, timestamps, file time, and pixel hashes. Write manual-complete.json only after native-actions.jsonl contains every action.'
		nativeWindow = $nativeWindow
		inputCapability = $inputCapability
		actionRequestPath = $clickPath
		gateOwnedEvidencePath = $nativeEvidencePath
		items = @($scenario.manualChecklist)
	})
	# Pre-Play operations create the exact assets and objects that the operator must
	# inspect. Run their reviewed operation/capture order before asking for manual
	# proof; otherwise a checklist could pass against an empty fixture.
	Invoke-ScenarioStage -Scenario $scenario -Stage 'prePlay' -CameraRoot (Join-Path $runRoot 'editor/cameras') -RunRoot $runRoot -CameraInventory $cameraInventory -OperationInventory $operationInventory -BindingRegistry $argumentBindingRegistry -ApprovalRegistry $approvalRegistry -ApprovalTimeoutSeconds $ManualEvidenceTimeoutSeconds
	$manualStageStartedAtUtc = [DateTimeOffset]::UtcNow
	Write-JsonFile -Path (Join-Path $runRoot 'editor/manual-stage.json') -Value ([ordered]@{ schemaVersion = 1; startedAtUtc = $manualStageStartedAtUtc.ToString('o'); inputCapability = $inputCapability; actionCount = @($scenario.manualChecklist).Count })
	if ($ContinueAfterNativeInputException) {
		# This mode is intentionally fail-closed. It lets unattended validation
		# finish MCP, Play Mode, camera, restoration, and reconnect checks after
		# Windows denies foreground input, but it can never accept manual evidence
		# or advance the acceptance matrix.
		$nativeInputException = [ordered]@{
			schemaVersion = 1
			status = 'authorized-native-input-exception'
			authority = $NativeInputExceptionAuthority
			reason = 'Exact-HWND Win32SendInput foreground acquisition failed in the preceding live attempt; continue automated correctness checks without claiming native interaction.'
			actionCount = @($scenario.manualChecklist).Count
			recordCount = 0
			startedAtUtc = $manualStageStartedAtUtc.ToString('o')
			recordedAtUtc = [DateTimeOffset]::UtcNow.ToString('o')
			acceptanceEligible = $false
		}
		Write-JsonFile -Path (Join-Path $runRoot 'editor/native-input-exception.json') -Value $nativeInputException
		$manualResult = [ordered]@{
			schemaVersion = 1
			completed = $false
			recordCount = 0
			defects = @('Native editor interaction was not claimed because exact-HWND foreground input was unavailable.')
			nativeInputException = 'editor/native-input-exception.json'
		}
	}
	else {
		$manualResult = Wait-AndValidateClickEvidence -Scenario $scenario -ClickPath $clickPath -NativeEvidencePath $nativeEvidencePath -CompletionPath $completionPath -RunRoot $runRoot -EditorProcess $editorProcess -ManualStageStartedAtUtc $manualStageStartedAtUtc -TimeoutSeconds $ManualEvidenceTimeoutSeconds
	}
	Write-JsonFile -Path (Join-Path $runRoot 'editor/manual-evidence.json') -Value $manualResult
	$initialPlay = Invoke-McpTool -Name 'play.state'
	if (-not [bool]$initialPlay.envelope.ok -or [string]$initialPlay.envelope.data.currentState -ne 'stopped') { throw 'Manual workflow must leave the editor stopped before the scripted Play Mode proof.' }
	$manualSceneSave = Invoke-McpTool -Name 'scene.save'
	if (-not [bool]$manualSceneSave.envelope.ok) { throw 'The manually inspected scene could not be saved before the authored-state snapshot.' }
	Write-JsonFile -Path (Join-Path $runRoot 'editor/manual-scene-save.json') -Value ([ordered]@{ saved = $true; response = $manualSceneSave.envelope.data })
	# Snapshot after manual authoring so Play Mode restoration protects both the
	# scripted setup and the operator's edits. The explicit save above makes those
	# edits part of the file inventory instead of relying on an in-memory dirty flag.
	$authoredBeforeStability = Get-StableAuthoredProjectState -ProjectRoot $projectRoot
	$authoredBefore = $authoredBeforeStability.snapshot
	Write-JsonFile -Path (Join-Path $runRoot 'editor/authored-state-stability-before.json') -Value ([ordered]@{
		schemaVersion = 1
		boundary = 'before-play'
		stabilizedAtUtc = [string]$authoredBeforeStability.stabilizedAtUtc
		requiredConsecutiveObservations = [int]$authoredBeforeStability.requiredConsecutiveObservations
		sampleIntervalMilliseconds = [int]$authoredBeforeStability.sampleIntervalMilliseconds
		timeoutSeconds = [int]$authoredBeforeStability.timeoutSeconds
		finalSha256 = [string]$authoredBefore.sha256
		samples = @($authoredBeforeStability.samples)
	})
	$recordingOutputPreflight = @(Start-PlayModeRecordingOutputContract -Scenario $scenario -ProjectRoot $projectRoot -BeforeSnapshot $authoredBefore)
	$playModeBeforePath = Join-Path $runRoot 'editor/play-mode-before.json'
	Write-JsonFile -Path $playModeBeforePath -Value ([ordered]@{
		schemaVersion = 1
		phase = [string]$scenario.phase
		kind = 'before-play'
		capturedAtUtc = [DateTimeOffset]::UtcNow.ToString('o')
		playModeRecordingOutputs = @($scenario.playModeRecordingOutputs)
		creationOnlyPreflight = @($recordingOutputPreflight)
		snapshot = $authoredBefore
	})
	$playModeBeforeReference = [ordered]@{
		path = 'editor/play-mode-before.json'
		sha256 = Get-Sha256 -Path $playModeBeforePath
	}
	$enter = Invoke-McpTool -Name 'play.enter' -Arguments ([ordered]@{ expectedState = 'stopped'; correlationId = "animation-$Phase-enter" })
	$playEnterSequence = $script:McpSequence
	if (-not [bool]$enter.envelope.ok) { throw 'Play Mode entry failed.' }
	Invoke-ScenarioStage -Scenario $scenario -Stage 'play' -CameraRoot (Join-Path $runRoot 'editor/cameras') -RunRoot $runRoot -CameraInventory $cameraInventory -OperationInventory $operationInventory -BindingRegistry $argumentBindingRegistry -ApprovalRegistry $approvalRegistry -ApprovalTimeoutSeconds $ManualEvidenceTimeoutSeconds
	$currentPlay = Invoke-McpTool -Name 'play.state'
	$currentState = [string]$currentPlay.envelope.data.currentState
	if ($currentState -notin @('playing', 'paused')) { throw "Play Mode reached unexpected state '$currentState'." }
	$exit = Invoke-McpTool -Name 'play.exit' -Arguments ([ordered]@{ expectedState = $currentState; correlationId = "animation-$Phase-exit" })
	$playExitSequence = $script:McpSequence
	if (-not [bool]$exit.envelope.ok -or -not [bool]$exit.envelope.data.restoration.completed -or @($exit.envelope.data.restoration.partialFailures).Count -ne 0) { throw 'Play Mode exit did not report complete authored-state restoration.' }
	$finalPlay = Invoke-McpTool -Name 'play.state'
	if ([string]$finalPlay.envelope.data.currentState -ne 'stopped') { throw 'Editor did not return to stopped Play Mode.' }
	$authoredAfterStability = Get-StableAuthoredProjectState -ProjectRoot $projectRoot
	$authoredAfter = $authoredAfterStability.snapshot
	Write-JsonFile -Path (Join-Path $runRoot 'editor/authored-state-stability-after.json') -Value ([ordered]@{
		schemaVersion = 1
		boundary = 'after-play'
		stabilizedAtUtc = [string]$authoredAfterStability.stabilizedAtUtc
		requiredConsecutiveObservations = [int]$authoredAfterStability.requiredConsecutiveObservations
		sampleIntervalMilliseconds = [int]$authoredAfterStability.sampleIntervalMilliseconds
		timeoutSeconds = [int]$authoredAfterStability.timeoutSeconds
		finalSha256 = [string]$authoredAfter.sha256
		samples = @($authoredAfterStability.samples)
	})
	$recordingOutputState = Complete-PlayModeRecordingOutputContract -Scenario $scenario -ProjectRoot $projectRoot -BeforeSnapshot $authoredBefore -AfterSnapshot $authoredAfter -BindingRegistry $argumentBindingRegistry -OperationInventory $operationInventory -PlayEnterSequence $playEnterSequence -PlayExitSequence $playExitSequence
	Invoke-ScenarioStage -Scenario $scenario -Stage 'postPlay' -CameraRoot (Join-Path $runRoot 'editor/cameras') -RunRoot $runRoot -CameraInventory $cameraInventory -OperationInventory $operationInventory -BindingRegistry $argumentBindingRegistry -ApprovalRegistry $approvalRegistry -ApprovalTimeoutSeconds $ManualEvidenceTimeoutSeconds
	# Scenario mutations such as prefab instantiation intentionally make the
	# disposable scene dirty after the operator's earlier Ctrl+S proof. Save that
	# final isolated state through the same project-write API before requesting
	# shutdown, otherwise the native unsaved-changes dialog can outlive the gate's
	# bounded CloseMainWindow wait on an unattended or locked desktop.
	$finalSceneSave = Invoke-McpTool -Name 'scene.save'
	if (-not [bool]$finalSceneSave.envelope.ok) { throw 'Final isolated-scene save failed before installed-editor shutdown.' }
	Write-JsonFile -Path (Join-Path $runRoot 'editor/final-scene-save.json') -Value ([ordered]@{ saved = $true; response = $finalSceneSave.envelope.data })
	# First force the currently usable HTTP connection to close, then prove that
	# a new initialize request, tool discovery, and post-reconnect state query all
	# succeed in order. A second initialize on an unspecified pooled connection
	# would not be meaningful reconnect evidence.
	$reconnectBoundaryAtUtc = [DateTimeOffset]::UtcNow
	$reconnectBoundary = Invoke-McpRpc -Method 'initialize' -Params ([ordered]@{ protocolVersion = '2025-03-26'; clientInfo = [ordered]@{ name = 'animation-phase-gate-close-boundary'; version = '1' } }) -DisableKeepAlive
	$reconnectBoundarySequence = $script:McpSequence
	if ([string]$reconnectBoundary.result.serverInfo.name -ne 'TerraEngine Editor') { throw 'MCP connection-close boundary did not return the expected server identity.' }
	$reconnectAtUtc = [DateTimeOffset]::UtcNow
	$reconnect = Invoke-McpRpc -Method 'initialize' -Params ([ordered]@{ protocolVersion = '2025-03-26'; clientInfo = [ordered]@{ name = 'animation-phase-gate-fresh-reconnect'; version = '1' } })
	$reconnectSequence = $script:McpSequence
	if ([string]$reconnect.result.serverInfo.name -ne 'TerraEngine Editor') { throw 'MCP fresh-connection initialize evidence failed.' }
	$postReconnectTools = Invoke-McpRpc -Method 'tools/list'
	$postReconnectToolsSequence = $script:McpSequence
	$postReconnectToolNames = @($postReconnectTools.result.tools | ForEach-Object { [string]$_.name })
	foreach ($requiredTool in @($requiredTools | Select-Object -Unique)) {
		if ($postReconnectToolNames -notcontains $requiredTool) { throw "Fresh MCP connection does not advertise required tool '$requiredTool'." }
	}
	$postReconnectPlay = Invoke-McpTool -Name 'play.state'
	$postReconnectPlaySequence = $script:McpSequence
	if (-not [bool]$postReconnectPlay.envelope.ok -or [string]$postReconnectPlay.envelope.data.currentState -ne 'stopped') {
		throw 'Fresh MCP connection did not remain usable for a stopped-state tool operation.'
	}
	$reconnectEvidence = [ordered]@{
		schemaVersion = 1
		endpoint = $script:McpEndpoint
		connectionBoundary = [ordered]@{
			requestedAtUtc = $reconnectBoundaryAtUtc.ToString('o')
			sequence = $reconnectBoundarySequence
			disableKeepAlive = $true
			serverName = [string]$reconnectBoundary.result.serverInfo.name
		}
		freshInitialize = [ordered]@{
			requestedAtUtc = $reconnectAtUtc.ToString('o')
			sequence = $reconnectSequence
			serverName = [string]$reconnect.result.serverInfo.name
		}
		postReconnect = [ordered]@{
			toolListSequence = $postReconnectToolsSequence
			requiredToolsPresent = $true
			advertisedTools = @($postReconnectToolNames | Sort-Object)
			playStateSequence = $postReconnectPlaySequence
			playState = [string]$postReconnectPlay.envelope.data.currentState
			operationPassed = $true
		}
		passed = $true
		completedAtUtc = [DateTimeOffset]::UtcNow.ToString('o')
	}
	Write-JsonFile -Path (Join-Path $runRoot 'editor/mcp-reconnect.json') -Value $reconnectEvidence
	foreach ($view in @('SceneView', 'GameView', 'InGameCamera')) { if (@($cameraInventory | Where-Object { $_.view -eq $view }).Count -lt 1) { throw "Camera inventory is missing $view evidence." } }
	$cameraValidation = Get-CameraEvidenceValidation -Scenario $scenario -CameraInventory $cameraInventory
	Write-JsonFile -Path (Join-Path $runRoot 'editor/camera-evidence.json') -Value ([ordered]@{ schemaVersion = 2; resolution = '1920x1080'; captures = @($cameraInventory); validation = $cameraValidation; missingRequiredViews = @() })
	$operationEvidencePath = Join-Path $runRoot 'editor/mcp-operations.json'
	$mcpTranscriptReference = [ordered]@{
		path = 'editor/mcp.jsonl'
		sha256 = Get-Sha256 -Path $script:McpTranscriptPath
	}
	Write-JsonFile -Path $operationEvidencePath -Value ([ordered]@{
		schemaVersion = 2
		phase = [string]$scenario.phase
		declaredItemCount = @(@($scenario.operations) + @($scenario.captures)).Count
		operations = @($operationInventory)
		transcriptEvidence = $mcpTranscriptReference
		reconnectPassed = $true
		reconnectEvidence = [ordered]@{ path = 'editor/mcp-reconnect.json'; sha256 = Get-Sha256 -Path (Join-Path $runRoot 'editor/mcp-reconnect.json') }
	})
	$operationEvidenceValidation = Read-AndValidateOperationEvidence -Path $operationEvidencePath -Scenario $scenario -RunRoot $runRoot -MinimumSchemaVersion 2
	$operationEvidenceReference = [ordered]@{
		path = 'editor/mcp-operations.json'
		sha256 = Get-Sha256 -Path $operationEvidencePath
	}
	$playBoundary = [ordered]@{
		enterMcpSequence = $playEnterSequence
		exitMcpSequence = $playExitSequence
		enterEnvelopeSha256 = Get-StringSha256 -Value ($enter.envelope | ConvertTo-Json -Depth 80 -Compress)
		exitEnvelopeSha256 = Get-StringSha256 -Value ($exit.envelope | ConvertTo-Json -Depth 80 -Compress)
	}
	$recordingOutputProofs = [System.Collections.Generic.List[object]]::new()
	foreach ($proof in @($recordingOutputState.outputs)) {
		$recordingOutputProofs.Add([ordered]@{
			path = [string]$proof.path
			bytes = [UInt64]$proof.bytes
			sha256 = [string]$proof.sha256
			producerOperationId = [string]$proof.producerOperationId
			producerResponsePath = [string]$proof.producerResponsePath
			producerMcpSequence = [int]$proof.producerMcpSequence
			producerCompletionMcpSequence = [int]$proof.producerCompletionMcpSequence
			producerEnvelopeSha256 = [string]$proof.producerEnvelopeSha256
			producerApproval = $proof.producerApproval
			producerArtifact = $proof.producerArtifact
			operationEvidenceSha256 = [string]$operationEvidenceReference.sha256
			transcriptSha256 = [string]$mcpTranscriptReference.sha256
		})
	}
	$playModeAfterPath = Join-Path $runRoot 'editor/play-mode-after.json'
	Write-JsonFile -Path $playModeAfterPath -Value ([ordered]@{
		schemaVersion = 1
		phase = [string]$scenario.phase
		kind = 'after-play'
		capturedAtUtc = [DateTimeOffset]::UtcNow.ToString('o')
		beforeEvidence = $playModeBeforeReference
		operationEvidence = $operationEvidenceReference
		transcriptEvidence = $mcpTranscriptReference
		playBoundary = $playBoundary
		comparison = $recordingOutputState.comparison
		recordingOutputs = @($recordingOutputProofs)
		snapshot = $authoredAfter
	})
	$playModeAfterReference = [ordered]@{
		path = 'editor/play-mode-after.json'
		sha256 = Get-Sha256 -Path $playModeAfterPath
	}
	$playModeEvidencePath = Join-Path $runRoot 'editor/play-mode.json'
	Write-JsonFile -Path $playModeEvidencePath -Value ([ordered]@{
		schemaVersion = 2
		phase = [string]$scenario.phase
		entered = $true
		exited = $true
		authoredStateRestored = $true
		creationOnly = $true
		beforeSha256 = [string]$authoredBefore.sha256
		afterSha256 = [string]$authoredAfter.sha256
		excludedTransientState = $authoredBefore.excludedTransientState
		beforeEvidence = $playModeBeforeReference
		afterEvidence = $playModeAfterReference
		operationEvidence = $operationEvidenceReference
		transcriptEvidence = $mcpTranscriptReference
		playBoundary = $playBoundary
		recordingOutputs = @($recordingOutputProofs)
		enter = $enter.envelope.data
		exit = $exit.envelope.data
	})
	$playModeEvidenceValidation = Read-AndValidatePlayModeEvidence -Path $playModeEvidencePath -Scenario $scenario -RunRoot $runRoot -ProjectRoot $projectRoot
	$phaseEvidence.installedEditor | Add-Member -NotePropertyName operationEvidence -NotePropertyValue $operationEvidenceValidation -Force
	$phaseEvidence.installedEditor.clickTranscript = $clickPath; $phaseEvidence.installedEditor.mcpTranscript = $script:McpTranscriptPath
	$phaseEvidence.installedEditor.playMode.entered = $true; $phaseEvidence.installedEditor.playMode.exited = $true; $phaseEvidence.installedEditor.playMode.authoredStateRestored = $true; $phaseEvidence.installedEditor.playMode.beforeSha256 = $authoredBefore.sha256; $phaseEvidence.installedEditor.playMode.afterSha256 = $authoredAfter.sha256
	$phaseEvidence.installedEditor.playMode | Add-Member -NotePropertyName recordingOutputs -NotePropertyValue @($recordingOutputProofs) -Force
	$phaseEvidence.installedEditor.playMode | Add-Member -NotePropertyName evidenceValidation -NotePropertyValue $playModeEvidenceValidation -Force
	$phaseEvidence.installedEditor.cameraEvidence.sceneView = @($cameraInventory | Where-Object { $_.view -eq 'SceneView' }); $phaseEvidence.installedEditor.cameraEvidence.gameView = @($cameraInventory | Where-Object { $_.view -eq 'GameView' }); $phaseEvidence.installedEditor.cameraEvidence.inGame = @($cameraInventory | Where-Object { $_.view -eq 'InGameCamera' }); $phaseEvidence.installedEditor.cameraEvidence.missingRequiredViews = @()
	$phaseEvidence.installedEditor.cameraEvidence | Add-Member -NotePropertyName validation -NotePropertyValue $cameraValidation -Force

	if ($performanceDeferred) {
		$deferral = [ordered]@{
			schemaVersion = 1
			feature = $script:FeatureName
			phase = $Phase
			gateTask = $GateTask
			status = 'authorized-deferred-to-final-integration'
			authority = $PerformanceDeferralAuthority
			scopes = @('feature benchmarks', 'feature comparisons', 'performance acceptance gates', 'cumulative engine regression')
			finalTasks = @($script:OutstandingFinalPerformanceTasks)
			requirementsWaived = $false
			thresholdsChanged = $false
			baselineChanged = $false
			candidateGenerationExecuted = $false
			comparatorExecuted = $false
			measuredPerformanceCTestExclusion = [ordered]@{ ctestRegex = $script:DeferredPerformanceCTestExcludeRegex; harnessFilter = $script:DeferredPerformanceHarnessExcludeFilter; dormantStructuralCasesRetained = $true }
			lockedHashes = $lockedPerformanceInputs
		}
		Write-JsonFile -Path (Join-Path $runRoot 'performance/deferral.json') -Value $deferral
		$phaseEvidence.performance.executionStatus = 'authorized-deferred-to-final-integration'
		$phaseEvidence.performance.authorizedPerformanceDeferral = $true
		$phaseEvidence.performance.finalIntegrationBlockedOnPerformance = $true
		$phaseEvidence.performance.outstandingTasks = @($script:OutstandingFinalPerformanceTasks)
		$phaseEvidence.performance.deferralEvidence = 'performance/deferral.json'
		# Null means deliberately not measured. False would incorrectly describe
		# the user-authorized schedule as a failed performance result.
		$phaseEvidence.performance.identityComparable = $null
		# The gate verifies every locked contract/workload hash before reaching
		# this branch, so deferral preserves (rather than evaluates) thresholds.
		$phaseEvidence.performance.thresholdsUnchanged = $true
		$phaseEvidence.performance.repeatabilityAccepted = $null
		$phaseEvidence.performance.passed = $null
	}
	else {
		$comparatorPath = Join-Path $repoRoot 'Tools/Animation/CompareAnimationPerformance.ps1'
		if (-not (Test-Path -LiteralPath $comparatorPath -PathType Leaf)) { throw 'Performance comparator is missing.' }
		$performanceOutput = Join-Path $runRoot 'performance/comparison.json'
		$currentPowerShell = (Get-Process -Id $PID).Path
		$performanceRecord = Invoke-BoundedProcess -Stage 'performance-comparison' -FilePath $currentPowerShell -Arguments @('-NoProfile', '-ExecutionPolicy', 'Bypass', '-File', $comparatorPath, '-CandidateManifest', $resolvedCandidate, '-OutputPath', $performanceOutput) -WorkingDirectory $repoRoot -OutputRoot (Join-Path $runRoot 'performance') -TimeoutSeconds $TestTimeoutSeconds
		if (-not (Test-Path -LiteralPath $performanceOutput -PathType Leaf)) { throw 'Performance comparator omitted its result JSON.' }
		$performance = Get-Content -Raw -LiteralPath $performanceOutput | ConvertFrom-Json
		if (-not [bool]$performance.passed -or [string]$performance.outcome -ne 'passed' -or -not [bool]$performance.identity.passed -or -not [bool]$performance.thresholdIntegrity.passed) { throw 'Performance comparison rejected identity, required metrics, or unchanged thresholds.' }
		if ($isFinalGate -and ([string]$performance.feature -ne $script:FeatureName -or [string]$performance.task -ne 'AnimationValidationLaterPerformanceRelatedTasksMayConstructComparatorsFixturesInstrumentationSchemas' -or [string]$performance.mode -ne 'comparison')) { throw 'Final engine-wide comparison omitted the expected feature/AnimationValidationLaterPerformanceRelatedTasksMayConstructComparatorsFixturesInstrumentationSchemas/comparison identity.' }
		$phaseEvidence.performance.executionStatus = 'passed-non-deferred'
		$phaseEvidence.performance.authorizedPerformanceDeferral = $false
		$phaseEvidence.performance.finalIntegrationBlockedOnPerformance = $false
		$phaseEvidence.performance.outstandingTasks = @()
		$phaseEvidence.performance.finalAnimationPerformanceEvidence = $finalAnimationPerformanceEvidence
		$phaseEvidence.performance.minimumMeasuredRuns = 5; $phaseEvidence.performance.identityComparable = $true; $phaseEvidence.performance.thresholdsUnchanged = $true; $phaseEvidence.performance.repeatabilityAccepted = $true; $phaseEvidence.performance.passed = $true; $phaseEvidence.performance.requiredMetricsMissing = @()
	}
	$baselineAfter = Get-BaselineLockState -BaselineRoot $baselineRoot
	if ($baselineAfter.aggregateSha256 -cne $baselineBefore.aggregateSha256 -or $baselineAfter.lockSha256 -cne $baselineBefore.lockSha256) { throw 'Immutable baseline changed during the candidate gate.' }
	if ($null -ne $nativeInputException) {
		# Throw only after every automatable live check has completed. The terminal
		# result remains failed/blocked, which prevents a skipped click path from
		# being mistaken for formal live-gate or acceptance evidence.
		throw 'Automated installed-editor correctness checks completed, but formal gate acceptance is blocked by the authorized native-input exception.'
	}

	$requiredArtifacts = Assert-RequiredEvidence -RunRoot $runRoot -Scenario $scenario -PerformanceDeferred $performanceDeferred -FinalGate $isFinalGate
	$phaseEvidence.commands = @($script:CommandRecords)
	$phaseEvidence.artifacts = @($requiredArtifacts)
	$phaseEvidence.defects = @($manualResult.defects)
	$phaseEvidence.completeness.requiredFieldsPresent = $true; $phaseEvidence.completeness.requiredArtifactsPresent = $true; $phaseEvidence.completeness.unexpectedUnavailableEvidence = @(); $phaseEvidence.completeness.failClosed = $true
	$phaseEvidence.status = 'passed'; $phaseEvidence.completedAt = (Get-Date).ToString('o'); $phaseEvidence.result.passed = $true; $phaseEvidence.result.failureCategory = ''; $phaseEvidence.result.message = if ($performanceDeferred) { 'All required intermediate correctness, build, install, installed-editor, MCP, camera, Play Mode, and evidence checks passed; performance remains authorized and blocked on final integration. Acceptance commit remains pending until this evidence is locked.' } else { 'All required build, test, install, installed-editor, MCP, camera, Play Mode, performance, and evidence checks passed. Acceptance commit remains pending until this evidence is locked.' }; $phaseEvidence.result.nextPhaseUnblocked = $false
	$phaseEvidence.result.authorizedPerformanceDeferral = $performanceDeferred; $phaseEvidence.result.finalIntegrationBlockedOnPerformance = $performanceDeferred; $phaseEvidence.result.outstandingPerformanceTasks = @($currentOutstandingPerformanceTasks)
	Write-JsonFile -Path (Join-Path $runRoot 'phase-evidence.json') -Value $phaseEvidence

	# Acceptance may advance only after the installed editor exits normally. The
	# exact compare-and-swap update is prepared here, but it is not committed until
	# result.json and the immutable evidence lock both exist.
	$gracefulShutdownAttempted = $true
	[void]$editorProcess.CloseMainWindow()
	if (-not $editorProcess.WaitForExit(60000)) { throw 'Installed editor did not shut down normally within 60 seconds.' }
	if ($editorProcess.ExitCode -ne 0) { throw "Installed editor shutdown returned exit code $($editorProcess.ExitCode)." }
	$phaseEvidence.installedEditor.normalShutdown = $true
	Write-JsonFile -Path (Join-Path $runRoot 'phase-evidence.json') -Value $phaseEvidence

	$acceptancePlan = New-AcceptanceMatrixUpdate `
		-MatrixPath (Join-Path $repoRoot 'specs/complete-animation-system/acceptance-matrix.md') `
		-UpdateManifestPath $resolvedAcceptance `
		-RepoRoot $repoRoot `
		-RunRoot $runRoot `
		-Scenario $scenario `
		-OperationInventory $operationInventory `
		-CameraInventory $cameraInventory `
		-ManualEvidence $manualResult `
		-PerformanceDeferred $performanceDeferred
	$acceptanceResult = [ordered]@{
		matrix = 'specs/complete-animation-system/acceptance-matrix.md'
		updateApplied = $false
		transactionState = 'prepared-before-evidence-lock'
		rowsEvaluated = @($acceptancePlan.rowsEvaluated)
		rowsAccepted = @($acceptancePlan.rowsAccepted)
		rowsBlocked = @($acceptancePlan.rowsBlocked)
		beforeSha256 = [string]$acceptancePlan.beforeSha256
		proposedAfterSha256 = [string]$acceptancePlan.proposedAfterSha256
		receipt = Get-RelativeForwardPath -Parent $repoRoot -Child $acceptanceReceiptPath
	}
	$phaseEvidence.acceptance = $acceptanceResult
	Write-JsonFile -Path (Join-Path $runRoot 'phase-evidence.json') -Value $phaseEvidence
}
catch {
	$failure = $_
}
finally {
	if ($null -ne $editorProcess) {
		try {
			$editorProcess.Refresh()
			if (-not $editorProcess.HasExited -and -not $gracefulShutdownAttempted) {
				$gracefulShutdownAttempted = $true
				[void]$editorProcess.CloseMainWindow()
				[void]$editorProcess.WaitForExit(60000)
				$editorProcess.Refresh()
			}
		}
		catch {
			$cleanupFailures.Add("Graceful editor shutdown check failed: $($_.Exception.Message)")
		}

		try {
			$editorProcess.Refresh()
			if (-not $editorProcess.HasExited) {
				$forcedTermination = $true
				# The Job Object owns the complete editor/compiler descendant tree.
				# Terminating it prevents a child process from retaining redirected
				# pipes or the loopback MCP listener after the root editor exits.
				if ($null -ne $editorJob) {
					if (-not $editorJob.Terminate(1)) {
						throw 'TerminateJobObject returned failure for the installed editor process tree.'
					}
				}
				else {
					$editorProcess.Kill()
				}
				[void]$editorProcess.WaitForExit(10000)
				$editorProcess.Refresh()
				if (-not $editorProcess.HasExited) {
					$cleanupFailures.Add('Installed editor remained alive after the bounded forced-termination wait.')
				}
				else {
					$cleanupFailures.Add('Installed editor required forced Job Object termination.')
				}
			}
		}
		catch {
			$cleanupFailures.Add("Forced editor-tree termination failed: $($_.Exception.Message)")
		}

		# Closing a kill-on-close Job Object also removes surviving descendants
		# after a normal root-process exit. Do this before draining redirected
		# streams so descendants cannot keep those pipes open indefinitely.
		if ($null -ne $editorJob) {
			try { $editorJob.Dispose() }
			catch { $cleanupFailures.Add("Editor Job Object disposal failed: $($_.Exception.Message)") }
			$editorJob = $null
		}

		try {
			$editorProcess.Refresh()
			$editorExited = [bool]$editorProcess.HasExited
			if ($editorExited) { $editorExitCode = [int]$editorProcess.ExitCode }
		}
		catch {
			$cleanupFailures.Add("Editor exit-state capture failed: $($_.Exception.Message)")
		}

		if ($runCreated) {
			foreach ($streamCapture in @(
				[ordered]@{ name = 'stdout'; task = $editorStdoutTask; path = (Join-Path $runRoot 'editor/editor.stdout.log') },
				[ordered]@{ name = 'stderr'; task = $editorStderrTask; path = (Join-Path $runRoot 'editor/editor.stderr.log') }
			)) {
				if ($null -eq $streamCapture.task) { continue }
				try {
					if ($streamCapture.task.Wait(10000)) {
						Write-Utf8Text -Path $streamCapture.path -Value $streamCapture.task.GetAwaiter().GetResult()
					}
					else {
						Write-Utf8Text -Path $streamCapture.path -Value "[phase-gate] $($streamCapture.name) drain exceeded the 10-second cleanup bound."
						$cleanupFailures.Add("Editor $($streamCapture.name) stream did not drain within 10 seconds.")
					}
				}
				catch {
					$cleanupFailures.Add("Editor $($streamCapture.name) stream capture failed: $($_.Exception.Message)")
				}
			}
		}

		$portReleaseDeadline = [DateTimeOffset]::UtcNow.AddSeconds(10)
		do {
			$mcpPortReleased = Test-LoopbackPortAvailable -Port $McpPort
			if ($mcpPortReleased) { break }
			Start-Sleep -Milliseconds 100
		} while ([DateTimeOffset]::UtcNow -lt $portReleaseDeadline)
		if (-not $mcpPortReleased) {
			$cleanupFailures.Add("MCP loopback port $McpPort remained occupied after editor-tree cleanup.")
		}

		try { $editorProcess.Dispose() }
		catch { $cleanupFailures.Add("Editor process handle disposal failed: $($_.Exception.Message)") }
	}
}

if ($cleanupFailures.Count -gt 0 -and $null -eq $failure) {
	$failure = [System.Management.Automation.ErrorRecord]::new(
		[Exception]::new("Editor cleanup failed: $($cleanupFailures -join '; ')"),
		'EditorCleanup',
		[System.Management.Automation.ErrorCategory]::OperationStopped,
		$null)
}

if (-not $Execute) {
	if ($null -ne $failure) {
		$dryRunFailure = [ordered]@{
			schemaVersion = 1
			tool = 'RunAnimationValidationGate'
			feature = $script:FeatureName
			phase = $Phase
			gateTask = $GateTask
			mode = 'dry-run'
			passed = $false
			outcome = 'invalid'
			exitCode = 1
			failClosed = $true
			message = $failure.Exception.Message
		}
		$dryRunFailure | ConvertTo-Json -Depth 10 -Compress | Write-Output
		exit 1
	}
	return
}

$vulkanValidationEvidence = $null
if ($runCreated -and $null -ne $editorProcess) {
	try {
		$vulkanValidationEvidence = Get-VulkanValidationLogEvidence `
			-StdoutPath (Join-Path $runRoot 'editor/editor.stdout.log') `
			-StderrPath (Join-Path $runRoot 'editor/editor.stderr.log')
		Write-JsonFile -Path (Join-Path $runRoot 'editor/vulkan-validation-log.json') -Value $vulkanValidationEvidence
		if (-not [bool]$vulkanValidationEvidence.passed -and $null -eq $failure) {
			$vuidSummary = @($vulkanValidationEvidence.vuidIds) -join ', '
			$failure = [System.Management.Automation.ErrorRecord]::new(
				[Exception]::new("Installed editor emitted Vulkan validation errors. VUIDs: [$vuidSummary]."),
				'VulkanValidationError',
				[System.Management.Automation.ErrorCategory]::InvalidData,
				$null)
		}
	}
	catch {
		if ($null -eq $failure) { $failure = $_ }
		else { $cleanupFailures.Add("Vulkan validation-log inspection failed: $($_.Exception.Message)") }
	}
}

$normalEditorShutdown = $null -ne $editorProcess -and $editorExited -and $editorExitCode -eq 0 -and -not $forcedTermination
if ($runCreated -and $null -ne $phaseEvidence) {
	$phaseEvidence.installedEditor.normalShutdown = $normalEditorShutdown
	$phaseEvidence.installedEditor | Add-Member -NotePropertyName cleanup -NotePropertyValue ([ordered]@{
		jobObjectCreated = $editorJobCreated
		gracefulShutdownAttempted = $gracefulShutdownAttempted
		forcedTermination = $forcedTermination
		processExited = $editorExited
		exitCode = $editorExitCode
		streamDrainTimeoutSeconds = 10
		mcpPort = $McpPort
		mcpPortReleased = $mcpPortReleased
		failures = @($cleanupFailures)
	}) -Force
}
$passed = $null -eq $failure -and $cleanupFailures.Count -eq 0 -and $normalEditorShutdown
if ($null -eq $failure -and -not $normalEditorShutdown) {
	$failure = [System.Management.Automation.ErrorRecord]::new(
		[Exception]::new('Installed editor did not shut down normally.'),
		'EditorShutdown',
		[System.Management.Automation.ErrorCategory]::OperationStopped,
		$null)
	$passed = $false
}
if ($null -ne $failure -and $runCreated -and $null -ne $phaseEvidence) {
	$phaseEvidence.status = 'failed'; $phaseEvidence.completedAt = (Get-Date).ToString('o'); $phaseEvidence.commands = @($script:CommandRecords); $phaseEvidence.acceptance = $acceptanceResult
	$phaseEvidence.result.passed = $false; $phaseEvidence.result.failureCategory = 'phase-gate'; $phaseEvidence.result.message = $failure.Exception.Message; $phaseEvidence.result.nextPhaseUnblocked = $false
	Write-JsonFile -Path (Join-Path $runRoot 'phase-evidence.json') -Value $phaseEvidence
}
elseif ($passed) {
	Write-JsonFile -Path (Join-Path $runRoot 'phase-evidence.json') -Value $phaseEvidence
}

$resultFailures = [System.Collections.Generic.List[object]]::new()
if ($null -ne $failure -and -not ($failure.FullyQualifiedErrorId -eq 'EditorCleanup')) {
	$resultFailures.Add([ordered]@{ message = $failure.Exception.Message; type = $failure.Exception.GetType().FullName })
}
foreach ($cleanupFailure in $cleanupFailures) {
	$resultFailures.Add([ordered]@{ message = [string]$cleanupFailure; type = 'EditorCleanupFailure' })
}
if ($resultFailures.Count -eq 0 -and $null -ne $failure) {
	$resultFailures.Add([ordered]@{ message = $failure.Exception.Message; type = $failure.Exception.GetType().FullName })
}

$result = [ordered]@{
	schemaVersion = 1
	tool = 'RunAnimationValidationGate'
	feature = $script:FeatureName
	task = 'AnimationValidationTGateOrchestratorInstalledEditorLaunchIsolatedFixtureCopiesMcp'
	gateTask = $GateTask
	phase = $Phase
	runId = $RunId
	passed = $passed
	outcome = if ($passed) { 'passed' } else { 'failed' }
	exitCode = if ($passed) { 0 } else { 1 }
	authorizedPerformanceDeferral = $performanceDeferred
	finalIntegrationBlockedOnPerformance = $performanceDeferred
	outstandingPerformanceTasks = @($currentOutstandingPerformanceTasks)
	startedAt = $startedAt.ToString('o')
	completedAt = (Get-Date).ToString('o')
	durationSeconds = [Math]::Round(((Get-Date) - $startedAt).TotalSeconds, 3)
	nextPhaseUnblocked = $passed -and $null -eq $acceptancePlan
	resultRole = if ($null -ne $acceptancePlan) { 'immutable-pre-acceptance-validation' } else { 'terminal-validation-result' }
	acceptanceCommitRequired = $passed -and $null -ne $acceptancePlan
	acceptanceState = if (-not $passed) { 'not-applicable-validation-failed' } elseif ($null -eq $acceptancePlan) { 'not-required' } else { 'pending-external-commit-receipt' }
	failClosed = $true
	fixedProfile = [ordered]@{ configuration = 'RelWithDebInfo'; workerCount = 31; resolution = '1920x1080'; vsync = $false; missingEvidence = 'reject'; thresholdWeakening = 'reject' }
	baseline = if (Get-Variable baselineBefore -ErrorAction SilentlyContinue) { [ordered]@{ aggregateSha256 = $baselineBefore.aggregateSha256; unchanged = $passed } } else { $null }
	stages = @($script:StageRecords)
	acceptance = $acceptanceResult
	forcedEditorTermination = $forcedTermination
	cleanup = [ordered]@{ mcpPortReleased = $mcpPortReleased; failures = @($cleanupFailures) }
	failures = @($resultFailures)
	evidenceRoot = $runRoot
}

if ($runCreated) {
	Write-JsonFile -Path $resultPath -Value $result
	# result.json is written before the evidence lock and is never rewritten on
	# success, keeping every hash recorded by the immutable manifest truthful.
	$evidenceLocked = $false
	try {
		[void](Write-EvidenceLock -RunRoot $runRoot)
		$evidenceLocked = $true
	}
	catch {
		if ($passed) {
			$passed = $false
			$result.passed = $false
			$result.outcome = 'failed'
			$result.exitCode = 1
			$result.nextPhaseUnblocked = $false
			$result.failures += [ordered]@{ message = $_.Exception.Message; type = $_.Exception.GetType().FullName }
			# The evidence lock did not succeed, so result.json is not immutable and
			# may truthfully record the locking failure.
			Write-JsonFile -Path $resultPath -Value $result
		}
	}
	if ($passed -and $evidenceLocked -and $null -ne $acceptancePlan) {
		$receiptTempPath = "$acceptanceReceiptPath.tmp"
		try {
			$receipt = [ordered]@{
				schemaVersion = 1
				receiptRole = 'terminal-acceptance-transaction-result'
				feature = $script:FeatureName
				phase = $Phase
				runId = $RunId
				gateTask = $GateTask
				updateApplied = $true
				transactionState = 'committed-after-evidence-lock'
				effectiveOutcome = 'passed'
				nextPhaseUnblocked = $true
				evidenceLock = Get-RelativeForwardPath -Parent $repoRoot -Child (Join-Path $runRoot 'evidence-lock.json')
				evidenceLockSha256 = Get-Sha256 -Path (Join-Path $runRoot 'evidence-lock.json')
				preCommitResult = Get-RelativeForwardPath -Parent $repoRoot -Child $resultPath
				preCommitResultSha256 = Get-Sha256 -Path $resultPath
				preCommitResultState = 'validation-passed-acceptance-pending'
				matrix = 'specs/complete-animation-system/acceptance-matrix.md'
				beforeSha256 = [string]$acceptancePlan.beforeSha256
				afterSha256 = [string]$acceptancePlan.proposedAfterSha256
				rowsEvaluated = @($acceptancePlan.rowsEvaluated)
				rowsAccepted = @($acceptancePlan.rowsAccepted)
				rowsBlocked = @($acceptancePlan.rowsBlocked)
				committedAtUtc = [DateTimeOffset]::UtcNow.ToString('o')
			}
			# Prepare the receipt before touching the matrix. Its final rename occurs
			# only after the compare-and-swap update succeeds.
			Write-JsonFile -Path $receiptTempPath -Value $receipt
			$acceptanceResult = Apply-AcceptanceMatrixUpdate -Plan $acceptancePlan
			Move-Item -LiteralPath $receiptTempPath -Destination $acceptanceReceiptPath
			$result.acceptance = $acceptanceResult
			$result | Add-Member -NotePropertyName acceptanceReceipt -NotePropertyValue (Get-RelativeForwardPath -Parent $repoRoot -Child $acceptanceReceiptPath) -Force
			$result.nextPhaseUnblocked = $true
			$result.resultRole = 'terminal-console-result-backed-by-acceptance-receipt'
			$result.acceptanceState = 'committed-by-external-receipt'
		}
		catch {
			$commitFailure = $_
			$rollbackFailure = $null
			try {
				if ((Get-Sha256 -Path ([string]$acceptancePlan.matrixPath)) -ceq [string]$acceptancePlan.proposedAfterSha256) {
					Restore-AcceptanceMatrixUpdate -Plan $acceptancePlan
				}
			}
			catch {
				$rollbackFailure = $_
			}
			$passed = $false
			$result.passed = $false
			$result.outcome = 'failed'
			$result.exitCode = 1
			$result.nextPhaseUnblocked = $false
			$result.acceptance = $acceptanceResult
			$result.failures += [ordered]@{ message = "Acceptance commit failed after evidence lock: $($commitFailure.Exception.Message)"; type = $commitFailure.Exception.GetType().FullName }
			if ($null -ne $rollbackFailure) {
				$result.failures += [ordered]@{ message = "Acceptance rollback also failed: $($rollbackFailure.Exception.Message)"; type = $rollbackFailure.Exception.GetType().FullName }
			}
			if (Test-Path -LiteralPath $receiptTempPath -PathType Leaf) {
				# The exact phase-local temporary receipt is not authoritative. A
				# cleanup failure must not hide the original transaction failure.
				Remove-Item -LiteralPath $receiptTempPath -Force -ErrorAction SilentlyContinue
			}
		}
	}
}
$result | ConvertTo-Json -Depth 30 -Compress | Write-Output
if (-not $passed) { exit 1 }
