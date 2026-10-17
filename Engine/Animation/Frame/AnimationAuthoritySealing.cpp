#include "Animation/Frame/AnimationFrameRuntime.h"

namespace TerraEngine::Animation {
	[[nodiscard]] bool
	AnimationSystem::Impl::CanRetainPublishedAuthorityRoot(const std::shared_ptr<const AnimationAuthorityFrame>& previous,
														   std::uint64_t session, std::uint64_t lifecycle) const noexcept {
		// Reusing a root is stricter than reusing a pose. Any command, specialist
		// producer, compact GPU packet, membership difference, or lifecycle change
		// needs a newly sealed authority frame so its sidecars remain exact.
		if(!previous || !previous->IsValid() || previous->sessionGeneration != session || previous->lifecycleGeneration != lifecycle ||
		   !frameInputs.capturedCommands.empty() || !frameInputs.capturedProceduralCommands.empty() ||
		   !frameInputs.capturedRetargetRequests.empty() || !frameInputs.capturedMotionMatchRequests.empty() ||
		   !frameInputs.capturedMotionMatchQueriesV2.empty() || !frameInputs.capturedRagdollRequests.empty() ||
		   !frameInputs.capturedDeterminismRequests.empty() || !pendingRootMotionApplications.empty()) {
			return false;
		}
		std::size_t priorRowCount = 0U;
		for(const std::shared_ptr<const AnimationAuthorityPage>& page : *previous->pages) {
			if(!page || page->rows.size() > frameInputs.capturedInputWorkIndices.size() - priorRowCount) {
				return false;
			}
			priorRowCount += page->rows.size();
		}
		if(priorRowCount != frameInputs.capturedInputWorkIndices.size())
			return false;
		for(const CapturedInputWorkIndex& captured : frameInputs.capturedInputWorkIndices) {
			if(captured.compact || captured.storageIndex >= frameStorage.frameWork.size())
				return false;
			const FrameWorkItem& item = frameStorage.frameWork[captured.storageIndex];
			const bool preservesPrior = item.cacheReused || item.fallbackDecision.source == AnimationFallbackSource::HeldPriorGeneration;
			const AnimationAuthorityRow* prior = previous->Find(item.input.handle);
			if(!preservesPrior || prior == nullptr || item.selectedPublication == nullptr ||
			   item.selectedPublication->snapshot == nullptr || item.selectedPublication->poseGeneration != prior->poseGeneration ||
			   item.selectedPublication->authorityPublicationGeneration != prior->authorityPublicationGeneration) {
				return false;
			}
		}
		return true;
	}

