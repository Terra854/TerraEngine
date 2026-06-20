// SystemScheduler.cpp - validation and single-threaded execution for systems.
//
// This implementation deliberately stops at deterministic ordering and
// metadata validation. It does not create worker jobs yet; the ordered graph is
// shaped so Part 2 can lower the same data into dependency handles and barriers.

#include "SystemScheduler.h"

#include "Core/SystemBase.h"
#include "Profiling/ProfilerScope.h"

#include <algorithm>
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
			{ "ObjectManager", "Entity/object storage and component ownership.", "Refined by component read/write query declarations in Part 3." },
			{ "SceneMutationQueue", "Deferred structural scene mutation requests.", "Feeds deferred entity/component mutation in Part 3." },
			{ "Scripting", "Managed runtime, script assemblies, and script component state.", "May later expose script component query resources." },
			{ "UI", "Runtime UI document, style, and generated geometry state.", "" },
			{ "Renderer", "Renderer frame graph, GPU submission, and render backend state.", "" },
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

	const ScheduleSummary& SystemScheduler::BuildSchedule()
	{
		latestSummary = {};
		latestSummary.componentQueryGroups = latestComponentQueryGroups;
		latestSummary.ecsGraphSummary = latestEcsGraphSummary;
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
		if (!latestSummary.valid)
		{
			return latestSummary;
		}

		std::vector<ScheduleDiagnostic> frameDiagnostics;
		auto runFlush = [&](const MutationFlushPoint& point) {
			if (!flushCallback)
			{
				return;
			}
			MutationFlushRuntimeState state = flushCallback(point);
			if (!state.flushAttempted)
			{
				AddDiagnostic(frameDiagnostics, DiagnosticSeverity::Warning, DiagnosticCategory::Mutation, "", point.phaseName, "SceneMutationQueue", "Mutation flush point did not run.", "Verify the scheduler flush callback is connected to SceneMutationQueue.");
			}
			if (point.position == MutationFlushPosition::FrameEnd && point.diagnosePendingAtFrameEnd && state.pendingAfterFlush > 0)
			{
				AddDiagnostic(frameDiagnostics, DiagnosticSeverity::Error, DiagnosticCategory::Mutation, "", point.phaseName, "SceneMutationQueue", "Scene mutations remain pending after the frame-end flush.", "Inspect rejected mutation requests and clear or rebuild invalid requests before the next frame.");
			}
		};

		std::vector<SchedulerPhaseDefinition> orderedPhases = phases;
		std::stable_sort(orderedPhases.begin(), orderedPhases.end(), [](const SchedulerPhaseDefinition& left, const SchedulerPhaseDefinition& right) {
			return std::tie(left.order, left.name) < std::tie(right.order, right.name);
		});

		for (const SchedulerPhaseDefinition& phase : orderedPhases)
		{
			for (const MutationFlushPoint& point : flushPoints)
			{
				if (point.position == MutationFlushPosition::BeforePhase && point.phaseName == phase.name)
				{
					runFlush(point);
				}
			}

			for (std::size_t orderedIndex : orderedIndices)
			{
				RegisteredSystem& registeredSystem = systems[orderedIndex];
				if (registeredSystem.declaration.phase != phase.name || registeredSystem.system == nullptr)
				{
					continue;
				}
				const std::string label = registeredSystem.declaration.profilingLabel.empty()
					? registeredSystem.declaration.name
					: registeredSystem.declaration.profilingLabel;
				TERRAENGINE_PROFILE_TASK_SCOPE("SystemScheduler::SystemUpdate", TerraEngine::Profiling::ProfilerModuleId::CpuEngine, label.c_str());
				registeredSystem.system->Update(deltaTime);
			}

			for (const MutationFlushPoint& point : flushPoints)
			{
				if (point.position == MutationFlushPosition::AfterPhase && point.phaseName == phase.name)
				{
					runFlush(point);
				}
			}
		}

		for (const MutationFlushPoint& point : flushPoints)
		{
			if (point.position == MutationFlushPosition::FrameEnd)
			{
				runFlush(point);
			}
		}

		if (!frameDiagnostics.empty())
		{
			latestSummary.diagnostics.insert(latestSummary.diagnostics.end(), frameDiagnostics.begin(), frameDiagnostics.end());
			SortDiagnostics(latestSummary.diagnostics);
			latestSummary.valid = std::none_of(latestSummary.diagnostics.begin(), latestSummary.diagnostics.end(), IsError);
			latestSummary.formatted = FormatScheduleSummary(latestSummary);
			// Runtime flush diagnostics describe this frame only. Mark the graph
			// dirty so the next frame rebuilds the static schedule instead of
			// treating a transient mutation rejection as a permanent graph state.
			dirty = true;
		}
		return latestSummary;
	}

	const ScheduleSummary& SystemScheduler::LatestSummary() const
	{
		return latestSummary;
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
