// MotionDatabaseCompiler.cpp - Staged extraction, normalization, validation, and publication.

#include "MotionDatabaseCompiler.h"

#include <algorithm>
#include <bit>
#include <cmath>
#include <fstream>
#include <limits>
#include <new>
#include <nlohmann/json.hpp>
#include <system_error>
#include <tuple>
#include <type_traits>
#include <unordered_map>

namespace TerraEngine::AssetPipeline {
		namespace {
		using namespace TerraEngine::Animation;

		constexpr std::size_t kCancellationCheckInterval = 1024U;
		constexpr std::uint32_t kCompileProgressSteps = 6U;
		std::mutex gMotionDatabasePublicationMutex;
		std::mutex gMotionDatabaseAuthoringPathLocksMutex;
		std::unordered_map<std::string, std::weak_ptr<std::mutex>>
			gMotionDatabaseAuthoringPathLocks;
		std::atomic_uint64_t gMotionDatabaseStagingSequence = 1U;
		static_assert(std::is_nothrow_move_assignable_v<MotionDatabaseAuthoringSnapshot>,
			"A published CAS token must be installable after rename without reporting a false failure.");

		std::shared_ptr<std::mutex> GetMotionDatabaseAuthoringPathLock(
			const std::filesystem::path& path) {
			// weakly_canonical accepts a missing leaf and gives equivalent relative
			// spellings one process-wide key. The fallback still normalizes a path if
			// its parent cannot be inspected because of an IO error.
			std::error_code error;
			std::filesystem::path normalized = std::filesystem::weakly_canonical(path, error);
			if(error) {
				error.clear();
				normalized = std::filesystem::absolute(path, error);
				if(error) normalized = path;
			}
			const std::string key = normalized.lexically_normal().generic_string();
			std::scoped_lock mapLock(gMotionDatabaseAuthoringPathLocksMutex);
			if(const auto found = gMotionDatabaseAuthoringPathLocks.find(key);
				found != gMotionDatabaseAuthoringPathLocks.end()) {
				if(std::shared_ptr<std::mutex> existing = found->second.lock()) return existing;
			}
			auto created = std::make_shared<std::mutex>();
			gMotionDatabaseAuthoringPathLocks[key] = created;
			return created;
		}

		void HashBytes(std::uint64_t& hash, const void* data, std::size_t size) noexcept {
			constexpr std::uint64_t kFnvPrime = 1099511628211ULL;
			const auto* bytes = static_cast<const std::uint8_t*>(data);
			for(std::size_t index = 0U; index < size; ++index) {
				hash ^= bytes[index];
				hash *= kFnvPrime;
			}
		}

		template<typename Value>
		void HashValue(std::uint64_t& hash, const Value& value) noexcept {
			HashBytes(hash, &value, sizeof(value));
		}

		bool PublishStagedFile(const std::filesystem::path& stagedPath,
			const std::filesystem::path& outputPath) noexcept {
			std::scoped_lock publicationLock(gMotionDatabasePublicationMutex);
			std::error_code error;
			if(!outputPath.parent_path().empty()) {
				std::filesystem::create_directories(outputPath.parent_path(), error);
				if(error) return false;
			}
			const std::filesystem::path backupPath = outputPath.string() + ".previous";
			std::filesystem::remove(backupPath, error);
			error.clear();
			const bool hadPrevious = std::filesystem::exists(outputPath, error) && !error;
			if(hadPrevious) {
				std::filesystem::rename(outputPath, backupPath, error);
				if(error) return false;
			}
			std::filesystem::rename(stagedPath, outputPath, error);
			if(error) {
				if(hadPrevious) {
					std::error_code restoreError;
					std::filesystem::rename(backupPath, outputPath, restoreError);
				}
				return false;
			}
			if(hadPrevious) std::filesystem::remove(backupPath, error);
			return true;
		}

		AnimationStatus StageMotionDatabaseAsset(const std::filesystem::path& outputPath,
			std::uint64_t sequence, const MotionDatabaseAsset& asset,
			std::filesystem::path& stagedPath) noexcept {
			if(outputPath.extension() != ".temotiondb"
				|| ValidateMotionDatabaseAsset(asset) != AnimationStatus::Ok) {
				return AnimationStatus::InvalidArgument;
			}
			std::vector<std::uint8_t> bytes;
			if(SerializeMotionDatabaseAsset(asset, bytes) != AnimationStatus::Ok) {
				return AnimationStatus::Rejected;
			}
			const std::uint64_t stagingSequence =
				gMotionDatabaseStagingSequence.fetch_add(1U, std::memory_order_relaxed);
			if(stagingSequence == 0U) return AnimationStatus::InternalError;
			stagedPath = outputPath.string() + ".staged." + std::to_string(sequence)
				+ "." + std::to_string(stagingSequence);
			std::error_code error;
			try {
				if(!outputPath.parent_path().empty()) {
					std::filesystem::create_directories(outputPath.parent_path(), error);
					if(error) return AnimationStatus::InternalError;
				}
				std::filesystem::remove(stagedPath, error);
				{
					std::ofstream stream(stagedPath, std::ios::binary | std::ios::trunc);
					if(!stream) return AnimationStatus::InternalError;
					stream.write(reinterpret_cast<const char*>(bytes.data()),
						static_cast<std::streamsize>(bytes.size()));
					if(!stream) return AnimationStatus::InternalError;
				}
				std::ifstream input(stagedPath, std::ios::binary | std::ios::ate);
				if(!input) return AnimationStatus::InternalError;
				const std::streamoff length = input.tellg();
				if(length < 0 || static_cast<std::uint64_t>(length) > kMotionDatabaseMaximumPayloadBytes) {
					return AnimationStatus::HardLimitExceeded;
				}
				input.seekg(0, std::ios::beg);
				std::vector<std::uint8_t> readBack(static_cast<std::size_t>(length));
				input.read(reinterpret_cast<char*>(readBack.data()), length);
				if(!input && length != 0) return AnimationStatus::TruncatedInput;
				input.close();
				MotionDatabaseAsset decoded;
				const AnimationStatus decodedStatus = DeserializeMotionDatabaseAsset(
					readBack, asset.compatibilityFingerprint, decoded);
				if(decodedStatus != AnimationStatus::Ok
					|| ComputeMotionDatabaseContentFingerprint(decoded)
						!= ComputeMotionDatabaseContentFingerprint(asset)) {
					std::filesystem::remove(stagedPath, error);
					return decodedStatus == AnimationStatus::Ok
						? AnimationStatus::ContentHashMismatch : decodedStatus;
				}
				return AnimationStatus::Ok;
			} catch(...) {
				std::filesystem::remove(stagedPath, error);
				return AnimationStatus::InternalError;
			}
		}

		nlohmann::json RangeJson(const MotionDatabaseContactRange& range) {
			return {{"firstTick", range.firstTick}, {"lastTick", range.lastTick},
				{"contactMask", range.contactMask}};
		}

		nlohmann::json RangeJson(const MotionDatabaseTagRange& range) {
			return {{"firstTick", range.firstTick}, {"lastTick", range.lastTick},
				{"tagMask", range.tagMask}};
		}

		nlohmann::json RangeJson(const MotionDatabaseExclusionRange& range) {
			return {{"firstTick", range.firstTick}, {"lastTick", range.lastTick},
				{"exclusionFlags", range.exclusionFlags}};
		}

		nlohmann::json AuthoringJson(const MotionDatabaseAuthoringDocument& document) {
			nlohmann::json root{
				{"schemaVersion", document.schemaVersion},
				{"authoringGeneration", document.authoringGeneration},
				{"assetId", TerraEngine::ToHexString(document.assetId.value)},
				{"compatibilityFingerprint", document.compatibilityFingerprint},
				{"samplesPerBlock", document.samplesPerBlock},
				{"featureWeights", document.featureWeights},
				{"sources", nlohmann::json::array()}
			};
			for(const MotionDatabaseAuthoringSource& source : document.sources) {
				nlohmann::json value{
					{"sourceClipId", source.sourceClipId},
					{"sourceClipFingerprint", source.sourceClipFingerprint},
					{"sourcePath", source.sourcePath}, {"enabled", source.enabled},
					{"baseTagMask", source.baseTagMask},
					{"baseContactMask", source.baseContactMask},
					{"contactRanges", nlohmann::json::array()},
					{"tagRanges", nlohmann::json::array()},
					{"exclusionRanges", nlohmann::json::array()}
				};
				for(const auto& range : source.contactRanges) value["contactRanges"].push_back(RangeJson(range));
				for(const auto& range : source.tagRanges) value["tagRanges"].push_back(RangeJson(range));
				for(const auto& range : source.exclusionRanges) value["exclusionRanges"].push_back(RangeJson(range));
				root["sources"].push_back(std::move(value));
			}
			return root;
		}

