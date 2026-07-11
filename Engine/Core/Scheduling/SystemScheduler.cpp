// SystemScheduler.cpp - validation and single-threaded execution for systems.
//
// This implementation deliberately stops at deterministic ordering and
// metadata validation. It does not create worker jobs yet; the ordered graph is
// shaped so Part 2 can lower the same data into dependency handles and barriers.

#include "SystemScheduler.h"

#include "Core/SystemBase.h"
#include "JobGraph.h"
#include "Profiling/ProfilerScope.h"
#include "Scene/SceneMutationQueue.h"

#include <algorithm>
#include <iterator>
#include <map>
#include <queue>
#include <set>
#include <sstream>
#include <tuple>
#include <unordered_map>
#include <unordered_set>
#include <utility>

namespace TerraEngine::Scheduling {
	namespace {
		bool IsError(const ScheduleDiagnostic& diagnostic)
		{
			return diagnostic.severity == DiagnosticSeverity::Error;
		}

		void AddDiagnostic(std::vector<ScheduleDiagnostic>& diagnostics, DiagnosticSeverity severity, DiagnosticCategory category, std::string systemName, std::string phaseName, std::string resourceName, std::string message, std::string action)
		{
			diagnostics.push_back({
				severity,
				category,
				std::move(systemName),
				std::move(phaseName),
				std::move(resourceName),
				std::move(message),
				std::move(action)
			});
		}

		int SeverityRank(DiagnosticSeverity severity)
		{
			switch (severity)
			{
			case DiagnosticSeverity::Error:
				return 0;
			case DiagnosticSeverity::Warning:
				return 1;
			case DiagnosticSeverity::Info:
			default:
				return 2;
			}
		}

		void SortDiagnostics(std::vector<ScheduleDiagnostic>& diagnostics)
		{
			std::stable_sort(diagnostics.begin(), diagnostics.end(), [](const ScheduleDiagnostic& left, const ScheduleDiagnostic& right) {
				return std::make_tuple(SeverityRank(left.severity), static_cast<int>(left.category), left.systemName, left.phaseName, left.resourceName, left.message, left.recommendedAction)
					< std::make_tuple(SeverityRank(right.severity), static_cast<int>(right.category), right.systemName, right.phaseName, right.resourceName, right.message, right.recommendedAction);
			});
		}

		std::string FormatFlushPoint(const MutationFlushPoint& point)
		{
			std::ostringstream stream;
			stream << point.name << " (" << ToString(point.position);
			if (!point.phaseName.empty())
			{
				stream << " " << point.phaseName;
			}
			stream << ")";
			return stream.str();
		}

		std::vector<ResourceAccess> CollectAccesses(const SystemDeclaration& declaration)
		{
			std::vector<ResourceAccess> accesses;
			for (const std::string& read : declaration.reads)
			{
				accesses.push_back({ read, ResourceAccessMode::Read });
			}
			for (const std::string& write : declaration.writes)
			{
				accesses.push_back({ write, ResourceAccessMode::Write });
			}
			return accesses;
		}

		bool HasWriteConflict(const ResourceAccess& left, const ResourceAccess& right)
		{
			return left.resourceName == right.resourceName
				&& (left.mode == ResourceAccessMode::Write || right.mode == ResourceAccessMode::Write);
		}

		SystemDeclaration NormalizeDeclaration(SystemsBase& system, SystemDeclaration declaration)
		{
			if (declaration.name.empty())
			{
				declaration.name = system.SystemName();
			}
			if (declaration.profilingLabel.empty())
			{
				declaration.profilingLabel = declaration.name;
			}
			if (declaration.phase.empty())
			{
				declaration.phase = "PostFrame";
			}
			return declaration;
		}

		bool FlushRunsBeforeSystem(const MutationFlushPoint& flushPoint, const SystemDeclaration& declaration, const std::unordered_map<std::string, int>& phaseOrder)
		{
			if (std::find(flushPoint.requiredBeforeSystems.begin(), flushPoint.requiredBeforeSystems.end(), declaration.name) != flushPoint.requiredBeforeSystems.end())
			{
				return true;
			}

			const auto systemPhase = phaseOrder.find(declaration.phase);
			const auto flushPhase = phaseOrder.find(flushPoint.phaseName);
			if (systemPhase == phaseOrder.end())
			{
				return false;
			}

			switch (flushPoint.position)
			{
			case MutationFlushPosition::BeforePhase:
				return flushPhase != phaseOrder.end() && flushPhase->second <= systemPhase->second;
			case MutationFlushPosition::AfterPhase:
				return flushPhase != phaseOrder.end() && flushPhase->second < systemPhase->second;
			case MutationFlushPosition::FrameEnd:
			default:
				return false;
			}
		}

		bool HasPath(std::size_t from, std::size_t to, const std::vector<std::vector<bool>>& reachability)
		{
			return from < reachability.size() && to < reachability.size() && reachability[from][to];
		}

		bool IsTransientFrameDiagnostic(const ScheduleDiagnostic& diagnostic)
		{
			return diagnostic.systemName == "SystemScheduler"
				&& (diagnostic.category == DiagnosticCategory::Mutation || diagnostic.category == DiagnosticCategory::Scheduler)
				&& (diagnostic.message.find("frame workload execution") != std::string::npos
					|| diagnostic.message.find("frame-end flush") != std::string::npos
					|| diagnostic.message.find("Mutation flush point did not run") != std::string::npos);
		}

		bool CanRunWholeSystemUpdateOnWorker(const SystemDeclaration& declaration)
		{
			// A scheduler declaration can expose worker-safe preparation subwork
			// while SystemsBase::Update still performs protected commits such as
			// presentation, managed callbacks, or editor state mutation. The live
			// frame path only moves the whole Update call to a worker after the
			// system explicitly advertises a fully worker-safe boundary.
			return !declaration.mainThread
				&& declaration.boundaryClassification == HardBoundaryClassification::WorkerSafe;
		}
	}

