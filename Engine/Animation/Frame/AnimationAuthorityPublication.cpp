#include "Animation/Frame/AnimationFrameRuntime.h"

namespace TerraEngine::Animation {
	bool AnimationSystem::Impl::CommitPublishedEvaluationCacheEntries(std::size_t animatorCount, std::uint64_t frameEpoch,
																	  std::uint64_t authorityGeneration,
																	  std::uint64_t sessionGeneration) noexcept {
		const PublishedAuthorityCommitToken token = authorityState.publishedAuthorityCommit;
		if(!token.ready || authorityGeneration == 0U || token.authorityGeneration != authorityGeneration ||
		   token.animatorCount != animatorCount || token.frameEpoch != frameEpoch || token.sessionGeneration != sessionGeneration ||
		   token.sceneLifecycleGeneration != frameInputs.frameLifecycleGenerations.Generation(AnimationLifecycleBoundary::Scene) ||
		   !runtimeActive.load(std::memory_order_acquire) || runtimeRecords.load(std::memory_order_acquire) != token.nextRuntimeRoot ||
		   compactRuntimeStates.load(std::memory_order_acquire) != token.nextCompactRuntimeRoot ||
		   authorityPublisher.Load() != authorityState.sealedAuthorityCandidate.frame || !token.expectedRuntimeRoot ||
		   !token.nextCompactRuntimeRoot || !token.specialistSidecars) {
			return false;
		}

		// The protected publisher exchanged the complete compact scalar root beside
		// authority. Validate its rows here without writing through scattered broad
		// runtime records; any mismatch rejects optional follow-up cache work.
		for(const PreparedCompactRuntimeCommit& update : token.specialistSidecars->compactRuntime) {
			const CompactRuntimeState* publishedState = FindCompactRuntimeState(token.nextCompactRuntimeRoot, update.handle);
			const RuntimeRecord* publishedBroadRecord = update.handle.IsValid() && update.handle.slotIndex < token.nextRuntimeRoot->size()
															? token.nextRuntimeRoot->at(update.handle.slotIndex).get()
															: nullptr;
			const bool compactStatePublished = publishedState && publishedState->poseGeneration == update.poseGeneration &&
											   publishedState->stateGeneration == update.stateGeneration &&
											   publishedState->executionModeGeneration == update.executionModeGeneration;
			const bool broadStatePublished = publishedBroadRecord && publishedBroadRecord != update.runtimeRecord &&
											 publishedBroadRecord->handle == update.handle &&
											 publishedBroadRecord->poseGeneration == update.poseGeneration &&
											 publishedBroadRecord->stateGeneration == update.stateGeneration &&
											 publishedBroadRecord->executionModeGeneration == update.executionModeGeneration;
			if(!update.runtimeRecord || !update.handle.IsValid() || update.handle.slotIndex >= token.expectedRuntimeRoot->size() ||
			   token.expectedRuntimeRoot->at(update.handle.slotIndex).get() != update.runtimeRecord ||
			   update.runtimeRecord->handle != update.handle || update.poseGeneration == 0U || update.stateGeneration == 0U ||
			   update.executionModeGeneration == 0U || (!compactStatePublished && !broadStatePublished))
				return false;
		}

		// The accepted authority/root identities above are the transaction
		// boundary. Cache insertion can now traverse the prepared rows on a worker
		// without making rejection observable and without extending the protected
		// publication lock. Each Store owns its internal synchronization; a failed
		// optional insertion remains an ordinary miss and cannot change authority.
		for(const std::size_t inputIndex : token.specialistSidecars->evaluationCacheWorkIndices) {
			FrameWorkItem* item = CompleteWorkForInputIndex(inputIndex);
			if(item == nullptr || !item->selectedPublication || !item->selectedPublication->snapshot)
				continue;
			AnimationEvaluationCacheKey storedKey = item->cacheKey;
			storedKey.poseGeneration = item->selectedPublication->snapshot->publicationGeneration;
			AnimationEvaluationCacheValue cacheValue;
			cacheValue.sourceFrameGeneration = item->candidateAuthorityPublicationGeneration;
			cacheValue.publicationGeneration = item->selectedPublication->authorityPublicationGeneration;
			cacheValue.poseToken = item->selectedPublication->snapshot->publicationGeneration;
			(void)persistentReuse.evaluationCache.Store(storedKey, cacheValue);
		}
		return true;
	}

	bool AnimationSystem::Impl::ApplySealedSpecialistSidecars(PublishedAuthorityCommitToken& token) noexcept {
		if(!token.ready || token.authorityGeneration == 0U || !token.specialistSidecars ||
		   runtimeRecords.load(std::memory_order_acquire) != token.nextRuntimeRoot)
			return false;
		SealedSpecialistSidecarBatch& sidecars = *token.specialistSidecars;
		// Visual workers produce only copied motor scales. The next protected
		// PhysicsCommandCommit owns provider mutation, preserving one Jolt authority.
		for(PreparedRagdollPublication& publication : sidecars.ragdoll) {
			RuntimeRecord* record = publication.runtimeRecord.get();
			if(record == nullptr)
				continue;
			record->ragdollMotorDriveCount = publication.motorDriveCount;
			if(publication.motorRequest) {
				record->ragdollMotorStatus =
					Physics::PhysicsSystem::Instance().SubmitRagdollPhysicsRequest(std::move(*publication.motorRequest));
			}
			if(publication.deactivateRequest) {
				if(record->ragdollLifecycle == RagdollLifecycleState::Recovering) {
					const auto bindings = rootControllerBindings.load(std::memory_order_acquire);
					const auto binding = bindings ? std::lower_bound(bindings->active.begin(), bindings->active.end(), record->handle,
																	 [](const AnimationRootControllerBinding& row,
																		AnimatorInstanceHandle handle) { return row.animator < handle; })
												  : std::vector<AnimationRootControllerBinding>::const_iterator{};
					if(bindings && binding != bindings->active.end() && binding->animator == record->handle) {
						// A bound controller needs a durable Physics recovery ticket;
						// otherwise keep the ragdoll Recovering and retry next frame.
						if(binding->bindingValidityGuard &&
						   binding->bindingValidityGuard->Validate() != Physics::CharacterMotionValidityReason::Valid)
							continue;
						const auto lease = Physics::PhysicsSystem::Instance().ReadCharacterMotionLease(binding->controller);
						const auto sourcePose = Physics::PhysicsSystem::Instance().LatestRagdollPhysicsPoseAtOrAfter(
							record->ragdollId, record->ragdollGeneration);
						const auto characterPacket = Physics::PhysicsSystem::Instance().LatestCharacterStatePacket();
						if(!lease || lease->state != Physics::CharacterMotionLeaseState::ExternalExclusive ||
						   lease->externalOwnerId != RagdollExternalOwnerId(record->handle) || !sourcePose ||
						   !sourcePose->completeCurrentStep || !characterPacket || !characterPacket->states)
							continue;
						const auto character = std::find_if(characterPacket->states->begin(), characterPacket->states->end(),
															[binding](const Physics::PhysicsCharacterState& state) {
																return state.world == binding->controller.world &&
																	   state.handle == binding->controller.character &&
																	   state.objectId == binding->controller.objectId &&
																	   state.objectGeneration == binding->controller.objectGeneration &&
																	   state.componentGeneration == binding->controller.componentGeneration;
															});
						if(character == characterPacket->states->end())
							continue;
						const AnimationQuaternion rotation{character->pose.rotation.x, character->pose.rotation.y,
														   character->pose.rotation.z, character->pose.rotation.w};
						const AnimationFloat3 offset = RotateVector(rotation, binding->capturedRelativeTranslation);
						Physics::CharacterExternalRecoveryIntent intent;
						intent.expectedExternalLease = *lease;
						intent.externalOwnerId = lease->externalOwnerId;
						intent.frame = binding->frame;
						intent.alignedControllerPose.position = {record->ragdollRecoveryAlignedRootPosition.x - offset.x,
																 record->ragdollRecoveryAlignedRootPosition.y - offset.y,
																 record->ragdollRecoveryAlignedRootPosition.z - offset.z};
						intent.alignedControllerPose.rotation = character->pose.rotation;
						intent.bindingGeneration = binding->bindingGeneration;
						intent.bindingGuard = binding->bindingValidityGuard;
						if(!intent.alignedControllerPose.IsFinite())
							continue;
						try {
							publication.deactivateRequest->recoveryIntent =
								std::make_shared<const Physics::CharacterExternalRecoveryIntent>(intent);
							publication.deactivateRequest->recoverySourcePose = sourcePose;
						} catch(const std::bad_alloc&) {
							continue;
						}
					}
				}
				const RagdollGenerationStamp generation = publication.deactivateRequest->generation;
				const std::uint64_t correlationId = publication.deactivateRequest->correlationId;
				const bool externalRecovery = publication.deactivateRequest->recoveryIntent != nullptr;
				const AnimationStatus queued =
					Physics::PhysicsSystem::Instance().SubmitRagdollPhysicsRequest(std::move(*publication.deactivateRequest));
				if(queued == AnimationStatus::Queued) {
					record->pendingRagdollPreviousGeneration = record->ragdollGeneration;
					record->pendingRagdollPreviousLifecycle = record->ragdollLifecycle;
					record->pendingRagdollPreviousProfileId = record->activeRagdollProfileId;
					record->pendingRagdollPreviousBlendWeight = record->ragdollBlendWeight;
					record->ragdollGeneration = generation;
					record->ragdollLifecycle = RagdollLifecycleState::TearingDown;
					record->pendingRagdollCorrelationId = correlationId;
					record->pendingRagdollOperation = NativeAnimationRagdollOperation::Deactivate;
					record->pendingRagdollExternalRecovery = externalRecovery;
				}
			}
		}

		// Procedural observations are diagnostic side publications. They become
		// visible only after the pose authority batch commits, so a result can
		// never advertise a generation whose pose was rejected during preflight.
		for(const PreparedProceduralPublication& publication : sidecars.procedural) {
			(void)proceduralCommands->PublishResults(publication.animator, publication.results, *instances);
		}

		// An active retarget publishes the evaluator's measured result, not merely
		// the earlier activation acknowledgement. This copied diagnostic follows the
		// committed pose and is therefore safe for managed/editor observations.
		if(advancedCommands != nullptr) {
			for(PreparedRetargetPublication& publication : sidecars.retarget) {
				(void)advancedCommands->Publish(std::move(publication.snapshot));
			}
		}

		// The protected publisher already exchanged the complete runtime root before
		// this sparse packet is consumed. Reused/held generations therefore preserve
		// the exact prior attachment publication and never relabel it as fresh work.
		AnimationAttachmentSnapshotPublisher& attachmentPublisher = AnimationAttachmentSnapshotPublisher::Instance();
		for(PreparedAttachmentPublication& publication : sidecars.attachments) {
			const bool attachmentPublished = publication.snapshot && attachmentPublisher.Publish(std::move(*publication.snapshot));
			// GPU-only, stale-source, or failed captures must retire an older socket
			// generation so copied gameplay reads cannot silently mix publications.
			if(!attachmentPublished) {
				attachmentPublisher.RetireSnapshot(publication.animator);
			}
		}

		// Route every copied root interval only after the complete pose set commits.
		// Explicit gameplay requests have deterministic precedence over authored
		// automatic policy. Both pass through the same reservation ledger, so a
		// later request for the same mutating generation is rejected, never applied
		// twice. Worker submissions remain copied until this protected loop.
		const auto rootBindings = rootControllerBindings.load(std::memory_order_acquire);
		const auto exclusions = ragdollExclusions.load(std::memory_order_acquire);
		if(rootBindings && exclusions && !exclusions->entries.empty()) {
			// A ragdoll Activate accepted after Locomotion publication still makes
			// the controller physically exclusive before any root interval routes.
			(void)ApplyRagdollControllerExclusions(*rootBindings, *exclusions, Physics::PhysicsSystem::Instance());
		}
		for(const AnimationRootMotionApplicationRequest& request : sidecars.explicitRootMotion) {
			(void)RouteRootMotionApplication(request, ObjectManager::instance(), TerraEngine::Scene::SceneMutationQueue::Instance(),
											 TerraEngine::Physics::PhysicsSystem::Instance(),
											 request.mode == AnimationRootMotionApplicationMode::ApplyToTransform
												 ? AnimationRootMotionSceneVisibility::ImmediateProtectedCommit
												 : AnimationRootMotionSceneVisibility::DeferredQueue,
											 true);
		}

		// The automatic producer uses one monotonically ordered identity, so a mode
		// switch or duplicate publication cannot apply the same generation twice.
		constexpr std::uint32_t kAutomaticRootMotionProducerId = 0xA11A0001U;
		for(PreparedAutomaticRootMotionPublication& publication : sidecars.automaticRootMotion) {
			AnimationRootMotionApplicationRequest& request = publication.request;
			const AnimationRootMotionDelta& delta = request.delta;
			if(automaticRootMotionSequence == std::numeric_limits<std::uint64_t>::max()) {
				// Sequence exhaustion is fail-closed and still observable. Reusing an
				// ordered producer identity could double-apply an earlier publication.
				AnimationRootMotionApplicationResult exhausted;
				exhausted.animator = delta.animator;
				exhausted.status = AnimationStatus::RangeOverflow;
				exhausted.disposition = AnimationRootMotionDisposition::Rejected;
				exhausted.generation = delta.generation;
				exhausted.sourceBeginTick = delta.sourceBeginTick;
				exhausted.sourceEndTick = delta.sourceEndTick;
				exhausted.localTranslation = delta.localTranslation;
				exhausted.localYawRadians = delta.localYawRadians;
				exhausted.ownerObjectId = request.ownerObjectId;
				exhausted.objectGeneration = request.objectGeneration;
				exhausted.componentGeneration = request.componentGeneration;
				exhausted.sourceStateId = delta.sourceStateId;
				exhausted.accumulatedCycles = delta.accumulatedCycles;
				exhausted.layerIndex = delta.layerIndex;
				exhausted.authoredPolicy = delta.policy;
				exhausted.applicationMode = request.mode;
				exhausted.producerId = kAutomaticRootMotionProducerId;
				exhausted.producerSequence = automaticRootMotionSequence;
				exhausted.correlationId = automaticRootMotionSequence;
				exhausted.idempotencyKey = automaticRootMotionSequence;
				exhausted.publicationGeneration = token.authorityGeneration;
				try {
					AnimationRootMotionResultPublisher::Instance().PublishImmediate(std::move(exhausted));
				} catch(...) {
					// Retained diagnostics are optional under memory exhaustion; the
					// protected authority boundary itself must remain non-throwing.
				}
				continue;
			}
			request.producerId = kAutomaticRootMotionProducerId;
			request.producerSequence = ++automaticRootMotionSequence;
			request.correlationId = request.producerSequence;
			request.idempotencyKey = request.producerSequence;
			request.publicationGeneration = token.authorityGeneration;
			(void)RouteRootMotionApplication(request, ObjectManager::instance(), TerraEngine::Scene::SceneMutationQueue::Instance(),
											 TerraEngine::Physics::PhysicsSystem::Instance(),
											 request.mode == AnimationRootMotionApplicationMode::ApplyToTransform
												 ? AnimationRootMotionSceneVisibility::ImmediateProtectedCommit
												 : AnimationRootMotionSceneVisibility::DeferredQueue);
		}
		return true;
	}

