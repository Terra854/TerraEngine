// MotionDatabaseAuthoringHost.cpp - shared-pool clip sampling for database builds.

#include "MotionDatabaseAuthoringHost.h"

#include "../Assets/AssetDatabase.h"
#include "Engine/Animation/Evaluation/CpuClipEvaluator.h"
#include "Engine/Resource/ResourceManager.h"
#include "Engine/ThreadPool/ThreadPool.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <fstream>
#include <limits>
#include <set>
#include <span>
#include <system_error>
#include <utility>

namespace TerraEngine::Editor::Animation {
	namespace {
		using namespace TerraEngine::Animation;
		using namespace TerraEngine::AssetPipeline;

		class ThreadPoolMotionDatabaseCompileExecutor final
			: public MotionDatabaseCompileExecutor {
		public:
			void Enqueue(std::function<void()> work) override {
				if(!work) return;
				if(ThreadPool::instance().getThreadCount() == 0U) {
					// Headless editor tests may intentionally omit worker initialization.
					// The same copied task boundary is retained even in this fallback.
					work();
					return;
				}
				ThreadPool::instance().enqueue(
					ThreadPool::instance().createTask(std::move(work)), 1U);
			}
		};

		bool IsInside(const std::filesystem::path& child,
			const std::filesystem::path& root) {
			if(root.empty()) return false;
			std::error_code error;
			const std::filesystem::path relative = std::filesystem::relative(child, root, error);
			if(error || relative.empty()) return false;
			const auto first = relative.begin();
			return first != relative.end() && *first != "..";
		}

		std::uint64_t HashBytes64(std::span<const std::uint8_t> bytes) noexcept {
			std::uint64_t hash = 14695981039346656037ULL;
			for(const std::uint8_t value : bytes) {
				hash = (hash ^ value) * 1099511628211ULL;
			}
			return hash == 0U ? 1U : hash;
		}

		std::uint64_t SourceId(const AnimationGuid& assetId) noexcept {
			return HashBytes64(std::span<const std::uint8_t>(
				assetId.value.data(), assetId.value.size()));
		}

		std::uint64_t SourceFingerprint(const AnimationContentHash& hash) noexcept {
			return HashBytes64(std::span<const std::uint8_t>(hash.data(), hash.size()));
		}

		AnimationStatus ReadEnvelope(const std::filesystem::path& path,
			AnimationAssetEnvelope& output) {
			std::error_code error;
			const std::uintmax_t size = std::filesystem::file_size(path, error);
			if(error || size < sizeof(AnimationAssetEnvelope)
				|| size > kMotionDatabaseMaximumPayloadBytes) {
				return error ? AnimationStatus::MissingAsset : AnimationStatus::HardLimitExceeded;
			}
			std::ifstream stream(path, std::ios::binary);
			AnimationAssetEnvelope staged;
			stream.read(reinterpret_cast<char*>(&staged), sizeof(staged));
			if(!stream) return AnimationStatus::TruncatedInput;
			if(staged.magic != kAnimationAssetMagic
				|| staged.majorVersion != kAnimationAssetMajorVersion
				|| staged.headerSize != sizeof(AnimationAssetEnvelope)
				|| staged.endianMarker != kAnimationLittleEndianMarker
				|| !staged.assetId.IsValid()
				|| staged.payloadSize != size - sizeof(AnimationAssetEnvelope)) {
				return AnimationStatus::InvalidEnvelope;
			}
			output = staged;
			return AnimationStatus::Ok;
		}

		AnimationStatus ReadClip(const std::filesystem::path& path,
			AnimationCompatibilityFingerprint compatibility,
			AnimationClipAsset& output) {
			std::error_code error;
			const std::uintmax_t size = std::filesystem::file_size(path, error);
			if(error || size < sizeof(AnimationAssetEnvelope)
				|| size > kMotionDatabaseMaximumPayloadBytes) {
				return error ? AnimationStatus::MissingAsset : AnimationStatus::HardLimitExceeded;
			}
			std::ifstream stream(path, std::ios::binary | std::ios::ate);
			if(!stream) return AnimationStatus::MissingAsset;
			const std::streamoff length = stream.tellg();
			if(length < 0 || static_cast<std::uint64_t>(length) != size) {
				return AnimationStatus::TruncatedInput;
			}
			stream.seekg(0, std::ios::beg);
			std::vector<std::uint8_t> bytes(static_cast<std::size_t>(length));
			stream.read(reinterpret_cast<char*>(bytes.data()), length);
			if(!stream && length != 0) return AnimationStatus::TruncatedInput;
			return DeserializeAnimationClipAsset(bytes, compatibility, output);
		}

