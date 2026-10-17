#include "Animation/Frame/AnimationFrameRuntime.h"

namespace TerraEngine::Animation {
	AnimationTransform AnimationSystem::Impl::ReferenceTransformForStableId(const FrameWorkItem& item,
																			std::uint64_t stableId) const noexcept {
		if(item.binding.skeleton) {
			for(const SkeletonJoint& joint : item.binding.skeleton->joints) {
				if(joint.stableId == stableId)
					return joint.localBindTransform;
			}
		}
		return {};
	}

	void AnimationSystem::Impl::BuildModelTransforms(const FrameWorkItem& item, std::span<const AnimationTransform> local,
													 std::span<AnimationTransform> model) const noexcept {
		for(std::size_t index = 0U; index < local.size(); ++index) {
			model[index] = local[index];
			if(!item.binding.skeleton || index >= item.binding.jointStableIds.size())
				continue;
			const std::uint64_t stableId = item.binding.jointStableIds[index];
			const auto joint = std::find_if(item.binding.skeleton->joints.begin(), item.binding.skeleton->joints.end(),
											[stableId](const SkeletonJoint& value) { return value.stableId == stableId; });
			if(joint == item.binding.skeleton->joints.end() || joint->parentIndex < 0 ||
			   static_cast<std::size_t>(joint->parentIndex) >= item.binding.skeleton->joints.size())
				continue;
			const std::uint64_t parentStableId = item.binding.skeleton->joints[static_cast<std::size_t>(joint->parentIndex)].stableId;
			const auto parent = std::find(item.binding.jointStableIds.begin(), item.binding.jointStableIds.end(), parentStableId);
			if(parent == item.binding.jointStableIds.end())
				continue;
			const std::size_t parentIndex = static_cast<std::size_t>(std::distance(item.binding.jointStableIds.begin(), parent));
			if(parentIndex < index)
				model[index] = CombineTransform(model[parentIndex], local[index]);
		}
	}

	bool AnimationSystem::Impl::BuildSkeletonModelTransforms(const SkeletonAsset& skeleton, std::span<const AnimationTransform> local,
															 std::vector<AnimationTransform>& model) const {
		if(local.size() != skeleton.joints.size())
			return false;
		model.resize(local.size());
		for(std::size_t index = 0U; index < local.size(); ++index) {
			const std::int32_t parent = skeleton.joints[index].parentIndex;
			if(parent < 0) {
				model[index] = local[index];
			} else if(static_cast<std::size_t>(parent) < index) {
				model[index] = CombineTransform(model[static_cast<std::size_t>(parent)], local[index]);
			} else {
				return false;
			}
		}
		return true;
	}

	AnimationStatus AnimationSystem::Impl::ApplyRetargetStage(FrameWorkItem& item, const CpuClipEvaluationResult& sampled,
															  std::span<AnimationTransform> targetLocal,
															  std::span<AnimationTransform> targetModel) const {
		if(!item.retargetActive || !item.sampledSourceSkeleton) {
			return AnimationStatus::Ok;
		}
		if(!item.binding.compiledRetargetProfile || !item.binding.retargetProfile || !item.binding.retargetSourceSkeleton ||
		   !item.binding.skeleton || sampled.joints.size() != item.binding.retargetSourceSkeleton->joints.size() ||
		   targetLocal.size() != item.binding.skeleton->joints.size() || targetModel.size() != targetLocal.size()) {
			return AnimationStatus::IncompatibleAsset;
		}

		try {
			auto sourcePose = std::make_shared<RetargetPoseSnapshot>();
			sourcePose->skeleton = {item.binding.retargetSourceSkeleton->assetId, item.binding.retargetSourceSkeletonGeneration};
			sourcePose->poseGeneration = item.candidatePoseGeneration;
			sourcePose->tick = item.playback.AbsoluteTick();
			sourcePose->localTransforms.reserve(sampled.joints.size());
			for(const CpuClipJointSample& joint : sampled.joints) {
				sourcePose->localTransforms.push_back(joint.localTransform);
			}
			if(!BuildSkeletonModelTransforms(*item.binding.retargetSourceSkeleton, sourcePose->localTransforms,
											 sourcePose->modelTransforms)) {
				return AnimationStatus::IncompatibleAsset;
			}

			auto targetReference = std::make_shared<RetargetPoseSnapshot>();
			targetReference->skeleton = {item.binding.skeleton->assetId, item.binding.skeletonGeneration};
			targetReference->poseGeneration = item.binding.skeletonGeneration;
			targetReference->tick = item.input.tick;
			targetReference->localTransforms.reserve(item.binding.skeleton->joints.size());
			for(const SkeletonJoint& joint : item.binding.skeleton->joints) {
				targetReference->localTransforms.push_back(joint.localBindTransform);
			}
			if(!BuildSkeletonModelTransforms(*item.binding.skeleton, targetReference->localTransforms, targetReference->modelTransforms)) {
				return AnimationStatus::IncompatibleAsset;
			}

			RetargetGenerationStamp generation;
			generation.profile = {item.binding.retargetProfile->assetId, item.binding.retargetProfileGeneration};
			generation.sourceSkeleton = sourcePose->skeleton;
			generation.targetSkeleton = targetReference->skeleton;
			generation.sourcePoseGeneration = sourcePose->poseGeneration;
			generation.targetReferencePoseGeneration = targetReference->poseGeneration;
			generation.publicationGeneration = item.candidatePoseGeneration;
			// A retarget packet is timed by the sampled source pose, not the
			// enclosing scheduler frame. Clip looping and playback-rate scaling
			// can make those ticks differ, and the constraint snapshot deliberately
			// rejects goals copied for any other source pose tick.
			generation.tick = sourcePose->tick;

			RetargetEvaluationRequest request;
			request.profile = item.binding.compiledRetargetProfile;
			request.sourceSkeleton = item.binding.retargetSourceSkeleton;
			request.targetSkeleton = item.binding.skeleton;
			request.sourcePose = std::move(sourcePose);
			request.targetReferencePose = std::move(targetReference);
			request.generation = generation;
			request.expectedGeneration = generation;
			request.normalizedTime = item.playback.Snapshot().normalizedTime;
			RetargetEvaluationResult result;
			const AnimationStatus status = EvaluateRetargetPose(request, result);
			if(status != AnimationStatus::Ok || result.localTransforms.size() != targetLocal.size() ||
			   result.modelTransforms.size() != targetModel.size()) {
				return status == AnimationStatus::Ok ? AnimationStatus::InternalError : status;
			}
			std::copy(result.localTransforms.begin(), result.localTransforms.end(), targetLocal.begin());
			std::copy(result.modelTransforms.begin(), result.modelTransforms.end(), targetModel.begin());
			item.retargetMetrics = result.metrics;
			item.retargetDeterministicHash = result.deterministicHash;
			return AnimationStatus::Ok;
		} catch(const std::bad_alloc&) {
			return AnimationStatus::InternalError;
		}
	}

	std::int16_t AnimationSystem::Impl::QuantizeMotionFeature(float value, float scale, float offset) noexcept {
		if(!std::isfinite(value) || !std::isfinite(scale) || !std::isfinite(offset) || scale <= 0.0F)
			return 0;
		const float normalized = (value - offset) / scale - 32767.0F;
		return static_cast<std::int16_t>(std::clamp(std::lround(normalized), static_cast<long>(std::numeric_limits<std::int16_t>::min()),
													static_cast<long>(std::numeric_limits<std::int16_t>::max())));
	}

	float AnimationSystem::Impl::MotionPoseFeature(std::span<const AnimationTransform> modelPose, std::size_t featureIndex) noexcept {
		constexpr std::size_t kTransformFeatureCount = 10U;
		const std::size_t jointIndex = featureIndex / kTransformFeatureCount;
		if(jointIndex >= modelPose.size())
			return 0.0F;
		const AnimationTransform& transform = modelPose[jointIndex];
		switch(featureIndex % kTransformFeatureCount) {
		case 0U:
			return transform.translation.x;
		case 1U:
			return transform.translation.y;
		case 2U:
			return transform.translation.z;
		case 3U:
			return transform.rotation.x;
		case 4U:
			return transform.rotation.y;
		case 5U:
			return transform.rotation.z;
		case 6U:
			return transform.rotation.w;
		case 7U:
			return transform.scale.x;
		case 8U:
			return transform.scale.y;
		default:
			return transform.scale.z;
		}
	}

	void AnimationSystem::Impl::EvaluateMotionMatchRequests(FrameWorkItem& item,
															std::span<const AnimationTransform> modelPose) const noexcept {
		if(item.pendingMotionQueries.empty())
			return;
		try {
			item.evaluatedMotionQueries.clear();
			item.evaluatedMotionQueries.reserve(item.pendingMotionQueries.size());
			for(const NativeAnimationMotionMatchRequest& native : item.pendingMotionQueries) {
				NativeAnimationMotionMatchSnapshot snapshot{};
				snapshot.header = MakeNativeHeader(NativeAnimationRecordKind::MotionMatchSnapshot, sizeof(snapshot));
				snapshot.animator = native.animator;
				snapshot.databaseAssetId = item.activeMotionDatabaseId;
				snapshot.databaseGeneration = item.activeMotionDatabaseGeneration;
				snapshot.queryId = native.queryId;
				snapshot.observationTick = item.input.tick;
				snapshot.publicationGeneration = item.candidateAuthorityPublicationGeneration;

				AnimationStatus status = item.evaluationStatus == AnimationStatus::Ok ? AnimationStatus::NotReady : item.evaluationStatus;
				const MotionDatabaseAsset* database = item.binding.motionDatabase.get();
				if(item.evaluationStatus == AnimationStatus::Ok && database != nullptr && item.activeMotionDatabaseGeneration != 0U &&
				   MatchesNativeUuid(item.activeMotionDatabaseId, database->assetId.value) &&
				   MatchesNativeUuid(native.databaseAssetId, database->assetId.value) &&
				   native.databaseGeneration == item.activeMotionDatabaseGeneration &&
				   item.activeMotionDatabaseGeneration == item.binding.motionDatabaseGeneration &&
				   native.featureCount == kMotionMatchFeatureCountV1) {
					MotionMatchQuery query;
					query.database = {database->assetId, item.activeMotionDatabaseGeneration};
					query.expectedSchemaFingerprint = database->featureSchemaFingerprint;
					query.featureWeights.fill(1U);
					query.maximumCandidates = kMotionDatabaseTopKDefault;
					const MotionDatabaseSample* priorSample = nullptr;
					if(item.hasMotionCandidate) {
						const auto prior =
							std::find_if(database->samples.begin(), database->samples.end(), [&item](const MotionDatabaseSample& sample) {
								return sample.stableSampleId == item.latestMotionCandidate.stableSampleId;
							});
						if(prior != database->samples.end())
							priorSample = &*prior;
					}
					if(priorSample != nullptr) {
						query.features = priorSample->features;
						query.preferredPhase = priorSample->phaseQuantized;
						query.preferredContinuityGroup = priorSample->continuityGroup;
						query.lastSampleId = priorSample->stableSampleId;
						query.lastSourceClipId = priorSample->sourceClipId;
					} else {
						for(std::size_t feature = 0U; feature < kMotionDatabaseFeatureCount; ++feature) {
							query.features.values[feature] =
								QuantizeMotionFeature(MotionPoseFeature(modelPose, feature), database->quantizationScales[feature],
													  database->quantizationOffsets[feature]);
						}
						query.preferredPhase =
							static_cast<std::uint16_t>(std::clamp(item.playback.Snapshot().normalizedTime * 65535.0F, 0.0F, 65535.0F));
					}

					MotionMatchCpuResult scored;
					status = ScoreMotionMatchCpu(*database, query, scored);
					if(status == AnimationStatus::Ok && !scored.candidates.empty()) {
						const MotionMatchCandidate& chosen = scored.candidates.front();
						snapshot.selectedSampleId = chosen.stableSampleId;
						const auto clip =
							std::lower_bound(item.binding.controllerClips.begin(), item.binding.controllerClips.end(), chosen.sourceClipId,
											 [](const AnimatorControllerClipBinding& candidate, std::uint64_t sourceClipId) {
												 return candidate.nodeStableId < sourceClipId;
											 });
						if(clip != item.binding.controllerClips.end() && clip->nodeStableId == chosen.sourceClipId) {
							snapshot.selectedClipGeneration = clip->clipGeneration;
						}
						const std::uint64_t trajectoryCost =
							chosen.score.phaseCost + chosen.score.continuityCost + chosen.score.transitionCost;
						snapshot.totalScore = static_cast<float>(chosen.score.totalCost);
						snapshot.trajectoryScore = static_cast<float>(trajectoryCost);
						snapshot.poseScore = static_cast<float>(chosen.score.featureCost);
						item.latestMotionCandidate = chosen;
						item.latestMotionQueryId = native.queryId;
						item.hasMotionCandidate = true;
					} else {
						snapshot.usedFallback = 1U;
					}
				} else if(database == nullptr) {
					status = AnimationStatus::MissingAsset;
				} else if(native.featureCount != kMotionMatchFeatureCountV1) {
					status = AnimationStatus::InvalidArgument;
				} else if(item.activeMotionDatabaseGeneration != 0U) {
					status = AnimationStatus::StaleHandle;
				}
				snapshot.status = ToNativeStatus(status);
				item.evaluatedMotionQueries.push_back(snapshot);
			}
		} catch(const std::bad_alloc&) {
			item.evaluatedMotionQueries.clear();
		}
	}

