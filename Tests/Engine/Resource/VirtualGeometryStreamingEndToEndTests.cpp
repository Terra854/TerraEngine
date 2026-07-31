// VirtualGeometryStreamingEndToEndTests.cpp - worker preparation to protected publication.

#include "Core/Scheduling/JobGraph.h"
#include "Resource/ResourceStreaming.h"
#include "Resource/VirtualGeometryCodec.h"
#include "Resource/VirtualGeometryStreamingSystem.h"
#include "TestHarness.h"

#include <atomic>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <memory>

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
			feedback.expectedIntegrityValue = Resource::HashAnimatedVirtualPageBytes(sourceBytes);
			feedback.staticPageRead = Resource::VirtualGeometryStaticPageReadRequest{
				sourcePath,
				feedback.pageId,
				2u,
				sourceBytes.size(),
				feedback.expectedIntegrityValue,
				0u,
				std::move(cancellation)
			};
			// These test bytes are a deliberately identity-decoded page payload. A
			// compressed format must leave this false until its decoder completes.
			feedback.rawPayloadIsDecoded = true;
			return feedback;
		}

		std::vector<std::uint8_t> MakeTerraCompactBlock(
			const std::vector<std::uint8_t>& decodedBytes)
		{
			// The Terra literal control stores one to 128 bytes. This small fixture
			// exercises the production decoder without treating encoded bytes as raw.
			std::vector<std::uint8_t> payload;
			payload.push_back(static_cast<std::uint8_t>(decodedBytes.size() - 1u));
			payload.insert(payload.end(), decodedBytes.begin(), decodedBytes.end());
			const VirtualGeometryCompactBlockHeader header{
				kVirtualGeometryCompactBlockMagicValue,
				kVirtualGeometryCompactBlockVersion,
				static_cast<std::uint16_t>(sizeof(VirtualGeometryCompactBlockHeader)),
				static_cast<std::uint32_t>(VirtualGeometryCompactCodec::Terra),
				0u,
				static_cast<std::uint32_t>(decodedBytes.size()),
				static_cast<std::uint32_t>(payload.size()),
				1u,
				static_cast<std::uint32_t>(decodedBytes.size()),
				kVirtualGeometryCodecCapabilityTerra,
				CalculateVirtualGeometryCompactHash(decodedBytes) };
			std::vector<std::uint8_t> block(sizeof(header) + payload.size());
			std::memcpy(block.data(), &header, sizeof(header));
			std::memcpy(block.data() + sizeof(header), payload.data(), payload.size());
			return block;
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

		registry.Add("VirtualGeometry.Streaming.TerraCompactRangeDecodesToPendingUpload",
			[](TestContext& context)
			{
				auto& system = Resource::VirtualGeometryStreamingSystem::Instance();
				system.Initialize();
				const auto asset = TerraEngine::GenerateUUID();
				const std::vector<std::uint8_t> expectedDecoded{ 7u, 7u, 9u, 11u };
				const std::vector<std::uint8_t> compactBlock = MakeTerraCompactBlock(expectedDecoded);
				const std::filesystem::path sourcePath = std::filesystem::temp_directory_path()
					/ ("TerraEngine-VGXD-" + TerraEngine::ToHexString(asset) + ".bin");
				{
					std::ofstream source(sourcePath, std::ios::binary | std::ios::trunc);
					source.write(reinterpret_cast<const char*>(compactBlock.data()),
						static_cast<std::streamsize>(compactBlock.size()));
				}

				Resource::VirtualGeometryStreamingFeedback feedback;
				feedback.assetId = asset;
				feedback.pageId = 2u;
				feedback.fallbackPageId = 2u;
				feedback.priority = 8;
				feedback.observedFrame = 1u;
				feedback.visible = true;
				feedback.expectedStoredByteCount = compactBlock.size();
				feedback.expectedDecodedByteCount = expectedDecoded.size();
				feedback.expectedIntegrityValue = Resource::HashAnimatedVirtualPageBytes(compactBlock);
				feedback.staticPageRead = { sourcePath, feedback.pageId, 0u,
					compactBlock.size(), feedback.expectedIntegrityValue, 0u, nullptr };
				// The compact block must take the decoder path rather than the legacy
				// identity-upload escape hatch.
				feedback.rawPayloadIsDecoded = false;
				system.SubmitPriorFrameFeedback({ feedback });
				auto workloads = system.DeclareFrameWorkloads(0.0f);
				TerraEngine::Scheduling::JobGraphContext graphContext;
				workloads.front().work(graphContext);
				const auto prepared = system.LastPreparedPlan();
				context.ExpectEqual(static_cast<std::size_t>(1u), prepared.decodedPages.size(),
					"A compact physical page should prepare exactly one decode result.");
				context.Expect(prepared.decodedPages.front().status == Resource::VirtualGeometryStreamingDecodeStatus::PendingUpload,
					"A verified Terra compact block must advance directly to PendingUpload.");
				context.Expect(prepared.decodedPages.front().decodedBytes == expectedDecoded,
					"The pending upload must contain decoded page bytes, not the VGCB envelope.");
				workloads.back().work(graphContext);
				context.Expect(system.ResidencySnapshot().pages.front().state == Resource::VirtualGeometryPagePublicationState::PendingUpload,
					"The protected boundary must publish decoded compact CPU work only as PendingUpload.");
				std::error_code cleanupError;
				std::filesystem::remove(sourcePath, cleanupError);
				system.Shutdown();
			});
	}
} // namespace TerraEngine::Tests
