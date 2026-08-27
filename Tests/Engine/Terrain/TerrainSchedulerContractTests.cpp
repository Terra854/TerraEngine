// TerrainSchedulerContractTests.cpp — terrain worker and protected-boundary declarations.

#include "../TestHarness.h"

#include "Core/Scheduling/SystemScheduler.h"
#include "Resource/VirtualGeometryStreamingSystem.h"
#include "Terrain/TerrainDerivedSystem.h"
#include "Terrain/TerrainStreamingSystem.h"

#include <algorithm>
#include <array>
#include <string_view>

namespace TerraEngine::Tests
{
	namespace
	{
		// Scheduler declarations use diagnostic-facing strings. This helper keeps
		// each assertion readable while still checking the exact public contract.
		bool Contains(const std::vector<std::string>& values, std::string_view expected)
		{
			return std::find(values.begin(), values.end(), expected) != values.end();
		}

		const TerraEngine::Scheduling::FrameWorkloadDeclaration* FindWorkload(
			const std::vector<TerraEngine::Scheduling::FrameWorkloadDeclaration>& workloads,
			std::string_view stableDescriptorId)
		{
			const auto found = std::find_if(workloads.begin(), workloads.end(), [stableDescriptorId](const auto& workload) {
				return workload.stableDescriptorId == stableDescriptorId;
			});
			return found == workloads.end() ? nullptr : &*found;
		}
	}