	void AnimationSystem::Impl::PublishCommandCompletionsLocked(std::uint64_t publicationGeneration) noexcept {
		if(commands == nullptr || publicationGeneration == 0U ||
		   frameInputs.capturedCommands.size() != frameInputs.capturedCommandStatuses.size())
			return;
		for(std::size_t index = 0U; index < frameInputs.capturedCommands.size(); ++index) {
			const AnimationStatus status = frameInputs.capturedCommandStatuses[index] == AnimationStatus::NotReady
											   ? AnimationStatus::StaleHandle
											   : frameInputs.capturedCommandStatuses[index];
			commands->PublishCompletion(frameInputs.capturedCommands[index], status, frameInputs.frameTick, publicationGeneration);
		}
	}

	void AnimationSystem::Impl::PublishProceduralCommandCompletionsLocked(std::uint64_t publicationGeneration) noexcept {
		if(proceduralCommands == nullptr || publicationGeneration == 0U ||
		   frameInputs.capturedProceduralCommands.size() != frameInputs.capturedProceduralCommandStatuses.size())
			return;
		for(std::size_t index = 0U; index < frameInputs.capturedProceduralCommands.size(); ++index) {
			// A drained request must reach a terminal receipt. NotReady is an
			// internal staging value; if it survives validation, fail closed as a
			// stale generation instead of leaving an un-cancellable orphan.
			const AnimationStatus status = frameInputs.capturedProceduralCommandStatuses[index] == AnimationStatus::NotReady
											   ? AnimationStatus::StaleHandle
											   : frameInputs.capturedProceduralCommandStatuses[index];
			proceduralCommands->PublishCompletion(frameInputs.capturedProceduralCommands[index], status, frameInputs.frameTick,
												  publicationGeneration);
		}
	}

	AnimationStatus AnimationSystem::Impl::BuildRagdollActivationPoseLocked(const RuntimeRecord& record,
																			const NativeAnimationRagdollRequest& request,
																			std::uint64_t publicationGeneration,
																			PhysicsPoseSnapshot& output,
																			RagdollGenerationStamp& generation) const noexcept {
		const std::shared_ptr<const AnimationAuthorityFrame> authority = authorityPublisher.Load();
		const AnimationAuthorityRow* published = authority ? authority->Find(record.handle) : nullptr;
		const Physics::PhysicsWorldHandle world = Physics::PhysicsSystem::Instance().WorldHandle();
		if(!world.IsValid() || request.physicsWorldGeneration != world.worldGeneration) {
			return AnimationStatus::StaleHandle;
		}
		if(!record.binding.ragdollProfile || !record.binding.skeleton ||
		   !MatchesNativeUuid(request.profileAssetId, record.binding.ragdollProfile->assetId.value) ||
		   request.profileGeneration != record.binding.ragdollProfileGeneration ||
		   record.binding.ragdollProfile->runtimeGeneration != record.binding.ragdollProfileGeneration) {
			return AnimationStatus::MissingAsset;
		}
		if(!published || !published->snapshot || !published->snapshot->pose || !published->sourceObjectWorldTransformCaptured ||
		   published->snapshot->pose->ValidateForPublication() != AnimationStatus::Ok) {
			return AnimationStatus::NotReady;
		}
		const auto model = published->snapshot->pose->ModelTransforms();
		if(model.size() != record.binding.jointStableIds.size() || record.binding.ragdollProfile->bodies.empty() ||
		   record.binding.ragdollProfile->bodies.size() > kRagdollSnapshotMaximumBodies) {
			return AnimationStatus::IncompatibleAsset;
		}
		try {
			PhysicsPoseSnapshot candidate;
			generation.worldGeneration = world.worldGeneration;
			generation.profileGeneration = record.binding.ragdollProfileGeneration;
			generation.skeletonGeneration = record.binding.skeletonGeneration;
			generation.publicationGeneration = publicationGeneration;
			// A ragdoll generation names a physics result. The animation frame tick
			// may be far ahead in an editor session, so use Physics' copied clock or
			// the next physics extraction would be rejected as artificially stale.
			generation.fixedStep = Physics::PhysicsSystem::Instance().CurrentFixedTick();
			candidate.generation = generation;
			candidate.completeCurrentStep = true;
			candidate.bodies.reserve(record.binding.ragdollProfile->bodies.size());
			for(const RagdollBodyRecord& body : record.binding.ragdollProfile->bodies) {
				const auto joint =
					std::find(record.binding.jointStableIds.begin(), record.binding.jointStableIds.end(), body.stableJointId);
				if(joint == record.binding.jointStableIds.end()) {
					return AnimationStatus::IncompatibleAsset;
				}
				const std::size_t jointIndex = static_cast<std::size_t>(std::distance(record.binding.jointStableIds.begin(), joint));
				PhysicsPoseBodySnapshot snapshot;
				snapshot.bodyId = body.bodyId;
				snapshot.stableJointId = body.stableJointId;
				// Physics consumes world-space activation. The pose remains immutable;
				// this composition creates a copied provider-neutral handoff record.
				snapshot.modelPose = CombineTransform(published->sourceObjectWorldTransform, model[jointIndex]);
				snapshot.bodyGeneration = 1U;
				candidate.bodies.push_back(snapshot);
			}
			if(ValidatePhysicsPoseSnapshot(candidate, generation) != AnimationStatus::Ok) {
				return AnimationStatus::Rejected;
			}
			// Consecutive-pose velocity is a future history sidecar responsibility.
			// The current hard cut deliberately does not retain a second row in the
			// mutable runtime record.
			const AnimationAuthorityRow* previous = nullptr;
			if(previous && previous->snapshot && previous->snapshot->pose && previous->snapshot->tick > 0 &&
			   previous->snapshot->tick < published->snapshot->tick && previous->skeleton == published->skeleton &&
			   previous->jointStableIds == published->jointStableIds && previous->sourceObjectWorldTransformCaptured &&
			   previous->snapshot->pose->ValidateForPublication() == AnimationStatus::Ok) {
				PhysicsPoseSnapshot priorCandidate;
				priorCandidate.generation = generation;
				priorCandidate.generation.publicationGeneration = previous->authorityPublicationGeneration;
				priorCandidate.generation.fixedStep = std::max<AnimationFixedTick>(1, previous->snapshot->tick);
				priorCandidate.completeCurrentStep = true;
				priorCandidate.bodies.reserve(record.binding.ragdollProfile->bodies.size());
				const auto previousModel = previous->snapshot->pose->ModelTransforms();
				for(const RagdollBodyRecord& body : record.binding.ragdollProfile->bodies) {
					const auto joint = std::find(previous->jointStableIds.begin(), previous->jointStableIds.end(), body.stableJointId);
					if(joint == previous->jointStableIds.end()) {
						return AnimationStatus::IncompatibleAsset;
					}
					const std::size_t jointIndex = static_cast<std::size_t>(std::distance(previous->jointStableIds.begin(), joint));
					if(jointIndex >= previousModel.size()) {
						return AnimationStatus::IncompatibleAsset;
					}
					PhysicsPoseBodySnapshot snapshot;
					snapshot.bodyId = body.bodyId;
					snapshot.stableJointId = body.stableJointId;
					snapshot.modelPose = CombineTransform(previous->sourceObjectWorldTransform, previousModel[jointIndex]);
					snapshot.bodyGeneration = 1U;
					priorCandidate.bodies.push_back(snapshot);
				}
				const float elapsedSeconds = static_cast<float>(published->snapshot->tick - previous->snapshot->tick) /
											 static_cast<float>(kAnimationFixedTicksPerSecond);
				// Pose-history velocity derives from consecutive animation publications,
				// not from physics steps. Keep that local ordering on the animation
				// clock while the outgoing candidate above remains physics-stamped.
				RagdollGenerationStamp motionGeneration = generation;
				motionGeneration.fixedStep = std::max<AnimationFixedTick>(1, published->snapshot->tick);
				PhysicsPoseSnapshot motionCandidate = candidate;
				motionCandidate.generation = motionGeneration;
				RagdollActivationPlan movingPlan;
				const AnimationStatus motionStatus = PrepareRagdollActivation(motionCandidate, priorCandidate, elapsedSeconds,
																			  *record.binding.ragdollProfile, motionGeneration, movingPlan);
				if(motionStatus != AnimationStatus::Ok)
					return motionStatus;
				for(PhysicsPoseBodySnapshot& body : candidate.bodies) {
					const auto motion =
						std::find_if(movingPlan.bodies.begin(), movingPlan.bodies.end(), [&body](const RagdollActivationBody& value) {
							return value.bodyId == body.bodyId && value.stableJointId == body.stableJointId;
						});
					if(motion == movingPlan.bodies.end()) {
						return AnimationStatus::IncompatibleAsset;
					}
					body.linearVelocity = motion->linearVelocity;
					body.angularVelocity = motion->angularVelocity;
				}
			}
			output = std::move(candidate);
			return AnimationStatus::Ok;
		} catch(const std::bad_alloc&) {
			return AnimationStatus::HardLimitExceeded;
		} catch(...) {
			return AnimationStatus::InternalError;
		}
	}