	[[nodiscard]] bool AnimationSystem::Impl::SealAuthorityFrameCandidate() {
		authorityState.sealedAuthorityCandidate = {};
		authorityState.sealedAuthorityCandidateReady = false;
		authorityState.publishedAuthorityCommit = {};
		authorityState.preparedDiagnostics.reset();
		authorityState.preparedDetailedDiagnostics.reset();
		authorityState.preparedPresentationFrame.reset();
		authorityState.retainsPublishedAuthorityRoot = false;
		if(frameInputs.capturedInputWorkIndices.empty() || frameInputs.frameTick <= 0)
			return false;
		try {
			std::uint64_t candidateGeneration = 0U;
			std::uint32_t largestSlot = 0U;
			for(const CapturedInputWorkIndex& captured : frameInputs.capturedInputWorkIndices) {
				const AnimatorInstanceHandle handle = captured.compact
														  ? frameInputs.compactGpuVisualCaptures.inputs[captured.storageIndex].handle
														  : frameStorage.frameWork[captured.storageIndex].input.handle;
				if(!handle.IsValid())
					return false;
				largestSlot = std::max(largestSlot, handle.slotIndex);
				const std::uint64_t rowGeneration =
					captured.compact ? frameInputs.compactGpuVisualCaptures.authorityGeneration
									 : frameStorage.frameWork[captured.storageIndex].candidateAuthorityPublicationGeneration;
				if(rowGeneration == 0U || (candidateGeneration != 0U && candidateGeneration != rowGeneration))
					return false;
				candidateGeneration = rowGeneration;
			}
			const std::uint64_t session = runtimeSessionEpoch.load(std::memory_order_acquire);
			const std::uint64_t lifecycle = frameInputs.frameLifecycleGenerations.Generation(AnimationLifecycleBoundary::Scene);
			if(candidateGeneration == 0U || session == 0U || lifecycle == 0U)
				return false;
			const std::shared_ptr<const AnimationAuthorityFrame> previous = authorityPublisher.Load();
			if(CanRetainPublishedAuthorityRoot(previous, session, lifecycle)) {
				// No pose, state, sidecar, or membership fact changed. Publishing a new
				// generation here would break hold-last identity and re-expose old events.
				authorityState.retainsPublishedAuthorityRoot = true;
				return true;
			}
			const std::size_t pageCount = static_cast<std::size_t>(largestSlot) / kAnimationAuthorityPageCapacity128 + 1U;
			std::vector<std::shared_ptr<AnimationAuthorityPage>> mutablePages;
			std::vector<bool> pageRetainsPrior(pageCount, previous != nullptr);
			// Reuse only a complete retired page set. Every page must be
			// owned solely by its pool slot before any row is cleared, so
			// a renderer holding an older authority frame can never observe
			// mutation. The local vector takes a second reference only after
			// that proof succeeds.
			for(std::size_t attempt = 0U; attempt < persistentReuse.authorityPagePool.size(); ++attempt) {
				const std::size_t slot = (persistentReuse.authorityPagePoolCursor + attempt) % persistentReuse.authorityPagePool.size();
				auto& candidatePages = persistentReuse.authorityPagePool[slot];
				const bool retired = std::all_of(candidatePages.begin(), candidatePages.end(),
												 [](const auto& page) { return !page || page.use_count() == 1U; });
				if(!retired)
					continue;
				candidatePages.resize(pageCount);
				for(std::size_t pageIndex = 0U; pageIndex < pageCount; ++pageIndex) {
					auto& page = candidatePages[pageIndex];
					if(!page)
						page = std::make_shared<AnimationAuthorityPage>();
					page->firstSlot = static_cast<std::uint32_t>(pageIndex * kAnimationAuthorityPageCapacity128);
					page->capacity = static_cast<std::uint32_t>(kAnimationAuthorityPageCapacity128);
					page->rows.clear();
					page->changedMasks.assign(2U, 0U);
					page->workerValidatedRowCount = 0U;
				}
				mutablePages = candidatePages;
				persistentReuse.authorityPagePoolCursor = (slot + 1U) % persistentReuse.authorityPagePool.size();
				break;
			}
			if(mutablePages.empty()) {
				mutablePages.resize(pageCount);
				for(std::size_t pageIndex = 0U; pageIndex < pageCount; ++pageIndex) {
					auto page = std::make_shared<AnimationAuthorityPage>();
					page->firstSlot = static_cast<std::uint32_t>(pageIndex * kAnimationAuthorityPageCapacity128);
					page->capacity = static_cast<std::uint32_t>(kAnimationAuthorityPageCapacity128);
					page->changedMasks.assign(2U, 0U);
					mutablePages[pageIndex] = std::move(page);
				}
			}

			std::shared_ptr<AnimationFrameSidecars> sidecars;
			for(std::size_t attempt = 0U; attempt < persistentReuse.authoritySidecarPool.size(); ++attempt) {
				const std::size_t slot =
					(persistentReuse.authoritySidecarPoolCursor + attempt) % persistentReuse.authoritySidecarPool.size();
				auto& candidate = persistentReuse.authoritySidecarPool[slot];
				if(!candidate)
					candidate = std::make_shared<AnimationFrameSidecars>();
				if(candidate.use_count() != 1U)
					continue;
				candidate->events.clear();
				candidate->rootMotion.clear();
				candidate->callbacks.clear();
				candidate->physics.clear();
				candidate->attachments.clear();
				candidate->procedural.clear();
				candidate->rollback.clear();
				candidate->completions.clear();
				candidate->gpuCandidates.clear();
				candidate->diagnostics.clear();
				candidate->summary = {};
				candidate->workerValidatedAuthorityGeneration = 0U;
				sidecars = candidate;
				persistentReuse.authoritySidecarPoolCursor = (slot + 1U) % persistentReuse.authoritySidecarPool.size();
				break;
			}
			if(!sidecars)
				sidecars = std::make_shared<AnimationFrameSidecars>();
			bool compactPagesWorkerValidated = false;
			const bool allCapturedInputsCompact =
				frameInputs.capturedInputWorkIndices.size() == frameInputs.compactGpuVisualCaptures.size() &&
				std::all_of(frameInputs.capturedInputWorkIndices.begin(), frameInputs.capturedInputWorkIndices.end(),
							[](const CapturedInputWorkIndex& captured) { return captured.compact; });
			if(allCapturedInputsCompact && frameInputs.capturedInputWorkIndices.size() >= 512U &&
			   evaluationState.availableWorkerCount > 1U) {
				if(!PrepareHomogeneousAuthorityRows(candidateGeneration, session, pageCount, previous, mutablePages, pageRetainsPrior, sidecars,
													compactPagesWorkerValidated))
					return false;
			} else if(frameInputs.compactGpuVisualCaptures.size() >= 512U && evaluationState.availableWorkerCount > 1U) {
				if(!PrepareMixedAuthorityRows(candidateGeneration, session, pageCount, previous, mutablePages, pageRetainsPrior, sidecars,
											  compactPagesWorkerValidated))
					return false;
			} else {
				if(!PrepareOrdinaryAuthorityRows(candidateGeneration, session, previous, mutablePages, pageRetainsPrior, sidecars,
												 compactPagesWorkerValidated))
					return false;
			}
			std::vector<std::shared_ptr<const AnimationAuthorityPage>> pages;
			pages.reserve(mutablePages.size());
			std::uint32_t reusedPageCount = 0U;
			std::uint32_t changedPageCount = 0U;
			{
				TERRAENGINE_PROFILE_SCOPE("Animation: Finalize Authority Pages", TerraEngine::Profiling::ProfilerModuleId::CpuEngine);
				for(std::size_t pageIndex = 0U; pageIndex < mutablePages.size(); ++pageIndex) {
					auto& page = mutablePages[pageIndex];
					if(!compactPagesWorkerValidated) {
						std::sort(page->rows.begin(), page->rows.end(),
								  [](const AnimationAuthorityRow& left, const AnimationAuthorityRow& right) {
									  return left.key.slot < right.key.slot;
								  });
					} else {
						// Strictly increasing input slots and
						// deterministic row positions prove
						// ordering; every row was validated by
						// its owning worker.
						page->workerValidatedRowCount = page->rows.size();
					}
					const std::shared_ptr<const AnimationAuthorityPage> priorPage =
						previous && previous->pages && pageIndex < previous->pages->size() ? previous->pages->at(pageIndex) : nullptr;
					const bool pageMembershipMatches = pageRetainsPrior[pageIndex] && priorPage != nullptr &&
													   priorPage->firstSlot == page->firstSlot && priorPage->capacity == page->capacity &&
													   priorPage->rows.size() == page->rows.size() &&
													   std::equal(page->rows.begin(), page->rows.end(), priorPage->rows.begin(),
																  [](const AnimationAuthorityRow& left,
																	 const AnimationAuthorityRow& right) { return left.key == right.key; });
					if(pageMembershipMatches) {
						// Page data and its immutable rows are
						// byte-for-byte the prior authority
						// facts. Retain the page pointer instead
						// of rebuilding a crowd-sized copy merely
						// because another page changed this
						// frame.
						pages.push_back(priorPage);
						++reusedPageCount;
					} else {
						// A changed page is delivered as one
						// complete immutable value. The all-ones
						// mask is conservative for this initial
						// page implementation: consumers never
						// miss a changed row, while wholly
						// unchanged pages retain their original
						// shared identity above.
						std::fill(page->changedMasks.begin(), page->changedMasks.end(), std::numeric_limits<std::uint64_t>::max());
						pages.push_back(std::move(page));
						++changedPageCount;
					}
				}
			}
			sidecars->summary.evaluatedRows = static_cast<std::uint32_t>(frameInputs.capturedInputWorkIndices.size());
			AnimationAuthoritySealInput input;
			input.frameId = static_cast<std::uint64_t>(frameInputs.frameTick);
			input.sessionGeneration = session;
			input.lifecycleGeneration = lifecycle;
			input.expectedPreviousGeneration = previous ? previous->authorityGeneration : 0U;
			input.candidateGeneration = candidateGeneration;
			input.pages = pages;
			// This worker is the sole mutable owner. Seal
			// validates it in place and publishes the same
			// allocation as const, avoiding a 4,000-entry
			// copy.
			input.ownedSidecars = std::move(sidecars);
			input.workersComplete = true;
			input.reusedPages = reusedPageCount;
			input.changedPages = changedPageCount;
			// Existing deterministic history has already
			// reserved its bounded ring; sealing needs no
			// later allocation to attach this candidate
			// generation.
			input.historyReserved = true;
			input.cancelled = PollFrameCancellation();
			authorityState.sealedAuthorityCandidate = SealAnimationFrameResult(input);
			authorityState.sealedAuthorityCandidateReady = authorityState.sealedAuthorityCandidate.IsValid();
			if(!authorityState.sealedAuthorityCandidateReady)
				frameInputs.commandFrameFailurePhase = "SealAnimationFrameResult";
			if(authorityState.sealedAuthorityCandidateReady) {
				// The candidate itself contains the
				// authoritative predecessor proof. Keep only
				// scalar identities here so protected
				// publication never needs to revisit the
				// crowd-sized frame work or sidecar
				// collections.
				authorityState.publishedAuthorityCommit.animatorCount = frameInputs.capturedInputWorkIndices.size();
				authorityState.publishedAuthorityCommit.frameEpoch = frameDeclarationEpoch.load(std::memory_order_acquire);
				authorityState.publishedAuthorityCommit.authorityGeneration = candidateGeneration;
				authorityState.publishedAuthorityCommit.sessionGeneration = session;
				authorityState.publishedAuthorityCommit.sceneLifecycleGeneration = lifecycle;
				const bool runtimeRootPrepared = authorityState.publishedAuthorityCommit.expectedRuntimeRoot != nullptr &&
												 authorityState.publishedAuthorityCommit.nextRuntimeRoot != nullptr &&
												 authorityState.publishedAuthorityCommit.specialistSidecars != nullptr;
				if(!runtimeRootPrepared && !PrepareRuntimeRootCandidate(candidateGeneration)) {
					if(!frameInputs.commandFrameFailurePhase)
						frameInputs.commandFrameFailurePhase = "PrepareRuntimeRootCandidate";
					authorityState.sealedAuthorityCandidate = {};
					authorityState.sealedAuthorityCandidateReady = false;
					authorityState.publishedAuthorityCommit = {};
				} else {
					// Diagnostics and GPU owner/lowering tables
					// are crowd-sized copied values. Build them
					// on this worker from the same sealed
					// generation so protected publication only
					// submits already-owned immutable roots.
					if(IsAnimationDetailedDiagnosticsRequested()) {
						authorityState.preparedDetailedDiagnostics = BuildActiveDetailedDiagnosticsLocked(candidateGeneration);
					}
					{
						TERRAENGINE_PROFILE_SCOPE("Animation: Build Summary "
												  "Diagnostics",
												  TerraEngine::Profiling::ProfilerModuleId::CpuEngine);
						authorityState.preparedDiagnostics = BuildActiveDiagnosticsLocked(
							candidateGeneration,
							authorityState.preparedDetailedDiagnostics ? authorityState.preparedDetailedDiagnostics->animators.size() : 0U);
					}
					{
						TERRAENGINE_PROFILE_SCOPE("Animation: Build GPU Presentation "
												  "Frame",
												  TerraEngine::Profiling::ProfilerModuleId::CpuEngine);
						authorityState.preparedPresentationFrame =
							BuildGpuPresentationFrame(candidateGeneration, authorityState.sealedAuthorityCandidate.frame);
					}
				}
			}
			return authorityState.sealedAuthorityCandidateReady;
		} catch(const std::bad_alloc&) {
			authorityState.sealedAuthorityCandidate = {};
			authorityState.sealedAuthorityCandidateReady = false;
			authorityState.publishedAuthorityCommit = {};
			authorityState.preparedDiagnostics.reset();
			authorityState.preparedDetailedDiagnostics.reset();
			authorityState.preparedPresentationFrame.reset();
			return false;
		}
	}

