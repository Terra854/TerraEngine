// VirtualGeometryStreamingSystem.cpp - worker planning and protected page publication.

#include "VirtualGeometryStreamingSystem.h"

#include "Core/Scheduling/SystemScheduler.h"
#include "Resource/VirtualGeometryCodec.h"

#include <algorithm>
#include <array>
#include <iterator>
#include <limits>
#include <utility>

namespace TerraEngine::Resource
{
	namespace
	{
		// Checked totals keep malformed directories from wrapping a page size before
		// the worker reserves or concatenates any file-controlled byte range.
		bool AddPageByteCount(std::uint64_t& total, std::uint64_t value) noexcept
		{
			if (value > std::numeric_limits<std::uint64_t>::max() - total)
			{
				return false;
			}
			total += value;
			return true;
		}

		bool FitsProcessVector(std::uint64_t byteCount) noexcept
		{
			return byteCount <= static_cast<std::uint64_t>(std::numeric_limits<std::size_t>::max());
		}
	} // namespace

	VirtualGeometryStreamingSystem& VirtualGeometryStreamingSystem::Instance()
	{
		// The engine retains singleton lifetime ownership while SystemScheduler owns
		// only per-frame descriptors and their explicit publication dependency.
		static VirtualGeometryStreamingSystem instance;
		return instance;
	}

	void VirtualGeometryStreamingSystem::Initialize()
	{
		// A new session must not retain candidates or visibility from an earlier
		// engine lifetime. Clearing occurs before the system becomes schedulable.
		preparedFrameCount.store(0u, std::memory_order_release);
		publishedFrameCount.store(0u, std::memory_order_release);
		{
			std::scoped_lock lock(candidateMutex);
			priorFrameFeedback.clear();
			pendingPublicationCandidates.clear();
			preparedPublicationCandidates.clear();
			lastPreparedPlan = {};
			publicationStore.Clear();
			residencyStore.Clear();
			diagnostics = {};
		}
		initialized.store(true, std::memory_order_release);
	}

	void VirtualGeometryStreamingSystem::Update(float)
	{
		// Converted systems execute only through DeclareFrameWorkloads(). Keeping
		// Update inert prevents a second path from bypassing PagePublish.
	}

	void VirtualGeometryStreamingSystem::Shutdown()
	{
		initialized.store(false, std::memory_order_release);
		std::scoped_lock lock(candidateMutex);
		priorFrameFeedback.clear();
		pendingPublicationCandidates.clear();
		preparedPublicationCandidates.clear();
		lastPreparedPlan = {};
		publicationStore.Clear();
		residencyStore.Clear();
	}

	void VirtualGeometryStreamingSystem::SubmitPriorFrameFeedback(
		std::vector<VirtualGeometryStreamingFeedback> feedback)
	{
		std::scoped_lock lock(candidateMutex);
		// Replacing, rather than appending, gives one preparation workload exact
		// ownership of its feedback generation and prevents stale request reuse.
		priorFrameFeedback = std::move(feedback);
	}

	void VirtualGeometryStreamingSystem::SubmitCompletedPublicationCandidates(
		std::vector<VirtualGeometryPagePublicationCandidate> candidates)
	{
		std::scoped_lock lock(candidateMutex);
		// Candidate values may arrive from an asynchronous decode/upload owner, but
		// no residency changes here. PagePublish is the only visible mutation edge.
		pendingPublicationCandidates.insert(
			pendingPublicationCandidates.end(),
			std::make_move_iterator(candidates.begin()),
			std::make_move_iterator(candidates.end()));
		diagnostics.submittedPublicationCandidates += candidates.size();
	}

	VirtualGeometryStreamingPlan VirtualGeometryStreamingSystem::LastPreparedPlan() const
	{
		std::scoped_lock lock(candidateMutex);
		return lastPreparedPlan;
	}

	std::vector<VirtualGeometryPagePublication> VirtualGeometryStreamingSystem::PublishedPages() const
	{
		std::scoped_lock lock(candidateMutex);
		return publicationStore.Snapshot();
	}