	AnimationStatus AnimationSystem::Impl::BeginRagdollRecoveryLocked(RuntimeRecord& record, const NativeAnimationRagdollRequest& request,
																	  bool& recoveryBlocked) noexcept {
		recoveryBlocked = false;
		if(!record.ragdollCreated || record.ragdollLifecycle != RagdollLifecycleState::Active || !record.ragdollGeneration.IsValid() ||
		   !record.binding.ragdollProfile || !record.binding.skeleton) {
			return AnimationStatus::NotReady;
		}
		if(request.physicsWorldGeneration != record.ragdollGeneration.worldGeneration ||
		   request.profileGeneration != record.binding.ragdollProfileGeneration ||
		   !MatchesNativeUuid(request.profileAssetId, record.binding.ragdollProfile->assetId.value)) {
			return AnimationStatus::StaleHandle;
		}
		const std::shared_ptr<const PhysicsPoseSnapshot> physicsPose =
			Physics::PhysicsSystem::Instance().LatestRagdollPhysicsPoseAtOrAfter(record.ragdollId, record.ragdollGeneration);
		const std::shared_ptr<const AnimationAuthorityFrame> authority = authorityPublisher.Load();
		const AnimationAuthorityRow* published = authority ? authority->Find(record.handle) : nullptr;
		if(!physicsPose || !published || !published->snapshot || !published->snapshot->pose ||
		   !published->sourceObjectWorldTransformCaptured) {
			return AnimationStatus::NotReady;
		}

		try {
			RagdollRecoveryObservation observation;
			observation.completeAnimationPose.generation = physicsPose->generation;
			observation.completeAnimationPose.completeCurrentStep = true;
			observation.completeAnimationPose.bodies.reserve(record.binding.ragdollProfile->bodies.size());
			const auto model = published->snapshot->pose->ModelTransforms();
			for(const RagdollBodyRecord& body : record.binding.ragdollProfile->bodies) {
				const auto joint =
					std::find(record.binding.jointStableIds.begin(), record.binding.jointStableIds.end(), body.stableJointId);
				if(joint == record.binding.jointStableIds.end()) {
					return AnimationStatus::IncompatibleAsset;
				}
				const std::size_t index = static_cast<std::size_t>(std::distance(record.binding.jointStableIds.begin(), joint));
				if(index >= model.size())
					return AnimationStatus::IncompatibleAsset;
				PhysicsPoseBodySnapshot animationBody;
				animationBody.bodyId = body.bodyId;
				animationBody.stableJointId = body.stableJointId;
				animationBody.modelPose = CombineTransform(published->sourceObjectWorldTransform, model[index]);
				animationBody.bodyGeneration = 1U;
				observation.completeAnimationPose.bodies.push_back(animationBody);
			}
			// Existing ABI flags remain fixed-width. Bit zero is a caller-confirmed
			// immutable clearance observation; bits 8..31 carry the observed contact
			// mask. A missing clearance bit truthfully produces SpaceBlocked.
			observation.clearanceConfirmed = (request.flags & 0x1U) != 0U;
			observation.observedContactMask = static_cast<std::uint64_t>(request.flags >> 8U);
			if((request.flags & 0x2U) != 0U) {
				observation.observedPosture = RagdollRecoveryPosture::FaceUp;
			} else if((request.flags & 0x4U) != 0U) {
				observation.observedPosture = RagdollRecoveryPosture::FaceDown;
			}
			RagdollRecoveryRequest recoveryRequest;
			AnimationStatus status = BuildRagdollRecoveryRequest(*record.binding.ragdollProfile, *physicsPose, physicsPose->generation,
																 observation, recoveryRequest);
			if(status != AnimationStatus::Ok)
				return status;
			const RagdollRecoveryResult selected =
				SelectRagdollRecovery(*record.binding.ragdollProfile, *physicsPose, physicsPose->generation, recoveryRequest);
			if(selected.status != AnimationStatus::Ok) {
				record.ragdollRecoveryBlockReason = selected.blockReason;
				recoveryBlocked = selected.blocked;
				return selected.status;
			}

			const auto authored =
				std::find_if(record.binding.ragdollProfile->recovery.begin(), record.binding.ragdollProfile->recovery.end(),
							 [&selected](const RagdollRecoveryRecord& value) { return value.candidateId == selected.candidateId; });
			if(authored == record.binding.ragdollProfile->recovery.end()) {
				return AnimationStatus::IncompatibleAsset;
			}
			const auto clip = std::find_if(record.binding.controllerClips.begin(), record.binding.controllerClips.end(),
										   [&authored](const AnimatorControllerClipBinding& value) {
											   return value.clip && value.clip->assetId == authored->clipAssetId &&
													  value.clipGeneration == authored->clipGeneration;
										   });
			if(clip == record.binding.controllerClips.end() || !clip->clip ||
			   clip->clip->compatibilityFingerprint != record.binding.skeleton->compatibilityFingerprint) {
				return AnimationStatus::MissingAsset;
			}

			AnimationClipView nextView;
			status = nextView.Open(*clip->clip, record.binding.skeleton.get(), clip->residentSegments, clip->clipGeneration,
								   clip->streamedSegmentPayloads);
			if(status != AnimationStatus::Ok)
				return status;
			AnimationEventStream nextEvents;
			status = nextEvents.Configure({clip->clip->assetId, clip->clipGeneration}, clip->clip->durationTicks, clip->clip->events);
			if(status != AnimationStatus::Ok)
				return status;
			AnimatorPlaybackConfiguration configuration = record.playback.Configuration();
			configuration.durationTicks = clip->clip->durationTicks;
			configuration.loop = false;
			configuration.reverse = false;
			AnimatorPlayback nextPlayback;
			status = nextPlayback.Configure(configuration);
			if(status == AnimationStatus::Ok)
				status = nextPlayback.Play();
			if(status != AnimationStatus::Ok)
				return status;
			StateMachineSnapshot nextGraph = record.graphState;
			if(record.binding.evaluationProgram) {
				const auto recoveryState =
					std::find_if(record.binding.evaluationProgram->states.begin(), record.binding.evaluationProgram->states.end(),
								 [&clip](const EvaluationState& value) {
									 return value.nodeOpcode == EvaluationOpcode::SampleClip && value.nodeId == clip->nodeStableId;
								 });
				if(recoveryState != record.binding.evaluationProgram->states.end()) {
					nextGraph = {};
					nextGraph.activeStateId = recoveryState->stableId;
					nextGraph.stateEnteredTick = frameInputs.frameTick;
					nextGraph.lastTick = frameInputs.frameTick;
				}
			}
			if(record.stateGeneration == std::numeric_limits<std::uint64_t>::max()) {
				return AnimationStatus::RangeOverflow;
			}

			// All allocation and compatibility checks completed. Publish the clip,
			// selector result, and lifecycle edge together at this protected boundary.
			record.binding.clip = clip->clip;
			record.binding.clipGeneration = clip->clipGeneration;
			record.binding.clipDeterministicContentFingerprint = clip->deterministicContentFingerprint;
			record.binding.residentSegments = clip->residentSegments;
			record.binding.streamedSegmentPayloads = clip->streamedSegmentPayloads;
			record.clipView = std::move(nextView);
			record.eventStream = std::move(nextEvents);
			record.playback = nextPlayback;
			record.graphState = std::move(nextGraph);
			record.binding.stateId = record.graphState.activeStateId != 0U ? record.graphState.activeStateId : record.binding.stateId;
			++record.stateGeneration;
			record.ragdollGeneration = physicsPose->generation;
			record.ragdollLifecycle = RagdollLifecycleState::Recovering;
			record.ragdollLifecycleStartTick = frameInputs.frameTick;
			record.ragdollBlendWeight = request.blendWeight;
			record.ragdollRecoveryCandidateId = selected.candidateId;
			record.ragdollRecoveryBlockReason = RagdollRecoveryBlockReason::None;
			record.ragdollRecoveryAlignedRootPosition = selected.alignedRootPosition;
			record.ragdollRecoveryDurationSeconds = selected.transitionDurationSeconds;
			return AnimationStatus::Ok;
		} catch(const std::bad_alloc&) {
			return AnimationStatus::HardLimitExceeded;
		} catch(...) {
			return AnimationStatus::InternalError;
		}
	}

	NativeAnimationRagdollSnapshot AnimationSystem::Impl::MakeRagdollSnapshot(const RuntimeRecord& record, NativeAnimationStatus status,
																			  AnimationFixedTick observationTick,
																			  std::uint64_t publicationGeneration,
																			  bool recoveryBlocked) const noexcept {
		NativeAnimationRagdollSnapshot snapshot{};
		snapshot.header = MakeNativeHeader(NativeAnimationRecordKind::RagdollSnapshot, sizeof(snapshot));
		snapshot.status = status;
		snapshot.animator = {record.handle.slotIndex, record.handle.generation};
		snapshot.profileAssetId = record.activeRagdollProfileId;
		snapshot.profileGeneration = record.ragdollGeneration.profileGeneration;
		snapshot.physicsWorldGeneration = record.ragdollGeneration.worldGeneration;
		snapshot.lifecycle = static_cast<std::uint32_t>(record.ragdollLifecycle);
		snapshot.bodyCount = record.binding.ragdollProfile ? static_cast<std::uint32_t>(record.binding.ragdollProfile->bodies.size()) : 0U;
		snapshot.blendWeight = record.ragdollBlendWeight;
		// Values remain boolean-compatible for ABI-v1 managed callers, while native
		// diagnostics can distinguish the exact recovery rejection reason.
		snapshot.recoveryBlocked =
			recoveryBlocked ? std::max<std::uint32_t>(1U, static_cast<std::uint32_t>(record.ragdollRecoveryBlockReason)) : 0U;
		snapshot.reserved =
			static_cast<std::uint16_t>(std::min<std::uint32_t>(record.ragdollMotorDriveCount, std::numeric_limits<std::uint16_t>::max()));
		snapshot.observationTick = observationTick;
		snapshot.publicationGeneration = publicationGeneration;
		return snapshot;
	}

	bool AnimationSystem::Impl::HasRagdollExternalLeaseLocked(const RuntimeRecord& record) const noexcept {
		const auto bindings = rootControllerBindings.load(std::memory_order_acquire);
		if(!bindings)
			return false;
		const auto found = std::lower_bound(
			bindings->active.begin(), bindings->active.end(), record.handle,
			[](const AnimationRootControllerBinding& row, AnimatorInstanceHandle handle) { return row.animator < handle; });
		if(found == bindings->active.end() || found->animator != record.handle)
			return false;
		const auto lease = Physics::PhysicsSystem::Instance().ReadCharacterMotionLease(found->controller);
		return lease && lease->state == Physics::CharacterMotionLeaseState::ExternalExclusive &&
			   lease->externalOwnerId == RagdollExternalOwnerId(record.handle);
	}

	void AnimationSystem::Impl::SettleFailedFrameCommandsLocked(AnimationStatus status, const char* fallbackPhase) noexcept {
		if(commands == nullptr)
			return;
		std::size_t unsettled = 0;
		for(const auto& command : frameInputs.capturedCommands) {
			const auto receipt = commands->ReadReceipt(command.producerId, command.producerSequence, command.correlationId);
			if(receipt.status != AnimationStatus::Queued && receipt.status != AnimationStatus::NotReady)
				continue;
			commands->PublishCompletion(command, status, frameInputs.frameTick, 0);
			++unsettled;
		}
		if(!unsettled)
			return;
		try {
			// One bounded record survives later successful frames; diagnostic
			// summaries only describe the latest accepted authority publication.
			Logger::Instance().Log(
				LogLevel::Warning, __FILE__, __LINE__, __func__,
				"Animation command frame failed phase=" +
					std::string(frameInputs.commandFrameFailurePhase ? frameInputs.commandFrameFailurePhase : fallbackPhase) +
					" status=" + std::string(ToString(status)) + " commandCount=" + std::to_string(unsettled) +
					" planReason=" + std::string(ToString(evaluationState.workPlanRejectionReason)) +
					" planned=" + std::to_string(evaluationState.plannedAnimatorCount.load()) +
					" evaluated=" + std::to_string(evaluationState.evaluatedAnimatorCount.load()) +
					" producer=" + std::to_string(frameInputs.capturedCommands.front().producerId) +
					" sequence=" + std::to_string(frameInputs.capturedCommands.front().producerSequence));
		} catch(...) { /* Logging cannot alter failed-batch settlement. */
		}
	}

	void AnimationSystem::Impl::PublishCommittedOrdinaryCommandsLocked() noexcept {
		// ready is set only after the actual protected authority/runtime exchange.
		// It proves a historical commit even if later specialist work is skipped
		// or the next frame changes lifecycle. Never infer it from a planned token.
		const auto& token = authorityState.publishedAuthorityCommit;
		if(token.ready && token.authorityGeneration != 0)
			PublishCommandCompletionsLocked(token.authorityGeneration);
	}

	AnimationStatus AnimationSystem::Impl::AbortFailedRagdollAdmissionLocked(RuntimeRecord& record) noexcept {
		// A terminal failed Activate is not itself a release instruction. This
		// method is reached only for a later explicit Deactivate/RequestRecovery.
		if(record.failedRagdollActivateCorrelationId == 0U || record.failedRagdollActivatePublicationGeneration == 0U)
			return AnimationStatus::NotReady;
		const auto bindings = rootControllerBindings.load(std::memory_order_acquire);
		if(!bindings)
			return AnimationStatus::NotReady;
		const auto found = std::lower_bound(
			bindings->active.begin(), bindings->active.end(), record.handle,
			[](const AnimationRootControllerBinding& row, AnimatorInstanceHandle handle) { return row.animator < handle; });
		if(found == bindings->active.end() || found->animator != record.handle || !found->bindingValidityGuard ||
		   found->bindingValidityGuard->Validate() != Physics::CharacterMotionValidityReason::Valid)
			return AnimationStatus::StaleHandle;
		const auto lease = Physics::PhysicsSystem::Instance().ReadCharacterMotionLease(found->controller);
		if(!lease || lease->state != Physics::CharacterMotionLeaseState::ExternalExclusive ||
		   lease->externalOwnerId != RagdollExternalOwnerId(record.handle))
			return AnimationStatus::StaleHandle;
		Physics::CharacterFailedRagdollAdmissionProof proof;
		proof.expectedExternalLease = *lease;
		proof.externalOwnerId = lease->externalOwnerId;
		proof.ragdollId = record.ragdollId;
		proof.failedActivationCorrelationId = record.failedRagdollActivateCorrelationId;
		proof.failedActivationGeneration = record.failedRagdollActivateGeneration;
		proof.failurePublicationGeneration = record.failedRagdollActivatePublicationGeneration;
		proof.failureFrameTick = record.failedRagdollActivateFrameTick;
		proof.frame = found->frame;
		proof.bindingGeneration = found->bindingGeneration;
		proof.bindingGuard = found->bindingValidityGuard;
		const auto released = Physics::PhysicsSystem::Instance().AbortFailedExternalRagdollAdmission(proof);
		if(!released.accepted)
			return AnimationStatus::NotReady;
		record.failedRagdollActivateCorrelationId = 0U;
		record.failedRagdollActivateGeneration = {};
		record.failedRagdollActivatePublicationGeneration = 0U;
		record.failedRagdollActivateFrameTick = 0;
		return AnimationStatus::Ok;
	}

