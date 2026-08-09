#include "TestHarness.h"
#include <fstream>
#include <filesystem>
#include <string>
namespace TerraEngine::Tests {
	void RegisterAnimationPublicationSourceGuardTests(TestRegistry& registry)
	{
		registry.Add("Animation.PublicationSourceGuard.HasNoLegacyPosePublicationPath",
			[](TestContext& context) {
				std::ifstream source(std::filesystem::path(TERRAENGINE_SOURCE_DIR)
					/ "Engine/Animation/AnimationSystem.cpp");
				const std::string text(
					(std::istreambuf_iterator<char>(source)), std::istreambuf_iterator<char>());
				std::ifstream authorityHeader(std::filesystem::path(TERRAENGINE_SOURCE_DIR)
					/ "Engine/Animation/Runtime/AnimationAuthorityPublication.h");
				const std::string authorityText(
					(std::istreambuf_iterator<char>(authorityHeader)),
					std::istreambuf_iterator<char>());
				context.Expect(!text.empty(),
					"AnimationSystem.cpp must be available to the source-boundary guard.");
				// Per-record pose pointers permit consumers to combine unrelated frame
				// generations. The authority root is the only supported publication path.
				context.Expect(text.find("PublishedAnimatorPose") == std::string::npos,
					"AnimationSystem must not restore the retired published-pose type.");
				context.Expect(text.find("LoadPublishedAnimatorPose") == std::string::npos,
					"AnimationSystem must not restore per-animator pose lookup APIs.");
				context.Expect(text.find("publishedPosePresent") == std::string::npos,
					"AnimationSystem must not reintroduce mutable record pose-presence state.");
				context.Expect(authorityText.find("Vulkan") == std::string::npos
					&& authorityText.find("DX12") == std::string::npos
					&& authorityText.find("CopyPublishedAnimatorPoses") == std::string::npos,
					"Authority publication contracts must remain backend-neutral and expose no pose-copy compatibility API.");
			});
		registry.Add("Animation.PublicationSourceGuard.ProtectedExchangeDoesNotScanCrowds",
			[](TestContext& context) {
				std::ifstream source(std::filesystem::path(TERRAENGINE_SOURCE_DIR)
					/ "Engine/Animation/AnimationSystem.cpp");
				const std::string text(
					(std::istreambuf_iterator<char>(source)), std::istreambuf_iterator<char>());
				const std::size_t publishBegin = text.find("descriptors[6].work =");
				const std::size_t cacheCommitBegin = text.find("descriptors[7].work =", publishBegin);
				const std::size_t finalizeBegin = text.find("descriptors[8].work =", cacheCommitBegin);
				const std::size_t workloadEnd = text.find("return {descriptors.begin()", finalizeBegin);
				context.Expect(publishBegin != std::string::npos
						&& cacheCommitBegin != std::string::npos
						&& finalizeBegin != std::string::npos,
					"Animation must separate protected root exchange, worker cache commit, and sparse accepted-token finalization.");
				if(publishBegin == std::string::npos
					|| cacheCommitBegin == std::string::npos
					|| finalizeBegin == std::string::npos
					|| workloadEnd == std::string::npos) return;
				const std::string publishRegion = text.substr(
					publishBegin, cacheCommitBegin - publishBegin);
				const std::string cacheCommitRegion = text.substr(
					cacheCommitBegin, finalizeBegin - cacheCommitBegin);
				const std::string finalizeRegion = text.substr(finalizeBegin, workloadEnd - finalizeBegin);
				const std::size_t cacheStore = text.find("evaluationCache.Store");
				const std::size_t specialistBegin = text.find(
					"bool ApplySealedSpecialistSidecars(");
				const std::size_t diagnosticsBegin = text.find(
					"AnimationDiagnosticsSnapshot BuildActiveDiagnosticsLocked(",
					specialistBegin);
				context.Expect(text.find("CommitSealedAuthorityStateLocked") == std::string::npos,
					"The legacy protected crowd-commit path must be removed.");
				context.Expect(cacheStore != std::string::npos && cacheStore < publishBegin
						&& text.find("evaluationCache.Store", cacheStore + 1U) == std::string::npos,
					"Evaluation-cache insertion must have one generation-validated implementation.");
				context.Expect(cacheCommitRegion.find("CommitPublishedEvaluationCacheEntries")
						!= std::string::npos
						&& cacheCommitRegion.find("runtimeOwnershipMutex") == std::string::npos,
					"The dependent cache workload must run on a worker after publication without holding the protected runtime lock across the crowd.");
				context.Expect(publishRegion.find("ApplySealedSpecialistSidecars") == std::string::npos
					&& publishRegion.find("for(") == std::string::npos,
					"PublishAnimationAuthority must only validate scalar seal identity and exchange the immutable root.");
				for(const char* forbidden : {"BuildActiveDiagnosticsLocked", "BuildGpuPresentationFrame",
					"std::make_shared", "RuntimeRecord*", "RuntimeRecord&"}) {
					context.Expect(publishRegion.find(forbidden) == std::string::npos,
						"Protected authority exchange must not construct diagnostics, presentation, allocations, or mutable runtime records.");
				}
				for(const char* forbidden : {"BuildActiveDiagnosticsLocked", "BuildActiveDetailedDiagnosticsLocked",
					"BuildGpuPresentationFrame", "std::make_shared", "deterministicPublicationOrder", "RuntimeRecord*", "RuntimeRecord&"}) {
					context.Expect(finalizeRegion.find(forbidden) == std::string::npos,
						"Protected finalization must not traverse a crowd or construct diagnostics/presentation/runtime state.");
				}
				context.Expect(text.find("FinalizePublishedAuthorityStateLocked") == std::string::npos,
					"The legacy protected finalizer must be replaced by sealed sparse sidecar application.");
				context.Expect(specialistBegin != std::string::npos
						&& diagnosticsBegin != std::string::npos,
					"The source guard must find sealed sparse sidecar application.");
				if(specialistBegin != std::string::npos
					&& diagnosticsBegin != std::string::npos) {
					const std::string specialistRegion = text.substr(
						specialistBegin, diagnosticsBegin - specialistBegin);
					for(const char* forbidden : {"FrameWorkItem", "frameWork", "capturedInputWorkIndices",
						"CompleteWorkForInputIndex",
						"preparedRagdollWorkIndices", "preparedProceduralWorkIndices",
						"preparedRetargetWorkIndices", "preparedAttachmentWorkIndices",
						"preparedAutomaticRootMotionWorkIndices", "capturedRagdollRequests",
						"preparedRagdollPublications", "preparedProceduralPublications",
						"preparedRetargetPublications", "preparedAttachmentPublications",
						"preparedAutomaticRootMotionPublications", "std::any_of"}) {
						context.Expect(specialistRegion.find(forbidden) == std::string::npos,
							"Accepted specialist application must consume sealed typed operations rather than reopen or rescan mutable frame work.");
					}
				}
			});
		registry.Add("Animation.PresentationSourceGuard.ProtectedExchangeUsesPreparedRoots",
			[](TestContext& context) {
				std::ifstream source(std::filesystem::path(TERRAENGINE_SOURCE_DIR)
					/ "Engine/Animation/AnimationPresentationSystem.cpp");
				const std::string text(
					(std::istreambuf_iterator<char>(source)), std::istreambuf_iterator<char>());
				const std::size_t preparationBegin = text.find("BuildPreparedPresentationRoots(");
				const std::size_t publishBegin = text.find("publish.work =");
				const std::size_t publishEnd = text.find("return {descriptors.begin()", publishBegin);
				context.Expect(preparationBegin != std::string::npos && publishBegin != std::string::npos
						&& publishEnd != std::string::npos && preparationBegin < publishBegin,
					"Presentation must build immutable roots in worker preparation before protected publication.");
				if(publishBegin == std::string::npos || publishEnd == std::string::npos) return;
				const std::string publishRegion = text.substr(publishBegin, publishEnd - publishBegin);
				// Presentation can contain thousands of packets. Its protected phase may
				// validate identities and exchange roots, but cannot allocate, sort, or
				// rebuild owner mappings for the whole population.
				for(const char* forbidden : {"std::make_shared", "std::unordered_map",
					"std::sort", "BuildOwnerObjectLookup", "for("}) {
					context.Expect(publishRegion.find(forbidden) == std::string::npos,
						"Protected presentation publication must only exchange worker-prepared immutable roots.");
				}
				context.Expect(publishRegion.find("prepared->publication") != std::string::npos
						&& publishRegion.find("prepared->presentationFrame") != std::string::npos,
					"Protected presentation publication must consume the exact prepared plan and frame roots.");
			});
		registry.Add("Animation.PublicationSourceGuard.CompactRowsUseCoarseNestedTasks",
			[](TestContext& context) {
				std::ifstream source(std::filesystem::path(TERRAENGINE_SOURCE_DIR)
					/ "Engine/Animation/AnimationSystem.cpp");
				const std::string text(
					(std::istreambuf_iterator<char>(source)), std::istreambuf_iterator<char>());
				// Match semantic tokens independently of formatter line wrapping. These
				// source guards protect the coarse-task policy, not whitespace layout.
				context.Expect(text.find("kMinimumCompactRowsPerTask")
						!= std::string::npos
						&& text.find("kMinimumMixedRowsPerTask")
							!= std::string::npos
						&& text.find("availableWorkerCount")
							!= std::string::npos
						&& text.find("usefulWorkerCount")
							!= std::string::npos
						&& text.find("workerIndex = 1U")
							!= std::string::npos
						&& text.find("buildCompactPublicationRows(")
							!= std::string::npos
						&& text.find("buildMixedCompactRows(")
							!= std::string::npos,
					"Cheap compact publication rows must be batched coarsely and use the current scheduler worker for one range instead of launching one nested task per hardware worker.");
			});
	}
}
