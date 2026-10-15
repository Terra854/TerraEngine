// RendererScratchDiagnosticsTests.cpp - Real owner evidence, no graphics host.
#include "../TestHarness.h"
#include "Editor/TerraEngine-Editor/Automation/RendererScratchDiagnostics.h"
#include "Renderer/Graph/RenderDiagnostics.h"
#include "Renderer/Graph/RenderFrameScratch.h"
#include "Core/Memory/TrackingMemoryResource.h"
#include <nlohmann/json.hpp>

#include <iostream>
#include <tuple>
#include <vector>

namespace TerraEngine::EditorTests {
	namespace {
		using Renderer::RenderFrameScratch;
		using Renderer::RenderFrameScratchDiagnostics;
		using Renderer::RenderDiagnosticsSummary;
		using Editor::Automation::SerializeRendererScratchDiagnostics;
		// Field-wise identity avoids reading object padding or introducing an
		// equality operator into the production evidence type just for a test.
		auto Facts(const RenderFrameScratchDiagnostics& value)
		{
			return std::make_tuple(value.lifetimeId, value.episodeId, value.frameId, value.capacityBytes,
				value.usedBytes, value.peakBytes, value.lifetimeHighWaterBytes, value.reuseAllocationCount,
				value.fallbackAllocationCount, value.resetCount, value.outcome);
		}
		void CheckEpisode(TestContext& context, const nlohmann::json& actual, const RenderFrameScratchDiagnostics& expected)
		{
			context.Expect(actual.at("lifetimeId") == expected.lifetimeId && actual.at("episodeId") == expected.episodeId
				&& actual.at("frameId") == expected.frameId && actual.at("capacityBytes") == expected.capacityBytes
				&& actual.at("usedBytes") == expected.usedBytes && actual.at("peakBytes") == expected.peakBytes
				&& actual.at("lifetimeHighWaterBytes") == expected.lifetimeHighWaterBytes
				&& actual.at("reuseAllocationCount") == expected.reuseAllocationCount
				&& actual.at("fallbackAllocationCount") == expected.fallbackAllocationCount
				&& actual.at("resetCount") == expected.resetCount && actual.at("outcomeCode") == static_cast<unsigned>(expected.outcome),
				"every serialized numeric episode fact must equal its actual copied native observation");
		}
		void Observe(const char* suffix, const nlohmann::json& value, const Memory::MemoryResourceStatistics& upstream)
		{
			// Emission follows actual settlement; counters come from independent
			// upstream allocations, never assumed constants or manual increments.
			const nlohmann::json record{{"case", std::string("Editor.RuntimeMemorySafety.Diagnostics.") + suffix},
				{"diagnostics", value}, {"upstreamAllocations", upstream.allocationCount},
				{"upstreamDeallocations", upstream.deallocationCount}, {"upstreamBytes", upstream.bytesInUse}};
			std::cout << "RMS_OBSERVATION " << record.dump() << '\n';
		}
	}
	void RegisterRendererScratchDiagnosticsTests(TestRegistry& registry)
	{
		registry.Add("Editor.RuntimeMemorySafety.Diagnostics.CopiedCounters", [](TestContext& context) {
			Memory::TrackingMemoryResource upstream;
			auto owner = RenderFrameScratch::Create(256U, &upstream);
			{
				auto lease = owner->BeginEpisode(101U);
				std::pmr::vector<int> reused(lease.Resource()); reused.assign(16U, 7);
				std::pmr::vector<int> fallback(lease.Resource()); fallback.assign(128U, 9);
				lease.MarkCompleted();
			}
			RenderDiagnosticsSummary summary;
			summary.frameScratch = owner->LastSnapshot(); summary.frameScratchReclamation = owner->ReclamationSnapshot();
			const auto json = SerializeRendererScratchDiagnostics(summary);
			CheckEpisode(context, json.at("latest"), summary.frameScratch);
			const auto actual = upstream.Statistics();
			context.Expect(summary.frameScratch.usedBytes > 0U && summary.frameScratch.reuseAllocationCount > 0U
				&& summary.frameScratch.fallbackAllocationCount > 0U && json.at("latest").at("outcome") == "Completed",
				"fixture must actually exercise reusable and fallback storage in a terminal completed episode");
			context.Expect(json.at("reclamation").at("totalFallbackAllocationCount") == actual.allocationCount
				&& json.at("reclamation").at("totalFallbackDeallocationCount") == actual.deallocationCount
				&& json.at("reclamation").at("outstandingFallbackAllocationCount") == actual.allocationCount - actual.deallocationCount
				&& json.at("reclamation").at("outstandingFallbackBytes") == actual.bytesInUse,
				"serialized fallback accounting must agree with the real independent upstream");
			Observe("CopiedCounters", json, actual);
		});
		registry.Add("Editor.RuntimeMemorySafety.Diagnostics.PreResetSnapshot", [](TestContext& context) {
			Memory::TrackingMemoryResource upstream;
			auto owner = RenderFrameScratch::Create(64U, &upstream);
			{
				auto lease = owner->BeginEpisode(102U);
				// Backing use and upstream pressure are separate counters. Exercise
				// both so the preserved terminal snapshot must retain both facts.
				std::pmr::vector<int> reused(lease.Resource()); reused.assign(4U, 7);
				std::pmr::vector<int> values(lease.Resource()); values.assign(64U, 3); lease.MarkCompleted();
			}
			const auto saved = owner->LastSnapshot();
			context.Expect(upstream.Statistics().bytesInUse > 0U, "terminal copy must precede actual protected reclamation");
			owner->Retire();
			RenderDiagnosticsSummary summary;
			summary.frameScratch = owner->LastSnapshot(); summary.frameScratchReclamation = owner->ReclamationSnapshot();
			const auto json = SerializeRendererScratchDiagnostics(summary);
			CheckEpisode(context, json.at("latest"), saved);
			context.Expect(saved.usedBytes > 0U && saved.peakBytes > 0U && saved.fallbackAllocationCount > 0U
				&& Facts(summary.frameScratch) == Facts(saved), "reset must preserve the complete pre-reset terminal copy");
			context.Expect(json.at("reclamation").at("resetCount") > saved.resetCount
				&& json.at("reclamation").at("outstandingFallbackAllocationCount") == 0U
				&& json.at("reclamation").at("outstandingFallbackBytes") == upstream.Statistics().bytesInUse
				&& upstream.Statistics().bytesInUse == 0U
				&& json.at("reclamation").at("totalFallbackDeallocationCount") == upstream.Statistics().deallocationCount
				&& upstream.Statistics().allocationCount == upstream.Statistics().deallocationCount,
				"current reclamation must show real balanced reset without zeroing the prior episode facts");
			Observe("PreResetSnapshot", json, upstream.Statistics());
		});
		registry.Add("Editor.RuntimeMemorySafety.Diagnostics.FailedAndLifetimeIdentity", [](TestContext& context) {
			Memory::TrackingMemoryResource upstream;
			auto owner = RenderFrameScratch::Create(64U, &upstream);
			{
				auto lease = owner->BeginEpisode(103U);
				// Keep actual backing pressure alongside the larger fallback owner.
				std::pmr::vector<int> reused(lease.Resource()); reused.assign(4U, 7);
				std::pmr::vector<int> values(lease.Resource()); values.assign(32U, 4); lease.MarkFailed();
			}
			const auto failed = owner->LastSnapshot();
			{
				auto lease = owner->BeginEpisode(104U);
				std::pmr::vector<int> reused(lease.Resource()); reused.assign(4U, 8);
				std::pmr::vector<int> values(lease.Resource()); values.assign(48U, 5);
				// Deliberately leave the real lease without a completion mark.
			}
			const auto cancelled = owner->LastSnapshot();
			RenderDiagnosticsSummary summary;
			summary.frameScratch = cancelled; summary.previousFrameScratch = failed;
			summary.frameScratchReclamation = owner->ReclamationSnapshot();
			const auto terminalJson = SerializeRendererScratchDiagnostics(summary);
			CheckEpisode(context, terminalJson.at("latest"), cancelled); CheckEpisode(context, terminalJson.at("previous"), failed);
			context.Expect(terminalJson.at("latest").at("outcome") == "Cancelled" && terminalJson.at("previous").at("outcome") == "Failed"
				&& cancelled.usedBytes > 0U && failed.usedBytes > 0U, "actual failed and cancelled pressure must remain distinguishable");
			owner->Retire(); owner.reset();
			auto replacement = RenderFrameScratch::Create(0U, &upstream);
			{
				auto lease = replacement->BeginEpisode(103U);
				std::pmr::vector<int> values(lease.Resource()); values.assign(16U, 6); lease.MarkCompleted();
			}
			summary.previousFrameScratch = cancelled; summary.frameScratch = replacement->LastSnapshot();
			summary.frameScratchReclamation = replacement->ReclamationSnapshot();
			const auto restartJson = SerializeRendererScratchDiagnostics(summary);
			context.Expect(summary.frameScratch.lifetimeId != failed.lifetimeId && summary.frameScratch.episodeId == failed.episodeId
				&& summary.frameScratch.frameId == failed.frameId && restartJson.at("previous").at("lifetimeId") == cancelled.lifetimeId
				&& restartJson.at("reclamation").at("lifetimeId") == summary.frameScratch.lifetimeId,
				"new lifetime identity must distinguish repeated frame/episode numbers and preserve old terminal evidence");
			Observe("FailedAndLifetimeIdentity", {{"terminal", terminalJson}, {"restart", restartJson}}, upstream.Statistics());
		});
		registry.Add("Editor.RuntimeMemorySafety.Diagnostics.ReadOnlyNoMutation", [](TestContext& context) {
			Memory::TrackingMemoryResource upstream;
			auto owner = RenderFrameScratch::Create(0U, &upstream);
			{
				auto lease = owner->BeginEpisode(105U);
				std::pmr::vector<int> values(lease.Resource()); values.assign(32U, 11); lease.MarkCompleted();
			}
			RenderDiagnosticsSummary summary;
			summary.frameScratch = owner->LastSnapshot(); summary.frameScratchReclamation = owner->ReclamationSnapshot();
			const auto copiedBefore = Facts(summary.frameScratch);
			const auto upstreamBefore = upstream.Statistics(); const auto referencesBefore = owner.use_count();
			const auto first = SerializeRendererScratchDiagnostics(summary); const auto second = SerializeRendererScratchDiagnostics(summary);
			context.Expect(first == second && copiedBefore == Facts(summary.frameScratch) && Facts(owner->LastSnapshot()) == copiedBefore
				&& !owner->IsActive() && referencesBefore == owner.use_count(), "serialization must preserve terminal copies, owner identity and admission state");
			const auto after = owner->ReclamationSnapshot();
			context.Expect(after.resetCount == summary.frameScratchReclamation.resetCount
				&& after.totalFallbackAllocationCount == summary.frameScratchReclamation.totalFallbackAllocationCount
				&& after.totalFallbackDeallocationCount == summary.frameScratchReclamation.totalFallbackDeallocationCount
				&& after.outstandingFallbackAllocationCount == summary.frameScratchReclamation.outstandingFallbackAllocationCount
				&& after.outstandingFallbackBytes == summary.frameScratchReclamation.outstandingFallbackBytes
				&& upstreamBefore.allocationCount == upstream.Statistics().allocationCount
				&& upstreamBefore.deallocationCount == upstream.Statistics().deallocationCount
				&& upstreamBefore.bytesInUse == upstream.Statistics().bytesInUse,
				"JSON construction must not allocate through, reclaim, reset or otherwise mutate the live scratch owner");
			Observe("ReadOnlyNoMutation", first, upstream.Statistics());
		});
	}
}