	AnimationStatus AnimationSystem::Impl::RefreshExternalRagdollRecoveryLocked(RuntimeRecord& record) noexcept {
		// A stale recovery ticket preserves the old external owner. Only an
		// explicit new RequestRecovery may replace its inert target pose.
		if(!record.pendingExternalRecoveryToken || !record.pendingExternalRecoveryNeedsRefresh)
			return AnimationStatus::NotReady;
		const auto bindings = rootControllerBindings.load(std::memory_order_acquire);
		if(!bindings)
			return AnimationStatus::NotReady;
		const auto found = std::lower_bound(
			bindings->active.begin(), bindings->active.end(), record.handle,
			[](const AnimationRootControllerBinding& row, AnimatorInstanceHandle handle) { return row.animator < handle; });
		if(found == bindings->active.end() || found->animator != record.handle || !found->bindingValidityGuard ||
		   found->bindingValidityGuard->Validate() != Physics::CharacterMotionValidityReason::Valid)
			return AnimationStatus::StaleHandle;
		const auto lease = Physics::PhysicsSystem::Instance().ReadCharacterMotionLease(found->controller);
		const auto characterPacket = Physics::PhysicsSystem::Instance().LatestCharacterStatePacket();
		if(!lease || lease->state != Physics::CharacterMotionLeaseState::ExternalExclusive ||
		   lease->externalOwnerId != RagdollExternalOwnerId(record.handle) || !characterPacket || !characterPacket->states)
			return AnimationStatus::StaleHandle;
		const auto character = std::find_if(
			characterPacket->states->begin(), characterPacket->states->end(), [found](const Physics::PhysicsCharacterState& state) {
				return state.world == found->controller.world && state.handle == found->controller.character &&
					   state.objectId == found->controller.objectId && state.objectGeneration == found->controller.objectGeneration &&
					   state.componentGeneration == found->controller.componentGeneration;
			});
		if(character == characterPacket->states->end())
			return AnimationStatus::NotReady;
		const AnimationQuaternion rotation{character->pose.rotation.x, character->pose.rotation.y, character->pose.rotation.z,
										   character->pose.rotation.w};
		const AnimationFloat3 offset = RotateVector(rotation, found->capturedRelativeTranslation);
		Physics::CharacterExternalRecoveryIntent fresh;
		fresh.expectedExternalLease = *lease;
		fresh.externalOwnerId = lease->externalOwnerId;
		fresh.frame = found->frame;
		fresh.alignedControllerPose.position = {record.ragdollRecoveryAlignedRootPosition.x - offset.x,
												record.ragdollRecoveryAlignedRootPosition.y - offset.y,
												record.ragdollRecoveryAlignedRootPosition.z - offset.z};
		fresh.alignedControllerPose.rotation = character->pose.rotation;
		fresh.bindingGeneration = found->bindingGeneration;
		fresh.bindingGuard = found->bindingValidityGuard;
		if(!fresh.alignedControllerPose.IsFinite())
			return AnimationStatus::InvalidArgument;
		try {
			const auto immutable = std::make_shared<const Physics::CharacterExternalRecoveryIntent>(fresh);
			if(Physics::PhysicsSystem::Instance().RefreshExternalCharacterRecovery(*record.pendingExternalRecoveryToken, immutable) !=
			   Physics::CharacterExternalRecoveryError::None)
				return AnimationStatus::NotReady;
		} catch(const std::bad_alloc&) {
			return AnimationStatus::HardLimitExceeded;
		}
		record.pendingExternalRecoveryNeedsRefresh = false;
		const auto completed = Physics::PhysicsSystem::Instance().CompleteExternalCharacterRecovery(*record.pendingExternalRecoveryToken);
		if(!completed.accepted) {
			record.pendingExternalRecoveryNeedsRefresh = completed.error != Physics::CharacterExternalRecoveryError::ProviderRejected &&
														 completed.error != Physics::CharacterExternalRecoveryError::CapacityExceeded;
			return AnimationStatus::Queued;
		}
		record.pendingExternalRecoveryToken.reset();
		record.ragdollLifecycle = RagdollLifecycleState::Inactive;
		record.ragdollBlendWeight = 0.0F;
		record.ragdollRecoveryCandidateId = 0U;
		record.ragdollRecoveryBlockReason = RagdollRecoveryBlockReason::None;
		record.ragdollRecoveryDurationSeconds = 0.0F;
		return AnimationStatus::Ok;
	}

	void AnimationSystem::Impl::PublishRagdollExclusionForRecordLocked(const RuntimeRecord& record,
																	   std::uint64_t completedCorrelation) noexcept {
		try {
			const auto current = ragdollExclusions.load(std::memory_order_acquire);
			if(!current && record.ragdollLifecycle == RagdollLifecycleState::Inactive && record.failedRagdollActivateCorrelationId == 0U)
				return;
			if(current && current->publicationGeneration == std::numeric_limits<std::uint64_t>::max())
				return;
			auto next = std::make_shared<AnimationRagdollExclusionSnapshot>();
			if(current)
				next->entries = current->entries;
			const auto found =
				std::find_if(next->entries.begin(), next->entries.end(),
							 [handle = record.handle](const AnimationRagdollExclusion& entry) { return entry.animator == handle; });
			// A newer queued Activate must not be erased by an older completion.
			if(found != next->entries.end() && found->state == AnimationRagdollExclusionState::PendingActivate &&
			   found->correlationId != completedCorrelation)
				return;
			if(record.ragdollLifecycle == RagdollLifecycleState::Inactive && record.failedRagdollActivateCorrelationId == 0U) {
				if(found == next->entries.end())
					return;
				next->entries.erase(found);
			} else {
				const AnimationRagdollExclusionState state = record.ragdollLifecycle == RagdollLifecycleState::Creating
																 ? AnimationRagdollExclusionState::PendingActivate
																 : AnimationRagdollExclusionState::Active;
				if(found != next->entries.end()) {
					found->state = state;
					found->correlationId = record.pendingRagdollCorrelationId != 0U			 ? record.pendingRagdollCorrelationId
										   : record.failedRagdollActivateCorrelationId != 0U ? record.failedRagdollActivateCorrelationId
																							 : completedCorrelation;
					found->externalOwnerId = RagdollExternalOwnerId(record.handle);
				} else {
					if(next->entries.size() >= 4096U)
						return;
					next->entries.push_back(
						{record.handle, state,
						 record.failedRagdollActivateCorrelationId != 0U ? record.failedRagdollActivateCorrelationId : completedCorrelation,
						 RagdollExternalOwnerId(record.handle)});
				}
			}
			next->publicationGeneration = current ? current->publicationGeneration + 1U : 1U;
			ragdollExclusions.store(std::move(next), std::memory_order_release);
		} catch(const std::bad_alloc&) {
			// Preserve the previous exclusion on memory pressure. Movement fails
			// closed until a later protected reconciliation succeeds.
		}
	}

	void AnimationSystem::Impl::RejectDrainedFrameCommandsLocked() noexcept {
		// A controller reload after Capture invalidates the entire sealed frame.
		// Every queue was already drained, so finish each copied request with a
		// terminal stale receipt while retaining the prior authority/runtime root.
		for(const AnimationCommand& command : frameInputs.capturedCommands)
			commands->PublishCompletion(command, AnimationStatus::StaleHandle, frameInputs.frameTick, 0U);
		for(const NativeAnimationProceduralGoalCommand& command : frameInputs.capturedProceduralCommands)
			proceduralCommands->PublishCompletion(command, AnimationStatus::StaleHandle, frameInputs.frameTick, 0U);
		if(advancedCommands == nullptr)
			return;
		for(const NativeAnimationRetargetRequest& request : frameInputs.capturedRetargetRequests) {
			NativeAnimationRetargetSnapshot result{};
			result.header = MakeNativeHeader(NativeAnimationRecordKind::RetargetSnapshot, sizeof(result));
			result.status = NativeAnimationStatus::StaleHandle;
			result.animator = request.animator;
			result.profileAssetId = request.profileAssetId;
			result.profileGeneration = request.profileGeneration;
			result.observationTick = frameInputs.frameTick;
			(void)advancedCommands->Publish(result);
		}
		for(const NativeAnimationMotionMatchRequest& request : frameInputs.capturedMotionMatchRequests) {
			NativeAnimationMotionMatchSnapshot result{};
			result.header = MakeNativeHeader(NativeAnimationRecordKind::MotionMatchSnapshot, sizeof(result));
			result.status = NativeAnimationStatus::StaleHandle;
			result.animator = request.animator;
			result.databaseAssetId = request.databaseAssetId;
			result.databaseGeneration = request.databaseGeneration;
			result.queryId = request.queryId;
			result.observationTick = frameInputs.frameTick;
			(void)advancedCommands->Publish(result);
		}
		for(const NativeAnimationMotionMatchQueryRequestV2& request : frameInputs.capturedMotionMatchQueriesV2) {
			NativeAnimationMotionMatchResultV2 result{};
			result.header = MakeNativeHeader(NativeAnimationRecordKind::MotionMatchResultV2, sizeof(result));
			result.status = NativeAnimationStatus::StaleHandle;
			result.gpuStatus = NativeAnimationStatus::StaleHandle;
			result.animator = request.animator;
			result.databaseAssetId = request.databaseAssetId;
			result.databaseGeneration = request.databaseGeneration;
			result.schemaFingerprint = request.schemaFingerprint;
			result.queryId = request.queryId;
			result.observationTick = frameInputs.frameTick;
			result.populatedFields = static_cast<std::uint32_t>(NativeAnimationMotionMatchResultField::Identity);
			(void)advancedCommands->Publish(result);
		}
		for(const NativeAnimationRagdollRequest& request : frameInputs.capturedRagdollRequests) {
			NativeAnimationRagdollSnapshot result{};
			result.header = MakeNativeHeader(NativeAnimationRecordKind::RagdollSnapshot, sizeof(result));
			result.status = NativeAnimationStatus::StaleHandle;
			result.animator = request.animator;
			result.profileAssetId = request.profileAssetId;
			result.profileGeneration = request.profileGeneration;
			result.physicsWorldGeneration = request.physicsWorldGeneration;
			result.observationTick = frameInputs.frameTick;
			(void)advancedCommands->Publish(result);
			DropStalePendingRagdollExclusionLocked({request.animator.slotIndex, request.animator.generation}, request.correlationId);
		}
		for(const NativeAnimationDeterminismRequest& request : frameInputs.capturedDeterminismRequests) {
			NativeAnimationDeterminismSnapshot result{};
			result.header = MakeNativeHeader(NativeAnimationRecordKind::DeterminismSnapshot, sizeof(result));
			result.status = NativeAnimationStatus::StaleHandle;
			result.animator = request.animator;
			result.tick = frameInputs.frameTick;
			(void)advancedCommands->Publish(result);
		}
	}

	void AnimationSystem::Impl::DropStalePendingRagdollExclusionLocked(AnimatorInstanceHandle animator,
																	   std::uint64_t correlationId) noexcept {
		try {
			const auto current = ragdollExclusions.load(std::memory_order_acquire);
			if(!current || current->publicationGeneration == std::numeric_limits<std::uint64_t>::max())
				return;
			const auto found = std::find_if(current->entries.begin(), current->entries.end(),
											[animator, correlationId](const AnimationRagdollExclusion& entry) {
												return entry.animator == animator && entry.correlationId == correlationId &&
													   entry.state == AnimationRagdollExclusionState::PendingActivate;
											});
			if(found == current->entries.end())
				return;
			auto next = std::make_shared<AnimationRagdollExclusionSnapshot>(*current);
			next->entries.erase(next->entries.begin() + static_cast<std::ptrdiff_t>(std::distance(current->entries.begin(), found)));
			next->publicationGeneration = current->publicationGeneration + 1U;
			ragdollExclusions.store(std::move(next), std::memory_order_release);
		} catch(const std::bad_alloc&) {
			// Keeping a stale exclusion is safer than accidentally restoring motion.
		}
	}

