#include "Animation/Frame/AnimationFrameRuntime.h"

namespace TerraEngine::Animation::FrameDetail {
	AnimationMemoryConfiguration MakeAnimationMemoryConfiguration() noexcept {
		AnimationMemoryConfiguration configuration;
		configuration.classes[static_cast<std::size_t>(AnimationMemoryClass::ScratchPool)].maximumBytes = kMaximumAnimationScratchBytes;
		configuration.classes[static_cast<std::size_t>(AnimationMemoryClass::FrameArena)].maximumBytes = kMaximumAnimationScratchBytes;
		configuration.classes[static_cast<std::size_t>(AnimationMemoryClass::PosePool)].maximumBytes = kMaximumAnimationPoseBytes;
		configuration.maximumTotalBytes = kMaximumAnimationMemoryBytes;
		configuration.maximumReservations = kMaximumAnimationMemoryReservations;
		return configuration;
	}

	NativeAnimationAbiHeader MakeNativeHeader(NativeAnimationRecordKind kind, std::size_t size) noexcept {
		return {static_cast<std::uint32_t>(size), kNativeAnimationAbiVersion, static_cast<std::uint16_t>(kind)};
	}

	NativeAnimationStatus ToNativeStatus(AnimationStatus status) noexcept {
		return static_cast<NativeAnimationStatus>(status);
	}

	NativeAnimationUuid ToNativeUuid(const UUID& value) noexcept {
		NativeAnimationUuid result{};
		std::memcpy(&result.first, value.data(), sizeof(result.first));
		std::memcpy(&result.second, value.data() + sizeof(result.first), sizeof(result.second));
		return result;
	}

	bool MatchesNativeUuid(NativeAnimationUuid value, const UUID& expected) noexcept {
		const NativeAnimationUuid nativeExpected = ToNativeUuid(expected);
		return value.first == nativeExpected.first && value.second == nativeExpected.second;
	}

	AnimationTransform FromNativeTransform(const NativeAnimationTransform& value) noexcept {
		AnimationTransform result;
		result.translation = {value.translation.x, value.translation.y, value.translation.z};
		result.rotation = {value.rotation.x, value.rotation.y, value.rotation.z, value.rotation.w};
		result.scale = {value.scale.x, value.scale.y, value.scale.z};
		return result;
	}

	NativeAnimationTransform ToNativeTransform(const AnimationTransform& value) noexcept {
		NativeAnimationTransform result{};
		result.translation = {value.translation.x, value.translation.y, value.translation.z};
		result.rotation = {value.rotation.x, value.rotation.y, value.rotation.z, value.rotation.w};
		result.scale = {value.scale.x, value.scale.y, value.scale.z};
		return result;
	}

	std::uint64_t MixDeterministicChecksum(std::uint64_t checksum, std::uint64_t value) noexcept {
		for(std::size_t byte = 0U; byte < sizeof(value); ++byte) {
			checksum ^= (value >> (byte * 8U)) & 0xffU;
			checksum *= 1099511628211ULL;
		}
		return checksum;
	}

	std::uint64_t QuantizedFloat(float value) noexcept {
		if(!std::isfinite(value))
			return 0U;
		const double scaled = std::round(static_cast<double>(value) * 65536.0);
		const double bounded = std::clamp(scaled, static_cast<double>(std::numeric_limits<std::int32_t>::min()),
										  static_cast<double>(std::numeric_limits<std::int32_t>::max()));
		return static_cast<std::uint32_t>(static_cast<std::int32_t>(bounded));
	}

	std::uint64_t HashDeterministicTransform(std::uint64_t checksum, const AnimationTransform& transform) noexcept {
		const std::array<float, 10U> values{transform.translation.x, transform.translation.y, transform.translation.z, transform.rotation.x,
											transform.rotation.y,	 transform.rotation.z,	  transform.rotation.w,	   transform.scale.x,
											transform.scale.y,		 transform.scale.z};
		for(const float value : values) {
			checksum = MixDeterministicChecksum(checksum, QuantizedFloat(value));
		}
		return checksum;
	}

	std::uint64_t HashAnimationGuid(std::uint64_t checksum, const AnimationGuid& value) noexcept {
		for(const std::uint8_t byte : value.value) {
			checksum ^= byte;
			checksum *= 1099511628211ULL;
		}
		return checksum;
	}

	AnimationStatus ComputeClipDeterministicContentFingerprint(const AnimationClipAsset& clip, std::uint64_t& output) noexcept {
		try {
			std::vector<std::uint8_t> bytes;
			const AnimationStatus status = SerializeAnimationClipAsset(clip, bytes);
			if(status != AnimationStatus::Ok)
				return status;
			std::uint64_t hash = 1469598103934665603ULL;
			for(const std::uint8_t byte : bytes) {
				hash ^= byte;
				hash *= 1099511628211ULL;
			}
			// FNV cannot produce zero for the valid non-empty animation format in
			// practice, but keep the public fingerprint contract fail-closed.
			if(hash == 0U)
				return AnimationStatus::ContentHashMismatch;
			output = hash;
			return AnimationStatus::Ok;
		} catch(const std::bad_alloc&) {
			return AnimationStatus::HardLimitExceeded;
		}
	}

	AnimationWorkSchedulerConfiguration MakeAnimationWorkSchedulerConfiguration() noexcept {
		AnimationWorkSchedulerConfiguration configuration;
		configuration.inlineItemThreshold = kInlineAnimatorThreshold;
		configuration.targetItemsPerRange = kAnimatorsPerWorkerPartition;
		configuration.maximumRangeCount = 65536U;
		configuration.maximumItemCount = 65536U;
		configuration.maximumScratchBytes = kMaximumAnimationScratchBytes;
		configuration.scratchAlignment = alignof(std::uint64_t);
		return configuration;
	}

	AnimationFallbackReason FallbackReasonForStatus(AnimationStatus status) noexcept {
		switch(status) {
		case AnimationStatus::MissingAsset:
			return AnimationFallbackReason::MissingAsset;
		case AnimationStatus::IncompatibleAsset:
			return AnimationFallbackReason::IncompatibleAsset;
		case AnimationStatus::Cancelled:
			return AnimationFallbackReason::Cancelled;
		case AnimationStatus::StaleHandle:
			return AnimationFallbackReason::StaleGeneration;
		case AnimationStatus::HardLimitExceeded:
		case AnimationStatus::RangeOverflow:
			return AnimationFallbackReason::BudgetPressure;
		case AnimationStatus::InvalidArgument:
		case AnimationStatus::Rejected:
			return AnimationFallbackReason::ValidationFailure;
		default:
			return AnimationFallbackReason::InternalFailure;
		}
	}

	void ConfigureProtectedWorkload(TerraEngine::Scheduling::FrameWorkloadDeclaration& workload, std::string name,
									TerraEngine::Scheduling::WorkerSystemPhaseKind phaseKind, std::vector<std::string> reads,
									std::vector<std::string> writes, std::string protectedReason) {
		using namespace TerraEngine::Scheduling;
		workload.workloadName = std::move(name);
		workload.ownerSystem = "AnimationSystem";
		workload.phase = "Animation";
		workload.stableDescriptorId = "AnimationSystem/Animation/" + workload.workloadName;
		workload.activationPredicateKey = "animation-active-membership";
		workload.reads = std::move(reads);
		workload.writes = std::move(writes);
		workload.workerEligible = false;
		workload.mainThreadRequired = true;
		workload.mutationPolicy = FrameWorkloadMutationPolicy::DeferredOnly;
		workload.phaseKind = phaseKind;
		workload.boundaryClassification = HardBoundaryClassification::Protected;
		workload.hardBoundary = true;
		workload.protectedCommit = protectedReason;
		workload.protectedReason = std::move(protectedReason);
		workload.profilingLabel = "Animation." + workload.workloadName;
	}

	void ConfigureWorkerWorkload(TerraEngine::Scheduling::FrameWorkloadDeclaration& workload, std::string name,
								 std::vector<std::string> reads, std::vector<std::string> writes, std::string safeSubwork) {
		using namespace TerraEngine::Scheduling;
		workload.workloadName = std::move(name);
		workload.ownerSystem = "AnimationSystem";
		workload.phase = "Animation";
		workload.stableDescriptorId = "AnimationSystem/Animation/" + workload.workloadName;
		workload.activationPredicateKey = "animation-active-membership";
		workload.reads = std::move(reads);
		workload.writes = std::move(writes);
		workload.inlineWorkItemThreshold = kInlineAnimatorThreshold;
		workload.partitionPolicy = FrameWorkloadPartitionPolicy::DeterministicRanges;
		workload.partitionWorkItemCount = kAnimatorsPerWorkerPartition;
		workload.workerEligible = true;
		workload.mainThreadRequired = false;
		workload.mutationPolicy = FrameWorkloadMutationPolicy::None;
		workload.phaseKind = WorkerSystemPhaseKind::WorkerPrepare;
		workload.boundaryClassification = HardBoundaryClassification::WorkerSafe;
		workload.safeSubwork = std::move(safeSubwork);
		workload.profilingLabel = "Animation." + workload.workloadName;
	}

	AnimationStatus ApplyPlaybackCommand(AnimatorPlayback& playback, const AnimationCommand& command) noexcept {
		switch(command.kind) {
		case AnimationCommandKind::Play:
			return playback.Play();
		case AnimationCommandKind::Stop:
			return playback.Stop();
		case AnimationCommandKind::Pause:
			return playback.Pause();
		case AnimationCommandKind::Resume:
			return playback.Resume();
		case AnimationCommandKind::Seek:
			return playback.Seek(command.playbackTick);
		case AnimationCommandKind::SetLooping:
			return playback.SetLooping(command.playbackFlag);
		case AnimationCommandKind::SetReverse:
			return playback.SetReverse(command.playbackFlag);
		case AnimationCommandKind::SetSpeed:
			return playback.SetSpeed(command.playbackSpeed);
		case AnimationCommandKind::CrossFade:
		case AnimationCommandKind::SetLayerWeight:
			// Controller commands need the immutable program and layer table, so the
			// planning loop applies them after ordinary state-machine advancement.
			return AnimationStatus::InvalidType;
		case AnimationCommandKind::SetParameter:
		case AnimationCommandKind::FireTrigger:
			// Controller parameters are captured now but become meaningful when the
			// immutable controller evaluator is completed in future controller work.
			return AnimationStatus::Ok;
		default:
			return AnimationStatus::InvalidArgument;
		}
	}

	AnimationStatus ApplyParameterCommand(std::vector<AnimationParameterSnapshot>& parameters, const AnimationCommand& command) {
		if(command.kind != AnimationCommandKind::SetParameter && command.kind != AnimationCommandKind::FireTrigger)
			return AnimationStatus::InvalidType;
		const auto found = std::find_if(parameters.begin(), parameters.end(), [&command](const AnimationParameterSnapshot& value) {
			return value.parameterId == command.parameterId;
		});
		AnimationParameterSnapshot next;
		next.parameterId = command.parameterId;
		next.type = command.kind == AnimationCommandKind::FireTrigger ? AnimationParameterType::Trigger : command.parameterType;
		next.value = command.value;
		if(found == parameters.end())
			parameters.push_back(std::move(next));
		else
			*found = std::move(next);
		return AnimationStatus::Ok;
	}

	std::uint32_t StableParameterId(std::string_view name) noexcept {
		std::uint32_t numeric = 0U;
		const auto numericResult = std::from_chars(name.data(), name.data() + name.size(), numeric);
		if(numericResult.ec == std::errc{} && numericResult.ptr == name.data() + name.size() && numeric != 0U)
			return numeric;
		// FNV-1a matches the engine's other stable copied fingerprints and does
		// not depend on the process-specific std::hash implementation.
		std::uint32_t hash = 2166136261U;
		for(const unsigned char byte : name) {
			hash ^= byte;
			hash *= 16777619U;
		}
		return hash == 0U ? 1U : hash;
	}