	void AnimationSystem::Impl::EvaluateMotionMatchQueriesV2(FrameWorkItem& item) const noexcept {
		if(item.pendingMotionQueriesV2.empty())
			return;
		try {
			item.evaluatedMotionQueriesV2.clear();
			item.evaluatedMotionQueriesV2.reserve(item.pendingMotionQueriesV2.size());
			for(const NativeAnimationMotionMatchQueryRequestV2& native : item.pendingMotionQueriesV2) {
				MotionMatchQueryCompletionV2 completion;
				completion.correlationId = native.correlationId;
				NativeAnimationMotionMatchResultV2& result = completion.result;
				result.header = MakeNativeHeader(NativeAnimationRecordKind::MotionMatchResultV2, sizeof(result));
				result.animator = native.animator;
				result.databaseAssetId = native.databaseAssetId;
				result.databaseGeneration = native.databaseGeneration;
				result.schemaFingerprint = native.schemaFingerprint;
				result.queryId = native.queryId;
				result.observationTick = item.input.tick;
				result.publicationGeneration = item.candidateAuthorityPublicationGeneration;
				result.populatedFields = static_cast<std::uint32_t>(NativeAnimationMotionMatchResultField::Identity);

				AnimationStatus status = item.evaluationStatus == AnimationStatus::Ok ? AnimationStatus::NotReady : item.evaluationStatus;
				const MotionDatabaseAsset* database = item.binding.motionDatabase.get();
				if(item.evaluationStatus == AnimationStatus::Ok && database == nullptr) {
					status = AnimationStatus::MissingAsset;
				} else if(item.evaluationStatus == AnimationStatus::Ok &&
						  !MatchesNativeUuid(native.databaseAssetId, database->assetId.value)) {
					status = AnimationStatus::IncompatibleAsset;
				} else if(item.evaluationStatus == AnimationStatus::Ok &&
						  (native.databaseGeneration != item.binding.motionDatabaseGeneration ||
						   native.databaseGeneration != database->runtimeGeneration)) {
					status = AnimationStatus::StaleHandle;
				} else if(item.evaluationStatus == AnimationStatus::Ok && native.schemaFingerprint != database->featureSchemaFingerprint) {
					status = AnimationStatus::IncompatibleAsset;
				} else if(item.evaluationStatus == AnimationStatus::Ok) {
					result.populatedFields |= static_cast<std::uint32_t>(NativeAnimationMotionMatchResultField::DatabaseSchema);
					MotionMatchQuery query;
					query.database = {database->assetId, native.databaseGeneration};
					query.expectedSchemaFingerprint = native.schemaFingerprint;
					for(std::size_t feature = 0U; feature < kMotionDatabaseFeatureCount; ++feature) {
						query.features.values[feature] = native.features[feature];
						query.featureWeights[feature] = native.featureWeights[feature];
					}

					auto hasField = [&native](NativeAnimationMotionMatchQueryField field) {
						return (native.queryFields & static_cast<std::uint32_t>(field)) != 0U;
					};
					// Optional groups are applied only when their field bit is present. This
					// keeps the receipt's scheduledFields mask an honest list of consumed data.
					if(hasField(NativeAnimationMotionMatchQueryField::Tags)) {
						query.requiredTags = native.requiredTags;
						query.excludedTags = native.excludedTags;
						query.preferredTags = native.preferredTags;
					}
					if(hasField(NativeAnimationMotionMatchQueryField::Contacts)) {
						query.requiredContacts = native.requiredContacts;
						query.preferredContacts = native.preferredContacts;
					}
					if(hasField(NativeAnimationMotionMatchQueryField::Phase)) {
						query.preferredPhase = native.preferredPhase;
						query.maximumPhaseDistance = native.maximumPhaseDistance;
					}
					if(hasField(NativeAnimationMotionMatchQueryField::Transitions)) {
						query.requiredTransitionFlags = native.requiredTransitionFlags;
						query.excludedTransitionFlags = native.excludedTransitionFlags;
						query.preferredTransitionFlags = native.preferredTransitionFlags;
					}
					if(hasField(NativeAnimationMotionMatchQueryField::Continuity)) {
						query.preferredContinuityGroup = native.preferredContinuityGroup;
						query.lastSampleId = native.lastSampleId;
						query.lastSourceClipId = native.lastSourceClipId;
					}
					if(hasField(NativeAnimationMotionMatchQueryField::Exclusions)) {
						query.excludedRegionFlags = native.excludedRegionFlags;
						query.excludedSampleIds = {native.excludedSampleIds, native.excludedSampleCount};
					}
					if(hasField(NativeAnimationMotionMatchQueryField::Costs)) {
						query.phaseWeight = native.phaseWeight;
						query.continuityPenalty = native.continuityPenalty;
						query.continuityBreakPenalty = native.continuityBreakPenalty;
						query.sameClipPenalty = native.sameClipPenalty;
						query.missingPreferredTagPenalty = native.missingPreferredTagPenalty;
						query.contactMismatchPenalty = native.contactMismatchPenalty;
						query.transitionMismatchPenalty = native.transitionMismatchPenalty;
					}
					if(hasField(NativeAnimationMotionMatchQueryField::Budgets)) {
						query.maximumCandidates = native.maximumCandidates;
						query.maximumSamplesToVisit = native.maximumSamplesToVisit;
						query.maximumDiagnosticExclusions = native.maximumDiagnosticExclusions;
					}

					MotionMatchCpuResult scored;
					status = ScoreMotionMatchCpu(*database, query, scored);
					result.gpuStatus = NativeAnimationStatus::RendererUnavailable;
					result.selectionSource = NativeAnimationMotionMatchSelectionSource::CpuFullSearch;
					result.fallbackReason = scored.fallbackReason == MotionMatchFallbackReason::None
												? NativeAnimationMotionMatchFallbackReason::GpuUnavailable
												: static_cast<NativeAnimationMotionMatchFallbackReason>(scored.fallbackReason);
					result.flags = static_cast<std::uint32_t>(NativeAnimationMotionMatchResultFlag::UsedFallback) |
								   static_cast<std::uint32_t>(NativeAnimationMotionMatchResultFlag::UsedCpuFallback);
					if(scored.candidateBudgetExhausted) {
						result.flags |= static_cast<std::uint32_t>(NativeAnimationMotionMatchResultFlag::CandidateBudgetExhausted);
					}
					result.visitedSamples = scored.visitedSamples;
					result.evaluatedSamples = scored.evaluatedSamples;
					result.excludedSamples = scored.excludedSamples;
					for(std::size_t reason = 0U; reason < scored.exclusionReasonCounts.size(); ++reason) {
						if(scored.exclusionReasonCounts[reason] != 0U) {
							result.exclusionReasonMask |= 1U << reason;
						}
					}
					result.populatedFields |= static_cast<std::uint32_t>(NativeAnimationMotionMatchResultField::Exclusions) |
											  static_cast<std::uint32_t>(NativeAnimationMotionMatchResultField::Fallback) |
											  static_cast<std::uint32_t>(NativeAnimationMotionMatchResultField::SelectionSource) |
											  static_cast<std::uint32_t>(NativeAnimationMotionMatchResultField::GpuStatus) |
											  static_cast<std::uint32_t>(NativeAnimationMotionMatchResultField::CandidateCounts);

					if(status == AnimationStatus::Ok && !scored.candidates.empty()) {
						const MotionMatchCandidate& chosen = scored.candidates.front();
						result.selectedSampleId = chosen.stableSampleId;
						result.selectedSourceClipId = chosen.sourceClipId;
						result.selectedSourceTick = chosen.sourceTick;
						const auto clip =
							std::lower_bound(item.binding.controllerClips.begin(), item.binding.controllerClips.end(), chosen.sourceClipId,
											 [](const AnimatorControllerClipBinding& candidate, std::uint64_t sourceClipId) {
												 return candidate.nodeStableId < sourceClipId;
											 });
						if(clip != item.binding.controllerClips.end() && clip->nodeStableId == chosen.sourceClipId) {
							result.selectedClipGeneration = clip->clipGeneration;
						}
						result.totalCost = chosen.score.totalCost;
						result.featureCost = chosen.score.featureCost;
						for(std::size_t block = 0U; block < chosen.score.featureBlockCosts.size(); ++block) {
							result.featureBlockCosts[block] = chosen.score.featureBlockCosts[block];
						}
						result.phaseCost = chosen.score.phaseCost;
						result.tagCost = chosen.score.tagCost;
						result.contactCost = chosen.score.contactCost;
						result.continuityCost = chosen.score.continuityCost;
						result.transitionCost = chosen.score.transitionCost;
						result.populatedFields |= static_cast<std::uint32_t>(NativeAnimationMotionMatchResultField::SelectedSample) |
												  static_cast<std::uint32_t>(NativeAnimationMotionMatchResultField::SelectedSource) |
												  static_cast<std::uint32_t>(NativeAnimationMotionMatchResultField::TotalCost) |
												  static_cast<std::uint32_t>(NativeAnimationMotionMatchResultField::FeatureBlockCosts) |
												  static_cast<std::uint32_t>(NativeAnimationMotionMatchResultField::MetadataCosts);
						item.latestMotionCandidate = chosen;
						item.latestMotionQueryId = native.queryId;
						item.hasMotionCandidate = true;
						completion.selectedCandidate = chosen;
						completion.hasSelectedCandidate = true;
					}
				}
				result.status = ToNativeStatus(status);
				item.evaluatedMotionQueriesV2.push_back(std::move(completion));
			}
		} catch(const std::bad_alloc&) {
			item.evaluatedMotionQueriesV2.clear();
		}
	}