	bool AnimationSystem::Impl::PrepareHomogeneousAuthorityRows(std::uint64_t candidateGeneration, std::uint64_t session, std::size_t pageCount,
																const std::shared_ptr<const AnimationAuthorityFrame>& previous,
																std::vector<std::shared_ptr<AnimationAuthorityPage>>& mutablePages,
																std::vector<bool>& pageRetainsPrior,
																const std::shared_ptr<AnimationFrameSidecars>& sidecars,
																bool& compactPagesWorkerValidated) {

		TERRAENGINE_PROFILE_SCOPE("Animation: Build Compact Publication "
								  "Roots",
								  TerraEngine::Profiling::ProfilerModuleId::CpuEngine);
		if(PollFrameCancellation() || !evaluationState.workPlanValidated || frameStorage.workRangeLedger == nullptr ||
		   !frameStorage.workRangeLedger->AllCompleted() ||
		   frameInputs.capturedInputWorkIndices.size() != evaluationState.plannedAnimatorCount.load(std::memory_order_acquire) ||
		   frameInputs.capturedInputWorkIndices.size() != evaluationState.evaluatedAnimatorCount.load(std::memory_order_acquire) ||
		   evaluationState.workCompletions.size() != frameStorage.workPlan.items.size() ||
		   evaluationState.compactGpuVisualCompletedCount.load(std::memory_order_acquire) !=
			   evaluationState.compactGpuVisualWorkItems.size())
			return false;
		const std::shared_ptr<RuntimeRecordRoot> expectedRuntimeRoot = runtimeRecords.load(std::memory_order_acquire);
		const std::shared_ptr<const CompactRuntimeStateRoot> expectedCompactRuntimeRoot =
			compactRuntimeStates.load(std::memory_order_acquire);
		if(!expectedRuntimeRoot || expectedRuntimeRoot != frameInputs.compactRuntimeCaptureRoot ||
		   expectedCompactRuntimeRoot != frameInputs.compactRuntimeStateCaptureRoot ||
		   frameInputs.compactGpuVisualCaptures.runtimeRecords.size() != frameInputs.compactGpuVisualCaptures.size())
			return false;
		const std::shared_ptr<RuntimeRecordRoot> nextRuntimeRoot = expectedRuntimeRoot;
		// Large crowds prepare runtime state in its own settled task group.
		// Smaller crowds retain the measured single-pass publication path.
		const bool prepareRuntimeSeparately = frameInputs.capturedInputWorkIndices.size() >= 8192U;
		auto nextCompactRuntimeRoot = prepareRuntimeSeparately ? std::shared_ptr<CompactRuntimeStateRoot>{}
															   : std::make_shared<CompactRuntimeStateRoot>(expectedRuntimeRoot->size());
		auto specialistSidecars =
			prepareRuntimeSeparately ? std::shared_ptr<SealedSpecialistSidecarBatch>{} : std::make_shared<SealedSpecialistSidecarBatch>();
		if(!prepareRuntimeSeparately)
			specialistSidecars->explicitRootMotion = pendingRootMotionApplications;
		// Assign each input one deterministic
		// destination before workers run.
		// Workers then write distinct
		// authority, sidecar, and runtime-root
		// elements without locks. Preparing
		// both immutable roots in one pass for smaller
		// crowds avoids dispatch overhead. Large crowds
		// prepare runtime state in its own settled group.
		std::vector<std::size_t> rowPositions(frameInputs.capturedInputWorkIndices.size());
		std::vector<std::size_t> pageRowCounts(pageCount, 0U);
		{
			TERRAENGINE_PROFILE_SCOPE("Animation: Prepare Compact "
									  "Publication Destinations",
									  TerraEngine::Profiling::ProfilerModuleId::CpuEngine);
			std::uint32_t previousRuntimeSlot = TerraEngine::Animation::kInvalidAnimatorInstanceSlot;
			for(std::size_t inputIndex = 0U; inputIndex < frameInputs.capturedInputWorkIndices.size(); ++inputIndex) {
				const CapturedInputWorkIndex& captured = frameInputs.capturedInputWorkIndices[inputIndex];
				if(captured.storageIndex >= frameInputs.compactGpuVisualCaptures.size())
					return false;
				const AnimatorInstanceHandle handle = frameInputs.compactGpuVisualCaptures.inputs[captured.storageIndex].handle;
				if(!handle.IsValid() || handle.slotIndex >= expectedRuntimeRoot->size() || handle.slotIndex >= nextRuntimeRoot->size() ||
				   (previousRuntimeSlot != TerraEngine::Animation::kInvalidAnimatorInstanceSlot && handle.slotIndex <= previousRuntimeSlot))
					return false;
				RuntimeRecord* const current = frameInputs.compactGpuVisualCaptures.runtimeRecords[captured.storageIndex];
				const CompactRuntimeState* priorCompactState = FindCompactRuntimeState(expectedCompactRuntimeRoot, handle);
				const AnimationGeneration priorPoseGeneration =
					priorCompactState ? priorCompactState->poseGeneration : (current ? current->poseGeneration : 0U);
				if(current == nullptr || current->handle != handle ||
				   frameInputs.compactGpuVisualCaptures.candidatePoseGenerations[captured.storageIndex] != priorPoseGeneration + 1U ||
				   frameInputs.compactGpuVisualCaptures.authorityGeneration != candidateGeneration ||
				   frameInputs.compactGpuVisualCaptures.evaluationStatuses[captured.storageIndex] != AnimationStatus::Ok)
					return false;
				const std::size_t pageIndex = handle.slotIndex / kAnimationAuthorityPageCapacity128;
				if(pageIndex >= mutablePages.size())
					return false;
				previousRuntimeSlot = handle.slotIndex;
				if(!prepareRuntimeSeparately && current->rollbackHistoryActive) {
					specialistSidecars->rollbackHistoryAnimators.push_back(handle);
				}
				rowPositions[inputIndex] = pageRowCounts[pageIndex]++;
				pageRetainsPrior[pageIndex] = false;
			}
			for(std::size_t pageIndex = 0U; pageIndex < pageCount; ++pageIndex) {
				mutablePages[pageIndex]->rows.resize(pageRowCounts[pageIndex]);
			}
		}
		sidecars->gpuCandidates.resize(frameInputs.capturedInputWorkIndices.size());
		std::atomic_bool compactRowsValid{true};
		const auto buildCompactPublicationRows = [this, session, candidateGeneration, &rowPositions, &mutablePages, &sidecars,
												  &specialistSidecars, &nextCompactRuntimeRoot, prepareRuntimeSeparately,
												  &compactRowsValid](std::size_t begin, std::size_t end) {
			TERRAENGINE_PROFILE_SCOPE("Animation Task: Build "
									  "Compact Publication Rows",
									  TerraEngine::Profiling::ProfilerModuleId::TaskExecution);
			for(std::size_t inputIndex = begin; inputIndex < end; ++inputIndex) {
				const std::size_t compactIndex = frameInputs.capturedInputWorkIndices[inputIndex].storageIndex;
				const CompactGpuVisualCaptures& compact = frameInputs.compactGpuVisualCaptures;
				const AnimatorInstanceHandle handle = compact.inputs[compactIndex].handle;
				AnimationAuthorityRow& row =
					mutablePages[handle.slotIndex / kAnimationAuthorityPageCapacity128]->rows[rowPositions[inputIndex]];
				row.key = {handle.slotIndex, handle.generation, session};
				row.ownerObjectId = compact.inputs[compactIndex].ownerObjectId;
				row.componentGeneration = compact.inputs[compactIndex].componentGeneration;
				row.sourceGeneration = compact.authorityGeneration;
				row.poseGeneration = compact.candidatePoseGenerations[compactIndex];
				row.tick = frameInputs.frameTick;
				row.stateId = 0U;
				row.renderBindingGeneration = compact.executionModeGenerations[compactIndex];
				row.active = true;
				row.safeFallback = false;
				AnimationSpecialistRecord& gpu = sidecars->gpuCandidates[inputIndex];
				gpu.key = {handle.slotIndex, handle.generation, session};
				gpu.authorityGeneration = candidateGeneration;
				gpu.kind = 1U;
				gpu.status = AnimationStatus::Ok;
				if(!row.IsValid() || !gpu.IsValidFor(candidateGeneration)) {
					compactRowsValid.store(false, std::memory_order_release);
				}

				if(!prepareRuntimeSeparately) {
					CompactRuntimeState& publishedState = nextCompactRuntimeRoot->at(handle.slotIndex);
					publishedState.handle = handle;
					publishedState.playback = compact.playbacks[compactIndex];
					publishedState.poseGeneration = compact.candidatePoseGenerations[compactIndex];
					publishedState.stateGeneration = compact.stateGenerations[compactIndex];
					publishedState.executionModeGeneration = compact.executionModeGenerations[compactIndex];
					publishedState.bindingContentGeneration = compact.runtimeRecords[compactIndex]->contentGeneration;
					publishedState.executionModeState = compact.executionDecisions[compactIndex].nextState;
				}
			}
		};
		// Compact rows perform only bounded
		// scalar copies. Profiling shows that a
		// 4,000-row publication completes
		// faster on the scheduler worker than
		// it does after creating and joining
		// several tiny nested tasks. Keep each
		// task coarse enough to amortize its
		// future, queue, and context-switch
		// costs; larger crowds can still use
		// multiple workers without
		// oversubscribing them.
		constexpr std::size_t kMinimumCompactRowsPerTask = 4096U;
		const std::size_t usefulWorkerCount = std::max<std::size_t>(
			1U, (frameInputs.capturedInputWorkIndices.size() + kMinimumCompactRowsPerTask - 1U) / kMinimumCompactRowsPerTask);
		const std::size_t workerCount = std::min<std::size_t>(evaluationState.availableWorkerCount, usefulWorkerCount);
		const std::size_t chunkSize = (frameInputs.capturedInputWorkIndices.size() + workerCount - 1U) / workerCount;
		std::vector<std::future<void>> futures;
		futures.reserve(workerCount);
		// This guard dies before every input captured by admitted children.
		TerraEngine::Tasks::ScopedTaskSettlement<void> futuresSettlement(futures,
																		 TerraEngine::Tasks::NestedTaskGroup::AnimationHomogeneousRows);
		// The scheduler worker already
		// executing this stage is useful
		// capacity. Queue the remaining chunks
		// first, then build chunk zero locally
		// while they run. This preserves
		// disjoint writes and avoids one
		// future, queue operation, and context
		// handoff on every animated frame.
		for(std::size_t workerIndex = 1U; workerIndex < workerCount; ++workerIndex) {
			const std::size_t begin = workerIndex * chunkSize;
			if(begin >= frameInputs.capturedInputWorkIndices.size())
				break;
			const std::size_t end = std::min(frameInputs.capturedInputWorkIndices.size(), begin + chunkSize);
			futuresSettlement.CreateAndEnqueue(ThreadPool::instance(),
											   [&buildCompactPublicationRows, begin, end]() { buildCompactPublicationRows(begin, end); });
		}
		futuresSettlement.RunInline(
			[&] { buildCompactPublicationRows(0U, std::min(frameInputs.capturedInputWorkIndices.size(), chunkSize)); });
		futuresSettlement.GetAll();
		if(PollFrameCancellation() || !compactRowsValid.load(std::memory_order_acquire))
			return false;
		compactPagesWorkerValidated = true;
		sidecars->workerValidatedAuthorityGeneration = candidateGeneration;
		if(!prepareRuntimeSeparately) {
			authorityState.publishedAuthorityCommit.expectedRuntimeRoot = expectedRuntimeRoot;
			authorityState.publishedAuthorityCommit.nextRuntimeRoot = std::move(nextRuntimeRoot);
			authorityState.publishedAuthorityCommit.expectedCompactRuntimeRoot = expectedCompactRuntimeRoot;
			authorityState.publishedAuthorityCommit.nextCompactRuntimeRoot = std::move(nextCompactRuntimeRoot);
			authorityState.publishedAuthorityCommit.specialistSidecars = std::move(specialistSidecars);
		}

		return true;
	}

