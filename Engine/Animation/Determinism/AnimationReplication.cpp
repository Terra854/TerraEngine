// AnimationReplication.cpp - Explicit bounded little-endian animation codec.

#include "Animation/Determinism/AnimationReplication.h"

#include <algorithm>
#include <cmath>
#include <limits>
#include <new>
#include <stdexcept>

namespace TerraEngine::Animation {
	namespace {
		constexpr std::size_t kByteCountOffset = 4U;
		constexpr std::size_t kPayloadChecksumOffset = 92U;
		constexpr std::size_t kPayloadOffset = 100U;
		constexpr std::uint64_t kFnvOffset = 14695981039346656037ULL;
		constexpr std::uint64_t kFnvPrime = 1099511628211ULL;

		class ByteWriter final {
		public:
			explicit ByteWriter(std::vector<std::uint8_t>& output) noexcept
				: output_(output)
			{
			}

			void U8(std::uint8_t value) { output_.push_back(value); }

			void U16(std::uint16_t value)
			{
				for(std::uint32_t index = 0U; index != 2U; ++index) {
					U8(static_cast<std::uint8_t>(value >> (index * 8U)));
				}
			}

			void U32(std::uint32_t value)
			{
				for(std::uint32_t index = 0U; index != 4U; ++index) {
					U8(static_cast<std::uint8_t>(value >> (index * 8U)));
				}
			}

			void U64(std::uint64_t value)
			{
				for(std::uint32_t index = 0U; index != 8U; ++index) {
					U8(static_cast<std::uint8_t>(value >> (index * 8U)));
				}
			}

		private:
			std::vector<std::uint8_t>& output_;
		};

		class ByteReader final {
		public:
			explicit ByteReader(std::span<const std::uint8_t> input) noexcept
				: input_(input)
			{
			}

			[[nodiscard]] bool U8(std::uint8_t& value) noexcept
			{
				if(cursor_ >= input_.size()) {
					return false;
				}
				value = input_[cursor_++];
				return true;
			}

			[[nodiscard]] bool U16(std::uint16_t& value) noexcept
			{
				if(Remaining() < 2U) {
					return false;
				}
				value = static_cast<std::uint16_t>(input_[cursor_])
					| (static_cast<std::uint16_t>(input_[cursor_ + 1U]) << 8U);
				cursor_ += 2U;
				return true;
			}

			[[nodiscard]] bool U32(std::uint32_t& value) noexcept
			{
				if(Remaining() < 4U) {
					return false;
				}
				value = 0U;
				for(std::uint32_t index = 0U; index != 4U; ++index) {
					value |= static_cast<std::uint32_t>(input_[cursor_ + index])
						<< (index * 8U);
				}
				cursor_ += 4U;
				return true;
			}

			[[nodiscard]] bool U64(std::uint64_t& value) noexcept
			{
				if(Remaining() < 8U) {
					return false;
				}
				value = 0U;
				for(std::uint32_t index = 0U; index != 8U; ++index) {
					value |= static_cast<std::uint64_t>(input_[cursor_ + index])
						<< (index * 8U);
				}
				cursor_ += 8U;
				return true;
			}

			[[nodiscard]] std::size_t Cursor() const noexcept { return cursor_; }
			[[nodiscard]] std::size_t Remaining() const noexcept
			{
				return cursor_ <= input_.size() ? input_.size() - cursor_ : 0U;
			}

		private:
			std::span<const std::uint8_t> input_;
			std::size_t cursor_ = 0U;
		};

		[[nodiscard]] std::uint64_t PayloadChecksum(
			std::span<const std::uint8_t> payload) noexcept
		{
			std::uint64_t checksum = kFnvOffset;
			for(const std::uint8_t byte : payload) {
				checksum = (checksum ^ byte) * kFnvPrime;
			}
			return checksum;
		}

		void PatchU32(
			std::vector<std::uint8_t>& bytes,
			std::size_t offset,
			std::uint32_t value) noexcept
		{
			for(std::uint32_t index = 0U; index != 4U; ++index) {
				bytes[offset + index] = static_cast<std::uint8_t>(value >> (index * 8U));
			}
		}

		void PatchU64(
			std::vector<std::uint8_t>& bytes,
			std::size_t offset,
			std::uint64_t value) noexcept
		{
			for(std::uint32_t index = 0U; index != 8U; ++index) {
				bytes[offset + index] = static_cast<std::uint8_t>(value >> (index * 8U));
			}
		}

		[[nodiscard]] bool IsSupported(AnimationNetworkPacketKind value) noexcept
		{
			return value >= AnimationNetworkPacketKind::AuthoritativeState
				&& value <= AnimationNetworkPacketKind::DivergenceReport;
		}

		[[nodiscard]] bool IsSupported(AnimationDeterministicStateKind value) noexcept
		{
			return value >= AnimationDeterministicStateKind::Controller
				&& value <= AnimationDeterministicStateKind::Physics;
		}