	AnimationStatus AnimationSystem::Impl::ApplyRetargetRootMotionPolicy(const FrameWorkItem& item,
																		 AnimationRootMotionDelta& delta) const noexcept {
		if(!item.retargetActive || !item.sampledSourceSkeleton || !item.binding.retargetProfile)
			return AnimationStatus::Ok;
		const RetargetProfileAsset& profile = *item.binding.retargetProfile;
		float proportionScale = 1.0F;
		switch(profile.proportion.translationScaleMode) {
		case RetargetTranslationScaleMode::None:
			break;
		case RetargetTranslationScaleMode::UniformHeight:
		case RetargetTranslationScaleMode::PerChainLength:
			if(profile.proportion.sourceReferenceHeightMeters <= 0.0F) {
				return AnimationStatus::IncompatibleAsset;
			}
			proportionScale = profile.proportion.targetReferenceHeightMeters / profile.proportion.sourceReferenceHeightMeters;
			break;
		case RetargetTranslationScaleMode::Authored:
			proportionScale = profile.proportion.authoredTranslationScale;
			break;
		}
		float displacementScale = profile.root.displacementScale;
		if(profile.root.motionMode == RetargetRootMotionMode::ScaleByProportion) {
			displacementScale *= proportionScale;
		} else if(profile.root.motionMode == RetargetRootMotionMode::InPlace) {
			displacementScale = 0.0F;
		}
		if(!std::isfinite(displacementScale))
			return AnimationStatus::RangeOverflow;
		auto transformTranslation = [&](AnimationFloat3& translation) {
			translation.x *= displacementScale * profile.trajectory.horizontalScale;
			translation.z *= displacementScale * profile.trajectory.horizontalScale;
			translation.y =
				profile.root.preserveVerticalDisplacement ? translation.y * displacementScale * profile.trajectory.verticalScale : 0.0F;
		};
		transformTranslation(delta.localTranslation);
		transformTranslation(delta.worldTranslation);
		if(!profile.root.preserveFacing || profile.root.motionMode == RetargetRootMotionMode::InPlace) {
			delta.localYawRadians = 0.0F;
			delta.worldYawRadians = 0.0F;
		}
		return IsFinite(delta.localTranslation) && IsFinite(delta.worldTranslation) && std::isfinite(delta.localYawRadians) &&
					   std::isfinite(delta.worldYawRadians)
				   ? AnimationStatus::Ok
				   : AnimationStatus::RangeOverflow;
	}

	bool AnimationSystem::Impl::BuildLocalTransformsFromModel(const FrameWorkItem& item, std::span<const AnimationTransform> model,
															  std::span<AnimationTransform> local) const noexcept {
		if(model.size() != local.size() || model.size() != item.binding.jointStableIds.size())
			return false;
		for(std::size_t index = 0U; index < model.size(); ++index) {
			const std::int32_t parent = index < item.binding.jointParents.size() ? item.binding.jointParents[index] : -1;
			if(parent < 0) {
				local[index] = model[index];
				continue;
			}
			if(static_cast<std::size_t>(parent) >= index ||
			   !ProceduralMath::TryRelativeTransform(model[static_cast<std::size_t>(parent)], model[index], local[index])) {
				return false;
			}
		}
		return true;
	}

	ProceduralGenerationStamp AnimationSystem::Impl::MakeProceduralGeneration(const FrameWorkItem& item) const noexcept {
		ProceduralGenerationStamp generation;
		generation.animator = item.input.handle;
		generation.componentGeneration = item.input.descriptor.componentGeneration;
		generation.skeletonGeneration = item.binding.skeletonGeneration;
		generation.programGeneration = item.binding.programGeneration;
		generation.poseGeneration = item.candidatePoseGeneration;
		generation.publicationGeneration = item.candidateAuthorityPublicationGeneration;
		generation.tick = item.input.tick;
		return generation;
	}

	std::uint32_t AnimationSystem::Impl::FindProceduralJointIndex(const FrameWorkItem& item, std::uint64_t stableJointId) const noexcept {
		const auto found = std::find(item.binding.jointStableIds.begin(), item.binding.jointStableIds.end(), stableJointId);
		return found == item.binding.jointStableIds.end()
				   ? kInvalidProceduralJointIndex
				   : static_cast<std::uint32_t>(std::distance(item.binding.jointStableIds.begin(), found));
	}

	AnimationStatus AnimationSystem::Impl::BuildProceduralNodes(const FrameWorkItem& item, const ProceduralGenerationStamp& generation,
																std::vector<ProceduralGraphNodeInput>& nodes) const {
		nodes.clear();
		nodes.reserve(item.activeProceduralGoals.size());
		for(const NativeAnimationProceduralGoalCommand& command : item.activeProceduralGoals) {
			const std::uint32_t jointIndex = FindProceduralJointIndex(item, command.targetJointStableId);
			if(jointIndex == kInvalidProceduralJointIndex) {
				return AnimationStatus::MissingAsset;
			}
			ProceduralGraphNodeInput node;
			node.stableId = command.goalId;
			node.layerOrder = command.layerIndex;
			node.nodeOrder = static_cast<std::uint32_t>(command.producerSequence & 0xffffffffU);
			node.nodeWeight = std::max(command.positionWeight, command.rotationWeight);
			const AnimationTransform target = FromNativeTransform(command.goalTransform);
			const auto space = static_cast<ProceduralCoordinateSpace>(command.coordinateSpace);
			const auto kind = static_cast<ProceduralGoalKind>(command.goalKind);
			const std::uint32_t rawFlags = static_cast<std::uint32_t>(command.flags);

			auto makeGoalSnapshot = [&]() {
				ProceduralGoal goal;
				goal.generation = generation;
				goal.target.stableTargetId = command.targetJointStableId;
				goal.goalId = command.goalId;
				goal.producerOrder = command.producerSequence;
				goal.priority = command.priority;
				goal.targetJointIndex = jointIndex;
				goal.kind = kind;
				goal.space = space;
				goal.position = target.translation;
				goal.rotation = target.rotation;
				goal.pole = {command.poleVector.x, command.poleVector.y, command.poleVector.z};
				goal.weight = node.nodeWeight;
				goal.effectiveTick = command.targetTick;
				ProceduralGoalSnapshot snapshot;
				(void)BuildProceduralGoalSnapshot(generation, std::span<const ProceduralGoal>(&goal, 1U), snapshot);
				return snapshot;
			};

			switch(command.goalKind) {
			case NativeAnimationProceduralGoalKind::Aim: {
				ProceduralAimNodeInput aim;
				aim.jointIndex = jointIndex;
				aim.goals = makeGoalSnapshot();
				aim.solverWeight = node.nodeWeight;
				node.payload = std::move(aim);
				break;
			}
			case NativeAnimationProceduralGoalKind::Look: {
				ProceduralLookNodeInput look;
				look.aim.jointIndex = jointIndex;
				look.aim.goals = makeGoalSnapshot();
				look.aim.solverWeight = node.nodeWeight;
				node.payload = std::move(look);
				break;
			}
			case NativeAnimationProceduralGoalKind::Twist: {
				ProceduralTwistNodeInput twist;
				twist.jointIndices.push_back(jointIndex);
				twist.distributionWeights.push_back(1.0F);
				twist.twistRadians = 2.0F * std::acos(std::clamp(target.rotation.w, -1.0F, 1.0F));
				twist.solverWeight = command.rotationWeight;
				twist.goalId = command.goalId;
				node.payload = std::move(twist);
				break;
			}
			case NativeAnimationProceduralGoalKind::Position:
			case NativeAnimationProceduralGoalKind::FootPlant: {
				const std::int32_t middle = jointIndex < item.binding.jointParents.size() ? item.binding.jointParents[jointIndex] : -1;
				const std::int32_t root = middle >= 0 && static_cast<std::size_t>(middle) < item.binding.jointParents.size()
											  ? item.binding.jointParents[static_cast<std::size_t>(middle)]
											  : -1;
				if(root >= 0 && middle >= 0) {
					ProceduralTwoBoneNodeInput twoBone;
					twoBone.rootJointIndex = static_cast<std::uint32_t>(root);
					twoBone.middleJointIndex = static_cast<std::uint32_t>(middle);
					twoBone.endJointIndex = jointIndex;
					twoBone.targetPosition = target.translation;
					twoBone.polePosition = {command.poleVector.x, command.poleVector.y, command.poleVector.z};
					twoBone.space = space;
					twoBone.goalId = command.goalId;
					twoBone.producerOrder = command.producerSequence;
					twoBone.priority = command.priority;
					twoBone.solverWeight = command.positionWeight;
					twoBone.maximumStretchScale = command.maximumStretchScale;
					twoBone.allowStretch = (rawFlags & static_cast<std::uint32_t>(NativeAnimationProceduralGoalFlag::AllowStretch)) != 0U;
					node.payload = twoBone;
					break;
				}
				[[fallthrough]];
			}
			case NativeAnimationProceduralGoalKind::Rotation:
			case NativeAnimationProceduralGoalKind::PositionAndRotation:
			case NativeAnimationProceduralGoalKind::CopyTransform: {
				ProceduralCopyTransformNodeInput copy;
				copy.jointIndex = jointIndex;
				copy.parentJointIndex = jointIndex < item.binding.jointParents.size() && item.binding.jointParents[jointIndex] >= 0
											? static_cast<std::uint32_t>(item.binding.jointParents[jointIndex])
											: kInvalidProceduralJointIndex;
				copy.targetTransform = target;
				copy.targetSpace = space;
				copy.translationWeight = command.goalKind == NativeAnimationProceduralGoalKind::Rotation ? 0.0F : command.positionWeight;
				copy.rotationWeight = command.goalKind == NativeAnimationProceduralGoalKind::Position ||
											  command.goalKind == NativeAnimationProceduralGoalKind::FootPlant
										  ? 0.0F
										  : command.rotationWeight;
				copy.scaleWeight = 0.0F;
				copy.goalId = command.goalId;
				node.payload = copy;
				break;
			}
			default:
				return AnimationStatus::Unsupported;
			}
			nodes.push_back(std::move(node));
		}
		return AnimationStatus::Ok;
	}

	NativeAnimationProceduralSolverKind AnimationSystem::Impl::NativeSolverForGoal(NativeAnimationProceduralGoalKind kind) const noexcept {
		switch(kind) {
		case NativeAnimationProceduralGoalKind::Aim:
			return NativeAnimationProceduralSolverKind::Aim;
		case NativeAnimationProceduralGoalKind::Look:
			return NativeAnimationProceduralSolverKind::Look;
		case NativeAnimationProceduralGoalKind::Twist:
			return NativeAnimationProceduralSolverKind::TwistDistribution;
		case NativeAnimationProceduralGoalKind::CopyTransform:
		case NativeAnimationProceduralGoalKind::Rotation:
		case NativeAnimationProceduralGoalKind::PositionAndRotation:
			return NativeAnimationProceduralSolverKind::CopyTransform;
		case NativeAnimationProceduralGoalKind::FootPlant:
			return NativeAnimationProceduralSolverKind::FootPlacement;
		default:
			return NativeAnimationProceduralSolverKind::TwoBoneIk;
		}
	}