	std::vector<SchedulerPhaseDefinition> DefaultSchedulerPhases()
	{
		return {
			{ "Startup", 0, true, false, false, false, "One-shot startup and boot validation." },
			{ "PreFrame", 10, true, false, false, false, "Per-frame platform and window preparation." },
			{ "Input", 20, true, false, false, false, "Input device polling and state updates." },
			{ "Scene", 30, true, false, false, true, "Scene loading, hierarchy state, and authored scene data." },
			{ "Scripting", 40, true, false, false, true, "Gameplay script execution and managed runtime updates." },
			{ "UI", 50, true, false, false, false, "Runtime UI layout and CPU-side UI data." },
			{ "Render", 60, true, false, false, false, "Renderer submission and presentation." },
			{ "PostFrame", 70, true, false, false, false, "Frame-end cleanup and compatibility defaults." }
		};
	}

	std::vector<SchedulerResourceDefinition> DefaultSchedulerResources()
	{
		return {
			{ "Window", "Native window and swapchain-facing platform state.", "" },
			{ "Input", "Input backend and per-frame input state.", "" },
			{ "SceneGraph", "Hierarchy and transform visibility for scene objects.", "Refined by safe transform and scene graph jobs in Part 3." },
			{ "HierarchySnapshot", "Immutable parent-before-child hierarchy data published for worker readers.", "Prepared from SceneGraph before transform and layout jobs run." },
			{ "DirtyTransformState", "Dirty transform roots and expanded branch state used to limit worker transform work.", "Prepared from SceneGraph before transform work units are planned." },
			{ "TransformWorkUnits", "Depth-ordered transform propagation batches that worker jobs can execute independently.", "Consumes hierarchy and dirty transform inputs before runtime layout publication." },
			{ "RuntimeLayout", "Compact active-object, transform, render, camera, and light records shared with worker-safe consumers.", "Published after hierarchy and transform preparation completes." },
			{ "ObjectManager", "Entity/object storage and component ownership.", "Refined by component read/write query declarations in Part 3." },
			{ "SceneMutationQueue", "Deferred structural scene mutation requests.", "Feeds deferred entity/component mutation in Part 3." },
			{ "Scripting", "Managed runtime, script assemblies, and script component state.", "May later expose script component query resources." },
			{ "UI", "Runtime UI document, style, and generated geometry state.", "" },
			{ "Renderer", "Renderer frame graph, GPU submission, and render backend state.", "" },
			{ "ActivePresentationState", "Read-only active swapchain state and presentation generation.", "" },
			{ "PresentationState", "Protected presentation lifecycle and recreation state.", "" },
			{ "PresentationGeneration", "Monotonic generation published after complete presentation recreation.", "" },
			{ "EditorSubmissionCompletion", "Fence-backed editor UI submission serial progress.", "" },
			{ "RendererDiagnostics", "Stable renderer and presentation diagnostic snapshots.", "" },
			{ "RendererOutput", "Renderer output images and immutable editor-facing handles.", "" },
			{ "WindowSnapshot", "Immutable window state observed by protected editor UI work.", "" },
			{ "InputSnapshot", "Immutable input state observed by protected editor UI work.", "" },
			{ "ProjectState", "Editor project lifecycle and settings state.", "" },
			{ "EditorState", "Native and managed editor state.", "" },
			{ "ImGuiDrawData", "Final CPU draw data prepared for protected backend recording.", "" },
			{ "EditorUiPresentation", "Protected native editor UI acquire, submit, and present state.", "" },
			{ "ViewportTextureRegistry", "Scene and Game descriptor registration and retirement ownership.", "" },
			{ "EditorDiagnostics", "Editor-facing UI submission and presentation diagnostics.", "" },
			{ "EditorAutomationState", "Editor automation and MCP-visible UI state.", "" },
			{ kResourceVirtualGeometryQualityProfile, "Renderer-owned virtual geometry quality, LOD, Hi-Z, debug visualization, and budget settings.", "Read by render preparation when choosing cluster packet detail and page request pressure." },
			{ kResourceVirtualGeometryResidencySnapshots, "Compatibility alias for immutable virtual geometry page residency state prepared before render selection.", "Kept while older RenderSystem declarations migrate to the explicit page-residency resource name." },
			{ kResourceVirtualGeometryPageResidencySnapshots, "Immutable virtual geometry page residency state prepared before render selection.", "Read by render preparation; publication remains protected by ResourceManager/resource residency boundaries." },
			{ kResourceVirtualGeometryHiZValidationState, "Per-view Hi-Z validation stamps that prove an occlusion pyramid matches the current camera, viewport, and depth resource.", "Read by render preparation before task-shader occlusion is allowed." },
			{ kResourceVirtualGeometryClusterPackets, "Frame-local selected virtual geometry cluster packets prepared from per-view cluster cuts.", "Written by render preparation and read by batching, diagnostics, and backend packet upload planning." },
			{ kResourceVirtualGeometryBatchPlans, "Frame-local virtual geometry work batches grouped from compatible cluster packets.", "Written by render preparation so backend submission can avoid one draw per selected cluster." },
			{ kResourceVirtualGeometryGpuPacketFrameSets, "Per-view GPU-visible virtual geometry packet, batch, compacted packet, generated submission, and counter buffer ownership records.", "Written by render preparation/protected Vulkan publication and read by command submission, diagnostics, and performance gates." },
			{ kResourceVirtualGeometryGpuCompactionDispatches, "Immutable virtual geometry GPU compaction dispatch plans built from packet frame sets.", "Prepared from per-view packets before protected Vulkan dispatch and validated before generated submissions are trusted." },
			{ kResourceVirtualGeometryGeneratedSubmissions, "Validated virtual geometry generated submission records derived from compacted GPU packets.", "Read by Vulkan submission and diagnostics; invalid or stale records fail open to the correctness fallback path." },
			{ kResourceVirtualGeometryCounterReadbacks, "Source-labeled virtual geometry GPU counter readbacks for packets, compaction, generated submissions, and meshlet culling.", "Read by overlays, automation, profiler, and gates without treating unavailable counters as authoritative zeroes." },
			{ kResourceVirtualGeometryCompactionResults, "Prepared or GPU-readback virtual geometry compaction counters for rejected, visible, and fail-open packets.", "Written after selection/compaction and read by diagnostics and performance gates." },
			{ kResourceVirtualGeometryPagePayloads, "Immutable virtual geometry page-local payload address records for resident high-detail pages, fallback pages, and conservative embedded fallback.", "Read by render preparation, upload planning, backend submission, and diagnostics without touching live mesh buffers." },
			{ kResourcePhysicalVirtualGeometryPagePayloads, "Exact physical page payload bindings for resident virtual geometry pages, fallback pages, and owning-mesh fail-open payloads.", "Read by render preparation and backend upload planning; written only after resource publication proves current ownership." },
			{ kResourceExactResidencySnapshots, "Immutable exact CPU/GPU residency snapshots keyed by asset and optional virtual geometry page ownership.", "Read by editor/resource diagnostics and renderer page planning without treating unavailable accounting as zero." },
			{ kResourceBackendGpuAllocationSnapshots, "Renderer-backend GPU allocation ownership records published after upload and removed after safe retirement.", "Read by Resource Residency and diagnostics as the source of exact Render GPU Bytes." },
			{ kResourceVirtualGeometryPageRequests, "Frame-local virtual geometry page request records with priority, fallback, and expected emission labels.", "Written by renderer page selection and read by ResourceStreaming before async IO scheduling." },
			{ kResourceVirtualGeometryPageNeedPackets, "Frame-local virtual geometry page request packets prepared by renderer selection for the streaming handoff.", "Written after cluster selection so resource streaming can request missing high-detail pages while fallback remains visible." },
			{ kResourceVirtualGeometryPagePublications, "Protected publication records for page payloads that became resident and shader-visible this frame.", "Written at the ResourceManager/RHI boundary and read by later frame snapshots after publication is complete." },
			{ kResourceVirtualGeometryPageRetirements, "Frame-safe virtual geometry page retirement records waiting for GPU completion before memory is released.", "Written by residency cleanup and drained only after protected frame completion." },
			{ kResourceVirtualGeometryPageDiagnostics, "Per-view page source, fallback, embedded-fallback, resident byte, request, and retirement diagnostics.", "Written after render/RHI work so editor, automation, and gates read stable page-source answers." },
			{ kResourceVirtualGeometryVisibilitySnapshots, "Per-view selected, dispatched, culled, and fallback virtual geometry visibility snapshots.", "Written after render preparation/protected submission so editor and automation counters read stable per-view state." },
			{ kResourceVirtualGeometryCounterSnapshots, "Deduplicated virtual geometry counter snapshots for frame stats and visualizer overlays.", "Written after protected render work so diagnostics do not observe partially assembled counters." },
			{ kResourceVirtualGeometryCullingDiagnostics, "Per-view culling reason counters for frustum, Hi-Z, cone, residency, and fail-open virtual geometry decisions.", "Written after protected render/RHI work so editor diagnostics read stable culling explanations." },
			{ kResourceVirtualGeometryPerformanceCounters, "Virtual geometry scalability gate counters for packet counts, submitted batches, meshlets, page pressure, and timing.", "Written by render diagnostics and read by tests, automation, and performance-gate reporting." },
			{ kResourceVirtualGeometryShadowVisibilitySnapshots, "Per-light or per-cascade virtual geometry shadow visibility selections.", "Written by shadow render preparation without reusing camera Hi-Z state." },
			{ kResourceVirtualGeometryDiagnostics, "Per-view virtual geometry counters, scheduler boundary metadata, and fail-open warnings.", "Written after protected render/RHI work so editor diagnostics and automation read a stable snapshot." },
			{ kResourceVisibleCommandReuseDiagnostics, "Per-view visible-command cache hit, conservative refresh, copy, and diagnostic demand telemetry.", "Written by render preparation and read by profiler, overlays, automation, and performance gates." },
			{ kResourceVisibleStreamingTouchSets, "Deduplicated visible mesh, material, and texture touch sets derived from cached visible commands.", "Written by render preparation and published through protected ResourceManager streaming boundaries." },
			{ kResourceRendererRecoveryStateSnapshot, "Immutable renderer recovery state for the current graphics session.", "Written by protected renderer/RHI boundaries and read by editor UI, automation, diagnostics, and export policy without touching live GPU handles." },
			{ kResourceRendererDeviceIncidentLog, "First-root-cause device-loss incident metadata plus suppressed duplicate failure counts.", "Written once per incident so later submit/present/upload errors group under the original failure." },
			{ "Resources", "Runtime asset and content mount state.", "" },
			{ "Profiler", "CPU/GPU profiling capture state.", "" },
			{ "Editor", "Editor-only systems and tooling state.", "" }
		};
	}