	AnimationStatus ParseInitialParameters(std::string_view payload, std::vector<AnimationParameterSnapshot>& output) {
		try {
			const nlohmann::json document = nlohmann::json::parse(payload.empty() ? "{}" : payload);
			if(!document.is_object())
				return AnimationStatus::InvalidType;
			if(ValidateAnimationHardLimit(AnimationLimit::ControllerParameters, document.size()) != AnimationStatus::Ok)
				return AnimationStatus::HardLimitExceeded;
			std::vector<AnimationParameterSnapshot> staged;
			staged.reserve(document.size());
			for(const auto& [name, authored] : document.items()) {
				AnimationParameterSnapshot parameter;
				parameter.parameterId = StableParameterId(name);
				const nlohmann::json* value = &authored;
				std::string explicitType;
				if(authored.is_object() && authored.contains("type") && authored.contains("value") && authored["type"].is_string()) {
					explicitType = authored["type"].get<std::string>();
					value = &authored["value"];
				}
				if(value->is_boolean() && (explicitType.empty() || explicitType == "Boolean")) {
					parameter.type = AnimationParameterType::Boolean;
					parameter.value = value->get<bool>();
				} else if(value->is_number_integer() && (explicitType.empty() || explicitType == "Integer")) {
					const std::int64_t integer = value->get<std::int64_t>();
					if(integer < std::numeric_limits<std::int32_t>::min() || integer > std::numeric_limits<std::int32_t>::max())
						return AnimationStatus::RangeOverflow;
					parameter.type = AnimationParameterType::Integer;
					parameter.value = static_cast<std::int32_t>(integer);
				} else if(value->is_number() && (explicitType.empty() || explicitType == "Float")) {
					const float scalar = value->get<float>();
					if(!std::isfinite(scalar))
						return AnimationStatus::InvalidArgument;
					parameter.type = AnimationParameterType::Float;
					parameter.value = scalar;
				} else if(value->is_array() && value->size() == 3U && (explicitType.empty() || explicitType == "Vector3")) {
					AnimationFloat3 vector{(*value)[0].get<float>(), (*value)[1].get<float>(), (*value)[2].get<float>()};
					if(!IsFinite(vector))
						return AnimationStatus::InvalidArgument;
					parameter.type = AnimationParameterType::Vector3;
					parameter.value = vector;
				} else if(value->is_array() && value->size() == 4U && explicitType == "Quaternion") {
					AnimationQuaternion rotation{(*value)[0].get<float>(), (*value)[1].get<float>(), (*value)[2].get<float>(),
												 (*value)[3].get<float>()};
					if(!IsFinite(rotation))
						return AnimationStatus::InvalidArgument;
					parameter.type = AnimationParameterType::Quaternion;
					parameter.value = rotation;
				} else if(value->is_object() && explicitType == "Transform" && value->contains("translation") &&
						  value->contains("rotation") && value->contains("scale")) {
					const nlohmann::json& translation = (*value)["translation"];
					const nlohmann::json& rotation = (*value)["rotation"];
					const nlohmann::json& scale = (*value)["scale"];
					if(!translation.is_array() || translation.size() != 3U || !rotation.is_array() || rotation.size() != 4U ||
					   !scale.is_array() || scale.size() != 3U)
						return AnimationStatus::InvalidType;
					AnimationTransform transform;
					transform.translation = {translation[0].get<float>(), translation[1].get<float>(), translation[2].get<float>()};
					transform.rotation = {rotation[0].get<float>(), rotation[1].get<float>(), rotation[2].get<float>(),
										  rotation[3].get<float>()};
					transform.scale = {scale[0].get<float>(), scale[1].get<float>(), scale[2].get<float>()};
					if(!IsFinite(transform))
						return AnimationStatus::InvalidArgument;
					parameter.type = AnimationParameterType::Transform;
					parameter.value = transform;
				} else {
					return AnimationStatus::InvalidType;
				}
				staged.push_back(std::move(parameter));
			}
			std::sort(staged.begin(), staged.end(),
					  [](const auto& left, const auto& right) { return left.parameterId < right.parameterId; });
			if(std::adjacent_find(staged.begin(), staged.end(), [](const auto& left, const auto& right) {
				   return left.parameterId == right.parameterId;
			   }) != staged.end())
				return AnimationStatus::Rejected;
			output.swap(staged);
			return AnimationStatus::Ok;
		} catch(const nlohmann::json::exception&) {
			return AnimationStatus::InvalidEnvelope;
		} catch(const std::bad_alloc&) {
			return AnimationStatus::InternalError;
		}
	}

	AnimationQuaternion MultiplyRotation(const AnimationQuaternion& left, const AnimationQuaternion& right) noexcept {
		AnimationQuaternion value{left.w * right.x + left.x * right.w + left.y * right.z - left.z * right.y,
								  left.w * right.y - left.x * right.z + left.y * right.w + left.z * right.x,
								  left.w * right.z + left.x * right.y - left.y * right.x + left.z * right.w,
								  left.w * right.w - left.x * right.x - left.y * right.y - left.z * right.z};
		const float lengthSquared = value.x * value.x + value.y * value.y + value.z * value.z + value.w * value.w;
		if(!std::isfinite(lengthSquared) || lengthSquared <= 1.0e-12F)
			return {};
		const float inverseLength = 1.0F / std::sqrt(lengthSquared);
		value.x *= inverseLength;
		value.y *= inverseLength;
		value.z *= inverseLength;
		value.w *= inverseLength;
		return value;
	}

	AnimationQuaternion ConjugateRotation(AnimationQuaternion value) noexcept {
		value.x = -value.x;
		value.y = -value.y;
		value.z = -value.z;
		return value;
	}

	AnimationFloat3 RotateVector(const AnimationQuaternion& rotation, const AnimationFloat3& vector) noexcept {
		const AnimationFloat3 twiceCross{2.0F * (rotation.y * vector.z - rotation.z * vector.y),
										 2.0F * (rotation.z * vector.x - rotation.x * vector.z),
										 2.0F * (rotation.x * vector.y - rotation.y * vector.x)};
		return {vector.x + rotation.w * twiceCross.x + (rotation.y * twiceCross.z - rotation.z * twiceCross.y),
				vector.y + rotation.w * twiceCross.y + (rotation.z * twiceCross.x - rotation.x * twiceCross.z),
				vector.z + rotation.w * twiceCross.z + (rotation.x * twiceCross.y - rotation.y * twiceCross.x)};
	}

	AnimationTransform CombineTransform(const AnimationTransform& parent, const AnimationTransform& local) noexcept {
		const AnimationFloat3 scaled{local.translation.x * parent.scale.x, local.translation.y * parent.scale.y,
									 local.translation.z * parent.scale.z};
		const AnimationFloat3 rotated = RotateVector(parent.rotation, scaled);
		AnimationTransform model;
		model.translation = {parent.translation.x + rotated.x, parent.translation.y + rotated.y, parent.translation.z + rotated.z};
		model.rotation = MultiplyRotation(parent.rotation, local.rotation);
		model.scale = {parent.scale.x * local.scale.x, parent.scale.y * local.scale.y, parent.scale.z * local.scale.z};
		return model;
	}

	bool TryMapRagdollBodyWorldToJointModel(const AnimationTransform& objectWorld, const AnimationTransform& animationJointModel,
											const RagdollBodyRecord& body, const AnimationTransform& bodyWorld,
											AnimationTransform& output) noexcept {
		// Jolt publishes a rigid body in world space, while animation blends a
		// skeleton joint in object/model space. Undo the authored joint-to-body
		// transform before removing the object's world transform; otherwise a
		// non-origin character or collider offset would visibly jump on activation.
		const AnimationTransform animationJointWorld = CombineTransform(objectWorld, animationJointModel);
		AnimationTransform jointWorld;
		jointWorld.rotation = MultiplyRotation(bodyWorld.rotation, ConjugateRotation(body.jointToBodyOrientation));
		jointWorld.scale = animationJointWorld.scale;
		const AnimationFloat3 scaledOffset{body.jointToBodyOffset.x * jointWorld.scale.x, body.jointToBodyOffset.y * jointWorld.scale.y,
										   body.jointToBodyOffset.z * jointWorld.scale.z};
		const AnimationFloat3 worldOffset = RotateVector(jointWorld.rotation, scaledOffset);
		jointWorld.translation = {bodyWorld.translation.x - worldOffset.x, bodyWorld.translation.y - worldOffset.y,
								  bodyWorld.translation.z - worldOffset.z};
		return IsFinite(jointWorld) && ProceduralMath::TryRelativeTransform(objectWorld, jointWorld, output);
	}

	bool CaptureObjectWorldTransform(ObjectManager& objects, Object object, AnimationTransform& output) noexcept {
		// The propagated canonical pose is the sole world-space authority. The
		// animation graph receives a bounded offset for local joint math only.
		const std::optional<TerraEngine::Scene::WorldPose> pose = objects.GetCanonicalWorldPose(object);
		if(!pose)
			return false;
		output.translation = {static_cast<float>(pose->position.offsetMetres.x), static_cast<float>(pose->position.offsetMetres.y),
							  static_cast<float>(pose->position.offsetMetres.z)};
		output.rotation = {pose->rotation.x, pose->rotation.y, pose->rotation.z, pose->rotation.w};
		output.scale = {pose->scale.x, pose->scale.y, pose->scale.z};
		return IsFinite(output) && IsNormalized(output.rotation);
	}

	AnimationPlaybackSnapshot MakePublishedPlaybackSnapshot(const AnimatorPlaybackStateSnapshot& source) noexcept {
		AnimationPlaybackSnapshot snapshot;
		snapshot.currentStateId = 1U;
		snapshot.transitionProgress = 0.0F;
		snapshot.normalizedTime = source.normalizedTime;
		snapshot.loopCount = source.loopCount;
		snapshot.effectiveSpeed = source.effectiveSpeed;
		snapshot.latestStatus = source.latestStatus;
		return snapshot;
	}
} // namespace TerraEngine::Animation::FrameDetail

namespace TerraEngine::Animation {
	AnimationSystem::Impl::Impl() {
		(void)persistentReuse.memoryController.Configure(MakeAnimationMemoryConfiguration());
	}

	[[nodiscard]] AnimationSystem::Impl::FrameWorkItem* AnimationSystem::Impl::CompleteWorkForInputIndex(std::size_t inputIndex) noexcept {
		if(inputIndex >= frameInputs.capturedInputWorkIndices.size())
			return nullptr;
		const CapturedInputWorkIndex& captured = frameInputs.capturedInputWorkIndices[inputIndex];
		return !captured.compact && captured.storageIndex < frameStorage.frameWork.size() ? &frameStorage.frameWork[captured.storageIndex]
																						  : nullptr;
	}

	[[nodiscard]] const AnimationSystem::Impl::FrameWorkItem*
	AnimationSystem::Impl::CompleteWorkForInputIndex(std::size_t inputIndex) const noexcept {
		return const_cast<Impl*>(this)->CompleteWorkForInputIndex(inputIndex);
	}