	AnimationStatus AnimationSystem::Impl::ApplyProceduralStage(FrameWorkItem& item, GraphPoseContribution& pose) const {
		if(item.activeProceduralGoals.empty())
			return AnimationStatus::Ok;
		const ProceduralGenerationStamp generation = MakeProceduralGeneration(item);
		std::vector<ProceduralGraphNodeInput> nodes;
		const AnimationStatus nodeStatus = BuildProceduralNodes(item, generation, nodes);
		if(nodeStatus != AnimationStatus::Ok)
			return nodeStatus;
		ProceduralGraphEvaluationRequest request;
		request.generation = generation;
		request.expectedGeneration = generation;
		request.inputPose = &pose;
		request.nodes = nodes;
		request.authority = ProceduralGraphAuthority::CpuAuthoritative;
		request.authoritativeConsumer = true;
		request.rollbackRequested = item.executionDecision.consumerClosure.rollback;
		request.recordingRequested = item.executionDecision.consumerClosure.recording;
		ProceduralGraphEvaluationResult evaluated;
		const AnimationStatus status = EvaluateProceduralGraphNodes(request, evaluated);
		if(status != AnimationStatus::Ok)
			return status;
		pose = std::move(evaluated.pose);

		item.proceduralResults.clear();
		item.proceduralResults.reserve(item.activeProceduralGoals.size());
		for(const NativeAnimationProceduralGoalCommand& command : item.activeProceduralGoals) {
			NativeAnimationProceduralResultSnapshot snapshot{};
			snapshot.header = MakeNativeHeader(NativeAnimationRecordKind::ProceduralResultSnapshot, sizeof(snapshot));
			snapshot.status = ToNativeStatus(AnimationStatus::Ok);
			snapshot.animator = {item.input.handle.slotIndex, item.input.handle.generation};
			snapshot.solver = NativeSolverForGoal(command.goalKind);
			snapshot.state = NativeAnimationProceduralSolverState::NoContribution;
			snapshot.goalId = command.goalId;
			snapshot.targetJointStableId = command.targetJointStableId;
			const std::uint32_t jointIndex = FindProceduralJointIndex(item, command.targetJointStableId);
			if(jointIndex < pose.joints.size()) {
				snapshot.resultTransform = ToNativeTransform(pose.joints[jointIndex]);
			}
			snapshot.inputWeight = std::max(command.positionWeight, command.rotationWeight);
			const auto diagnostic =
				std::find_if(evaluated.diagnostics.begin(), evaluated.diagnostics.end(),
							 [&command](const ProceduralDiagnosticRecord& value) { return value.goalId == command.goalId; });
			if(diagnostic != evaluated.diagnostics.end()) {
				snapshot.state = static_cast<NativeAnimationProceduralSolverState>(diagnostic->state);
				snapshot.targetErrorMeters = diagnostic->targetErrorMeters;
				snapshot.targetErrorRadians = diagnostic->targetErrorRadians;
				snapshot.finalContribution = diagnostic->finalContribution;
				snapshot.iterationCount = diagnostic->iterationCount;
				snapshot.iterationLimit = diagnostic->iterationLimit;
				std::uint32_t flags = static_cast<std::uint32_t>(NativeAnimationProceduralResultFlag::CpuAuthoritative);
				if(diagnostic->converged)
					flags |= static_cast<std::uint32_t>(NativeAnimationProceduralResultFlag::Converged);
				if(diagnostic->clamped)
					flags |= static_cast<std::uint32_t>(NativeAnimationProceduralResultFlag::Clamped);
				if(diagnostic->singular)
					flags |= static_cast<std::uint32_t>(NativeAnimationProceduralResultFlag::Singular);
				if(diagnostic->unreachable)
					flags |= static_cast<std::uint32_t>(NativeAnimationProceduralResultFlag::Unreachable);
				if(diagnostic->staleInput)
					flags |= static_cast<std::uint32_t>(NativeAnimationProceduralResultFlag::StaleInput);
				snapshot.flags = static_cast<NativeAnimationProceduralResultFlag>(flags);
			} else {
				snapshot.flags = NativeAnimationProceduralResultFlag::CpuAuthoritative;
			}
			snapshot.poseGeneration = item.candidatePoseGeneration;
			snapshot.componentGeneration = item.input.descriptor.componentGeneration;
			snapshot.skeletonGeneration = item.binding.skeletonGeneration;
			snapshot.programGeneration = item.binding.programGeneration;
			snapshot.observationTick = item.input.tick;
			snapshot.publicationGeneration = item.candidateAuthorityPublicationGeneration;
			snapshot.producerOrder = command.producerSequence;
			snapshot.correlationId = command.correlationId;
			item.proceduralResults.push_back(snapshot);
		}
		return AnimationStatus::Ok;
	}

	AnimationStatus AnimationSystem::Impl::ApplyRagdollStage(FrameWorkItem& item, GraphPoseContribution& pose) const {
		if(item.ragdollLifecycle != RagdollLifecycleState::Active && item.ragdollLifecycle != RagdollLifecycleState::Recovering) {
			return AnimationStatus::Ok;
		}
		if(!item.ragdollPhysicsPose || !item.binding.ragdollProfile || !item.sourceObjectWorldTransformCaptured) {
			return AnimationStatus::NotReady;
		}
		if(item.binding.jointParents.size() != pose.jointStableIds.size() || pose.jointStableIds.size() != pose.joints.size()) {
			return AnimationStatus::IncompatibleAsset;
		}
		if(item.ragdollLifecycle == RagdollLifecycleState::Recovering) {
			const auto rootBody =
				std::find_if(item.binding.ragdollProfile->bodies.begin(), item.binding.ragdollProfile->bodies.end(),
							 [&item](const RagdollBodyRecord& value) { return value.bodyId == item.binding.ragdollProfile->rootBodyId; });
			if(rootBody != item.binding.ragdollProfile->bodies.end()) {
				const auto rootJoint = std::find(pose.jointStableIds.begin(), pose.jointStableIds.end(), rootBody->stableJointId);
				if(rootJoint != pose.jointStableIds.end()) {
					const std::size_t rootIndex = static_cast<std::size_t>(std::distance(pose.jointStableIds.begin(), rootJoint));
					AnimationTransform alignedWorld = CombineTransform(item.sourceObjectWorldTransform, pose.joints[rootIndex]);
					alignedWorld.translation = item.ragdollRecoveryAlignedRootPosition;
					AnimationTransform alignedModel;
					if(!ProceduralMath::TryRelativeTransform(item.sourceObjectWorldTransform, alignedWorld, alignedModel)) {
						return AnimationStatus::InvalidArgument;
					}
					pose.joints[rootIndex] = alignedModel;
				}
			}
		}
		PhysicsPoseSnapshot mappedPhysicsPose = *item.ragdollPhysicsPose;
		for(const RagdollBodyRecord& body : item.binding.ragdollProfile->bodies) {
			const auto physicsBody = std::find_if(mappedPhysicsPose.bodies.begin(), mappedPhysicsPose.bodies.end(),
												  [&body](const PhysicsPoseBodySnapshot& value) {
													  return value.bodyId == body.bodyId && value.stableJointId == body.stableJointId;
												  });
			const auto joint = std::find(pose.jointStableIds.begin(), pose.jointStableIds.end(), body.stableJointId);
			if(physicsBody == mappedPhysicsPose.bodies.end() || joint == pose.jointStableIds.end()) {
				return AnimationStatus::IncompatibleAsset;
			}
			const std::size_t jointIndex = static_cast<std::size_t>(std::distance(pose.jointStableIds.begin(), joint));
			AnimationTransform jointModel;
			if(!TryMapRagdollBodyWorldToJointModel(item.sourceObjectWorldTransform, pose.joints[jointIndex], body, physicsBody->modelPose,
												   jointModel)) {
				return AnimationStatus::InvalidArgument;
			}
			physicsBody->modelPose = jointModel;
		}
		std::vector<RagdollBlendInput> inputs;
		inputs.reserve(item.binding.ragdollProfile->bodies.size());
		for(const RagdollBodyRecord& body : item.binding.ragdollProfile->bodies) {
			RagdollBlendInput input;
			input.stableJointId = body.stableJointId;
			input.animationDrivenKinematic =
				body.motionType == RagdollMotionType::AnimationDriven || body.motionType == RagdollMotionType::Kinematic;
			input.layerWeight = item.ragdollBlendWeight;
			input.timeStartWeight = item.ragdollLifecycle == RagdollLifecycleState::Recovering ? 1.0F : 0.0F;
			input.timeTargetWeight = item.ragdollLifecycle == RagdollLifecycleState::Recovering ? 0.0F : 1.0F;
			input.durationSeconds = item.ragdollLifecycle == RagdollLifecycleState::Recovering ? item.ragdollRecoveryDurationSeconds : 0.0F;
			input.elapsedSeconds = item.ragdollLifecycleStartTick <= item.input.tick
									   ? static_cast<float>(item.input.tick - item.ragdollLifecycleStartTick) /
											 static_cast<float>(kAnimationFixedTicksPerSecond)
									   : 0.0F;
			const auto blend = std::find_if(item.binding.ragdollProfile->blends.begin(), item.binding.ragdollProfile->blends.end(),
											[&body](const RagdollBlendRecord& value) { return value.stableJointId == body.stableJointId; });
			if(blend != item.binding.ragdollProfile->blends.end()) {
				// The authored joint mask is independent from branch inheritance and
				// the request-wide layer contribution. Keeping them in separate inputs
				// prevents an include-descendants record from squaring its own weight.
				input.weight = 1.0F;
				input.branchWeight = blend->includeDescendants ? blend->defaultWeight : 1.0F;
				input.maskWeight = blend->includeDescendants ? 1.0F : blend->defaultWeight;
				input.boundaryPolicy = blend->boundaryPolicy;
				input.motorStrength = blend->motorStrengthScale;
				input.includeDescendants = blend->includeDescendants;
				input.durationSeconds =
					item.ragdollLifecycle == RagdollLifecycleState::Recovering
						? (blend->recoveryBlendSeconds > 0.0F ? blend->recoveryBlendSeconds : item.ragdollRecoveryDurationSeconds)
						: blend->activationBlendSeconds;
			}
			const auto joint = std::find(pose.jointStableIds.begin(), pose.jointStableIds.end(), body.stableJointId);
			if(joint == pose.jointStableIds.end())
				return AnimationStatus::IncompatibleAsset;
			const std::size_t jointIndex = static_cast<std::size_t>(std::distance(pose.jointStableIds.begin(), joint));
			const std::int32_t parentIndex = item.binding.jointParents[jointIndex];
			if(parentIndex >= 0) {
				if(static_cast<std::size_t>(parentIndex) >= jointIndex) {
					return AnimationStatus::IncompatibleAsset;
				}
				const std::uint64_t parentJointId = pose.jointStableIds[static_cast<std::size_t>(parentIndex)];
				const auto parentBody =
					std::find_if(item.binding.ragdollProfile->bodies.begin(), item.binding.ragdollProfile->bodies.end(),
								 [parentJointId](const RagdollBodyRecord& value) { return value.stableJointId == parentJointId; });
				const bool parentAnimationDriven =
					parentBody == item.binding.ragdollProfile->bodies.end() || parentBody->motionType != RagdollMotionType::Dynamic;
				input.isBoundary =
					parentBody == item.binding.ragdollProfile->bodies.end() || parentAnimationDriven != input.animationDrivenKinematic;
			}
			inputs.push_back(input);
		}
		RagdollBlendResult blended;
		const AnimationStatus status = BlendRagdollPose(pose.jointStableIds, item.binding.jointParents, pose.joints, mappedPhysicsPose,
														item.ragdollGeneration, inputs, 1.0F, blended);
		if(status != AnimationStatus::Ok)
			return status;
		item.ragdollMotorDrives.clear();
		item.ragdollMotorDrives.reserve(item.binding.ragdollProfile->constraints.size());
		for(const RagdollConstraintRecord& constraint : item.binding.ragdollProfile->constraints) {
			const bool hasMotor = std::any_of(constraint.motors.begin(), constraint.motors.end(),
											  [](const RagdollAxisMotor& motor) { return motor.mode != RagdollMotorMode::Disabled; });
			if(!hasMotor)
				continue;
			const auto childBody =
				std::find_if(item.binding.ragdollProfile->bodies.begin(), item.binding.ragdollProfile->bodies.end(),
							 [&constraint](const RagdollBodyRecord& value) { return value.bodyId == constraint.childBodyId; });
			if(childBody == item.binding.ragdollProfile->bodies.end()) {
				return AnimationStatus::IncompatibleAsset;
			}
			const auto childJoint = std::find(pose.jointStableIds.begin(), pose.jointStableIds.end(), childBody->stableJointId);
			if(childJoint == pose.jointStableIds.end()) {
				return AnimationStatus::IncompatibleAsset;
			}
			const std::size_t childIndex = static_cast<std::size_t>(std::distance(pose.jointStableIds.begin(), childJoint));
			item.ragdollMotorDrives.push_back({constraint.constraintId, blended.effectiveMotorStrengths[childIndex]});
		}
		if(item.ragdollLifecycle == RagdollLifecycleState::Recovering) {
			item.ragdollRecoveryBlendComplete = std::all_of(
				item.binding.ragdollProfile->bodies.begin(), item.binding.ragdollProfile->bodies.end(), [&](const RagdollBodyRecord& body) {
					const auto joint = std::find(pose.jointStableIds.begin(), pose.jointStableIds.end(), body.stableJointId);
					return joint != pose.jointStableIds.end() &&
						   blended.effectiveWeights[static_cast<std::size_t>(std::distance(pose.jointStableIds.begin(), joint))] <= 1.0e-4F;
				});
		}
		pose.joints = std::move(blended.modelPose);
		return AnimationStatus::Ok;
	}