		AnimationStatus SerializeAuthoringDocument(
			const MotionDatabaseAuthoringDocument& document, std::string& output) noexcept {
			try {
				output = AuthoringJson(document).dump(2);
				output.push_back('\n');
				return output.size() <= kMotionDatabaseMaximumAuthoringBytes
					? AnimationStatus::Ok : AnimationStatus::HardLimitExceeded;
			} catch(...) {
				return AnimationStatus::InternalError;
			}
		}

		AnimationStatus DecodeAuthoringDocument(std::string_view bytes,
			MotionDatabaseAuthoringDocument& output) noexcept {
			if(bytes.size() > kMotionDatabaseMaximumAuthoringBytes) {
				return AnimationStatus::HardLimitExceeded;
			}
			try {
				const nlohmann::json root = nlohmann::json::parse(bytes.begin(), bytes.end());
				MotionDatabaseAuthoringDocument staged;
				staged.schemaVersion = root.at("schemaVersion").get<std::uint32_t>();
				staged.authoringGeneration = root.at("authoringGeneration").get<std::uint64_t>();
				const auto parsedId = TerraEngine::ParseUUID(root.at("assetId").get<std::string>());
				if(!parsedId) return AnimationStatus::InvalidArgument;
				staged.assetId.value = *parsedId;
				staged.compatibilityFingerprint =
					root.at("compatibilityFingerprint").get<std::uint64_t>();
				staged.samplesPerBlock = root.at("samplesPerBlock").get<std::uint32_t>();
				staged.featureWeights = root.at("featureWeights").get<decltype(staged.featureWeights)>();
				for(const nlohmann::json& value : root.at("sources")) {
					MotionDatabaseAuthoringSource source;
					source.sourceClipId = value.at("sourceClipId").get<std::uint64_t>();
					source.sourceClipFingerprint =
						value.at("sourceClipFingerprint").get<std::uint64_t>();
					source.sourcePath = value.at("sourcePath").get<std::string>();
					source.enabled = value.value("enabled", true);
					source.baseTagMask = value.value("baseTagMask", std::uint64_t{0U});
					source.baseContactMask = value.value("baseContactMask", std::uint32_t{0U});
					for(const nlohmann::json& item :
						value.value("contactRanges", nlohmann::json::array())) {
						source.contactRanges.push_back({
							item.at("firstTick").get<AnimationFixedTick>(),
							item.at("lastTick").get<AnimationFixedTick>(),
							item.at("contactMask").get<std::uint32_t>()});
					}
					for(const nlohmann::json& item :
						value.value("tagRanges", nlohmann::json::array())) {
						source.tagRanges.push_back({
							item.at("firstTick").get<AnimationFixedTick>(),
							item.at("lastTick").get<AnimationFixedTick>(),
							item.at("tagMask").get<std::uint64_t>()});
					}
					for(const nlohmann::json& item :
						value.value("exclusionRanges", nlohmann::json::array())) {
						source.exclusionRanges.push_back({
							item.at("firstTick").get<AnimationFixedTick>(),
							item.at("lastTick").get<AnimationFixedTick>(),
							item.at("exclusionFlags").get<std::uint32_t>()});
					}
					staged.sources.push_back(std::move(source));
				}
				const AnimationStatus status = ValidateMotionDatabaseAuthoringDocument(staged);
				if(status != AnimationStatus::Ok) return status;
				output = std::move(staged);
				return AnimationStatus::Ok;
			} catch(const nlohmann::json::exception&) {
				return AnimationStatus::InvalidArgument;
			} catch(...) {
				return AnimationStatus::InternalError;
			}
		}

		AnimationStatus ReadAuthoringBytes(const std::filesystem::path& path,
			std::string& output) noexcept {
			try {
				std::ifstream stream(path, std::ios::binary | std::ios::ate);
				if(!stream) return AnimationStatus::MissingAsset;
				const std::streamoff length = stream.tellg();
				if(length < 0 || static_cast<std::uint64_t>(length)
					> kMotionDatabaseMaximumAuthoringBytes) {
					return AnimationStatus::HardLimitExceeded;
				}
				stream.seekg(0, std::ios::beg);
				std::string staged(static_cast<std::size_t>(length), '\0');
				stream.read(staged.data(), length);
				if(!stream && length != 0) return AnimationStatus::TruncatedInput;
				output = std::move(staged);
				return AnimationStatus::Ok;
			} catch(...) {
				return AnimationStatus::InternalError;
			}
		}

		bool SameAuthoringPayload(const MotionDatabaseAuthoringDocument& left,
			const MotionDatabaseAuthoringDocument& right) {
			MotionDatabaseAuthoringDocument normalizedLeft = left;
			MotionDatabaseAuthoringDocument normalizedRight = right;
			normalizedLeft.authoringGeneration = 1U;
			normalizedRight.authoringGeneration = 1U;
			return normalizedLeft == normalizedRight;
		}

		bool IsFinite(const MotionDatabaseSourceSample& sample) noexcept {
			return std::all_of(sample.features.begin(), sample.features.end(), [](float value) {
				return std::isfinite(value);
			});
		}

		bool IsFinite(const MotionDatabaseSourceVector3& value) noexcept {
			return std::isfinite(value.x) && std::isfinite(value.y) && std::isfinite(value.z);
		}

		bool IsFinite(const MotionDatabaseTrajectoryPoint& value) noexcept {
			return std::isfinite(value.positionX) && std::isfinite(value.positionZ)
				&& std::isfinite(value.facingX) && std::isfinite(value.facingZ);
		}

		bool IsFinite(const MotionDatabaseClipSampleInput& sample) noexcept {
			return std::all_of(sample.trajectory.begin(), sample.trajectory.end(),
				[](const MotionDatabaseTrajectoryPoint& point) { return IsFinite(point); })
				&& std::all_of(sample.posePoints.begin(), sample.posePoints.end(),
					[](const MotionDatabaseSourceVector3& point) { return IsFinite(point); })
				&& std::all_of(sample.velocityPoints.begin(), sample.velocityPoints.end(),
					[](const MotionDatabaseSourceVector3& point) { return IsFinite(point); })
				&& IsFinite(sample.facing);
		}

		bool ContainsTick(AnimationFixedTick tick, AnimationFixedTick first,
			AnimationFixedTick last) noexcept {
			return tick >= first && tick <= last;
		}

		template<typename Range>
		bool ValidRange(const Range& range) noexcept {
			return range.firstTick >= 0 && range.lastTick >= range.firstTick;
		}

		std::uint64_t MixStableId(std::uint64_t value, std::uint64_t input) noexcept {
			constexpr std::uint64_t kFnvPrime = 1099511628211ULL;
			for(std::size_t byte = 0U; byte < sizeof(input); ++byte) {
				value ^= static_cast<std::uint8_t>(input >> (byte * 8U));
				value *= kFnvPrime;
			}
			return value;
		}

		std::uint64_t DeriveStableSampleId(std::uint64_t sourceClipId,
			AnimationFixedTick sourceTick) noexcept {
			std::uint64_t hash = 14695981039346656037ULL;
			hash = MixStableId(hash, sourceClipId);
			hash = MixStableId(hash, static_cast<std::uint64_t>(sourceTick));
			// Zero is reserved for invalid/unset identities. This deterministic
			// remap avoids making one otherwise valid clip/tick pair unrepresentable.
			return hash == 0U ? 1U : hash;
		}