		[[nodiscard]] bool IsSupported(AnimationRollbackValueType value) noexcept
		{
			return value >= AnimationRollbackValueType::Trigger
				&& value <= AnimationRollbackValueType::Transform;
		}

		[[nodiscard]] bool IsSupported(AnimationReplicatedCommandKind value) noexcept
		{
			return value >= AnimationReplicatedCommandKind::Play
				&& value <= AnimationReplicatedCommandKind::ResetTrigger;
		}

		[[nodiscard]] bool IsSupported(AnimationRollbackRagdollState value) noexcept
		{
			return value >= AnimationRollbackRagdollState::Inactive
				&& value <= AnimationRollbackRagdollState::Rejected;
		}

		[[nodiscard]] bool IsFinite(std::uint32_t word) noexcept
		{
			return std::isfinite(DecodeAnimationNetworkFloat(word));
		}

		template<std::size_t Count>
		[[nodiscard]] bool AreFinite(const std::array<std::uint32_t, Count>& words) noexcept
		{
			return std::all_of(words.begin(), words.end(), IsFinite);
		}

		[[nodiscard]] std::size_t ParameterWordCount(AnimationRollbackValueType type) noexcept
		{
			switch(type) {
			case AnimationRollbackValueType::Trigger: return 0U;
			case AnimationRollbackValueType::Boolean: return 1U;
			case AnimationRollbackValueType::Integer: return 1U;
			case AnimationRollbackValueType::Float: return 1U;
			case AnimationRollbackValueType::Vector3: return 3U;
			case AnimationRollbackValueType::Quaternion: return 4U;
			case AnimationRollbackValueType::Transform: return 10U;
			}
			return 0U;
		}

		[[nodiscard]] bool ValidParameter(
			const AnimationParameterRollbackState& value) noexcept
		{
			if(value.parameterId == 0U || !IsSupported(value.type)) {
				return false;
			}
			const std::size_t used = ParameterWordCount(value.type);
			if(value.type == AnimationRollbackValueType::Boolean && value.valueWords[0] > 1U) {
				return false;
			}
			if(value.type != AnimationRollbackValueType::Trigger
				&& value.type != AnimationRollbackValueType::Boolean
				&& value.type != AnimationRollbackValueType::Integer) {
				for(std::size_t index = 0U; index < used; ++index) {
					if(!IsFinite(value.valueWords[index])) {
						return false;
					}
				}
			}
			return std::all_of(value.valueWords.begin() + used, value.valueWords.end(),
				[](std::uint32_t word) { return word == 0U; });
		}

		[[nodiscard]] bool ValidRoot(
			const AnimationReplicatedRootMotionRecord& value) noexcept
		{
			return value.state.accumulationId != 0U
				&& value.state.sourceEndTick >= value.state.sourceBeginTick
				&& value.state.generation != 0U
				&& AreFinite(value.state.localTranslation.words)
				&& AreFinite(value.state.worldTranslation.words)
				&& IsFinite(value.state.localYawBits)
				&& IsFinite(value.state.worldYawBits);
		}

		[[nodiscard]] bool ValidMotion(
			const AnimationReplicatedMotionMatchRecord& value) noexcept
		{
			return value.state.stableNodeId != 0U
				&& value.state.databaseFingerprint != 0U
				&& value.state.databaseGeneration != 0U
				&& value.state.querySequence != 0U
				&& (value.state.usedFallback || value.state.candidateId != 0U)
				&& value.state.selectionHash != 0U
				&& IsFinite(value.state.totalScoreBits);
		}

		[[nodiscard]] bool ValidRagdoll(
			const AnimationReplicatedRagdollRecord& value) noexcept
		{
			return value.state.ragdollInstanceId != 0U && value.state.bodyId != 0U
				&& value.state.stableJointId != 0U && IsSupported(value.state.lifecycle)
				&& value.state.worldGeneration != 0U && value.state.profileGeneration != 0U
				&& value.state.skeletonGeneration != 0U
				&& value.state.physicsPublicationGeneration != 0U
				&& value.state.bodyGeneration != 0U && value.state.fixedStep >= 0
				&& AreFinite(value.state.modelTransform.translation.words)
				&& AreFinite(value.state.modelTransform.rotation.words)
				&& AreFinite(value.state.modelTransform.scale.words)
				&& AreFinite(value.state.linearVelocity.words)
				&& AreFinite(value.state.angularVelocity.words)
				&& IsFinite(value.state.blendWeightBits);
		}

		template<typename Value, typename Less, typename Validator>
		[[nodiscard]] bool StrictlyOrderedAndValid(
			const std::vector<Value>& values,
			Less less,
			Validator validator) noexcept
		{
			for(std::size_t index = 0U; index < values.size(); ++index) {
				if(!validator(values[index])) {
					return false;
				}
				if(index != 0U && !less(values[index - 1U], values[index])) {
					return false;
				}
			}
			return true;
		}