	void AnimationSystem::Impl::ResetFrameIntegrationStateLocked() noexcept {
		// Protected reset follows the previous workload boundary. Nested producers
		// settle before returning, so no admitted child still borrows these arrays.
		evaluationState.workCancellationSource.RequestCancellation();
		(void)evaluationState.evaluationCancellationSource.Cancel(AnimationCancellationReason::OwnerReleased);
		evaluationState.workCancellationSource = AnimationWorkCancellationSource{};
		evaluationState.evaluationCancellationSource = AnimationCancellationSource{};
		evaluationState.evaluationCancellation = {};
		frameStorage.workPlan = {};
		evaluationState.workPlanRejectionReason = AnimationPlanRejectionReason::None;
		evaluationState.workPlanRejectionItemIndex = std::numeric_limits<std::size_t>::max();
		frameStorage.workRangeLedger.reset();
		// Move the last complete frame into candidate ownership
		// before clearing the active view. CaptureFrameLocked can now
		// reuse each animator's validated clip view and vector
		// storage, while a failed capture still leaves frameWork
		// empty so later phases cannot evaluate stale work.
		persistentReuse.candidateFrameWork.swap(frameStorage.frameWork);
		frameStorage.frameWork.clear();
		persistentReuse.candidateCompactGpuVisualCaptures = std::move(frameInputs.compactGpuVisualCaptures);
		frameInputs.compactGpuVisualCaptures.clear();
		persistentReuse.candidateCompactRuntimeCaptureRoot = std::move(frameInputs.compactRuntimeCaptureRoot);
		persistentReuse.candidateCompactRuntimeStateCaptureRoot = std::move(frameInputs.compactRuntimeStateCaptureRoot);
		persistentReuse.candidateInputWorkIndices.swap(frameInputs.capturedInputWorkIndices);
		frameInputs.capturedInputWorkIndices.clear();
		frameInputs.compactCaptureAdmissionProof.clear();
		frameInputs.capturedRetargetRequests.clear();
		frameInputs.capturedMotionMatchRequests.clear();
		frameInputs.capturedMotionMatchQueriesV2.clear();
		frameInputs.capturedRagdollRequests.clear();
		frameInputs.capturedDeterminismRequests.clear();
		frameInputs.determinismRequestCompletions.clear();
		evaluationState.capturedWorkItems.clear();
		evaluationState.workCompletions.clear();
		evaluationState.compactGpuVisualWorkItems.clear();
		evaluationState.compactGpuVisualCompletions.clear();
		evaluationState.deterministicPublicationOrder.clear();
		frameStorage.frameScratch.clear();
		frameStorage.frameScratchLease.Release();
		frameStorage.framePoseReservation.reset();
		frameInputs.frameLifecycleGenerations = {};
		frameInputs.frameSceneGeneration = 0U;
		frameInputs.frameHasConsumerDeclarations = false;
		frameInputs.frameConsumerReadView = {};
		evaluationState.deterministicPublicationValidated = false;
		// A lifecycle reset may run between root exchange and its
		// dependent finalizer.  Dropping the scalar receipt prevents
		// that stale frame from applying internal runtime state after
		// its authority root was retired.
		authorityState.publishedAuthorityCommit = {};
		authorityState.preparedDiagnostics.reset();
		authorityState.preparedDetailedDiagnostics.reset();
		authorityState.preparedPresentationFrame.reset();
		evaluationState.workPlanValidated = false;
		authorityState.retainsPublishedAuthorityRoot = false;
		evaluationState.availableWorkerCount = 0U;
		evaluationState.frameCancellationRequested.store(false, std::memory_order_release);
		evaluationState.cacheReusedAnimatorCount.store(0U, std::memory_order_release);
		evaluationState.sharedPoseAnimatorCount.store(0U, std::memory_order_release);
		evaluationState.fallbackAnimatorCount.store(0U, std::memory_order_release);
		evaluationState.compactGpuVisualCompletedCount.store(0U, std::memory_order_release);
		evaluationState.compactPlanningNanoseconds.store(0U, std::memory_order_release);
		evaluationState.compactPreCopyCaptureCount.store(0U, std::memory_order_release);
		evaluationState.completePlanningNanoseconds.store(0U, std::memory_order_release);
		evaluationState.compactEvaluationNanoseconds.store(0U, std::memory_order_release);
		evaluationState.completeEvaluationNanoseconds.store(0U, std::memory_order_release);
		{
			std::scoped_lock finalizeLock(evaluationState.workPlanFinalizeMutex);
			evaluationState.workPlanFinalizeState = WorkPlanFinalizeState::Pending;
		}
	}

	void AnimationSystem::Impl::RequestFrameCancellation(AnimationCancellationReason reason) noexcept {
		(void)evaluationState.evaluationCancellationSource.Cancel(reason);
		evaluationState.workCancellationSource.RequestCancellation();
		evaluationState.frameCancellationRequested.store(true, std::memory_order_release);
	}

	bool AnimationSystem::Impl::PollFrameCancellation() noexcept {
		const bool cancelled = lifecycleLatch == nullptr || !evaluationState.evaluationCancellation.IsValid() ||
							   evaluationState.evaluationCancellation.IsCancellationRequested(*lifecycleLatch);
		if(cancelled) {
			evaluationState.workCancellationSource.RequestCancellation();
			evaluationState.frameCancellationRequested.store(true, std::memory_order_release);
		}
		return cancelled;
	}

	AnimationStatus AnimationSystem::Impl::InvalidateLifecycleLocked(AnimationLifecycleBoundary boundary) noexcept {
		if(lifecycleLatch == nullptr)
			return AnimationStatus::Cancelled;
		const AnimationStatus status = lifecycleLatch->Invalidate(boundary);
		if(status == AnimationStatus::Ok || status == AnimationStatus::Cancelled) {
			const std::uint64_t currentGeneration = lifecycleLatch->Capture().Generation(boundary);
			(void)evaluationState.evaluationCancellationSource.Cancel(CancellationReasonForBoundary(boundary, currentGeneration));
			evaluationState.workCancellationSource.RequestCancellation();
			evaluationState.frameCancellationRequested.store(true, std::memory_order_release);
		}
		return status;
	}

	void AnimationSystem::Impl::RetireLifecyclePublicationStateLocked() noexcept {
		// Resolve copied commands before retirement discards their commit token.
		// Accepted historical commits survive; uncommitted work is cancelled.
		PublishCommittedOrdinaryCommandsLocked();
		SettleFailedFrameCommandsLocked(AnimationStatus::Cancelled, "LifecycleRetirement");
		// A lifecycle boundary is different from an ordinary failed
		// frame: the old immutable result names a world that no
		// longer exists.  Clear the public root before releasing
		// worker-owned candidate memory so a fresh reader can never
		// acquire an otherwise well-formed stale authority frame.
		const AnimationLifecycleGenerationSnapshot lifecycle =
			lifecycleLatch ? lifecycleLatch->Capture() : AnimationLifecycleGenerationSnapshot{};
		authorityPublisher.Clear(runtimeSessionEpoch.load(std::memory_order_acquire),
												lifecycle.Generation(AnimationLifecycleBoundary::Scene));

		// ResetFrameIntegrationStateLocked drops the sealed commit
		// token and its sidecars.  It swaps reusable frame buffers,
		// so clear both sides here: lifecycle transitions release
		// old-world references instead of retaining them as the next
		// frame's allocation cache.
		ResetFrameIntegrationStateLocked();
		authorityState.sealedAuthorityCandidate = {};
		authorityState.sealedAuthorityCandidateReady = false;
		frameInputs.capturedFrameInputs.clear();
		frameInputs.capturedCommands.clear();
		frameInputs.qualifiedControllerResourceRevision = 0U;
		frameInputs.capturedCommandStatuses.clear();
		frameInputs.capturedProceduralCommands.clear();
		frameInputs.capturedProceduralCommandStatuses.clear();
		frameInputs.capturedProceduralGoalRanges.clear();
		frameInputs.capturedProceduralGoals.clear();
		frameInputs.capturedRetargetRequests.clear();
		frameInputs.capturedMotionMatchRequests.clear();
		frameInputs.capturedMotionMatchQueriesV2.clear();
		frameInputs.capturedRagdollRequests.clear();
		frameInputs.capturedDeterminismRequests.clear();
		pendingRootMotionApplications.clear();
		frameStorage.frameWork.clear();
		persistentReuse.candidateFrameWork.clear();
		frameInputs.compactGpuVisualCaptures.clear();
		persistentReuse.candidateCompactGpuVisualCaptures.clear();
		frameInputs.compactRuntimeCaptureRoot.reset();
		persistentReuse.candidateCompactRuntimeCaptureRoot.reset();
		frameInputs.compactRuntimeStateCaptureRoot.reset();
		persistentReuse.candidateCompactRuntimeStateCaptureRoot.reset();
		frameInputs.capturedInputWorkIndices.clear();
		persistentReuse.candidateInputWorkIndices.clear();
		frameInputs.compactCaptureAdmissionProof.clear();
		persistentReuse.gpuPresentationFramePool = {};
		persistentReuse.gpuPresentationFramePoolCursor = 0U;
		persistentReuse.authorityPagePool = {};
		persistentReuse.authorityPagePoolCursor = 0U;
		persistentReuse.authoritySidecarPool = {};
		persistentReuse.authoritySidecarPoolCursor = 0U;
		evaluationState.publishedAnimatorCount.store(0U, std::memory_order_release);
		// The scalar generation is observable by deferred producers.
		// Advancing it makes every receipt captured before this
		// lifecycle boundary unambiguously older even though the
		// replacement authority root is intentionally empty.
		authorityPublicationGeneration.fetch_add(1U, std::memory_order_acq_rel);
		frameDeclarationEpoch.fetch_add(1U, std::memory_order_acq_rel);
	}

	void AnimationSystem::Impl::ActivateRuntimeOwnershipLocked() {
		// AnimatorInstanceStore intentionally survives every shutdown so its
		// slot generations cannot restart and alias handles from an old session.
		if(commands == nullptr)
			commands = std::make_unique<AnimationCommandQueue>();
		if(proceduralCommands == nullptr) {
			proceduralCommands = std::make_unique<ProceduralCommandStore>();
		}
		if(advancedCommands == nullptr) {
			advancedCommands = std::make_unique<AdvancedAnimationCommandStore>();
		}
		if(lifecycleLatch == nullptr || lifecycleLatch->Capture().Generation(AnimationLifecycleBoundary::Shutdown) == 0U) {
			lifecycleLatch = std::make_unique<AnimationGenerationLatch>();
		}
		(void)persistentReuse.memoryController.Configure(MakeAnimationMemoryConfiguration());
		ResetFrameIntegrationStateLocked();
		// Publish activity only after every owned runtime object is restored.
		runtimeActive.store(true, std::memory_order_release);
	}

	[[nodiscard]] const AnimationSystem::Impl::CompactRuntimeState*
	AnimationSystem::Impl::FindCompactRuntimeState(const std::shared_ptr<const CompactRuntimeStateRoot>& root,
												   AnimatorInstanceHandle handle) noexcept {
		if(!root || !handle.IsValid() || handle.slotIndex >= root->size())
			return nullptr;
		const CompactRuntimeState& state = root->at(handle.slotIndex);
		return state.IsValidFor(handle) ? &state : nullptr;
	}

	[[nodiscard]] bool AnimationSystem::Impl::MaterializeCompactRuntimeStateLocked(AnimatorInstanceHandle handle,
																				   RuntimeRecord& record) noexcept {
		std::shared_ptr<const CompactRuntimeStateRoot> expected = compactRuntimeStates.load(std::memory_order_acquire);
		const CompactRuntimeState* state = FindCompactRuntimeState(expected, handle);
		if(state == nullptr)
			return true;
		try {
			auto next = std::make_shared<CompactRuntimeStateRoot>(*expected);
			next->at(handle.slotIndex) = {};
			std::shared_ptr<const CompactRuntimeStateRoot> published = std::move(next);
			if(!compactRuntimeStates.compare_exchange_strong(expected, published, std::memory_order_acq_rel, std::memory_order_acquire))
				return false;
			record.playback = state->playback;
			record.poseGeneration = state->poseGeneration;
			record.stateGeneration = state->stateGeneration;
			record.executionModeGeneration = state->executionModeGeneration;
			record.executionModeState = state->executionModeState;
			return true;
		} catch(const std::bad_alloc&) {
			// Authored mutation remains fail-closed. Leaving the compact root intact
			// is safer than exposing only part of its scalar state.
			return false;
		}
	}

	AnimationSystem::Impl::RuntimeRecord* AnimationSystem::Impl::FindRuntimeRecordLocked(AnimatorInstanceHandle handle) noexcept {
		const std::shared_ptr<RuntimeRecordRoot> root = runtimeRecords.load(std::memory_order_acquire);
		if(!handle.IsValid() || root == nullptr || handle.slotIndex >= root->size())
			return nullptr;
		RuntimeRecord* record = root->at(handle.slotIndex).get();
		if(record == nullptr || record->handle != handle)
			return nullptr;
		if(!MaterializeCompactRuntimeStateLocked(handle, *record))
			return nullptr;
		return record;
	}

	const AnimationSystem::Impl::RuntimeRecord*
	AnimationSystem::Impl::FindRuntimeRecordLocked(AnimatorInstanceHandle handle) const noexcept {
		const std::shared_ptr<RuntimeRecordRoot> root = runtimeRecords.load(std::memory_order_acquire);
		if(!handle.IsValid() || root == nullptr || handle.slotIndex >= root->size())
			return nullptr;
		const RuntimeRecord* record = root->at(handle.slotIndex).get();
		return record != nullptr && record->handle == handle ? record : nullptr;
	}