	void RegisterTerrainSchedulerContractTests(TestRegistry& registry)
	{
		registry.Add("Terrain.Scheduler.SystemDeclarationsExposeExactReadWriteAndBoundaryOwnership", [](TestContext& context) {
			auto& streaming = TerraEngine::Terrain::TerrainStreamingSystem::Instance();
			auto& derived = TerraEngine::Terrain::TerrainDerivedSystem::Instance();
			auto& virtualGeometry = TerraEngine::Resource::VirtualGeometryStreamingSystem::Instance();
			streaming.Initialize();
			derived.Initialize();
			virtualGeometry.Initialize();

			const auto streamingDeclaration = streaming.SchedulerDeclaration();
			const auto derivedDeclaration = derived.SchedulerDeclaration();
			const auto virtualGeometryDeclaration = virtualGeometry.SchedulerDeclaration();
			for(const auto* declaration : { &streamingDeclaration, &derivedDeclaration, &virtualGeometryDeclaration })
			{
				context.Expect(declaration->phase == "Render" && !declaration->mainThread
					&& declaration->phaseKind == TerraEngine::Scheduling::WorkerSystemPhaseKind::WorkerPrepare
					&& declaration->boundaryClassification == TerraEngine::Scheduling::HardBoundaryClassification::PartiallyWorkerSafe
					&& declaration->hardBoundary && !declaration->safeSubwork.empty()
					&& !declaration->protectedCommit.empty(),
					"Terrain and shared virtual-geometry systems must declare worker preparation plus a documented protected publication boundary.");
			}

			context.Expect(Contains(streamingDeclaration.before, "TerrainDerivedSystem")
				&& Contains(streamingDeclaration.before, "VirtualGeometryStreamingSystem")
				&& Contains(derivedDeclaration.after, "TerrainStreamingSystem")
				&& Contains(derivedDeclaration.before, "VirtualGeometryStreamingSystem")
				&& Contains(virtualGeometryDeclaration.before, "RenderSystem"),
				"The declared Render ordering must publish residency, derived terrain, and virtual-geometry pages before render capture.");
			context.Expect(Contains(streamingDeclaration.reads, TerraEngine::Scheduling::kResourceTerrainStreamingDemands)
				&& Contains(streamingDeclaration.writes, TerraEngine::Scheduling::kResourceTerrainPreparedResidency)
				&& Contains(streamingDeclaration.writes, TerraEngine::Scheduling::kResourceTerrainPublishedResidency)
				&& Contains(derivedDeclaration.reads, TerraEngine::Scheduling::kResourceTerrainPublishedResidency)
				&& Contains(derivedDeclaration.writes, TerraEngine::Scheduling::kResourceTerrainPreparedDerived)
				&& Contains(derivedDeclaration.writes, TerraEngine::Scheduling::kResourceTerrainPublishedDerived)
				&& Contains(virtualGeometryDeclaration.reads, TerraEngine::Scheduling::kResourceVirtualGeometryPageResidencySnapshots)
				&& Contains(virtualGeometryDeclaration.writes, TerraEngine::Scheduling::kResourceVirtualGeometryPagePublications)
				&& Contains(virtualGeometryDeclaration.writes, TerraEngine::Scheduling::kResourceVirtualGeometryPageRetirements),
				"Scheduler diagnostics must expose every immutable input, private candidate, publication, and retirement handoff.");

			const auto resources = TerraEngine::Scheduling::DefaultSchedulerResources();
			const std::array requiredResources{
				TerraEngine::Scheduling::kResourceTerrainAssetRoots,
				TerraEngine::Scheduling::kResourceTerrainPreparedResidency,
				TerraEngine::Scheduling::kResourceTerrainPublishedResidency,
				TerraEngine::Scheduling::kResourceTerrainPreparedDerived,
				TerraEngine::Scheduling::kResourceTerrainPublishedDerived,
				TerraEngine::Scheduling::kResourceVirtualGeometryPageResidencySnapshots,
				TerraEngine::Scheduling::kResourceVirtualGeometryPagePublications,
				TerraEngine::Scheduling::kResourceVirtualGeometryPageRetirements,
				TerraEngine::Scheduling::kResourceTerrainDiagnostics
			};
			for(const char* required : requiredResources)
			{
				context.Expect(std::any_of(resources.begin(), resources.end(), [required](const auto& resource) {
					return resource.name == required;
				}), "Every terrain and virtual-geometry handoff must be registered in scheduler diagnostics.");
			}

			virtualGeometry.Shutdown();
			derived.Shutdown();
			streaming.Shutdown();
		});

		registry.Add("Terrain.Scheduler.WorkloadsSeparateParallelPreparationFromAtomicSameFramePublication", [](TestContext& context) {
			auto& streaming = TerraEngine::Terrain::TerrainStreamingSystem::Instance();
			auto& derived = TerraEngine::Terrain::TerrainDerivedSystem::Instance();
			auto& virtualGeometry = TerraEngine::Resource::VirtualGeometryStreamingSystem::Instance();
			streaming.Initialize();
			derived.Initialize();
			virtualGeometry.Initialize();

			const auto streamingWorkloads = streaming.DeclareFrameWorkloads(0.0F);
			const auto derivedWorkloads = derived.DeclareFrameWorkloads(0.0F);
			const auto virtualGeometryWorkloads = virtualGeometry.DeclareFrameWorkloads(0.0F);
			const auto* streamingPrepare = FindWorkload(streamingWorkloads,
				"TerrainStreamingSystem/Render/TerrainStreamingPrepare");
			const auto* streamingPublish = FindWorkload(streamingWorkloads,
				"TerrainStreamingSystem/Render/TerrainStreamingPublish");
			const auto* derivedPrepare = FindWorkload(derivedWorkloads,
				"TerrainDerivedSystem/Render/TerrainDerivedPrepare");
			const auto* derivedPublish = FindWorkload(derivedWorkloads,
				"TerrainDerivedSystem/Render/TerrainDerivedPublish");
			const auto* pagePrepare = FindWorkload(virtualGeometryWorkloads,
				"VirtualGeometryStreamingSystem/Render/StreamingPrepare");
			const auto* pagePublish = FindWorkload(virtualGeometryWorkloads,
				"VirtualGeometryStreamingSystem/Render/PagePublish");
			context.Expect(streamingPrepare != nullptr && streamingPublish != nullptr
				&& derivedPrepare != nullptr && derivedPublish != nullptr
				&& pagePrepare != nullptr && pagePublish != nullptr,
				"Each scheduler owner must expose stable worker and publication descriptor identities.");
			if(streamingPrepare == nullptr || streamingPublish == nullptr || derivedPrepare == nullptr
				|| derivedPublish == nullptr || pagePrepare == nullptr || pagePublish == nullptr)
			{
				virtualGeometry.Shutdown();
				derived.Shutdown();
				streaming.Shutdown();
				return;
			}

			for(const auto* prepare : { streamingPrepare, derivedPrepare, pagePrepare })
			{
				context.Expect(prepare->workerEligible && !prepare->mainThreadRequired
					&& prepare->phaseKind == TerraEngine::Scheduling::WorkerSystemPhaseKind::WorkerPrepare
					&& prepare->boundaryClassification == TerraEngine::Scheduling::HardBoundaryClassification::WorkerSafe
					&& !prepare->safeSubwork.empty(),
					"Preparation descriptors must be worker-safe value jobs with an actionable safety explanation.");
			}
			for(const auto* publish : { streamingPublish, derivedPublish, pagePublish })
			{
				context.Expect(!publish->workerEligible && publish->mainThreadRequired
					&& publish->phaseKind == TerraEngine::Scheduling::WorkerSystemPhaseKind::ProtectedCommit
					&& publish->boundaryClassification == TerraEngine::Scheduling::HardBoundaryClassification::Protected
					&& publish->hardBoundary && !publish->protectedCommit.empty(),
					"Publication descriptors must be protected hard boundaries with an actionable atomic-commit explanation.");
			}
			context.Expect(streamingPublish->explicitDependencies == std::vector<std::string>{streamingPrepare->stableDescriptorId}
				&& derivedPublish->explicitDependencies == std::vector<std::string>{derivedPrepare->stableDescriptorId}
				&& pagePublish->explicitDependencies == std::vector<std::string>{pagePrepare->stableDescriptorId},
				"Each protected publication must explicitly depend on its matching private preparation job.");
			context.Expect(streamingPublish->sameFrameVisibilityRequired
				&& derivedPublish->sameFrameVisibilityRequired && pagePublish->sameFrameVisibilityRequired,
				"Residency, derived terrain, and page publications are consumed later in Render and therefore require same-frame visibility.");

			virtualGeometry.Shutdown();
			derived.Shutdown();
			streaming.Shutdown();
		});
	}
}