		[[nodiscard]] bool CommandLess(
			const AnimationReplicatedCommandRecord& left,
			const AnimationReplicatedCommandRecord& right) noexcept
		{
			if(left.effectiveTick != right.effectiveTick) {
				return left.effectiveTick < right.effectiveTick;
			}
			if(left.producerId != right.producerId) {
				return left.producerId < right.producerId;
			}
			if(left.producerSequence != right.producerSequence) {
				return left.producerSequence < right.producerSequence;
			}
			return left.commandId < right.commandId;
		}

		[[nodiscard]] bool ParameterLess(
			const AnimationReplicatedParameterRecord& left,
			const AnimationReplicatedParameterRecord& right) noexcept
		{
			if(left.effectiveTick != right.effectiveTick) {
				return left.effectiveTick < right.effectiveTick;
			}
			if(left.producerId != right.producerId) {
				return left.producerId < right.producerId;
			}
			if(left.producerSequence != right.producerSequence) {
				return left.producerSequence < right.producerSequence;
			}
			return left.parameter.parameterId < right.parameter.parameterId;
		}

		[[nodiscard]] bool StateLess(
			const AnimationReplicationRecord& left,
			const AnimationReplicationRecord& right) noexcept
		{
			if(left.state.kind != right.state.kind) {
				return left.state.kind < right.state.kind;
			}
			if(left.state.stableId != right.state.stableId) {
				return left.state.stableId < right.state.stableId;
			}
			return left.state.ordinal < right.state.ordinal;
		}

		[[nodiscard]] bool RootLess(
			const AnimationReplicatedRootMotionRecord& left,
			const AnimationReplicatedRootMotionRecord& right) noexcept
		{
			return left.state.accumulationId < right.state.accumulationId;
		}

		[[nodiscard]] bool MotionLess(
			const AnimationReplicatedMotionMatchRecord& left,
			const AnimationReplicatedMotionMatchRecord& right) noexcept
		{
			return left.state.stableNodeId < right.state.stableNodeId;
		}

		[[nodiscard]] bool RagdollLess(
			const AnimationReplicatedRagdollRecord& left,
			const AnimationReplicatedRagdollRecord& right) noexcept
		{
			if(left.state.ragdollInstanceId != right.state.ragdollInstanceId) {
				return left.state.ragdollInstanceId < right.state.ragdollInstanceId;
			}
			return left.state.bodyId < right.state.bodyId;
		}

		[[nodiscard]] std::size_t TotalRecords(
			const AnimationReplicationPacket& packet) noexcept
		{
			return packet.commands.size() + packet.parameters.size() + packet.records.size()
				+ packet.rootMotion.size() + packet.motionMatches.size() + packet.ragdolls.size();
		}

		[[nodiscard]] AnimationStatus ValidateCounts(
			const AnimationReplicationPacket& packet) noexcept
		{
			if(packet.commands.size() > kAnimationDeterminismMaximumPacketRecords
				|| packet.parameters.size() > kAnimationDeterminismMaximumPacketRecords
				|| packet.records.size() > kAnimationDeterminismMaximumPacketRecords
				|| packet.rootMotion.size() > kAnimationDeterminismMaximumPacketRecords
				|| packet.motionMatches.size() > kAnimationDeterminismMaximumPacketRecords
				|| packet.ragdolls.size() > kAnimationDeterminismMaximumPacketRecords
				|| TotalRecords(packet) > kAnimationDeterminismMaximumPacketRecords) {
				return AnimationStatus::HardLimitExceeded;
			}
			return AnimationStatus::Ok;
		}

		void Canonicalize(AnimationReplicationPacket& packet)
		{
			std::sort(packet.commands.begin(), packet.commands.end(), CommandLess);
			std::sort(packet.parameters.begin(), packet.parameters.end(), ParameterLess);
			std::sort(packet.records.begin(), packet.records.end(), StateLess);
			std::sort(packet.rootMotion.begin(), packet.rootMotion.end(), RootLess);
			std::sort(packet.motionMatches.begin(), packet.motionMatches.end(), MotionLess);
			std::sort(packet.ragdolls.begin(), packet.ragdolls.end(), RagdollLess);
		}

		[[nodiscard]] bool HasPredictedValue(const AnimationReplicationPacket& packet) noexcept
		{
			return std::any_of(packet.commands.begin(), packet.commands.end(),
				[](const auto& value) { return value.predicted; })
				|| std::any_of(packet.parameters.begin(), packet.parameters.end(),
					[](const auto& value) { return value.predicted; })
				|| std::any_of(packet.records.begin(), packet.records.end(),
					[](const auto& value) { return value.predicted; })
				|| std::any_of(packet.rootMotion.begin(), packet.rootMotion.end(),
					[](const auto& value) { return value.predicted; })
				|| std::any_of(packet.motionMatches.begin(), packet.motionMatches.end(),
					[](const auto& value) { return value.predicted; })
				|| std::any_of(packet.ragdolls.begin(), packet.ragdolls.end(),
					[](const auto& value) { return value.predicted; });
		}