	std::vector<MutationFlushPoint> DefaultMutationFlushPoints()
	{
		return {
			{ "After Scene", MutationFlushPosition::AfterPhase, "Scene", {}, true, false },
			{ "After Scripting", MutationFlushPosition::AfterPhase, "Scripting", {}, true, false },
			{ "Frame End", MutationFlushPosition::FrameEnd, "", {}, true, true }
		};
	}

	std::string ToString(ResourceAccessMode mode)
	{
		return mode == ResourceAccessMode::Write ? "write" : "read";
	}

	std::string ToString(DiagnosticSeverity severity)
	{
		switch (severity)
		{
		case DiagnosticSeverity::Error:
			return "error";
		case DiagnosticSeverity::Warning:
			return "warning";
		case DiagnosticSeverity::Info:
		default:
			return "info";
		}
	}

	std::string ToString(DiagnosticCategory category)
	{
		switch (category)
		{
		case DiagnosticCategory::Phase:
			return "phase";
		case DiagnosticCategory::Dependency:
			return "dependency";
		case DiagnosticCategory::Resource:
			return "resource";
		case DiagnosticCategory::MainThread:
			return "main-thread";
		case DiagnosticCategory::Mutation:
			return "mutation";
		case DiagnosticCategory::Scheduler:
			return "scheduler";
		case DiagnosticCategory::DefaultMetadata:
			return "default-metadata";
		case DiagnosticCategory::Summary:
		default:
			return "summary";
		}
	}

	std::string ToString(MutationFlushPosition position)
	{
		switch (position)
		{
		case MutationFlushPosition::BeforePhase:
			return "before";
		case MutationFlushPosition::AfterPhase:
			return "after";
		case MutationFlushPosition::FrameEnd:
		default:
			return "frame-end";
		}
	}