		AnimationStatus EvaluatePoint(const AnimationClipView& view,
			std::span<const std::uint64_t> jointIds, AnimationFixedTick tick,
			CpuClipEvaluationResult& output) {
			CpuClipEvaluationRequest request;
			request.tick = tick;
			request.jointStableIds = jointIds;
			request.includeRootMotionPoint = true;
			request.includeEventsAtPoint = false;
			return CpuClipEvaluator{}.Evaluate(view, request, output);
		}

		AnimationStatus ExtractClipSamples(const AnimationClipAsset& clip,
			std::uint64_t sourceClipId, MotionDatabaseSourceClipInput& output,
			const std::atomic_bool& cancellationRequested) {
			AnimationClipView view;
			if(const AnimationStatus status = view.Open(clip); status != AnimationStatus::Ok) {
				return status;
			}
			std::vector<std::uint64_t> jointIds;
			for(const AnimationClipTrack& track : clip.tracks) {
				if(track.kind == AnimationClipTrackKind::Translation
					|| track.kind == AnimationClipTrackKind::Rotation
					|| track.kind == AnimationClipTrackKind::Scale) {
					jointIds.push_back(track.stableTargetId);
				}
			}
			std::sort(jointIds.begin(), jointIds.end());
			jointIds.erase(std::unique(jointIds.begin(), jointIds.end()), jointIds.end());
			if(jointIds.size() > 6U) jointIds.resize(6U);
			if(jointIds.empty()) return AnimationStatus::InvalidArgument;

			const AnimationFixedTick step = std::max<AnimationFixedTick>(1,
				static_cast<AnimationFixedTick>(kAnimationFixedTicksPerSecond
					/ std::max(clip.sampleRate, 1U)));
			const std::uint64_t sampleCount = static_cast<std::uint64_t>(clip.durationTicks / step) + 1U;
			if(sampleCount > kMotionDatabaseMaximumSamples) return AnimationStatus::HardLimitExceeded;

			MotionDatabaseSourceClipInput staged;
			staged.sourceClipId = sourceClipId;
			staged.samples.reserve(static_cast<std::size_t>(sampleCount));
			for(AnimationFixedTick tick = 0; tick <= clip.durationTicks; tick += step) {
				if(cancellationRequested.load(std::memory_order_acquire)) return AnimationStatus::Cancelled;
				CpuClipEvaluationResult current;
				const AnimationStatus currentStatus = EvaluatePoint(view, jointIds, tick, current);
				if(currentStatus != AnimationStatus::Ok) return currentStatus;
				CpuClipEvaluationResult previous;
				const AnimationStatus previousStatus = EvaluatePoint(
					view, jointIds, std::max<AnimationFixedTick>(0, tick - step), previous);
				if(previousStatus != AnimationStatus::Ok) return previousStatus;

				MotionDatabaseClipSampleInput sample;
				sample.sourceTick = tick;
				sample.continuityGroup = static_cast<std::uint32_t>(sourceClipId);
				if(sample.continuityGroup == 0U) sample.continuityGroup = 1U;
				for(std::size_t joint = 0U; joint < current.joints.size() && joint < 6U; ++joint) {
					const AnimationFloat3& position = current.joints[joint].localTransform.translation;
					sample.posePoints[joint] = {position.x, position.y, position.z};
					if(joint < 3U) {
						const AnimationFloat3& prior = previous.joints[joint].localTransform.translation;
						const float rate = static_cast<float>(kAnimationFixedTicksPerSecond)
							/ static_cast<float>(step);
						sample.velocityPoints[joint] = {
							(position.x - prior.x) * rate,
							(position.y - prior.y) * rate,
							(position.z - prior.z) * rate};
					}
				}
				const AnimationFloat3 currentRoot = current.rootMotion.enabled
					? current.rootMotion.translation : AnimationFloat3{};
				for(std::size_t horizon = 0U; horizon < sample.trajectory.size(); ++horizon) {
					const AnimationFixedTick futureTick = std::min<AnimationFixedTick>(clip.durationTicks,
						tick + static_cast<AnimationFixedTick>((horizon + 1U) * 12U));
					CpuClipEvaluationResult future;
					const AnimationStatus futureStatus = EvaluatePoint(view, jointIds, futureTick, future);
					if(futureStatus != AnimationStatus::Ok) return futureStatus;
					const AnimationFloat3 futureRoot = future.rootMotion.enabled
						? future.rootMotion.translation : AnimationFloat3{};
					sample.trajectory[horizon].positionX = futureRoot.x - currentRoot.x;
					sample.trajectory[horizon].positionZ = futureRoot.z - currentRoot.z;
					sample.trajectory[horizon].facingX = std::sin(future.rootMotion.yawRadians);
					sample.trajectory[horizon].facingZ = std::cos(future.rootMotion.yawRadians);
				}
				sample.facing = {std::sin(current.rootMotion.yawRadians), 0.0F,
					std::cos(current.rootMotion.yawRadians)};
				sample.phaseQuantized = clip.durationTicks == 0 ? 0U
					: static_cast<std::uint16_t>((static_cast<std::uint64_t>(tick) * UINT16_MAX)
						/ static_cast<std::uint64_t>(clip.durationTicks));
				staged.samples.push_back(sample);
				if(clip.durationTicks - tick < step) break;
			}
			output = std::move(staged);
			return AnimationStatus::Ok;
		}
	}