	void AnimationSystem::Impl::SynchronizeRagdollResultsLocked(std::uint64_t publicationGeneration) noexcept {
		if(advancedCommands == nullptr)
			return;
		// Provider Deactivate may have succeeded while the final character
		// teleport/unlease was temporarily unavailable. The Physics ticket
		// retains its immutable pose and lease proof across render frames.
		const auto retryRoot = runtimeRecords.load(std::memory_order_acquire);
		if(retryRoot)
			retryRoot->ForEach([this, publicationGeneration](const std::shared_ptr<RuntimeRecord>& owned) {
				if(!owned || !owned->pendingExternalRecoveryToken || owned->pendingExternalRecoveryNeedsRefresh)
					return;
				const auto completed =
					Physics::PhysicsSystem::Instance().CompleteExternalCharacterRecovery(*owned->pendingExternalRecoveryToken);
				if(completed.accepted) {
					owned->pendingExternalRecoveryToken.reset();
					owned->ragdollLifecycle = RagdollLifecycleState::Inactive;
					owned->ragdollBlendWeight = 0.0F;
					owned->ragdollRecoveryCandidateId = 0U;
					owned->ragdollRecoveryBlockReason = RagdollRecoveryBlockReason::None;
					owned->ragdollRecoveryDurationSeconds = 0.0F;
					PublishRagdollExclusionForRecordLocked(*owned, 0U);
					(void)advancedCommands->Publish(
						MakeRagdollSnapshot(*owned, NativeAnimationStatus::Ok, frameInputs.frameTick, publicationGeneration));
				} else if(completed.error != Physics::CharacterExternalRecoveryError::ProviderRejected &&
						  completed.error != Physics::CharacterExternalRecoveryError::CapacityExceeded) {
					owned->pendingExternalRecoveryNeedsRefresh = true;
				}
			});
		const std::shared_ptr<const RagdollPhysicsResultSnapshot> results =
			Physics::PhysicsSystem::Instance().LatestRagdollPhysicsResults();
		if(!results || !results->results)
			return;
		for(const RagdollPhysicsResult& result : *results->results) {
			const std::shared_ptr<RuntimeRecordRoot> root = runtimeRecords.load(std::memory_order_acquire);
			if(root == nullptr)
				continue;
			root->ForEach([&](const std::shared_ptr<RuntimeRecord>& owned) {
				if(!owned || owned->ragdollId != result.ragdollId || owned->pendingRagdollCorrelationId == 0U ||
				   owned->pendingRagdollCorrelationId != result.correlationId)
					return;
				RuntimeRecord& record = *owned;
				AnimationStatus snapshotStatus = result.status;
				if(result.status == AnimationStatus::Ok) {
					record.ragdollGeneration = result.generation;
					switch(record.pendingRagdollOperation) {
					case NativeAnimationRagdollOperation::Activate:
						record.ragdollCreated = true;
						record.failedRagdollActivateCorrelationId = 0U;
						record.failedRagdollActivateGeneration = {};
						record.failedRagdollActivatePublicationGeneration = 0U;
						record.failedRagdollActivateFrameTick = 0;
						record.ragdollLifecycle = RagdollLifecycleState::Active;
						record.ragdollLifecycleStartTick = result.committedStep;
						record.ragdollRecoveryBlockReason = RagdollRecoveryBlockReason::None;
						break;
					case NativeAnimationRagdollOperation::Deactivate:
						if(record.pendingRagdollExternalRecovery &&
						   record.pendingRagdollPreviousLifecycle == RagdollLifecycleState::Recovering) {
							Physics::RagdollExternalRecoveryToken token;
							token.world = Physics::PhysicsSystem::Instance().WorldHandle();
							token.ragdollId = result.ragdollId;
							token.correlationId = result.correlationId;
							token.deactivateGeneration = result.generation;
							token.committedStep = result.committedStep;
							const auto completed = Physics::PhysicsSystem::Instance().CompleteExternalCharacterRecovery(token);
							if(completed.accepted) {
								record.ragdollLifecycle = RagdollLifecycleState::Inactive;
								record.ragdollBlendWeight = 0.0F;
								record.ragdollRecoveryCandidateId = 0U;
								record.ragdollRecoveryBlockReason = RagdollRecoveryBlockReason::None;
								record.ragdollRecoveryDurationSeconds = 0.0F;
								record.pendingExternalRecoveryToken.reset();
								record.pendingExternalRecoveryNeedsRefresh = false;
							} else {
								record.pendingExternalRecoveryToken = token;
								record.ragdollLifecycle = RagdollLifecycleState::TearingDown;
								// The provider stopped the ragdoll, but the character is
								// still physically ExternalExclusive. Do not advertise a
								// completed recovery until the retained ticket grants it.
								snapshotStatus = AnimationStatus::NotReady;
								record.pendingExternalRecoveryNeedsRefresh =
									completed.error != Physics::CharacterExternalRecoveryError::ProviderRejected &&
									completed.error != Physics::CharacterExternalRecoveryError::CapacityExceeded;
							}
						} else {
							record.ragdollLifecycle = RagdollLifecycleState::Inactive;
							record.ragdollBlendWeight = 0.0F;
							record.ragdollRecoveryCandidateId = 0U;
							record.ragdollRecoveryBlockReason = RagdollRecoveryBlockReason::None;
							record.ragdollRecoveryDurationSeconds = 0.0F;
						}
						break;
					case NativeAnimationRagdollOperation::RequestRecovery:
						record.ragdollLifecycle = RagdollLifecycleState::Recovering;
						break;
					}
				} else {
					// Physics owns the complete lifecycle batch. If it rejects, restore
					// the exact animation-side state captured before enqueueing instead of
					// exposing a half-transitioned Creating/TearingDown record.
					record.ragdollGeneration = record.pendingRagdollPreviousGeneration;
					record.ragdollLifecycle = record.pendingRagdollPreviousLifecycle;
					record.activeRagdollProfileId = record.pendingRagdollPreviousProfileId;
					record.ragdollBlendWeight = record.pendingRagdollPreviousBlendWeight;
					if(record.pendingRagdollOperation == NativeAnimationRagdollOperation::Activate) {
						record.failedRagdollActivateCorrelationId = result.correlationId;
						record.failedRagdollActivateGeneration = result.generation;
						record.failedRagdollActivatePublicationGeneration = publicationGeneration;
						record.failedRagdollActivateFrameTick = result.committedStep;
					}
				}
				(void)advancedCommands->Publish(
					MakeRagdollSnapshot(record, ToNativeStatus(snapshotStatus), result.committedStep, publicationGeneration));
				record.pendingRagdollCorrelationId = 0U;
				record.pendingRagdollPreviousGeneration = {};
				record.pendingRagdollExternalRecovery = false;
				PublishRagdollExclusionForRecordLocked(record, result.correlationId);
			});
		}
	}

	void AnimationSystem::Impl::PublishAdvancedCommandCompletionsLocked(std::uint64_t publicationGeneration) noexcept {
		if(advancedCommands == nullptr || publicationGeneration == 0U)
			return;
		SynchronizeRagdollResultsLocked(publicationGeneration);

		PublishRetargetCompletionsLocked(publicationGeneration);

		PublishMotionMatchCompletionsLocked(publicationGeneration);

		PublishMotionMatchV2CompletionsLocked(publicationGeneration);

		PublishRagdollCompletionsLocked(publicationGeneration);

		PublishDeterminismCompletionsLocked(publicationGeneration);
	}

	[[nodiscard]] std::shared_ptr<const GpuAnimationPresentationFrame>
	AnimationSystem::Impl::BuildGpuPresentationFrame(std::uint64_t sourceGeneration,
													 std::shared_ptr<const AnimationAuthorityFrame> sealedAuthority) noexcept {
		if(sourceGeneration == 0U)
			return {};
		const std::shared_ptr<const AnimationAuthorityFrame> authority =
			sealedAuthority ? std::move(sealedAuthority) : authorityPublisher.Load();
		if(!authority || !authority->IsValid() || authority->authorityGeneration != sourceGeneration)
			return {};
		try {
			std::shared_ptr<GpuAnimationPresentationFrame> frame;
			for(std::size_t attempt = 0U; attempt < persistentReuse.gpuPresentationFramePool.size(); ++attempt) {
				const std::size_t slot =
					(persistentReuse.gpuPresentationFramePoolCursor + attempt) % persistentReuse.gpuPresentationFramePool.size();
				std::shared_ptr<GpuAnimationPresentationFrame>& candidate = persistentReuse.gpuPresentationFramePool[slot];
				if(!candidate)
					candidate = std::make_shared<GpuAnimationPresentationFrame>();
				if(candidate.use_count() == 1U) {
					frame = candidate;
					persistentReuse.gpuPresentationFramePoolCursor = (slot + 1U) % persistentReuse.gpuPresentationFramePool.size();
					break;
				}
			}
			if(!frame)
				frame = std::make_shared<GpuAnimationPresentationFrame>();
			frame->inputs.clear();
			frame->retainedStaticPlan.reset();
			frame->preparedDynamicLanes.ResetForReuse();
			frame->preparedEvaluationFingerprints.clear();
			frame->preparedDynamicPlanComplete = false;
			frame->ownerObjectMappings.clear();
			frame->authority = authority;
			// This system runs before Physics and can therefore provide
			// only the authority-bound GPU source.
			// AnimationPresentationSystem attaches the typed
			// post-physics boundary after the Physics workload
			// completes.
			frame->sourceGeneration = sourceGeneration;
			frame->planGeneration = static_cast<std::uint32_t>(sourceGeneration);
			if(frame->planGeneration == 0U)
				frame->planGeneration = 1U;
			frame->inputs.reserve(frameStorage.frameWork.size() + frameInputs.compactGpuVisualCaptures.size());
			frame->ownerObjectMappings.reserve(frameStorage.frameWork.size() + frameInputs.compactGpuVisualCaptures.size());
			// Stable owner identity is independent of the animated
			// sample. Hash it while the mandatory mapping pass is
			// already cache-hot so presentation can reuse its sorted
			// binding and packet-lookup roots on later frames.
			std::uint64_t ownerMappingSignature = 1469598103934665603ULL;
			GpuAnimationLoweringTopologySignatureBuilder topologySignatureBuilder;
			// Capture the original immutable topology root once. The producer is
			// already about to visit every current input for owner/signature sealing,
			// so it can populate the changing generation/tick lanes in the same pass.
			// Presentation then performs only scalar proof checks on the warm path.
			const std::shared_ptr<const PublishedGpuAnimationPresentationPlan> previousPresentation =
				AnimationPresentationSystem::Instance().LoadPublishedGpuAnimationPresentationPlan();
			std::shared_ptr<const GpuAnimationPlan> retainedStaticPlan;
			if(previousPresentation && !previousPresentation->safeFallback) {
				retainedStaticPlan = previousPresentation->plan.retainedStaticPlan
										 ? previousPresentation->plan.retainedStaticPlan
										 : std::shared_ptr<const GpuAnimationPlan>(previousPresentation, &previousPresentation->plan);
			}
			const GpuAnimationPlan* staticPlan = retainedStaticPlan.get();
			bool preparedDynamicPlanComplete = staticPlan != nullptr && staticPlan->generation != 0U &&
											   staticPlan->topologyInputSignature != 0U &&
											   staticPlan->StablePacketIndices().size() == staticPlan->WorkPackets().size();
			std::size_t preparedPacketCount = 0U;
			const bool deferStableEnvelope = preparedDynamicPlanComplete;
			std::size_t gpuInputCount = 0U;
			if(preparedDynamicPlanComplete) {
				const std::size_t packetCount = staticPlan->WorkPackets().size();
				frame->preparedDynamicLanes.generationRecords.resize(staticPlan->GenerationRecordCount());
				frame->preparedDynamicLanes.sampleTicks.resize(packetCount);
				frame->preparedDynamicLanes.renderIdentities.resize(packetCount);
				frame->preparedEvaluationFingerprints.resize(packetCount);
			}
			const auto appendPreparedDynamicLane = [&](const GpuAnimationVisualInput& visual, const EvaluationProgram* program,
													   std::uint64_t programContentId, std::uint32_t stageMask,
													   bool residentPayloadsValidated) noexcept {
				if(!preparedDynamicPlanComplete)
					return;
				const auto& packets = staticPlan->WorkPackets();
				std::uint32_t packetIndex = 0U;
				if(!staticPlan->FindWorkPacketIndex(visual.stableInstanceId, visual.stableSubmissionOrdinal, packetIndex) ||
				   packetIndex >= packets.size() || !residentPayloadsValidated || program == nullptr ||
				   program->stableHash != programContentId || program->compatibilityFingerprint != visual.compatibilityFingerprint ||
				   !IsGpuAnimationGenerationRecordValid(visual.generations) || !IsGpuAnimationRenderIdentityValid(visual.renderIdentity)) {
					preparedDynamicPlanComplete = false;
					return;
				}
				const GpuAnimationWorkPacket& packet = packets[packetIndex];
				if(stageMask != packet.stageMask || packet.generationRecordIndex >= frame->preparedDynamicLanes.generationRecords.size() ||
				   IsGpuAnimationGenerationRecordValid(frame->preparedDynamicLanes.generationRecords[packet.generationRecordIndex])) {
					// A repeated owner would overwrite one lane and leave another absent.
					// Reject it while the mandatory producer pass is already at that row.
					preparedDynamicPlanComplete = false;
					return;
				}
				GpuAnimationGenerationRecord generation = visual.generations;
				if(visual.suspendVisualEvaluation) {
					generation.flags |= kGpuAnimationGenerationFlagSuspendVisualEvaluation;
				} else {
					generation.flags &= ~kGpuAnimationGenerationFlagSuspendVisualEvaluation;
				}
				generation.reserved = visual.allowVisualRagdoll ? visual.ragdollTableOffsetBytes : 0U;
				frame->preparedDynamicLanes.generationRecords[packet.generationRecordIndex] = generation;
				frame->preparedDynamicLanes.sampleTicks[packetIndex] = visual.sampleTick;
				frame->preparedDynamicLanes.renderIdentities[packetIndex] = visual.renderIdentity;
				// Evaluation sharing also depends on generation equality. Collapse its
				// dynamic half into one CPU-only proof value; static resource selectors,
				// packet flags, and batch layout remain covered by the topology signature.
				std::uint64_t evaluationFingerprint = visual.evaluationParameterFingerprint;
				const auto mixEvaluation = [&evaluationFingerprint](std::uint64_t value) noexcept {
					evaluationFingerprint ^= value;
					evaluationFingerprint *= 1099511628211ULL;
				};
				mixEvaluation(generation.programGeneration);
				mixEvaluation(generation.controllerGeneration);
				mixEvaluation(generation.skeletonGeneration);
				mixEvaluation(generation.clipGeneration);
				mixEvaluation(generation.meshGeneration);
				mixEvaluation(generation.resourceGeneration);
				mixEvaluation(generation.rendererGeneration);
				mixEvaluation(generation.sceneGeneration);
				frame->preparedEvaluationFingerprints[packetIndex] = evaluationFingerprint;
				++preparedPacketCount;
			};
			const auto materializeCompactLowering = [](const CompactGpuVisualCaptures& compact, std::size_t compactIndex) {
				const CompactGpuVisualCohort& cohort = compact.cohort(compactIndex);
				GpuAnimationLoweringInput lowering;
				lowering.program.program = cohort.program;
				lowering.program.programContentId = cohort.program->stableHash;
				lowering.program.programResourceIndex = cohort.fastBinding->programResourceIndex;
				lowering.visual = compact.gpuPresentationVisuals[compactIndex];
				lowering.effectiveMode = compact.executionDecisions[compactIndex].effectiveMode;
				lowering.programValidated = true;
				lowering.prevalidatedStageMask = compact.stageMasks[compactIndex];
				lowering.residentPayloads = cohort.fastBinding->residentPayloads;
				lowering.residentPayloadsValidated = true;
				lowering.allowSharedVisualEvaluation = true;
				return lowering;
			};
			const auto includeOwnerMapping = [&ownerMappingSignature](const GpuAnimationOwnerObjectMapping& mapping) {
				const auto combine = [&ownerMappingSignature](std::uint64_t value) {
					ownerMappingSignature ^= value;
					ownerMappingSignature *= 1099511628211ULL;
				};
				combine(static_cast<std::uint64_t>(std::hash<UUID>{}(mapping.ownerObjectId)));
				combine(mapping.stableInstanceId);
				combine(mapping.instanceSlot);
				combine(mapping.instanceGeneration);
				combine(mapping.componentGeneration);
			};
			for(FrameWorkItem& item : frameStorage.frameWork) {
				if(!item.gpuLowering)
					continue;
				// Diagnostics have already consumed the frame-local
				// decision by the time this handoff runs. Capture the
				// owner key before moving the lowering input: a failed
				// mapping must not depend on an unspecified moved-from
				// value and accidentally discard the entire presentation
				// frame.
				GpuAnimationOwnerObjectMapping mapping{};
				mapping.ownerObjectId = item.input.descriptor.ownerObjectId;
				mapping.stableInstanceId = item.gpuLowering->visual.stableInstanceId;
				mapping.instanceSlot = item.input.handle.slotIndex;
				mapping.instanceGeneration = item.input.handle.generation;
				mapping.componentGeneration = item.input.descriptor.componentGeneration;
				if(!mapping.IsComplete())
					return {};
				includeOwnerMapping(mapping);
				topologySignatureBuilder.Include(*item.gpuLowering);
				appendPreparedDynamicLane(item.gpuLowering->visual, item.gpuLowering->program.program.get(),
										  item.gpuLowering->program.programContentId, item.gpuLowering->prevalidatedStageMask,
										  item.gpuLowering->residentPayloadsValidated);
				// Transfer immutable owners only after the complete
				// mapping proof. This lets presentation distinguish a
				// valid empty GPU source from a rejected owner identity
				// without exposing partially moved frame contents.
				++gpuInputCount;
				if(!deferStableEnvelope) {
					frame->inputs.push_back(std::move(*item.gpuLowering));
					frame->ownerObjectMappings.push_back(mapping);
				}
			}
			for(std::size_t compactIndex = 0U; compactIndex < frameInputs.compactGpuVisualCaptures.size(); ++compactIndex) {
				CompactGpuVisualCaptures& compact = frameInputs.compactGpuVisualCaptures;
				if(compact.processingClasses[compactIndex] != AnimationProcessingClass::CompactGpuVisual)
					continue;
				GpuAnimationOwnerObjectMapping mapping{};
				mapping.ownerObjectId = compact.inputs[compactIndex].ownerObjectId;
				mapping.stableInstanceId = compact.gpuPresentationVisuals[compactIndex].stableInstanceId;
				mapping.instanceSlot = compact.inputs[compactIndex].handle.slotIndex;
				mapping.instanceGeneration = compact.inputs[compactIndex].handle.generation;
				mapping.componentGeneration = compact.inputs[compactIndex].componentGeneration;
				if(!mapping.IsComplete())
					return {};
				includeOwnerMapping(mapping);
				const CompactGpuVisualCohort& cohort = compact.cohort(compactIndex);
				const GpuAnimationVisualInput& visual = compact.gpuPresentationVisuals[compactIndex];
				topologySignatureBuilder.IncludePrepared(cohort.program->stableHash, cohort.fastBinding->programResourceIndex, visual,
														 compact.stageMasks[compactIndex],
														 compact.executionDecisions[compactIndex].effectiveMode, true);
				appendPreparedDynamicLane(visual, cohort.program.get(), cohort.program->stableHash, compact.stageMasks[compactIndex], true);
				// A prior no-view snapshot cannot suppress this packet:
				// custom and editor captures can become visible after
				// animation capture has completed. Keep every proven
				// compact owner in the immutable presentation frame and
				// let renderer-side culling decide whether it is
				// submitted this frame. Presentation is the sole
				// remaining consumer after diagnostics, so move the
				// lowering instead of performing thousands of
				// shared-ownership bumps.
				++gpuInputCount;
				if(!deferStableEnvelope) {
					frame->inputs.push_back(materializeCompactLowering(compact, compactIndex));
					frame->ownerObjectMappings.push_back(mapping);
				}
			}
			frame->ownerMappingSignature = gpuInputCount == 0U ? 0U : ownerMappingSignature;
			frame->topologyInputSignature = gpuInputCount == 0U ? 0U : topologySignatureBuilder.Complete();
			// Retaining an extra evaluation after two private owners converge is
			// conservative. Diverging a formerly shared owner is not: it would read
			// another owner's pose. Verify only the latter relationship in one tight
			// scalar pass, without relowering requests or touching resource payloads.
			if(preparedDynamicPlanComplete) {
				const auto& evaluationSources = staticPlan->EvaluationSourcePacketIndices();
				if(evaluationSources.size() != preparedPacketCount) {
					preparedDynamicPlanComplete = false;
				} else {
					for(std::size_t packetIndex = 0U; packetIndex < evaluationSources.size(); ++packetIndex) {
						const std::uint32_t sourceIndex = evaluationSources[packetIndex];
						if(sourceIndex >= evaluationSources.size() ||
						   (sourceIndex != packetIndex &&
							(frame->preparedEvaluationFingerprints[packetIndex] != frame->preparedEvaluationFingerprints[sourceIndex] ||
							 frame->preparedDynamicLanes.sampleTicks[packetIndex] !=
								 frame->preparedDynamicLanes.sampleTicks[sourceIndex]))) {
							preparedDynamicPlanComplete = false;
							break;
						}
					}
				}
			}
			preparedDynamicPlanComplete = preparedDynamicPlanComplete && preparedPacketCount == staticPlan->WorkPackets().size() &&
										  frame->topologyInputSignature == staticPlan->topologyInputSignature &&
										  frame->preparedDynamicLanes.HasParallelLengths(staticPlan->WorkPackets().size());
			if(preparedDynamicPlanComplete) {
				frame->preparedEvaluationFingerprints.clear();
				frame->retainedStaticPlan = std::move(retainedStaticPlan);
				frame->preparedDynamicPlanComplete = true;
			} else {
				if(deferStableEnvelope) {
					// A topology proof may fail after the producer has already visited
					// part of the population. Build the complete cold envelope only in
					// that uncommon case; warmed frames retain only dynamic lanes.
					frame->inputs.reserve(gpuInputCount);
					frame->ownerObjectMappings.reserve(gpuInputCount);
					for(FrameWorkItem& item : frameStorage.frameWork) {
						if(!item.gpuLowering)
							continue;
						GpuAnimationOwnerObjectMapping mapping{};
						mapping.ownerObjectId = item.input.descriptor.ownerObjectId;
						mapping.stableInstanceId = item.gpuLowering->visual.stableInstanceId;
						mapping.instanceSlot = item.input.handle.slotIndex;
						mapping.instanceGeneration = item.input.handle.generation;
						mapping.componentGeneration = item.input.descriptor.componentGeneration;
						if(!mapping.IsComplete())
							return {};
						frame->inputs.push_back(std::move(*item.gpuLowering));
						frame->ownerObjectMappings.push_back(mapping);
					}
					for(std::size_t compactIndex = 0U; compactIndex < frameInputs.compactGpuVisualCaptures.size(); ++compactIndex) {
						CompactGpuVisualCaptures& compact = frameInputs.compactGpuVisualCaptures;
						if(compact.processingClasses[compactIndex] != AnimationProcessingClass::CompactGpuVisual)
							continue;
						GpuAnimationOwnerObjectMapping mapping{};
						mapping.ownerObjectId = compact.inputs[compactIndex].ownerObjectId;
						mapping.stableInstanceId = compact.gpuPresentationVisuals[compactIndex].stableInstanceId;
						mapping.instanceSlot = compact.inputs[compactIndex].handle.slotIndex;
						mapping.instanceGeneration = compact.inputs[compactIndex].handle.generation;
						mapping.componentGeneration = compact.inputs[compactIndex].componentGeneration;
						if(!mapping.IsComplete())
							return {};
						frame->inputs.push_back(materializeCompactLowering(compact, compactIndex));
						frame->ownerObjectMappings.push_back(mapping);
					}
				}
				frame->preparedDynamicLanes.ResetForReuse();
				frame->preparedEvaluationFingerprints.clear();
			}
			return frame;
		} catch(const std::bad_alloc&) {
			// CPU authority has already committed.  A
			// render-presentation allocation failure must remain an
			// isolated CPU fallback for this frame.
			return {};
		}
	}