	void AnimationSystem::Impl::PrepareFallbackSelection(FrameWorkItem& item) noexcept {
		if(item.cacheReused)
			return;
		const AnimationCompatibilityFingerprint compatibility = BuildFallbackCompatibilityFingerprint(item.binding);
		item.fallbackGenerations.animator = item.input.handle;
		item.fallbackGenerations.componentGeneration = item.input.descriptor.componentGeneration;
		item.fallbackGenerations.sceneGeneration = frameInputs.frameSceneGeneration;
		item.fallbackGenerations.contentGeneration = item.contentGeneration;
		item.fallbackGenerations.rendererGeneration = item.binding.evaluationGenerations.rendererGeneration;
		item.fallbackGenerations.physicsWorldGeneration = item.binding.evaluationGenerations.physicsGeneration;
		item.fallbackGenerations.publicationGeneration = item.candidatePoseGeneration;
		item.fallbackGenerations.compatibilityFingerprint = compatibility;

		const bool poseUsable = item.publication && item.publication->snapshot && item.publication->snapshot->pose &&
								item.publication->snapshot->pose->ValidateForPublication() == AnimationStatus::Ok;
		const bool currentUsable = item.evaluationStatus == AnimationStatus::Ok && poseUsable;
		// A runtime shell without a skeleton has no authored bind pose. Its sealed
		// zero-channel pose is nevertheless a complete typed CPU-safe result and is
		// needed to keep lifecycle membership publications monotonic across restart.
		// Skeleton-backed dependency failures remain bind-pose/hold-last candidates,
		// so a compatible prior generation retains precedence after a failed rebind.
		const bool unboundRuntimeShell = !item.binding.skeleton && !item.binding.clip && item.binding.jointStableIds.empty() &&
										 item.binding.morphStableIds.empty() && item.binding.curveStableIds.empty();
		const bool cpuFallbackUsable = poseUsable && unboundRuntimeShell;
		AnimationFallbackRequest request;
		request.policy = item.binding.fallbackPolicy;
		request.reason = currentUsable ? AnimationFallbackReason::None : FallbackReasonForStatus(item.evaluationStatus);
		request.expectedGenerations = item.fallbackGenerations;
		request.current = {item.fallbackGenerations, currentUsable, currentUsable, currentUsable};
		request.cpuFallback = {item.fallbackGenerations, cpuFallbackUsable, cpuFallbackUsable, cpuFallbackUsable};
		request.prior = {item.fallbackGenerations, item.priorPublication != nullptr, item.priorPublication != nullptr,
						 item.priorPublication != nullptr};
		request.bindPoseAvailable = poseUsable && item.binding.skeleton;
		item.fallbackDecision = SelectAnimationFallback(request);

		switch(item.fallbackDecision.source) {
		case AnimationFallbackSource::Current:
		case AnimationFallbackSource::CpuDeterministic:
		case AnimationFallbackSource::BindPose:
			item.selectedPublication = item.publication;
			break;
		case AnimationFallbackSource::HeldPriorGeneration:
			item.selectedPublication = item.priorPublication;
			break;
		case AnimationFallbackSource::None:
		case AnimationFallbackSource::Disabled:
			item.selectedPublication.reset();
			break;
		}

		if(item.fallbackDecision.source != AnimationFallbackSource::Current) {
			evaluationState.fallbackAnimatorCount.fetch_add(1U, std::memory_order_acq_rel);
		}
		if(!item.selectedPublication || (item.fallbackDecision.source != AnimationFallbackSource::Current &&
										 item.fallbackDecision.source != AnimationFallbackSource::CpuDeterministic &&
										 item.fallbackDecision.source != AnimationFallbackSource::BindPose)) {
			return;
		}
	}

	AnimationFallbackGenerationKey AnimationSystem::Impl::BuildLiveFallbackGenerations(const RuntimeRecord& record,
																					   const FrameWorkItem& item,
																					   std::uint64_t sceneGeneration) noexcept {
		AnimationFallbackGenerationKey key;
		key.animator = record.handle;
		key.componentGeneration = record.descriptor.componentGeneration;
		key.sceneGeneration = sceneGeneration;
		key.contentGeneration = record.contentGeneration;
		key.rendererGeneration = record.binding.evaluationGenerations.rendererGeneration;
		key.physicsWorldGeneration = record.binding.evaluationGenerations.physicsGeneration;
		key.publicationGeneration = item.candidatePoseGeneration;
		key.compatibilityFingerprint = BuildFallbackCompatibilityFingerprint(record.binding);
		return key;
	}

	AnimationStatus AnimationSystem::Impl::BlendControllerTransitionSample(FrameWorkItem& item, CpuClipEvaluator& evaluator,
																		   CpuClipEvaluationResult& source) const noexcept {
		if(item.candidateGraphState.activeTransitionStableId == 0U || !item.transition.ready) {
			return AnimationStatus::Ok;
		}

		try {
			CpuClipEvaluationRequest destinationRequest;
			destinationRequest.tick = item.transition.destinationPlayback.AbsoluteTick();
			destinationRequest.loop = item.transition.destinationPlayback.Configuration().loop;
			destinationRequest.jointStableIds = item.sampledSourceSkeleton
													? std::span<const std::uint64_t>(item.binding.retargetSourceJointStableIds)
													: std::span<const std::uint64_t>(item.binding.jointStableIds);
			destinationRequest.morphStableIds = item.binding.morphStableIds;
			destinationRequest.curveStableIds = item.binding.curveStableIds;
			CpuClipEvaluationResult destination;
			const AnimationStatus destinationStatus =
				evaluator.Evaluate(item.transition.destinationClipView, destinationRequest, destination);
			if(destinationStatus != AnimationStatus::Ok || source.joints.size() != destination.joints.size() ||
			   source.morphs.size() != destination.morphs.size() || source.curves.size() != destination.curves.size()) {
				return destinationStatus == AnimationStatus::Ok ? AnimationStatus::IncompatibleAsset : destinationStatus;
			}

			std::vector<AnimationTransform> sourcePose;
			std::vector<AnimationTransform> destinationPose;
			sourcePose.reserve(source.joints.size());
			destinationPose.reserve(destination.joints.size());
			for(std::size_t index = 0U; index < source.joints.size(); ++index) {
				if(source.joints[index].stableJointId != destination.joints[index].stableJointId) {
					return AnimationStatus::IncompatibleAsset;
				}
				sourcePose.push_back(source.joints[index].localTransform);
				destinationPose.push_back(destination.joints[index].localTransform);
			}
			const std::span<const AnimationTransform> effectiveSource =
				item.transition.capturedSourcePose ? std::span<const AnimationTransform>(*item.transition.capturedSourcePose)
												   : std::span<const AnimationTransform>(sourcePose);
			if(effectiveSource.size() != destinationPose.size()) {
				return AnimationStatus::IncompatibleAsset;
			}

			const float destinationWeight = std::clamp(item.candidateGraphState.transitionProgressNormalized, 0.0F, 1.0F);
			const WeightedLocalPose weighted[] = {{effectiveSource, 1.0F - destinationWeight, 0U},
												  {destinationPose, destinationWeight, 1U}};
			std::vector<AnimationTransform> blended;
			const AnimationStatus blendStatus = BlendLocalPoses(weighted, effectiveSource, blended);
			if(blendStatus != AnimationStatus::Ok || blended.size() != source.joints.size()) {
				return blendStatus == AnimationStatus::Ok ? AnimationStatus::InternalError : blendStatus;
			}
			for(std::size_t index = 0U; index < blended.size(); ++index) {
				source.joints[index].localTransform = blended[index];
			}

			auto blendNamedValues = [destinationWeight](auto& sourceValues, const auto& destinationValues) noexcept {
				for(std::size_t index = 0U; index < sourceValues.size(); ++index) {
					if(sourceValues[index].stableId != destinationValues[index].stableId || !std::isfinite(sourceValues[index].value) ||
					   !std::isfinite(destinationValues[index].value)) {
						return false;
					}
					sourceValues[index].value =
						sourceValues[index].value * (1.0F - destinationWeight) + destinationValues[index].value * destinationWeight;
				}
				return true;
			};
			if(!blendNamedValues(source.morphs, destination.morphs) || !blendNamedValues(source.curves, destination.curves)) {
				return AnimationStatus::IncompatibleAsset;
			}
			if(destination.fallbackChannelCount > std::numeric_limits<std::uint32_t>::max() - source.fallbackChannelCount ||
			   destination.unavailableChannelCount > std::numeric_limits<std::uint32_t>::max() - source.unavailableChannelCount) {
				return AnimationStatus::RangeOverflow;
			}
			source.fallbackChannelCount += destination.fallbackChannelCount;
			source.unavailableChannelCount += destination.unavailableChannelCount;
			return AnimationStatus::Ok;
		} catch(const std::bad_alloc&) {
			return AnimationStatus::InternalError;
		}
	}

	std::pair<std::uint64_t, std::uint64_t> AnimationSystem::Impl::BuildVisualPoseContentIdentity(const PoseBuffer& pose) noexcept {
		// Hash only model-space deformation inputs. Local transforms and gameplay
		// curves do not change the renderer palette once model transforms and morph
		// values are fixed. Field-wise mixing avoids hashing structure padding.
		std::uint64_t low = 1469598103934665603ULL;
		std::uint64_t high = 0xd6e8feb86659fd93ULL;
		const auto mix = [&low, &high](std::uint64_t value) noexcept {
			for(std::size_t byte = 0U; byte < sizeof(value); ++byte) {
				const std::uint8_t lane = static_cast<std::uint8_t>((value >> (byte * 8U)) & 0xffU);
				low = (low ^ lane) * 1099511628211ULL;
				high ^= static_cast<std::uint64_t>(lane) + 0x9e3779b97f4a7c15ULL + (high << 6U) + (high >> 2U);
			}
		};
		const auto mixFloat = [&mix](float value) noexcept {
			std::uint32_t bits = 0U;
			std::memcpy(&bits, &value, sizeof(bits));
			mix(bits);
		};
		mix(pose.ModelTransforms().size());
		for(const AnimationTransform& transform : pose.ModelTransforms()) {
			mixFloat(transform.translation.x);
			mixFloat(transform.translation.y);
			mixFloat(transform.translation.z);
			mixFloat(transform.rotation.x);
			mixFloat(transform.rotation.y);
			mixFloat(transform.rotation.z);
			mixFloat(transform.rotation.w);
			mixFloat(transform.scale.x);
			mixFloat(transform.scale.y);
			mixFloat(transform.scale.z);
		}
		mix(pose.MorphValues().size());
		for(const AnimationNamedFloatValue& morph : pose.MorphValues()) {
			mix(morph.stableId);
			mixFloat(morph.value);
		}
		return {low == 0U ? 1U : low, high == 0U ? 1U : high};
	}