	AnimationNetworkFloat3Bits AnimationSystem::Impl::ToNetworkBits(const AnimationFloat3& value) noexcept {
		return {{EncodeAnimationNetworkFloat(value.x), EncodeAnimationNetworkFloat(value.y), EncodeAnimationNetworkFloat(value.z)}};
	}

	AnimationNetworkQuaternionBits AnimationSystem::Impl::ToNetworkBits(const AnimationQuaternion& value) noexcept {
		return {{EncodeAnimationNetworkFloat(value.x), EncodeAnimationNetworkFloat(value.y), EncodeAnimationNetworkFloat(value.z),
				 EncodeAnimationNetworkFloat(value.w)}};
	}

	AnimationNetworkTransformBits AnimationSystem::Impl::ToNetworkBits(const AnimationTransform& value) noexcept {
		return {ToNetworkBits(value.translation), ToNetworkBits(value.rotation), ToNetworkBits(value.scale)};
	}

	std::uint64_t AnimationSystem::Impl::RuntimePoseChecksum(const PoseBuffer& pose) noexcept {
		std::uint64_t hash = 1469598103934665603ULL;
		for(const AnimationTransform& transform : pose.ModelTransforms()) {
			hash = HashDeterministicTransform(hash, transform);
		}
		for(const AnimationNamedFloatValue& value : pose.MorphValues()) {
			hash = MixDeterministicChecksum(hash, value.stableId);
			hash = MixDeterministicChecksum(hash, QuantizedFloat(value.value));
		}
		for(const AnimationNamedFloatValue& value : pose.CurveValues()) {
			hash = MixDeterministicChecksum(hash, value.stableId);
			hash = MixDeterministicChecksum(hash, QuantizedFloat(value.value));
		}
		return hash;
	}

	std::uint64_t AnimationSystem::Impl::RuntimeStateChecksum(const RuntimeRecord& record) noexcept {
		const AnimatorPlaybackStateSnapshot playback = record.playback.Snapshot();
		std::uint64_t hash = 1469598103934665603ULL;
		hash = MixDeterministicChecksum(hash, record.binding.stateId);
		hash = MixDeterministicChecksum(hash, static_cast<std::uint64_t>(playback.absoluteTick));
		hash = MixDeterministicChecksum(hash, static_cast<std::uint64_t>(playback.loopCount));
		hash = MixDeterministicChecksum(hash, QuantizedFloat(playback.normalizedTime));
		hash = MixDeterministicChecksum(hash, record.activeRetargetProfileGeneration);
		hash = MixDeterministicChecksum(hash, record.activeMotionDatabaseGeneration);
		hash = MixDeterministicChecksum(hash, record.ragdollGeneration.publicationGeneration);
		return hash;
	}

	AnimationDeterministicFingerprint AnimationSystem::Impl::BuildDeterministicFingerprint(const RuntimeRecord& record) noexcept {
		AnimationDeterministicFingerprint result;
		result.compatibility = record.binding.skeleton ? record.binding.skeleton->compatibilityFingerprint : 0U;

		const AnimationGuid controllerId = record.descriptor.controller.IsValid()
											   ? record.descriptor.controller.assetId
											   : (record.binding.clip ? record.binding.clip->assetId : AnimationGuid{});
		result.controllerAsset = HashAnimationGuid(1469598103934665603ULL, controllerId);
		result.programAsset = record.binding.evaluationProgram && record.binding.evaluationProgram->stableHash != 0U
								  ? record.binding.evaluationProgram->stableHash
								  : record.binding.clipDeterministicContentFingerprint;
		if(record.binding.retargetProfile) {
			result.retargetAsset = HashAnimationGuid(1469598103934665603ULL, record.binding.retargetProfile->assetId);
		}
		if(record.binding.motionDatabase) {
			result.motionDatabaseAsset = HashAnimationGuid(1469598103934665603ULL, record.binding.motionDatabase->assetId);
		}
		if(record.binding.ragdollProfile) {
			result.ragdollProfileAsset = HashAnimationGuid(1469598103934665603ULL, record.binding.ragdollProfile->assetId);
		}

		std::uint64_t aggregate = 1469598103934665603ULL;
		aggregate = MixDeterministicChecksum(aggregate, result.compatibility);
		aggregate = MixDeterministicChecksum(aggregate, result.controllerAsset);
		aggregate = MixDeterministicChecksum(aggregate, result.programAsset);
		aggregate = MixDeterministicChecksum(aggregate, result.retargetAsset);
		aggregate = MixDeterministicChecksum(aggregate, result.motionDatabaseAsset);
		aggregate = MixDeterministicChecksum(aggregate, result.ragdollProfileAsset);
		aggregate = MixDeterministicChecksum(aggregate, record.binding.clipGeneration);
		aggregate = MixDeterministicChecksum(aggregate, record.binding.skeletonGeneration);
		aggregate = MixDeterministicChecksum(aggregate, record.binding.programGeneration);
		aggregate = MixDeterministicChecksum(aggregate, record.binding.clipDeterministicContentFingerprint);
		if(record.binding.skeleton) {
			aggregate = HashAnimationGuid(aggregate, record.binding.skeleton->assetId);
			aggregate = MixDeterministicChecksum(aggregate, record.binding.skeleton->referencePoseHash);
		}
		result.aggregateAssetHash = aggregate;
		return result;
	}

	AnimationRollbackGenerationStamp
	AnimationSystem::Impl::BuildRollbackGenerations(const RuntimeRecord& record, const AnimationAuthorityRow& published) const noexcept {
		const AnimationLifecycleGenerationSnapshot lifecycle =
			lifecycleLatch ? lifecycleLatch->Capture() : AnimationLifecycleGenerationSnapshot{};
		auto narrowLifecycleGeneration = [](std::uint64_t value) noexcept {
			return value > std::numeric_limits<AnimationGeneration>::max() ? AnimationGeneration{0U}
																		   : static_cast<AnimationGeneration>(value);
		};
		return {published.objectGeneration != 0U ? published.objectGeneration : record.handle.generation,
				record.descriptor.componentGeneration,
				narrowLifecycleGeneration(lifecycle.Generation(AnimationLifecycleBoundary::Scene)),
				record.contentGeneration,
				record.binding.programGeneration,
				narrowLifecycleGeneration(lifecycle.Generation(AnimationLifecycleBoundary::PhysicsWorld)),
				record.historyGeneration};
	}

	AnimationStatus AnimationSystem::Impl::EncodeParameter(const AnimationParameterSnapshot& source,
														   AnimationParameterRollbackState& output) noexcept {
		AnimationParameterRollbackState candidate;
		candidate.parameterId = source.parameterId;
		candidate.type = static_cast<AnimationRollbackValueType>(source.type);
		switch(source.type) {
		case AnimationParameterType::Trigger:
			if(!std::holds_alternative<std::monostate>(source.value)) {
				return AnimationStatus::InvalidArgument;
			}
			break;
		case AnimationParameterType::Boolean: {
			const bool* value = std::get_if<bool>(&source.value);
			if(value == nullptr)
				return AnimationStatus::InvalidArgument;
			candidate.valueWords[0U] = *value ? 1U : 0U;
			break;
		}
		case AnimationParameterType::Integer: {
			const std::int32_t* value = std::get_if<std::int32_t>(&source.value);
			if(value == nullptr)
				return AnimationStatus::InvalidArgument;
			candidate.valueWords[0U] = static_cast<std::uint32_t>(*value);
			break;
		}
		case AnimationParameterType::Float: {
			const float* value = std::get_if<float>(&source.value);
			if(value == nullptr || !std::isfinite(*value))
				return AnimationStatus::InvalidArgument;
			candidate.valueWords[0U] = EncodeAnimationNetworkFloat(*value);
			break;
		}
		case AnimationParameterType::Vector3: {
			const AnimationFloat3* value = std::get_if<AnimationFloat3>(&source.value);
			if(value == nullptr || !IsFinite(*value))
				return AnimationStatus::InvalidArgument;
			const auto words = ToNetworkBits(*value).words;
			std::copy(words.begin(), words.end(), candidate.valueWords.begin());
			break;
		}
		case AnimationParameterType::Quaternion: {
			const AnimationQuaternion* value = std::get_if<AnimationQuaternion>(&source.value);
			if(value == nullptr || !IsFinite(*value))
				return AnimationStatus::InvalidArgument;
			const auto words = ToNetworkBits(*value).words;
			std::copy(words.begin(), words.end(), candidate.valueWords.begin());
			break;
		}
		case AnimationParameterType::Transform: {
			const AnimationTransform* value = std::get_if<AnimationTransform>(&source.value);
			if(value == nullptr || !IsFinite(*value))
				return AnimationStatus::InvalidArgument;
			candidate.valueWords = ToNetworkBits(*value).words();
			break;
		}
		default:
			return AnimationStatus::InvalidArgument;
		}
		output = candidate;
		return AnimationStatus::Ok;
	}

	AnimationStatus AnimationSystem::Impl::DecodeParameter(const AnimationParameterRollbackState& source,
														   AnimationParameterSnapshot& output) noexcept {
		AnimationParameterSnapshot candidate;
		if(source.parameterId > UINT32_MAX)
			return AnimationStatus::RangeOverflow;
		candidate.parameterId = static_cast<std::uint32_t>(source.parameterId);
		candidate.type = static_cast<AnimationParameterType>(source.type);
		switch(source.type) {
		case AnimationRollbackValueType::Trigger:
			candidate.value = std::monostate{};
			break;
		case AnimationRollbackValueType::Boolean:
			candidate.value = source.valueWords[0U] != 0U;
			break;
		case AnimationRollbackValueType::Integer:
			candidate.value = static_cast<std::int32_t>(source.valueWords[0U]);
			break;
		case AnimationRollbackValueType::Float:
			candidate.value = DecodeAnimationNetworkFloat(source.valueWords[0U]);
			break;
		case AnimationRollbackValueType::Vector3:
			candidate.value =
				AnimationFloat3{DecodeAnimationNetworkFloat(source.valueWords[0U]), DecodeAnimationNetworkFloat(source.valueWords[1U]),
								DecodeAnimationNetworkFloat(source.valueWords[2U])};
			break;
		case AnimationRollbackValueType::Quaternion:
			candidate.value =
				AnimationQuaternion{DecodeAnimationNetworkFloat(source.valueWords[0U]), DecodeAnimationNetworkFloat(source.valueWords[1U]),
									DecodeAnimationNetworkFloat(source.valueWords[2U]), DecodeAnimationNetworkFloat(source.valueWords[3U])};
			break;
		case AnimationRollbackValueType::Transform:
			candidate.value =
				AnimationTransform{{DecodeAnimationNetworkFloat(source.valueWords[0U]), DecodeAnimationNetworkFloat(source.valueWords[1U]),
									DecodeAnimationNetworkFloat(source.valueWords[2U])},
								   {DecodeAnimationNetworkFloat(source.valueWords[3U]), DecodeAnimationNetworkFloat(source.valueWords[4U]),
									DecodeAnimationNetworkFloat(source.valueWords[5U]), DecodeAnimationNetworkFloat(source.valueWords[6U])},
								   {DecodeAnimationNetworkFloat(source.valueWords[7U]), DecodeAnimationNetworkFloat(source.valueWords[8U]),
									DecodeAnimationNetworkFloat(source.valueWords[9U])}};
			break;
		default:
			return AnimationStatus::InvalidArgument;
		}
		output = std::move(candidate);
		return AnimationStatus::Ok;
	}

