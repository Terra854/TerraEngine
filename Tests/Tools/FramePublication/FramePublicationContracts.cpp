// FramePublicationContracts.cpp - Deterministic native publication contracts.
//
// This executable deliberately exercises TerraEngine's real renderer, animation,
// and profiler value contracts.  It does not render a benchmark frame or invent
// performance measurements; the resulting JSON is structural correctness
// evidence that the installed runtime can publish and reject complete roots.

#include "Animation/Runtime/AnimationAuthorityPublication.h"
#include "Profiling/ProfilerCapture.h"
#include "Renderer/Graph/RenderFramePublication.h"

#include <algorithm>
#include <array>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <memory>
#include <numeric>
#include <span>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

namespace
{
	using nlohmann::json;

	TerraEngine::UUID MakeUuid(std::uint64_t value) noexcept
	{
		TerraEngine::UUID id{};
		// A deterministic non-zero UUID keeps contract output reproducible while
		// still satisfying the same identity checks as production publications.
		for(std::size_t index = 0U; index < id.size(); ++index)
		{
			id[index] = static_cast<std::uint8_t>(
				((value >> ((index % 8U) * 8U)) + index + 1U) & 0xffU);
		}
		return id;
	}

	struct RendererFixture
	{
		std::shared_ptr<const TerraEngine::Renderer::RenderPersistentTopologyRoot> persistent;
		std::shared_ptr<const TerraEngine::Renderer::RenderDynamicStateRoot> dynamic;
	};

	RendererFixture BuildRendererRoots(std::uint32_t population,
		std::uint32_t directCount, std::uint64_t sessionGeneration)
	{
		using namespace TerraEngine::Renderer;
		constexpr std::uint32_t kPageCapacity = 256U;
		auto persistent = std::make_shared<RenderPersistentTopologyRoot>();
		auto dynamic = std::make_shared<RenderDynamicStateRoot>();
		persistent->generation = 1U;
		persistent->rendererSessionGeneration = sessionGeneration;
		dynamic->generation = 1U;
		dynamic->rendererSessionGeneration = sessionGeneration;
		for(std::uint32_t first = 0U; first < population; first += kPageCapacity)
		{
			auto persistentPage = std::make_shared<RenderPersistentPage>();
			auto dynamicPage = std::make_shared<RenderDynamicPage>();
			persistentPage->firstOrdinal = first;
			dynamicPage->firstOrdinal = first;
			persistentPage->generation = 1U;
			dynamicPage->generation = 1U;
			const std::uint32_t count = std::min(kPageCapacity, population - first);
			persistentPage->slots.reserve(count);
			dynamicPage->lanes.reserve(count);
			dynamicPage->changedLanes.reserve(count);
			for(std::uint32_t offset = 0U; offset < count; ++offset)
			{
				const std::uint32_t ordinal = first + offset;
				RenderPersistentSlot slot{};
				slot.key = {ordinal, 1U};
				slot.objectId = MakeUuid(static_cast<std::uint64_t>(ordinal) * 3U + 1U);
				slot.meshId = MakeUuid(static_cast<std::uint64_t>(ordinal) * 3U + 2U);
				slot.materialId = MakeUuid(static_cast<std::uint64_t>(ordinal) * 3U + 3U);
				slot.meshGeneration = 1U;
				slot.materialGeneration = 1U;
				slot.resourceGeneration = 1U;
				// Contract ranges deliberately model two reusable packet templates:
				// direct and dense virtual geometry. Every selected entry in a typed
				// range must carry the exact same persistent template identity.
				slot.packetTemplateIdentity = ordinal < directCount
					? 1U : static_cast<std::uint64_t>(directCount) + 1U;
				slot.occupied = true;
				persistentPage->slots.push_back(slot);

				RenderDynamicLane lane{};
				lane.key = slot.key;
				lane.transformGeneration = 1U;
				lane.lifecycleGeneration = 1U;
				lane.occupied = true;
				dynamicPage->lanes.push_back(lane);
				dynamicPage->changedLanes.push_back(true);
			}
			persistent->pages.push_back(std::move(persistentPage));
			dynamic->pages.push_back(std::move(dynamicPage));
		}
		return {std::move(persistent), std::move(dynamic)};
	}