	std::string FormatScheduleSummary(const ScheduleSummary& summary)
	{
		std::ostringstream stream;
		stream << "System scheduler summary: " << (summary.valid ? "valid" : "invalid") << '\n';
		stream << "Phases:";
		for (const std::string& phase : summary.phases)
		{
			stream << ' ' << phase;
		}
		stream << '\n';
		stream << "Systems:";
		for (const std::string& system : summary.systems)
		{
			stream << ' ' << system;
		}
		stream << '\n';
		stream << "Flush points:";
		for (const std::string& point : summary.flushPoints)
		{
			stream << " [" << point << ']';
		}
		stream << '\n';
		stream << "Main-thread systems:";
		for (const std::string& system : summary.mainThreadSystems)
		{
			stream << ' ' << system;
		}
		stream << '\n';
		stream << "Default metadata systems:";
		for (const std::string& system : summary.defaultedSystems)
		{
			stream << ' ' << system;
		}
		stream << '\n';
		stream << "Component query groups:";
		for (const std::string& group : summary.componentQueryGroups)
		{
			stream << " [" << group << "]";
		}
		stream << '\n';
		if (!summary.ecsGraphSummary.empty())
		{
			stream << "ECS graph: " << summary.ecsGraphSummary << '\n';
		}
		if (!summary.workerCoverageReport.formattedSummary.empty())
		{
			stream << summary.workerCoverageReport.formattedSummary << '\n';
		}
		stream << "Transaction diagnostics:"
			<< " submitted=" << summary.transactionDiagnostics.submittedCount
			<< " committed=" << summary.transactionDiagnostics.committedCount
			<< " rejected=" << summary.transactionDiagnostics.rejectedCount
			<< " conflicts=" << summary.transactionDiagnostics.conflictCount
			<< " canceled=" << summary.transactionDiagnostics.canceledCount
			<< " largestBatch=" << summary.transactionDiagnostics.largestSubmittedBatchSize
			<< " validationNs=" << summary.transactionDiagnostics.averageValidationCostNs
			<< " commitNs=" << summary.transactionDiagnostics.averageCommitCostNs
			<< '\n';
		if (summary.diagnostics.empty())
		{
			stream << "Diagnostics: none";
		}
		else
		{
			stream << "Diagnostics:";
			for (const ScheduleDiagnostic& diagnostic : summary.diagnostics)
			{
				stream << "\n- " << ToString(diagnostic.severity) << "/" << ToString(diagnostic.category)
					<< " system='" << diagnostic.systemName
					<< "' phase='" << diagnostic.phaseName
					<< "' resource='" << diagnostic.resourceName
					<< "': " << diagnostic.message
					<< " Action: " << diagnostic.recommendedAction;
			}
		}
		return stream.str();
	}

	SystemScheduler::SystemScheduler()
		: phases(DefaultSchedulerPhases())
		, resources(DefaultSchedulerResources())
		, flushPoints(DefaultMutationFlushPoints())
	{
	}

	void SystemScheduler::Clear()
	{
		systems.clear();
		orderedIndices.clear();
		orderedDeclarations.clear();
		latestComponentQueryGroups.clear();
		latestEcsGraphSummary.clear();
		latestSummary = {};
		dirty = true;
	}

	void SystemScheduler::SetPhaseDefinitions(std::vector<SchedulerPhaseDefinition> newPhases)
	{
		phases = std::move(newPhases);
		dirty = true;
	}

	void SystemScheduler::SetResourceDefinitions(std::vector<SchedulerResourceDefinition> newResources)
	{
		resources = std::move(newResources);
		dirty = true;
	}

	void SystemScheduler::SetMutationFlushPoints(std::vector<MutationFlushPoint> newFlushPoints)
	{
		flushPoints = std::move(newFlushPoints);
		dirty = true;
	}

	void SystemScheduler::RegisterSystem(SystemsBase& system)
	{
		RegisterSystem(system, system.SchedulerDeclaration());
	}

	void SystemScheduler::RegisterSystem(SystemsBase& system, SystemDeclaration declaration)
	{
		systems.push_back({
			&system,
			NormalizeDeclaration(system, std::move(declaration)),
			systems.size()
		});
		dirty = true;
	}

	void SystemScheduler::SetLatestEcsGraphSummary(std::string summary)
	{
		if (latestEcsGraphSummary == summary)
		{
			return;
		}
		latestEcsGraphSummary = std::move(summary);
		latestSummary.ecsGraphSummary = latestEcsGraphSummary;
		latestSummary.formatted = FormatScheduleSummary(latestSummary);
	}

	void SystemScheduler::SetLatestComponentQueryGroups(std::vector<std::string> queryGroups)
	{
		if (latestComponentQueryGroups == queryGroups)
		{
			return;
		}
		latestComponentQueryGroups = std::move(queryGroups);
		latestSummary.componentQueryGroups = latestComponentQueryGroups;
		latestSummary.formatted = FormatScheduleSummary(latestSummary);
	}

	void SystemScheduler::SetFrameWorkloadCompatibilityFallbackEnabled(bool enabled)
	{
		frameWorkloadCompatibilityFallbackEnabled = enabled;
	}

	bool SystemScheduler::FrameWorkloadCompatibilityFallbackEnabled() const
	{
		return frameWorkloadCompatibilityFallbackEnabled;
	}