		std::int16_t Quantize(float value, float scale, float offset) noexcept {
			// Center the representable interval around zero. std::lround has defined
			// halfway-away-from-zero behavior and does not depend on the FP rounding mode.
			const float normalized = (value - offset) / scale - 32767.0F;
			return static_cast<std::int16_t>(std::clamp(std::lround(normalized),
				static_cast<long>(std::numeric_limits<std::int16_t>::min()),
				static_cast<long>(std::numeric_limits<std::int16_t>::max())));
		}

		AnimationStatus AppendExtractedClipSamples(const MotionDatabaseSourceClipInput& clip,
			std::vector<MotionDatabaseSourceSample>& output,
			const MotionDatabaseCompileCancellation& cancellation) {
			if(clip.sourceClipId == 0U || clip.sourceClipFingerprint == 0U
				|| !std::all_of(clip.contactRanges.begin(), clip.contactRanges.end(), ValidRange<MotionDatabaseContactRange>)
				|| !std::all_of(clip.tagRanges.begin(), clip.tagRanges.end(), ValidRange<MotionDatabaseTagRange>)
				|| !std::all_of(clip.exclusionRanges.begin(), clip.exclusionRanges.end(), ValidRange<MotionDatabaseExclusionRange>)) {
				return AnimationStatus::InvalidArgument;
			}
			std::vector<MotionDatabaseClipSampleInput> ordered = clip.samples;
			std::sort(ordered.begin(), ordered.end(), [](const auto& left, const auto& right) {
				if(left.sourceTick != right.sourceTick) return left.sourceTick < right.sourceTick;
				return left.stableSampleId < right.stableSampleId;
			});
			AnimationFixedTick priorTick = -1;
			for(std::size_t sampleIndex = 0U; sampleIndex < ordered.size(); ++sampleIndex) {
				if((sampleIndex % kCancellationCheckInterval) == 0U
					&& cancellation.IsCancellationRequested()) {
					return AnimationStatus::Cancelled;
				}
				const MotionDatabaseClipSampleInput& input = ordered[sampleIndex];
				if(input.sourceTick < 0 || input.sourceTick == priorTick || !IsFinite(input)) {
					return AnimationStatus::InvalidArgument;
				}
				priorTick = input.sourceTick;
				MotionDatabaseSourceSample sample;
				sample.stableSampleId = input.stableSampleId == 0U
					? DeriveStableSampleId(clip.sourceClipId, input.sourceTick)
					: input.stableSampleId;
				sample.sourceClipId = clip.sourceClipId;
				sample.sourceTick = input.sourceTick;
				sample.tagMask = clip.baseTagMask | input.tagMask;
				sample.contactMask = clip.baseContactMask | input.contactMask;
				sample.phaseQuantized = input.phaseQuantized;
				sample.transitionFlags = input.transitionFlags;
				sample.continuityGroup = input.continuityGroup;
				sample.exclusionFlags = input.exclusionFlags;

				for(const MotionDatabaseContactRange& range : clip.contactRanges) {
					if(ContainsTick(input.sourceTick, range.firstTick, range.lastTick)) {
						sample.contactMask |= range.contactMask;
					}
				}
				for(const MotionDatabaseTagRange& range : clip.tagRanges) {
					if(ContainsTick(input.sourceTick, range.firstTick, range.lastTick)) {
						sample.tagMask |= range.tagMask;
					}
				}
				for(const MotionDatabaseExclusionRange& range : clip.exclusionRanges) {
					if(ContainsTick(input.sourceTick, range.firstTick, range.lastTick)) {
						sample.exclusionFlags |= range.exclusionFlags;
					}
				}

				std::size_t feature = 0U;
				for(const MotionDatabaseTrajectoryPoint& point : input.trajectory) {
					sample.features[feature++] = point.positionX;
					sample.features[feature++] = point.positionZ;
					sample.features[feature++] = point.facingX;
					sample.features[feature++] = point.facingZ;
				}
				for(const MotionDatabaseSourceVector3& point : input.posePoints) {
					sample.features[feature++] = point.x;
					sample.features[feature++] = point.y;
					sample.features[feature++] = point.z;
				}
				for(const MotionDatabaseSourceVector3& point : input.velocityPoints) {
					sample.features[feature++] = point.x;
					sample.features[feature++] = point.y;
					sample.features[feature++] = point.z;
				}
				sample.features[feature++] = input.facing.x;
				sample.features[feature++] = input.facing.y;
				sample.features[feature++] = input.facing.z;
				sample.features[feature++] = (sample.contactMask & 1U) != 0U ? 1.0F : 0.0F;
				sample.features[feature++] = (sample.contactMask & 2U) != 0U ? 1.0F : 0.0F;
				sample.features[feature++] = static_cast<float>(sample.phaseQuantized) / 65535.0F;
				sample.features[feature++] = static_cast<float>(std::popcount(sample.tagMask));
				sample.features[feature++] = static_cast<float>(std::popcount(sample.transitionFlags));
				sample.features[feature++] = static_cast<float>(sample.continuityGroup);
				if(feature != kMotionDatabaseFeatureCount) return AnimationStatus::InternalError;
				output.push_back(sample);
			}
			return AnimationStatus::Ok;
		}

		void FillContinuityAdjacency(std::vector<MotionDatabaseSourceSample>& samples) {
			std::vector<std::size_t> order(samples.size());
			for(std::size_t index = 0U; index < order.size(); ++index) order[index] = index;
			std::sort(order.begin(), order.end(), [&samples](std::size_t left, std::size_t right) {
				const MotionDatabaseSourceSample& lhs = samples[left];
				const MotionDatabaseSourceSample& rhs = samples[right];
				return std::tie(lhs.continuityGroup, lhs.sourceClipId, lhs.sourceTick, lhs.stableSampleId)
					< std::tie(rhs.continuityGroup, rhs.sourceClipId, rhs.sourceTick, rhs.stableSampleId);
			});
			for(std::size_t orderedIndex = 0U; orderedIndex < order.size(); ++orderedIndex) {
				MotionDatabaseSourceSample& sample = samples[order[orderedIndex]];
				if(sample.continuityGroup == 0U) continue;
				if(sample.continuityPredecessorId == 0U && orderedIndex != 0U) {
					const MotionDatabaseSourceSample& previous = samples[order[orderedIndex - 1U]];
					if(previous.continuityGroup == sample.continuityGroup) {
						sample.continuityPredecessorId = previous.stableSampleId;
					}
				}
				if(sample.continuitySuccessorId == 0U && orderedIndex + 1U < order.size()) {
					const MotionDatabaseSourceSample& next = samples[order[orderedIndex + 1U]];
					if(next.continuityGroup == sample.continuityGroup) {
						sample.continuitySuccessorId = next.stableSampleId;
					}
				}
			}
		}
	}

	AnimationStatus ValidateMotionDatabaseAuthoringDocument(
		const MotionDatabaseAuthoringDocument& document) noexcept {
		if(document.schemaVersion != kMotionDatabaseAuthoringSchemaVersion
			|| document.authoringGeneration == 0U || !document.assetId.IsValid()
			|| document.compatibilityFingerprint == 0U || document.samplesPerBlock == 0U
			|| document.samplesPerBlock > kMotionDatabaseSamplesPerBlockDefault
			|| document.sources.size() > kMotionDatabaseMaximumAuthoredSources) {
			return AnimationStatus::InvalidArgument;
		}
		std::vector<std::uint64_t> enabledIds;
		try {
			enabledIds.reserve(document.sources.size());
			std::size_t annotationCount = 0U;
			for(const MotionDatabaseAuthoringSource& source : document.sources) {
				const std::size_t sourceAnnotations = source.contactRanges.size()
					+ source.tagRanges.size() + source.exclusionRanges.size();
				if(sourceAnnotations > kMotionDatabaseMaximumAuthoredAnnotations - annotationCount) {
					return AnimationStatus::HardLimitExceeded;
				}
				annotationCount += sourceAnnotations;
				if(source.sourcePath.empty() || source.sourcePath.size() > 4096U
					|| source.sourceClipId == 0U || source.sourceClipFingerprint == 0U
					|| !std::all_of(source.contactRanges.begin(), source.contactRanges.end(),
						ValidRange<MotionDatabaseContactRange>)
					|| !std::all_of(source.tagRanges.begin(), source.tagRanges.end(),
						ValidRange<MotionDatabaseTagRange>)
					|| !std::all_of(source.exclusionRanges.begin(), source.exclusionRanges.end(),
						ValidRange<MotionDatabaseExclusionRange>)) {
					return AnimationStatus::InvalidArgument;
				}
				if(source.enabled) enabledIds.push_back(source.sourceClipId);
			}
			std::sort(enabledIds.begin(), enabledIds.end());
			if(std::adjacent_find(enabledIds.begin(), enabledIds.end()) != enabledIds.end()) {
				return AnimationStatus::InvalidArgument;
			}
			return AnimationStatus::Ok;
		} catch(...) {
			return AnimationStatus::InternalError;
		}
	}

