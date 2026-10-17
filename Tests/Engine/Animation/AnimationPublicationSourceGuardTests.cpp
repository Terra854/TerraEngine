#include "TestHarness.h"
#include <fstream>
#include <filesystem>
#include <stdexcept>
#include <string>
#include <vector>
namespace TerraEngine::Tests {
	namespace {
		// Missing files must fail even when the caller checks only forbidden text.
		std::string ReadAnimationGuardSource(const std::filesystem::path& relative)
		{
			std::ifstream source(std::filesystem::path(TERRAENGINE_SOURCE_DIR) / relative);
			if(!source) throw std::runtime_error("Animation source guard cannot read " + relative.generic_string());
			std::string text((std::istreambuf_iterator<char>(source)), {});
			if(source.bad() || text.empty()) throw std::runtime_error("Animation source guard found empty/unreadable source " + relative.generic_string());
			return text;
		}
		// Global exclusions cover all coordinator siblings and private stage headers;
		// concatenation is deliberately avoided for ordering-sensitive assertions.
		std::vector<std::string> AnimationGuardClosure()
		{
			const auto root = std::filesystem::path(TERRAENGINE_SOURCE_DIR) / "Engine/Animation";
			std::vector<std::string> sources;
			for(const auto& entry : std::filesystem::recursive_directory_iterator(root)) {
				if(!entry.is_regular_file()) continue;
				const auto relative = entry.path().lexically_relative(std::filesystem::path(TERRAENGINE_SOURCE_DIR));
				const auto extension = entry.path().extension().string();
				if(extension != ".cpp" && extension != ".h" && extension != ".hpp" && extension != ".inl") continue;
				if(entry.path().filename().string().starts_with("AnimationSystem")
					|| relative.generic_string().starts_with("Engine/Animation/Frame/"))
					sources.push_back(ReadAnimationGuardSource(relative));
			}
			if(sources.empty()) throw std::runtime_error("Animation source closure is empty");
			return sources;
		}
		// Select definitions using two required, ordered markers in the SAME file.
		// Missing/reversed boundaries throw instead of silently skipping assertions.
		std::string AnimationGuardRegion(const std::string& text, const char* beginMarker, const char* endMarker)
		{
			const auto begin = text.find(beginMarker);
			const auto end = begin == std::string::npos ? std::string::npos : text.find(endMarker, begin + std::string(beginMarker).size());
			if(begin == std::string::npos || end == std::string::npos || end <= begin)
				throw std::runtime_error(std::string("Animation source definition boundaries unavailable: ") + beginMarker);
			return text.substr(begin, end - begin);
		}
	}
	void RegisterAnimationPublicationSourceGuardTests(TestRegistry& registry)
	{
		registry.Add("Animation.PublicationSourceGuard.HasNoLegacyPosePublicationPath",
			[](TestContext& context) {
				const auto closure = AnimationGuardClosure();
				const auto authority = ReadAnimationGuardSource("Engine/Animation/Runtime/AnimationAuthorityPublication.h");
				// Relocation must not hide compatibility state in a private module/header.
				for(const auto& text : closure) {
					for(const char* forbidden : {"PublishedAnimatorPose", "LoadPublishedAnimatorPose", "publishedPosePresent",
						"CopyPublishedAnimatorPoses", "CommitSealedAuthorityStateLocked", "FinalizePublishedAuthorityStateLocked"})
						context.Expect(text.find(forbidden) == std::string::npos,
							"The complete animation stage closure must not restore legacy pose-copy or crowd-commit paths.");
				}
				context.Expect(authority.find("Vulkan") == std::string::npos
					&& authority.find("DX12") == std::string::npos
					&& authority.find("CopyPublishedAnimatorPoses") == std::string::npos,
					"The required authority contract must remain backend-neutral without pose-copy compatibility APIs.");
			});
		registry.Add("Animation.PublicationSourceGuard.ProtectedExchangeDoesNotScanCrowds",
			[](TestContext& context) {
				const auto assembly = ReadAnimationGuardSource("Engine/Animation/AnimationSystem.cpp");
				const auto publication = ReadAnimationGuardSource("Engine/Animation/Frame/AnimationAuthorityPublication.cpp");
				const auto runtime = ReadAnimationGuardSource("Engine/Animation/Frame/AnimationFrameRuntime.h");
				const auto publishCallback = AnimationGuardRegion(assembly, "descriptors[6].work =", "descriptors[7].work =");
				const auto cacheCallback = AnimationGuardRegion(assembly, "descriptors[7].work =", "descriptors[8].work =");
				const auto finalizeCallback = AnimationGuardRegion(assembly, "descriptors[8].work =", "return {descriptors.begin()");
				context.Expect(assembly.find("std::array<FrameWorkloadDeclaration, 9> descriptors") != std::string::npos
					&& publishCallback.find("impl_->PublishAnimationAuthorityStage(") != std::string::npos
					&& cacheCallback.find("impl_->CommitPublishedAnimationCacheStage(") != std::string::npos
					&& finalizeCallback.find("impl_->FinalizePublishedAnimationStage(") != std::string::npos,
					"The unchanged nine-callback assembly must delegate publication, dependent cache work and finalization in order.");
				for(const char* dependency : {
					"descriptors[6].explicitDependencies = {descriptors[5].stableDescriptorId}",
					"descriptors[7].explicitDependencies = {descriptors[6].stableDescriptorId}",
					"descriptors[8].explicitDependencies = {descriptors[7].stableDescriptorId}"})
					context.Expect(assembly.find(dependency) != std::string::npos,
						"Publication, cache follow-up and sparse finalization must retain their explicit dependencies.");
				// Inspect actual compiled helpers, not just the small forwarding callbacks.
				const auto cache = AnimationGuardRegion(publication,
					"bool AnimationSystem::Impl::CommitPublishedEvaluationCacheEntries(",
					"bool AnimationSystem::Impl::ApplySealedSpecialistSidecars(");
				const auto specialist = AnimationGuardRegion(publication,
					"bool AnimationSystem::Impl::ApplySealedSpecialistSidecars(",
					"void AnimationSystem::Impl::PublishCommandCompletionsLocked(");
				const auto publish = AnimationGuardRegion(publication,
					"void AnimationSystem::Impl::PublishAnimationAuthorityStage(",
					"void AnimationSystem::Impl::CommitPublishedAnimationCacheStage(");
				const auto cacheStage = AnimationGuardRegion(publication,
					"void AnimationSystem::Impl::CommitPublishedAnimationCacheStage(",
					"void AnimationSystem::Impl::FinalizePublishedAnimationStage(");
				const auto finalize = AnimationGuardRegion(publication,
					"void AnimationSystem::Impl::FinalizePublishedAnimationStage(",
					"} // namespace TerraEngine::Animation");
				std::size_t cacheStores = 0;
				for(const auto& text : AnimationGuardClosure()) {
					for(auto position = text.find("evaluationCache.Store"); position != std::string::npos;
						position = text.find("evaluationCache.Store", position + 1)) ++cacheStores;
				}
				context.Expect(cacheStores == 1 && cache.find("evaluationCache.Store") != std::string::npos
					&& cache.find("token.nextRuntimeRoot") != std::string::npos
					&& cache.find("authorityPublisher.Load()") != std::string::npos,
					"Exactly one cache insertion implementation must validate the accepted generation and retained roots.");
				context.Expect(cacheStage.find("this->CommitPublishedEvaluationCacheEntries(") != std::string::npos
					&& cacheStage.find("runtimeOwnershipMutex") == std::string::npos
					&& cache.find("runtimeOwnershipMutex") == std::string::npos,
					"The dependent worker cache stage and its implementation must not hold the protected runtime lock across the crowd.");
				context.Expect(runtime.find("PublishAnimationAuthorityStage(") != std::string::npos
					&& runtime.find("CommitPublishedAnimationCacheStage(") != std::string::npos
					&& runtime.find("FinalizePublishedAnimationStage(") != std::string::npos
					&& publish.find("authorityPublisher.Publish(") != std::string::npos
					&& publish.find("runtimeRecords.store(token.nextRuntimeRoot") != std::string::npos
					&& publish.find("compactRuntimeStates.store(token.nextCompactRuntimeRoot") != std::string::npos,
					"The declared protected helper must exchange the sealed authority and exact prepared runtime roots.");
				for(const auto* region : {&publishCallback, &publish}) {
					for(const char* forbidden : {"ApplySealedSpecialistSidecars", "for(", "for (", "while(", "while (",
						"BuildActiveDiagnosticsLocked", "BuildGpuPresentationFrame", "std::make_shared", "RuntimeRecord*", "RuntimeRecord&"})
						context.Expect(region->find(forbidden) == std::string::npos,
							"Protected exchange must remain scalar and consume sealed roots rather than rebuilding a crowd.");
				}
				for(const char* forbidden : {"BuildActiveDiagnosticsLocked", "BuildActiveDetailedDiagnosticsLocked",
					"BuildGpuPresentationFrame", "std::make_shared", "deterministicPublicationOrder", "RuntimeRecord*", "RuntimeRecord&"})
					context.Expect(finalize.find(forbidden) == std::string::npos,
						"The actual protected finalizer must not construct crowd-sized diagnostics, presentation or runtime state.");
				context.Expect(finalize.find("this->ApplySealedSpecialistSidecars(token)") != std::string::npos,
					"Sparse finalization must consume the sealed specialist implementation checked below.");
				for(const char* forbidden : {"FrameWorkItem", "frameWork", "capturedInputWorkIndices", "CompleteWorkForInputIndex",
					"preparedRagdollWorkIndices", "preparedProceduralWorkIndices", "preparedRetargetWorkIndices", "preparedAttachmentWorkIndices",
					"preparedAutomaticRootMotionWorkIndices", "capturedRagdollRequests", "preparedRagdollPublications",
					"preparedProceduralPublications", "preparedRetargetPublications", "preparedAttachmentPublications",
					"preparedAutomaticRootMotionPublications", "std::any_of"})
					context.Expect(specialist.find(forbidden) == std::string::npos,
						"Specialist application must consume sealed typed operations without reopening mutable frame work.");
			});
		registry.Add("Animation.PresentationSourceGuard.ProtectedExchangeUsesPreparedRoots",
			[](TestContext& context) {
				const auto text = ReadAnimationGuardSource("Engine/Animation/AnimationPresentationSystem.cpp");
				const auto publish = AnimationGuardRegion(text, "publish.work =", "return {descriptors.begin()");
				const auto preparation = text.find("BuildPreparedPresentationRoots(");
				context.Expect(preparation != std::string::npos && preparation < text.find("publish.work ="),
					"Presentation must prepare immutable roots before protected publication.");
				// This separate system retains its actual protected callback definition.
				for(const char* forbidden : {"std::make_shared", "std::unordered_map", "std::sort", "BuildOwnerObjectLookup", "for(", "for ("})
					context.Expect(publish.find(forbidden) == std::string::npos,
						"Protected presentation must exchange prepared roots without allocation, sorting or population scans.");
				context.Expect(publish.find("prepared->publication") != std::string::npos
					&& publish.find("prepared->presentationFrame") != std::string::npos,
					"Protected presentation must consume the exact prepared plan and frame roots.");
			});
		registry.Add("Animation.PublicationSourceGuard.CompactRowsUseCoarseNestedTasks",
			[](TestContext& context) {
				const auto text = ReadAnimationGuardSource("Engine/Animation/Frame/AnimationAuthoritySealing.cpp");
				const auto homogeneous = AnimationGuardRegion(text,
					"bool AnimationSystem::Impl::PrepareHomogeneousAuthorityRows(", "bool AnimationSystem::Impl::PrepareMixedAuthorityRows(");
				const auto mixed = AnimationGuardRegion(text,
					"bool AnimationSystem::Impl::PrepareMixedAuthorityRows(", "bool AnimationSystem::Impl::PrepareOrdinaryAuthorityRows(");
				for(const auto* region : {&homogeneous, &mixed}) {
					for(const char* required : {"availableWorkerCount", "usefulWorkerCount", "workerIndex = 1U", "futuresSettlement.RunInline(", "futuresSettlement.GetAll()"})
						context.Expect(region->find(required) != std::string::npos,
							"Each compact-row builder must use coarse tasks, one local worker range and settled children.");
				}
				context.Expect(homogeneous.find("kMinimumCompactRowsPerTask") != std::string::npos
					&& homogeneous.find("buildCompactPublicationRows(") != std::string::npos
					&& mixed.find("kMinimumMixedRowsPerTask") != std::string::npos
					&& mixed.find("buildMixedCompactRows(") != std::string::npos,
					"Both actual row-builder definitions must retain their independent coarse batching policy.");
			});
	}
}