	std::vector<FrameWorkloadDeclaration> SystemScheduler::BuildSystemOwnedWorkloadDeclarations(SystemsBase& system, const SystemDeclaration& declaration, float deltaTime, std::size_t executionOrder) const
	{
		std::vector<FrameWorkloadDeclaration> declaredWorkloads = system.DeclareFrameWorkloads(deltaTime);
		if (!declaredWorkloads.empty())
		{
			for (FrameWorkloadDeclaration& workload : declaredWorkloads)
			{
				// System-owned phases inherit the stable scheduler identity when
				// the declaring system omits repeated metadata. Keeping the fill-in
				// deterministic makes diagnostics readable without hiding bad access
				// declarations from validation.
				if (workload.ownerSystem.empty())
				{
					workload.ownerSystem = declaration.name;
				}
				if (workload.phase.empty())
				{
					workload.phase = declaration.phase;
				}
				if (workload.profilingLabel.empty())
				{
					workload.profilingLabel = workload.workloadName.empty() ? declaration.name : workload.workloadName;
				}
				if (workload.executionOrder == 0)
				{
					workload.executionOrder = executionOrder;
				}
			}
			return declaredWorkloads;
		}

		FrameWorkloadDeclaration workload;
		workload.workloadName = declaration.name;
		workload.ownerSystem = declaration.name;
		workload.phase = declaration.phase;
		workload.reads = declaration.reads;
		workload.writes = declaration.writes;
		workload.executionOrder = executionOrder;
		const bool wholeUpdateWorkerSafe = CanRunWholeSystemUpdateOnWorker(declaration);
		workload.workerEligible = wholeUpdateWorkerSafe;
		workload.mainThreadRequired = !wholeUpdateWorkerSafe;
		workload.mutationPolicy = declaration.requiresFlushedSceneMutations
			? FrameWorkloadMutationPolicy::DeferredOnly
			: FrameWorkloadMutationPolicy::None;
		workload.phaseKind = declaration.phaseKind;
		workload.boundaryClassification = declaration.boundaryClassification;
		workload.hardBoundary = declaration.hardBoundary;
		workload.safeSubwork = declaration.safeSubwork;
		workload.protectedCommit = declaration.protectedCommit;
		workload.profilingLabel = declaration.profilingLabel.empty() ? declaration.name : declaration.profilingLabel;
		workload.protectedReason = !declaration.protectedReason.empty()
			? declaration.protectedReason
			: !wholeUpdateWorkerSafe && !declaration.mainThread
			? "Full SystemsBase::Update remains protected until the entire system boundary is worker-safe."
			: declaration.mainThread
			? "System declaration still requires the main thread."
			: "System is eligible for worker execution after scheduler validation.";
		workload.fallbackReason = declaration.fallbackReason;
		workload.work = [&system, deltaTime](JobGraphContext&) {
			// The default workload preserves the existing system entry point for
			// unconverted systems. Converted systems should override
			// DeclareFrameWorkloads() so only proven worker-safe phases run away
			// from the protected coordinator path.
			system.Update(deltaTime);
		};
		return { std::move(workload) };
	}