	std::shared_ptr<TerraEngine::Renderer::RenderFramePublication> BuildRendererPublication(
		const RendererFixture& fixture, std::uint32_t population,
		std::uint32_t directCount, std::uint32_t denseCount,
		std::uint64_t frameGeneration, std::uint64_t previousGeneration,
		std::uint64_t sessionGeneration)
	{
		using namespace TerraEngine::Renderer;
		auto publication = std::make_shared<RenderFramePublication>();
		publication->frameId = frameGeneration;
		publication->rendererSessionGeneration = sessionGeneration;
		// Native contract roots model one live installed backend session. A nonzero
		// backend-resource generation is required so the same fixture exercises the
		// stale-swapchain rejection enforced by production publication validation.
		publication->backendResourceGeneration = 1U;
		publication->sceneGeneration = 1U;
		publication->animationPresentationGeneration = 1U;
		publication->expectedPreviousGeneration = previousGeneration;
		publication->generation = frameGeneration;
		publication->persistent = fixture.persistent;
		publication->dynamic = fixture.dynamic;
		auto backendWork = std::make_shared<RenderBackendWorkPublication>();
		backendWork->frameId = frameGeneration;
		backendWork->animationWork.frameId = frameGeneration;
		backendWork->animationWork.valid = true;
		backendWork->valid = true;
		publication->backendWork = std::move(backendWork);
		publication->receipt.reusedPersistentPages = previousGeneration == 0U
			? 0U : static_cast<std::uint32_t>(fixture.persistent->pages.size());
		publication->receipt.changedDynamicPages = previousGeneration == 0U
			? static_cast<std::uint32_t>(fixture.dynamic->pages.size()) : 0U;

		RenderViewSelection view{};
		view.generation = frameGeneration;
		view.visibleOrdinals.reserve(population);
		view.selectedLods.reserve(population);
		view.routeSelectors.reserve(population);
		view.packetTemplateIdentities.reserve(population);
		view.tintSelectors.reserve(population);
		view.editorTints.reserve(population);
		for(std::uint32_t index = 0U; index < population; ++index)
		{
			view.visibleOrdinals.push_back({index, 1U});
			view.selectedLods.push_back(0U);
			view.routeSelectors.push_back(index < directCount ? 0U : 2U);
			view.packetTemplateIdentities.push_back(index < directCount
				? 1U : static_cast<std::uint64_t>(directCount) + 1U);
			view.tintSelectors.push_back(0U);
			view.editorTints.emplace_back(0.0F);
		}
		publication->views.push_back(std::move(view));
		RenderViewPresentationInput presentation{};
		presentation.output.kind = publication->views.front().outputKind;
		presentation.output.diagnosticsView = publication->views.front().viewType;
		presentation.output.outputId = publication->views.front().outputId;
		presentation.commandLanes.resize(population);
		presentation.commandSelectionIndices.resize(population);
		std::iota(presentation.commandSelectionIndices.begin(),
			presentation.commandSelectionIndices.end(), 0U);
		publication->presentationInputs.push_back(std::move(presentation));
		if(directCount != 0U)
		{
			publication->submissionRanges.push_back({RenderSubmissionRoute::Direct,
				0U, 0U, directCount, 1U, directCount == 1U});
		}
		if(denseCount != 0U)
		{
			publication->submissionRanges.push_back({RenderSubmissionRoute::VirtualGeometry,
				0U, directCount, denseCount, static_cast<std::uint64_t>(directCount) + 1U,
				denseCount == 1U});
		}
		return publication;
	}