	MotionDatabaseSourceResolver::MotionDatabaseSourceResolver(
		std::filesystem::path assetRoot, std::filesystem::path buildRoot)
		: assetRoot_(std::move(assetRoot)), buildRoot_(std::move(buildRoot)) {}

	AnimationStatus MotionDatabaseSourceResolver::ResolvePath(
		const std::filesystem::path& authoredPath, std::filesystem::path& output) const {
		if(authoredPath.empty() || authoredPath.extension() != ".teanim") {
			return AnimationStatus::InvalidArgument;
		}
		auto resolveTier = [this, &output](
			std::span<const std::filesystem::path> candidates) {
			std::set<std::filesystem::path> matches;
			for(const std::filesystem::path& candidate : candidates) {
				std::error_code error;
				if(!std::filesystem::is_regular_file(candidate, error) || error) continue;
				const std::filesystem::path canonical =
					std::filesystem::weakly_canonical(candidate, error);
				if(error || (!IsInside(canonical, assetRoot_)
					&& !IsInside(canonical, buildRoot_))) continue;
				matches.insert(canonical);
			}
			if(matches.size() > 1U) return AnimationStatus::Rejected;
			if(matches.empty()) return AnimationStatus::MissingAsset;
			output = *matches.begin();
			return AnimationStatus::Ok;
		};
		if(authoredPath.is_absolute()) {
			const std::array absoluteCandidates{authoredPath};
			return resolveTier(absoluteCandidates);
		}

		// A normal project import can exist both as the authoritative loose asset
		// and as its cooked Build mirror. Prefer the checked Assets tier so that
		// this expected duplication is not mistaken for an ambiguous authored path.
		const std::array assetCandidates{
			assetRoot_ / authoredPath,
			assetRoot_.parent_path() / authoredPath};
		const AnimationStatus assetStatus = resolveTier(assetCandidates);
		if(assetStatus != AnimationStatus::MissingAsset) return assetStatus;
		const std::array buildCandidates{buildRoot_ / authoredPath};
		return resolveTier(buildCandidates);
	}