	AnimationStatus AnimationSystem::Impl::BuildRollbackSnapshotLocked(const RuntimeRecord& record, const AnimationAuthorityRow* published,
																	   AnimationRollbackSnapshot& output) const noexcept {
		if(!published || !published->snapshot || !published->snapshot->pose || !record.binding.clip || !record.binding.skeleton ||
		   record.binding.clipDeterministicContentFingerprint == 0U) {
			return AnimationStatus::NotReady;
		}
		try {
			AnimationRollbackSnapshot source;
			source.animator = record.handle;
			source.tick = published->snapshot->tick;
			// AnimationHistory retains one ring for one compatible binding epoch.
			// Frame authority remains available in the generic summary below.
			source.publicationGeneration = record.contentGeneration;
			source.frozenWorkerCount = static_cast<std::uint32_t>(
				std::max<std::size_t>(1U, std::min<std::size_t>(evaluationState.availableWorkerCount, UINT32_MAX)));
			source.fingerprint = BuildDeterministicFingerprint(record);
			source.generations = BuildRollbackGenerations(record, *published);
			if(!source.fingerprint.IsValid() || !source.generations.IsValid()) {
				return AnimationStatus::InvalidArgument;
			}

			const AnimatorPlaybackStateSnapshot playback = record.playback.Snapshot();
			const AnimatorPlaybackDeterministicState deterministicPlayback = record.playback.CaptureDeterministicState();
			AnimationControllerRollbackState controller;
			controller.controllerInstanceId = source.fingerprint.controllerAsset;
			controller.activeStateId = record.binding.stateId;
			controller.previousStateId = record.graphState.transitionSourceStateId;
			controller.stateEnteredTick = record.graphState.stateEnteredTick;
			controller.lastEvaluatedTick = std::max(record.graphState.lastTick, controller.stateEnteredTick);
			controller.deterministicRandomState = record.stateGeneration;
			source.controllers.push_back(controller);

			AnimationClipRollbackState clip;
			clip.clipInstanceId = HashAnimationGuid(1469598103934665603ULL, record.binding.clip->assetId);
			clip.clipAssetId = record.binding.clip->assetId;
			clip.clipGeneration = record.binding.clipGeneration;
			clip.clipContentFingerprint = record.binding.clipDeterministicContentFingerprint;
			clip.absoluteTick = deterministicPlayback.absoluteTick;
			clip.resolvedTick = playback.resolvedTick;
			clip.durationTicks = deterministicPlayback.configuration.durationTicks;
			clip.loopCount = playback.loopCount;
			clip.speedBits = EncodeAnimationNetworkFloat(deterministicPlayback.configuration.speed);
			clip.speedQ16 = deterministicPlayback.speedQ16;
			clip.speedProductRemainder = deterministicPlayback.speedProductRemainder;
			clip.subTickRemainderQ16 = deterministicPlayback.subTickRemainderQ16;
			clip.runState = static_cast<AnimationRollbackPlaybackState>(deterministicPlayback.runState);
			clip.direction = deterministicPlayback.configuration.speed == 0.0F
								 ? AnimationRollbackPlaybackDirection::Stationary
								 : (deterministicPlayback.configuration.reverse ? AnimationRollbackPlaybackDirection::Reverse
																				: AnimationRollbackPlaybackDirection::Forward);
			clip.looping = deterministicPlayback.configuration.loop;
			source.clips.push_back(clip);

			if(record.graphState.activeTransitionStableId != 0U) {
				AnimationTransitionRollbackState transition;
				transition.transitionStableId = record.graphState.activeTransitionStableId;
				transition.sourceStateId = record.graphState.transitionSourceStateId;
				transition.destinationStateId = record.graphState.transitionDestinationStateId;
				transition.startTick = record.graphState.transitionStartTick;
				transition.durationTicks = record.graphState.transitionDurationTicks;
				transition.progressBits = EncodeAnimationNetworkFloat(record.graphState.transitionProgressNormalized);
				transition.destinationOffsetBits = EncodeAnimationNetworkFloat(record.graphState.retainedDestinationOffsetNormalized);
				transition.destinationEnteredTick = record.graphState.transitionDestinationEnteredTick;
				transition.interruptedTransitionStableId = record.graphState.interruptedTransitionStableId;
				transition.interruptedProgressBits = EncodeAnimationNetworkFloat(record.graphState.interruptedTransitionProgressNormalized);
				source.transitions.push_back(transition);
			}

			if(record.binding.evaluationProgram) {
				for(std::size_t layerIndex = 0U; layerIndex < record.binding.evaluationProgram->layers.size(); ++layerIndex) {
					const EvaluationLayer& value = record.binding.evaluationProgram->layers[layerIndex];
					AnimationLayerRollbackState layer;
					layer.layerStableId = value.stableId;
					layer.activeStateId = record.binding.stateId;
					layer.stateTick = record.graphState.stateEnteredTick;
					layer.weightBits = EncodeAnimationNetworkFloat(layerIndex < record.layerWeights.size() ? record.layerWeights[layerIndex]
																										   : value.defaultWeight);
					layer.additive = value.additive;
					source.layers.push_back(layer);
				}
			}

			source.parameters.reserve(record.parameters.size());
			for(const AnimationParameterSnapshot& value : record.parameters) {
				AnimationParameterRollbackState encoded;
				const AnimationStatus status = EncodeParameter(value, encoded);
				if(status != AnimationStatus::Ok)
					return status;
				source.parameters.push_back(encoded);
			}
			for(const StateMachineTriggerEdge& value : record.graphState.retainedTriggerEdges) {
				source.triggers.push_back({value.parameterId, value.sequenceId, value.raised, false, false});
			}

			std::vector<AnimationEventIdentity> deliveredEvents;
			record.eventStream.CopyRetainedEvents(deliveredEvents);
			for(std::size_t index = 0U; index < deliveredEvents.size(); ++index) {
				const AnimationEventIdentity& value = deliveredEvents[index];
				AnimationEventCursorRollbackState event;
				event.streamId = source.fingerprint.controllerAsset;
				event.stateInstanceId = value.stateInstanceId;
				event.clipAssetId = value.clipAssetId;
				event.eventId = value.eventId;
				event.sourceTick = value.sourceTick;
				event.loopCount = value.loopCount;
				event.deliveredSequence = index + 1U;
				event.deliveredThroughTick = source.tick;
				event.delivered = true;
				source.events.push_back(event);
			}
			source.eventWatermark = {source.fingerprint.controllerAsset, static_cast<std::uint64_t>(deliveredEvents.size()), source.tick};

			const PoseBuffer& pose = *published->snapshot->pose;
			if(const std::optional<AnimationRootMotionDelta>& delta = pose.RootMotionDelta()) {
				AnimationRootMotionRollbackState root;
				root.accumulationId = delta->generation;
				root.sourceBeginTick = delta->sourceBeginTick;
				root.sourceEndTick = delta->sourceEndTick;
				root.localTranslation = ToNetworkBits(delta->localTranslation);
				root.worldTranslation = ToNetworkBits(delta->worldTranslation);
				root.localYawBits = EncodeAnimationNetworkFloat(delta->localYawRadians);
				root.worldYawBits = EncodeAnimationNetworkFloat(delta->worldYawRadians);
				root.accumulatedCycles = delta->accumulatedCycles;
				root.sourceStateId = delta->sourceStateId;
				root.layerIndex = delta->layerIndex;
				root.generation = delta->generation;
				root.appliedThroughTick = delta->sourceEndTick;
				source.rootMotionStates.push_back(root);
				source.rootMotion.sourceTick = delta->sourceEndTick;
				source.rootMotion.translationAndYawBits = {
					EncodeAnimationNetworkFloat(delta->localTranslation.x), EncodeAnimationNetworkFloat(delta->localTranslation.y),
					EncodeAnimationNetworkFloat(delta->localTranslation.z), EncodeAnimationNetworkFloat(delta->localYawRadians)};
				source.rootMotion.requiresSceneTransaction = record.binding.rootMotionPolicy == AnimatorRootMotionPolicy::ApplyToObject;
				source.rootMotion.requiresPhysicsCommand = record.binding.rootMotionPolicy == AnimatorRootMotionPolicy::ApplyToCharacter;
			}

			std::uint32_t resultCount = 0U;
			const NativeAnimationAnimatorHandle nativeHandle{record.handle.slotIndex, record.handle.generation};
			AnimationStatus resultStatus = proceduralCommands->CopyResults(nativeHandle, {}, resultCount);
			if(resultStatus == AnimationStatus::Ok || resultStatus == AnimationStatus::BufferTooSmall) {
				std::vector<NativeAnimationProceduralResultSnapshot> results(resultCount);
				if(resultCount != 0U) {
					resultStatus = proceduralCommands->CopyResults(nativeHandle, results, resultCount);
				}
				if(resultStatus != AnimationStatus::Ok)
					return resultStatus;
				for(const NativeAnimationProceduralResultSnapshot& value : results) {
					AnimationIkRollbackState ik;
					ik.stableNodeId = value.goalId;
					ik.goalId = value.goalId;
					ik.targetStableId = value.targetJointStableId;
					ik.evaluatedTick = value.observationTick;
					ik.solverState = static_cast<std::uint32_t>(value.state) <=
											 static_cast<std::uint32_t>(NativeAnimationProceduralSolverState::NoContribution)
										 ? static_cast<AnimationRollbackIkState>(value.state)
										 : AnimationRollbackIkState::NoContribution;
					ik.weightBits = EncodeAnimationNetworkFloat(value.finalContribution);
					ik.errorBits = EncodeAnimationNetworkFloat(value.targetErrorMeters);
					ik.lockedContactTransform = ToNetworkBits(FromNativeTransform(value.resultTransform));
					ik.contactSurfaceNormal = ToNetworkBits(AnimationFloat3{});
					ik.contactContributionBits = EncodeAnimationNetworkFloat(0.0F);
					source.ikStates.push_back(ik);
				}
			}

			if(record.activeRetargetProfileGeneration != 0U && record.binding.compiledRetargetProfile &&
			   record.latestRetargetDeterministicHash != 0U) {
				AnimationRetargetRollbackState retarget;
				retarget.stableNodeId = 1U;
				retarget.profileFingerprint = record.binding.compiledRetargetProfile->stableHash;
				retarget.profileGeneration = record.activeRetargetProfileGeneration;
				retarget.sourceSkeletonGeneration = record.binding.retargetSourceSkeletonGeneration;
				retarget.targetSkeletonGeneration = record.binding.skeletonGeneration;
				retarget.sourcePoseGeneration = published->snapshot->publicationGeneration;
				retarget.targetPoseGeneration = published->snapshot->publicationGeneration;
				retarget.sourceTick = source.tick;
				retarget.normalizedTimeBits = EncodeAnimationNetworkFloat(playback.normalizedTime);
				retarget.deterministicHash = record.latestRetargetDeterministicHash;
				retarget.reducedFidelity = record.latestRetargetMetrics.fallbackCount != 0U;
				source.retargetStates.push_back(retarget);
			}

			if(record.hasMotionCandidate && record.binding.motionDatabase && record.latestMotionQueryId != 0U) {
				AnimationMotionMatchRollbackState motion;
				motion.stableNodeId = 1U;
				motion.databaseFingerprint = record.binding.motionDatabase->featureSchemaFingerprint;
				motion.databaseGeneration = record.activeMotionDatabaseGeneration;
				motion.querySequence = record.latestMotionQueryId;
				motion.candidateId = record.latestMotionCandidate.stableSampleId;
				motion.sourceClipFingerprint = record.latestMotionCandidate.sourceClipId;
				motion.sampleTick = record.latestMotionCandidate.sourceTick;
				const float score = static_cast<float>(std::min<std::uint64_t>(
					record.latestMotionCandidate.score.totalCost, static_cast<std::uint64_t>(std::numeric_limits<std::uint32_t>::max())));
				motion.totalScoreBits = EncodeAnimationNetworkFloat(score);
				std::uint64_t selectionHash = 1469598103934665603ULL;
				selectionHash = MixDeterministicChecksum(selectionHash, motion.candidateId);
				selectionHash = MixDeterministicChecksum(selectionHash, motion.sourceClipFingerprint);
				selectionHash = MixDeterministicChecksum(selectionHash, static_cast<std::uint64_t>(motion.sampleTick));
				motion.selectionHash = selectionHash;
				source.motionMatchStates.push_back(motion);
			}

			for(const AnimationNamedFloatValue& value : pose.MorphValues()) {
				source.morphStates.push_back(
					{value.stableId, value.stableId, EncodeAnimationNetworkFloat(value.value), published->snapshot->publicationGeneration});
			}

			if(record.ragdollGeneration.IsValid()) {
				const std::shared_ptr<const PhysicsPoseSnapshot> physicsPose =
					Physics::PhysicsSystem::Instance().LatestRagdollPhysicsPoseAtOrAfter(record.ragdollId, record.ragdollGeneration);
				if(physicsPose && ValidatePhysicsPoseSnapshot(*physicsPose, physicsPose->generation) == AnimationStatus::Ok) {
					for(const PhysicsPoseBodySnapshot& value : physicsPose->bodies) {
						AnimationRagdollRollbackState ragdoll;
						ragdoll.ragdollInstanceId = record.ragdollId;
						ragdoll.bodyId = value.bodyId;
						ragdoll.stableJointId = value.stableJointId;
						ragdoll.lifecycle = static_cast<AnimationRollbackRagdollState>(record.ragdollLifecycle);
						ragdoll.worldGeneration = physicsPose->generation.worldGeneration;
						ragdoll.profileGeneration = physicsPose->generation.profileGeneration;
						ragdoll.skeletonGeneration = physicsPose->generation.skeletonGeneration;
						ragdoll.physicsPublicationGeneration = physicsPose->generation.publicationGeneration;
						ragdoll.bodyGeneration = value.bodyGeneration;
						ragdoll.fixedStep = physicsPose->generation.fixedStep;
						ragdoll.modelTransform = ToNetworkBits(value.modelPose);
						ragdoll.linearVelocity = ToNetworkBits(value.linearVelocity);
						ragdoll.angularVelocity = ToNetworkBits(value.angularVelocity);
						ragdoll.blendWeightBits = EncodeAnimationNetworkFloat(record.ragdollBlendWeight);
						ragdoll.eventSequence = record.eventStream.Diagnostics().emittedEvents;
						ragdoll.sleeping = value.sleeping;
						source.ragdollStates.push_back(ragdoll);
					}
				}
			}
			if((record.ragdollLifecycle == RagdollLifecycleState::Creating || record.ragdollLifecycle == RagdollLifecycleState::Active ||
				record.ragdollLifecycle == RagdollLifecycleState::Recovering ||
				record.ragdollLifecycle == RagdollLifecycleState::TearingDown) &&
			   source.ragdollStates.empty()) {
				// Never capture an animation-only half of a cross-owner ragdoll state.
				// A later restore would otherwise appear successful while Jolt remained
				// at a different lifecycle or body generation.
				return AnimationStatus::NotReady;
			}

			const std::uint64_t poseHash = RuntimePoseChecksum(pose);
			const std::uint64_t stateHash = RuntimeStateChecksum(record);
			source.state.push_back({AnimationDeterministicStateKind::Physics,
									0U,
									0U,
									1U,
									{poseHash, stateHash, published->authorityPublicationGeneration, 0U}});
			return persistentReuse.rollbackSnapshotCodec.Capture(source, output);
		} catch(const std::bad_alloc&) {
			return AnimationStatus::HardLimitExceeded;
		}
	}