	VirtualGeometryPhysicalPageResidencySnapshot VirtualGeometryStreamingSystem::ResidencySnapshot() const
	{
		std::scoped_lock lock(candidateMutex);
		return residencyStore.Snapshot();
	}

	VirtualGeometryStreamingDiagnostics VirtualGeometryStreamingSystem::DiagnosticsSnapshot() const
	{
		std::scoped_lock lock(candidateMutex);
		return diagnostics;
	}

	std::string VirtualGeometryStreamingSystem::SystemName()
	{
		return "VirtualGeometryStreamingSystem";
	}

	TerraEngine::Scheduling::SystemDeclaration VirtualGeometryStreamingSystem::SchedulerDeclaration()
	{
		using namespace TerraEngine::Scheduling;
		SystemDeclaration declaration;
		declaration.name = SystemName();
		declaration.phase = "Render";
		declaration.before = { "RenderSystem" };
		declaration.reads = {
			kResourceVirtualGeometryPageRequests,
			kResourceVirtualGeometryPageNeedPackets,
			kResourceVirtualGeometryPageResidencySnapshots,
			kResourceVirtualGeometryPagePublications
		};
		declaration.writes = {
			kResourceVirtualGeometryPageNeedPackets,
			kResourceVirtualGeometryPagePublications,
			kResourceVirtualGeometryPageRetirements,
			kResourceVirtualGeometryPageDiagnostics
		};
		declaration.mainThread = false;
		declaration.phaseKind = WorkerSystemPhaseKind::WorkerPrepare;
		declaration.boundaryClassification = HardBoundaryClassification::PartiallyWorkerSafe;
		declaration.hardBoundary = true;
		declaration.safeSubwork = "Copies prior-frame feedback, expands dependencies, and forms value-only decode/upload intents and private verified publication batches without ResourceManager or RHI mutation.";
		declaration.protectedCommit = "Validates and atomically swaps one complete page publication batch, including hierarchy fallback coverage, descriptor intent, diagnostics, and retirement intent, before RenderSystem captures the next snapshot.";
		declaration.protectedReason = declaration.protectedCommit;
		declaration.profilingLabel = "VirtualGeometryStreaming";
		declaration.usesDefaultMetadata = false;
		return declaration;
	}

