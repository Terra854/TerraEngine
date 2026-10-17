// Protected prior-frame feedback resolves copied canonical page metadata before the next streaming phase.
#include "RenderFrameState.h"
#include "Resource/ResourceManager.h"
#include "Resource/VirtualGeometryStreamingSystem.h"
#include "File Formats/TerraEngineMeshFile.h"
#include "Profiling/ProfilerScope.h"
#include <set>
#include <cstring>
void RenderSystem::PublishFrameStreamingFeedback()
{
	auto& resourceManager = ResourceManager::Instance();
	// Submit copied observations after this frame's immutable snapshot is built.
	// StreamingPrepare runs before the next render update, so these values are
	// naturally prior-frame feedback and cannot make a page resident mid-frame.
	TERRAENGINE_PROFILE_SCOPE("RenderSystem: VG Streaming Feedback and Cleanup",
		TerraEngine::Profiling::ProfilerModuleId::CpuEngine);
	std::size_t streamingFeedbackCount = 0U;
	for(const auto& view : latestRenderFrameSnapshot.virtualGeometryViewSelections) {
		streamingFeedbackCount += view.PageRequests().size();
	}
	std::vector<TerraEngine::Resource::VirtualGeometryStreamingFeedback> streamingFeedback;
	streamingFeedback.reserve(streamingFeedbackCount);
	std::set<std::pair<TerraEngine::UUID, std::uint32_t>> pagesWithCanonicalMetadata;
	for(const auto& view : latestRenderFrameSnapshot.virtualGeometryViewSelections) {
		for(const auto& request : view.PageRequests()) {
			TerraEngine::Resource::VirtualGeometryStreamingFeedback feedback{};
			feedback.assetId = request.meshId; feedback.pageId = request.pageId;
			feedback.fallbackPageId = request.fallbackPageId; feedback.clusterId = request.clusterId;
			feedback.priority = request.priority; feedback.observedFrame = activeRenderFrameId;
			// Build immutable source metadata only for the first occurrence of an exact
			// mesh/page key. StreamingPrepare already coalesces duplicate view/instance
			// feedback while retaining maximum priority and every fallback dependency;
			// repeating the page-table scan for a shared crowd would add no information.
			const bool resolveCanonicalPage = pagesWithCanonicalMetadata.emplace(
				request.meshId, request.pageId).second;
			if(resolveCanonicalPage) {
				feedback.canonicalPageReadDiagnostic =
					"No exact compact page range was resolved from the current mesh snapshot.";
			}
			// Resolve every canonical TEMFPRD1 block for this unique page while
			// RenderSystem owns the main-thread ResourceManager snapshot. Worker
			// preparation receives copied values only and never inspects live mesh state.
			const Mesh* mesh = resolveCanonicalPage
				? resourceManager.GetMesh(request.meshId) : nullptr;
			if(mesh != nullptr)
			{
				if(mesh->sourcePath.empty())
				{
					feedback.canonicalPageReadDiagnostic =
						"The validated TEMFPRD1 directory has no immutable cooked source path.";
				}
				else if(mesh->storage.size() < sizeof(TemfProductionHeader))
				{
					feedback.canonicalPageReadDiagnostic =
						"The loaded mesh does not retain a canonical TEMFPRD1 production header.";
				}
				else
				{
					TemfProductionHeader header{};
					std::memcpy(&header, mesh->storage.data(), sizeof(header));
					const auto rangeFitsStorage = [&storage = mesh->storage](std::uint64_t offset,
						std::uint64_t count, std::size_t stride) {
						return offset <= storage.size()
							&& count <= (storage.size() - static_cast<std::size_t>(offset)) / stride;
					};
					if(header.formatContractId != kTemfProductionContractId
						|| header.headerBytes != kTemfProductionHeaderBytes)
					{
						feedback.canonicalPageReadDiagnostic = "The mesh header is not canonical TEMFPRD1 data; recook-required.";
					}
					else if(!rangeFitsStorage(header.pageTableOffset, header.pageCount, sizeof(TemfPageDescriptor))
						|| !rangeFitsStorage(header.blockTableOffset, header.blockCount, sizeof(TemfBlockDescriptor))
						|| !rangeFitsStorage(header.quantizationTableOffset, header.quantizationDomainCount, sizeof(TemfQuantizationDomain)))
					{
						feedback.canonicalPageReadDiagnostic = "The canonical TEMFPRD1 directory range is unavailable in retained mesh storage.";
					}
					else
					{
						TemfPageDescriptor page{};
						bool foundPage = false;
						for(std::uint32_t index = 0u; index < header.pageCount; ++index)
						{
							std::memcpy(&page, mesh->storage.data() + header.pageTableOffset
								+ static_cast<std::uint64_t>(index) * sizeof(page), sizeof(page));
							if(page.logicalPageId == request.pageId) { foundPage = true; break; }
						}
						if(!foundPage)
						{
							feedback.canonicalPageReadDiagnostic = "The requested page ID is absent from the canonical TEMFPRD1 page table.";
						}
						else if(page.blockCount == 0u || page.firstBlockIndex > header.blockCount
							|| page.blockCount > header.blockCount - page.firstBlockIndex
							|| page.attributedStoredBytes == 0u || page.decodedBytes == 0u || page.pageIntegrity == 0u)
						{
							feedback.canonicalPageReadDiagnostic = "The requested TEMFPRD1 page has invalid block bounds, totals, or aggregate integrity.";
						}
						else
						{
							std::uint64_t storedTotal = 0u;
							std::uint64_t decodedTotal = 0u;
							std::uint64_t expectedNextPayloadOffset = 0u;
							bool pageRangesValid = true;
							feedback.exactBlockReads.reserve(page.blockCount);
							for(std::uint32_t blockOffset = 0u; blockOffset < page.blockCount; ++blockOffset)
							{
								TemfBlockDescriptor block{};
								const std::uint64_t blockIndex = static_cast<std::uint64_t>(page.firstBlockIndex) + blockOffset;
								std::memcpy(&block, mesh->storage.data() + header.blockTableOffset
									+ blockIndex * sizeof(block), sizeof(block));
								const bool totalsOverflow = block.encodedBytes > std::numeric_limits<std::uint64_t>::max() - storedTotal
									|| block.decodedBytes > std::numeric_limits<std::uint64_t>::max() - decodedTotal;
								const bool payloadOffsetOverflow = block.encodedBytes > std::numeric_limits<std::uint64_t>::max()
									- block.encodedPayloadOffset;
								const bool contiguousPayload = blockOffset == 0u
									|| block.encodedPayloadOffset == expectedNextPayloadOffset;
								if(block.logicalPageId != page.logicalPageId || block.blockOrdinal != blockOffset
									|| block.encodedBytes == 0u
									|| block.decodedBytes == 0u || totalsOverflow || payloadOffsetOverflow
									|| !contiguousPayload
									|| !rangeFitsStorage(block.encodedPayloadOffset, block.encodedBytes, 1u))
								{
									pageRangesValid = false;
									break;
								}
								storedTotal += block.encodedBytes;
								decodedTotal += block.decodedBytes;
								expectedNextPayloadOffset = block.encodedPayloadOffset + block.encodedBytes;
								TerraEngine::Resource::VirtualGeometryStaticPageReadRequest pageRead{};
								pageRead.path = mesh->sourcePath;
								pageRead.pageId = request.pageId;
								pageRead.byteOffset = block.encodedPayloadOffset;
								pageRead.byteSize = block.encodedBytes;
								pageRead.expectedHash = block.encodedIntegrity;
								pageRead.mountedLogicalPath = mesh->sourceLogicalPath;
								pageRead.mountedContentRequired =
									resourceManager.IsExportRuntime();
								feedback.exactBlockReads.push_back({ std::move(pageRead), block });
							}
							if(!pageRangesValid || storedTotal != page.attributedStoredBytes
								|| decodedTotal != page.decodedBytes)
							{
								feedback.exactBlockReads.clear();
								feedback.canonicalPageReadDiagnostic = "The TEMFPRD1 page block ranges, ordering, or byte totals are inconsistent.";
							}
							else
							{
								feedback.expectedStoredByteCount = page.attributedStoredBytes;
								feedback.expectedDecodedByteCount = page.decodedBytes;
								feedback.expectedIntegrityValue = page.pageIntegrity;
								feedback.quantizationDomains.resize(header.quantizationDomainCount);
								if(!feedback.quantizationDomains.empty())
								{
									std::memcpy(feedback.quantizationDomains.data(), mesh->storage.data()
										+ header.quantizationTableOffset,
										feedback.quantizationDomains.size() * sizeof(TemfQuantizationDomain));
								}
								// This is a CPU decoder capability copied to the worker. It is not
								// GPU execution evidence and permits the bundled Meshoptimizer path.
								feedback.availableDecoderCapabilities = kVirtualGeometryCodecCapabilityMeshoptimizer;
								feedback.canonicalPageReadDiagnostic = "Resolved ordered exact TEMFPRD1 block ranges and aggregate page integrity.";
							}
						}
					}
				}
			}
			else if(resolveCanonicalPage)
			{
				feedback.canonicalPageReadDiagnostic =
					"The requested mesh ID is not present in ResourceManager.";
			}
			if(request.fallbackPageId != request.pageId && request.fallbackPageId != 0xffffffffu)
				feedback.dependencyPageIds.push_back(request.fallbackPageId);
			streamingFeedback.push_back(std::move(feedback));
		}
	}
	TerraEngine::Resource::VirtualGeometryStreamingSystem::Instance()
		.SubmitPriorFrameFeedback(std::move(streamingFeedback));

	ResourceManager::Instance().RunResidencyCleanup(activeRenderFrameId);
	ProcessQueuedCaptureRequests();
	DrainCaptureReadbacks();

	ClearOverrideCameras();
}