	const ScheduleSummary& SystemScheduler::BuildSchedule()
	{
		latestSummary = {};
		latestSummary.componentQueryGroups = latestComponentQueryGroups;
		latestSummary.ecsGraphSummary = latestEcsGraphSummary;
		latestSummary.transactionDiagnostics = TerraEngine::Scene::SceneMutationQueue::Instance().GetTransactionDiagnostics();
		orderedIndices.clear();
		orderedDeclarations.clear();

		std::vector<ScheduleDiagnostic>& diagnostics = latestSummary.diagnostics;
		std::vector<SchedulerPhaseDefinition> orderedPhases = phases;
		std::stable_sort(orderedPhases.begin(), orderedPhases.end(), [](const SchedulerPhaseDefinition& left, const SchedulerPhaseDefinition& right) {
			return std::tie(left.order, left.name) < std::tie(right.order, right.name);
		});

		std::unordered_map<std::string, int> phaseOrder;
		std::unordered_map<std::string, bool> mainThreadCompatible;
		for (const SchedulerPhaseDefinition& phase : orderedPhases)
		{
			if (phase.name.empty() || phaseOrder.contains(phase.name))
			{
				AddDiagnostic(diagnostics, DiagnosticSeverity::Error, DiagnosticCategory::Phase, "", phase.name, "", "Scheduler phase names must be unique and non-empty.", "Rename duplicate or empty phases before building the schedule.");
				continue;
			}
			phaseOrder[phase.name] = phase.order;
			mainThreadCompatible[phase.name] = phase.mainThreadCompatible;
			latestSummary.phases.push_back(phase.name);
		}

		std::unordered_set<std::string> knownResources;
		for (const SchedulerResourceDefinition& resource : resources)
		{
			if (resource.name.empty() || knownResources.contains(resource.name))
			{
				AddDiagnostic(diagnostics, DiagnosticSeverity::Error, DiagnosticCategory::Resource, "", "", resource.name, "Scheduler resource names must be unique and non-empty.", "Rename duplicate or empty resources before building the schedule.");
				continue;
			}
			knownResources.insert(resource.name);
		}

		std::unordered_map<std::string, std::size_t> systemByName;
		std::unordered_set<std::string> duplicatedNames;
		for (std::size_t index = 0; index < systems.size(); ++index)
		{
			const SystemDeclaration& declaration = systems[index].declaration;
			if (declaration.name.empty())
			{
				AddDiagnostic(diagnostics, DiagnosticSeverity::Error, DiagnosticCategory::Dependency, "", declaration.phase, "", "Scheduled system name is empty.", "Give every scheduled system a stable non-empty name.");
			}
			else if (systemByName.contains(declaration.name))
			{
				duplicatedNames.insert(declaration.name);
				AddDiagnostic(diagnostics, DiagnosticSeverity::Error, DiagnosticCategory::Dependency, declaration.name, declaration.phase, "", "Duplicate scheduled system name.", "Use unique system names so dependency edges resolve deterministically.");
			}
			else
			{
				systemByName[declaration.name] = index;
			}

			if (!phaseOrder.contains(declaration.phase))
			{
				AddDiagnostic(diagnostics, DiagnosticSeverity::Error, DiagnosticCategory::Phase, declaration.name, declaration.phase, "", "System references an unknown scheduler phase.", "Move the system to a known phase or register the phase definition first.");
			}
			else if (declaration.mainThread && !mainThreadCompatible[declaration.phase])
			{
				AddDiagnostic(diagnostics, DiagnosticSeverity::Error, DiagnosticCategory::MainThread, declaration.name, declaration.phase, "", "Main-thread-only system is placed in a non-main-thread phase.", "Move the system to a main-thread-compatible phase or remove the main-thread requirement.");
			}

			for (const ResourceAccess& access : CollectAccesses(declaration))
			{
				if (!knownResources.contains(access.resourceName))
				{
					AddDiagnostic(diagnostics, DiagnosticSeverity::Error, DiagnosticCategory::Resource, declaration.name, declaration.phase, access.resourceName, "System references an unknown scheduler resource.", "Use one of the registered coarse resources or add a resource definition before validation.");
				}
			}

			if (declaration.mainThread)
			{
				latestSummary.mainThreadSystems.push_back(declaration.name);
			}
			if ((declaration.phaseKind == WorkerSystemPhaseKind::WorkerPrepare || !declaration.mainThread) && declaration.reads.empty() && declaration.writes.empty())
			{
				AddDiagnostic(diagnostics, DiagnosticSeverity::Error, DiagnosticCategory::Resource, declaration.name, declaration.phase, "", "Worker-preparation system has no declared access.", "Declare every resource read/write or keep the system protected.");
			}
			if (declaration.hardBoundary)
			{
				const std::string classification = ToString(declaration.boundaryClassification);
				AddDiagnostic(diagnostics, DiagnosticSeverity::Info, DiagnosticCategory::Summary, declaration.name, declaration.phase, "", "Hard boundary classified as " + classification + ".", "Only the documented safe subwork may run on workers.");
			}
			if (declaration.usesDefaultMetadata)
			{
				latestSummary.defaultedSystems.push_back(declaration.name);
				AddDiagnostic(diagnostics, DiagnosticSeverity::Info, DiagnosticCategory::DefaultMetadata, declaration.name, declaration.phase, "", "System is using default scheduler metadata.", "Add an explicit scheduler declaration when the system has ordering or resource requirements.");
			}
		}

		const std::size_t count = systems.size();
		std::vector<std::vector<bool>> edges(count, std::vector<bool>(count, false));
		auto addEdge = [&](std::size_t before, std::size_t after) {
			if (before != after && before < count && after < count)
			{
				edges[before][after] = true;
			}
		};

		for (std::size_t left = 0; left < count; ++left)
		{
			for (std::size_t right = 0; right < count; ++right)
			{
				if (left == right)
				{
					continue;
				}
				const auto leftPhase = phaseOrder.find(systems[left].declaration.phase);
				const auto rightPhase = phaseOrder.find(systems[right].declaration.phase);
				if (leftPhase != phaseOrder.end() && rightPhase != phaseOrder.end() && leftPhase->second < rightPhase->second)
				{
					addEdge(left, right);
				}
			}
		}

		for (std::size_t index = 0; index < count; ++index)
		{
			const SystemDeclaration& declaration = systems[index].declaration;
			auto validateDependency = [&](const std::string& targetName, bool currentRunsBeforeTarget) {
				if (targetName == declaration.name)
				{
					AddDiagnostic(diagnostics, DiagnosticSeverity::Error, DiagnosticCategory::Dependency, declaration.name, declaration.phase, "", "System declares a dependency on itself.", "Remove the self dependency.");
					return;
				}
				const auto target = systemByName.find(targetName);
				if (target == systemByName.end() || duplicatedNames.contains(targetName))
				{
					AddDiagnostic(diagnostics, DiagnosticSeverity::Error, DiagnosticCategory::Dependency, declaration.name, declaration.phase, "", "System references an unknown dependency target '" + targetName + "'.", "Correct the dependency name or register the target system.");
					return;
				}
				if (currentRunsBeforeTarget)
				{
					addEdge(index, target->second);
				}
				else
				{
					addEdge(target->second, index);
				}
			};

			for (const std::string& target : declaration.before)
			{
				validateDependency(target, true);
			}
			for (const std::string& target : declaration.after)
			{
				validateDependency(target, false);
			}
		}

		std::vector<int> indegree(count, 0);
		for (std::size_t from = 0; from < count; ++from)
		{
			for (std::size_t to = 0; to < count; ++to)
			{
				if (edges[from][to])
				{
					++indegree[to];
				}
			}
		}

		auto stableLess = [&](std::size_t left, std::size_t right) {
			const SystemDeclaration& leftDeclaration = systems[left].declaration;
			const SystemDeclaration& rightDeclaration = systems[right].declaration;
			const int leftPhase = phaseOrder.contains(leftDeclaration.phase) ? phaseOrder[leftDeclaration.phase] : 100000;
			const int rightPhase = phaseOrder.contains(rightDeclaration.phase) ? phaseOrder[rightDeclaration.phase] : 100000;
			return std::tie(leftPhase, systems[left].registrationIndex, leftDeclaration.name)
				> std::tie(rightPhase, systems[right].registrationIndex, rightDeclaration.name);
		};

		std::priority_queue<std::size_t, std::vector<std::size_t>, decltype(stableLess)> ready(stableLess);
		for (std::size_t index = 0; index < count; ++index)
		{
			if (indegree[index] == 0)
			{
				ready.push(index);
			}
		}

		while (!ready.empty())
		{
			const std::size_t current = ready.top();
			ready.pop();
			orderedIndices.push_back(current);
			for (std::size_t next = 0; next < count; ++next)
			{
				if (!edges[current][next])
				{
					continue;
				}
				--indegree[next];
				if (indegree[next] == 0)
				{
					ready.push(next);
				}
			}
		}

		if (orderedIndices.size() != count)
		{
			AddDiagnostic(diagnostics, DiagnosticSeverity::Error, DiagnosticCategory::Dependency, "", "", "", "System dependency graph contains a cycle.", "Remove or reverse one of the before/after dependencies in the cycle.");
		}

		std::vector<std::vector<bool>> reachability = edges;
		for (std::size_t pivot = 0; pivot < count; ++pivot)
		{
			for (std::size_t from = 0; from < count; ++from)
			{
				if (!reachability[from][pivot])
				{
					continue;
				}
				for (std::size_t to = 0; to < count; ++to)
				{
					reachability[from][to] = reachability[from][to] || reachability[pivot][to];
				}
			}
		}

		for (std::size_t left = 0; left < count; ++left)
		{
			const std::vector<ResourceAccess> leftAccesses = CollectAccesses(systems[left].declaration);
			for (std::size_t right = left + 1; right < count; ++right)
			{
				const std::vector<ResourceAccess> rightAccesses = CollectAccesses(systems[right].declaration);
				for (const ResourceAccess& leftAccess : leftAccesses)
				{
					for (const ResourceAccess& rightAccess : rightAccesses)
					{
						if (!HasWriteConflict(leftAccess, rightAccess))
						{
							continue;
						}
						if (!HasPath(left, right, reachability) && !HasPath(right, left, reachability))
						{
							AddDiagnostic(diagnostics, DiagnosticSeverity::Error, DiagnosticCategory::Resource, systems[left].declaration.name + " <-> " + systems[right].declaration.name, systems[left].declaration.phase, leftAccess.resourceName, "Unordered systems have incompatible resource access.", "Add an explicit before/after dependency or move one system to a separate phase.");
						}
					}
				}
			}
		}

		for (const MutationFlushPoint& flushPoint : flushPoints)
		{
			if (flushPoint.name.empty())
			{
				AddDiagnostic(diagnostics, DiagnosticSeverity::Error, DiagnosticCategory::Mutation, "", flushPoint.phaseName, "", "Mutation flush point name is empty.", "Give every flush point a stable name for diagnostics.");
			}
			if (flushPoint.position != MutationFlushPosition::FrameEnd && !phaseOrder.contains(flushPoint.phaseName))
			{
				AddDiagnostic(diagnostics, DiagnosticSeverity::Error, DiagnosticCategory::Mutation, "", flushPoint.phaseName, "", "Mutation flush point references an unknown phase.", "Point the flush at a known scheduler phase.");
			}
			latestSummary.flushPoints.push_back(FormatFlushPoint(flushPoint));
		}

		for (const RegisteredSystem& registeredSystem : systems)
		{
			if (!registeredSystem.declaration.requiresFlushedSceneMutations)
			{
				continue;
			}
			const bool hasFlush = std::any_of(flushPoints.begin(), flushPoints.end(), [&](const MutationFlushPoint& point) {
				return FlushRunsBeforeSystem(point, registeredSystem.declaration, phaseOrder);
			});
			if (!hasFlush)
			{
				AddDiagnostic(diagnostics, DiagnosticSeverity::Error, DiagnosticCategory::Mutation, registeredSystem.declaration.name, registeredSystem.declaration.phase, "SceneMutationQueue", "System requires flushed scene mutations but no earlier flush point is configured.", "Add a mutation flush point before this system or move it after an existing flush.");
			}
		}

		for (std::size_t index : orderedIndices)
		{
			latestSummary.systems.push_back(systems[index].declaration.name);
			orderedDeclarations.push_back(systems[index].declaration);
		}
		SortDiagnostics(diagnostics);
		latestSummary.valid = std::none_of(diagnostics.begin(), diagnostics.end(), IsError);
		latestSummary.formatted = FormatScheduleSummary(latestSummary);
		dirty = false;
		return latestSummary;
	}