	void AnimationSystem::Impl::PublishEvaluatedCpuPose(FrameWorkItem& item) noexcept {
		// The pose channels are already sealed before this helper runs. Keep the
		// instance-specific snapshot and ownership envelope separate so a reused
		// visual pose can never inherit another animator's handle, object, or
		// component generation.
		try {
			const std::shared_ptr<const PoseBuffer> publishedPose =
				item.pose ? std::shared_ptr<const PoseBuffer>(item.pose) : item.retainedVisualPose;
			if(!publishedPose) {
				item.evaluationStatus = AnimationStatus::InternalError;
				item.evaluated = true;
				PrepareFallbackSelection(item);
				return;
			}
			if(item.visualPoseContentIdLow == 0U || item.visualPoseContentIdHigh == 0U) {
				std::tie(item.visualPoseContentIdLow, item.visualPoseContentIdHigh) = BuildVisualPoseContentIdentity(*publishedPose);
			}
			AnimationSnapshot snapshot;
			snapshot.animator = item.input.handle;
			snapshot.publicationGeneration = item.candidatePoseGeneration;
			snapshot.tick = frameInputs.frameTick;
			snapshot.playback = MakePublishedPlaybackSnapshot(item.playback.Snapshot());
			snapshot.playback.currentStateId = item.binding.stateId;
			snapshot.playback.previousStateId =
				item.candidateGraphState.activeTransitionStableId != 0U ? item.candidateGraphState.transitionSourceStateId : 0U;
			snapshot.playback.transitionProgress = item.candidateGraphState.transitionProgressNormalized;
			if(item.binding.evaluationProgram) {
				for(std::size_t layerIndex = 0U; layerIndex < item.binding.evaluationProgram->layers.size(); ++layerIndex) {
					const EvaluationLayer& layer = item.binding.evaluationProgram->layers[layerIndex];
					snapshot.playback.activeLayers.push_back(
						{static_cast<std::uint32_t>(layerIndex), item.binding.stateId,
						 layerIndex < item.layerWeights.size() ? item.layerWeights[layerIndex] : layer.defaultWeight,
						 item.input.tick - item.candidateGraphState.stateEnteredTick});
				}
			}
			snapshot.playback.latestStatus = item.evaluationStatus;
			snapshot.playback.parameters = item.parameters;
			snapshot.pose = publishedPose;
			auto immutableSnapshot = std::make_shared<const AnimationSnapshot>(std::move(snapshot));
			AnimationAuthorityRow published;
			published.key = {item.input.handle.slotIndex, item.input.handle.generation,
							 runtimeSessionEpoch.load(std::memory_order_acquire)};
			published.ownerObjectId = item.input.descriptor.ownerObjectId;
			published.objectGeneration = item.sourceObjectGeneration;
			published.componentGeneration = item.input.descriptor.componentGeneration;
			published.sourceObjectWorldTransform = item.sourceObjectWorldTransform;
			published.sourceObjectWorldTransformCaptured = item.sourceObjectWorldTransformCaptured;
			published.authorityPublicationGeneration = item.candidateAuthorityPublicationGeneration;
			published.visualPoseContentIdLow = item.visualPoseContentIdLow;
			published.visualPoseContentIdHigh = item.visualPoseContentIdHigh;
			published.controller = item.input.descriptor.controller;
			published.jointStableIds = item.binding.jointStableIds;
			if(item.binding.clip) {
				published.clip = {item.binding.clip->assetId, item.binding.clipGeneration};
			}
			if(item.binding.skeleton) {
				published.skeleton = {item.binding.skeleton->assetId, item.binding.skeletonGeneration};
				published.skeletonAsset = item.binding.skeleton;
				published.skeletonCompatibilityFingerprint = item.binding.skeleton->compatibilityFingerprint;
			}
			published.status = item.evaluationStatus;
			published.requestedExecutionPolicy = item.binding.requestedExecutionPolicy;
			published.effectiveExecutionMode = item.executionDecision.effectiveMode;
			published.rootMotionPolicy = item.binding.rootMotionPolicy;
			published.updatePolicy = item.binding.updatePolicy;
			published.eventPolicy = item.binding.eventPolicy;
			published.cullingPolicy = item.binding.cullingPolicy;
			published.safeFallback = item.safePose;
			published.streamedClipPayloadsActive = item.binding.streamedSegmentPayloads != nullptr;
			published.poseMemoryReservation = item.retainedVisualPose && item.priorPublication
												  ? item.priorPublication->poseMemoryReservation
												  : frameStorage.framePoseReservation;
			published.snapshot = std::move(immutableSnapshot);
			item.publication = std::make_shared<const AnimationAuthorityRow>(std::move(published));
			item.evaluated = true;
			PrepareFallbackSelection(item);
		} catch(const std::bad_alloc&) {
			evaluationState.workPlanRejectionReason = AnimationPlanRejectionReason::AllocationFailure;
			item.evaluationStatus = AnimationStatus::InternalError;
			item.evaluated = true;
			PrepareFallbackSelection(item);
		}
	}

	void AnimationSystem::Impl::EvaluateItem(FrameWorkItem& item) noexcept {
		if(!item.planned || item.candidatePoseGeneration == 0U)
			return;
		if(item.cacheReused) {
			// V2 supplies its own features, so a reused pose can still perform the
			// worker-side search without rebuilding the unchanged pose publication.
			EvaluateMotionMatchQueriesV2(item);
			return;
		}
		if(PollFrameCancellation()) {
			item.evaluationStatus = AnimationStatus::Cancelled;
			EvaluateMotionMatchQueriesV2(item);
			item.evaluated = true;
			PrepareFallbackSelection(item);
			return;
		}
		if(item.executionDecision.effectiveMode == AnimationEffectiveExecutionMode::GpuVisual && item.gpuLowering.has_value()) {
			// The direct query remains CPU-authoritative even when presentation owns
			// a GPU-only pose. Its inline features require no pose readback.
			EvaluateMotionMatchQueriesV2(item);
			// A complete Visual GPU request owns joint, morph, and curve evaluation.
			// Do not manufacture a CPU pose: doing so both defeats the mode's CPU
			// budget and could let RenderSystem accidentally prefer stale CPU output.
			item.safePose = false;
			item.evaluated = true;
			// GPU-only publication is produced by AnimationPresentationSystem. There is
			// no CPU payload for the fallback latch at this authority boundary.
			return;
		}
		CpuClipEvaluationResult sampled;
		AnimationStatus sampleStatus = item.dependencyStatus;
		CpuClipEvaluator evaluator;
		if(item.dependencyStatus == AnimationStatus::Ok) {
			CpuClipEvaluationRequest request;
			request.tick = item.playback.AbsoluteTick();
			request.loop = item.playback.Configuration().loop;
			request.jointStableIds = item.sampledSourceSkeleton ? std::span<const std::uint64_t>(item.binding.retargetSourceJointStableIds)
																: std::span<const std::uint64_t>(item.binding.jointStableIds);
			request.morphStableIds = item.binding.morphStableIds;
			request.curveStableIds = item.binding.curveStableIds;
			sampleStatus = evaluator.Evaluate(item.clipView, request, sampled);
			if(sampleStatus == AnimationStatus::Ok) {
				sampleStatus = BlendControllerTransitionSample(item, evaluator, sampled);
			}
		}
		item.safePose = sampleStatus != AnimationStatus::Ok;
		if(item.sampledSourceSkeleton) {
			item.retargetEvaluationStatus = sampleStatus;
		}
		if(item.evaluationStatus == AnimationStatus::Ok)
			item.evaluationStatus = sampleStatus;

		AnimationEventTraversalResult traversedEvents;
		if(item.eventStateCaptured && item.dependencyStatus == AnimationStatus::Ok && !item.transition.capturedSourcePose &&
		   item.advance.previousAbsoluteTick != item.advance.currentAbsoluteTick) {
			AnimationEventTraversalRequest traversal;
			traversal.animator = item.input.handle;
			traversal.stateInstanceId = item.binding.stateId;
			traversal.sourceStateId = item.binding.stateId;
			traversal.previousAbsoluteTick = item.advance.previousAbsoluteTick;
			traversal.currentAbsoluteTick = item.advance.currentAbsoluteTick;
			traversal.producerOrderBase = 1U;
			traversal.eventPolicy = item.binding.eventPolicy;
			const AnimationStatus eventStatus = item.eventStream.Traverse(traversal, traversedEvents);
			if(item.evaluationStatus == AnimationStatus::Ok && eventStatus != AnimationStatus::Ok) {
				item.evaluationStatus = eventStatus;
			}
		}
		if(item.dependencyStatus == AnimationStatus::Ok && item.candidateGraphState.activeTransitionStableId != 0U &&
		   item.transition.ready &&
		   item.transition.destinationAdvance.previousAbsoluteTick != item.transition.destinationAdvance.currentAbsoluteTick) {
			AnimationEventTraversalRequest traversal;
			traversal.animator = item.input.handle;
			traversal.stateInstanceId = item.transition.destinationStateId;
			traversal.sourceStateId = item.transition.destinationStateId;
			traversal.previousAbsoluteTick = item.transition.destinationAdvance.previousAbsoluteTick;
			traversal.currentAbsoluteTick = item.transition.destinationAdvance.currentAbsoluteTick;
			// Keep source and destination producer ranges disjoint. The source
			// clip remains first, while the destination's authored order is
			// stable within this high range throughout the transition.
			traversal.producerOrderBase = 1ULL << 32U;
			traversal.eventPolicy = item.binding.eventPolicy;
			AnimationEventTraversalResult destinationEvents;
			const AnimationStatus destinationEventStatus = item.transition.destinationEventStream.Traverse(traversal, destinationEvents);
			if(destinationEventStatus == AnimationStatus::Ok) {
				try {
					traversedEvents.events.insert(traversedEvents.events.end(), std::make_move_iterator(destinationEvents.events.begin()),
												  std::make_move_iterator(destinationEvents.events.end()));
				} catch(const std::bad_alloc&) {
					if(item.evaluationStatus == AnimationStatus::Ok) {
						item.evaluationStatus = AnimationStatus::InternalError;
					}
				}
			} else if(item.evaluationStatus == AnimationStatus::Ok) {
				item.evaluationStatus = destinationEventStatus;
			}
		}

		try {
			item.pose = std::make_shared<PoseBuffer>();
		} catch(const std::bad_alloc&) {
			item.evaluationStatus = AnimationStatus::InternalError;
			item.evaluated = true;
			PrepareFallbackSelection(item);
			return;
		}
		PoseBufferLayout layout;
		layout.jointCount = item.binding.jointStableIds.size();
		layout.morphCount = item.binding.morphStableIds.size();
		layout.curveCount = item.binding.curveStableIds.size();
		layout.eventCapacity = traversedEvents.events.size();
		AnimationPoseGeneration poseGeneration;
		poseGeneration.animator = item.input.handle;
		poseGeneration.poseGeneration = item.candidatePoseGeneration;
		if(item.binding.skeleton) {
			poseGeneration.skeleton = {item.binding.skeleton->assetId, item.binding.skeletonGeneration};
		}
		poseGeneration.programGeneration = item.binding.programGeneration;
		poseGeneration.tick = frameInputs.frameTick;
		if(const AnimationStatus status = item.pose->BeginWrite(layout, poseGeneration); status != AnimationStatus::Ok) {
			item.evaluationStatus = status;
			item.evaluated = true;
			PrepareFallbackSelection(item);
			return;
		}

		auto local = item.pose->MutableLocalTransforms();
		for(std::size_t index = 0U; index < local.size(); ++index) {
			local[index] = !item.sampledSourceSkeleton && index < sampled.joints.size()
							   ? sampled.joints[index].localTransform
							   : ReferenceTransformForStableId(item, item.binding.jointStableIds[index]);
		}
		auto model = item.pose->MutableModelTransforms();
		BuildModelTransforms(item, local, model);
		if(item.evaluationStatus == AnimationStatus::Ok && item.sampledSourceSkeleton) {
			const AnimationStatus retargetStatus = ApplyRetargetStage(item, sampled, local, model);
			item.retargetEvaluationStatus = retargetStatus;
			if(retargetStatus != AnimationStatus::Ok) {
				item.evaluationStatus = retargetStatus;
				item.safePose = true;
			}
		}
		if(item.evaluationStatus == AnimationStatus::Ok &&
		   (!item.activeProceduralGoals.empty() || item.ragdollLifecycle == RagdollLifecycleState::Active ||
			item.ragdollLifecycle == RagdollLifecycleState::Recovering)) {
			try {
				GraphPoseContribution advancedPose;
				advancedPose.compatibilityFingerprint = item.binding.skeleton ? item.binding.skeleton->compatibilityFingerprint : 0U;
				advancedPose.jointCount = static_cast<std::uint32_t>(model.size());
				advancedPose.jointStableIds = item.binding.jointStableIds;
				advancedPose.joints.assign(model.begin(), model.end());
				AnimationStatus advancedStatus = ApplyProceduralStage(item, advancedPose);
				if(advancedStatus == AnimationStatus::Ok) {
					advancedStatus = ApplyRagdollStage(item, advancedPose);
				}
				if(advancedStatus == AnimationStatus::Ok && advancedPose.joints.size() == model.size() &&
				   BuildLocalTransformsFromModel(item, advancedPose.joints, local)) {
					std::copy(advancedPose.joints.begin(), advancedPose.joints.end(), model.begin());
				} else {
					item.evaluationStatus = advancedStatus == AnimationStatus::Ok ? AnimationStatus::InternalError : advancedStatus;
					item.safePose = true;
				}
			} catch(const std::bad_alloc&) {
				item.evaluationStatus = AnimationStatus::InternalError;
				item.safePose = true;
			}
		}
		// Motion search is part of the worker-owned item. Even a production-size
		// database is scored away from the protected publication boundary; only the
		// bounded copied result is committed later.
		EvaluateMotionMatchRequests(item, model);
		EvaluateMotionMatchQueriesV2(item);
		auto morphs = item.pose->MutableMorphValues();
		for(std::size_t index = 0U; index < morphs.size(); ++index) {
			morphs[index] = {item.binding.morphStableIds[index], index < sampled.morphs.size() ? sampled.morphs[index].value : 0.0F};
		}
		auto curves = item.pose->MutableCurveValues();
		for(std::size_t index = 0U; index < curves.size(); ++index) {
			curves[index] = {item.binding.curveStableIds[index], index < sampled.curves.size() ? sampled.curves[index].value : 0.0F};
		}
		for(AnimationEventRecord& event : traversedEvents.events) {
			if(const AnimationStatus status = item.pose->AppendEvent(std::move(event));
			   status != AnimationStatus::Ok && item.evaluationStatus == AnimationStatus::Ok) {
				item.evaluationStatus = status;
			}
		}

		EvaluateCompleteRootMotion(item, evaluator);

		if(const AnimationStatus sealStatus = item.pose->Seal(); sealStatus != AnimationStatus::Ok) {
			item.evaluationStatus = sealStatus;
			item.evaluated = true;
			PrepareFallbackSelection(item);
			return;
		}
		if(item.visualEvaluationSuspended && item.priorPublication && item.priorPublication->snapshot &&
		   item.priorPublication->snapshot->pose && item.executionDecision.effectiveMode != AnimationEffectiveExecutionMode::GpuVisual) {
			// Playback/controller state advanced during planning, but a purely visual
			// off-screen animator may publish the prior immutable pose without decoding
			// its clip. Reappearance evaluates the current tick, so no time is lost.
			item.retainedVisualPose = item.priorPublication->snapshot->pose;
			item.visualPoseContentIdLow = item.priorPublication->visualPoseContentIdLow;
			item.visualPoseContentIdHigh = item.priorPublication->visualPoseContentIdHigh;
			item.safePose = item.priorPublication->safeFallback;
			item.evaluationStatus = AnimationStatus::Ok;
			PublishEvaluatedCpuPose(item);
			return;
		}
		PublishEvaluatedCpuPose(item);
	}

