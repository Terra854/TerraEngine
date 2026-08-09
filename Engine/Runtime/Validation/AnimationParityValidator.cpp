#include "Runtime/Validation/AnimationParityValidator.h"

#include "Animation/Runtime/PoseBuffer.h"
#include "Utilities/UUID.h"

#include <cmath>
#include <cstring>
#include <iomanip>
#include <sstream>
#include <string_view>
#include <type_traits>
#include <variant>

namespace TerraEngine::Runtime {
namespace {
	class CanonicalHash final {
	public:
		void Add(std::uint64_t value) noexcept {
			for (std::size_t i = 0U; i < sizeof(value); ++i) {
				hash_ ^= static_cast<std::uint8_t>(value >> (i * 8U));
				hash_ *= 1099511628211ULL;
			}
		}
		void Add(float value) noexcept {
			std::uint32_t bits = 0U;
			std::memcpy(&bits, &value, sizeof(bits));
			Add(static_cast<std::uint64_t>(bits));
		}
		[[nodiscard]] std::string Finish() const {
			std::ostringstream stream;
			stream << std::hex << std::setfill('0') << std::setw(16) << hash_;
			return stream.str();
		}
	private:
		std::uint64_t hash_ = 1469598103934665603ULL;
	};

	void AddGuid(CanonicalHash& hash, const Animation::AnimationGuid& guid) {
		for (const auto byte : guid.value) hash.Add(static_cast<std::uint64_t>(byte));
	}
	void AddHandle(CanonicalHash& hash, const Animation::AnimationAssetHandle& handle) {
		// Generations are deliberately omitted: they are publication-local, not authored identity.
		AddGuid(hash, handle.assetId);
	}
	void AddPlayback(CanonicalHash& hash, const Animation::AnimationPlaybackSnapshot& playback) {
		hash.Add(playback.currentStateId); hash.Add(playback.previousStateId);
		hash.Add(playback.transitionProgress); hash.Add(playback.normalizedTime);
		hash.Add(static_cast<std::uint64_t>(playback.loopCount)); hash.Add(playback.effectiveSpeed);
		hash.Add(static_cast<std::uint64_t>(playback.latestStatus));
		hash.Add(static_cast<std::uint64_t>(playback.activeLayers.size()));
		for (const auto& layer : playback.activeLayers) {
			hash.Add(static_cast<std::uint64_t>(layer.layerIndex)); hash.Add(layer.stateId); hash.Add(layer.weight);
			hash.Add(static_cast<std::uint64_t>(layer.stateTick));
		}
		hash.Add(static_cast<std::uint64_t>(playback.parameters.size()));
		for (const auto& parameter : playback.parameters) {
			hash.Add(static_cast<std::uint64_t>(parameter.parameterId)); hash.Add(static_cast<std::uint64_t>(parameter.type));
			// The variant tag separates values such as Boolean false and Integer
			// zero even if malformed input paired them with the same authored type.
			hash.Add(static_cast<std::uint64_t>(parameter.value.index()));
			std::visit([&](const auto& value) {
				using T = std::decay_t<decltype(value)>;
				if constexpr (std::is_same_v<T, std::monostate>) hash.Add(std::uint64_t{0});
				else if constexpr (std::is_same_v<T, bool>) hash.Add(static_cast<std::uint64_t>(value));
				else if constexpr (std::is_integral_v<T>) hash.Add(static_cast<std::uint64_t>(value));
				else if constexpr (std::is_floating_point_v<T>) hash.Add(value);
				else if constexpr (std::is_same_v<T, Animation::AnimationFloat3>) { hash.Add(value.x); hash.Add(value.y); hash.Add(value.z); }
				else if constexpr (std::is_same_v<T, Animation::AnimationQuaternion>) { hash.Add(value.x); hash.Add(value.y); hash.Add(value.z); hash.Add(value.w); }
				else { hash.Add(value.translation.x); hash.Add(value.translation.y); hash.Add(value.translation.z); hash.Add(value.rotation.x); hash.Add(value.rotation.y); hash.Add(value.rotation.z); hash.Add(value.rotation.w); hash.Add(value.scale.x); hash.Add(value.scale.y); hash.Add(value.scale.z); }
			}, parameter.value);
		}
	}
	std::string HashPose(const Animation::AnimationAuthorityRow& owner) {
		const auto& snapshot = *owner.snapshot;
		CanonicalHash hash;
		for (const auto byte : owner.ownerObjectId) hash.Add(static_cast<std::uint64_t>(byte));
		hash.Add(static_cast<std::uint64_t>(snapshot.tick));
		if (!snapshot.pose) return hash.Finish();
		hash.Add(static_cast<std::uint64_t>(owner.jointStableIds.size()));
		for (const std::uint64_t stableId : owner.jointStableIds) hash.Add(stableId);
		hash.Add(static_cast<std::uint64_t>(snapshot.pose->LocalTransforms().size()));
		for (const auto& transform : snapshot.pose->LocalTransforms()) {
			hash.Add(transform.translation.x); hash.Add(transform.translation.y); hash.Add(transform.translation.z);
			hash.Add(transform.rotation.x); hash.Add(transform.rotation.y); hash.Add(transform.rotation.z); hash.Add(transform.rotation.w);
			hash.Add(transform.scale.x); hash.Add(transform.scale.y); hash.Add(transform.scale.z);
		}
		hash.Add(static_cast<std::uint64_t>(snapshot.pose->ModelTransforms().size()));
		for (const auto& transform : snapshot.pose->ModelTransforms()) {
			hash.Add(transform.translation.x); hash.Add(transform.translation.y); hash.Add(transform.translation.z);
			hash.Add(transform.rotation.x); hash.Add(transform.rotation.y); hash.Add(transform.rotation.z); hash.Add(transform.rotation.w);
			hash.Add(transform.scale.x); hash.Add(transform.scale.y); hash.Add(transform.scale.z);
		}
		hash.Add(static_cast<std::uint64_t>(snapshot.pose->MorphValues().size()));
		for (const auto& value : snapshot.pose->MorphValues()) { hash.Add(static_cast<std::uint64_t>(value.stableId)); hash.Add(value.value); }
		hash.Add(static_cast<std::uint64_t>(snapshot.pose->CurveValues().size()));
		for (const auto& value : snapshot.pose->CurveValues()) { hash.Add(static_cast<std::uint64_t>(value.stableId)); hash.Add(value.value); }
		hash.Add(static_cast<std::uint64_t>(snapshot.pose->Events().size()));
		for (const auto& event : snapshot.pose->Events()) {
			AddGuid(hash, event.identity.clipAssetId);
			hash.Add(event.identity.stateInstanceId);
			hash.Add(static_cast<std::uint64_t>(event.identity.eventId));
			hash.Add(static_cast<std::uint64_t>(event.identity.sourceTick));
			hash.Add(static_cast<std::uint64_t>(event.identity.loopCount));
			hash.Add(event.sourceStateId); hash.Add(static_cast<std::uint64_t>(event.layerIndex));
			hash.Add(static_cast<std::uint64_t>(event.direction)); hash.Add(static_cast<std::uint64_t>(event.payloadType));
			hash.Add(static_cast<std::uint64_t>(event.payload.size()));
			for (const auto byte : event.payload) hash.Add(static_cast<std::uint64_t>(static_cast<unsigned char>(byte)));
			hash.Add(static_cast<std::uint64_t>(event.historical));
		}
		hash.Add(static_cast<std::uint64_t>(snapshot.pose->RootMotionDelta().has_value()));
		if (snapshot.pose->RootMotionDelta()) {
			const auto& root = *snapshot.pose->RootMotionDelta();
			hash.Add(static_cast<std::uint64_t>(root.sourceBeginTick)); hash.Add(static_cast<std::uint64_t>(root.sourceEndTick));
			hash.Add(root.localTranslation.x); hash.Add(root.localTranslation.y); hash.Add(root.localTranslation.z); hash.Add(root.localYawRadians);
			hash.Add(root.worldTranslation.x); hash.Add(root.worldTranslation.y); hash.Add(root.worldTranslation.z); hash.Add(root.worldYawRadians);
			hash.Add(static_cast<std::uint64_t>(root.accumulatedCycles)); hash.Add(root.sourceStateId); hash.Add(static_cast<std::uint64_t>(root.layerIndex));
			hash.Add(static_cast<std::uint64_t>(root.policy));
		}
		return hash.Finish();
	}
	std::string HashState(const Animation::AnimationAuthorityRow& pose) {
		CanonicalHash hash;
		for (const auto byte : pose.ownerObjectId) hash.Add(static_cast<std::uint64_t>(byte));
		AddHandle(hash, pose.controller); AddHandle(hash, pose.clip); AddHandle(hash, pose.skeleton);
		hash.Add(static_cast<std::uint64_t>(pose.requestedExecutionPolicy));
		hash.Add(static_cast<std::uint64_t>(pose.rootMotionPolicy));
		hash.Add(static_cast<std::uint64_t>(pose.updatePolicy));
		hash.Add(static_cast<std::uint64_t>(pose.eventPolicy));
		hash.Add(static_cast<std::uint64_t>(pose.cullingPolicy));
		hash.Add(static_cast<std::uint64_t>(pose.sourceObjectWorldTransformCaptured));
		if (pose.sourceObjectWorldTransformCaptured) {
			const auto& transform = pose.sourceObjectWorldTransform;
			hash.Add(transform.translation.x); hash.Add(transform.translation.y); hash.Add(transform.translation.z);
			hash.Add(transform.rotation.x); hash.Add(transform.rotation.y); hash.Add(transform.rotation.z); hash.Add(transform.rotation.w);
			hash.Add(transform.scale.x); hash.Add(transform.scale.y); hash.Add(transform.scale.z);
		}
		if (pose.snapshot) AddPlayback(hash, pose.snapshot->playback);
		if (pose.snapshot) hash.Add(static_cast<std::uint64_t>(pose.snapshot->tick));
		return hash.Finish();
	}