		[[nodiscard]] bool HasAuthoritativeValue(const AnimationReplicationPacket& packet) noexcept
		{
			return std::any_of(packet.commands.begin(), packet.commands.end(),
				[](const auto& value) { return !value.predicted; })
				|| std::any_of(packet.parameters.begin(), packet.parameters.end(),
					[](const auto& value) { return !value.predicted; })
				|| std::any_of(packet.records.begin(), packet.records.end(),
					[](const auto& value) { return !value.predicted; })
				|| std::any_of(packet.rootMotion.begin(), packet.rootMotion.end(),
					[](const auto& value) { return !value.predicted; })
				|| std::any_of(packet.motionMatches.begin(), packet.motionMatches.end(),
					[](const auto& value) { return !value.predicted; })
				|| std::any_of(packet.ragdolls.begin(), packet.ragdolls.end(),
					[](const auto& value) { return !value.predicted; });
		}

		[[nodiscard]] AnimationStatus ValidatePacket(
			const AnimationReplicationPacket& packet) noexcept
		{
			const AnimationStatus countStatus = ValidateCounts(packet);
			if(countStatus != AnimationStatus::Ok) {
				return countStatus;
			}
			if(packet.header.version != kAnimationDeterminismPacketVersion
				|| !IsSupported(packet.header.kind) || packet.header.sequence == 0U
				|| !packet.header.animator.IsValid() || packet.header.tick < 0
				|| packet.header.publicationGeneration == 0U
				|| !packet.header.fingerprint.IsValid()) {
				return AnimationStatus::InvalidArgument;
			}
			const bool commandsValid = StrictlyOrderedAndValid(packet.commands, CommandLess,
				[](const AnimationReplicatedCommandRecord& value) {
					return value.commandId != 0U && value.targetStableId != 0U
						&& value.correlationId != 0U && value.effectiveTick >= 0
						&& value.producerId != 0U && value.producerSequence != 0U
						&& IsSupported(value.operation)
						&& value.payloadWordCount <= value.payloadWords.size()
						&& std::all_of(
							value.payloadWords.begin() + value.payloadWordCount,
							value.payloadWords.end(),
							[](std::uint32_t word) { return word == 0U; });
				});
			const bool parametersValid = StrictlyOrderedAndValid(packet.parameters, ParameterLess,
				[](const AnimationReplicatedParameterRecord& value) {
					return value.effectiveTick >= 0 && value.producerId != 0U
						&& value.producerSequence != 0U && ValidParameter(value.parameter);
				});
			const bool statesValid = StrictlyOrderedAndValid(packet.records, StateLess,
				[](const AnimationReplicationRecord& value) {
					return value.state.stableId != 0U && IsSupported(value.state.kind);
				});
			const bool rootsValid = StrictlyOrderedAndValid(packet.rootMotion, RootLess, ValidRoot);
			const bool motionsValid = StrictlyOrderedAndValid(
				packet.motionMatches, MotionLess, ValidMotion);
			const bool ragdollsValid = StrictlyOrderedAndValid(
				packet.ragdolls, RagdollLess, ValidRagdoll);
			if(!commandsValid || !parametersValid || !statesValid || !rootsValid
				|| !motionsValid || !ragdollsValid) {
				return AnimationStatus::InvalidArgument;
			}
			const bool predictedKind = packet.header.kind
				== AnimationNetworkPacketKind::PredictedInput;
			if((predictedKind && HasAuthoritativeValue(packet))
				|| (!predictedKind && HasPredictedValue(packet))) {
				return AnimationStatus::InvalidArgument;
			}
			return AnimationStatus::Ok;
		}

		void WriteFingerprint(ByteWriter& writer, const AnimationDeterministicFingerprint& value)
		{
			writer.U64(value.compatibility);
			writer.U64(value.controllerAsset);
			writer.U64(value.programAsset);
			writer.U64(value.retargetAsset);
			writer.U64(value.motionDatabaseAsset);
			writer.U64(value.ragdollProfileAsset);
			writer.U64(value.aggregateAssetHash);
		}

		[[nodiscard]] bool ReadFingerprint(
			ByteReader& reader,
			AnimationDeterministicFingerprint& value) noexcept
		{
			return reader.U64(value.compatibility) && reader.U64(value.controllerAsset)
				&& reader.U64(value.programAsset) && reader.U64(value.retargetAsset)
				&& reader.U64(value.motionDatabaseAsset)
				&& reader.U64(value.ragdollProfileAsset)
				&& reader.U64(value.aggregateAssetHash);
		}

		template<std::size_t Count>
		void WriteWords(ByteWriter& writer, const std::array<std::uint32_t, Count>& words)
		{
			for(const std::uint32_t word : words) {
				writer.U32(word);
			}
		}

		template<std::size_t Count>
		[[nodiscard]] bool ReadWords(
			ByteReader& reader,
			std::array<std::uint32_t, Count>& words) noexcept
		{
			for(std::uint32_t& word : words) {
				if(!reader.U32(word)) {
					return false;
				}
			}
			return true;
		}

		void WriteBool(ByteWriter& writer, bool value) { writer.U8(value ? 1U : 0U); }