	std::uint64_t AnimationSystem::Impl::RuntimePoseChecksum(const AnimationRollbackSnapshot& snapshot) noexcept {
		const auto summary = std::find_if(snapshot.state.begin(), snapshot.state.end(), [](const auto& value) {
			return value.kind == AnimationDeterministicStateKind::Physics && value.stableId == 1U && value.ordinal == 0U;
		});
		return summary == snapshot.state.end() ? 0U : summary->values[0U];
	}

	std::uint64_t AnimationSystem::Impl::RuntimeStateChecksum(const AnimationRollbackSnapshot& snapshot) noexcept {
		const auto summary = std::find_if(snapshot.state.begin(), snapshot.state.end(), [](const auto& value) {
			return value.kind == AnimationDeterministicStateKind::Physics && value.stableId == 1U && value.ordinal == 0U;
		});
		return summary == snapshot.state.end() ? 0U : summary->values[1U];
	}

	AnimationStatus AnimationSystem::Impl::RestoreRollbackSnapshotLocked(RuntimeRecord& record,
																		 const AnimationRollbackSnapshot& snapshot) noexcept {
		if(snapshot.controllers.size() != 1U || snapshot.clips.size() != 1U || !snapshot.ikStates.empty() ||
		   !snapshot.ragdollStates.empty() || snapshot.rootMotion.requiresSceneTransaction || snapshot.rootMotion.requiresPhysicsCommand) {
			// Procedural goals and active physics state require their owning command
			// boundaries. Until a complete cross-owner transaction is available, reject
			// atomically instead of partially restoring only animation-owned fields.
			return AnimationStatus::Unsupported;
		}
		if(record.historyGeneration == std::numeric_limits<AnimationGeneration>::max() ||
		   record.stateGeneration == std::numeric_limits<std::uint64_t>::max() ||
		   record.parameterGeneration == std::numeric_limits<std::uint64_t>::max() ||
		   record.executionModeGeneration == std::numeric_limits<std::uint64_t>::max()) {
			return AnimationStatus::RangeOverflow;
		}

		try {
			AnimatorPlaybackBinding candidateBinding = record.binding;
			const AnimationClipRollbackState& clipState = snapshot.clips.front();
			auto selectClip = [&](const std::shared_ptr<const AnimationClipAsset>& clip, AnimationGeneration generation,
								  std::uint64_t fingerprint, const std::vector<std::uint8_t>& residentSegments,
								  const std::shared_ptr<const AnimationClipSegmentPayloadSet>& streamedSegmentPayloads) {
				if(!clip || clip->assetId != clipState.clipAssetId || generation != clipState.clipGeneration ||
				   fingerprint != clipState.clipContentFingerprint) {
					return false;
				}
				candidateBinding.clip = clip;
				candidateBinding.clipGeneration = generation;
				candidateBinding.clipDeterministicContentFingerprint = fingerprint;
				candidateBinding.residentSegments = residentSegments;
				candidateBinding.streamedSegmentPayloads = streamedSegmentPayloads;
				return true;
			};
			bool clipSelected =
				selectClip(record.binding.clip, record.binding.clipGeneration, record.binding.clipDeterministicContentFingerprint,
						   record.binding.residentSegments, record.binding.streamedSegmentPayloads);
			if(!clipSelected) {
				for(const AnimatorControllerClipBinding& candidate : record.binding.controllerClips) {
					if(selectClip(candidate.clip, candidate.clipGeneration, candidate.deterministicContentFingerprint,
								  candidate.residentSegments, candidate.streamedSegmentPayloads)) {
						clipSelected = true;
						break;
					}
				}
			}
			if(!clipSelected || !candidateBinding.clip) {
				return AnimationStatus::IncompatibleAsset;
			}

			const AnimationControllerRollbackState& controller = snapshot.controllers.front();
			candidateBinding.stateId = controller.activeStateId;
			StateMachineSnapshot candidateGraph;
			candidateGraph.activeStateId = controller.activeStateId;
			candidateGraph.stateEnteredTick = controller.stateEnteredTick;
			candidateGraph.lastTick = controller.lastEvaluatedTick;
			if(snapshot.transitions.size() > 1U)
				return AnimationStatus::InvalidArgument;
			if(!snapshot.transitions.empty()) {
				const AnimationTransitionRollbackState& transition = snapshot.transitions.front();
				candidateGraph.activeTransitionStableId = transition.transitionStableId;
				candidateGraph.transitionSourceStateId = transition.sourceStateId;
				candidateGraph.transitionDestinationStateId = transition.destinationStateId;
				candidateGraph.transitionStartTick = transition.startTick;
				candidateGraph.transitionDurationTicks = transition.durationTicks;
				candidateGraph.transitionProgressNormalized = DecodeAnimationNetworkFloat(transition.progressBits);
				candidateGraph.retainedDestinationOffsetNormalized = DecodeAnimationNetworkFloat(transition.destinationOffsetBits);
				candidateGraph.transitionDestinationEnteredTick = transition.destinationEnteredTick;
				candidateGraph.interruptedTransitionStableId = transition.interruptedTransitionStableId;
				candidateGraph.interruptedTransitionProgressNormalized = DecodeAnimationNetworkFloat(transition.interruptedProgressBits);
			}
			for(const AnimationTriggerRollbackState& trigger : snapshot.triggers) {
				candidateGraph.retainedTriggerEdges.push_back({trigger.parameterId, trigger.sequenceId, trigger.raised});
			}
			if(candidateBinding.evaluationProgram &&
			   ValidateStateMachineSnapshot(*candidateBinding.evaluationProgram, candidateGraph) != AnimationStatus::Ok) {
				return AnimationStatus::IncompatibleAsset;
			}

			AnimatorPlaybackDeterministicState playbackState;
			playbackState.configuration = record.playback.Configuration();
			playbackState.configuration.durationTicks = clipState.durationTicks;
			playbackState.configuration.loop = clipState.looping;
			playbackState.configuration.reverse = clipState.direction == AnimationRollbackPlaybackDirection::Reverse;
			playbackState.configuration.speed = DecodeAnimationNetworkFloat(clipState.speedBits);
			playbackState.runState = static_cast<AnimatorPlaybackRunState>(clipState.runState);
			playbackState.absoluteTick = clipState.absoluteTick;
			playbackState.speedQ16 = clipState.speedQ16;
			playbackState.speedProductRemainder = clipState.speedProductRemainder;
			playbackState.subTickRemainderQ16 = clipState.subTickRemainderQ16;
			playbackState.latestStatus = AnimationStatus::Ok;
			playbackState.configured = true;
			AnimatorPlayback candidatePlayback;
			const AnimationStatus playbackStatus = candidatePlayback.RestoreDeterministicState(playbackState);
			if(playbackStatus != AnimationStatus::Ok)
				return playbackStatus;

			std::vector<AnimationParameterSnapshot> candidateParameters;
			candidateParameters.reserve(snapshot.parameters.size());
			for(const AnimationParameterRollbackState& value : snapshot.parameters) {
				AnimationParameterSnapshot decoded;
				const AnimationStatus status = DecodeParameter(value, decoded);
				if(status != AnimationStatus::Ok)
					return status;
				candidateParameters.push_back(std::move(decoded));
			}
			std::vector<float> candidateLayerWeights;
			if(candidateBinding.evaluationProgram) {
				if(snapshot.layers.size() != candidateBinding.evaluationProgram->layers.size()) {
					return AnimationStatus::IncompatibleAsset;
				}
				candidateLayerWeights.reserve(snapshot.layers.size());
				for(std::size_t layerIndex = 0U; layerIndex < snapshot.layers.size(); ++layerIndex) {
					const AnimationLayerRollbackState& retained = snapshot.layers[layerIndex];
					const EvaluationLayer& authored = candidateBinding.evaluationProgram->layers[layerIndex];
					const float weight = DecodeAnimationNetworkFloat(retained.weightBits);
					if(retained.layerStableId != authored.stableId || !std::isfinite(weight) || weight < 0.0F || weight > 1.0F) {
						return AnimationStatus::IncompatibleAsset;
					}
					candidateLayerWeights.push_back(weight);
				}
			} else if(!snapshot.layers.empty()) {
				return AnimationStatus::IncompatibleAsset;
			}

			AnimationEventStream candidateEventStream;
			const AnimationStatus eventConfigure =
				candidateEventStream.Configure({candidateBinding.clip->assetId, candidateBinding.clipGeneration},
											   candidateBinding.clip->durationTicks, candidateBinding.clip->events);
			if(eventConfigure != AnimationStatus::Ok)
				return eventConfigure;
			std::vector<AnimationEventIdentity> deliveredEvents;
			deliveredEvents.reserve(snapshot.events.size());
			for(const AnimationEventCursorRollbackState& value : snapshot.events) {
				if(!value.delivered)
					continue;
				deliveredEvents.push_back(
					{record.handle, value.stateInstanceId, value.clipAssetId, value.eventId, value.sourceTick, value.loopCount});
			}
			const AnimationStatus eventRestore = candidateEventStream.RestoreDeliveryHistory(deliveredEvents);
			if(eventRestore != AnimationStatus::Ok)
				return eventRestore;

			NativeAnimationUuid candidateRetargetId{};
			AnimationGeneration candidateRetargetGeneration = 0U;
			std::uint64_t candidateRetargetHash = 0U;
			if(snapshot.retargetStates.size() > 1U)
				return AnimationStatus::InvalidArgument;
			if(!snapshot.retargetStates.empty()) {
				const AnimationRetargetRollbackState& value = snapshot.retargetStates.front();
				if(!candidateBinding.retargetProfile || !candidateBinding.compiledRetargetProfile ||
				   value.profileGeneration != candidateBinding.retargetProfileGeneration ||
				   value.profileFingerprint != candidateBinding.compiledRetargetProfile->stableHash) {
					return AnimationStatus::IncompatibleAsset;
				}
				candidateRetargetId = ToNativeUuid(candidateBinding.retargetProfile->assetId.value);
				candidateRetargetGeneration = value.profileGeneration;
				candidateRetargetHash = value.deterministicHash;
			}

			NativeAnimationUuid candidateMotionId{};
			AnimationGeneration candidateMotionGeneration = 0U;
			std::uint64_t candidateMotionQueryId = 0U;
			MotionMatchCandidate candidateMotion;
			bool candidateHasMotion = false;
			if(snapshot.motionMatchStates.size() > 1U)
				return AnimationStatus::InvalidArgument;
			if(!snapshot.motionMatchStates.empty()) {
				const AnimationMotionMatchRollbackState& value = snapshot.motionMatchStates.front();
				if(!candidateBinding.motionDatabase || value.databaseGeneration != candidateBinding.motionDatabaseGeneration ||
				   value.databaseFingerprint != candidateBinding.motionDatabase->featureSchemaFingerprint) {
					return AnimationStatus::IncompatibleAsset;
				}
				candidateMotionId = ToNativeUuid(candidateBinding.motionDatabase->assetId.value);
				candidateMotionGeneration = value.databaseGeneration;
				candidateMotionQueryId = value.querySequence;
				candidateMotion.stableSampleId = value.candidateId;
				candidateMotion.sourceClipId = value.sourceClipFingerprint;
				candidateMotion.sourceTick = value.sampleTick;
				const float score = DecodeAnimationNetworkFloat(value.totalScoreBits);
				candidateMotion.score.totalCost = score <= 0.0F ? 0U : static_cast<std::uint64_t>(score);
				candidateHasMotion = !value.usedFallback && value.candidateId != 0U;
			}

			// Every allocation and compatibility check above completed before this
			// protected all-or-none replacement of animation-owned live state.
			record.binding = std::move(candidateBinding);
			record.playback = candidatePlayback;
			record.eventStream = std::move(candidateEventStream);
			record.graphState = std::move(candidateGraph);
			record.parameters = std::move(candidateParameters);
			record.layerWeights = std::move(candidateLayerWeights);
			record.activeRetargetProfileId = candidateRetargetId;
			record.activeRetargetProfileGeneration = candidateRetargetGeneration;
			record.latestRetargetDeterministicHash = candidateRetargetHash;
			record.activeMotionDatabaseId = candidateMotionId;
			record.activeMotionDatabaseGeneration = candidateMotionGeneration;
			record.latestMotionQueryId = candidateMotionQueryId;
			record.appliedMotionQueryId = candidateMotionQueryId;
			record.latestMotionCandidate = candidateMotion;
			record.hasMotionCandidate = candidateHasMotion;
			++record.historyGeneration;
			++record.stateGeneration;
			++record.parameterGeneration;
			++record.executionModeGeneration;
			persistentReuse.rollbackHistory.Clear(record.handle);
			(void)persistentReuse.evaluationCache.InvalidateAnimator(record.handle);
			return AnimationStatus::Ok;
		} catch(const std::bad_alloc&) {
			return AnimationStatus::HardLimitExceeded;
		}
	}

