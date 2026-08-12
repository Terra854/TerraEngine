// PhysicsDiagnosticsWindow.cpp - immutable physics snapshot presentation.

#include "PhysicsDiagnosticsWindow.h"

#include "Engine/Physics/PhysicsDiagnostics.h"

#include <imgui.h>
#include <memory>

namespace
{
	std::shared_ptr<const TerraEngine::Physics::PhysicsDiagnosticsSnapshot> AcquireSnapshot()
	{
		return TerraEngine::Physics::AcquirePhysicsDiagnosticsSnapshot();
	}
}

PhysicsDiagnosticsWindow::PhysicsDiagnosticsWindow(MainEditorSubsystem* windowOwner)
	: EditorWindow("Physics Diagnostics", false, "Diagnostics"), owner(windowOwner)
{
}

void PhysicsDiagnosticsWindow::OnGui()
{
	// Do not request producer diagnostics here: physics owns its own opt-in
	// settings and this panel must add zero simulation work while merely open.
	if (!m_IsVisible)
	{
		return;
	}
	const std::shared_ptr<const TerraEngine::Physics::PhysicsDiagnosticsSnapshot> snapshot = AcquireSnapshot();
	if (!snapshot)
	{
		ImGui::TextDisabled("Waiting for an immutable physics diagnostics snapshot.");
		return;
	}
	ImGui::Text("World %llu / generation %llu  Tick %llu  State %u",
		static_cast<unsigned long long>(snapshot->world.worldId.value),
		static_cast<unsigned long long>(snapshot->world.worldGeneration),
		static_cast<unsigned long long>(snapshot->tick),
		static_cast<unsigned>(snapshot->state));
	DrawCounters();
	DrawTimings();
	DrawSchedulerDeclarations();
}