		[[nodiscard]] bool ReadBool(ByteReader& reader, bool& value) noexcept
		{
			std::uint8_t encoded = 0U;
			if(!reader.U8(encoded) || encoded > 1U) {
				return false;
			}
			value = encoded != 0U;
			return true;
		}

		void WriteCommand(ByteWriter& writer, const AnimationReplicatedCommandRecord& value)
		{
			writer.U64(value.commandId);
			writer.U64(value.targetStableId);
			writer.U64(value.correlationId);
			writer.U64(static_cast<std::uint64_t>(value.effectiveTick));
			writer.U32(value.producerId);
			writer.U64(value.producerSequence);
			writer.U16(static_cast<std::uint16_t>(value.operation));
			writer.U8(value.payloadWordCount);
			WriteWords(writer, value.payloadWords);
			WriteBool(writer, value.predicted);
		}

		[[nodiscard]] bool ReadCommand(
			ByteReader& reader,
			AnimationReplicatedCommandRecord& value) noexcept
		{
			std::uint64_t tick = 0U;
			std::uint16_t operation = 0U;
			if(!reader.U64(value.commandId) || !reader.U64(value.targetStableId)
				|| !reader.U64(value.correlationId) || !reader.U64(tick)
				|| !reader.U32(value.producerId) || !reader.U64(value.producerSequence)
				|| !reader.U16(operation) || !reader.U8(value.payloadWordCount)
				|| !ReadWords(reader, value.payloadWords) || !ReadBool(reader, value.predicted)) {
				return false;
			}
			value.effectiveTick = static_cast<AnimationFixedTick>(tick);
			value.operation = static_cast<AnimationReplicatedCommandKind>(operation);
			return true;
		}

		void WriteParameter(ByteWriter& writer, const AnimationReplicatedParameterRecord& value)
		{
			writer.U64(value.parameter.parameterId);
			writer.U8(static_cast<std::uint8_t>(value.parameter.type));
			WriteWords(writer, value.parameter.valueWords);
			writer.U64(static_cast<std::uint64_t>(value.effectiveTick));
			writer.U32(value.producerId);
			writer.U64(value.producerSequence);
			WriteBool(writer, value.predicted);
		}

		[[nodiscard]] bool ReadParameter(
			ByteReader& reader,
			AnimationReplicatedParameterRecord& value) noexcept
		{
			std::uint8_t type = 0U;
			std::uint64_t tick = 0U;
			if(!reader.U64(value.parameter.parameterId) || !reader.U8(type)
				|| !ReadWords(reader, value.parameter.valueWords) || !reader.U64(tick)
				|| !reader.U32(value.producerId) || !reader.U64(value.producerSequence)
				|| !ReadBool(reader, value.predicted)) {
				return false;
			}
			value.parameter.type = static_cast<AnimationRollbackValueType>(type);
			value.effectiveTick = static_cast<AnimationFixedTick>(tick);
			return true;
		}

		void WriteState(ByteWriter& writer, const AnimationReplicationRecord& value)
		{
			writer.U16(static_cast<std::uint16_t>(value.state.kind));
			writer.U16(value.state.flags);
			writer.U32(value.state.ordinal);
			writer.U64(value.state.stableId);
			for(const std::uint64_t word : value.state.values) {
				writer.U64(word);
			}
			WriteBool(writer, value.predicted);
		}

		[[nodiscard]] bool ReadState(
			ByteReader& reader,
			AnimationReplicationRecord& value) noexcept
		{
			std::uint16_t kind = 0U;
			if(!reader.U16(kind) || !reader.U16(value.state.flags)
				|| !reader.U32(value.state.ordinal) || !reader.U64(value.state.stableId)) {
				return false;
			}
			for(std::uint64_t& word : value.state.values) {
				if(!reader.U64(word)) {
					return false;
				}
			}
			value.state.kind = static_cast<AnimationDeterministicStateKind>(kind);
			return ReadBool(reader, value.predicted);
		}

		void WriteRoot(ByteWriter& writer, const AnimationReplicatedRootMotionRecord& value)
		{
			writer.U64(value.state.accumulationId);
			writer.U64(static_cast<std::uint64_t>(value.state.sourceBeginTick));
			writer.U64(static_cast<std::uint64_t>(value.state.sourceEndTick));
			WriteWords(writer, value.state.localTranslation.words);
			WriteWords(writer, value.state.worldTranslation.words);
			writer.U32(value.state.localYawBits);
			writer.U32(value.state.worldYawBits);
			writer.U64(static_cast<std::uint64_t>(value.state.accumulatedCycles));
			writer.U64(value.state.sourceStateId);
			writer.U32(value.state.layerIndex);
			writer.U32(value.state.generation);
			writer.U64(value.state.correlationId);
			writer.U64(static_cast<std::uint64_t>(value.state.appliedThroughTick));
			WriteBool(writer, value.acknowledged);
			WriteBool(writer, value.predicted);
		}