	std::string HashBytes(std::string_view bytes) {
		CanonicalHash hash;
		for (const unsigned char byte : bytes) hash.Add(static_cast<std::uint64_t>(byte));
		return hash.Finish();
	}
}

AnimationParityAnimatorDigest ComputeAnimationParityAnimatorDigest(const Animation::AnimationAuthorityRow& pose) {
	AnimationParityAnimatorDigest digest;
	if (!pose.snapshot || !pose.snapshot->pose) return digest;
	digest.stateHash = HashState(pose);
	digest.poseHash = HashPose(pose);
	return digest;
}

std::string CanonicalAnimationParityHash(std::string_view bytes) { return HashBytes(bytes); }

nlohmann::json AnimationParityReport::ToJson() const {
	return {
		{"schemaVersion", 1}, {"succeeded", succeeded}, {"orderlyShutdown", orderlyShutdown},
		{"mode", mode}, {"requestedTicks", requestedTicks}, {"fixedDeltaSeconds", fixedDeltaSeconds},
		{"ticksEvaluated", ticksEvaluated}, {"animatorCount", animatorCount}, {"diagnostic", diagnostic},
		{"animators", animators}, {"ticks", ticks}, {"visualCapture", visualCapture}
	};
}

bool CaptureAnimationParityTick(
	const Animation::AnimationSystem& animation, std::uint32_t tickIndex, AnimationParityReport& report) {
	const std::shared_ptr<const Animation::AnimationAuthorityFrame> authority =
		animation.LoadPublishedAuthorityFrame();
	if (!authority || !authority->IsValid() || !authority->pages) {
		report.diagnostic = "The mounted startup scene published zero animators.";
		return false;
	}
	std::size_t animatorCount = 0U;
	for(const auto& page : *authority->pages) if(page) {
		for(const Animation::AnimationAuthorityRow& pose : page->rows) {
			// A pinned authority root may intentionally outlive component retirement so
			// in-flight readers remain safe. Parity evidence counts only generation-valid
			// live animators and never mistakes a retired immutable row for scene output.
			if(animation.IsAnimatorInstanceAlive(pose.key.Handle())) ++animatorCount;
		}
	}
	if(animatorCount == 0U) {
		report.diagnostic = "The mounted startup scene published zero animators.";
		return false;
	}
	report.animatorCount = static_cast<std::uint32_t>(animatorCount);
	nlohmann::json frame = { {"tickIndex", tickIndex}, {"animators", nlohmann::json::array()} };
	for(const auto& page : *authority->pages) if(page) for(const Animation::AnimationAuthorityRow& pose : page->rows) {
		if(!animation.IsAnimatorInstanceAlive(pose.key.Handle())) continue;
		if (pose.safeFallback || pose.status != Animation::AnimationStatus::Ok
			|| pose.objectGeneration == 0U || pose.componentGeneration == 0U
			|| !pose.snapshot || !pose.snapshot->pose || !pose.snapshot->pose->IsSealed()) {
			report.diagnostic = "A published animator pose was missing, safe, invalid, or unsealed.";
			return false;
		}
		const auto digest = ComputeAnimationParityAnimatorDigest(pose);
		frame["animators"].push_back({
			{"ownerObjectId", ToHexString(pose.ownerObjectId)},
			{"effectiveExecutionMode", static_cast<std::uint32_t>(pose.effectiveExecutionMode)},
			{"status", std::string(Animation::ToString(pose.status))},
			{"stateHash", digest.stateHash}, {"poseHash", digest.poseHash},
			{"eventCount", pose.snapshot->pose->Events().size()},
			{"rootMotion", pose.snapshot->pose->RootMotionDelta().has_value()}
		});
	}
	report.ticks.push_back(std::move(frame));
	report.ticksEvaluated = static_cast<std::uint32_t>(report.ticks.size());
	return true;
}

AnimationParityReport ValidatePublishedAnimationParity(
	const Animation::AnimationSystem& animation,
	const AnimationParityValidationOptions& options) {
	AnimationParityReport report;
	report.requestedTicks = options.tickCount;
	report.fixedDeltaSeconds = options.fixedDeltaSeconds;
	report.mode = options.mode;
	report.visualCapture = {
		{"required", options.requireRenderedVisualCapture},
		{"available", false}
	};
	if (options.tickCount == 0U || !std::isfinite(options.fixedDeltaSeconds) || options.fixedDeltaSeconds <= 0.0F) {
		report.diagnostic = "Animation parity requires a positive bounded fixed tick configuration.";
		return report;
	}
	if (!CaptureAnimationParityTick(animation, 0U, report)) return report;
	report.animators = report.ticks.front().at("animators");
	report.succeeded = true;
	return report;
}
} // namespace TerraEngine::Runtime