void PhysicsDiagnosticsWindow::DrawCounters()
{
	const auto snapshot = AcquireSnapshot();
	if (!snapshot || !ImGui::BeginTable("PhysicsDiagnosticsCounters", 6, ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg | ImGuiTableFlags_SizingStretchProp))
	{
		return;
	}
	ImGui::TableSetupColumn("Bodies");
	ImGui::TableSetupColumn("Shapes");
	ImGui::TableSetupColumn("Constraints");
	ImGui::TableSetupColumn("Characters");
	ImGui::TableSetupColumn("Queries / Hits");
	ImGui::TableSetupColumn("Events / Dropped");
	ImGui::TableHeadersRow();
	ImGui::TableNextRow();
	ImGui::TableSetColumnIndex(0); ImGui::Text("%llu", static_cast<unsigned long long>(snapshot->counters.bodyCount));
	ImGui::TableSetColumnIndex(1); ImGui::Text("%llu", static_cast<unsigned long long>(snapshot->counters.shapeCount));
	ImGui::TableSetColumnIndex(2); ImGui::Text("%llu", static_cast<unsigned long long>(snapshot->counters.constraintCount));
	ImGui::TableSetColumnIndex(3); ImGui::Text("%llu", static_cast<unsigned long long>(snapshot->counters.characterCount));
	ImGui::TableSetColumnIndex(4); ImGui::Text("%llu / %llu", static_cast<unsigned long long>(snapshot->counters.queryCount), static_cast<unsigned long long>(snapshot->counters.queryHitCount));
	ImGui::TableSetColumnIndex(5); ImGui::Text("%llu / %llu", static_cast<unsigned long long>(snapshot->counters.eventCount), static_cast<unsigned long long>(snapshot->counters.eventDroppedCount));
	ImGui::EndTable();
	ImGui::Text("Queues: commands %llu, queries %llu, events %llu. Rejected %llu, stale %llu.",
		static_cast<unsigned long long>(snapshot->queues.commandDepth),
		static_cast<unsigned long long>(snapshot->queues.queryDepth),
		static_cast<unsigned long long>(snapshot->queues.eventDepth),
		static_cast<unsigned long long>(snapshot->counters.rejectedCount),
		static_cast<unsigned long long>(snapshot->counters.staleCount));
	ImGui::Text("Contacts %llu. Shape cache: %llu shared / %llu assets / %llu B; hits %llu, misses %llu.",
		static_cast<unsigned long long>(snapshot->counters.contactCount),
		static_cast<unsigned long long>(snapshot->cache.sharedShapeCount),
		static_cast<unsigned long long>(snapshot->cache.assetCount),
		static_cast<unsigned long long>(snapshot->cache.payloadBytes),
		static_cast<unsigned long long>(snapshot->cache.hitCount),
		static_cast<unsigned long long>(snapshot->cache.missCount));
	ImGui::SeparatorText("Incremental Pipeline");
	ImGui::Text("Authoring gen %llu / snapshot %llu: %llu changed (%llu added, %llu removed, %llu transforms), reused %s.",
		static_cast<unsigned long long>(snapshot->authoring.authoringGeneration),
		static_cast<unsigned long long>(snapshot->authoring.snapshotGeneration),
		static_cast<unsigned long long>(snapshot->authoring.changedObjects),
		static_cast<unsigned long long>(snapshot->authoring.added),
		static_cast<unsigned long long>(snapshot->authoring.removed),
		static_cast<unsigned long long>(snapshot->authoring.transforms),
		snapshot->authoring.snapshotReused ? "yes" : "no");
	ImGui::Text("Prepare %s: %llu items / %llu ranges; scratch %llu active, %llu reused.",
		snapshot->preparation.executionPath.c_str(),
		static_cast<unsigned long long>(snapshot->preparation.estimatedWorkItems),
		static_cast<unsigned long long>(snapshot->preparation.rangeCount),
		static_cast<unsigned long long>(snapshot->preparation.scratchActiveItems),
		static_cast<unsigned long long>(snapshot->preparation.scratchReuses));
	ImGui::Text("Extract %llu/%llu bodies (%llu examined, %llu active, %llu changed), ranges %llu, parallel %s, fallback %s.",
		static_cast<unsigned long long>(snapshot->extraction.extractedBodies),
		static_cast<unsigned long long>(snapshot->extraction.totalBodies),
		static_cast<unsigned long long>(snapshot->extraction.examinedBodies),
		static_cast<unsigned long long>(snapshot->extraction.activeBodies),
		static_cast<unsigned long long>(snapshot->extraction.changedBodies),
		static_cast<unsigned long long>(snapshot->extraction.rangeCount),
		snapshot->extraction.parallelReads ? "yes" : "no",
		snapshot->extraction.serialFallback ? "yes" : "no");
	ImGui::Text("State store gen %llu: %llu bodies / %llu pages / %llu B; pages reused %llu, copied %llu.",
		static_cast<unsigned long long>(snapshot->stateStore.storeGeneration),
		static_cast<unsigned long long>(snapshot->stateStore.bodyCount),
		static_cast<unsigned long long>(snapshot->stateStore.pageCount),
		static_cast<unsigned long long>(snapshot->stateStore.retainedBytes),
		static_cast<unsigned long long>(snapshot->stateStore.pagesReused),
		static_cast<unsigned long long>(snapshot->stateStore.pagesCopied));
	ImGui::Text("Publication: %llu entries / %llu chunks / %llu pending, target %s; accepted %llu, rejected %llu, empty skipped %llu.",
		static_cast<unsigned long long>(snapshot->publication.currentEntries),
		static_cast<unsigned long long>(snapshot->publication.currentChunks),
		static_cast<unsigned long long>(snapshot->publication.pendingGroups),
		snapshot->publication.targetBoundary.c_str(),
		static_cast<unsigned long long>(snapshot->publication.acceptedPackets),
		static_cast<unsigned long long>(snapshot->publication.rejectedPackets),
		static_cast<unsigned long long>(snapshot->publication.emptyPacketsSkipped));
	if (!snapshot->publication.lastFailure.empty())
	{
		ImGui::TextWrapped("Current publication failure: %s", snapshot->publication.lastFailure.c_str());
	}
	ImGui::Text("Aggregate cadence: every %u frames, last tick %llu, refreshed %s; health is current.",
		snapshot->rate.aggregateIntervalFrames,
		static_cast<unsigned long long>(snapshot->rate.lastAggregateTick),
		snapshot->rate.aggregateRefreshed ? "yes" : "no");
	ImGui::Text("Solver quality: %u velocity / %u position iterations (settings gen %llu, world gen %llu, applied %s).",
		snapshot->quality.velocityIterations,
		snapshot->quality.positionIterations,
		static_cast<unsigned long long>(snapshot->quality.settingsGeneration),
		static_cast<unsigned long long>(snapshot->quality.worldGeneration),
		snapshot->quality.applied ? "yes" : "no");
	ImGui::Text("Jobs: %llu submitted / %llu complete / %llu rejected; %llu batches, %llu queue-lock acquisitions, %u/%u pumps.",
		static_cast<unsigned long long>(snapshot->jobs.submitted),
		static_cast<unsigned long long>(snapshot->jobs.completed),
		static_cast<unsigned long long>(snapshot->jobs.rejected),
		static_cast<unsigned long long>(snapshot->jobs.batchCount),
		static_cast<unsigned long long>(snapshot->jobs.lockAcquisitionCount),
		snapshot->jobs.activePumps,
		snapshot->jobs.maximumActivePumps);
}