		[[nodiscard]] bool ReadRoot(
			ByteReader& reader,
			AnimationReplicatedRootMotionRecord& value) noexcept
		{
			std::uint64_t begin = 0U;
			std::uint64_t end = 0U;
			std::uint64_t cycles = 0U;
			std::uint64_t applied = 0U;
			if(!reader.U64(value.state.accumulationId) || !reader.U64(begin)
				|| !reader.U64(end) || !ReadWords(reader, value.state.localTranslation.words)
				|| !ReadWords(reader, value.state.worldTranslation.words)
				|| !reader.U32(value.state.localYawBits) || !reader.U32(value.state.worldYawBits)
				|| !reader.U64(cycles) || !reader.U64(value.state.sourceStateId)
				|| !reader.U32(value.state.layerIndex) || !reader.U32(value.state.generation)
				|| !reader.U64(value.state.correlationId) || !reader.U64(applied)
				|| !ReadBool(reader, value.acknowledged)
				|| !ReadBool(reader, value.predicted)) {
				return false;
			}
			value.state.sourceBeginTick = static_cast<AnimationFixedTick>(begin);
			value.state.sourceEndTick = static_cast<AnimationFixedTick>(end);
			value.state.accumulatedCycles = static_cast<std::int64_t>(cycles);
			value.state.appliedThroughTick = static_cast<AnimationFixedTick>(applied);
			return true;
		}

		void WriteMotion(ByteWriter& writer, const AnimationReplicatedMotionMatchRecord& value)
		{
			writer.U64(value.state.stableNodeId);
			writer.U64(value.state.databaseFingerprint);
			writer.U32(value.state.databaseGeneration);
			writer.U64(value.state.querySequence);
			writer.U64(value.state.candidateId);
			writer.U64(value.state.sourceClipFingerprint);
			writer.U64(static_cast<std::uint64_t>(value.state.sampleTick));
			writer.U32(value.state.totalScoreBits);
			writer.U64(value.state.selectionHash);
			WriteBool(writer, value.state.usedFallback);
			WriteBool(writer, value.predicted);
		}

		[[nodiscard]] bool ReadMotion(
			ByteReader& reader,
			AnimationReplicatedMotionMatchRecord& value) noexcept
		{
			std::uint64_t sampleTick = 0U;
			if(!reader.U64(value.state.stableNodeId)
				|| !reader.U64(value.state.databaseFingerprint)
				|| !reader.U32(value.state.databaseGeneration)
				|| !reader.U64(value.state.querySequence)
				|| !reader.U64(value.state.candidateId)
				|| !reader.U64(value.state.sourceClipFingerprint)
				|| !reader.U64(sampleTick) || !reader.U32(value.state.totalScoreBits)
				|| !reader.U64(value.state.selectionHash)
				|| !ReadBool(reader, value.state.usedFallback)
				|| !ReadBool(reader, value.predicted)) {
				return false;
			}
			value.state.sampleTick = static_cast<AnimationFixedTick>(sampleTick);
			return true;
		}

		void WriteTransform(ByteWriter& writer, const AnimationNetworkTransformBits& value)
		{
			WriteWords(writer, value.translation.words);
			WriteWords(writer, value.rotation.words);
			WriteWords(writer, value.scale.words);
		}

		[[nodiscard]] bool ReadTransform(
			ByteReader& reader,
			AnimationNetworkTransformBits& value) noexcept
		{
			return ReadWords(reader, value.translation.words)
				&& ReadWords(reader, value.rotation.words)
				&& ReadWords(reader, value.scale.words);
		}

		void WriteRagdoll(ByteWriter& writer, const AnimationReplicatedRagdollRecord& value)
		{
			writer.U64(value.state.ragdollInstanceId);
			writer.U64(value.state.bodyId);
			writer.U64(value.state.stableJointId);
			writer.U8(static_cast<std::uint8_t>(value.state.lifecycle));
			writer.U32(value.state.worldGeneration);
			writer.U32(value.state.profileGeneration);
			writer.U32(value.state.skeletonGeneration);
			writer.U32(value.state.physicsPublicationGeneration);
			writer.U32(value.state.bodyGeneration);
			writer.U64(static_cast<std::uint64_t>(value.state.fixedStep));
			WriteTransform(writer, value.state.modelTransform);
			WriteWords(writer, value.state.linearVelocity.words);
			WriteWords(writer, value.state.angularVelocity.words);
			writer.U32(value.state.blendWeightBits);
			writer.U64(value.state.eventSequence);
			WriteBool(writer, value.state.sleeping);
			WriteBool(writer, value.predicted);
		}

