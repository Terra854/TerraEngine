// VirtualGeometryStreamingEndToEndTests.cpp - worker preparation to protected publication.

#include "Core/Scheduling/JobGraph.h"
#include "Resource/ResourceStreaming.h"
#include "Resource/VirtualGeometryStreamingSystem.h"
#include "TestHarness.h"

#include <atomic>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <memory>
#include <vector>

namespace TerraEngine::Tests
{
	namespace
	{
		Resource::VirtualGeometryPagePublicationCandidate MakeCompletedCandidate(
			const TerraEngine::UUID& assetId,
			std::uint64_t publicationGeneration)
		{
			Resource::VirtualGeometryPagePublicationCandidate candidate;
			candidate.candidateId = 100u + publicationGeneration;
			candidate.publicationGeneration = publicationGeneration;
			candidate.key.asset.assetId = assetId;
			candidate.key.asset.sourceHash = 1u;
			candidate.key.asset.buildRecipeHash = 1u;
			candidate.key.pageId = 7u;
			candidate.key.generations = {
				publicationGeneration,
				publicationGeneration,
				publicationGeneration,
				publicationGeneration,
				publicationGeneration
			};
			candidate.key.pageContentGeneration = publicationGeneration;
			candidate.payload = { 16u, 32u, 0x1234u, true, true };
			candidate.hierarchy = { { 3u }, 0u, true, true };
			candidate.descriptor = { 200u + publicationGeneration, 1u, 2u, true };
			candidate.dependencyPageIds = { 0u };
			candidate.dependenciesVerified = true;
			candidate.residencyTransitionVerified = true;
			return candidate;
		}

		Resource::VirtualGeometryStreamingFeedback MakeExactRangeFeedback(
			const TerraEngine::UUID& assetId,
			const std::filesystem::path& sourcePath,
			const std::vector<std::uint8_t>& sourceBytes,
			std::shared_ptr<const std::atomic_bool> cancellation = nullptr)
		{
			Resource::VirtualGeometryStreamingFeedback feedback;
			feedback.assetId = assetId;
			feedback.pageId = 7u;
			feedback.fallbackPageId = 7u;
			feedback.priority = 5;
			feedback.observedFrame = 1u;
			feedback.visible = true;
			feedback.expectedStoredByteCount = sourceBytes.size();
			feedback.expectedDecodedByteCount = sourceBytes.size();
			feedback.expectedIntegrityValue = CalculateVirtualGeometryCompactHash(sourceBytes);
			TemfBlockDescriptor descriptor{};
			descriptor.blockId = 1u;
			descriptor.logicalPageId = feedback.pageId;
			descriptor.streamKind = static_cast<std::uint16_t>(TemfStreamKind::Metadata);
			descriptor.codecProfileId = static_cast<std::uint32_t>(TemfCodecProfile::ExpandedProduction);
			descriptor.codecVersion = 1u;
			descriptor.encodedPayloadOffset = 2u;
			descriptor.encodedBytes = sourceBytes.size();
			descriptor.decodedBytes = sourceBytes.size();
			descriptor.decodedElementCount = static_cast<std::uint32_t>(sourceBytes.size());
			descriptor.decodedStride = 1u;
			descriptor.requiredAlignment = 1u;
			descriptor.encodedIntegrity = CalculateVirtualGeometryCompactHash(sourceBytes);
			descriptor.decodedIntegrity = descriptor.encodedIntegrity;
			feedback.exactBlockReads.push_back({
				{ sourcePath, feedback.pageId, 2u, sourceBytes.size(),
					descriptor.encodedIntegrity, 0u, std::move(cancellation) },
				descriptor
			});
			return feedback;
		}