	std::uint64_t ComputeMotionDatabaseAuthoringFingerprint(
		const MotionDatabaseAuthoringDocument& document) noexcept {
		// Generation is deliberately excluded: saving an unchanged document creates
		// a new stale-work boundary without changing deterministic cooked content.
		std::uint64_t hash = 14695981039346656037ULL;
		HashValue(hash, document.schemaVersion);
		HashBytes(hash, document.assetId.value.data(), document.assetId.value.size());
		HashValue(hash, document.compatibilityFingerprint);
		HashValue(hash, document.samplesPerBlock);
		HashBytes(hash, document.featureWeights.data(), sizeof(document.featureWeights));
		for(const MotionDatabaseAuthoringSource& source : document.sources) {
			HashValue(hash, source.sourceClipId);
			HashValue(hash, source.sourceClipFingerprint);
			HashBytes(hash, source.sourcePath.data(), source.sourcePath.size());
			HashValue(hash, source.enabled);
			HashValue(hash, source.baseTagMask);
			HashValue(hash, source.baseContactMask);
			for(const auto& range : source.contactRanges) {
				HashValue(hash, range.firstTick); HashValue(hash, range.lastTick); HashValue(hash, range.contactMask);
			}
			for(const auto& range : source.tagRanges) {
				HashValue(hash, range.firstTick); HashValue(hash, range.lastTick); HashValue(hash, range.tagMask);
			}
			for(const auto& range : source.exclusionRanges) {
				HashValue(hash, range.firstTick); HashValue(hash, range.lastTick); HashValue(hash, range.exclusionFlags);
			}
		}
		return hash == 0U ? 1U : hash;
	}

	AnimationStatus LoadMotionDatabaseAuthoringDocument(const std::filesystem::path& path,
		MotionDatabaseAuthoringDocument& output) noexcept {
		MotionDatabaseAuthoringSnapshot snapshot;
		const AnimationStatus status = LoadMotionDatabaseAuthoringSnapshot(path, snapshot);
		if(status == AnimationStatus::Ok) output = std::move(snapshot.document);
		return status;
	}

	AnimationStatus LoadMotionDatabaseAuthoringSnapshot(const std::filesystem::path& path,
		MotionDatabaseAuthoringSnapshot& output) noexcept {
		try {
			const std::shared_ptr<std::mutex> pathLock = GetMotionDatabaseAuthoringPathLock(path);
			std::scoped_lock lock(*pathLock);
			std::string bytes;
			const AnimationStatus readStatus = ReadAuthoringBytes(path, bytes);
			if(readStatus != AnimationStatus::Ok) return readStatus;
			MotionDatabaseAuthoringDocument document;
			const AnimationStatus decodeStatus = DecodeAuthoringDocument(bytes, document);
			if(decodeStatus != AnimationStatus::Ok) return decodeStatus;
			MotionDatabaseAuthoringSnapshot staged;
			staged.document = std::move(document);
			staged.serializedBytes = std::move(bytes);
			output = std::move(staged);
			return AnimationStatus::Ok;
		} catch(...) {
			return AnimationStatus::InternalError;
		}
	}

	AnimationStatus SaveMotionDatabaseAuthoringDocumentAtomically(
		const std::filesystem::path& path,
		const MotionDatabaseAuthoringDocument& document) noexcept {
		return SaveMotionDatabaseAuthoringDocumentAtomically(
			path, document, nullptr, nullptr);
	}

	AnimationStatus SaveMotionDatabaseAuthoringDocumentAtomically(
		const std::filesystem::path& path,
		const MotionDatabaseAuthoringDocument& document,
		const MotionDatabaseAuthoringSnapshot* expectedCurrent,
		MotionDatabaseAuthoringSnapshot* publishedCurrent) noexcept {
		const AnimationStatus validation = ValidateMotionDatabaseAuthoringDocument(document);
		if(validation != AnimationStatus::Ok) return validation;
		if(path.empty()) return AnimationStatus::InvalidArgument;

		std::string candidateBytes;
		const AnimationStatus serializeStatus = SerializeAuthoringDocument(document, candidateBytes);
		if(serializeStatus != AnimationStatus::Ok) return serializeStatus;
		MotionDatabaseAuthoringDocument candidateReadBack;
		if(DecodeAuthoringDocument(candidateBytes, candidateReadBack) != AnimationStatus::Ok
			|| candidateReadBack != document) {
			return AnimationStatus::Rejected;
		}

		// Reject a forged token before taking the path lock. The typed value and raw
		// bytes must describe the same valid document for the bytes to be trusted.
		if(expectedCurrent != nullptr) {
			MotionDatabaseAuthoringDocument expectedReadBack;
			if(DecodeAuthoringDocument(expectedCurrent->serializedBytes, expectedReadBack)
					!= AnimationStatus::Ok
				|| expectedReadBack != expectedCurrent->document
				|| document.authoringGeneration
					<= expectedCurrent->document.authoringGeneration
				|| document.assetId != expectedCurrent->document.assetId
				|| document.compatibilityFingerprint
					!= expectedCurrent->document.compatibilityFingerprint
				|| document.schemaVersion != expectedCurrent->document.schemaVersion) {
				return AnimationStatus::InvalidArgument;
			}
		}

		const std::uint64_t stagingSequence =
			gMotionDatabaseStagingSequence.fetch_add(1U, std::memory_order_relaxed);
		if(stagingSequence == 0U) return AnimationStatus::InternalError;
		std::filesystem::path stagedPath;
		std::error_code error;
		try {
			stagedPath = path.string() + ".staged." + std::to_string(stagingSequence);
			std::optional<MotionDatabaseAuthoringSnapshot> preparedPublishedSnapshot;
			if(publishedCurrent != nullptr) {
				preparedPublishedSnapshot.emplace();
				preparedPublishedSnapshot->document = document;
				preparedPublishedSnapshot->serializedBytes = candidateBytes;
			}
			const std::shared_ptr<std::mutex> pathLock = GetMotionDatabaseAuthoringPathLock(path);
			std::scoped_lock lock(*pathLock);

			// The existence/byte comparison and the complete staged replacement share
			// this lock. Two writers starting from one snapshot can therefore never
			// both pass the compare-and-swap boundary inside this process.
			std::string currentBytes;
			const AnimationStatus currentStatus = ReadAuthoringBytes(path, currentBytes);
			if(expectedCurrent == nullptr) {
				if(currentStatus == AnimationStatus::Ok) return AnimationStatus::StaleHandle;
				if(currentStatus != AnimationStatus::MissingAsset) return currentStatus;
			} else {
				MotionDatabaseAuthoringDocument currentDocument;
				if(currentStatus != AnimationStatus::Ok
					|| currentBytes != expectedCurrent->serializedBytes
					|| DecodeAuthoringDocument(currentBytes, currentDocument)
						!= AnimationStatus::Ok
					|| currentDocument != expectedCurrent->document) {
					return AnimationStatus::StaleHandle;
				}
			}

			if(!path.parent_path().empty()) std::filesystem::create_directories(path.parent_path(), error);
			if(error) return AnimationStatus::InternalError;
			std::filesystem::remove(stagedPath, error);
			{
				std::ofstream stream(stagedPath, std::ios::binary | std::ios::trunc);
				if(!stream) {
					std::filesystem::remove(stagedPath, error);
					return AnimationStatus::InternalError;
				}
				stream.write(candidateBytes.data(),
					static_cast<std::streamsize>(candidateBytes.size()));
				if(!stream) {
					stream.close();
					std::filesystem::remove(stagedPath, error);
					return AnimationStatus::InternalError;
				}
			}
			std::string stagedBytes;
			MotionDatabaseAuthoringDocument stagedDocument;
			if(ReadAuthoringBytes(stagedPath, stagedBytes) != AnimationStatus::Ok
				|| stagedBytes != candidateBytes
				|| DecodeAuthoringDocument(stagedBytes, stagedDocument) != AnimationStatus::Ok
				|| stagedDocument != document) {
				std::filesystem::remove(stagedPath, error);
				return AnimationStatus::Rejected;
			}
			if(!PublishStagedFile(stagedPath, path)) {
				std::filesystem::remove(stagedPath, error);
				return AnimationStatus::InternalError;
			}
			if(publishedCurrent != nullptr) {
				*publishedCurrent = std::move(*preparedPublishedSnapshot);
			}
			return AnimationStatus::Ok;
		} catch(...) {
			if(!stagedPath.empty()) std::filesystem::remove(stagedPath, error);
			return AnimationStatus::InternalError;
		}
	}

