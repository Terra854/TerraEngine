// SystemScheduler.cpp - validation and single-threaded execution for systems.
//
// This implementation deliberately stops at deterministic ordering and
// metadata validation. It does not create worker jobs yet; the ordered graph is
// shaped so Part 2 can lower the same data into dependency handles and barriers.

#include "SystemScheduler.h"

#include "Core/SystemBase.h"
#include "EcsQueryTaskGraph.h"
#include "JobGraph.h"
#include "Profiling/Profiler.h"
#include "Profiling/ProfilerScope.h"
#include "Scene/SceneMutationQueue.h"

#include <algorithm>
#include <atomic>
#include <iterator>
#include <map>
#include <mutex>
#include <queue>
#include <set>
#include <sstream>
#include <tuple>
#include <unordered_map>
#include <unordered_set>
#include <utility>

namespace TerraEngine::Scheduling {
	namespace {
		std::atomic_bool g_SameFramePhysicsPublicationConsumerActive = false;
		std::mutex g_SameFramePhysicsPublicationStateMutex;
		SameFramePhysicsPublicationState g_SameFramePhysicsPublicationState;

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

	void SetSameFramePhysicsPublicationConsumerActive(bool active) noexcept
	{
		g_SameFramePhysicsPublicationConsumerActive.store(active, std::memory_order_release);
		std::lock_guard<std::mutex> lock(g_SameFramePhysicsPublicationStateMutex);
		g_SameFramePhysicsPublicationState.active = active;
		if (!active)
		{
			g_SameFramePhysicsPublicationState.consumerSystem.clear();
			g_SameFramePhysicsPublicationState.reason = "No later system requires same-frame physics scene visibility; publication is deferred to Frame End.";
		}
	}

	void SetSameFramePhysicsPublicationState(SameFramePhysicsPublicationState state)
	{
		g_SameFramePhysicsPublicationConsumerActive.store(state.active, std::memory_order_release);
		std::lock_guard<std::mutex> lock(g_SameFramePhysicsPublicationStateMutex);
		g_SameFramePhysicsPublicationState = std::move(state);
	}

	bool SameFramePhysicsPublicationConsumerActive() noexcept
	{
		return g_SameFramePhysicsPublicationConsumerActive.load(std::memory_order_acquire);
	}

	SameFramePhysicsPublicationState SameFramePhysicsPublicationStateSnapshot()
	{
		std::lock_guard<std::mutex> lock(g_SameFramePhysicsPublicationStateMutex);
		return g_SameFramePhysicsPublicationState;
	}