		TemfBlockDescriptor MakeIdentityProductionBlock(
			std::uint32_t pageId, std::uint64_t blockId,
			std::uint64_t sourceOffset, const std::vector<std::uint8_t>& bytes)
		{
			// ExpandedProduction is deliberately identity-coded in this test. That
			// makes each assertion about ordering and atomic concatenation explicit.
			TemfBlockDescriptor descriptor{};
			descriptor.blockId = blockId;
			descriptor.logicalPageId = pageId;
			descriptor.blockOrdinal = static_cast<std::uint16_t>(blockId - 1u);
			descriptor.streamKind = static_cast<std::uint16_t>(TemfStreamKind::Metadata);
			descriptor.codecProfileId = static_cast<std::uint32_t>(TemfCodecProfile::ExpandedProduction);
			descriptor.codecVersion = 1u;
			descriptor.encodedPayloadOffset = sourceOffset;
			descriptor.encodedBytes = bytes.size();
			descriptor.decodedBytes = bytes.size();
			descriptor.decodedElementCount = static_cast<std::uint32_t>(bytes.size());
			descriptor.decodedStride = 1u;
			descriptor.requiredAlignment = 1u;
			descriptor.encodedIntegrity = CalculateVirtualGeometryCompactHash(bytes);
			descriptor.decodedIntegrity = descriptor.encodedIntegrity;
			return descriptor;
		}

	}