	void AnimationSystem::Impl::PrepareDeterminismRequestsLocked() noexcept {
		frameInputs.determinismRequestCompletions.clear();
		try {
			frameInputs.determinismRequestCompletions.reserve(frameInputs.capturedDeterminismRequests.size());
			for(const NativeAnimationDeterminismRequest& request : frameInputs.capturedDeterminismRequests) {
				DeterminismRequestCompletion completion;
				completion.request = request;
				RuntimeRecord* record = FindRuntimeRecordLocked({request.animator.slotIndex, request.animator.generation});
				if(record == nullptr || !instances->IsAlive(record->handle)) {
					completion.status = AnimationStatus::StaleHandle;
					frameInputs.determinismRequestCompletions.push_back(std::move(completion));
					continue;
				}
				// From this protected boundary onward, retain every published tick for
				// the requesting animator. Restore and replay can therefore address a
				// continuous ring without charging unrelated crowd members.
				record->rollbackHistoryActive = true;
				const std::shared_ptr<const AnimationAuthorityFrame> authority = authorityPublisher.Load();
				const AnimationAuthorityRow* published = authority ? authority->Find(record->handle) : nullptr;
				if(!published || !published->snapshot || !published->snapshot->pose) {
					completion.status = AnimationStatus::NotReady;
					frameInputs.determinismRequestCompletions.push_back(std::move(completion));
					continue;
				}
				if(request.expectedPublicationGeneration != 0U &&
				   request.expectedPublicationGeneration != published->authorityPublicationGeneration) {
					completion.status = AnimationStatus::StaleHandle;
					frameInputs.determinismRequestCompletions.push_back(std::move(completion));
					continue;
				}
				if(request.operation == NativeAnimationDeterminismOperation::Capture ||
				   request.operation == NativeAnimationDeterminismOperation::RequestReplication) {
					// Capture reports the pose committed later in this same frame.
					completion.status = AnimationStatus::NotReady;
					frameInputs.determinismRequestCompletions.push_back(std::move(completion));
					continue;
				}

				const AnimationRollbackRestoreRequirements requirements{record->handle, BuildDeterministicFingerprint(*record),
																		record->contentGeneration,
																		BuildRollbackGenerations(*record, *published)};
				AnimationStatus lookupStatus = AnimationStatus::InvalidArgument;
				if(request.operation == NativeAnimationDeterminismOperation::Restore) {
					lookupStatus = persistentReuse.rollbackHistory
									   .LookupDetailed(record->handle, request.sourceTick, requirements, completion.sourceSnapshot)
									   .status;
				} else if(request.operation == NativeAnimationDeterminismOperation::ReplaySeek) {
					AnimationReplayRequest replay;
					replay.mode = AnimationReplayMode::ReplaySeek;
					replay.animator = record->handle;
					replay.seekTick = request.targetTick;
					replay.requirements = requirements;
					replay.eventPolicy = (request.flags & 1U) != 0U ? AnimationReplayEventPolicy::ReplayHistorical
																	: AnimationReplayEventPolicy::SuppressHistorical;
					// Production keeps seek publication atomic by requiring the exact
					// retained tick. The standalone replay helper remains available for
					// host-owned bounded catch-up/resimulation.
					replay.requireExactTick = true;
					const AnimationReplayResult replayResult = ResolveAnimationReplay(persistentReuse.rollbackHistory, replay);
					lookupStatus = replayResult.status;
					if(lookupStatus == AnimationStatus::Ok) {
						completion.sourceSnapshot = replayResult.resumeSnapshot;
					}
				} else {
					lookupStatus = AnimationStatus::InvalidArgument;
				}

				if(lookupStatus == AnimationStatus::Ok && request.expectedChecksum != 0U &&
				   request.expectedChecksum != RuntimePoseChecksum(completion.sourceSnapshot)) {
					lookupStatus = AnimationStatus::DeterminismViolation;
				}
				completion.status =
					lookupStatus == AnimationStatus::Ok ? RestoreRollbackSnapshotLocked(*record, completion.sourceSnapshot) : lookupStatus;
				completion.stateRestored = completion.status == AnimationStatus::Ok;
				frameInputs.determinismRequestCompletions.push_back(std::move(completion));
			}
		} catch(const std::bad_alloc&) {
			// A bounded history allocation failure is reported per request and leaves
			// every live runtime record in its prior complete state.
			for(const NativeAnimationDeterminismRequest& request : frameInputs.capturedDeterminismRequests) {
				const auto found = std::find_if(
					frameInputs.determinismRequestCompletions.begin(), frameInputs.determinismRequestCompletions.end(),
					[&request](const DeterminismRequestCompletion& value) { return value.request.correlationId == request.correlationId; });
				if(found == frameInputs.determinismRequestCompletions.end()) {
					frameInputs.determinismRequestCompletions.push_back({request, AnimationStatus::HardLimitExceeded, {}, false});
				}
			}
		}
	}

	void AnimationSystem::Impl::CaptureRollbackHistoryLocked(std::span<const AnimatorInstanceHandle> rollbackHistoryAnimators) noexcept {
		const std::shared_ptr<RuntimeRecordRoot> root = runtimeRecords.load(std::memory_order_acquire);
		if(root == nullptr)
			return;
		for(const AnimatorInstanceHandle handle : rollbackHistoryAnimators) {
			if(!handle.IsValid() || handle.slotIndex >= root->size())
				continue;
			const std::shared_ptr<RuntimeRecord>& owned = root->at(handle.slotIndex);
			if(!owned || owned->handle != handle || !owned->rollbackHistoryActive || !instances->IsAlive(handle))
				continue;
			const std::shared_ptr<const AnimationAuthorityFrame> authority = authorityPublisher.Load();
			const AnimationAuthorityRow* published = authority ? authority->Find(owned->handle) : nullptr;
			AnimationRollbackSnapshot snapshot;
			AnimationStatus status = BuildRollbackSnapshotLocked(*owned, published, snapshot);
			if(status == AnimationStatus::Ok) {
				status = persistentReuse.rollbackHistory.Store(std::move(snapshot));
			}

			// Capture and replication requests become terminal only after the same
			// immutable value has entered the bounded history ring. This prevents a
			// successful receipt from naming state that cannot subsequently restore.
			for(DeterminismRequestCompletion& completion : frameInputs.determinismRequestCompletions) {
				if(completion.request.animator.slotIndex != owned->handle.slotIndex ||
				   completion.request.animator.generation != owned->handle.generation || completion.status != AnimationStatus::NotReady ||
				   (completion.request.operation != NativeAnimationDeterminismOperation::Capture &&
					completion.request.operation != NativeAnimationDeterminismOperation::RequestReplication)) {
					continue;
				}
				completion.status =
					completion.request.expectedPublicationGeneration != 0U &&
							(!published || completion.request.expectedPublicationGeneration != published->authorityPublicationGeneration)
						? AnimationStatus::StaleHandle
						: status;
				if(completion.status == AnimationStatus::Ok && published) {
					const AnimationRollbackRestoreRequirements requirements{owned->handle, BuildDeterministicFingerprint(*owned),
																			owned->contentGeneration,
																			BuildRollbackGenerations(*owned, *published)};
					completion.status =
						persistentReuse.rollbackHistory
							.LookupDetailed(owned->handle, published->snapshot->tick, requirements, completion.sourceSnapshot)
							.status;
				}
			}
		}
	}

	void AnimationSystem::Impl::AssertCoordinatorThread() const {
		// SystemScheduler invokes DeclareFrameWorkloads on its protected
		// coordinator before any worker job is launched. The one membership load
		// and dormant diagnostics publication rely on that invariant.
		assert(coordinatorThread == std::this_thread::get_id() &&
			   "Animation workload declarations must be bound on the coordinator thread.");
	}

	AnimatorInstanceHandle AnimationSystem::CreateAnimatorInstance(AnimatorInstanceDescriptor descriptor) {
		impl_->AssertCoordinatorThread();
		std::scoped_lock lock(impl_->runtimeOwnershipMutex);
		if(!impl_->runtimeActive.load(std::memory_order_acquire))
			return {};
		const AnimatorInstanceHandle handle = impl_->instances->Create(descriptor);
		if(!handle.IsValid())
			return {};
		try {
			const std::shared_ptr<Impl::RuntimeRecordRoot> currentRoot = impl_->runtimeRecords.load(std::memory_order_acquire);
			auto nextRoot = std::make_shared<Impl::RuntimeRecordRoot>(currentRoot ? *currentRoot : Impl::RuntimeRecordRoot{});
			if(nextRoot->size() <= handle.slotIndex) {
				nextRoot->resize(static_cast<std::size_t>(handle.slotIndex) + 1U);
			}
			auto record = std::make_shared<Impl::RuntimeRecord>();
			record->handle = handle;
			record->descriptor = descriptor;
			// The copied ragdoll identity is generation-safe and never aliases a
			// reused animator slot. Zero remains reserved as an invalid physics key.
			record->ragdollId =
				(static_cast<std::uint64_t>(handle.generation) << 32U) | (static_cast<std::uint64_t>(handle.slotIndex) + 1U);
			(void)record->playback.Configure({});
			nextRoot->Set(handle.slotIndex, std::move(record));
			impl_->runtimeRecords.store(std::move(nextRoot), std::memory_order_release);
			impl_->frameDeclarationEpoch.fetch_add(1U, std::memory_order_acq_rel);
		} catch(const std::bad_alloc&) {
			(void)impl_->instances->Invalidate(handle);
			return {};
		}
		return handle;
	}