	bool AnimationSystem::Impl::PrepareMixedAuthorityRows(std::uint64_t candidateGeneration, std::uint64_t session, std::size_t pageCount,
														  const std::shared_ptr<const AnimationAuthorityFrame>& previous,
														  std::vector<std::shared_ptr<AnimationAuthorityPage>>& mutablePages,
														  std::vector<bool>& pageRetainsPrior,
														  const std::shared_ptr<AnimationFrameSidecars>& sidecars,
														  bool& compactPagesWorkerValidated) {

		TERRAENGINE_PROFILE_SCOPE("Animation: Build Mixed Publication "
								  "Roots",
								  TerraEngine::Profiling::ProfilerModuleId::CpuEngine);
		// A few CPU/fallback exceptions must not
		// serialize a mostly compact crowd.  Assign
		// stable destinations first, then let workers
		// fill only the compact rows while this
		// worker handles the small exception set.
		std::vector<std::size_t> rowPositions(frameInputs.capturedInputWorkIndices.size());
		std::vector<std::size_t> pageRowCounts(pageCount, 0U);
		std::vector<std::size_t> gpuPositions(frameInputs.capturedInputWorkIndices.size(), std::numeric_limits<std::size_t>::max());
		std::vector<std::uint8_t> rowRetainsPrior(frameInputs.capturedInputWorkIndices.size(), 0U);
		std::uint32_t previousSlot = TerraEngine::Animation::kInvalidAnimatorInstanceSlot;
		std::size_t gpuCandidateCount = 0U;
		for(std::size_t inputIndex = 0U; inputIndex < frameInputs.capturedInputWorkIndices.size(); ++inputIndex) {
			const CapturedInputWorkIndex& captured = frameInputs.capturedInputWorkIndices[inputIndex];
			AnimatorInstanceHandle handle;
			bool producesGpuCandidate = false;
			if(captured.compact) {
				if(captured.storageIndex >= frameInputs.compactGpuVisualCaptures.size())
					return false;
				handle = frameInputs.compactGpuVisualCaptures.inputs[captured.storageIndex].handle;
				producesGpuCandidate = true;
			} else {
				if(captured.storageIndex >= frameStorage.frameWork.size())
					return false;
				const FrameWorkItem& item = frameStorage.frameWork[captured.storageIndex];
				handle = item.input.handle;
				const bool preservesPrior =
					item.cacheReused || item.fallbackDecision.source == AnimationFallbackSource::HeldPriorGeneration;
				const AnimationAuthorityRow* prior = preservesPrior && previous ? previous->Find(handle) : nullptr;
				rowRetainsPrior[inputIndex] = prior != nullptr ? 1U : 0U;
				producesGpuCandidate = prior == nullptr && item.gpuLowering;
			}
			if(!handle.IsValid() ||
			   (previousSlot != TerraEngine::Animation::kInvalidAnimatorInstanceSlot && handle.slotIndex <= previousSlot))
				return false;
			const std::size_t pageIndex = handle.slotIndex / kAnimationAuthorityPageCapacity128;
			if(pageIndex >= mutablePages.size())
				return false;
			previousSlot = handle.slotIndex;
			rowPositions[inputIndex] = pageRowCounts[pageIndex]++;
			if(producesGpuCandidate) {
				gpuPositions[inputIndex] = gpuCandidateCount++;
			}
		}
		for(std::size_t pageIndex = 0U; pageIndex < pageCount; ++pageIndex) {
			mutablePages[pageIndex]->rows.resize(pageRowCounts[pageIndex]);
		}
		sidecars->gpuCandidates.resize(gpuCandidateCount);
		std::atomic_bool mixedRowsValid{true};
		const auto buildMixedCompactRows = [this, session, candidateGeneration, &rowPositions, &gpuPositions, &mutablePages, &sidecars,
											&mixedRowsValid](std::size_t begin, std::size_t end) {
			TERRAENGINE_PROFILE_SCOPE("Animation Task: Build Mixed "
									  "Compact Rows",
									  TerraEngine::Profiling::ProfilerModuleId::TaskExecution);
			for(std::size_t inputIndex = begin; inputIndex < end; ++inputIndex) {
				const CapturedInputWorkIndex& captured = frameInputs.capturedInputWorkIndices[inputIndex];
				if(!captured.compact)
					continue;
				const std::size_t compactIndex = captured.storageIndex;
				const AnimatorInstanceHandle handle = frameInputs.compactGpuVisualCaptures.inputs[compactIndex].handle;
				AnimationAuthorityRow& row =
					mutablePages[handle.slotIndex / kAnimationAuthorityPageCapacity128]->rows[rowPositions[inputIndex]];
				row.key = {handle.slotIndex, handle.generation, session};
				row.ownerObjectId = frameInputs.compactGpuVisualCaptures.inputs[compactIndex].ownerObjectId;
				row.componentGeneration = frameInputs.compactGpuVisualCaptures.inputs[compactIndex].componentGeneration;
				row.sourceGeneration = frameInputs.compactGpuVisualCaptures.authorityGeneration;
				row.poseGeneration = frameInputs.compactGpuVisualCaptures.candidatePoseGenerations[compactIndex];
				row.tick = frameInputs.frameTick;
				row.stateId = 0U;
				row.renderBindingGeneration = frameInputs.compactGpuVisualCaptures.executionModeGenerations[compactIndex];
				row.active = true;
				row.safeFallback = false;
				AnimationSpecialistRecord& gpu = sidecars->gpuCandidates[gpuPositions[inputIndex]];
				gpu.key = {handle.slotIndex, handle.generation, session};
				gpu.authorityGeneration = candidateGeneration;
				gpu.kind = 1U;
				gpu.status = AnimationStatus::Ok;
				if(!row.IsValid() || !gpu.IsValidFor(candidateGeneration)) {
					mixedRowsValid.store(false, std::memory_order_release);
				}
			}
		};
		// Mixed crowds still contain the same cheap
		// compact-row writes. Use the measured coarse
		// granularity from the homogeneous path while
		// the current worker handles the small
		// exception set, avoiding nested
		// oversubscription.
		constexpr std::size_t kMinimumMixedRowsPerTask = 4096U;
		const std::size_t usefulWorkerCount = std::max<std::size_t>(
			1U, (frameInputs.capturedInputWorkIndices.size() + kMinimumMixedRowsPerTask - 1U) / kMinimumMixedRowsPerTask);
		const std::size_t workerCount = std::min<std::size_t>(evaluationState.availableWorkerCount, usefulWorkerCount);
		const std::size_t chunkSize = (frameInputs.capturedInputWorkIndices.size() + workerCount - 1U) / workerCount;
		std::vector<std::future<void>> futures;
		futures.reserve(workerCount);
		// This guard dies before every input captured by admitted children.
		TerraEngine::Tasks::ScopedTaskSettlement<void> futuresSettlement(futures, TerraEngine::Tasks::NestedTaskGroup::AnimationMixedRows);
		// As in the homogeneous path, the current
		// scheduler worker builds the first disjoint
		// range while queued workers build the rest.
		for(std::size_t workerIndex = 1U; workerIndex < workerCount; ++workerIndex) {
			const std::size_t begin = workerIndex * chunkSize;
			if(begin >= frameInputs.capturedInputWorkIndices.size())
				break;
			const std::size_t end = std::min(frameInputs.capturedInputWorkIndices.size(), begin + chunkSize);
			futuresSettlement.CreateAndEnqueue(ThreadPool::instance(),
											   [&buildMixedCompactRows, begin, end]() { buildMixedCompactRows(begin, end); });
		}
		futuresSettlement.RunInline([&] { buildMixedCompactRows(0U, std::min(frameInputs.capturedInputWorkIndices.size(), chunkSize)); });
		futuresSettlement.GetAll();
		if(PollFrameCancellation() || !mixedRowsValid.load(std::memory_order_acquire))
			return false;
		// Only the exceptional full rows remain on
		// this worker. Their snapshots own
		// variable-length events, so merge those
		// sidecars after all writes.
		for(std::size_t inputIndex = 0U; inputIndex < frameInputs.capturedInputWorkIndices.size(); ++inputIndex) {
			const CapturedInputWorkIndex& captured = frameInputs.capturedInputWorkIndices[inputIndex];
			if(captured.compact) {
				const AnimatorInstanceHandle handle = frameInputs.compactGpuVisualCaptures.inputs[captured.storageIndex].handle;
				pageRetainsPrior[handle.slotIndex / kAnimationAuthorityPageCapacity128] = false;
				continue;
			}
			const FrameWorkItem& item = frameStorage.frameWork[captured.storageIndex];
			const AnimatorInstanceHandle handle = item.input.handle;
			AnimationAuthorityRow& row =
				mutablePages[handle.slotIndex / kAnimationAuthorityPageCapacity128]->rows[rowPositions[inputIndex]];
			if(rowRetainsPrior[inputIndex] != 0U) {
				row = *previous->Find(handle);
			} else {
				row.key = {handle.slotIndex, handle.generation, session};
				row.ownerObjectId = item.input.descriptor.ownerObjectId;
				row.objectGeneration = item.sourceObjectGeneration;
				row.componentGeneration = item.input.descriptor.componentGeneration;
				row.sourceGeneration = item.candidateAuthorityPublicationGeneration;
				row.authorityPublicationGeneration = item.selectedPublication ? item.selectedPublication->authorityPublicationGeneration
																			  : item.candidateAuthorityPublicationGeneration;
				row.poseGeneration = item.selectedPublication && item.selectedPublication->snapshot
										 ? item.selectedPublication->snapshot->publicationGeneration
										 : item.candidatePoseGeneration;
				row.tick = frameInputs.frameTick;
				row.stateId = item.binding.stateId;
				row.renderBindingGeneration = item.binding.programGeneration;
				row.controller = item.input.descriptor.controller;
				if(item.binding.clip) {
					row.clip = {item.binding.clip->assetId, item.binding.clipGeneration};
				}
				if(item.binding.skeleton) {
					row.skeleton = {item.binding.skeleton->assetId, item.binding.skeletonGeneration};
					row.skeletonAsset = item.binding.skeleton;
					row.skeletonCompatibilityFingerprint = item.binding.skeleton->compatibilityFingerprint;
				}
				row.jointStableIds.assign(item.binding.jointStableIds.begin(), item.binding.jointStableIds.end());
				row.visualPoseContentIdLow = item.visualPoseContentIdLow;
				row.visualPoseContentIdHigh = item.visualPoseContentIdHigh;
				row.poseMemoryReservation = item.selectedPublication ? item.selectedPublication->poseMemoryReservation : nullptr;
				row.sourceObjectWorldTransform = item.sourceObjectWorldTransform;
				row.sourceObjectWorldTransformCaptured = item.sourceObjectWorldTransformCaptured;
				row.requestedExecutionPolicy = item.binding.requestedExecutionPolicy;
				row.effectiveExecutionMode = item.executionDecision.effectiveMode;
				row.rootMotionPolicy = item.binding.rootMotionPolicy;
				row.updatePolicy = item.binding.updatePolicy;
				row.eventPolicy = item.binding.eventPolicy;
				row.cullingPolicy = item.binding.cullingPolicy;
				row.status = item.evaluationStatus;
				row.snapshot = item.selectedPublication ? item.selectedPublication->snapshot : nullptr;
				row.streamedClipPayloadsActive = item.selectedPublication ? item.selectedPublication->streamedClipPayloadsActive
																		  : item.binding.streamedSegmentPayloads != nullptr;
				row.active = true;
				row.safeFallback = item.safePose;
				pageRetainsPrior[handle.slotIndex / kAnimationAuthorityPageCapacity128] = false;
				if(row.snapshot && row.snapshot->pose) {
					sidecars->events.insert(sidecars->events.end(), row.snapshot->pose->Events().begin(),
											row.snapshot->pose->Events().end());
					if(row.snapshot->pose->RootMotionDelta()) {
						sidecars->rootMotion.push_back(*row.snapshot->pose->RootMotionDelta());
					}
				}
			}
			if(!row.IsValid())
				return false;
			const std::size_t gpuPosition = gpuPositions[inputIndex];
			if(gpuPosition != std::numeric_limits<std::size_t>::max()) {
				AnimationSpecialistRecord& gpu = sidecars->gpuCandidates[gpuPosition];
				gpu.key = {row.key.slot, row.key.lifetimeGeneration, session};
				gpu.authorityGeneration = candidateGeneration;
				gpu.kind = 1U;
				gpu.status = AnimationStatus::Ok;
				if(!gpu.IsValidFor(candidateGeneration))
					return false;
			}
		}
		compactPagesWorkerValidated = true;
		sidecars->workerValidatedAuthorityGeneration = candidateGeneration;

		return true;
	}