	MotionDatabaseAuthoringHistory::MotionDatabaseAuthoringHistory(
		std::size_t maximumEntries)
		: maximumEntries_(std::clamp<std::size_t>(maximumEntries, 1U,
			kMotionDatabaseMaximumAuthoringHistoryEntries)) {
	}

	void MotionDatabaseAuthoringHistory::Reset(
		const MotionDatabaseAuthoringDocument& document) {
		current_ = document;
		greatestGeneration_ = document.authoringGeneration;
		initialized_ = true;
		editBaseline_.reset();
		undo_.clear();
		redo_.clear();
	}

	AnimationStatus MotionDatabaseAuthoringHistory::BeginEdit(
		const MotionDatabaseAuthoringDocument& document) {
		try {
			if(!initialized_) Reset(document);
			if(editBaseline_) return AnimationStatus::Ok;
			if(document != current_) return AnimationStatus::StaleHandle;
			editBaseline_ = document;
			return AnimationStatus::Ok;
		} catch(...) {
			return AnimationStatus::InternalError;
		}
	}

	AnimationStatus MotionDatabaseAuthoringHistory::CommitEdit(
		MotionDatabaseAuthoringDocument& document) {
		if(!initialized_ || !editBaseline_) return AnimationStatus::NotReady;
		if(document.authoringGeneration != current_.authoringGeneration
			|| document.assetId != current_.assetId
			|| document.compatibilityFingerprint != current_.compatibilityFingerprint
			|| document.schemaVersion != current_.schemaVersion) {
			return AnimationStatus::StaleHandle;
		}
		try {
			if(SameAuthoringPayload(document, current_)) {
				document = current_;
				editBaseline_.reset();
				return AnimationStatus::NotReady;
			}
			if(greatestGeneration_ == std::numeric_limits<std::uint64_t>::max()) {
				return AnimationStatus::HardLimitExceeded;
			}
			MotionDatabaseAuthoringDocument candidate = document;
			candidate.authoringGeneration = greatestGeneration_ + 1U;
			const AnimationStatus validation = ValidateMotionDatabaseAuthoringDocument(candidate);
			if(validation != AnimationStatus::Ok) return validation;
			auto stagedUndo = undo_;
			PushBounded(stagedUndo, current_);
			MotionDatabaseAuthoringDocument nextCurrent = candidate;
			document = std::move(candidate);
			current_ = std::move(nextCurrent);
			undo_ = std::move(stagedUndo);
			greatestGeneration_ = current_.authoringGeneration;
			editBaseline_.reset();
			redo_.clear();
			return AnimationStatus::Ok;
		} catch(...) {
			return AnimationStatus::InternalError;
		}
	}

	AnimationStatus MotionDatabaseAuthoringHistory::CancelEdit(
		MotionDatabaseAuthoringDocument& document) noexcept {
		if(!initialized_ || !editBaseline_) return AnimationStatus::NotReady;
		try {
			// The baseline and current document are the same committed value. Copying
			// it back removes every field edit without allocating a new generation.
			document = *editBaseline_;
			editBaseline_.reset();
			return AnimationStatus::Ok;
		} catch(...) {
			return AnimationStatus::InternalError;
		}
	}

	AnimationStatus MotionDatabaseAuthoringHistory::Undo(
		MotionDatabaseAuthoringDocument& document) {
		return RestoreFrom(undo_, redo_, document);
	}

	AnimationStatus MotionDatabaseAuthoringHistory::Redo(
		MotionDatabaseAuthoringDocument& document) {
		return RestoreFrom(redo_, undo_, document);
	}

	bool MotionDatabaseAuthoringHistory::IsEditActive() const noexcept {
		return editBaseline_.has_value();
	}

	bool MotionDatabaseAuthoringHistory::CanUndo() const noexcept {
		return initialized_ && !editBaseline_ && !undo_.empty();
	}

	bool MotionDatabaseAuthoringHistory::CanRedo() const noexcept {
		return initialized_ && !editBaseline_ && !redo_.empty();
	}

	const MotionDatabaseAuthoringDocument& MotionDatabaseAuthoringHistory::Current() const noexcept {
		return current_;
	}

	AnimationStatus MotionDatabaseAuthoringHistory::RestoreFrom(
		std::vector<std::shared_ptr<const MotionDatabaseAuthoringDocument>>& source,
		std::vector<std::shared_ptr<const MotionDatabaseAuthoringDocument>>& destination,
		MotionDatabaseAuthoringDocument& document) {
		if(!initialized_ || editBaseline_ || source.empty()) return AnimationStatus::NotReady;
		if(greatestGeneration_ == std::numeric_limits<std::uint64_t>::max()) {
			return AnimationStatus::HardLimitExceeded;
		}
		try {
			auto stagedSource = source;
			auto stagedDestination = destination;
			MotionDatabaseAuthoringDocument restored = *stagedSource.back();
			restored.authoringGeneration = greatestGeneration_ + 1U;
			const AnimationStatus validation = ValidateMotionDatabaseAuthoringDocument(restored);
			if(validation != AnimationStatus::Ok) return validation;
			PushBounded(stagedDestination, current_);
			stagedSource.pop_back();
			MotionDatabaseAuthoringDocument nextCurrent = restored;
			document = std::move(restored);
			current_ = std::move(nextCurrent);
			source = std::move(stagedSource);
			destination = std::move(stagedDestination);
			greatestGeneration_ = current_.authoringGeneration;
			return AnimationStatus::Ok;
		} catch(...) {
			return AnimationStatus::InternalError;
		}
	}

	void MotionDatabaseAuthoringHistory::PushBounded(
		std::vector<std::shared_ptr<const MotionDatabaseAuthoringDocument>>& stack,
		const MotionDatabaseAuthoringDocument& document) {
		auto immutable = std::make_shared<const MotionDatabaseAuthoringDocument>(document);
		if(stack.size() == maximumEntries_) stack.erase(stack.begin());
		stack.push_back(std::move(immutable));
	}

	AnimationStatus BuildMotionDatabaseCompileInput(
		const MotionDatabaseAuthoringDocument& document,
		std::span<const MotionDatabaseSourceClipInput> resolvedClips,
		MotionDatabaseCompileInput& output) noexcept {
		const AnimationStatus validation = ValidateMotionDatabaseAuthoringDocument(document);
		if(validation != AnimationStatus::Ok) return validation;
		try {
			MotionDatabaseCompileInput staged;
			staged.assetId = document.assetId;
			staged.compatibilityFingerprint = document.compatibilityFingerprint;
			staged.sourceFingerprint = ComputeMotionDatabaseAuthoringFingerprint(document);
			staged.authoringGeneration = document.authoringGeneration;
			staged.samplesPerBlock = document.samplesPerBlock;
			for(const MotionDatabaseAuthoringSource& authored : document.sources) {
				if(!authored.enabled) continue;
				const auto resolved = std::find_if(resolvedClips.begin(), resolvedClips.end(),
					[&authored](const auto& clip) { return clip.sourceClipId == authored.sourceClipId; });
				if(resolved == resolvedClips.end()
					|| resolved->sourceClipFingerprint != authored.sourceClipFingerprint) {
					return AnimationStatus::IncompatibleAsset;
				}
				MotionDatabaseSourceClipInput clip = *resolved;
				clip.baseTagMask = authored.baseTagMask;
				clip.baseContactMask = authored.baseContactMask;
				clip.contactRanges = authored.contactRanges;
				clip.tagRanges = authored.tagRanges;
				clip.exclusionRanges = authored.exclusionRanges;
				staged.sourceClips.push_back(std::move(clip));
			}
			output = std::move(staged);
			return AnimationStatus::Ok;
		} catch(...) {
			return AnimationStatus::InternalError;
		}
	}