void PhysicsDiagnosticsWindow::DrawTimings()
{
	const auto snapshot = AcquireSnapshot();
	if (!snapshot || !ImGui::BeginTable("PhysicsDiagnosticsTimings", 6, ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg | ImGuiTableFlags_SizingStretchProp))
	{
		return;
	}
	ImGui::TableSetupColumn("Prepare"); ImGui::TableSetupColumn("Commit"); ImGui::TableSetupColumn("Simulate");
	ImGui::TableSetupColumn("Queries"); ImGui::TableSetupColumn("Events"); ImGui::TableSetupColumn("Publish");
	ImGui::TableHeadersRow(); ImGui::TableNextRow();
	ImGui::TableSetColumnIndex(0); ImGui::Text("%.3f ms", snapshot->timings.prepareMs);
	ImGui::TableSetColumnIndex(1); ImGui::Text("%.3f ms", snapshot->timings.commitMs);
	ImGui::TableSetColumnIndex(2); ImGui::Text("%.3f ms", snapshot->timings.simulateMs);
	ImGui::TableSetColumnIndex(3); ImGui::Text("%.3f ms", snapshot->timings.queryMs);
	ImGui::TableSetColumnIndex(4); ImGui::Text("%.3f ms", snapshot->timings.eventPublishMs);
	ImGui::TableSetColumnIndex(5); ImGui::Text("%.3f ms", snapshot->timings.publishMs);
	ImGui::EndTable();
	ImGui::Text("Memory: provider %llu B, snapshots %llu B, high-water %llu B.",
		static_cast<unsigned long long>(snapshot->memory.providerBytes),
		static_cast<unsigned long long>(snapshot->memory.snapshotBytes),
		static_cast<unsigned long long>(snapshot->memory.highWaterBytes));
	ImGui::Text("Provider allocations: %llu outstanding, %llu high-water (provider byte totals unavailable).",
		static_cast<unsigned long long>(snapshot->memory.outstandingProviderAllocations),
		static_cast<unsigned long long>(snapshot->memory.highWaterProviderAllocations));
	ImGui::Text("Extraction %.3f ms; state validation %.3f ms; snapshot %.3f ms; transactions %.3f ms.",
		snapshot->timings.extractionMs,
		snapshot->timings.stateValidationMs,
		snapshot->timings.snapshotConstructionMs,
		snapshot->timings.transactionConstructionMs);
}

void PhysicsDiagnosticsWindow::DrawSchedulerDeclarations()
{
	const auto snapshot = AcquireSnapshot();
	if (!snapshot || !snapshot->scheduler || snapshot->scheduler->empty())
	{
		ImGui::TextDisabled("No published physics scheduler declarations.");
		return;
	}
	ImGui::SeparatorText("Scheduler Declarations");
	if (!ImGui::BeginTable("PhysicsDiagnosticsScheduler", 6, ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg | ImGuiTableFlags_Resizable))
	{
		return;
	}
	ImGui::TableSetupColumn("Workload"); ImGui::TableSetupColumn("Phase"); ImGui::TableSetupColumn("Reads");
	ImGui::TableSetupColumn("Writes"); ImGui::TableSetupColumn("Worker"); ImGui::TableSetupColumn("Boundary"); ImGui::TableHeadersRow();
	for (const auto& declaration : *snapshot->scheduler)
	{
		ImGui::TableNextRow();
		ImGui::TableSetColumnIndex(0); ImGui::TextUnformatted(declaration.workload.c_str());
		ImGui::TableSetColumnIndex(1); ImGui::TextUnformatted(declaration.phase.c_str());
		ImGui::TableSetColumnIndex(2); ImGui::TextWrapped("%s", declaration.reads.c_str());
		ImGui::TableSetColumnIndex(3); ImGui::TextWrapped("%s", declaration.writes.c_str());
		ImGui::TableSetColumnIndex(4); ImGui::TextUnformatted(declaration.workerSafe ? "yes" : "protected");
		ImGui::TableSetColumnIndex(5); ImGui::TextWrapped("%s", declaration.boundary.c_str());
	}
	ImGui::EndTable();
}