	std::vector<std::shared_ptr<const TerraEngine::Animation::AnimationAuthorityPage>>
		BuildAnimationPages(std::uint32_t population, std::uint64_t sessionGeneration)
	{
		using namespace TerraEngine::Animation;
		std::vector<std::shared_ptr<const AnimationAuthorityPage>> pages;
		constexpr std::uint32_t kCapacity =
			static_cast<std::uint32_t>(kAnimationAuthorityPageCapacity128);
		for(std::uint32_t first = 0U; first < population; first += kCapacity)
		{
			auto page = std::make_shared<AnimationAuthorityPage>();
			page->firstSlot = first;
			page->capacity = kCapacity;
			page->changedMasks.assign((kCapacity + 63U) / 64U, ~0ULL);
			const std::uint32_t count = std::min(kCapacity, population - first);
			page->rows.reserve(count);
			for(std::uint32_t offset = 0U; offset < count; ++offset)
			{
				const std::uint32_t slot = first + offset;
				AnimationAuthorityRow row{};
				row.key = {slot, 1U, sessionGeneration};
				row.ownerObjectId = MakeUuid(static_cast<std::uint64_t>(slot) + 1U);
				row.componentGeneration = 1U;
				row.sourceGeneration = 1U;
				row.poseGeneration = 1U;
				row.authorityPublicationGeneration = 1U;
				row.tick = 1;
				row.stateId = 1U;
				row.active = true;
				row.safeFallback = true;
				page->rows.push_back(std::move(row));
			}
			pages.push_back(std::move(page));
		}
		return pages;
	}

	TerraEngine::Animation::SealedAnimationFrameResult BuildAnimationPublication(
		std::span<const std::shared_ptr<const TerraEngine::Animation::AnimationAuthorityPage>> pages,
		std::uint32_t population, std::uint64_t frameGeneration,
		std::uint64_t previousGeneration, std::uint64_t sessionGeneration,
		std::uint32_t reusedPages, std::uint32_t changedPages)
	{
		using namespace TerraEngine::Animation;
		auto sidecars = std::make_shared<AnimationFrameSidecars>();
		sidecars->summary.evaluatedRows = population;
		sidecars->summary.fallbackRows = population;
		AnimationAuthoritySealInput input{};
		input.frameId = frameGeneration;
		input.sessionGeneration = sessionGeneration;
		input.lifecycleGeneration = 1U;
		input.expectedPreviousGeneration = previousGeneration;
		input.candidateGeneration = frameGeneration;
		input.pages = pages;
		input.sidecars = std::move(sidecars);
		input.reusedPages = reusedPages;
		input.changedPages = changedPages;
		input.workersComplete = true;
		input.historyReserved = true;
		return SealAnimationFrameResult(input);
	}