		[[nodiscard]] bool ReadRagdoll(
			ByteReader& reader,
			AnimationReplicatedRagdollRecord& value) noexcept
		{
			std::uint8_t lifecycle = 0U;
			std::uint64_t fixedStep = 0U;
			if(!reader.U64(value.state.ragdollInstanceId)
				|| !reader.U64(value.state.bodyId) || !reader.U64(value.state.stableJointId)
				|| !reader.U8(lifecycle) || !reader.U32(value.state.worldGeneration)
				|| !reader.U32(value.state.profileGeneration)
				|| !reader.U32(value.state.skeletonGeneration)
				|| !reader.U32(value.state.physicsPublicationGeneration)
				|| !reader.U32(value.state.bodyGeneration) || !reader.U64(fixedStep)
				|| !ReadTransform(reader, value.state.modelTransform)
				|| !ReadWords(reader, value.state.linearVelocity.words)
				|| !ReadWords(reader, value.state.angularVelocity.words)
				|| !reader.U32(value.state.blendWeightBits)
				|| !reader.U64(value.state.eventSequence)
				|| !ReadBool(reader, value.state.sleeping)
				|| !ReadBool(reader, value.predicted)) {
				return false;
			}
			value.state.lifecycle = static_cast<AnimationRollbackRagdollState>(lifecycle);
			value.state.fixedStep = static_cast<AnimationFixedTick>(fixedStep);
			return true;
		}

		void WritePacket(ByteWriter& writer, const AnimationReplicationPacket& packet)
		{
			writer.U16(packet.header.version);
			writer.U16(static_cast<std::uint16_t>(packet.header.kind));
			writer.U32(0U);
			writer.U64(packet.header.sequence);
			writer.U32(packet.header.animator.slotIndex);
			writer.U32(packet.header.animator.generation);
			writer.U64(static_cast<std::uint64_t>(packet.header.tick));
			writer.U32(packet.header.publicationGeneration);
			WriteFingerprint(writer, packet.header.fingerprint);
			writer.U64(0U);
			writer.U32(static_cast<std::uint32_t>(packet.commands.size()));
			writer.U32(static_cast<std::uint32_t>(packet.parameters.size()));
			writer.U32(static_cast<std::uint32_t>(packet.records.size()));
			writer.U32(static_cast<std::uint32_t>(packet.rootMotion.size()));
			writer.U32(static_cast<std::uint32_t>(packet.motionMatches.size()));
			writer.U32(static_cast<std::uint32_t>(packet.ragdolls.size()));
			for(const auto& value : packet.commands) { WriteCommand(writer, value); }
			for(const auto& value : packet.parameters) { WriteParameter(writer, value); }
			for(const auto& value : packet.records) { WriteState(writer, value); }
			for(const auto& value : packet.rootMotion) { WriteRoot(writer, value); }
			for(const auto& value : packet.motionMatches) { WriteMotion(writer, value); }
			for(const auto& value : packet.ragdolls) { WriteRagdoll(writer, value); }
		}

		[[nodiscard]] AnimationStatus ReadPacket(
			std::span<const std::uint8_t> bytes,
			AnimationReplicationPacket& packet) noexcept
		{
			if(bytes.size() < kPayloadOffset) {
				return AnimationStatus::TruncatedInput;
			}
			ByteReader reader(bytes);
			std::uint16_t kind = 0U;
			std::uint64_t tick = 0U;
			std::array<std::uint32_t, 6U> counts{};
			if(!reader.U16(packet.header.version) || !reader.U16(kind)
				|| !reader.U32(packet.header.byteCount) || !reader.U64(packet.header.sequence)
				|| !reader.U32(packet.header.animator.slotIndex)
				|| !reader.U32(packet.header.animator.generation) || !reader.U64(tick)
				|| !reader.U32(packet.header.publicationGeneration)
				|| !ReadFingerprint(reader, packet.header.fingerprint)
				|| !reader.U64(packet.header.payloadChecksum)) {
				return AnimationStatus::TruncatedInput;
			}
			packet.header.kind = static_cast<AnimationNetworkPacketKind>(kind);
			packet.header.tick = static_cast<AnimationFixedTick>(tick);
			if(packet.header.byteCount > kAnimationDeterminismMaximumPacketBytes) {
				return AnimationStatus::HardLimitExceeded;
			}
			if(packet.header.byteCount > bytes.size()) {
				return AnimationStatus::TruncatedInput;
			}
			if(packet.header.byteCount != bytes.size()) {
				return AnimationStatus::InvalidEnvelope;
			}
			if(packet.header.payloadChecksum != PayloadChecksum(bytes.subspan(kPayloadOffset))) {
				return AnimationStatus::ContentHashMismatch;
			}
			for(std::uint32_t& count : counts) {
				if(!reader.U32(count)) {
					return AnimationStatus::TruncatedInput;
				}
			}
			std::uint64_t total = 0U;
			for(const std::uint32_t count : counts) {
				if(count > kAnimationDeterminismMaximumPacketRecords) {
					return AnimationStatus::HardLimitExceeded;
				}
				total += count;
			}
			if(total > kAnimationDeterminismMaximumPacketRecords) {
				return AnimationStatus::HardLimitExceeded;
			}
			try {
				packet.commands.resize(counts[0]);
				packet.parameters.resize(counts[1]);
				packet.records.resize(counts[2]);
				packet.rootMotion.resize(counts[3]);
				packet.motionMatches.resize(counts[4]);
				packet.ragdolls.resize(counts[5]);
				for(auto& value : packet.commands) {
					if(!ReadCommand(reader, value)) { return AnimationStatus::TruncatedInput; }
				}
				for(auto& value : packet.parameters) {
					if(!ReadParameter(reader, value)) { return AnimationStatus::TruncatedInput; }
				}
				for(auto& value : packet.records) {
					if(!ReadState(reader, value)) { return AnimationStatus::TruncatedInput; }
				}
				for(auto& value : packet.rootMotion) {
					if(!ReadRoot(reader, value)) { return AnimationStatus::TruncatedInput; }
				}
				for(auto& value : packet.motionMatches) {
					if(!ReadMotion(reader, value)) { return AnimationStatus::TruncatedInput; }
				}
				for(auto& value : packet.ragdolls) {
					if(!ReadRagdoll(reader, value)) { return AnimationStatus::TruncatedInput; }
				}
			} catch(const std::bad_alloc&) {
				return AnimationStatus::HardLimitExceeded;
			} catch(const std::length_error&) {
				return AnimationStatus::HardLimitExceeded;
			}
			if(reader.Cursor() != bytes.size()) {
				return AnimationStatus::InvalidEnvelope;
			}
			const AnimationStatus validation = ValidatePacket(packet);
			return validation == AnimationStatus::InvalidArgument
				? AnimationStatus::InvalidEnvelope
				: validation;
		}
	}