	std::vector<SchedulerPhaseDefinition> DefaultSchedulerPhases()
	{
		return {
			{ "Startup", 0, true, false, false, false, "One-shot startup and boot validation." },
			{ "PreFrame", 10, true, false, false, false, "Per-frame platform and window preparation." },
			{ "Input", 20, true, false, false, false, "Input device polling and state updates." },
			{ "Scene", 30, true, false, false, true, "Scene loading, hierarchy state, and authored scene data." },
			{ "Scripting", 40, true, false, false, true, "Gameplay script execution and managed runtime updates." },
			{ "Animation", 45, true, true, false, false, "Controller authority, worker evaluation planning, and protected physics handoff." },
			{ "Physics", 50, true, true, false, true, "Fixed-step physics snapshot, preparation, simulation, and protected publication." },
			{ "AnimationPresentation", 55, true, true, false, false, "Current-step physics mapping and immutable animation presentation publication." },
			{ "UI", 60, true, false, false, false, "Runtime UI layout and CPU-side UI data." },
			{ "Render", 70, true, false, false, false, "Renderer submission and presentation." },
			{ "PostFrame", 80, true, false, false, false, "Frame-end cleanup and compatibility defaults." }
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
			{ "RuntimeTimeQueue", "Validated managed time-scale requests awaiting the next frame-start publication.", "Consumed before scheduler workload planning so every system observes one scale." },
			{ "RuntimeTime", "Immutable scaled and unscaled frame timing published at frame start.", "Read by managed callbacks and every delta-time consumer." },
			{ kResourceAnimatorComponents, "Protected authored AnimatorComponent membership and persisted configuration.", "Captured once before animation worker planning; workers never read live component storage." },
			{ kResourceAnimationCommandQueues, "Typed parameter and playback commands captured from script, gameplay, editor, and automation producers.", "Commands are copied at the protected Animation boundary and deterministically ordered before worker evaluation." },
			{ kResourceAnimationFrameInputs, "Immutable generation-labeled animator inputs for the current Animation phase.", "Written only by CaptureAnimationInputs and consumed by worker-safe planning." },
			{ kResourceAnimationEvaluationPrograms, "Immutable compiled controller programs referenced by animation frame inputs.", "Worker planning reads generation-stable views rather than authored graph objects." },
			{ kResourceAnimationResidencyCapabilities, "Immutable animation asset-residency, renderer-capability, and budget views.", "Mode planning reads this snapshot without requesting residency from worker code." },
			{ kResourceAnimationExecutionModes, "Prepared requested-to-effective execution-mode decisions and explicit reasons.", "Worker-owned output becomes visible only with the protected authority publication." },
			{ kResourceAnimationConsumerMasks, "Prepared authoritative joint, curve, event, root-motion, and physics consumer closures.", "The frame mask is immutable after planning so worker partitions remain range-disjoint." },
			{ kResourceAnimationWorkPackets, "Typed deterministic animation evaluation packets grouped by compatible work.", "Worker evaluation reads packets without touching AnimatorComponent or ObjectManager storage." },
			{ kResourceAnimationVisualPackets, "Generation-labeled visual evaluation packets retained across the physics handoff.", "AnimationPresentation consumes these packets after the current physics step." },
			{ kResourceAnimationClipSegments, "Immutable checked clip-segment views used by authoritative animation evaluation.", "Resident resource views are captured before workers run and contain no live ResourceManager ownership." },
			{ kResourceAnimationRollbackState, "Immutable fixed-tick rollback and deterministic history inputs.", "Authoritative worker evaluation reads bounded snapshots only." },
			{ kResourceAnimationAuthoritativeOutputs, "Range-disjoint prepared authoritative subsets, events, curves, and root-motion deltas.", "Workers write owned ranges; PublishAnimationAuthority validates the complete generation before swapping." },
			{ kResourceAnimationRuntimeState, "Protected immutable animation state publication visible to later systems.", "No partial worker output becomes observable through this resource." },
			{ kResourceAnimationPhysicsHandoffQueue, "Typed root-motion and animation-to-physics commands for the current fixed step.", "Published once at the protected Animation-to-Physics boundary; workers never mutate physics or transforms directly." },
			{ kResourceAnimationManagedCallbackBatches, "Copied animation callback batches awaiting protected managed delivery.", "Managed code never receives worker-owned storage or callbacks from a worker thread." },
			{ kResourceAnimationRagdollMappings, "Prepared mappings from immutable current-step physics snapshots into skeleton space.", "Worker output is private until animation presentation publication." },
			{ kResourceAnimationRecordingSamples, "Prepared bounded recording samples sourced from authoritative and presentation data.", "Recording publication remains generation-checked and protected." },
			{ kResourceAnimationPresentationPrepared, "Worker-owned ragdoll, recording, and visual presentation preparation.", "The prepared value is not render-visible until PublishAnimationPresentation completes." },
			{ kResourceAnimationPresentationPlan, "Immutable generation-labeled render-facing animation presentation plan.", "UI and Render read only the complete plan published at the protected presentation boundary." },
			{ "PhysicsSettingsUpdateQueue", "Validated project or packaged-runtime physics settings awaiting protected application.", "Read only at a declared physics boundary before a world is created or rebuilt." },
			{ "PhysicsSettings", "The active immutable gravity, fixed-step, layer, solver, query, and diagnostic configuration.", "Published through a protected settings boundary and read by physics workloads." },
			{ "PhysicsAuthoringSnapshot", "Immutable hierarchy-resolved physics component and project-settings input.", "Written at PhysicsSnapshot and read by worker preparation." },
			{ "PhysicsPreparedWork", "Typed body, collider, command, and query preparation packets.", "Worker-written and committed only at the protected physics boundary." },
			{ "PhysicsQueryQueue", "Bounded generation-validated scheduled query submissions.", "Read by PhysicsPrepare; submissions never expose provider state." },
			{ "PhysicsPreparedQueries", "Immutable validated query batches ready for provider execution.", "Written by PhysicsPrepare and consumed after the provider simulation barrier." },
			{ "PhysicsWorld", "Generation-validated runtime world, registries, and provider state.", "Provider writes occur through declared commit/simulate boundaries." },
			{ "PhysicsBackendQueryResults", "Bounded provider-neutral hit candidates awaiting protected publication.", "Produced by shared-pool query jobs after simulation joins." },
			{ "PhysicsQueryResults", "Immutable ordered query result history.", "Published only after target generation revalidation." },
			{ "PhysicsRawEvents", "Engine-owned contact and activation records collected after provider jobs join.", "Callback order is never directly observable." },
			{ "PhysicsEventSubscriptions", "Data-only event subscription snapshot and filters.", "Snapshotted before protected event publication." },
			{ "PhysicsEventPackets", "Immutable ordered bounded event packet history.", "Published only after participant generation revalidation." },
			{ "PhysicsPublishedSnapshots", "Immutable body, command, event, query, debug, and diagnostic publications.", "Swapped only after sorted publication completes." },
			{ "RagdollPhysicsQueue", "Typed generation-qualified animation-to-physics ragdoll requests.", "Drained only by the protected Physics command boundary; worker producers never mutate the live Jolt world." },
			{ "RagdollPhysicsSnapshots", "Immutable current-step ragdoll pose snapshots copied from physics authority.", "Published after provider work completes and consumed by animation presentation with exact generations." },
			{ "RagdollPhysicsResults", "Immutable accepted or rejected ragdoll command results.", "Published atomically with the protected ragdoll request batch outcome." },
			{ "PhysicsTransformTransactions", "Typed transform updates awaiting After Physics scene publication.", "Worker extraction cannot mutate live Transform storage." },
			{ "UI", "Runtime UI document, style, and generated geometry state.", "" },
			{ "Renderer", "Renderer frame graph, GPU submission, and render backend state.", "" },
			{ "ActivePresentationState", "Read-only active swapchain state and presentation generation.", "" },
			{ "PresentationState", "Protected presentation lifecycle and recreation state.", "" },
			{ "PresentationGeneration", "Monotonic generation published after complete presentation recreation.", "" },
			{ "EditorSubmissionCompletion", "Fence-backed editor UI submission serial progress.", "" },
			{ "RendererDiagnostics", "Stable renderer and presentation diagnostic snapshots.", "" },
			{ kResourceRuntimeRenderSettingsQueue, "Copied game-wide rendering overrides submitted by managed gameplay callbacks.", "Consumed only by RenderSystem at its protected frame boundary." },
			{ kResourceRuntimeRenderSettings, "Effective game-wide runtime rendering settings used to build immutable frame snapshots.", "Published by RenderSystem after draining the managed settings queue." },
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
			{ "EditorAutomationQueue", "Immutable queued MCP/editor automation requests awaiting worker preparation or protected commit.", "Prepared without mutating live editor or scene state." },
			{ "EditorAutomationSession", "Capability, job, idempotency, approval, activity, and structured-log state retained for the current editor session.", "Worker readers consume immutable snapshots; publication remains protected." },
			{ kResourceVirtualGeometryQualityProfile, "Renderer-owned virtual geometry quality, LOD, Hi-Z, debug visualization, and budget settings.", "Read by render preparation when choosing cluster packet detail and page request pressure." },
			{ kResourceRenderFrameSnapshot, "Exact immutable scene, view, renderer, geometry, residency, and completed-readback inputs for one frame.", "Written only by the protected RenderSnapshot workload and consumed by worker-safe renderer preparation." },
			{ kResourceAnimatedObjectVisibilitySnapshot, "Prior-frame union of animated objects visible to camera or shadow submissions.", "Published after protected render preparation and consumed by the next Animation phase; stale or absent generations fail open." },
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
			{ kResourceAnimatedGeometryPageDemands, "Immutable animated page demands collected from every frame view.", "Worker preparation sorts and deduplicates these records before backend work is published." },
			{ kResourceAnimatedGeometryDeformationPlans, "Frame-wide immutable animated geometry compute plans.", "Published after checked prefix planning and consumed once before all view passes." },
			{ kResourceAnimatedGeometryOutputs, "Current and previous engine-owned deformed page vertices.", "Written by renderer compute and visible to canonical geometry assembly after the declared barrier." },
			{ kResourceAnimatedGeometryDynamicBounds, "Current-generation animated meshlet and cluster bounds.", "Written after deformation and read only when exact publication identities match." },
			{ kResourceAnimatedGeometryPublications, "Atomic geometry, history, bounds, and refinement publication rows.", "Swapped only at the protected renderer boundary; rejection leaves prior state intact." },
			{ kResourceAnimatedGeometryRetirements, "Fence-delayed animated geometry allocation retirements.", "Protected backend cleanup owns final destruction after every referencing frame completes." },
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
			{ "After Physics", MutationFlushPosition::AfterPhase, "Physics", { "PhysicsSystem" }, true, false },
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
		stream << "After Physics visibility: active=" << (summary.afterPhysicsFlushActive ? "true" : "false")
			<< " consumer='" << summary.afterPhysicsConsumer
			<< "' reason='" << summary.afterPhysicsReason << "'\n";
		stream << "Frame topology cache: " << (summary.frameTopologyCacheHit ? "hit" : "miss")
			<< " hits=" << summary.frameTopologyCacheHitCount
			<< " misses=" << summary.frameTopologyCacheMissCount << '\n';
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
		frameTopologyCache = {};
		SetSameFramePhysicsPublicationState({false, {}, "Scheduler was cleared; no same-frame physics consumer is active."});
		dirty = true;
	}

	void SystemScheduler::SetPhaseDefinitions(std::vector<SchedulerPhaseDefinition> newPhases)
	{
		phases = std::move(newPhases);
		frameTopologyCache.valid = false;
		dirty = true;
	}

	void SystemScheduler::SetResourceDefinitions(std::vector<SchedulerResourceDefinition> newResources)
	{
		resources = std::move(newResources);
		frameTopologyCache.valid = false;
		dirty = true;
	}

	void SystemScheduler::SetMutationFlushPoints(std::vector<MutationFlushPoint> newFlushPoints)
	{
		flushPoints = std::move(newFlushPoints);
		frameTopologyCache.valid = false;
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
		frameTopologyCache.valid = false;
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

	std::vector<FrameWorkloadDeclaration> SystemScheduler::BuildSystemOwnedWorkloadDeclarations(RegisteredSystem& registeredSystem, float deltaTime, std::size_t executionOrder)
	{
		SystemsBase& system = *registeredSystem.system;
		const SystemDeclaration& declaration = registeredSystem.declaration;
		// The system name is retained as task context so an expensive declaration
		// identifies its owner instead of collapsing into one opaque scheduler cost.
		TERRAENGINE_PROFILE_TASK_SCOPE("SystemScheduler: Build System Declaration",
			TerraEngine::Profiling::ProfilerModuleId::CpuEngine, declaration.name.c_str());
		std::vector<FrameWorkloadDeclaration> current = system.DeclareFrameWorkloads(deltaTime);
		if (current.empty())
		{
			if (declaration.omitWhenNoFrameWorkloads)
			{
				// The system performed its protected activation check and proved there
				// is no useful frame work. Do not synthesize a legacy Update job; that
				// would turn zero-work paths into protected per-frame overhead.
				return {};
			}
			FrameWorkloadDeclaration workload;
			workload.workloadName = declaration.name;
			workload.ownerSystem = declaration.name;
			workload.phase = declaration.phase;
			workload.reads = declaration.reads;
			workload.writes = declaration.writes;
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
			current.push_back(std::move(workload));
		}

		const std::size_t systemOrderBase = executionOrder * 1024U;
		for (std::size_t localIndex = 0; localIndex < current.size(); ++localIndex)
		{
			FrameWorkloadDeclaration& workload = current[localIndex];
			if (workload.ownerSystem.empty()) workload.ownerSystem = declaration.name;
			if (workload.phase.empty()) workload.phase = declaration.phase;
			if (workload.profilingLabel.empty())
			{
				workload.profilingLabel = workload.workloadName.empty() ? declaration.name : workload.workloadName;
			}
			if (workload.stableDescriptorId.empty())
			{
				workload.stableDescriptorId = workload.ownerSystem + "/" + workload.phase + "/" + workload.workloadName;
			}
			if (workload.executionOrder == 0)
			{
				workload.executionOrder = systemOrderBase + localIndex + 1U;
			}
		}

		auto stableMetadataMatches = [](const FrameWorkloadDeclaration& left, const FrameWorkloadDeclaration& right) {
			return left.workloadName == right.workloadName
				&& left.ownerSystem == right.ownerSystem
				&& left.phase == right.phase
				&& left.stableDescriptorId == right.stableDescriptorId
				&& left.activationPredicateKey == right.activationPredicateKey
				&& left.reads == right.reads
				&& left.writes == right.writes
				&& left.explicitDependencies == right.explicitDependencies
				&& left.inlineWorkItemThreshold == right.inlineWorkItemThreshold
				&& left.partitionPolicy == right.partitionPolicy
				&& left.partitionWorkItemCount == right.partitionWorkItemCount
				&& left.maximumPartitionCount == right.maximumPartitionCount
				&& left.workerEligible == right.workerEligible
				&& left.mainThreadRequired == right.mainThreadRequired
				&& left.mutationPolicy == right.mutationPolicy
				&& left.phaseKind == right.phaseKind
				&& left.boundaryClassification == right.boundaryClassification
				&& left.hardBoundary == right.hardBoundary
				&& left.safeSubwork == right.safeSubwork
				&& left.protectedCommit == right.protectedCommit
				&& left.protectedReason == right.protectedReason
				&& left.fallbackReason == right.fallbackReason
				&& left.profilingLabel == right.profilingLabel;
		};

		bool cacheChanged = registeredSystem.stableWorkloadDescriptors.size() != current.size();
		if (!cacheChanged)
		{
			for (std::size_t index = 0; index < current.size(); ++index)
			{
				if (!stableMetadataMatches(registeredSystem.stableWorkloadDescriptors[index], current[index]))
				{
					cacheChanged = true;
					break;
				}
			}
		}

		if (cacheChanged || registeredSystem.stableDescriptorGeneration == 0)
		{
			++registeredSystem.stableDescriptorGeneration;
			if (registeredSystem.stableDescriptorGeneration == 0) registeredSystem.stableDescriptorGeneration = 1;
			registeredSystem.stableWorkloadDescriptors = current;
			for (FrameWorkloadDeclaration& cached : registeredSystem.stableWorkloadDescriptors)
			{
				cached.stableDescriptorGeneration = registeredSystem.stableDescriptorGeneration;
				cached.stableDescriptorGeneration = registeredSystem.stableDescriptorGeneration;
				// Stable scheduling state must not retain closures or immutable frame
				// inputs beyond the frame that supplied them.
				cached.work = {};
				cached.rangeWork = {};
				cached.active = true;
				cached.ready = true;
				cached.readinessReason.clear();
				cached.estimatedWorkItemCount = 0;
				cached.sameFrameVisibilityRequired = false;
			}
		}

		std::vector<FrameWorkloadDeclaration> bound;
		bound.reserve(current.size());
		for (std::size_t index = 0; index < current.size(); ++index)
		{
			FrameWorkloadDeclaration workload = registeredSystem.stableWorkloadDescriptors[index];
			workload.executionOrder = current[index].executionOrder;
			workload.active = current[index].active;
			workload.ready = current[index].ready;
			workload.readinessReason = std::move(current[index].readinessReason);
			workload.estimatedWorkItemCount = current[index].estimatedWorkItemCount;
			workload.sameFrameVisibilityRequired = current[index].sameFrameVisibilityRequired;
			workload.work = std::move(current[index].work);
			workload.rangeWork = std::move(current[index].rangeWork);
			bound.push_back(std::move(workload));
		}
		return bound;
	}

	SameFramePhysicsPublicationState SystemScheduler::ResolveSameFramePhysicsPublicationState() const
	{
		SameFramePhysicsPublicationState state;
		const auto physicsPhase = std::find_if(phases.begin(), phases.end(), [](const SchedulerPhaseDefinition& phase) {
			return phase.name == "Physics";
		});
		const int physicsOrder = physicsPhase == phases.end() ? 0 : physicsPhase->order;
		for (const SystemDeclaration& declaration : orderedDeclarations)
		{
			const auto phase = std::find_if(phases.begin(), phases.end(), [&](const SchedulerPhaseDefinition& candidate) {
				return candidate.name == declaration.phase;
			});
			if (phase == phases.end() || phase->order <= physicsOrder || !declaration.requiresFlushedSceneMutations)
			{
				continue;
			}
			const bool readsPublishedScene = std::find(declaration.reads.begin(), declaration.reads.end(), "SceneGraph") != declaration.reads.end()
				|| std::find(declaration.reads.begin(), declaration.reads.end(), "ObjectManager") != declaration.reads.end();
			if (!readsPublishedScene) continue;
			state.active = true;
			if (!state.consumerSystem.empty()) state.consumerSystem += ",";
			state.consumerSystem += declaration.name;
		}
		state.reason = state.active
			? "Later declared consumer requires flushed SceneGraph/ObjectManager state after Physics."
			: "No later system requires same-frame physics scene visibility; publication is deferred to Frame End.";
		return state;
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

		FrameExecutionPlan plan = ExecuteFrameWorkloads(
			deltaTime, flushCallback, true);
		// The scheduler is the sole owner of the completed plan. Move its callbacks,
		// result vectors, and optional execution evidence into the retained snapshot
		// instead of deep-copying the complete frame graph every frame.
		latestFrameExecutionPlan = std::move(plan);
		const bool schedulerDiagnosticsRequested =
			TerraEngine::Scheduling::EcsGraphFrameDiagnosticsEnabled();
		if (schedulerDiagnosticsRequested)
		{
			latestSummary.workerCoverageReport =
				::TerraEngine::Scheduling::BuildWorkerCoverageReport(
					latestFrameExecutionPlan);
		}
		else
		{
			// Compact frames retain the numeric health envelope used by automation,
			// but defer category/string construction until the diagnostics window opens.
			WorkerCoverageReport compactCoverage{};
			compactCoverage.frameId = latestFrameExecutionPlan.frameId;
			compactCoverage.barrierCount =
				latestFrameExecutionPlan.migrationSummary.barrierCount;
			compactCoverage.protectedTimeNs =
				latestFrameExecutionPlan.migrationSummary.protectedBaselineTimeNs
				+ latestFrameExecutionPlan.migrationSummary.protectedCommitTimeNs;
			compactCoverage.workerTimeNs =
				latestFrameExecutionPlan.migrationSummary.workerPreparationTimeNs;
			latestSummary.workerCoverageReport = std::move(compactCoverage);
		}
		latestSummary.transactionDiagnostics = TerraEngine::Scene::SceneMutationQueue::Instance().GetTransactionDiagnostics();
		if (!latestFrameExecutionPlan.valid || !latestFrameExecutionPlan.completed)
		{
			AddDiagnostic(latestSummary.diagnostics, DiagnosticSeverity::Error, DiagnosticCategory::Scheduler, "SystemScheduler", "", "", "Parallel frame workload execution did not complete.", "Inspect the frame execution plan and architecture diagnostics for failed worker/protected jobs.");
			SortDiagnostics(latestSummary.diagnostics);
			latestSummary.valid = std::none_of(latestSummary.diagnostics.begin(), latestSummary.diagnostics.end(), IsError);
		}
		if(schedulerDiagnosticsRequested || !latestSummary.valid)
		{
			latestSummary.formatted = FormatScheduleSummary(latestSummary);
		}
		return latestSummary;
	}

	std::vector<FrameWorkloadDeclaration> SystemScheduler::BuildFrameWorkloadDeclarations(float deltaTime)
	{
		if (dirty)
		{
			TERRAENGINE_PROFILE_SCOPE("SystemScheduler: Rebuild Dirty Schedule", TerraEngine::Profiling::ProfilerModuleId::CpuEngine);
			BuildSchedule();
			// Phase/resource/flush changes can alter dependency meaning even when a
			// descriptor retains the same public identity.
			frameTopologyCache.valid = false;
		}

		std::vector<FrameWorkloadDeclaration> workloads;
		if (!latestSummary.valid)
		{
			return workloads;
		}

		// Resolve conditional visibility before asking PhysicsSystem to bind its
		// per-frame activation. This prevents the physics packet and flush
		// descriptor from observing the previous frame's consumer state.
		SameFramePhysicsPublicationState physicsPublicationState;
		{
			TERRAENGINE_PROFILE_SCOPE("SystemScheduler: Resolve Same-Frame Publication", TerraEngine::Profiling::ProfilerModuleId::CpuEngine);
			physicsPublicationState = ResolveSameFramePhysicsPublicationState();
			SetSameFramePhysicsPublicationState(physicsPublicationState);
		}
		latestSummary.afterPhysicsFlushActive = physicsPublicationState.active;
		latestSummary.afterPhysicsConsumer = physicsPublicationState.consumerSystem;
		latestSummary.afterPhysicsReason = physicsPublicationState.reason;

		std::size_t nextExecutionOrder = 1;
		auto appendSystemWorkload = [&](RegisteredSystem& registeredSystem, std::size_t executionOrder) {
			if (registeredSystem.system == nullptr)
			{
				return;
			}
			std::vector<FrameWorkloadDeclaration> systemWorkloads = BuildSystemOwnedWorkloadDeclarations(registeredSystem, deltaTime, executionOrder);
			workloads.insert(workloads.end(), std::make_move_iterator(systemWorkloads.begin()), std::make_move_iterator(systemWorkloads.end()));
		};

		auto appendFlushWorkload = [&](const MutationFlushPoint& point, std::size_t executionOrder) {
			const bool physicsPublicationPending = std::any_of(workloads.begin(), workloads.end(), [](const FrameWorkloadDeclaration& declared) {
				return declared.ownerSystem == "PhysicsSystem"
					&& declared.sameFrameVisibilityRequired
					&& declared.active
					&& declared.ready;
			});
			FrameWorkloadDeclaration workload;
			workload.workloadName = "Flush:" + point.name;
			workload.ownerSystem = "SystemScheduler";
			workload.phase = point.phaseName.empty() ? "FrameEnd" : point.phaseName;
			workload.stableDescriptorId = "SystemScheduler/MutationFlush/" + point.name;
			workload.activationPredicateKey = point.name == "After Physics"
				? "same-frame-physics-publication-consumer"
				: "mutation-flush-point-configured";
			workload.active = point.name != "After Physics"
				|| (physicsPublicationState.active && physicsPublicationPending);
			workload.reads = { "SceneMutationQueue" };
			workload.writes = { "SceneMutationQueue" };
			// Flushes use the same reserved global order blocks as system workloads.
			// This keeps an After-Phase flush after every local workload in that phase.
			workload.executionOrder = executionOrder * 1024U;
			workload.workerEligible = false;
			workload.mainThreadRequired = true;
			workload.mutationPolicy = FrameWorkloadMutationPolicy::DeferredOnly;
			workload.phaseKind = WorkerSystemPhaseKind::Barrier;
			workload.sameFrameVisibilityRequired = point.name == "After Physics" && physicsPublicationState.active;
			workload.protectedReason = "Mutation flushes remain protected barriers until deferred mutation batches are proven worker-safe.";
			workload.profilingLabel = workload.workloadName;
			if (point.name == "After Physics")
			{
				latestSummary.afterPhysicsFlushActive = workload.active;
				if (!physicsPublicationPending)
				{
					latestSummary.afterPhysicsReason = "A later consumer exists, but physics has no publication work; the protected flush is omitted.";
				}
			}
			workloads.push_back(std::move(workload));
		};

		// Systems that explicitly omit a legacy fallback contribute no descriptor on
		// a dormant frame. Excluding them from the initial estimate keeps adding a
		// zero-work subsystem from increasing the scheduler's per-frame allocation;
		// an active system can still grow the vector when it publishes real work.
		{
		TERRAENGINE_PROFILE_SCOPE("SystemScheduler: Collect Workload Declarations", TerraEngine::Profiling::ProfilerModuleId::CpuEngine);
		const std::size_t alwaysDeclaredSystemCount = static_cast<std::size_t>(std::count_if(
			orderedIndices.begin(), orderedIndices.end(), [this](std::size_t index) {
				return !systems[index].declaration.omitWhenNoFrameWorkloads;
			}));
		workloads.reserve(alwaysDeclaredSystemCount + flushPoints.size());
		// BuildSchedule validates, sorts, and publishes this name list whenever the
		// scheduler is dirty. Reusing it avoids copying phase definitions and
		// allocating long phase-name strings on every frame, while PhaseDefinitions()
		// continues to expose the caller's original definition order.
		for (const std::string& phaseName : latestSummary.phases)
		{
			for (const MutationFlushPoint& point : flushPoints)
			{
				if (point.position == MutationFlushPosition::BeforePhase && point.phaseName == phaseName)
				{
					appendFlushWorkload(point, nextExecutionOrder++);
				}
			}

			for (std::size_t orderedIndex : orderedIndices)
			{
				RegisteredSystem& registeredSystem = systems[orderedIndex];
				if (registeredSystem.declaration.phase == phaseName)
				{
					// Give each system a distinct order block. Converted systems can use
					// local offsets within that block without interleaving another owner.
					appendSystemWorkload(registeredSystem, nextExecutionOrder++);
				}
			}

			for (const MutationFlushPoint& point : flushPoints)
			{
				if (point.position == MutationFlushPosition::AfterPhase && point.phaseName == phaseName)
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
		}

		const auto topologyMatches = [this, &workloads]() {
			if(!frameTopologyCache.valid
				|| frameTopologyCache.entries.size() != workloads.size()
				|| frameTopologyCache.loweredDependencies.size() != workloads.size())
			{
				return false;
			}
			for(std::size_t index = 0u; index < workloads.size(); ++index)
			{
				const FrameWorkloadDeclaration& workload = workloads[index];
				const FrameTopologyCacheEntry& cached = frameTopologyCache.entries[index];
				if(cached.stableDescriptorId != workload.stableDescriptorId
					|| cached.phase != workload.phase
					|| cached.ownerSystem != workload.ownerSystem
					|| cached.stableDescriptorGeneration != workload.stableDescriptorGeneration
					|| cached.executionOrder != workload.executionOrder
					|| cached.active != workload.active
					|| cached.ready != workload.ready
					|| cached.sameFrameVisibilityRequired != workload.sameFrameVisibilityRequired)
				{
					return false;
				}
			}
			return true;
		};
		bool topologyCacheHit = false;
		{
			TERRAENGINE_PROFILE_SCOPE("SystemScheduler: Match Topology Cache", TerraEngine::Profiling::ProfilerModuleId::CpuEngine);
			topologyCacheHit = topologyMatches();
		}
		if(topologyCacheHit)
		{
			for(std::size_t index = 0u; index < workloads.size(); ++index)
			{
				workloads[index].explicitDependencies =
					frameTopologyCache.loweredDependencies[index];
			}
			++frameTopologyCache.hitCount;
			latestSummary.frameTopologyCacheHit = true;
			latestSummary.frameTopologyCacheHitCount = frameTopologyCache.hitCount;
			latestSummary.frameTopologyCacheMissCount = frameTopologyCache.missCount;
			return workloads;
		}
		++frameTopologyCache.missCount;
		latestSummary.frameTopologyCacheHit = false;
		latestSummary.frameTopologyCacheHitCount = frameTopologyCache.hitCount;
		latestSummary.frameTopologyCacheMissCount = frameTopologyCache.missCount;

		// Lower phase and system ordering into explicit descriptor edges. The
		// numeric executionOrder remains a deterministic display/tie-break value;
		// it no longer serializes unrelated work in FrameWorkload execution.
		auto addDependency = [&workloads](std::size_t dependentIndex, std::size_t prerequisiteIndex) {
			if (dependentIndex == prerequisiteIndex) return;
			const std::string& prerequisite = workloads[prerequisiteIndex].stableDescriptorId;
			auto& dependencies = workloads[dependentIndex].explicitDependencies;
			if (!prerequisite.empty() && std::find(dependencies.begin(), dependencies.end(), prerequisite) == dependencies.end())
			{
				dependencies.push_back(prerequisite);
			}
		};
		auto activeIndices = [&workloads](const std::string& phase, bool schedulerOwned) {
			std::vector<std::size_t> indices;
			for (std::size_t index = 0; index < workloads.size(); ++index)
			{
				const FrameWorkloadDeclaration& workload = workloads[index];
				if (workload.phase == phase && workload.active && workload.ready
					&& (workload.ownerSystem == "SystemScheduler") == schedulerOwned)
				{
					indices.push_back(index);
				}
			}
			return indices;
		};
		auto flushPositionFor = [this, &workloads](std::size_t index) {
			const std::string prefix = "Flush:";
			const std::string name = workloads[index].workloadName.starts_with(prefix)
				? workloads[index].workloadName.substr(prefix.size())
				: std::string{};
			const auto found = std::find_if(flushPoints.begin(), flushPoints.end(), [&name](const MutationFlushPoint& point) {
				return point.name == name;
			});
			return found == flushPoints.end() ? MutationFlushPosition::FrameEnd : found->position;
		};

		std::unordered_map<std::string, std::vector<std::size_t>> descriptorsByOwner;
		for (std::size_t index = 0; index < workloads.size(); ++index)
		{
			if (workloads[index].ownerSystem != "SystemScheduler" && workloads[index].active && workloads[index].ready)
			{
				descriptorsByOwner[workloads[index].ownerSystem].push_back(index);
			}
		}
		for (const SystemDeclaration& declaration : orderedDeclarations)
		{
			const auto current = descriptorsByOwner.find(declaration.name);
			if (current == descriptorsByOwner.end() || current->second.empty()) continue;
			for (const std::string& afterSystem : declaration.after)
			{
				const auto prerequisite = descriptorsByOwner.find(afterSystem);
				if (prerequisite != descriptorsByOwner.end() && !prerequisite->second.empty())
				{
					addDependency(current->second.front(), prerequisite->second.back());
				}
			}
			for (const std::string& beforeSystem : declaration.before)
			{
				const auto dependent = descriptorsByOwner.find(beforeSystem);
				if (dependent != descriptorsByOwner.end() && !dependent->second.empty())
				{
					addDependency(dependent->second.front(), current->second.back());
				}
			}
		}

		std::vector<std::size_t> previousPhaseTerminals;
		for (const std::string& phaseName : latestSummary.phases)
		{
			const std::vector<std::size_t> systemNodes = activeIndices(phaseName, false);
			const std::vector<std::size_t> flushNodes = activeIndices(phaseName, true);
			if (systemNodes.empty() && flushNodes.empty())
			{
				// An empty phase is transparent to dependency lowering. In particular,
				// keep the previous active phase terminals intact so a later phase still
				// depends on them without constructing empty owner maps and vectors.
				continue;
			}
			std::vector<std::size_t> beforeFlushes;
			std::vector<std::size_t> afterFlushes;
			for (const std::size_t flushIndex : flushNodes)
			{
				if (flushPositionFor(flushIndex) == MutationFlushPosition::BeforePhase) beforeFlushes.push_back(flushIndex);
				else if (flushPositionFor(flushIndex) == MutationFlushPosition::AfterPhase) afterFlushes.push_back(flushIndex);
			}
			for (std::size_t index = 0; index < beforeFlushes.size(); ++index)
			{
				for (const std::size_t prerequisite : index == 0U ? previousPhaseTerminals : std::vector<std::size_t>{beforeFlushes[index - 1U]})
				{
					addDependency(beforeFlushes[index], prerequisite);
				}
			}
			const std::vector<std::size_t>& phasePrerequisites = beforeFlushes.empty() ? previousPhaseTerminals : beforeFlushes;
			std::unordered_map<std::string, std::size_t> firstByOwner;
			std::unordered_map<std::string, std::size_t> lastByOwner;
			for (const std::size_t index : systemNodes)
			{
				firstByOwner.try_emplace(workloads[index].ownerSystem, index);
				lastByOwner[workloads[index].ownerSystem] = index;
			}
			for (const auto& [owner, first] : firstByOwner)
			{
				(void)owner;
				for (const std::size_t prerequisite : phasePrerequisites) addDependency(first, prerequisite);
			}
			std::vector<std::size_t> systemTerminals;
			for (const auto& [owner, last] : lastByOwner)
			{
				(void)owner;
				systemTerminals.push_back(last);
			}
			if (systemTerminals.empty()) systemTerminals = phasePrerequisites;
			for (std::size_t index = 0; index < afterFlushes.size(); ++index)
			{
				const std::vector<std::size_t>& prerequisites = index == 0U ? systemTerminals : std::vector<std::size_t>{afterFlushes[index - 1U]};
				for (const std::size_t prerequisite : prerequisites) addDependency(afterFlushes[index], prerequisite);
			}
			previousPhaseTerminals = afterFlushes.empty() ? systemTerminals : std::vector<std::size_t>{afterFlushes.back()};
		}
		for (std::size_t index = 0; index < workloads.size(); ++index)
		{
			if (workloads[index].ownerSystem != "SystemScheduler" || !workloads[index].active
				|| flushPositionFor(index) != MutationFlushPosition::FrameEnd) continue;
			for (const std::size_t prerequisite : previousPhaseTerminals) addDependency(index, prerequisite);
			previousPhaseTerminals = {index};
		}

		// Replace the cache only after the complete dependency graph has been
		// lowered. A failed allocation leaves the prior exact cache untouched and
		// affects performance only; frame correctness continues from workloads.
		try
		{
			FrameTopologyCache refreshed;
			refreshed.valid = true;
			refreshed.hitCount = frameTopologyCache.hitCount;
			refreshed.missCount = frameTopologyCache.missCount;
			refreshed.entries.reserve(workloads.size());
			refreshed.loweredDependencies.reserve(workloads.size());
			for(const FrameWorkloadDeclaration& workload : workloads)
			{
				refreshed.entries.push_back({
					workload.stableDescriptorId,
					workload.phase,
					workload.ownerSystem,
					workload.stableDescriptorGeneration,
					workload.executionOrder,
					workload.active,
					workload.ready,
					workload.sameFrameVisibilityRequired});
				refreshed.loweredDependencies.push_back(workload.explicitDependencies);
			}
			frameTopologyCache = std::move(refreshed);
		}
		catch(const std::bad_alloc&)
		{
			frameTopologyCache.valid = false;
		}

		return workloads;
	}

	FrameExecutionPlan SystemScheduler::ExecuteFrameWorkloads(
		float deltaTime,
		const MutationFlushCallback& flushCallback,
		bool compactRuntimeDiagnostics)
	{
		// This scope is the scheduler call itself. Previous labels described the
		// entire call as a coordinator wait and protected-inline region, which made
		// every system appear as a child of waits that had not actually occurred.
		TERRAENGINE_PROFILE_SCOPE("SystemScheduler::ExecuteFrameWorkloads", TerraEngine::Profiling::ProfilerModuleId::CpuEngine);
		std::vector<FrameWorkloadDeclaration> workloads;
		{
			TERRAENGINE_PROFILE_SCOPE("SystemScheduler: Build Frame Workloads", TerraEngine::Profiling::ProfilerModuleId::CpuEngine);
			workloads = BuildFrameWorkloadDeclarations(deltaTime);
		}
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
		{
			// The validated workload executor contains worker-eligible and protected
			// jobs. Give their shared parent a neutral label so the hierarchy does not
			// claim all of that time belongs to either execution policy.
			TERRAENGINE_PROFILE_SCOPE("SystemScheduler: Execute Validated Workloads", TerraEngine::Profiling::ProfilerModuleId::CpuEngine);
			FrameWorkloadExecutionOptions options;
			options.formatDiagnostics = !compactRuntimeDiagnostics
				|| EcsGraphFrameDiagnosticsEnabled();
			options.collectExecutionRecords = options.formatDiagnostics
				|| TerraEngine::Profiling::Profiler::Instance()
					.IsFrameRecordingActive();
			return TerraEngine::Scheduling::ExecuteFrameWorkloads(
				std::move(workloads), "system-scheduler-frame", options);
		}
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