	bool AnimationSystem::Impl::CanShareCpuVisualPose(const FrameWorkItem& source, const FrameWorkItem& destination) noexcept {
		// Same-frame sharing is deliberately narrower than ordinary CPU fallback.
		// Anything that can publish per-instance authority or mutate evaluator state
		// keeps its own evaluation. This leaves synchronized crowd visuals as the
		// useful fast path without aliasing events, root motion, gameplay curves,
		// procedural results, physics, rollback, or diagnostics observations.
		const auto hasNonShareableAuthority = [](const AnimationConsumerClosure& closure) noexcept {
			// Root-motion policy selects CPU authority even for clips that contain
			// no root-motion track. The clip checks below prove that special case
			// has no per-instance output; every other authority request remains a
			// hard barrier to sharing.
			return !closure.joints.empty() || closure.curves || closure.morphs || closure.events || closure.contacts ||
				   closure.proceduralResults || closure.physics || closure.attachments || closure.networking || closure.rollback ||
				   closure.replay || closure.recording || closure.automation || closure.diagnostics || closure.completePose;
		};
		if(!source.evaluated || source.evaluationStatus != AnimationStatus::Ok || source.safePose || source.pose == nullptr ||
		   !source.pose->IsSealed() || source.pose->ValidateForPublication() != AnimationStatus::Ok || source.pose->Events().size() != 0U ||
		   source.pose->RootMotionDelta().has_value() || !destination.planned || destination.cacheReused ||
		   destination.evaluationStatus != AnimationStatus::Ok ||
		   source.executionDecision.effectiveMode != AnimationEffectiveExecutionMode::CpuDeterministicFallback ||
		   destination.executionDecision.effectiveMode != AnimationEffectiveExecutionMode::CpuDeterministicFallback ||
		   source.resolvedConsumers.deterministic || destination.resolvedConsumers.deterministic ||
		   hasNonShareableAuthority(source.executionDecision.consumerClosure) ||
		   hasNonShareableAuthority(destination.executionDecision.consumerClosure) || source.binding.clip == nullptr ||
		   destination.binding.clip == nullptr || source.binding.clip->rootMotion.enabled || destination.binding.clip->rootMotion.enabled ||
		   !source.binding.clip->events.empty() || !destination.binding.clip->events.empty() ||
		   source.candidateGraphState.activeTransitionStableId != 0U || destination.candidateGraphState.activeTransitionStableId != 0U ||
		   source.retargetActive || destination.retargetActive || source.sampledSourceSkeleton || destination.sampledSourceSkeleton ||
		   !source.activeProceduralGoals.empty() || !destination.activeProceduralGoals.empty() ||
		   source.ragdollLifecycle != RagdollLifecycleState::Inactive || destination.ragdollLifecycle != RagdollLifecycleState::Inactive ||
		   !source.pendingMotionQueries.empty() || !destination.pendingMotionQueries.empty() || !source.pendingMotionQueriesV2.empty() ||
		   !destination.pendingMotionQueriesV2.empty() || source.activeMotionDatabaseGeneration != 0U ||
		   destination.activeMotionDatabaseGeneration != 0U) {
			return false;
		}

		const bool sameClip = source.binding.clip->assetId == destination.binding.clip->assetId &&
							  source.binding.clipGeneration == destination.binding.clipGeneration &&
							  source.binding.clip->compatibilityFingerprint == destination.binding.clip->compatibilityFingerprint;
		const bool sameSkeleton =
			source.binding.skeleton && destination.binding.skeleton &&
			source.binding.skeleton->assetId == destination.binding.skeleton->assetId &&
			source.binding.skeletonGeneration == destination.binding.skeletonGeneration &&
			source.binding.skeleton->compatibilityFingerprint == destination.binding.skeleton->compatibilityFingerprint;
		const std::uint64_t sourceProgram = source.binding.evaluationProgram ? source.binding.evaluationProgram->stableHash : 0U;
		const std::uint64_t destinationProgram =
			destination.binding.evaluationProgram ? destination.binding.evaluationProgram->stableHash : 0U;
		return sameClip && sameSkeleton && source.binding.programGeneration == destination.binding.programGeneration &&
			   sourceProgram == destinationProgram && source.binding.stateId == destination.binding.stateId &&
			   source.playback.Snapshot() == destination.playback.Snapshot() &&
			   source.candidateGraphState == destination.candidateGraphState && source.parameters == destination.parameters &&
			   source.layerWeights == destination.layerWeights && source.binding.jointStableIds == destination.binding.jointStableIds &&
			   source.binding.morphStableIds == destination.binding.morphStableIds &&
			   source.binding.curveStableIds == destination.binding.curveStableIds &&
			   source.pose->Layout().jointCount == destination.binding.jointStableIds.size() &&
			   source.pose->Layout().morphCount == destination.binding.morphStableIds.size() &&
			   source.pose->Layout().curveCount == destination.binding.curveStableIds.size();
	}

	bool AnimationSystem::Impl::TryShareCpuVisualPose(const FrameWorkItem& source, FrameWorkItem& destination) noexcept {
		if(!CanShareCpuVisualPose(source, destination))
			return false;
		try {
			auto pose = std::make_shared<PoseBuffer>();
			AnimationPoseGeneration generation;
			generation.animator = destination.input.handle;
			generation.poseGeneration = destination.candidatePoseGeneration;
			generation.skeleton = {destination.binding.skeleton->assetId, destination.binding.skeletonGeneration};
			generation.programGeneration = destination.binding.programGeneration;
			generation.tick = frameInputs.frameTick;
			// The PoseBuffer owns the per-animator generation but aliases only the
			// leader's already-sealed visual payload. This removes both repeated
			// evaluation and the former local/model/palette memcpy for synchronized
			// crowds while retaining a distinct publication identity per animator.
			if(pose->ShareSealedVisualPayloadFrom(*source.pose, generation) != AnimationStatus::Ok)
				return false;

			destination.pose = std::move(pose);
			destination.visualPoseContentIdLow = source.visualPoseContentIdLow;
			destination.visualPoseContentIdHigh = source.visualPoseContentIdHigh;
			destination.safePose = false;
			destination.evaluationStatus = AnimationStatus::Ok;
			PublishEvaluatedCpuPose(destination);
			if(!destination.evaluated || destination.fallbackDecision.source != AnimationFallbackSource::Current) {
				return false;
			}
			evaluationState.sharedPoseAnimatorCount.fetch_add(1U, std::memory_order_acq_rel);
			return true;
		} catch(const std::bad_alloc&) {
			return false;
		}
	}