	json RunScenario(const json& scenario)
	{
		using namespace TerraEngine;
		constexpr std::uint64_t kSession = 7U;
		const std::uint32_t population = scenario.at("population").get<std::uint32_t>();
		const std::uint32_t direct = scenario.at("routeMix").at("direct").get<std::uint32_t>();
		const std::uint32_t dense = scenario.at("routeMix").at("denseVirtualGeometry").get<std::uint32_t>();
		const bool animated = scenario.at("animationPolicy").get<std::string>()
			!= "static-sparse-change";

		const RendererFixture rendererRoots = BuildRendererRoots(
			population, direct, kSession);
		Renderer::RenderFramePublicationStore rendererStore;
		rendererStore.ResetForSession(kSession);
		const auto rendererFirst = BuildRendererPublication(rendererRoots, population,
			direct, dense, 1U, 0U, kSession);
		const Renderer::RenderPublicationReceipt rendererFirstReceipt =
			rendererStore.TryPublish(Renderer::SealRenderFramePublication(
				rendererFirst, kSession), kSession);
		const auto rendererFirstRoot = rendererStore.Load();
		const auto rendererRejected = BuildRendererPublication(rendererRoots, population,
			direct, dense, 2U, 1U, kSession);
		const Renderer::RenderPublicationReceipt rendererFailureReceipt =
			rendererStore.TryPublish(Renderer::SealRenderFramePublication(
				rendererRejected, kSession, {true, false, false}), kSession);
		const bool rendererFailureAtomic = rendererStore.Load() == rendererFirstRoot;
		const auto rendererSecond = BuildRendererPublication(rendererRoots, population,
			direct, dense, 2U, 1U, kSession);
		const Renderer::RenderPublicationReceipt rendererSecondReceipt =
			rendererStore.TryPublish(Renderer::SealRenderFramePublication(
				rendererSecond, kSession), kSession);
		const auto rendererSecondRoot = rendererStore.Load();
		const auto rendererStale = BuildRendererPublication(rendererRoots, population,
			direct, dense, 3U, 0U, kSession);
		const Renderer::RenderPublicationReceipt rendererStaleReceipt =
			rendererStore.TryPublish(Renderer::SealRenderFramePublication(
				rendererStale, kSession), kSession);
		const bool rendererStaleAtomic = rendererStore.Load() == rendererSecondRoot;
		std::uint64_t selectedCount = 0U;
		for(const Renderer::RenderSubmissionRange& range : rendererSecond->submissionRanges)
		{
			selectedCount += range.selectionCount;
		}

		const std::uint32_t animationPopulation = animated ? population : 0U;
		const auto animationPages = BuildAnimationPages(animationPopulation, kSession);
		Animation::AnimationAuthorityPublisher animationPublisher;
		const auto animationFirst = BuildAnimationPublication(animationPages,
			animationPopulation, 1U, 0U, kSession, 0U,
			static_cast<std::uint32_t>(animationPages.size()));
		const Animation::AnimationPublicationReceipt animationFirstReceipt =
			animationPublisher.Publish(animationFirst, kSession, 1U);
		const auto animationFirstRoot = animationPublisher.Load();
		Animation::SetAnimationAuthorityFailureInjectionForTesting(
			Animation::AnimationAuthorityPublicationOutcome::RejectedAllocation);
		const auto animationFailedSeal = BuildAnimationPublication(animationPages,
			animationPopulation, 2U, 1U, kSession, 0U, 0U);
		Animation::ClearAnimationAuthorityFailureInjectionForTesting();
		const bool animationFailureAtomic = !animationFailedSeal.IsValid()
			&& animationPublisher.Load() == animationFirstRoot;
		const auto animationSecond = BuildAnimationPublication(animationPages,
			animationPopulation, 2U, 1U, kSession,
			static_cast<std::uint32_t>(animationPages.size()), 0U);
		const Animation::AnimationPublicationReceipt animationSecondReceipt =
			animationPublisher.Publish(animationSecond, kSession, 1U);
		const auto animationSecondRoot = animationPublisher.Load();
		const auto animationStale = BuildAnimationPublication(animationPages,
			animationPopulation, 3U, 0U, kSession, 0U, 0U);
		const Animation::AnimationPublicationReceipt animationStaleReceipt =
			animationPublisher.Publish(animationStale, kSession, 1U);
		const bool animationStaleAtomic = animationPublisher.Load() == animationSecondRoot;

		Profiling::ProfilerFramePublicationEvidence profilerEvidence{};
		profilerEvidence.frameGeneration = 2U;
		profilerEvidence.sceneGeneration = 1U;
		profilerEvidence.renderSessionGeneration = kSession;
		profilerEvidence.topologyGeneration = 1U;
		profilerEvidence.dynamicGeneration = 1U;
		profilerEvidence.authorityGeneration = 2U;
		profilerEvidence.presentationGeneration = 2U;
		profilerEvidence.viewCount = 1U;
		profilerEvidence.views[0].valid = true;
		profilerEvidence.views[0].frameGeneration = 2U;
		profilerEvidence.disjointTimingNanoseconds =
			{180U, 120U, 250U, 160U, 90U, 100U, 50U, 50U};
		const bool profilerEvidenceValid =
			Profiling::ValidateProfilerFramePublicationEvidence(profilerEvidence);
		const bool profilerTimingConserved = Profiling::ValidateProfilerDisjointTimingBuckets(
			profilerEvidence.disjointTimingNanoseconds, 1000U, 10U);

		const bool passed = rendererFirstReceipt.IsSuccess()
			&& rendererSecondReceipt.IsSuccess() && !rendererFailureReceipt.IsSuccess()
			&& !rendererStaleReceipt.IsSuccess() && rendererFailureAtomic
			&& rendererStaleAtomic && selectedCount == population
			&& animationFirstReceipt.Published() && animationSecondReceipt.Published()
			&& animationFailureAtomic
			&& animationStaleReceipt.outcome
				== Animation::AnimationAuthorityPublicationOutcome::RejectedStalePredecessor
			&& animationStaleAtomic && profilerEvidenceValid && profilerTimingConserved;

		return {
			{"id", scenario.at("id")},
			{"status", passed ? "passed" : "failed"},
			{"population", population},
			{"renderer", {
				{"firstGeneration", rendererFirstReceipt.publicationGeneration},
				{"secondGeneration", rendererSecondReceipt.publicationGeneration},
				{"persistentPages", rendererRoots.persistent->pages.size()},
				{"reusedPersistentPages", rendererSecondReceipt.reusedPersistentPages},
				{"changedDynamicPages", rendererSecondReceipt.changedDynamicPages},
				{"selectedCount", selectedCount},
				{"rangeCount", rendererSecondReceipt.rangeCount},
				{"failureAtomic", rendererFailureAtomic},
				{"staleAtomic", rendererStaleAtomic}
			}},
			{"animation", {
				{"population", animationPopulation},
				{"firstGeneration", animationFirstReceipt.authorityGeneration},
				{"secondGeneration", animationSecondReceipt.authorityGeneration},
				{"pageCount", animationPages.size()},
				{"reusedPages", animationSecondReceipt.reusedPages},
				{"changedPages", animationSecondReceipt.changedPages},
				{"failureAtomic", animationFailureAtomic},
				{"staleAtomic", animationStaleAtomic}
			}},
			{"profiler", {
				{"source", "native-contract"},
				{"frameNanoseconds", 1000U},
				{"toleranceNanoseconds", 10U},
				{"evidenceValid", profilerEvidenceValid},
				{"timingConserved", profilerTimingConserved}
			}}
		};
	}
}