	std::shared_ptr<const AnimationAuthorityRow>
	AnimationSystem::Impl::FindPreEvaluationPublication(NativeAnimationAnimatorHandle animator) {

		const auto work = std::find_if(frameStorage.frameWork.begin(), frameStorage.frameWork.end(), [animator](const FrameWorkItem& item) {
			return item.input.handle.slotIndex == animator.slotIndex && item.input.handle.generation == animator.generation;
		});
		// FrameWorkItem owns an immutable pointer captured before worker evaluation.
		// Completion runs after the sealed authority state commits, so
		// only this retained pointer can validate the generation the producer saw.
		return work == frameStorage.frameWork.end() ? std::shared_ptr<const AnimationAuthorityRow>{} : work->priorPublication;
	}

	void AnimationSystem::Impl::PublishRetargetCompletionsLocked(std::uint64_t publicationGeneration) noexcept {
		for(const NativeAnimationRetargetRequest& request : frameInputs.capturedRetargetRequests) {
			RuntimeRecord* record = FindRuntimeRecordLocked({request.animator.slotIndex, request.animator.generation});
			NativeAnimationRetargetSnapshot snapshot{};
			snapshot.header = MakeNativeHeader(NativeAnimationRecordKind::RetargetSnapshot, sizeof(snapshot));
			snapshot.animator = request.animator;
			snapshot.profileAssetId = request.profileAssetId;
			snapshot.profileGeneration = request.profileGeneration;
			snapshot.observationTick = frameInputs.frameTick;
			snapshot.publicationGeneration = publicationGeneration;
			AnimationStatus status = AnimationStatus::StaleHandle;
			if(record != nullptr && instances->IsAlive(record->handle)) {
				const std::shared_ptr<const AnimationAuthorityFrame> authority = authorityPublisher.Load();
				const AnimationAuthorityRow* published = authority ? authority->Find(record->handle) : nullptr;
				const std::shared_ptr<const AnimationAuthorityRow> preEvaluationPublication =
					FindPreEvaluationPublication(request.animator);
				snapshot.sourceSkeletonGeneration = record->binding.retargetSourceSkeletonGeneration;
				snapshot.targetSkeletonGeneration = record->binding.skeletonGeneration;
				snapshot.poseGeneration = published && published->snapshot ? published->snapshot->publicationGeneration : 0U;
				if(request.operation == NativeAnimationRetargetOperation::DeactivateProfile) {
					record->activeRetargetProfileId = {};
					record->activeRetargetProfileGeneration = 0U;
					status = AnimationStatus::Ok;
				} else if(!record->binding.retargetProfile ||
						  !MatchesNativeUuid(request.profileAssetId, record->binding.retargetProfile->assetId.value) ||
						  request.profileGeneration != record->binding.retargetProfileGeneration) {
					status = AnimationStatus::MissingAsset;
				} else if((request.expectedPoseGeneration != 0U &&
						   (!preEvaluationPublication || !preEvaluationPublication->snapshot ||
							request.expectedPoseGeneration != preEvaluationPublication->snapshot->publicationGeneration)) ||
						  (request.expectedPublicationGeneration != 0U &&
						   (!preEvaluationPublication ||
							request.expectedPublicationGeneration != preEvaluationPublication->authorityPublicationGeneration))) {
					status = AnimationStatus::StaleHandle;
				} else if(!record->binding.retargetSourceSkeleton || !record->binding.skeleton ||
						  record->binding.retargetProfile->sourceSkeletonFingerprint !=
							  record->binding.retargetSourceSkeleton->compatibilityFingerprint ||
						  record->binding.retargetProfile->targetSkeletonFingerprint !=
							  record->binding.skeleton->compatibilityFingerprint) {
					status = AnimationStatus::IncompatibleAsset;
				} else {
					RetargetValidationResult validation;
					RetargetCompiledProfile compiled;
					status = CompileRetargetProfile(*record->binding.retargetProfile, *record->binding.retargetSourceSkeleton,
													*record->binding.skeleton, validation, compiled);
					if(status == AnimationStatus::Ok) {
						try {
							// Cooked bytes intentionally omit session-local generations. Pair the
							// newly compiled table with the exact publication validated above.
							compiled.profile.runtimeGeneration = request.profileGeneration;
							auto immutable = std::make_shared<const RetargetCompiledProfile>(std::move(compiled));
							// The complete mapping is allocated before either live identity changes,
							// preserving the prior active profile on any failed rebuild.
							record->binding.compiledRetargetProfile = std::move(immutable);
							record->activeRetargetProfileId = request.profileAssetId;
							record->activeRetargetProfileGeneration = request.profileGeneration;
						} catch(const std::bad_alloc&) {
							status = AnimationStatus::InternalError;
						}
					}
				}
			}
			snapshot.status = ToNativeStatus(status);
			(void)advancedCommands->Publish(std::move(snapshot));
		}
	}