	struct MotionDatabaseCompiler::Job {
		std::uint64_t sequence = 0U;
		MotionDatabaseCompileInput input{};
		std::atomic_bool cancelled = false;
		std::atomic_bool complete = false;
		std::atomic_uint32_t completedSteps = 0U;
		std::atomic_uint32_t totalSteps = kCompileProgressSteps;
		std::atomic<MotionDatabaseBuildStage> stage = MotionDatabaseBuildStage::Queued;
		std::mutex resultMutex;
		AnimationStatus status = AnimationStatus::NotReady;
		MotionDatabaseAsset asset{};
		MotionDatabaseCompileReport report{};
		std::filesystem::path stagedPath;
	};

	AnimationStatus CompileMotionDatabase(const MotionDatabaseCompileInput& input,
		MotionDatabaseAsset& output, MotionDatabaseCompileReport& report,
		MotionDatabaseCompileCancellation cancellation) noexcept {
		MotionDatabaseCompileReport stagedReport;
		cancellation.ReportProgress(0U, kCompileProgressSteps);
		if(!input.assetId.IsValid() || input.compatibilityFingerprint == 0U
			|| input.sourceFingerprint == 0U || input.samplesPerBlock == 0U
			|| input.samplesPerBlock > kMotionDatabaseSamplesPerBlockDefault
			|| input.workerSourceResolver) {
			return AnimationStatus::InvalidArgument;
		}
		std::uint64_t totalSamples = input.samples.size();
		if(totalSamples > kMotionDatabaseMaximumSamples) {
			return AnimationStatus::HardLimitExceeded;
		}
		for(const MotionDatabaseSourceClipInput& clip : input.sourceClips) {
			if(clip.samples.size() > kMotionDatabaseMaximumSamples - totalSamples) {
				return AnimationStatus::HardLimitExceeded;
			}
			totalSamples += clip.samples.size();
		}
		if(totalSamples > kMotionDatabaseMaximumSamples) return AnimationStatus::HardLimitExceeded;
		if(cancellation.IsCancellationRequested()) return AnimationStatus::Cancelled;
		try {
			std::vector<MotionDatabaseSourceSample> source = input.samples;
			std::vector<MotionDatabaseSourceClipInput> clips = input.sourceClips;
			std::sort(clips.begin(), clips.end(), [](const auto& left, const auto& right) {
				if(left.sourceClipId != right.sourceClipId) return left.sourceClipId < right.sourceClipId;
				return left.sourceClipFingerprint < right.sourceClipFingerprint;
			});
			std::uint64_t priorClipId = 0U;
			for(const MotionDatabaseSourceClipInput& clip : clips) {
				if(priorClipId != 0U && clip.sourceClipId == priorClipId) {
					return AnimationStatus::InvalidArgument;
				}
				priorClipId = clip.sourceClipId;
				const AnimationStatus extractionStatus = AppendExtractedClipSamples(
					clip, source, cancellation);
				if(extractionStatus != AnimationStatus::Ok) return extractionStatus;
			}
			if(cancellation.IsCancellationRequested()) return AnimationStatus::Cancelled;
			cancellation.ReportProgress(1U, kCompileProgressSteps);

			FillContinuityAdjacency(source);
			std::sort(source.begin(), source.end(), [](const auto& left, const auto& right) {
				return left.stableSampleId < right.stableSampleId;
			});
			std::array<float, kMotionDatabaseFeatureCount> minima{};
			std::array<float, kMotionDatabaseFeatureCount> maxima{};
			minima.fill(std::numeric_limits<float>::infinity());
			maxima.fill(-std::numeric_limits<float>::infinity());
			std::uint64_t priorId = 0U;
			for(std::size_t sampleIndex = 0U; sampleIndex < source.size(); ++sampleIndex) {
				if((sampleIndex % kCancellationCheckInterval) == 0U
					&& cancellation.IsCancellationRequested()) {
					return AnimationStatus::Cancelled;
				}
				const MotionDatabaseSourceSample& sample = source[sampleIndex];
				if(sample.stableSampleId == 0U || sample.sourceClipId == 0U || sample.sourceTick < 0
					|| !IsFinite(sample) || (priorId != 0U && priorId == sample.stableSampleId)) {
					return AnimationStatus::InvalidArgument;
				}
				priorId = sample.stableSampleId;
				for(std::size_t feature = 0U; feature < kMotionDatabaseFeatureCount; ++feature) {
					minima[feature] = std::min(minima[feature], sample.features[feature]);
					maxima[feature] = std::max(maxima[feature], sample.features[feature]);
				}
			}
			cancellation.ReportProgress(2U, kCompileProgressSteps);

			MotionDatabaseAsset staged;
			staged.assetId = input.assetId;
			staged.compatibilityFingerprint = input.compatibilityFingerprint;
			staged.sourceFingerprint = input.sourceFingerprint;
			staged.storageLayout.samplesPerBlock = input.samplesPerBlock;
			for(std::size_t feature = 0U; feature < kMotionDatabaseFeatureCount; ++feature) {
				staged.quantizationOffsets[feature] = source.empty() ? 0.0F : minima[feature];
				const float range = source.empty() ? 1.0F : maxima[feature] - minima[feature];
				staged.quantizationScales[feature] = std::max(range / 65534.0F, 1.0e-6F);
			}
			staged.samples.reserve(source.size());
			for(std::size_t sampleIndex = 0U; sampleIndex < source.size(); ++sampleIndex) {
				if((sampleIndex % kCancellationCheckInterval) == 0U
					&& cancellation.IsCancellationRequested()) {
					return AnimationStatus::Cancelled;
				}
				const MotionDatabaseSourceSample& sourceSample = source[sampleIndex];
				MotionDatabaseSample cooked;
				cooked.stableSampleId = sourceSample.stableSampleId;
				cooked.sourceClipId = sourceSample.sourceClipId;
				cooked.sourceTick = sourceSample.sourceTick;
				cooked.tagMask = sourceSample.tagMask;
				cooked.contactMask = sourceSample.contactMask;
				cooked.phaseQuantized = sourceSample.phaseQuantized;
				cooked.transitionFlags = sourceSample.transitionFlags;
				cooked.continuityGroup = sourceSample.continuityGroup;
				cooked.exclusionFlags = sourceSample.exclusionFlags;
				cooked.continuityPredecessorId = sourceSample.continuityPredecessorId;
				cooked.continuitySuccessorId = sourceSample.continuitySuccessorId;
				for(std::size_t feature = 0U; feature < kMotionDatabaseFeatureCount; ++feature) {
					cooked.features.values[feature] = Quantize(sourceSample.features[feature],
						staged.quantizationScales[feature], staged.quantizationOffsets[feature]);
				}
				staged.samples.push_back(cooked);
			}
			cancellation.ReportProgress(3U, kCompileProgressSteps);
			if(cancellation.IsCancellationRequested()) return AnimationStatus::Cancelled;
			if(const AnimationStatus layoutStatus = RebuildMotionDatabaseStorageMetadata(staged);
				layoutStatus != AnimationStatus::Ok) {
				return layoutStatus;
			}
			staged.featureSchemaFingerprint = ComputeMotionDatabaseSchemaFingerprint(staged);
			if(const AnimationStatus status = ValidateMotionDatabaseAsset(staged);
				status != AnimationStatus::Ok) {
				return status;
			}
			if(cancellation.IsCancellationRequested()) return AnimationStatus::Cancelled;
			cancellation.ReportProgress(4U, kCompileProgressSteps);

			stagedReport.sourceSamples = static_cast<std::uint32_t>(input.samples.size());
			stagedReport.extractedSamples = static_cast<std::uint32_t>(
				source.size() - input.samples.size());
			stagedReport.publishedSamples = static_cast<std::uint32_t>(source.size());
			stagedReport.residentBlocks = static_cast<std::uint32_t>(staged.residentBlocks.size());
			stagedReport.contentFingerprint = ComputeMotionDatabaseContentFingerprint(staged);
			stagedReport.diagnostic =
				"Compiled deterministic trajectory/pose/velocity/facing/contact/tag motion features.";
			output = std::move(staged);
			report = std::move(stagedReport);
			cancellation.ReportProgress(5U, kCompileProgressSteps);
			return AnimationStatus::Ok;
		} catch(const std::bad_alloc&) {
			return AnimationStatus::InternalError;
		} catch(...) {
			return AnimationStatus::InternalError;
		}
	}