int main(int argc, char** argv)
{
	std::filesystem::path manifestPath;
	std::filesystem::path outputPath;
	bool contractMode = false;
	for(int index = 1; index < argc; ++index)
	{
		const std::string argument = argv[index];
		if(argument == "--frame-publication-contracts")
		{
			contractMode = true;
		}
		else if(argument == "--manifest" && index + 1 < argc)
		{
			manifestPath = argv[++index];
		}
		else if(argument == "--output" && index + 1 < argc)
		{
			outputPath = argv[++index];
		}
	}
	if(!contractMode || manifestPath.empty() || outputPath.empty())
	{
		std::cerr << "Usage: --frame-publication-contracts --manifest <file> --output <directory>\n";
		return 2;
	}

	try
	{
		std::ifstream manifestInput(manifestPath);
		if(!manifestInput)
		{
			std::cerr << "Manifest is unavailable: " << manifestPath << '\n';
			return 3;
		}
		json manifest;
		manifestInput >> manifest;
		if(manifest.value("schemaVersion", 0) != 1
			|| !manifest.contains("scenarios") || !manifest["scenarios"].is_array()
			|| manifest["scenarios"].size() != 6U)
		{
			std::cerr << "The canonical six-scenario manifest is invalid.\n";
			return 4;
		}

		json scenarioReports = json::array();
		bool passed = true;
		for(const json& scenario : manifest["scenarios"])
		{
			json report = RunScenario(scenario);
			passed = passed && report.value("status", "failed") == "passed";
			scenarioReports.push_back(std::move(report));
		}
		std::filesystem::create_directories(outputPath);
		std::ofstream output(outputPath / "contracts.json", std::ios::trunc);
		if(!output)
		{
			std::cerr << "Contract report could not be created.\n";
			return 5;
		}
		output << json{
			{"schemaVersion", 1},
			{"status", passed ? "passed" : "failed"},
			{"scenarioCount", scenarioReports.size()},
			{"scenarios", std::move(scenarioReports)}
		}.dump(2) << '\n';
		return passed ? 0 : 10;
	}
	catch(const std::exception& exception)
	{
		std::cerr << "Contract execution failed: " << exception.what() << '\n';
		return 9;
	}
}