	void AnimationSystem::Impl::PublishMotionMatchCompletionsLocked(std::uint64_t publicationGeneration) noexcept {
		for(const NativeAnimationMotionMatchRequest& request : frameInputs.capturedMotionMatchRequests) {
			RuntimeRecord* record = FindRuntimeRecordLocked({request.animator.slotIndex, request.animator.generation});
			NativeAnimationMotionMatchSnapshot snapshot{};
			snapshot.header = MakeNativeHeader(NativeAnimationRecordKind::MotionMatchSnapshot, sizeof(snapshot));
			snapshot.animator = request.animator;
			snapshot.databaseAssetId = request.databaseAssetId;
			snapshot.databaseGeneration = request.databaseGeneration;
			snapshot.queryId = request.queryId;
			snapshot.observationTick = frameInputs.frameTick;
			snapshot.publicationGeneration = publicationGeneration;
			AnimationStatus status = AnimationStatus::StaleHandle;
			if(record != nullptr && instances->IsAlive(record->handle)) {
				if(request.operation == NativeAnimationMotionMatchOperation::Clear) {
					record->activeMotionDatabaseId = {};
					record->activeMotionDatabaseGeneration = 0U;
					record->latestMotionQueryId = 0U;
					record->appliedMotionQueryId = 0U;
					record->latestMotionCandidate = {};
					record->hasMotionCandidate = false;
					status = AnimationStatus::Ok;
				} else if(request.operation == NativeAnimationMotionMatchOperation::SetDatabase) {
					if(!record->binding.motionDatabase ||
					   !MatchesNativeUuid(request.databaseAssetId, record->binding.motionDatabase->assetId.value) ||
					   request.databaseGeneration != record->binding.motionDatabaseGeneration ||
					   request.featureCount != kMotionMatchFeatureCountV1) {
						status = record->binding.motionDatabase ? AnimationStatus::IncompatibleAsset : AnimationStatus::MissingAsset;
					} else {
						record->activeMotionDatabaseId = request.databaseAssetId;
						record->activeMotionDatabaseGeneration = request.databaseGeneration;
						record->latestMotionQueryId = 0U;
						record->appliedMotionQueryId = 0U;
						record->latestMotionCandidate = {};
						record->hasMotionCandidate = false;
						status = AnimationStatus::Ok;
					}
				} else if(record->activeMotionDatabaseGeneration == 0U || request.featureCount != kMotionMatchFeatureCountV1) {
					status = AnimationStatus::NotReady;
				} else {
					const auto work =
						std::find_if(frameStorage.frameWork.begin(), frameStorage.frameWork.end(), [&request](const FrameWorkItem& item) {
							return item.input.handle.slotIndex == request.animator.slotIndex &&
								   item.input.handle.generation == request.animator.generation;
						});
					const auto evaluated = work == frameStorage.frameWork.end()
											   ? std::vector<NativeAnimationMotionMatchSnapshot>::const_iterator{}
											   : std::find_if(work->evaluatedMotionQueries.begin(), work->evaluatedMotionQueries.end(),
															  [&request](const NativeAnimationMotionMatchSnapshot& value) {
																  return value.queryId == request.queryId;
															  });
					if(work != frameStorage.frameWork.end() && evaluated != work->evaluatedMotionQueries.end()) {
						snapshot = *evaluated;
						snapshot.observationTick = frameInputs.frameTick;
						snapshot.publicationGeneration = publicationGeneration;
						status = static_cast<AnimationStatus>(snapshot.status);
						if(status == AnimationStatus::Ok && work->hasMotionCandidate) {
							record->latestMotionQueryId = request.queryId;
							record->latestMotionCandidate = work->latestMotionCandidate;
							record->hasMotionCandidate = true;
						}
					} else {
						// A newly activated database is visible to worker queries on the next
						// capture boundary. Publication never performs a large synchronous search.
						status = AnimationStatus::NotReady;
					}
				}
			}
			snapshot.status = ToNativeStatus(status);
			if(status != AnimationStatus::Ok)
				snapshot.usedFallback = 1U;
			(void)advancedCommands->Publish(std::move(snapshot));
		}
	}

	void AnimationSystem::Impl::PublishMotionMatchV2CompletionsLocked(std::uint64_t publicationGeneration) noexcept {
		for(const NativeAnimationMotionMatchQueryRequestV2& request : frameInputs.capturedMotionMatchQueriesV2) {
			RuntimeRecord* record = FindRuntimeRecordLocked({request.animator.slotIndex, request.animator.generation});
			NativeAnimationMotionMatchResultV2 result{};
			result.header = MakeNativeHeader(NativeAnimationRecordKind::MotionMatchResultV2, sizeof(result));
			result.animator = request.animator;
			result.databaseAssetId = request.databaseAssetId;
			result.databaseGeneration = request.databaseGeneration;
			result.schemaFingerprint = request.schemaFingerprint;
			result.queryId = request.queryId;
			result.observationTick = frameInputs.frameTick;
			result.publicationGeneration = publicationGeneration;
			result.populatedFields = static_cast<std::uint32_t>(NativeAnimationMotionMatchResultField::Identity);
			AnimationStatus status = AnimationStatus::StaleHandle;
			if(record != nullptr && instances->IsAlive(record->handle)) {
				const auto work =
					std::find_if(frameStorage.frameWork.begin(), frameStorage.frameWork.end(), [&request](const FrameWorkItem& item) {
						return item.input.handle.slotIndex == request.animator.slotIndex &&
							   item.input.handle.generation == request.animator.generation;
					});
				if(work != frameStorage.frameWork.end()) {
					const auto evaluated =
						std::find_if(work->evaluatedMotionQueriesV2.begin(), work->evaluatedMotionQueriesV2.end(),
									 [&request](const MotionMatchQueryCompletionV2& value) {
										 return value.correlationId == request.correlationId && value.result.queryId == request.queryId;
									 });
					if(evaluated != work->evaluatedMotionQueriesV2.end()) {
						result = evaluated->result;
						result.observationTick = frameInputs.frameTick;
						result.publicationGeneration = publicationGeneration;
						status = static_cast<AnimationStatus>(result.status);
						if(status == AnimationStatus::Ok && evaluated->hasSelectedCandidate) {
							// A successful direct query validates the immutable bound database.
							// Retaining that identity lets the chosen candidate enter the same
							// next-frame controller path used by the frozen V1 API.
							record->activeMotionDatabaseId = request.databaseAssetId;
							record->activeMotionDatabaseGeneration = request.databaseGeneration;
							record->latestMotionQueryId = request.queryId;
							record->appliedMotionQueryId = 0U;
							record->latestMotionCandidate = evaluated->selectedCandidate;
							record->hasMotionCandidate = true;
						}
					} else {
						// A live scheduled item without a completion indicates bounded
						// staging failed; do not silently report an endless pending query.
						status = AnimationStatus::InternalError;
					}
				} else {
					status = AnimationStatus::NotReady;
				}
			}
			result.status = ToNativeStatus(status);
			(void)advancedCommands->Publish(std::move(result));
		}
	}

	void AnimationSystem::Impl::PublishRagdollCompletionsLocked(std::uint64_t publicationGeneration) noexcept {
		for(const NativeAnimationRagdollRequest& request : frameInputs.capturedRagdollRequests) {
			RuntimeRecord* record = FindRuntimeRecordLocked({request.animator.slotIndex, request.animator.generation});
			if(record == nullptr || !instances->IsAlive(record->handle)) {
				NativeAnimationRagdollSnapshot stale{};
				stale.header = MakeNativeHeader(NativeAnimationRecordKind::RagdollSnapshot, sizeof(stale));
				stale.status = NativeAnimationStatus::StaleHandle;
				stale.animator = request.animator;
				stale.profileAssetId = request.profileAssetId;
				stale.profileGeneration = request.profileGeneration;
				stale.physicsWorldGeneration = request.physicsWorldGeneration;
				stale.observationTick = frameInputs.frameTick;
				stale.publicationGeneration = publicationGeneration;
				(void)advancedCommands->Publish(std::move(stale));
				DropStalePendingRagdollExclusionLocked({request.animator.slotIndex, request.animator.generation}, request.correlationId);
				continue;
			}

			AnimationStatus status = AnimationStatus::NotReady;
			bool recoveryBlocked = false;
			if(record->pendingRagdollCorrelationId != 0U) {
				status = AnimationStatus::NotReady;
			} else if(request.operation == NativeAnimationRagdollOperation::RequestRecovery && record->pendingExternalRecoveryToken) {
				status =
					record->pendingExternalRecoveryNeedsRefresh ? RefreshExternalRagdollRecoveryLocked(*record) : AnimationStatus::Queued;
			} else if(record->failedRagdollActivateCorrelationId != 0U &&
					  (request.operation == NativeAnimationRagdollOperation::RequestRecovery ||
					   request.operation == NativeAnimationRagdollOperation::Deactivate)) {
				status = AbortFailedRagdollAdmissionLocked(*record);
			} else if(request.operation == NativeAnimationRagdollOperation::RequestRecovery) {
				status = BeginRagdollRecoveryLocked(*record, request, recoveryBlocked);
			} else if(request.operation == NativeAnimationRagdollOperation::Activate) {
				PhysicsPoseSnapshot activationPose;
				RagdollGenerationStamp generation;
				// SubmitRagdollRequest published PendingActivate already. Recheck
				// the copied controller table here so Physics cannot activate a body
				// while its character still accepts velocity or root movement.
				const auto rootBindings = rootControllerBindings.load(std::memory_order_acquire);
				const auto exclusions = ragdollExclusions.load(std::memory_order_acquire);
				status = rootBindings && exclusions
							 ? ApplyRagdollControllerExclusions(*rootBindings, *exclusions, Physics::PhysicsSystem::Instance())
							 : AnimationStatus::Ok;
				if(status == AnimationStatus::Ok)
					status = BuildRagdollActivationPoseLocked(*record, request, publicationGeneration, activationPose, generation);
				if(status == AnimationStatus::Ok) {
					RagdollPhysicsRequest activate;
					activate.kind = RagdollPhysicsRequestKind::Activate;
					activate.ragdollId = record->ragdollId;
					activate.generation = generation;
					activate.expectedCurrentGeneration = record->ragdollGeneration;
					activate.profile = record->binding.ragdollProfile;
					activate.activationPose = activationPose;
					activate.correlationId = request.correlationId;
					activate.producerOrder = 2U;
					if(record->ragdollCreated) {
						status = Physics::PhysicsSystem::Instance().SubmitRagdollPhysicsRequest(std::move(activate));
					} else {
						RagdollPhysicsRequest create = activate;
						create.kind = RagdollPhysicsRequestKind::Create;
						create.expectedCurrentGeneration = {};
						create.correlationId = request.correlationId ^ 0xA11A000000000000ULL;
						if(create.correlationId == 0U)
							create.correlationId = 1U;
						create.producerOrder = 1U;
						activate.expectedCurrentGeneration = generation;
						const std::array<RagdollPhysicsRequest, 2U> batch{std::move(create), std::move(activate)};
						status = Physics::PhysicsSystem::Instance().SubmitRagdollPhysicsRequestBatch(batch);
					}
					if(status == AnimationStatus::Queued) {
						record->pendingRagdollPreviousGeneration = record->ragdollGeneration;
						record->pendingRagdollPreviousLifecycle = record->ragdollLifecycle;
						record->pendingRagdollPreviousProfileId = record->activeRagdollProfileId;
						record->pendingRagdollPreviousBlendWeight = record->ragdollBlendWeight;
						record->ragdollGeneration = generation;
						record->activeRagdollProfileId = request.profileAssetId;
						record->ragdollLifecycle = RagdollLifecycleState::Creating;
						record->ragdollBlendWeight = request.blendWeight;
						record->pendingRagdollCorrelationId = request.correlationId;
						record->pendingRagdollOperation = request.operation;
					}
				}
			} else if(!record->ragdollCreated || !record->ragdollGeneration.IsValid()) {
				status = AnimationStatus::NotReady;
			} else if(request.physicsWorldGeneration != record->ragdollGeneration.worldGeneration) {
				status = AnimationStatus::StaleHandle;
			} else {
				RagdollGenerationStamp generation = record->ragdollGeneration;
				generation.publicationGeneration = publicationGeneration;
				generation.fixedStep = std::max<AnimationFixedTick>(1, frameInputs.frameTick);
				RagdollPhysicsRequest deactivate;
				deactivate.kind = RagdollPhysicsRequestKind::Deactivate;
				deactivate.ragdollId = record->ragdollId;
				deactivate.generation = generation;
				deactivate.expectedCurrentGeneration = record->ragdollGeneration;
				deactivate.correlationId = request.correlationId;
				deactivate.producerOrder = 1U;
				status = Physics::PhysicsSystem::Instance().SubmitRagdollPhysicsRequest(std::move(deactivate));
				if(status == AnimationStatus::Queued) {
					record->pendingRagdollPreviousGeneration = record->ragdollGeneration;
					record->pendingRagdollPreviousLifecycle = record->ragdollLifecycle;
					record->pendingRagdollPreviousProfileId = record->activeRagdollProfileId;
					record->pendingRagdollPreviousBlendWeight = record->ragdollBlendWeight;
					record->ragdollGeneration = generation;
					record->ragdollLifecycle = RagdollLifecycleState::TearingDown;
					record->pendingRagdollCorrelationId = request.correlationId;
					record->pendingRagdollOperation = request.operation;
				}
			}
			(void)advancedCommands->Publish(
				MakeRagdollSnapshot(*record, ToNativeStatus(status), frameInputs.frameTick, publicationGeneration, recoveryBlocked));
			if(request.operation == NativeAnimationRagdollOperation::Activate && status != AnimationStatus::Queued &&
			   status != AnimationStatus::Ok) {
				record->failedRagdollActivateCorrelationId = request.correlationId;
				record->failedRagdollActivateGeneration = {};
				record->failedRagdollActivatePublicationGeneration = publicationGeneration;
				record->failedRagdollActivateFrameTick = frameInputs.frameTick;
			}
			if(status != AnimationStatus::Queued)
				PublishRagdollExclusionForRecordLocked(*record, request.correlationId);
		}
	}