	MotionDatabaseCompiler::MotionDatabaseCompiler(
		std::shared_ptr<MotionDatabaseCompileExecutor> executor,
		std::size_t maximumPendingJobs)
		: executor_(std::move(executor))
		, maximumPendingJobs_(std::min<std::size_t>(maximumPendingJobs, 64U)) {}

	AnimationStatus MotionDatabaseCompiler::RequestBuild(const MotionDatabaseCompileInput& input) {
		if(executor_ == nullptr || maximumPendingJobs_ == 0U) {
			return AnimationStatus::InvalidArgument;
		}
		std::shared_ptr<Job> job;
		try {
			job = std::make_shared<Job>();
			job->input = input;
		} catch(...) {
			return AnimationStatus::InternalError;
		}
		{
			std::scoped_lock lock(mutex_);
			if(jobs_.size() >= maximumPendingJobs_ || newestSequence_ == UINT64_MAX) {
				return AnimationStatus::Rejected;
			}
			job->sequence = ++newestSequence_;
			jobs_.push_back(job);
			lastProgress_ = {job->sequence, input.authoringGeneration, 0U,
				kCompileProgressSteps, MotionDatabaseBuildStage::Queued, AnimationStatus::Queued};
		}
		try {
			// The worker captures only its copied job. The compiler can be destroyed
			// after cancellation without leaving a callback that dereferences `this`.
			executor_->Enqueue([job]() {
				job->stage.store(MotionDatabaseBuildStage::Building, std::memory_order_release);
				MotionDatabaseAsset asset;
				MotionDatabaseCompileReport report;
				MotionDatabaseCompileInput preparedInput;
				AnimationStatus status = AnimationStatus::Ok;
				try {
					preparedInput = job->input;
					if(job->cancelled.load(std::memory_order_acquire)) {
						status = AnimationStatus::Cancelled;
					} else if(preparedInput.workerSourceResolver) {
						std::vector<MotionDatabaseSourceClipInput> resolved;
						status = preparedInput.workerSourceResolver(resolved, job->cancelled);
						if(status == AnimationStatus::Ok) preparedInput.sourceClips = std::move(resolved);
						preparedInput.workerSourceResolver = {};
					}
				} catch(...) {
					status = AnimationStatus::InternalError;
				}
				if(job->cancelled.load(std::memory_order_acquire)) status = AnimationStatus::Cancelled;
				if(status == AnimationStatus::Ok) status = CompileMotionDatabase(preparedInput, asset, report,
					MotionDatabaseCompileCancellation{&job->cancelled,
						&job->completedSteps, &job->totalSteps});
				std::filesystem::path stagedPath;
				if(status == AnimationStatus::Ok && !job->input.publicationPath.empty()
					&& !job->cancelled.load(std::memory_order_acquire)) {
					status = StageMotionDatabaseAsset(job->input.publicationPath,
						job->sequence, asset, stagedPath);
				}
				if(status == AnimationStatus::Ok) {
					job->completedSteps.store(kCompileProgressSteps, std::memory_order_release);
				}
				if((status != AnimationStatus::Ok
					|| job->cancelled.load(std::memory_order_acquire)) && !stagedPath.empty()) {
					std::error_code cleanupError;
					std::filesystem::remove(stagedPath, cleanupError);
					stagedPath.clear();
				}
				{
					std::scoped_lock lock(job->resultMutex);
					job->status = job->cancelled.load(std::memory_order_acquire)
						? AnimationStatus::Cancelled : status;
					if(job->status == AnimationStatus::Ok) {
						job->asset = std::move(asset);
						job->report = std::move(report);
						job->stagedPath = std::move(stagedPath);
					}
				}
				job->stage.store(job->status == AnimationStatus::Ok
					? MotionDatabaseBuildStage::ReadyToPublish
					: (job->status == AnimationStatus::Cancelled
						? MotionDatabaseBuildStage::Cancelled : MotionDatabaseBuildStage::Rejected),
					std::memory_order_release);
				job->complete.store(true, std::memory_order_release);
			});
		} catch(...) {
			std::scoped_lock lock(job->resultMutex);
			job->status = AnimationStatus::InternalError;
			job->stage.store(MotionDatabaseBuildStage::Rejected, std::memory_order_release);
			job->complete.store(true, std::memory_order_release);
		}
		return AnimationStatus::Queued;
	}

	void MotionDatabaseCompiler::CancelPending() noexcept {
		std::scoped_lock lock(mutex_);
		for(const std::shared_ptr<Job>& job : jobs_) {
			job->cancelled.store(true, std::memory_order_release);
		}
	}

	AnimationStatus MotionDatabaseCompiler::PollAndPublish(AnimationGuid expectedAssetId,
		AnimationCompatibilityFingerprint expectedCompatibilityFingerprint,
		std::uint64_t expectedAuthoringGeneration,
		MotionDatabasePublicationValidator publicationValidator) {
		std::scoped_lock lock(mutex_);
		for(auto iterator = jobs_.begin(); iterator != jobs_.end();) {
			const std::shared_ptr<Job>& job = *iterator;
			const bool cancelled = job->cancelled.load(std::memory_order_acquire);
			if(!job->complete.load(std::memory_order_acquire) && !cancelled) {
				++iterator;
				continue;
			}
			AnimationStatus status = AnimationStatus::NotReady;
			if(cancelled) {
				status = AnimationStatus::Cancelled;
			} else if(job->sequence != newestSequence_ || job->input.assetId != expectedAssetId
				|| job->input.compatibilityFingerprint != expectedCompatibilityFingerprint
				|| (expectedAuthoringGeneration != 0U
					&& job->input.authoringGeneration != expectedAuthoringGeneration)) {
				status = AnimationStatus::StaleHandle;
			} else {
				std::scoped_lock resultLock(job->resultMutex);
				if(job->status != AnimationStatus::Ok) {
					status = job->status;
				} else if(nextGeneration_ == 0U) {
					status = AnimationStatus::InternalError;
				} else {
					try {
						MotionDatabaseAsset staged = job->asset;
						staged.runtimeGeneration = nextGeneration_;
						MotionDatabasePublishedRecord candidate{
							nextGeneration_, job->report.contentFingerprint,
							std::make_shared<const MotionDatabaseAsset>(std::move(staged))};
						status = publicationValidator
							? publicationValidator(*candidate.asset, job->stagedPath)
							: AnimationStatus::Ok;
						if(status == AnimationStatus::Ok) {
							published_ = std::move(candidate);
							++nextGeneration_;
						}
					} catch(...) {
						status = AnimationStatus::InternalError;
					}
				}
			}
			if(status != AnimationStatus::Ok && !job->stagedPath.empty()) {
				std::error_code cleanupError;
				std::filesystem::remove(job->stagedPath, cleanupError);
			}
			lastProgress_ = {job->sequence, job->input.authoringGeneration,
				job->completedSteps.load(std::memory_order_acquire),
				job->totalSteps.load(std::memory_order_acquire),
				status == AnimationStatus::Ok ? MotionDatabaseBuildStage::Published
					: (status == AnimationStatus::Cancelled
						? MotionDatabaseBuildStage::Cancelled : MotionDatabaseBuildStage::Rejected),
				status};
			iterator = jobs_.erase(iterator);
			return status;
		}
		return AnimationStatus::NotReady;
	}