	const ScheduleSummary& SystemScheduler::ExecuteFrame(float deltaTime, const MutationFlushCallback& flushCallback)
	{
		if (dirty)
		{
			BuildSchedule();
		}
		latestSummary.diagnostics.erase(std::remove_if(latestSummary.diagnostics.begin(), latestSummary.diagnostics.end(), IsTransientFrameDiagnostic), latestSummary.diagnostics.end());
		latestSummary.valid = std::none_of(latestSummary.diagnostics.begin(), latestSummary.diagnostics.end(), IsError);
		if (!latestSummary.valid)
		{
			latestSummary.formatted = FormatScheduleSummary(latestSummary);
			return latestSummary;
		}

		FrameExecutionPlan plan = ExecuteFrameWorkloads(deltaTime, flushCallback);
		latestFrameExecutionPlan = plan;
		latestSummary.workerCoverageReport = ::TerraEngine::Scheduling::BuildWorkerCoverageReport(plan);
		latestSummary.transactionDiagnostics = TerraEngine::Scene::SceneMutationQueue::Instance().GetTransactionDiagnostics();
		if (!plan.valid || !plan.completed)
		{
			AddDiagnostic(latestSummary.diagnostics, DiagnosticSeverity::Error, DiagnosticCategory::Scheduler, "SystemScheduler", "", "", "Parallel frame workload execution did not complete.", "Inspect the frame execution plan and architecture diagnostics for failed worker/protected jobs.");
			SortDiagnostics(latestSummary.diagnostics);
			latestSummary.valid = std::none_of(latestSummary.diagnostics.begin(), latestSummary.diagnostics.end(), IsError);
		}
		latestSummary.formatted = FormatScheduleSummary(latestSummary);
		return latestSummary;
	}

	std::vector<FrameWorkloadDeclaration> SystemScheduler::BuildFrameWorkloadDeclarations(float deltaTime)
	{
		if (dirty)
		{
			BuildSchedule();
		}

		std::vector<FrameWorkloadDeclaration> workloads;
		if (!latestSummary.valid)
		{
			return workloads;
		}

		std::size_t nextExecutionOrder = 1;
		auto appendSystemWorkload = [&](RegisteredSystem& registeredSystem, std::size_t executionOrder) {
			if (registeredSystem.system == nullptr)
			{
				return;
			}
			std::vector<FrameWorkloadDeclaration> systemWorkloads = BuildSystemOwnedWorkloadDeclarations(*registeredSystem.system, registeredSystem.declaration, deltaTime, executionOrder);
			workloads.insert(workloads.end(), std::make_move_iterator(systemWorkloads.begin()), std::make_move_iterator(systemWorkloads.end()));
		};

		auto appendFlushWorkload = [&](const MutationFlushPoint& point, std::size_t executionOrder) {
			FrameWorkloadDeclaration workload;
			workload.workloadName = "Flush:" + point.name;
			workload.ownerSystem = "SystemScheduler";
			workload.phase = point.phaseName.empty() ? "FrameEnd" : point.phaseName;
			workload.reads = { "SceneMutationQueue" };
			workload.writes = { "SceneMutationQueue" };
			workload.executionOrder = executionOrder;
			workload.workerEligible = false;
			workload.mainThreadRequired = true;
			workload.mutationPolicy = FrameWorkloadMutationPolicy::DeferredOnly;
			workload.phaseKind = WorkerSystemPhaseKind::Barrier;
			workload.protectedReason = "Mutation flushes remain protected barriers until deferred mutation batches are proven worker-safe.";
			workload.profilingLabel = workload.workloadName;
			workloads.push_back(std::move(workload));
		};

		std::vector<SchedulerPhaseDefinition> orderedPhases = phases;
		std::stable_sort(orderedPhases.begin(), orderedPhases.end(), [](const SchedulerPhaseDefinition& left, const SchedulerPhaseDefinition& right) {
			return std::tie(left.order, left.name) < std::tie(right.order, right.name);
		});

		workloads.reserve(orderedIndices.size() + flushPoints.size());
		for (const SchedulerPhaseDefinition& phase : orderedPhases)
		{
			for (const MutationFlushPoint& point : flushPoints)
			{
				if (point.position == MutationFlushPosition::BeforePhase && point.phaseName == phase.name)
				{
					appendFlushWorkload(point, nextExecutionOrder++);
				}
			}

			const std::size_t phaseExecutionOrder = nextExecutionOrder++;
			for (std::size_t orderedIndex : orderedIndices)
			{
				RegisteredSystem& registeredSystem = systems[orderedIndex];
				if (registeredSystem.declaration.phase == phase.name)
				{
					appendSystemWorkload(registeredSystem, phaseExecutionOrder);
				}
			}

			for (const MutationFlushPoint& point : flushPoints)
			{
				if (point.position == MutationFlushPosition::AfterPhase && point.phaseName == phase.name)
				{
					appendFlushWorkload(point, nextExecutionOrder++);
				}
			}
		}

		for (const MutationFlushPoint& point : flushPoints)
		{
			if (point.position == MutationFlushPosition::FrameEnd)
			{
				appendFlushWorkload(point, nextExecutionOrder++);
			}
		}

		return workloads;
	}