	std::vector<TerraEngine::Scheduling::FrameWorkloadDeclaration>
	VirtualGeometryStreamingSystem::DeclareFrameWorkloads(float)
	{
		using namespace TerraEngine::Scheduling;
		constexpr const char* kOwner = "VirtualGeometryStreamingSystem";
		std::array<FrameWorkloadDeclaration, 2> workloads;

		FrameWorkloadDeclaration& prepare = workloads[0];
		prepare.workloadName = "StreamingPrepare";
		prepare.ownerSystem = kOwner;
		prepare.phase = "Render";
		prepare.stableDescriptorId = "VirtualGeometryStreamingSystem/Render/StreamingPrepare";
		prepare.activationPredicateKey = "virtual-geometry-streaming-lifecycle";
		prepare.reads = {
			kResourceVirtualGeometryPageRequests,
			kResourceVirtualGeometryPageResidencySnapshots,
			kResourceVirtualGeometryPagePublications
		};
		prepare.writes = { kResourceVirtualGeometryPageNeedPackets };
		prepare.workerEligible = true;
		prepare.mainThreadRequired = false;
		prepare.phaseKind = WorkerSystemPhaseKind::WorkerPrepare;
		prepare.boundaryClassification = HardBoundaryClassification::WorkerSafe;
		prepare.safeSubwork = "Copies immutable prior-frame feedback into deterministic dependency-expanded decode/upload intents and stages already verified candidate values privately; it never mutates residency, page storage, RHI state, or ObjectManager.";
		prepare.profilingLabel = "VirtualGeometryStreaming.StreamingPrepare";

		FrameWorkloadDeclaration& publish = workloads[1];
		publish.workloadName = "PagePublish";
		publish.ownerSystem = kOwner;
		publish.phase = "Render";
		publish.stableDescriptorId = "VirtualGeometryStreamingSystem/Render/PagePublish";
		publish.activationPredicateKey = prepare.activationPredicateKey;
		publish.reads = {
			kResourceVirtualGeometryPageNeedPackets,
			kResourceVirtualGeometryPagePublications
		};
		publish.writes = {
			kResourceVirtualGeometryPagePublications,
			kResourceVirtualGeometryPageRetirements,
			kResourceVirtualGeometryPageDiagnostics
		};
		publish.explicitDependencies = { prepare.stableDescriptorId };
		publish.workerEligible = false;
		publish.mainThreadRequired = true;
		publish.mutationPolicy = FrameWorkloadMutationPolicy::None;
		publish.phaseKind = WorkerSystemPhaseKind::ProtectedCommit;
		publish.boundaryClassification = HardBoundaryClassification::Protected;
		publish.hardBoundary = true;
		publish.sameFrameVisibilityRequired = true;
		publish.protectedCommit = "Atomically validates and swaps the complete page-residency candidate set before RenderSystem captures live render inputs; rejected batches retain the prior visible pages.";
		publish.protectedReason = publish.protectedCommit;
		publish.profilingLabel = "VirtualGeometryStreaming.PagePublish";

		const bool active = initialized.load(std::memory_order_acquire);
		for (FrameWorkloadDeclaration& workload : workloads)
		{
			workload.active = active;
			workload.ready = active;
			workload.readinessReason = active ? std::string{} : "virtual geometry streaming system is not initialized";
		}

		prepare.work = [this](JobGraphContext&)
		{
			std::vector<VirtualGeometryStreamingFeedback> feedback;
			std::vector<VirtualGeometryPagePublicationCandidate> candidates;
			{
				std::scoped_lock lock(candidateMutex);
				feedback = priorFrameFeedback;
				candidates = std::move(pendingPublicationCandidates);
				pendingPublicationCandidates.clear();
			}

			// This is the worker-safe boundary: all work uses copied values and cannot
			// expose a page, allocate a descriptor, or write a live resource.
			const std::uint64_t preparedFrame = preparedFrameCount.fetch_add(1u, std::memory_order_acq_rel) + 1u;
			VirtualGeometryStreamingPlan plan = BuildVirtualGeometryStreamingPlan(std::move(feedback));
			for (const VirtualGeometryStreamingDecodeIntent& intent : plan.decodeIntents)
			{
				if (intent.request.exactBlockReads.empty())
				{
					continue;
				}

				VirtualGeometryStreamingDecodedPage decoded;
				decoded.request = intent.request;
				const auto failPage = [&decoded](VirtualGeometryStreamingDecodeStatus status, std::string detail)
				{
					// Do not retain a prefix when any later block rejects the page. The
					// protected boundary must only ever see a whole, verified page value.
					decoded.status = status;
					decoded.decodedBytes.clear();
					decoded.detail = std::move(detail);
				};

				if (intent.request.expectedStoredByteCount == 0u
					|| intent.request.expectedDecodedByteCount == 0u
					|| intent.request.expectedIntegrityValue == 0u
					|| !FitsProcessVector(intent.request.expectedStoredByteCount)
					|| !FitsProcessVector(intent.request.expectedDecodedByteCount))
				{
					failPage(VirtualGeometryStreamingDecodeStatus::Failed,
						"Canonical TEMFPRD1 page totals or aggregate integrity are invalid.");
				}
				else
				{
					std::vector<std::uint8_t> aggregateEncodedBytes;
					std::vector<std::uint8_t> aggregateDecodedBytes;
					aggregateEncodedBytes.reserve(static_cast<std::size_t>(intent.request.expectedStoredByteCount));
					aggregateDecodedBytes.reserve(static_cast<std::size_t>(intent.request.expectedDecodedByteCount));
					std::uint64_t storedTotal = 0u;
					std::uint64_t decodedTotal = 0u;
					std::uint64_t expectedNextPayloadOffset = 0u;
					bool accepted = true;

					for (std::size_t blockOffset = 0u;
						blockOffset < intent.request.exactBlockReads.size(); ++blockOffset)
					{
						const VirtualGeometryStreamingFeedback::ExactBlockReadDescriptor& blockRead =
							intent.request.exactBlockReads[blockOffset];
						const TemfBlockDescriptor& block = blockRead.productionBlock;
						VirtualGeometryStaticPageReadRequest readRequest = blockRead.staticPageRead;
						// Every copied pair must still agree before physical IO. This prevents
						// a malformed producer from swapping a legitimate descriptor onto a
						// different block's exact source range.
						if (block.logicalPageId != intent.request.pageId
							|| block.blockOrdinal != blockOffset
							|| readRequest.pageId != intent.request.pageId
							|| readRequest.byteOffset != block.encodedPayloadOffset
							|| readRequest.byteSize != block.encodedBytes
							|| readRequest.expectedHash != block.encodedIntegrity
							|| block.encodedBytes == 0u || block.decodedBytes == 0u
							|| (blockOffset != 0u && block.encodedPayloadOffset != expectedNextPayloadOffset)
							|| block.encodedBytes > std::numeric_limits<std::uint64_t>::max()
								- block.encodedPayloadOffset
							|| !AddPageByteCount(storedTotal, block.encodedBytes)
							|| !AddPageByteCount(decodedTotal, block.decodedBytes)
							|| storedTotal > intent.request.expectedStoredByteCount
							|| decodedTotal > intent.request.expectedDecodedByteCount)
						{
							failPage(VirtualGeometryStreamingDecodeStatus::Failed,
								"Canonical TEMFPRD1 block ranges disagree with their page totals.");
							accepted = false;
							break;
						}
						expectedNextPayloadOffset = block.encodedPayloadOffset + block.encodedBytes;

						readRequest.requestGeneration = preparedFrame;
						const VirtualGeometryStaticPageReadCompletion read =
							ReadVirtualGeometryStaticPageRange(readRequest, preparedFrame);
						if (read.status == VirtualGeometryStaticPageReadStatus::Cancelled)
						{
							++decoded.cancelledPhysicalReadCount;
							failPage(VirtualGeometryStreamingDecodeStatus::Cancelled, read.diagnostic);
							accepted = false;
							break;
						}
						if (read.status != VirtualGeometryStaticPageReadStatus::Completed)
						{
							failPage(VirtualGeometryStreamingDecodeStatus::Failed, read.diagnostic);
							accepted = false;
							break;
						}
						++decoded.completedPhysicalReadCount;

						const TemfProductionBlockDecodeResult production = DecodeTemfProductionBlock({
							block, read.bytes, intent.request.quantizationDomains,
							intent.request.availableDecoderCapabilities }, {});
						if (!production.IsComplete() || production.bytes.size() != block.decodedBytes)
						{
							failPage(VirtualGeometryStreamingDecodeStatus::Failed,
								production.diagnostic.empty()
									? "Canonical TEMFPRD1 block decoded to a size that disagrees with its descriptor."
									: production.diagnostic);
							accepted = false;
							break;
						}
						aggregateEncodedBytes.insert(aggregateEncodedBytes.end(), read.bytes.begin(), read.bytes.end());
						aggregateDecodedBytes.insert(aggregateDecodedBytes.end(), production.bytes.begin(), production.bytes.end());
					}

					if (accepted && (storedTotal != intent.request.expectedStoredByteCount
						|| decodedTotal != intent.request.expectedDecodedByteCount
						|| aggregateEncodedBytes.size() != storedTotal
						|| aggregateDecodedBytes.size() != decodedTotal
						|| CalculateVirtualGeometryCompactHash(aggregateEncodedBytes)
							!= intent.request.expectedIntegrityValue))
					{
						failPage(VirtualGeometryStreamingDecodeStatus::Failed,
							"Canonical TEMFPRD1 aggregate page totals or encoded integrity are invalid.");
						accepted = false;
					}
					if (accepted)
					{
						decoded.status = VirtualGeometryStreamingDecodeStatus::PendingUpload;
						decoded.decodedBytes = std::move(aggregateDecodedBytes);
						decoded.detail = "All canonical TEMFPRD1 page blocks decoded atomically into one bounded pending GPU upload.";
					}
				}
				plan.decodedPages.push_back(std::move(decoded));
			}
			{
				std::scoped_lock lock(candidateMutex);
				lastPreparedPlan = std::move(plan);
				preparedPublicationCandidates = std::move(candidates);
				diagnostics.submittedRequests += lastPreparedPlan.requests.size();
				diagnostics.expandedDependencyRequests += lastPreparedPlan.expandedDependencyCount;
				diagnostics.preparedDecodeIntents += lastPreparedPlan.decodeIntents.size();
				diagnostics.preparedUploadIntents += lastPreparedPlan.uploadIntents.size();
				for (const VirtualGeometryStreamingDecodedPage& decoded : lastPreparedPlan.decodedPages)
				{
					// These are physical IO facts, not decoded-page success claims. A
					// corrupt/cancelled later block must not erase completed earlier reads.
					diagnostics.completedReads += decoded.completedPhysicalReadCount;
					diagnostics.cancelledReads += decoded.cancelledPhysicalReadCount;
				}
				diagnostics.lastPreparedFrame = preparedFrame;
				RecordVirtualGeometryStreamingEvent(diagnostics, {
					preparedFrame,
					kInvalidVirtualGeometryPageId,
					"prepare",
					"prepared dependency-expanded decode/upload intents and private publication candidates",
					false
				});
			}
		};

		publish.work = [this](JobGraphContext&)
		{
			const std::uint64_t preparedFrame = preparedFrameCount.load(std::memory_order_acquire);
			if (preparedFrame == 0u || preparedFrame <= publishedFrameCount.load(std::memory_order_acquire))
			{
				return;
			}

			std::scoped_lock lock(candidateMutex);
			// Recheck after taking the lock so a repeated protected callback cannot
			// publish the same private batch twice.
			if (preparedFrame <= publishedFrameCount.load(std::memory_order_relaxed))
			{
				return;
			}
			const std::size_t candidateCount = preparedPublicationCandidates.size();
			std::vector<VirtualGeometryPhysicalPageResidencyRow> residencyUpdates;
			const auto appendOrReplaceResidencyUpdate = [&residencyUpdates](VirtualGeometryPhysicalPageResidencyRow update)
			{
				auto existing = std::find_if(residencyUpdates.begin(), residencyUpdates.end(), [&update](const auto& row)
				{
					return row.assetId == update.assetId && row.pageId == update.pageId;
				});
				if (existing == residencyUpdates.end())
				{
					residencyUpdates.push_back(std::move(update));
				}
				else
				{
					*existing = std::move(update);
				}
			};
			for (const VirtualGeometryStreamingDecodedPage& decoded : lastPreparedPlan.decodedPages)
			{
				VirtualGeometryPhysicalPageResidencyRow update;
				update.assetId = decoded.request.assetId;
				update.pageId = decoded.request.pageId;
				update.residencyGeneration = preparedFrame;
				// Exact TEMFPRD1 page totals are mandatory and were validated before
				// worker IO, so publication never guesses a size from one block.
				update.storedByteCount = decoded.request.expectedStoredByteCount;
				update.decodedByteCount = !decoded.decodedBytes.empty()
					? decoded.decodedBytes.size()
					: decoded.request.expectedDecodedByteCount;
				update.integrityValue = decoded.request.expectedIntegrityValue;
				if (decoded.status == VirtualGeometryStreamingDecodeStatus::PendingUpload)
				{
					update.state = VirtualGeometryPagePublicationState::PendingUpload;
					appendOrReplaceResidencyUpdate(std::move(update));
				}
				else if (decoded.status == VirtualGeometryStreamingDecodeStatus::PendingDecode)
				{
					update.state = VirtualGeometryPagePublicationState::PendingDecode;
					appendOrReplaceResidencyUpdate(std::move(update));
				}
				else if (decoded.status == VirtualGeometryStreamingDecodeStatus::Failed)
				{
					update.state = VirtualGeometryPagePublicationState::Failed;
					appendOrReplaceResidencyUpdate(std::move(update));
				}
			}

			VirtualGeometryPagePublicationBatchResult candidatePreview;
			if (!preparedPublicationCandidates.empty())
			{
				candidatePreview = PublishVirtualGeometryPagesAtomically(
					publicationStore.Snapshot(), preparedPublicationCandidates, preparedFrame);
				if (candidatePreview.accepted)
				{
					for (const VirtualGeometryPagePublicationCandidate& candidate : preparedPublicationCandidates)
					{
						appendOrReplaceResidencyUpdate({
							candidate.key.asset.assetId,
							candidate.key.pageId,
							VirtualGeometryPagePublicationState::Resident,
							candidate.publicationGeneration,
							candidate.payload.storedByteCount,
							candidate.payload.decodedByteCount,
							candidate.payload.integrityValue
						});
					}
				}
			}

			VirtualGeometryPhysicalPageResidencyBatchResult residencyResult;
			if (!residencyUpdates.empty())
			{
				residencyResult = residencyStore.PublishAtProtectedBoundary(residencyUpdates, preparedFrame);
			}
			const bool residencyAccepted = residencyUpdates.empty() || residencyResult.accepted;
			VirtualGeometryPagePublicationBatchResult result;
			if (!preparedPublicationCandidates.empty() && candidatePreview.accepted && residencyAccepted)
			{
				result = publicationStore.PublishAtProtectedBoundary(preparedPublicationCandidates, preparedFrame);
			}
			preparedPublicationCandidates.clear();
			publishedFrameCount.store(preparedFrame, std::memory_order_release);
			diagnostics.lastPublicationFrame = preparedFrame;
			if (candidateCount != 0u && result.accepted)
			{
				++diagnostics.acceptedPublicationBatches;
				diagnostics.acceptedPublicationPages += candidateCount;
				diagnostics.visibleResidentPages = 0u;
				for (const VirtualGeometryPhysicalPageResidencyRow& row : residencyStore.Snapshot().pages)
				{
					if (row.state == VirtualGeometryPagePublicationState::Resident)
					{
						++diagnostics.visibleResidentPages;
					}
				}
				diagnostics.lastRejectionReason.clear();
				RecordVirtualGeometryStreamingEvent(diagnostics, {
					preparedFrame,
					kInvalidVirtualGeometryPageId,
					"publish",
					"accepted complete page publication batch",
					false
				});
				return;
			}

			if (candidateCount == 0u && residencyAccepted)
			{
				RecordVirtualGeometryStreamingEvent(diagnostics, {
					preparedFrame,
					kInvalidVirtualGeometryPageId,
					residencyUpdates.empty() ? "publish-empty" : "publish-pending-upload",
					residencyUpdates.empty()
						? "no candidates or decoded-page state required publication at this boundary"
						: "published immutable pending decode/upload residency without claiming GPU completion",
					false
				});
				return;
			}

			diagnostics.rejectedPublications += candidateCount;
			diagnostics.lastRejectionReason = !candidatePreview.rejectionReason.empty()
				? candidatePreview.rejectionReason
				: residencyResult.rejectionReason;
			RecordVirtualGeometryStreamingEvent(diagnostics, {
				preparedFrame,
				kInvalidVirtualGeometryPageId,
				"publish-rejected",
				diagnostics.lastRejectionReason,
				true
			});
		};
		return { workloads.begin(), workloads.end() };
	}
} // namespace TerraEngine::Resource