	AnimationStatus MotionDatabaseSourceResolver::ResolveIdentity(
		MotionDatabaseAuthoringSource& source,
		AnimationCompatibilityFingerprint expectedCompatibility) const {
		std::filesystem::path resolved;
		if(const AnimationStatus status = ResolvePath(source.sourcePath, resolved);
			status != AnimationStatus::Ok) return status;
		AnimationAssetEnvelope envelope;
		if(const AnimationStatus status = ReadEnvelope(resolved, envelope);
			status != AnimationStatus::Ok) return status;
		if(expectedCompatibility == 0U
			|| envelope.compatibilityFingerprint != expectedCompatibility) {
			return AnimationStatus::IncompatibleAsset;
		}
		const std::uint64_t id = SourceId(envelope.assetId);
		const std::uint64_t fingerprint = SourceFingerprint(envelope.contentHash);
		if((source.sourceClipId != 0U && source.sourceClipId != id)
			|| (source.sourceClipFingerprint != 0U
				&& source.sourceClipFingerprint != fingerprint)) {
			return AnimationStatus::StaleHandle;
		}
		source.sourceClipId = id;
		source.sourceClipFingerprint = fingerprint;
		return AnimationStatus::Ok;
	}

	AnimationStatus MotionDatabaseSourceResolver::ReadCompatibility(
		const MotionDatabaseAuthoringSource& source,
		AnimationCompatibilityFingerprint& compatibility) const {
		std::filesystem::path resolved;
		if(const AnimationStatus status = ResolvePath(source.sourcePath, resolved);
			status != AnimationStatus::Ok) return status;
		AnimationAssetEnvelope envelope;
		if(const AnimationStatus status = ReadEnvelope(resolved, envelope);
			status != AnimationStatus::Ok) return status;
		if(envelope.compatibilityFingerprint == 0U) return AnimationStatus::IncompatibleAsset;
		compatibility = envelope.compatibilityFingerprint;
		return AnimationStatus::Ok;
	}

	AnimationStatus MotionDatabaseSourceResolver::Resolve(
		const MotionDatabaseAuthoringDocument& document,
		std::vector<MotionDatabaseSourceClipInput>& output,
		const std::atomic_bool& cancellationRequested) const {
		if(ValidateMotionDatabaseAuthoringDocument(document) != AnimationStatus::Ok) {
			return AnimationStatus::InvalidArgument;
		}
		try {
			std::vector<MotionDatabaseSourceClipInput> resolvedInputs;
			std::size_t totalSamples = 0U;
			for(const MotionDatabaseAuthoringSource& authored : document.sources) {
				if(!authored.enabled) continue;
				if(cancellationRequested.load(std::memory_order_acquire)) return AnimationStatus::Cancelled;
				MotionDatabaseAuthoringSource checked = authored;
				const AnimationStatus identityStatus = ResolveIdentity(
					checked, document.compatibilityFingerprint);
				if(identityStatus != AnimationStatus::Ok) return identityStatus;
				std::filesystem::path resolvedPath;
				const AnimationStatus pathStatus = ResolvePath(checked.sourcePath, resolvedPath);
				if(pathStatus != AnimationStatus::Ok) return pathStatus;
				AnimationClipAsset clip;
				const AnimationStatus decodeStatus = ReadClip(resolvedPath,
					document.compatibilityFingerprint, clip);
				if(decodeStatus != AnimationStatus::Ok
					|| SourceId(clip.assetId) != checked.sourceClipId) {
					return decodeStatus == AnimationStatus::Ok
						? AnimationStatus::StaleHandle : decodeStatus;
				}
				MotionDatabaseSourceClipInput input;
				const AnimationStatus extractionStatus = ExtractClipSamples(
					clip, checked.sourceClipId, input, cancellationRequested);
				if(extractionStatus != AnimationStatus::Ok) return extractionStatus;
				if(input.samples.size() > kMotionDatabaseMaximumSamples - totalSamples) {
					return AnimationStatus::HardLimitExceeded;
				}
				totalSamples += input.samples.size();
				input.sourceClipFingerprint = checked.sourceClipFingerprint;
				input.baseTagMask = checked.baseTagMask;
				input.baseContactMask = checked.baseContactMask;
				input.contactRanges = checked.contactRanges;
				input.tagRanges = checked.tagRanges;
				input.exclusionRanges = checked.exclusionRanges;
				resolvedInputs.push_back(std::move(input));
			}
			output = std::move(resolvedInputs);
			return AnimationStatus::Ok;
		} catch(...) {
			return AnimationStatus::InternalError;
		}
	}

	struct MotionDatabaseProductionAuthoringHost::State {
		std::filesystem::path assetRoot;
		std::filesystem::path buildRoot;
		std::shared_ptr<MotionDatabaseCompileExecutor> executor;
		bool useLiveRoots = true;
	};