	bool AnimationSystem::InvalidateAnimatorInstance(AnimatorInstanceHandle handle) {
		const std::array<AnimatorInstanceHandle, 1U> handles{handle};
		return InvalidateAnimatorInstances(handles) == 1U;
	}

	std::size_t AnimationSystem::InvalidateAnimatorInstances(std::span<const AnimatorInstanceHandle> handles) {
		impl_->AssertCoordinatorThread();
		if(handles.empty())
			return 0U;
		std::scoped_lock lock(impl_->runtimeOwnershipMutex);
		if(!impl_->runtimeActive.load(std::memory_order_acquire))
			return 0U;

		// Stage every allocation before mutating the instance store. A failed reserve
		// or copy therefore leaves both handle liveness and the published root intact.
		std::vector<AnimatorInstanceHandle> retired;
		std::shared_ptr<Impl::RuntimeRecordRoot> nextRoot;
		try {
			retired.reserve(handles.size());
			for(const AnimatorInstanceHandle handle : handles) {
				if(handle.IsValid() && impl_->instances->IsAlive(handle)) {
					retired.push_back(handle);
				}
			}
			std::sort(retired.begin(), retired.end());
			retired.erase(std::unique(retired.begin(), retired.end()), retired.end());
			if(retired.empty())
				return 0U;

			const std::shared_ptr<Impl::RuntimeRecordRoot> currentRoot = impl_->runtimeRecords.load(std::memory_order_acquire);
			nextRoot = currentRoot ? std::make_shared<Impl::RuntimeRecordRoot>(*currentRoot) : std::shared_ptr<Impl::RuntimeRecordRoot>{};
			// Stage the retired root and every physical Teardown request before making
			// any handle stale. A full bridge queue or failed allocation leaves the old
			// runtime, stream ownership, and external character lease untouched so the
			// component coordinator can retry at the next protected boundary.
			std::vector<RagdollPhysicsRequest> externalTeardowns;
			externalTeardowns.reserve(retired.size());
			const auto bindings = impl_->rootControllerBindings.load(std::memory_order_acquire);
			for(const AnimatorInstanceHandle handle : retired) {
				if(nextRoot && handle.slotIndex < nextRoot->size()) {
					nextRoot->Set(handle.slotIndex, {});
				}
				const std::uint64_t externalOwnerId = RagdollExternalOwnerId(handle);
				const auto external = Physics::PhysicsSystem::Instance().ReadExternalCharacterMotionOwner(externalOwnerId);
				if(!external)
					continue;
				const Impl::RuntimeRecord* record = impl_->FindRuntimeRecordLocked(handle);
				if(!record || record->ragdollId == 0U)
					return 0U;
				Physics::CharacterExternalRetirementIntent intent;
				intent.expectedExternalLease = *external;
				intent.externalOwnerId = externalOwnerId;
				// Locomotion binds the primary frame. A still-valid copied row can
				// supply its exact frame/generation; a removed AnimatorComponent may
				// already stale that guard, so physical owner identity is authoritative.
				intent.frame = {Physics::kPrimaryPhysicsFrameId, external->controller.world.worldGeneration};
				if(bindings) {
					const auto found = std::lower_bound(
						bindings->active.begin(), bindings->active.end(), handle,
						[](const AnimationRootControllerBinding& row, AnimatorInstanceHandle key) { return row.animator < key; });
					if(found != bindings->active.end() && found->animator == handle && found->controller == external->controller &&
					   found->bindingValidityGuard &&
					   found->bindingValidityGuard->Validate() == Physics::CharacterMotionValidityReason::Valid) {
						intent.frame = found->frame;
						intent.bindingGeneration = found->bindingGeneration;
					}
				}
				RagdollPhysicsRequest teardown;
				teardown.kind = RagdollPhysicsRequestKind::Teardown;
				teardown.ragdollId = record->ragdollId;
				teardown.generation = record->ragdollGeneration;
				teardown.expectedCurrentGeneration = record->ragdollGeneration;
				teardown.correlationId = record->ragdollId ^ 0xA11AFFFF00000000ULL;
				if(teardown.correlationId == 0U)
					teardown.correlationId = 1U;
				teardown.producerOrder = std::numeric_limits<std::uint64_t>::max();
				teardown.retirementIntent = std::make_shared<const Physics::CharacterExternalRetirementIntent>(intent);
				externalTeardowns.push_back(std::move(teardown));
			}
			if(!externalTeardowns.empty() &&
			   Physics::PhysicsSystem::Instance().SubmitRagdollPhysicsRequestBatch(externalTeardowns) != AnimationStatus::Queued)
				return 0U;
		} catch(const std::bad_alloc&) {
			return 0U;
		}
		for(const AnimatorInstanceHandle handle : retired) {
			// Runtime ownership is held for the complete batch, so a handle proven live
			// above cannot be replaced between validation and this generation advance.
			(void)impl_->instances->Invalidate(handle);
		}

		(void)impl_->InvalidateLifecycleLocked(AnimationLifecycleBoundary::Component);
		(void)AnimationPresentationSystem::Instance().NotifyLifecycleInvalidation(AnimationLifecycleBoundary::Component);
		(void)impl_->persistentReuse.evaluationCache.InvalidateAnimators(retired);
		impl_->persistentReuse.rollbackHistory.ClearMany(retired);
		if(impl_->proceduralCommands != nullptr) {
			for(const AnimatorInstanceHandle handle : retired) {
				impl_->proceduralCommands->RetireAnimator({handle.slotIndex, handle.generation});
			}
		}
		if(impl_->advancedCommands != nullptr) {
			impl_->advancedCommands->RetireAnimators(retired);
		}
		if(nextRoot) {
			impl_->runtimeRecords.store(std::move(nextRoot), std::memory_order_release);
		}
		AnimationAttachmentSnapshotPublisher::Instance().RetireMany(retired);
		std::erase_if(impl_->pendingRootMotionApplications, [&retired](const AnimationRootMotionApplicationRequest& value) {
			return std::binary_search(retired.begin(), retired.end(), value.delta.animator);
		});
		AnimationRootMotionResultPublisher::Instance().RetireMany(retired);
		impl_->frameDeclarationEpoch.fetch_add(1U, std::memory_order_acq_rel);
		return retired.size();
	}

	void AnimationSystem::SetAuthoredComponentPlaybackEnabled(bool enabled) {
		impl_->AssertCoordinatorThread();
		if(impl_->authoredComponentPlaybackEnabled == enabled)
			return;

		// Invalidate every component-owned instance before changing domains. The
		// next protected synchronization either rebuilds runtime bindings or leaves
		// them absent in the edit domain, retiring the last GPU/runtime pose.
		impl_->componentBindings.InvalidateAll(*this);
		impl_->authoredComponentPlaybackEnabled = enabled;
		impl_->frameDeclarationEpoch.fetch_add(1U, std::memory_order_acq_rel);
	}

	bool AnimationSystem::IsAuthoredComponentPlaybackEnabled() const noexcept {
		return impl_->authoredComponentPlaybackEnabled;
	}

	void AnimationSystem::SetHeadlessExecutionRequired(bool required) {
		impl_->AssertCoordinatorThread();
		if(impl_->headlessExecutionRequired == required)
			return;

		// This configuration is changed only before system initialization or at a
		// protected lifecycle boundary. Existing component-owned instances are
		// invalidated so their persisted hysteresis state cannot keep a GPU-capable
		// mode after the renderer has been removed.
		impl_->componentBindings.InvalidateAll(*this);
		impl_->headlessExecutionRequired = required;
		impl_->frameDeclarationEpoch.fetch_add(1U, std::memory_order_acq_rel);
	}

	bool AnimationSystem::IsHeadlessExecutionRequired() const noexcept {
		return impl_->headlessExecutionRequired;
	}

	void AnimationSystem::SetAuthoredComponentPlaybackAdvancing(bool advancing) {
		impl_->AssertCoordinatorThread();
		if(impl_->authoredComponentPlaybackAdvancing == advancing)
			return;

		// Restart each retained binding at the next protected capture boundary.
		// Keeping its handle and compiled controller prevents Edit/Play transitions
		// from exposing an asynchronous NotReady/reference-pose flash. The already
		// published complete pose stays visible until that replacement frame lands.
		impl_->componentBindings.RequestPlaybackRestart();
		impl_->authoredComponentPlaybackAdvancing = advancing;
		impl_->frameDeclarationEpoch.fetch_add(1U, std::memory_order_acq_rel);
	}

	bool AnimationSystem::IsAuthoredComponentPlaybackAdvancing() const noexcept {
		return impl_->authoredComponentPlaybackAdvancing;
	}

	AnimationStatus AnimationSystem::NotifyLifecycleInvalidation(AnimationLifecycleBoundary boundary) {
		impl_->AssertCoordinatorThread();
		AnimationStatus status = AnimationStatus::Cancelled;
		{
			std::scoped_lock lock(impl_->runtimeOwnershipMutex);
			if(!impl_->runtimeActive.load(std::memory_order_acquire)) {
				return AnimationStatus::Cancelled;
			}
			status = impl_->InvalidateLifecycleLocked(boundary);
			if(status == AnimationStatus::Ok || status == AnimationStatus::Cancelled) {
				// Unlike a recoverable evaluation failure, a lifecycle change retires the
				// whole authority envelope.  This makes an old scene, Play session, or
				// asset world unobservable to fresh scripting and rendering consumers.
				impl_->RetireLifecyclePublicationStateLocked();
				impl_->persistentReuse.evaluationCache.InvalidateAll();
				if(boundary != AnimationLifecycleBoundary::Renderer && boundary != AnimationLifecycleBoundary::Device) {
					// World, component, asset, physics, play-domain, and shutdown changes
					// invalidate at least one generation captured by rollback state. Retain
					// visual-only history across renderer/device recovery because its CPU
					// authority and compatibility identities remain unchanged.
					impl_->persistentReuse.rollbackHistory.ClearAll();
				}
				// Scene/domain/play/asset/physics changes invalidate captured world or
				// program inputs owned by procedural goals. Renderer/device recovery is
				// visual-only and therefore preserves CPU-authoritative gameplay goals.
				if(impl_->proceduralCommands != nullptr && boundary != AnimationLifecycleBoundary::Renderer &&
				   boundary != AnimationLifecycleBoundary::Device) {
					impl_->proceduralCommands->Clear();
				}
				if(impl_->advancedCommands != nullptr && boundary != AnimationLifecycleBoundary::Renderer &&
				   boundary != AnimationLifecycleBoundary::Device) {
					impl_->advancedCommands->Clear();
				}
			}
		}
		if(status == AnimationStatus::Ok || status == AnimationStatus::Cancelled) {
			// These sidecars borrow the authority session.  Clear them after releasing
			// runtime ownership to preserve their independent publisher lock ordering.
			AnimationAttachmentSnapshotPublisher::Instance().Clear();
			AnimationRootMotionResultPublisher::Instance().Clear();
			ClearPublishedAnimationDiagnostics();
			ClearPublishedAnimationDetailedDiagnostics();
			(void)AnimationPresentationSystem::Instance().NotifyLifecycleInvalidation(boundary);
		}
		return status;
	}

	bool AnimationSystem::IsAnimatorInstanceAlive(AnimatorInstanceHandle handle) const {
		std::scoped_lock lock(impl_->runtimeOwnershipMutex);
		return impl_->instances->IsAlive(handle);
	}

	std::uint64_t AnimationSystem::AnimatorInstanceMembershipRevision() const noexcept {
		// Store lifetime is protected by AnimationSystem and survives ordinary play
		// sessions, so reading its monotonic atomic revision needs no ownership lock.
		return impl_->instances ? impl_->instances->MembershipRevision() : 0U;
	}
} // namespace TerraEngine::Animation