	std::optional<MotionDatabasePublishedRecord> MotionDatabaseCompiler::GetPublished() const {
		std::scoped_lock lock(mutex_);
		return published_;
	}

	MotionDatabaseBuildProgress MotionDatabaseCompiler::GetBuildProgress() const noexcept {
		std::scoped_lock lock(mutex_);
		if(jobs_.empty()) return lastProgress_;
		const std::shared_ptr<Job>& job = jobs_.back();
		MotionDatabaseBuildProgress progress;
		progress.sequence = job->sequence;
		progress.authoringGeneration = job->input.authoringGeneration;
		progress.completedSteps = job->completedSteps.load(std::memory_order_acquire);
		progress.totalSteps = job->totalSteps.load(std::memory_order_acquire);
		progress.stage = job->stage.load(std::memory_order_acquire);
		progress.status = job->complete.load(std::memory_order_acquire)
			? job->status : AnimationStatus::Queued;
		return progress;
	}

	AnimationStatus PublishMotionDatabaseAssetAtomically(
		const std::filesystem::path& outputPath,
		const MotionDatabaseAsset& asset,
		std::uint64_t expectedAuthoringGeneration,
		std::uint64_t currentAuthoringGeneration) noexcept {
		if(expectedAuthoringGeneration == 0U
			|| expectedAuthoringGeneration != currentAuthoringGeneration) {
			return expectedAuthoringGeneration != currentAuthoringGeneration
				? AnimationStatus::StaleHandle : AnimationStatus::InvalidArgument;
		}
		std::filesystem::path stagedPath;
		const AnimationStatus staged = StageMotionDatabaseAsset(
			outputPath, expectedAuthoringGeneration, asset, stagedPath);
		if(staged != AnimationStatus::Ok) {
			std::error_code cleanupError;
			if(!stagedPath.empty()) std::filesystem::remove(stagedPath, cleanupError);
			return staged;
		}
		if(PublishStagedFile(stagedPath, outputPath)) return AnimationStatus::Ok;
		std::error_code error;
		std::filesystem::remove(stagedPath, error);
		return AnimationStatus::InternalError;
	}

	MotionDatabaseAuthoringSession::MotionDatabaseAuthoringSession(
		std::shared_ptr<MotionDatabaseCompileExecutor> executor,
		std::size_t maximumPendingJobs)
		: compiler_(std::move(executor), maximumPendingJobs) {}

	AnimationStatus MotionDatabaseAuthoringSession::RequestBuild(
		const MotionDatabaseAuthoringDocument& document,
		std::span<const MotionDatabaseSourceClipInput> resolvedClips,
		std::filesystem::path outputPath) {
		MotionDatabaseCompileInput input;
		const AnimationStatus inputStatus = BuildMotionDatabaseCompileInput(
			document, resolvedClips, input);
		if(inputStatus != AnimationStatus::Ok || outputPath.extension() != ".temotiondb") {
			return inputStatus == AnimationStatus::Ok
				? AnimationStatus::InvalidArgument : inputStatus;
		}
		input.publicationPath = outputPath;
		const AnimationStatus queued = compiler_.RequestBuild(input);
		if(queued != AnimationStatus::Queued) return queued;
		std::scoped_lock lock(mutex_);
		outputPath_ = std::move(outputPath);
		requestedAuthoringGeneration_ = document.authoringGeneration;
		queryPreview_.reset();
		return queued;
	}

	AnimationStatus MotionDatabaseAuthoringSession::RequestBuild(
		const MotionDatabaseAuthoringDocument& document,
		MotionDatabaseWorkerSourceResolver workerSourceResolver,
		std::filesystem::path outputPath) {
		const AnimationStatus validation = ValidateMotionDatabaseAuthoringDocument(document);
		if(validation != AnimationStatus::Ok) return validation;
		if(!workerSourceResolver || outputPath.extension() != ".temotiondb") {
			return AnimationStatus::InvalidArgument;
		}
		MotionDatabaseCompileInput input;
		input.assetId = document.assetId;
		input.compatibilityFingerprint = document.compatibilityFingerprint;
		input.sourceFingerprint = ComputeMotionDatabaseAuthoringFingerprint(document);
		input.authoringGeneration = document.authoringGeneration;
		input.samplesPerBlock = document.samplesPerBlock;
		input.publicationPath = outputPath;
		input.workerSourceResolver = std::move(workerSourceResolver);
		const AnimationStatus queued = compiler_.RequestBuild(input);
		if(queued != AnimationStatus::Queued) return queued;
		std::scoped_lock lock(mutex_);
		outputPath_ = std::move(outputPath);
		requestedAuthoringGeneration_ = document.authoringGeneration;
		queryPreview_.reset();
		return queued;
	}

	void MotionDatabaseAuthoringSession::CancelBuild() noexcept {
		compiler_.CancelPending();
	}

	AnimationStatus MotionDatabaseAuthoringSession::Poll(
		const MotionDatabaseAuthoringDocument& currentDocument) {
		std::filesystem::path outputPath;
		std::uint64_t expectedGeneration = 0U;
		{
			std::scoped_lock lock(mutex_);
			outputPath = outputPath_;
			expectedGeneration = requestedAuthoringGeneration_;
		}
		return compiler_.PollAndPublish(currentDocument.assetId,
			currentDocument.compatibilityFingerprint,
			currentDocument.authoringGeneration,
			[outputPath, expectedGeneration, currentGeneration = currentDocument.authoringGeneration](
				const MotionDatabaseAsset& asset, const std::filesystem::path& stagedPath) {
				if(expectedGeneration != currentGeneration) return AnimationStatus::StaleHandle;
				if(stagedPath.empty() || stagedPath.parent_path() != outputPath.parent_path()
					|| ValidateMotionDatabaseAsset(asset) != AnimationStatus::Ok) {
					return AnimationStatus::Rejected;
				}
				return PublishStagedFile(stagedPath, outputPath)
					? AnimationStatus::Ok : AnimationStatus::InternalError;
			});
	}

	MotionDatabaseBuildProgress MotionDatabaseAuthoringSession::GetBuildProgress() const noexcept {
		return compiler_.GetBuildProgress();
	}

	std::optional<MotionDatabasePublishedRecord> MotionDatabaseAuthoringSession::GetPublished() const {
		return compiler_.GetPublished();
	}

	AnimationStatus MotionDatabaseAuthoringSession::RunQueryPreview(MotionMatchQuery query,
		std::span<const std::uint64_t> excludedSampleIds) {
		const std::optional<MotionDatabasePublishedRecord> published = compiler_.GetPublished();
		if(!published || !published->asset) return AnimationStatus::NotReady;
		if(query.database.assetId != published->asset->assetId
			|| (query.database.generation != 0U
				&& query.database.generation != published->generation)) {
			return AnimationStatus::StaleHandle;
		}
		MotionDatabaseQueryPreviewState staged;
		try {
			staged.databaseGeneration = published->generation;
			staged.excludedSampleIds.assign(excludedSampleIds.begin(), excludedSampleIds.end());
			std::sort(staged.excludedSampleIds.begin(), staged.excludedSampleIds.end());
			staged.excludedSampleIds.erase(std::unique(staged.excludedSampleIds.begin(),
				staged.excludedSampleIds.end()), staged.excludedSampleIds.end());
			staged.query = query;
			staged.query.database.generation = published->generation;
			staged.query.excludedSampleIds = staged.excludedSampleIds;
			const AnimationStatus status = ScoreMotionMatchCpu(
				*published->asset, staged.query, staged.result);
			if(status != AnimationStatus::Ok) return status;
			std::scoped_lock lock(mutex_);
			queryPreview_ = std::move(staged);
			return AnimationStatus::Ok;
		} catch(...) {
			return AnimationStatus::InternalError;
		}
	}

	std::optional<MotionDatabaseQueryPreviewState>
	MotionDatabaseAuthoringSession::GetQueryPreview() const {
		std::scoped_lock lock(mutex_);
		return queryPreview_;
	}
}