	MotionDatabaseProductionAuthoringHost::MotionDatabaseProductionAuthoringHost()
		: state_(std::make_unique<State>()) {
		state_->executor = std::make_shared<ThreadPoolMotionDatabaseCompileExecutor>();
	}

	MotionDatabaseProductionAuthoringHost::MotionDatabaseProductionAuthoringHost(
		std::filesystem::path assetRoot, std::filesystem::path buildRoot,
		std::shared_ptr<MotionDatabaseCompileExecutor> executor)
		: state_(std::make_unique<State>()) {
		state_->assetRoot = std::move(assetRoot);
		state_->buildRoot = std::move(buildRoot);
		state_->executor = executor ? std::move(executor)
			: std::make_shared<ThreadPoolMotionDatabaseCompileExecutor>();
		state_->useLiveRoots = false;
	}

	MotionDatabaseProductionAuthoringHost::~MotionDatabaseProductionAuthoringHost() = default;

	std::shared_ptr<MotionDatabaseCompileExecutor>
	MotionDatabaseProductionAuthoringHost::GetMotionDatabaseCompileExecutor() {
		return state_->executor;
	}

	AnimationStatus MotionDatabaseProductionAuthoringHost::ResolveMotionDatabaseSourceIdentity(
		MotionDatabaseAuthoringSource& source,
		AnimationCompatibilityFingerprint expectedCompatibility) {
		const std::filesystem::path assetRoot = state_->useLiveRoots
			? AssetDatabase::Instance().GetAssetRoot() : state_->assetRoot;
		const std::filesystem::path buildRoot = state_->useLiveRoots
			? AssetDatabase::Instance().GetBuildRoot() : state_->buildRoot;
		return MotionDatabaseSourceResolver(assetRoot, buildRoot).ResolveIdentity(
			source, expectedCompatibility);
	}

	AnimationStatus MotionDatabaseProductionAuthoringHost::ReadMotionDatabaseSourceCompatibility(
		const MotionDatabaseAuthoringSource& source,
		AnimationCompatibilityFingerprint& compatibility) {
		const std::filesystem::path assetRoot = state_->useLiveRoots
			? AssetDatabase::Instance().GetAssetRoot() : state_->assetRoot;
		const std::filesystem::path buildRoot = state_->useLiveRoots
			? AssetDatabase::Instance().GetBuildRoot() : state_->buildRoot;
		return MotionDatabaseSourceResolver(assetRoot, buildRoot).ReadCompatibility(
			source, compatibility);
	}

	MotionDatabaseWorkerSourceResolver
	MotionDatabaseProductionAuthoringHost::CreateMotionDatabaseSourceResolver(
		const MotionDatabaseAuthoringDocument& document) {
		const std::filesystem::path assetRoot = state_->useLiveRoots
			? AssetDatabase::Instance().GetAssetRoot() : state_->assetRoot;
		const std::filesystem::path buildRoot = state_->useLiveRoots
			? AssetDatabase::Instance().GetBuildRoot() : state_->buildRoot;
		return [resolver = MotionDatabaseSourceResolver(assetRoot, buildRoot), document](
			std::vector<MotionDatabaseSourceClipInput>& output,
			const std::atomic_bool& cancellationRequested) {
			return resolver.Resolve(document, output, cancellationRequested);
		};
	}

	void MotionDatabaseProductionAuthoringHost::NotifyMotionDatabasePublished(
		const std::filesystem::path& outputPath, AnimationGuid assetId) {
		const std::filesystem::path assetRoot = state_->useLiveRoots
			? AssetDatabase::Instance().GetAssetRoot() : state_->assetRoot;
		std::error_code error;
		const std::filesystem::path canonical = std::filesystem::weakly_canonical(outputPath, error);
		if(!error && IsInside(canonical, assetRoot) && state_->useLiveRoots) {
			AssetDatabase::Instance().ImportAssetWithProgress(canonical, true);
			return;
		}
		if(state_->useLiveRoots) {
			(void)ResourceManager::Instance().ReloadAnimationAsset(
				assetId.value, AssetType::MotionDatabase);
		}
	}
}