	void AnimationSystem::Impl::EvaluatePartition(std::size_t partitionIndex, std::size_t partitionCount) noexcept {
		if(partitionCount == 0U || partitionIndex >= partitionCount || partitionIndex >= std::numeric_limits<std::uint32_t>::max() ||
		   frameStorage.workPlan.status != AnimationStatus::Ok || !evaluationState.workPlanValidated ||
		   frameStorage.workRangeLedger == nullptr)
			return;
		const auto evaluationStart = std::chrono::steady_clock::now();
		// FinalizeAnimationWorkPlan is a single worker dependency between planning
		// and evaluation. Every range therefore observes one completed immutable
		// plan and never blocks a worker on the former shared finalization mutex.
		std::size_t locallyEvaluated = 0U;
		auto publishLocalCount = [this, &locallyEvaluated]() noexcept {
			if(locallyEvaluated != 0U) {
				evaluationState.evaluatedAnimatorCount.fetch_add(locallyEvaluated, std::memory_order_acq_rel);
				locallyEvaluated = 0U;
			}
		};
		const std::uint32_t workerOrdinal = static_cast<std::uint32_t>(partitionIndex + 1U);
		for(std::size_t rangeIndex = partitionIndex; rangeIndex < frameStorage.workPlan.ranges.size(); rangeIndex += partitionCount) {
			if(!frameStorage.workRangeLedger->TryBegin(rangeIndex, workerOrdinal))
				continue;
			const AnimationWorkRange& range = frameStorage.workPlan.ranges[rangeIndex];
			for(std::size_t scheduledIndex = range.itemBegin; scheduledIndex < range.itemEnd; ++scheduledIndex) {
				const AnimationScheduledWorkItem& scheduled = frameStorage.workPlan.items[scheduledIndex];
				AnimationWorkCompletion& completion = evaluationState.workCompletions[scheduledIndex];
				completion.scheduledItemIndex = scheduledIndex;
				completion.animator = scheduled.item.animator;
				completion.sourceGeneration = scheduled.item.sourceGeneration;
				// Keep cancellation responsive without rereading the lifecycle latch
				// for every tiny GPU-visual completion in a large crowd.
				if(((scheduledIndex - range.itemBegin) & 15U) == 0U && PollFrameCancellation()) {
					completion.status = AnimationStatus::Cancelled;
					continue;
				}
				if(scheduled.item.stablePublicationOrder >= frameInputs.capturedInputWorkIndices.size()) {
					completion.status = AnimationStatus::Rejected;
					continue;
				}
				const CapturedInputWorkIndex& captured =
					frameInputs.capturedInputWorkIndices[static_cast<std::size_t>(scheduled.item.stablePublicationOrder)];
				if(captured.compact || captured.storageIndex >= frameStorage.frameWork.size()) {
					completion.status = AnimationStatus::Rejected;
					continue;
				}

				FrameWorkItem& item = frameStorage.frameWork[captured.storageIndex];
				if(item.candidateAuthorityPublicationGeneration != scheduled.item.sourceGeneration) {
					completion.status = AnimationStatus::Rejected;
					continue;
				}
				bool sharedPose = false;
				// Ranges contain one scheduler-proven evaluator/layout contract and
				// execute serially inside one worker. Search only already-completed
				// members of this range, avoiding cross-worker waits while still
				// collapsing synchronized crowds to one evaluator per 64-instance
				// partition.
				const bool mayShareCpuVisualPose =
					!item.cacheReused && !item.visualEvaluationSuspended &&
					item.executionDecision.effectiveMode == AnimationEffectiveExecutionMode::CpuDeterministicFallback;
				if(mayShareCpuVisualPose) {
					for(std::size_t sourceIndex = range.itemBegin; !sharedPose && sourceIndex < scheduledIndex; ++sourceIndex) {
						const AnimationScheduledWorkItem& sourceScheduled = frameStorage.workPlan.items[sourceIndex];
						if(sourceScheduled.item.stablePublicationOrder >= frameInputs.capturedInputWorkIndices.size())
							continue;
						const CapturedInputWorkIndex& sourceCaptured =
							frameInputs.capturedInputWorkIndices[static_cast<std::size_t>(sourceScheduled.item.stablePublicationOrder)];
						if(sourceCaptured.compact || sourceCaptured.storageIndex >= frameStorage.frameWork.size())
							continue;
						sharedPose = TryShareCpuVisualPose(frameStorage.frameWork[sourceCaptured.storageIndex], item);
					}
				}
				if(!sharedPose && !item.cacheReused && item.executionDecision.effectiveMode == AnimationEffectiveExecutionMode::GpuVisual &&
				   item.gpuLowering.has_value() && frameInputs.capturedMotionMatchQueriesV2.empty()) {
					// Planning already proved this packet complete. With no CPU query,
					// evaluation has no payload to build; publish the terminal flags
					// directly instead of entering the general CPU evaluator per model.
					item.safePose = false;
					item.evaluated = true;
				} else if(!sharedPose) {
					EvaluateItem(item);
				}
				completion.status = item.evaluated ? AnimationStatus::Ok : AnimationStatus::Rejected;
				if(item.evaluated)
					++locallyEvaluated;
			}
			if(!frameStorage.workRangeLedger->Complete(rangeIndex, workerOrdinal) && PollFrameCancellation()) {
				frameStorage.workRangeLedger->CancelUnclaimed();
				publishLocalCount();
				return;
			}
		}
		publishLocalCount();
		evaluationState.completeEvaluationNanoseconds.fetch_add(
			static_cast<std::uint64_t>(
				std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now() - evaluationStart).count()),
			std::memory_order_acq_rel);
	}

	void AnimationSystem::Impl::EvaluateCompleteRootMotion(FrameWorkItem& item, CpuClipEvaluator& evaluator) noexcept {
		if(item.dependencyStatus == AnimationStatus::Ok) {
			const auto rootSourceFingerprint = [&item](AnimationGeneration clipGeneration, std::uint64_t clipContentFingerprint) noexcept {
				// This is a source/graph identity, not the authority publication
				// generation that advances every frame. Hot reload changes it even
				// when the animator handle remains unchanged.
				std::uint64_t hash = 14695981039346656037ULL;
				for(const std::uint64_t value : {static_cast<std::uint64_t>(item.binding.programGeneration),
												 static_cast<std::uint64_t>(clipGeneration), clipContentFingerprint}) {
					hash ^= value;
					hash *= 1099511628211ULL;
				}
				return hash == 0U ? 1U : hash;
			};
			auto extractClipRootMotion = [&item, &evaluator](const AnimationClipAsset* clip, const AnimationClipView& view,
															 const AnimatorPlayback& playback, const AnimatorPlaybackAdvanceResult& advance,
															 std::uint64_t stateId, std::uint64_t sourceGeneration,
															 std::optional<AnimationRootMotionDelta>& output) {
				output.reset();
				if(clip == nullptr || !clip->rootMotion.enabled || advance.previousAbsoluteTick == advance.currentAbsoluteTick) {
					return AnimationStatus::Ok;
				}
				AnimationResolvedClipTime beginTime;
				AnimationResolvedClipTime endTime;
				CpuClipRootMotionPoint beginPoint;
				CpuClipRootMotionPoint endPoint;
				AnimationStatus status = view.ResolveTime(advance.previousAbsoluteTick, playback.Configuration().loop, beginTime);
				if(status != AnimationStatus::Ok)
					return status;
				status = view.ResolveTime(advance.currentAbsoluteTick, playback.Configuration().loop, endTime);
				if(status != AnimationStatus::Ok)
					return status;
				status = evaluator.SampleRootMotionPoint(view, beginTime, beginPoint);
				if(status != AnimationStatus::Ok)
					return status;
				status = evaluator.SampleRootMotionPoint(view, endTime, endPoint);
				if(status != AnimationStatus::Ok)
					return status;
				if(!beginPoint.enabled || !endPoint.enabled) {
					return AnimationStatus::Ok;
				}

				AnimationRootMotionExtractionRequest extraction;
				extraction.animator = item.input.handle;
				extraction.sourceBeginTick = advance.previousAbsoluteTick;
				extraction.sourceEndTick = advance.currentAbsoluteTick;
				extraction.begin = {beginPoint.translation, beginPoint.yawRadians, beginTime.loopIndex};
				extraction.end = {endPoint.translation, endPoint.yawRadians, endTime.loopIndex};
				extraction.accumulatedCycleTranslation = clip->rootMotion.accumulatedCycleTranslation;
				extraction.accumulatedCycleYawRadians = clip->rootMotion.accumulatedCycleYawRadians;
				extraction.sourceStateId = stateId;
				extraction.generation = item.candidatePoseGeneration;
				extraction.policy = item.rootMotionEnabled ? AnimatorRootMotionPolicy::ApplyToCharacter : item.binding.rootMotionPolicy;
				extraction.sourceGeneration = sourceGeneration;
				extraction.evaluationDurationSeconds = item.physicalEvaluationDurationSeconds;
				AnimationRootMotionDelta candidate;
				status = ExtractRootMotion(extraction, candidate);
				if(status == AnimationStatus::Ok) {
					output = candidate;
				}
				return status;
			};

			std::optional<AnimationRootMotionDelta> sourceRootMotion;
			AnimationStatus rootStatus = extractClipRootMotion(
				item.binding.clip.get(), item.clipView, item.playback, item.advance,
				item.candidateGraphState.activeTransitionStableId != 0U ? item.candidateGraphState.transitionSourceStateId
																		: item.binding.stateId,
				rootSourceFingerprint(item.binding.clipGeneration, item.binding.clipDeterministicContentFingerprint), sourceRootMotion);
			if(rootStatus == AnimationStatus::Ok && item.transition.capturedSourceRootMotion) {
				sourceRootMotion = item.transition.capturedSourceRootMotion;
				sourceRootMotion->animator = item.input.handle;
				sourceRootMotion->sourceBeginTick = item.advance.previousAbsoluteTick;
				sourceRootMotion->sourceEndTick = item.advance.currentAbsoluteTick;
				sourceRootMotion->sourceStateId = item.transition.sourceStateId;
				sourceRootMotion->generation = item.candidatePoseGeneration;
				sourceRootMotion->policy =
					item.rootMotionEnabled ? AnimatorRootMotionPolicy::ApplyToCharacter : item.binding.rootMotionPolicy;
				sourceRootMotion->sourceGeneration =
					rootSourceFingerprint(item.binding.clipGeneration, item.binding.clipDeterministicContentFingerprint);
				sourceRootMotion->evaluationDurationSeconds = item.physicalEvaluationDurationSeconds;
			}
			std::optional<AnimationRootMotionDelta> destinationRootMotion;
			if(rootStatus == AnimationStatus::Ok && item.candidateGraphState.activeTransitionStableId != 0U && item.transition.ready) {
				rootStatus = extractClipRootMotion(item.transition.destination.clip.get(), item.transition.destinationClipView,
												   item.transition.destinationPlayback, item.transition.destinationAdvance,
												   item.transition.destinationStateId,
												   rootSourceFingerprint(item.transition.destination.clipGeneration,
																		 item.transition.destination.deterministicContentFingerprint),
												   destinationRootMotion);
			}
			if(rootStatus == AnimationStatus::Ok && (sourceRootMotion || destinationRootMotion)) {
				AnimationRootMotionDelta blended = sourceRootMotion ? *sourceRootMotion : *destinationRootMotion;
				if(item.candidateGraphState.activeTransitionStableId != 0U && item.transition.ready) {
					const float destinationWeight = std::clamp(item.candidateGraphState.transitionProgressNormalized, 0.0F, 1.0F);
					const float sourceWeight = 1.0F - destinationWeight;
					const AnimationRootMotionDelta empty{.animator = item.input.handle,
														 .generation = item.candidatePoseGeneration,
														 .policy = item.rootMotionEnabled ? AnimatorRootMotionPolicy::ApplyToCharacter
																						  : item.binding.rootMotionPolicy};
					const AnimationRootMotionDelta& source = sourceRootMotion ? *sourceRootMotion : empty;
					const AnimationRootMotionDelta& destination = destinationRootMotion ? *destinationRootMotion : empty;
					blended.localTranslation = {
						source.localTranslation.x * sourceWeight + destination.localTranslation.x * destinationWeight,
						source.localTranslation.y * sourceWeight + destination.localTranslation.y * destinationWeight,
						source.localTranslation.z * sourceWeight + destination.localTranslation.z * destinationWeight};
					blended.worldTranslation = {
						source.worldTranslation.x * sourceWeight + destination.worldTranslation.x * destinationWeight,
						source.worldTranslation.y * sourceWeight + destination.worldTranslation.y * destinationWeight,
						source.worldTranslation.z * sourceWeight + destination.worldTranslation.z * destinationWeight};
					blended.localYawRadians = source.localYawRadians * sourceWeight + destination.localYawRadians * destinationWeight;
					blended.worldYawRadians = source.worldYawRadians * sourceWeight + destination.worldYawRadians * destinationWeight;
					blended.sourceStateId = destinationWeight >= 0.5F ? item.transition.destinationStateId : item.transition.sourceStateId;
					blended.accumulatedCycles = destinationWeight >= 0.5F ? destination.accumulatedCycles : source.accumulatedCycles;
					blended.sourceGeneration =
						source.sourceGeneration ^ (destination.sourceGeneration + 0x9e3779b97f4a7c15ULL + (source.sourceGeneration << 6U) +
												   (source.sourceGeneration >> 2U));
					if(blended.sourceGeneration == 0U)
						blended.sourceGeneration = 1U;
				}
				rootStatus = ApplyRetargetRootMotionPolicy(item, blended);
				if(rootStatus == AnimationStatus::Ok) {
					// Ignore remains a complete copied result. Keeping its source
					// interval and generations proves that policy intentionally
					// discarded the blended transition delta.
					rootStatus = item.pose->SetRootMotion(blended);
				}
			}
			if(rootStatus != AnimationStatus::Ok && item.evaluationStatus == AnimationStatus::Ok) {
				item.evaluationStatus = rootStatus;
			}
		}
	}

	void AnimationSystem::Impl::EvaluateCompleteAnimationStage(std::uint64_t runtimeSessionEpoch, std::uint64_t frameDeclarationEpoch,
															   const TerraEngine::Scheduling::FrameWorkloadPartition& partition,
															   TerraEngine::Scheduling::JobGraphContext& context) {

		TERRAENGINE_PROFILE_SCOPE("Animation: Evaluate Complete Range", TerraEngine::Profiling::ProfilerModuleId::CpuEngine);
		if(context.IsCancellationRequested()) {
			this->RequestFrameCancellation();
			return;
		}
		if(!this->runtimeActive.load(std::memory_order_acquire) ||
		   this->runtimeSessionEpoch.load(std::memory_order_acquire) != runtimeSessionEpoch ||
		   this->frameDeclarationEpoch.load(std::memory_order_acquire) != frameDeclarationEpoch)
			return;
		this->EvaluatePartition(partition.partitionIndex, partition.partitionCount);
	}

} // namespace TerraEngine::Animation