	FrameExecutionPlan SystemScheduler::ExecuteFrameWorkloads(float deltaTime, const MutationFlushCallback& flushCallback)
	{
		TERRAENGINE_PROFILE_SCOPE("SystemScheduler: Coordinator Wait Region", TerraEngine::Profiling::ProfilerModuleId::CpuEngine);
		std::vector<FrameWorkloadDeclaration> workloads = BuildFrameWorkloadDeclarations(deltaTime);
		if (!frameWorkloadCompatibilityFallbackEnabled)
		{
			TERRAENGINE_PROFILE_SCOPE("SystemScheduler: Inline Region Validation", TerraEngine::Profiling::ProfilerModuleId::CpuEngine);
			FrameExecutionPlan plan = TerraEngine::Scheduling::ValidateFrameWorkloads(std::move(workloads), "system-scheduler-frame");
			AddArchitectureDiagnostic(plan.diagnostics, ArchitectureDiagnosticSeverity::Warning, ArchitectureDiagnosticCode::CompatibilityFallback, "SystemScheduler", "", "Frame workload compatibility execution is disabled; metadata was validated without calling SystemsBase::Update.", "Enable compatibility fallback before replacing the serial frame path, or lower the validated plan into the worker scheduler.");
			plan.completed = false;
			plan.formatted = FormatFrameExecutionPlan(plan);
			return plan;
		}
		for (FrameWorkloadDeclaration& workload : workloads)
		{
			if (workload.ownerSystem != "SystemScheduler" || !flushCallback)
			{
				continue;
			}
			const std::string flushName = workload.workloadName.substr(std::string("Flush:").size());
			workload.work = [this, flushName, flushCallback](JobGraphContext&) {
				TERRAENGINE_PROFILE_SCOPE("SystemScheduler: Protected Region Flush", TerraEngine::Profiling::ProfilerModuleId::CpuEngine);
				const auto found = std::find_if(flushPoints.begin(), flushPoints.end(), [&](const MutationFlushPoint& point) {
					return point.name == flushName;
				});
				if (found != flushPoints.end())
				{
					const MutationFlushRuntimeState state = flushCallback(*found);
					if (!state.flushAttempted)
					{
						AddDiagnostic(latestSummary.diagnostics, DiagnosticSeverity::Warning, DiagnosticCategory::Mutation, "SystemScheduler", found->phaseName.empty() ? "FrameEnd" : found->phaseName, "SceneMutationQueue", "Mutation flush point did not run.", "Verify the scheduler flush callback is connected to SceneMutationQueue.");
					}
					if (found->position == MutationFlushPosition::FrameEnd && found->diagnosePendingAtFrameEnd && state.pendingAfterFlush > 0)
					{
						AddDiagnostic(latestSummary.diagnostics, DiagnosticSeverity::Warning, DiagnosticCategory::Mutation, "SystemScheduler", "FrameEnd", "SceneMutationQueue", "Scene mutations remain pending after the frame-end flush.", "Inspect rejected mutation requests; they no longer fail the scheduler frame by themselves.");
					}
				}
			};
		}
		TERRAENGINE_PROFILE_SCOPE("SystemScheduler: Worker Protected Inline Execution Region", TerraEngine::Profiling::ProfilerModuleId::CpuEngine);
		return TerraEngine::Scheduling::ExecuteFrameWorkloads(std::move(workloads), "system-scheduler-frame");
	}

	WorkerCoverageReport SystemScheduler::BuildWorkerCoverageReport(float deltaTime)
	{
		// Coverage reporting intentionally validates the same declarations used
		// for execution, but it does not call SystemsBase::Update. Editor panels
		// can therefore show conversion status without changing the frame.
		FrameExecutionPlan plan = TerraEngine::Scheduling::ValidateFrameWorkloads(BuildFrameWorkloadDeclarations(deltaTime), "system-scheduler-coverage");
		WorkerCoverageReport report = TerraEngine::Scheduling::BuildWorkerCoverageReport(plan);
		latestSummary.workerCoverageReport = report;
		latestSummary.formatted = FormatScheduleSummary(latestSummary);
		return report;
	}

	const ScheduleSummary& SystemScheduler::LatestSummary() const
	{
		return latestSummary;
	}

	const FrameExecutionPlan& SystemScheduler::LatestFrameExecutionPlan() const
	{
		return latestFrameExecutionPlan;
	}

	const std::vector<SystemDeclaration>& SystemScheduler::OrderedDeclarations() const
	{
		return orderedDeclarations;
	}

	const std::vector<SchedulerPhaseDefinition>& SystemScheduler::PhaseDefinitions() const
	{
		return phases;
	}

	const std::vector<MutationFlushPoint>& SystemScheduler::MutationFlushPoints() const
	{
		return flushPoints;
	}
}