	void RegisterVirtualGeometryStreamingEndToEndTests(TestRegistry& registry)
	{
		registry.Add("VirtualGeometry.Streaming.WorkerPreparationPublishesOnlyAtProtectedBoundary",
			[](TestContext& context)
			{
				auto& system = Resource::VirtualGeometryStreamingSystem::Instance();
				system.Initialize();
				const auto asset = TerraEngine::GenerateUUID();
				system.SubmitPriorFrameFeedback({ { asset, 7u, 0u, { 3u }, 4, 1u, true } });
				system.SubmitCompletedPublicationCandidates({ MakeCompletedCandidate(asset, 1u) });

				auto workloads = system.DeclareFrameWorkloads(0.0f);
				TerraEngine::Scheduling::JobGraphContext contextValue;
				context.ExpectEqual(static_cast<std::size_t>(2u), workloads.size(),
					"Streaming declares separate worker and protected workloads.");
				context.Expect(workloads.front().workerEligible && workloads.back().mainThreadRequired,
					"Planning stays worker-safe while publication remains protected.");
				workloads.front().work(contextValue);
				context.Expect(system.PublishedPages().empty(),
					"Prepared candidates must remain private before the protected PagePublish callback.");
				context.ExpectEqual(static_cast<std::size_t>(3u), system.LastPreparedPlan().decodeIntents.size(),
					"Requested page, fallback, and dependency should all receive worker decode intents.");
				workloads.back().work(contextValue);
				const auto pages = system.PublishedPages();
				const auto diagnostics = system.DiagnosticsSnapshot();
				context.ExpectEqual(static_cast<std::size_t>(1u), pages.size(),
					"Only the protected callback may expose an accepted page publication.");
				context.Expect(pages.front().IsVisible(),
					"The protected store must expose only a complete verified page record.");
				context.ExpectEqual(1u, diagnostics.acceptedPublicationBatches,
					"Diagnostics must report the accepted protected transaction.");
				system.Shutdown();
			});

		registry.Add("VirtualGeometry.Streaming.RejectedProtectedBatchRetainsPriorPublication",
			[](TestContext& context)
			{
				auto& system = Resource::VirtualGeometryStreamingSystem::Instance();
				system.Initialize();
				const auto asset = TerraEngine::GenerateUUID();
				system.SubmitPriorFrameFeedback({ { asset, 7u, 0u, {}, 1, 1u, true } });
				system.SubmitCompletedPublicationCandidates({ MakeCompletedCandidate(asset, 1u) });
				auto workloads = system.DeclareFrameWorkloads(0.0f);
				TerraEngine::Scheduling::JobGraphContext contextValue;
				workloads.front().work(contextValue);
				workloads.back().work(contextValue);
				const auto prior = system.PublishedPages();

				auto invalid = MakeCompletedCandidate(asset, 2u);
				invalid.descriptor.uploadCompleted = false;
				system.SubmitPriorFrameFeedback({ { asset, 7u, 0u, {}, 1, 2u, true } });
				system.SubmitCompletedPublicationCandidates({ invalid });
				workloads = system.DeclareFrameWorkloads(0.0f);
				workloads.front().work(contextValue);
				workloads.back().work(contextValue);
				const auto diagnostics = system.DiagnosticsSnapshot();
				context.ExpectEqual(prior.size(), system.PublishedPages().size(),
					"Rejected protected batches must leave the prior visible publication set unchanged.");
				context.ExpectEqual(1u, diagnostics.rejectedPublications,
					"Diagnostics must count rejected candidate rows without reporting a successful publication.");
				context.Expect(!diagnostics.lastRejectionReason.empty(),
					"Rejected publication diagnostics must retain the validator reason.");
				const auto residency = system.ResidencySnapshot();
				context.ExpectEqual(static_cast<std::size_t>(1u), residency.pages.size(),
					"A rejected replacement must preserve the immutable last-known-good residency row.");
				context.Expect(residency.pages.front().state == Resource::VirtualGeometryPagePublicationState::Resident,
					"A failed candidate cannot downgrade a page whose prior GPU publication is resident.");
				system.Shutdown();
			});

		registry.Add("VirtualGeometry.Streaming.ExactRangeReadPublishesPendingUploadThenNextFrameResident",
			[](TestContext& context)
			{
				auto& system = Resource::VirtualGeometryStreamingSystem::Instance();
				system.Initialize();
				const auto asset = TerraEngine::GenerateUUID();
				const std::vector<std::uint8_t> sourceBytes{ 11u, 22u, 33u, 44u };
				const std::vector<std::uint8_t> fileBytes{ 99u, 88u, 11u, 22u, 33u, 44u };
				const std::filesystem::path sourcePath = std::filesystem::temp_directory_path()
					/ ("TerraEngine-VirtualGeometry-" + TerraEngine::ToHexString(asset) + ".bin");
				{
					std::ofstream source(sourcePath, std::ios::binary | std::ios::trunc);
					// The prefix proves this request reads its authored byte range rather
					// than treating the complete source file as one physical page.
					source.write(reinterpret_cast<const char*>(fileBytes.data()),
						static_cast<std::streamsize>(fileBytes.size()));
				}

				system.SubmitPriorFrameFeedback({ MakeExactRangeFeedback(asset, sourcePath, sourceBytes) });
				auto workloads = system.DeclareFrameWorkloads(0.0f);
				TerraEngine::Scheduling::JobGraphContext graphContext;
				workloads.front().work(graphContext);
				const auto prepared = system.LastPreparedPlan();
				context.ExpectEqual(static_cast<std::size_t>(1u), prepared.decodedPages.size(),
					"The requested physical page should perform one exact-range worker read.");
				context.Expect(prepared.decodedPages.front().status == Resource::VirtualGeometryStreamingDecodeStatus::PendingUpload,
					"Verified identity-decoded bytes become a typed pending upload, not a resident page.");
				context.ExpectEqual(sourceBytes.size(), prepared.decodedPages.front().decodedBytes.size(),
					"The worker plan must retain only the verified exact byte range.");
				context.Expect(prepared.decodedPages.front().decodedBytes == sourceBytes,
					"The worker must exclude unrelated source bytes before it creates the upload intent.");
				context.Expect(system.ResidencySnapshot().pages.empty(),
					"Worker IO must not expose residency before the protected publication boundary.");
				workloads.back().work(graphContext);
				const auto pendingSnapshot = system.ResidencySnapshot();
				context.ExpectEqual(static_cast<std::size_t>(1u), pendingSnapshot.pages.size(),
					"The protected boundary publishes one immutable pending upload row.");
				context.Expect(pendingSnapshot.pages.front().state == Resource::VirtualGeometryPagePublicationState::PendingUpload,
					"CPU IO/decode success must not claim that the backend upload completed.");
				context.Expect(system.PublishedPages().empty(),
					"Pending uploads remain unavailable to the renderer page publication snapshot.");

				// The next frame supplies an externally verified descriptor/upload candidate.
				system.SubmitCompletedPublicationCandidates({ MakeCompletedCandidate(asset, 2u) });
				workloads = system.DeclareFrameWorkloads(0.0f);
				workloads.front().work(graphContext);
				workloads.back().work(graphContext);
				const auto residentSnapshot = system.ResidencySnapshot();
				context.ExpectEqual(static_cast<std::size_t>(1u), system.PublishedPages().size(),
					"Only a later verified upload candidate can publish the renderer-visible page.");
				context.Expect(residentSnapshot.pages.front().state == Resource::VirtualGeometryPagePublicationState::Resident,
					"The next protected boundary atomically replaces pending upload with resident state.");

				// Cancellation after a good publication is intentionally a no-op for the
				// visible row, preserving last-known-good geometry for the renderer.
				auto cancellation = std::make_shared<std::atomic_bool>(true);
				system.SubmitPriorFrameFeedback({ MakeExactRangeFeedback(asset, sourcePath, sourceBytes, cancellation) });
				workloads = system.DeclareFrameWorkloads(0.0f);
				workloads.front().work(graphContext);
				workloads.back().work(graphContext);
				const auto afterCancellation = system.ResidencySnapshot();
				context.Expect(afterCancellation.pages.front().state == Resource::VirtualGeometryPagePublicationState::Resident,
					"Cancelled replacement IO must leave the last-known-good resident row intact.");
				context.ExpectEqual(1u, system.DiagnosticsSnapshot().cancelledReads,
					"Diagnostics must distinguish cancelled reads from completed upload intents.");

				std::error_code cleanupError;
				std::filesystem::remove(sourcePath, cleanupError);
				system.Shutdown();
			});

		registry.Add("VirtualGeometry.Streaming.CanonicalDecodedRangePublishesToPendingUpload",
			[](TestContext& context)
			{
				auto& system = Resource::VirtualGeometryStreamingSystem::Instance();
				system.Initialize();
				const auto asset = TerraEngine::GenerateUUID();
				const std::vector<std::uint8_t> expectedDecoded{ 7u, 7u, 9u, 11u };
				const std::vector<std::uint8_t> productionPayload = expectedDecoded;
				const std::filesystem::path sourcePath = std::filesystem::temp_directory_path()
					/ ("TerraEngine-TEMFPRD1-" + TerraEngine::ToHexString(asset) + ".bin");
				{
					std::ofstream source(sourcePath, std::ios::binary | std::ios::trunc);
					source.write(reinterpret_cast<const char*>(productionPayload.data()),
						static_cast<std::streamsize>(productionPayload.size()));
				}

				Resource::VirtualGeometryStreamingFeedback feedback;
				feedback.assetId = asset;
				feedback.pageId = 2u;
				feedback.fallbackPageId = 2u;
				feedback.priority = 8;
				feedback.observedFrame = 1u;
				feedback.visible = true;
				feedback.expectedStoredByteCount = productionPayload.size();
				feedback.expectedDecodedByteCount = expectedDecoded.size();
				feedback.expectedIntegrityValue = CalculateVirtualGeometryCompactHash(productionPayload);
				TemfBlockDescriptor descriptor{};
				descriptor.blockId = 1u;
				descriptor.logicalPageId = feedback.pageId;
				descriptor.streamKind = static_cast<std::uint16_t>(TemfStreamKind::Metadata);
				descriptor.codecProfileId = static_cast<std::uint32_t>(TemfCodecProfile::ExpandedProduction);
				descriptor.codecVersion = 1u;
				descriptor.encodedBytes = productionPayload.size();
				descriptor.decodedBytes = productionPayload.size();
				descriptor.decodedElementCount = static_cast<std::uint32_t>(productionPayload.size());
				descriptor.decodedStride = 1u;
				descriptor.requiredAlignment = 1u;
				descriptor.encodedIntegrity = feedback.expectedIntegrityValue;
				descriptor.decodedIntegrity = feedback.expectedIntegrityValue;
				feedback.exactBlockReads.push_back({
					{ sourcePath, feedback.pageId, 0u, productionPayload.size(),
						descriptor.encodedIntegrity, 0u, nullptr },
					descriptor
				});
				system.SubmitPriorFrameFeedback({ feedback });
				auto workloads = system.DeclareFrameWorkloads(0.0f);
				TerraEngine::Scheduling::JobGraphContext graphContext;
				workloads.front().work(graphContext);
				const auto prepared = system.LastPreparedPlan();
				context.ExpectEqual(static_cast<std::size_t>(1u), prepared.decodedPages.size(),
					"A canonical production page should prepare exactly one upload result.");
				context.Expect(prepared.decodedPages.front().status == Resource::VirtualGeometryStreamingDecodeStatus::PendingUpload,
					"A verified canonical decoded range must advance directly to PendingUpload.");
				context.Expect(prepared.decodedPages.front().decodedBytes == expectedDecoded,
					"The pending upload must contain the validated canonical page bytes.");
				workloads.back().work(graphContext);
				context.Expect(system.ResidencySnapshot().pages.front().state == Resource::VirtualGeometryPagePublicationState::PendingUpload,
					"The protected boundary must publish canonical CPU work only as PendingUpload.");
				std::error_code cleanupError;
				std::filesystem::remove(sourcePath, cleanupError);
				system.Shutdown();
			});

		registry.Add("VirtualGeometry.Streaming.MultiBlockPageDecodesAndPublishesOneConcatenatedUpload",
			[](TestContext& context)
			{
				auto& system = Resource::VirtualGeometryStreamingSystem::Instance();
				system.Initialize();
				const auto asset = TerraEngine::GenerateUUID();
				const std::vector<std::uint8_t> firstBlock{ 10u, 20u };
				const std::vector<std::uint8_t> secondBlock{ 30u, 40u, 50u };
				const std::vector<std::uint8_t> pageBytes{ 10u, 20u, 30u, 40u, 50u };
				const std::vector<std::uint8_t> fileBytes{ 99u, 10u, 20u, 30u, 40u, 50u };
				const std::filesystem::path sourcePath = std::filesystem::temp_directory_path()
					/ ("TerraEngine-TEMFPRD1-MultiBlock-" + TerraEngine::ToHexString(asset) + ".bin");
				{
					std::ofstream source(sourcePath, std::ios::binary | std::ios::trunc);
					source.write(reinterpret_cast<const char*>(fileBytes.data()),
						static_cast<std::streamsize>(fileBytes.size()));
				}

				Resource::VirtualGeometryStreamingFeedback feedback{};
				feedback.assetId = asset;
				feedback.pageId = 13u;
				feedback.fallbackPageId = 13u;
				feedback.priority = 9;
				feedback.observedFrame = 1u;
				feedback.visible = true;
				feedback.expectedStoredByteCount = pageBytes.size();
				feedback.expectedDecodedByteCount = pageBytes.size();
				feedback.expectedIntegrityValue = CalculateVirtualGeometryCompactHash(pageBytes);
				const TemfBlockDescriptor first = MakeIdentityProductionBlock(
					feedback.pageId, 1u, 1u, firstBlock);
				const TemfBlockDescriptor second = MakeIdentityProductionBlock(
					feedback.pageId, 2u, 3u, secondBlock);
				feedback.exactBlockReads = {
					{ { sourcePath, feedback.pageId, 1u, firstBlock.size(), first.encodedIntegrity, 0u, nullptr }, first },
					{ { sourcePath, feedback.pageId, 3u, secondBlock.size(), second.encodedIntegrity, 0u, nullptr }, second }
				};

				system.SubmitPriorFrameFeedback({ feedback });
				auto workloads = system.DeclareFrameWorkloads(0.0f);
				TerraEngine::Scheduling::JobGraphContext graphContext;
				workloads.front().work(graphContext);
				const auto prepared = system.LastPreparedPlan();
				context.ExpectEqual(static_cast<std::size_t>(2u), prepared.requests.front().exactBlockReads.size(),
					"The prepared request must preserve every ordered canonical block descriptor.");
				context.ExpectEqual(static_cast<std::size_t>(1u), prepared.decodedPages.size(),
					"All blocks for one logical page must produce one decoded-page result.");
				context.Expect(prepared.decodedPages.front().status == Resource::VirtualGeometryStreamingDecodeStatus::PendingUpload,
					"A complete multi-block page becomes one pending upload only after every block validates.");
				context.Expect(prepared.decodedPages.front().decodedBytes == pageBytes,
					"The pending upload must concatenate decoded blocks in authored page order.");
				context.ExpectEqual(2u, system.DiagnosticsSnapshot().completedReads,
					"Completed physical read diagnostics must count each validated block range.");
				workloads.back().work(graphContext);
				context.ExpectEqual(static_cast<std::size_t>(1u), system.ResidencySnapshot().pages.size(),
					"One logical multi-block page publishes exactly one protected pending-upload row.");

				std::error_code cleanupError;
				std::filesystem::remove(sourcePath, cleanupError);
				system.Shutdown();
			});

		registry.Add("VirtualGeometry.Streaming.CorruptLaterBlockDoesNotPublishPartialPage",
			[](TestContext& context)
			{
				auto& system = Resource::VirtualGeometryStreamingSystem::Instance();
				system.Initialize();
				const auto asset = TerraEngine::GenerateUUID();
				const std::vector<std::uint8_t> firstBlock{ 1u, 2u };
				const std::vector<std::uint8_t> secondBlock{ 3u, 4u };
				const std::vector<std::uint8_t> pageBytes{ 1u, 2u, 3u, 4u };
				const std::filesystem::path sourcePath = std::filesystem::temp_directory_path()
					/ ("TerraEngine-TEMFPRD1-CorruptLater-" + TerraEngine::ToHexString(asset) + ".bin");
				{
					std::ofstream source(sourcePath, std::ios::binary | std::ios::trunc);
					source.write(reinterpret_cast<const char*>(pageBytes.data()),
						static_cast<std::streamsize>(pageBytes.size()));
				}

				Resource::VirtualGeometryStreamingFeedback feedback{};
				feedback.assetId = asset;
				feedback.pageId = 14u;
				feedback.fallbackPageId = 14u;
				feedback.priority = 9;
				feedback.observedFrame = 1u;
				feedback.visible = true;
				feedback.expectedStoredByteCount = pageBytes.size();
				feedback.expectedDecodedByteCount = pageBytes.size();
				feedback.expectedIntegrityValue = CalculateVirtualGeometryCompactHash(pageBytes);
				const TemfBlockDescriptor first = MakeIdentityProductionBlock(
					feedback.pageId, 1u, 0u, firstBlock);
				TemfBlockDescriptor corruptSecond = MakeIdentityProductionBlock(
					feedback.pageId, 2u, 2u, secondBlock);
				// The physical range is valid, but the later block's decoded proof is
				// intentionally false. This proves the first decoded prefix is discarded.
				corruptSecond.decodedIntegrity ^= 1u;
				feedback.exactBlockReads = {
					{ { sourcePath, feedback.pageId, 0u, firstBlock.size(), first.encodedIntegrity, 0u, nullptr }, first },
					{ { sourcePath, feedback.pageId, 2u, secondBlock.size(), corruptSecond.encodedIntegrity, 0u, nullptr }, corruptSecond }
				};

				system.SubmitPriorFrameFeedback({ feedback });
				auto workloads = system.DeclareFrameWorkloads(0.0f);
				TerraEngine::Scheduling::JobGraphContext graphContext;
				workloads.front().work(graphContext);
				const auto prepared = system.LastPreparedPlan();
				context.Expect(prepared.decodedPages.front().status == Resource::VirtualGeometryStreamingDecodeStatus::Failed,
					"A corrupt later block must fail the aggregate page without accepting an upload.");
				context.Expect(prepared.decodedPages.front().decodedBytes.empty(),
					"A corrupt later block must discard all earlier decoded bytes atomically.");
				context.ExpectEqual(2u, system.DiagnosticsSnapshot().completedReads,
					"Both physical reads completed before the later decode proof rejected the page.");
				workloads.back().work(graphContext);
				context.Expect(system.PublishedPages().empty(),
					"No renderer-visible page may publish when any later canonical block is corrupt.");

				std::error_code cleanupError;
				std::filesystem::remove(sourcePath, cleanupError);
				system.Shutdown();
			});

		registry.Add("VirtualGeometry.Streaming.CancelledLaterBlockDoesNotPublishPartialPage",
			[](TestContext& context)
			{
				auto& system = Resource::VirtualGeometryStreamingSystem::Instance();
				system.Initialize();
				const auto asset = TerraEngine::GenerateUUID();
				const std::vector<std::uint8_t> firstBlock{ 5u, 6u };
				const std::vector<std::uint8_t> secondBlock{ 7u, 8u };
				const std::vector<std::uint8_t> pageBytes{ 5u, 6u, 7u, 8u };
				const std::filesystem::path sourcePath = std::filesystem::temp_directory_path()
					/ ("TerraEngine-TEMFPRD1-CancelLater-" + TerraEngine::ToHexString(asset) + ".bin");
				{
					std::ofstream source(sourcePath, std::ios::binary | std::ios::trunc);
					source.write(reinterpret_cast<const char*>(pageBytes.data()),
						static_cast<std::streamsize>(pageBytes.size()));
				}

				Resource::VirtualGeometryStreamingFeedback feedback{};
				feedback.assetId = asset;
				feedback.pageId = 15u;
				feedback.fallbackPageId = 15u;
				feedback.priority = 9;
				feedback.observedFrame = 1u;
				feedback.visible = true;
				feedback.expectedStoredByteCount = pageBytes.size();
				feedback.expectedDecodedByteCount = pageBytes.size();
				feedback.expectedIntegrityValue = CalculateVirtualGeometryCompactHash(pageBytes);
				const TemfBlockDescriptor first = MakeIdentityProductionBlock(
					feedback.pageId, 1u, 0u, firstBlock);
				const TemfBlockDescriptor second = MakeIdentityProductionBlock(
					feedback.pageId, 2u, 2u, secondBlock);
				const auto cancelled = std::make_shared<std::atomic_bool>(true);
				feedback.exactBlockReads = {
					{ { sourcePath, feedback.pageId, 0u, firstBlock.size(), first.encodedIntegrity, 0u, nullptr }, first },
					{ { sourcePath, feedback.pageId, 2u, secondBlock.size(), second.encodedIntegrity, 0u, cancelled }, second }
				};

				system.SubmitPriorFrameFeedback({ feedback });
				auto workloads = system.DeclareFrameWorkloads(0.0f);
				TerraEngine::Scheduling::JobGraphContext graphContext;
				workloads.front().work(graphContext);
				const auto prepared = system.LastPreparedPlan();
				context.Expect(prepared.decodedPages.front().status == Resource::VirtualGeometryStreamingDecodeStatus::Cancelled,
					"Cancelling a later physical block must cancel the aggregate logical page.");
				context.Expect(prepared.decodedPages.front().decodedBytes.empty(),
					"Cancelled later IO must discard the completed earlier decoded prefix.");
				const auto diagnostics = system.DiagnosticsSnapshot();
				context.ExpectEqual(1u, diagnostics.completedReads,
					"Completed physical reads remain truthful when a later block is cancelled.");
				context.ExpectEqual(1u, diagnostics.cancelledReads,
					"Cancelled physical reads remain distinct from completed reads.");
				workloads.back().work(graphContext);
				context.Expect(system.PublishedPages().empty(),
					"Cancelled later IO must not publish a partial renderer-visible page.");

				std::error_code cleanupError;
				std::filesystem::remove(sourcePath, cleanupError);
				system.Shutdown();
			});
	}
} // namespace TerraEngine::Tests