	void AnimationSystem::Impl::PublishDeterminismCompletionsLocked(std::uint64_t publicationGeneration) noexcept {
		for(const NativeAnimationDeterminismRequest& request : frameInputs.capturedDeterminismRequests) {
			RuntimeRecord* record = FindRuntimeRecordLocked({request.animator.slotIndex, request.animator.generation});
			NativeAnimationDeterminismSnapshot snapshot{};
			snapshot.header = MakeNativeHeader(NativeAnimationRecordKind::DeterminismSnapshot, sizeof(snapshot));
			snapshot.animator = request.animator;
			snapshot.tick = frameInputs.frameTick;
			snapshot.publicationGeneration = publicationGeneration;
			AnimationStatus status = AnimationStatus::StaleHandle;
			if(record != nullptr && instances->IsAlive(record->handle)) {
				const auto completion =
					std::find_if(frameInputs.determinismRequestCompletions.begin(), frameInputs.determinismRequestCompletions.end(),
								 [&request](const DeterminismRequestCompletion& value) {
									 return value.request.correlationId == request.correlationId &&
											value.request.animator.slotIndex == request.animator.slotIndex &&
											value.request.animator.generation == request.animator.generation;
								 });
				if(completion == frameInputs.determinismRequestCompletions.end()) {
					status = AnimationStatus::InternalError;
				} else {
					status = completion->status;
					AnimationRollbackSnapshot currentObserved;
					const AnimationRollbackSnapshot* observed = nullptr;
					if((request.operation == NativeAnimationDeterminismOperation::Capture ||
						request.operation == NativeAnimationDeterminismOperation::RequestReplication) &&
					   status == AnimationStatus::Ok) {
						observed = &completion->sourceSnapshot;
					} else if((request.operation == NativeAnimationDeterminismOperation::Restore ||
							   request.operation == NativeAnimationDeterminismOperation::ReplaySeek) &&
							  status == AnimationStatus::Ok) {
						const std::shared_ptr<const AnimationAuthorityFrame> authority = authorityPublisher.Load();
						const AnimationAuthorityRow* published = authority ? authority->Find(record->handle) : nullptr;
						status = BuildRollbackSnapshotLocked(*record, published, currentObserved);
						if(status == AnimationStatus::Ok) {
							observed = &currentObserved;
							const std::uint64_t expectedPose = RuntimePoseChecksum(completion->sourceSnapshot);
							if(expectedPose == 0U || RuntimePoseChecksum(*observed) != expectedPose) {
								status = AnimationStatus::DeterminismViolation;
							}
						}
					} else if(status == AnimationStatus::DeterminismViolation && RuntimePoseChecksum(completion->sourceSnapshot) != 0U) {
						// Mismatch diagnostics retain the rejected source identity without
						// implying that its state became live.
						observed = &completion->sourceSnapshot;
					}

					if(observed != nullptr) {
						snapshot.tick = observed->tick;
						snapshot.stateChecksum = RuntimeStateChecksum(*observed);
						snapshot.poseChecksum = RuntimePoseChecksum(*observed);
						if(!observed->controllers.empty()) {
							snapshot.controllerStateId = observed->controllers.front().activeStateId;
						}
						snapshot.controllerGeneration = record->binding.programGeneration;
						snapshot.eventWatermark =
							static_cast<std::uint32_t>(std::min<std::uint64_t>(observed->eventWatermark.deliveredSequence, UINT32_MAX));
						if(request.expectedChecksum != 0U && request.expectedChecksum != snapshot.poseChecksum) {
							status = AnimationStatus::DeterminismViolation;
						}
					} else if(request.operation == NativeAnimationDeterminismOperation::Capture ||
							  request.operation == NativeAnimationDeterminismOperation::RequestReplication) {
						// An unbound or compatibility-fallback animator has no valid asset
						// fingerprint and therefore cannot enter rollback history. Preserve
						// useful copied diagnostics while the non-Ok status makes that lack of
						// restorability explicit to the caller.
						const std::shared_ptr<const AnimationAuthorityFrame> authority = authorityPublisher.Load();
						const AnimationAuthorityRow* published = authority ? authority->Find(record->handle) : nullptr;
						if(published && published->snapshot && published->snapshot->pose) {
							snapshot.tick = published->snapshot->tick;
							snapshot.stateChecksum = RuntimeStateChecksum(*record);
							snapshot.poseChecksum = RuntimePoseChecksum(*published->snapshot->pose);
							snapshot.controllerStateId = record->binding.stateId;
							snapshot.controllerGeneration = record->binding.programGeneration;
							snapshot.eventWatermark =
								static_cast<std::uint32_t>(std::min<std::size_t>(published->snapshot->pose->Events().size(), UINT32_MAX));
						}
					}
					if(status == AnimationStatus::DeterminismViolation) {
						snapshot.divergenceKind = 1U;
					}
				}
			}
			snapshot.status = ToNativeStatus(status);
			(void)advancedCommands->Publish(std::move(snapshot));
		}
	}

	void AnimationSystem::Impl::PublishAnimationAuthorityStage(std::size_t animatorCount, std::uint64_t runtimeSessionEpoch,
															   std::uint64_t frameDeclarationEpoch,
															   TerraEngine::Scheduling::JobGraphContext& context) {

		TERRAENGINE_PROFILE_SCOPE("Animation: Publish Animation Authority", TerraEngine::Profiling::ProfilerModuleId::CpuEngine);
		if(context.IsCancellationRequested()) {
			this->RequestFrameCancellation();
			return;
		}
		if(!this->runtimeActive.load(std::memory_order_acquire) ||
		   this->runtimeSessionEpoch.load(std::memory_order_acquire) != runtimeSessionEpoch ||
		   this->frameDeclarationEpoch.load(std::memory_order_acquire) != frameDeclarationEpoch ||
		   this->lastPublishedFrameDeclarationEpoch == frameDeclarationEpoch ||
		   (!this->authorityState.sealedAuthorityCandidateReady && !this->authorityState.retainsPublishedAuthorityRoot))
			return;
		std::scoped_lock lock(this->runtimeOwnershipMutex);
		if(this->authorityState.retainsPublishedAuthorityRoot) {
			// Cache-only and compatible hold-last frames have already proved that every
			// observable authority fact is present in the retained root. Mark this
			// scheduler epoch complete without committing state or swapping a root.
			this->lastPublishedFrameDeclarationEpoch = frameDeclarationEpoch;
			return;
		}
		const Impl::PublishedAuthorityCommitToken token = this->authorityState.publishedAuthorityCommit;
		this->frameInputs.commandFrameFailurePhase = "PublishAnimationAuthorityGenerationGate";
		if(token.authorityGeneration == 0U || token.animatorCount != animatorCount || token.frameEpoch != frameDeclarationEpoch ||
		   token.sessionGeneration != runtimeSessionEpoch || !token.expectedRuntimeRoot || !token.nextRuntimeRoot ||
		   !token.expectedCompactRuntimeRoot || !token.nextCompactRuntimeRoot || !token.specialistSidecars ||
		   this->runtimeRecords.load(std::memory_order_acquire) != token.expectedRuntimeRoot ||
		   this->compactRuntimeStates.load(std::memory_order_acquire) != token.expectedCompactRuntimeRoot ||
		   token.sceneLifecycleGeneration != this->frameInputs.frameLifecycleGenerations.Generation(AnimationLifecycleBoundary::Scene)) {
			return;
		}
		// Capture qualified individual commands against the controller generation
		// and sealed the binding-resource revision before those checks. A concurrent
		// resource publication changes this one scalar even if it races Capture;
		// the protected authority exchange remains constant-time for large crowds.
		if(this->frameInputs.qualifiedControllerResourceRevision != 0U &&
		   ResourceManager::Instance().GetAnimationBindingResourceRevision() != this->frameInputs.qualifiedControllerResourceRevision) {
			this->RejectDrainedFrameCommandsLocked();
			this->authorityState.sealedAuthorityCandidate = {};
			this->authorityState.sealedAuthorityCandidateReady = false;
			this->authorityState.publishedAuthorityCommit = {};
			return;
		}
		const AnimationPublicationReceipt authorityReceipt = this->authorityPublisher.Publish(
			this->authorityState.sealedAuthorityCandidate, runtimeSessionEpoch,
			this->frameInputs.frameLifecycleGenerations.Generation(AnimationLifecycleBoundary::Scene));
		if(!authorityReceipt.Published()) {
			this->frameInputs.commandFrameFailurePhase = "AuthorityPublisherRejected";
			return;
		}
		// The expected-root identity check above proves no lifecycle or authored
		// mutation replaced evaluator state after worker sealing.  The store is the
		// complete runtime-state commit; it performs no record traversal or allocation.
		this->runtimeRecords.store(token.nextRuntimeRoot, std::memory_order_release);
		this->compactRuntimeStates.store(token.nextCompactRuntimeRoot, std::memory_order_release);
		// The publisher is deliberately the whole protected publication boundary:
		// it validates the sealed predecessor/session receipt and atomically swaps
		// one immutable root.  Crowd-sized internal state and specialist batches
		// are deferred to the dependent finalizer below.
		this->authorityState.publishedAuthorityCommit.ready = true;
		this->frameInputs.commandFrameFailurePhase = nullptr;
		this->authorityPublicationGeneration.store(token.authorityGeneration, std::memory_order_release);
	}

	void AnimationSystem::Impl::CommitPublishedAnimationCacheStage(std::size_t animatorCount, std::uint64_t runtimeSessionEpoch,
																   std::uint64_t frameDeclarationEpoch,
																   TerraEngine::Scheduling::JobGraphContext& context) {

		TERRAENGINE_PROFILE_SCOPE("Animation: Commit Published Evaluation Cache", TerraEngine::Profiling::ProfilerModuleId::CpuEngine);
		// Once authority is published, its internal scalar runtime commit is
		// mandatory. A late graph cancellation may suppress optional cache work on
		// the next frame, but must not leave evaluator progress behind authority.
		(void)context;
		const Impl::PublishedAuthorityCommitToken token = this->authorityState.publishedAuthorityCommit;
		(void)this->CommitPublishedEvaluationCacheEntries(animatorCount, frameDeclarationEpoch, token.authorityGeneration,
														  runtimeSessionEpoch);
	}

	void AnimationSystem::Impl::FinalizePublishedAnimationStage(std::size_t animatorCount, std::uint64_t runtimeSessionEpoch,
																std::uint64_t frameDeclarationEpoch,
																TerraEngine::Scheduling::JobGraphContext& context) {

		if(context.IsCancellationRequested()) {
			std::scoped_lock lock(this->runtimeOwnershipMutex);
			this->PublishCommittedOrdinaryCommandsLocked();
			this->SettleFailedFrameCommandsLocked(AnimationStatus::Cancelled, "FinalizeGraphCancelled");
			return;
		}
		// Drain previous completed physical ticks before opening the next
		// authority publication. Physics never calls back under its mutex.
		AnimationRootMotionResultPublisher::Instance().ObserveRootMotionIntervalResults(Physics::PhysicsSystem::Instance());
		std::scoped_lock lock(this->runtimeOwnershipMutex);
		// Ordinary command state already belongs to the committed runtime root.
		// Its exact receipt must precede fallible specialist side effects; failed
		// settlement below only changes commands that remain genuinely uncommitted.
		this->PublishCommittedOrdinaryCommandsLocked();
		Impl::PublishedAuthorityCommitToken token = this->authorityState.publishedAuthorityCommit;
		if(!token.ready || token.animatorCount != animatorCount || token.frameEpoch != frameDeclarationEpoch ||
		   token.sessionGeneration != runtimeSessionEpoch ||
		   token.sceneLifecycleGeneration != this->frameInputs.frameLifecycleGenerations.Generation(AnimationLifecycleBoundary::Scene) ||
		   !this->runtimeActive.load(std::memory_order_acquire) || !token.specialistSidecars) {
			this->SettleFailedFrameCommandsLocked(AnimationStatus::Rejected, "FinalizeAuthorityGate");
			return;
		}
		if(!this->ApplySealedSpecialistSidecars(token)) {
			this->SettleFailedFrameCommandsLocked(AnimationStatus::Rejected, "ApplySpecialistSidecars");
			return;
		}
		// The accepted packet has now consumed every copied explicit root-motion
		// request. Clear the live capture queue only after that success point.
		this->pendingRootMotionApplications.clear();
		this->authorityState.publishedAuthorityCommit = {};
		this->CaptureRollbackHistoryLocked(token.specialistSidecars->rollbackHistoryAnimators);
		this->PublishCommandCompletionsLocked(token.authorityGeneration);
		this->PublishProceduralCommandCompletionsLocked(token.authorityGeneration);
		this->PublishAdvancedCommandCompletionsLocked(token.authorityGeneration);
		this->evaluationState.publishedAnimatorCount.store(animatorCount, std::memory_order_release);
		// Worker sealing already copied diagnostics and lowered the GPU owner tables.
		// Protected work performs only bounded publication/submission operations.
		if(this->authorityState.preparedDetailedDiagnostics) {
			(void)PublishAnimationDetailedDiagnosticsSnapshot(*this->authorityState.preparedDetailedDiagnostics);
		}
		if(this->authorityState.preparedDiagnostics && PublishAnimationDiagnosticsSnapshot(*this->authorityState.preparedDiagnostics)) {
			TerraEngine::Profiling::Profiler::Instance().RecordAnimationDiagnostics(*this->authorityState.preparedDiagnostics);
		}
		// Presentation submission is a separate protected consumer boundary. A
		// rejection (for example a newer frame or shutdown) never rolls back the
		// CPU-authoritative state committed immediately above.
		if(this->authorityState.preparedPresentationFrame) {
			TERRAENGINE_PROFILE_SCOPE("Animation: Submit GPU Presentation Frame", TerraEngine::Profiling::ProfilerModuleId::CpuEngine);
			(void)AnimationPresentationSystem::Instance().SubmitGpuAnimationPresentationFrame(
				this->authorityState.preparedPresentationFrame);
		}
	}

} // namespace TerraEngine::Animation