	bool AnimationSystem::Impl::PrepareOrdinaryAuthorityRows(std::uint64_t candidateGeneration, std::uint64_t session,
															 const std::shared_ptr<const AnimationAuthorityFrame>& previous,
															 std::vector<std::shared_ptr<AnimationAuthorityPage>>& mutablePages,
															 std::vector<bool>& pageRetainsPrior,
															 const std::shared_ptr<AnimationFrameSidecars>& sidecars,
															 bool& compactPagesWorkerValidated) {

		for(const CapturedInputWorkIndex& captured : frameInputs.capturedInputWorkIndices) {
			AnimationAuthorityRow row;
			std::shared_ptr<const AnimationSnapshot> snapshot;
			if(captured.compact) {
				const CompactGpuVisualCaptures& compact = frameInputs.compactGpuVisualCaptures;
				const std::size_t index = captured.storageIndex;
				row.key = {compact.inputs[index].handle.slotIndex, compact.inputs[index].handle.generation, session};
				row.ownerObjectId = compact.inputs[index].ownerObjectId;
				row.componentGeneration = compact.inputs[index].componentGeneration;
				row.sourceGeneration = compact.authorityGeneration;
				row.poseGeneration = compact.candidatePoseGenerations[index];
				row.tick = frameInputs.frameTick;
				// Compact visual packets do not capture
				// controller graph state; zero explicitly
				// records that this row has no
				// CPU-authoritative graph id.
				row.stateId = 0U;
				row.renderBindingGeneration = compact.executionModeGenerations[index];
				row.active = true;
				row.safeFallback = false;
				AnimationSpecialistRecord gpu;
				gpu.key = {row.key.slot, row.key.lifetimeGeneration, session};
				gpu.authorityGeneration = candidateGeneration;
				gpu.kind = 1U;
				gpu.status = AnimationStatus::Ok;
				sidecars->gpuCandidates.push_back(gpu);
				pageRetainsPrior[row.key.slot / kAnimationAuthorityPageCapacity128] = false;
			} else {
				const FrameWorkItem& item = frameStorage.frameWork[captured.storageIndex];
				const bool preservesPrior =
					item.cacheReused || item.fallbackDecision.source == AnimationFallbackSource::HeldPriorGeneration;
				const AnimationAuthorityRow* prior = preservesPrior && previous ? previous->Find(item.input.handle) : nullptr;
				const bool retainRow = prior != nullptr;
				if(retainRow) {
					// A changed neighboring animator may
					// require a new root, but this row
					// still describes the exact prior
					// authority rather than the currently
					// incomplete binding that requested
					// hold-last.
					row = *prior;
				} else {
					row.key = {item.input.handle.slotIndex, item.input.handle.generation, session};
					row.ownerObjectId = item.input.descriptor.ownerObjectId;
					row.objectGeneration = item.sourceObjectGeneration;
					row.componentGeneration = item.input.descriptor.componentGeneration;
					row.sourceGeneration = item.candidateAuthorityPublicationGeneration;
					// Preserve the exact immutable row
					// selected by worker evaluation.  The
					// frame seal may reuse a prior
					// publication after a recoverable
					// failure, so reconstructing these
					// diagnostics from the current binding
					// would incorrectly describe the
					// retained authority state.
					row.authorityPublicationGeneration = item.selectedPublication ? item.selectedPublication->authorityPublicationGeneration
																				  : item.candidateAuthorityPublicationGeneration;
					row.poseGeneration = item.selectedPublication && item.selectedPublication->snapshot
											 ? item.selectedPublication->snapshot->publicationGeneration
											 : item.candidatePoseGeneration;
					row.tick = frameInputs.frameTick;
					row.stateId = item.binding.stateId;
					row.renderBindingGeneration = item.binding.programGeneration;
					row.controller = item.input.descriptor.controller;
					if(item.binding.clip)
						row.clip = {item.binding.clip->assetId, item.binding.clipGeneration};
					if(item.binding.skeleton) {
						row.skeleton = {item.binding.skeleton->assetId, item.binding.skeletonGeneration};
						row.skeletonAsset = item.binding.skeleton;
						row.skeletonCompatibilityFingerprint = item.binding.skeleton->compatibilityFingerprint;
					}
					row.jointStableIds.assign(item.binding.jointStableIds.begin(), item.binding.jointStableIds.end());
					row.visualPoseContentIdLow = item.visualPoseContentIdLow;
					row.visualPoseContentIdHigh = item.visualPoseContentIdHigh;
					row.poseMemoryReservation = item.selectedPublication ? item.selectedPublication->poseMemoryReservation : nullptr;
					row.sourceObjectWorldTransform = item.sourceObjectWorldTransform;
					row.sourceObjectWorldTransformCaptured = item.sourceObjectWorldTransformCaptured;
					row.requestedExecutionPolicy = item.binding.requestedExecutionPolicy;
					row.effectiveExecutionMode = item.executionDecision.effectiveMode;
					row.rootMotionPolicy = item.binding.rootMotionPolicy;
					row.updatePolicy = item.binding.updatePolicy;
					row.eventPolicy = item.binding.eventPolicy;
					row.cullingPolicy = item.binding.cullingPolicy;
					row.status = item.evaluationStatus;
					row.snapshot = item.selectedPublication ? item.selectedPublication->snapshot : nullptr;
					row.streamedClipPayloadsActive = item.selectedPublication ? item.selectedPublication->streamedClipPayloadsActive
																			  : item.binding.streamedSegmentPayloads != nullptr;
					row.active = true;
					row.safeFallback = item.safePose;
					if(row.snapshot && row.snapshot->pose) {
						sidecars->events.insert(sidecars->events.end(), row.snapshot->pose->Events().begin(),
												row.snapshot->pose->Events().end());
						if(row.snapshot->pose->RootMotionDelta())
							sidecars->rootMotion.push_back(*row.snapshot->pose->RootMotionDelta());
					}
					if(item.gpuLowering) {
						AnimationSpecialistRecord gpu;
						gpu.key = {row.key.slot, row.key.lifetimeGeneration, session};
						gpu.authorityGeneration = candidateGeneration;
						gpu.kind = 1U;
						gpu.status = AnimationStatus::Ok;
						sidecars->gpuCandidates.push_back(gpu);
					}
				}
				if(!retainRow) {
					pageRetainsPrior[row.key.slot / kAnimationAuthorityPageCapacity128] = false;
				}
			}
			if(!row.IsValid())
				return false;
			mutablePages[row.key.slot / kAnimationAuthorityPageCapacity128]->rows.push_back(std::move(row));
		}

		return true;
	}

	void AnimationSystem::Impl::SealAnimationAuthorityStage(std::uint64_t runtimeSessionEpoch, std::uint64_t frameDeclarationEpoch,
															TerraEngine::Scheduling::JobGraphContext& context) {

		TERRAENGINE_PROFILE_SCOPE("Animation: Seal Authority Candidate", TerraEngine::Profiling::ProfilerModuleId::CpuEngine);
		if(context.IsCancellationRequested()) {
			this->RequestFrameCancellation();
			return;
		}
		if(!this->runtimeActive.load(std::memory_order_acquire) ||
		   this->runtimeSessionEpoch.load(std::memory_order_acquire) != runtimeSessionEpoch ||
		   this->frameDeclarationEpoch.load(std::memory_order_acquire) != frameDeclarationEpoch)
			return;
		// Sealing performs all allocations and deterministic merging on a worker.
		// The following protected workload can therefore exchange only one root.
		this->frameInputs.commandFrameFailurePhase = "SealAnimationAuthority";
		if(this->SealAuthorityFrameCandidate())
			this->frameInputs.commandFrameFailurePhase = nullptr;
	}

} // namespace TerraEngine::Animation