	AnimationStatus AnimationReplicationCodec::Encode(
		const AnimationReplicationPacket& packet,
		std::vector<std::uint8_t>& bytes) const noexcept
	{
		const AnimationStatus countStatus = ValidateCounts(packet);
		if(countStatus != AnimationStatus::Ok) {
			return countStatus;
		}
		try {
			AnimationReplicationPacket canonical = packet;
			Canonicalize(canonical);
			const AnimationStatus validation = ValidatePacket(canonical);
			if(validation != AnimationStatus::Ok) {
				return validation;
			}
			std::vector<std::uint8_t> staged;
			staged.reserve(std::min(
				kAnimationDeterminismMaximumPacketBytes,
				kPayloadOffset + 24U + TotalRecords(canonical) * 160U));
			ByteWriter writer(staged);
			WritePacket(writer, canonical);
			if(staged.size() > kAnimationDeterminismMaximumPacketBytes
				|| staged.size() > std::numeric_limits<std::uint32_t>::max()) {
				return AnimationStatus::HardLimitExceeded;
			}
			PatchU32(staged, kByteCountOffset, static_cast<std::uint32_t>(staged.size()));
			PatchU64(staged, kPayloadChecksumOffset,
				PayloadChecksum(std::span<const std::uint8_t>(staged).subspan(kPayloadOffset)));
			bytes = std::move(staged);
			return AnimationStatus::Ok;
		} catch(const std::bad_alloc&) {
			return AnimationStatus::HardLimitExceeded;
		} catch(const std::length_error&) {
			return AnimationStatus::HardLimitExceeded;
		}
	}

	AnimationStatus AnimationReplicationCodec::Decode(
		std::span<const std::uint8_t> bytes,
		AnimationReplicationPacket& packet) const noexcept
	{
		if(bytes.size() > kAnimationDeterminismMaximumPacketBytes) {
			return AnimationStatus::HardLimitExceeded;
		}
		AnimationReplicationPacket staged;
		const AnimationStatus status = ReadPacket(bytes, staged);
		if(status != AnimationStatus::Ok) {
			return status;
		}
		packet = std::move(staged);
		return AnimationStatus::Ok;
	}

	AnimationStatus AnimationReplicationCodec::Decode(
		std::span<const std::uint8_t> bytes,
		const AnimationReplicationAcceptance& acceptance,
		AnimationReplicationPacket& packet) const noexcept
	{
		if(!acceptance.animator.IsValid() || !acceptance.fingerprint.IsValid()
			|| acceptance.publicationGeneration == 0U) {
			return AnimationStatus::InvalidArgument;
		}
		AnimationReplicationPacket staged;
		const AnimationStatus decode = Decode(bytes, staged);
		if(decode != AnimationStatus::Ok) {
			return decode;
		}
		if(staged.header.fingerprint != acceptance.fingerprint) {
			return AnimationStatus::IncompatibleAsset;
		}
		if(staged.header.animator != acceptance.animator
			|| staged.header.publicationGeneration != acceptance.publicationGeneration) {
			return AnimationStatus::StaleHandle;
		}
		if(staged.header.sequence < acceptance.minimumSequence) {
			return AnimationStatus::Rejected;
		}
		if(staged.header.kind == AnimationNetworkPacketKind::PredictedInput
			&& !acceptance.allowPredictedInput) {
			return AnimationStatus::Rejected;
		}
		if(staged.header.kind == AnimationNetworkPacketKind::LateJoinSnapshot
			&& !acceptance.allowLateJoinSnapshot) {
			return AnimationStatus::Rejected;
		}
		packet = std::move(staged);
		return AnimationStatus::Ok;
	}
}
