// AnimationReplication.h - Transport-neutral deterministic animation packet codec.

#pragma once

#include "Animation/Determinism/AnimationNetworkPackets.h"

namespace TerraEngine::Animation {
	// Decode requirements are supplied by the live protected boundary. The codec
	// validates them after parsing into a staged value and before replacing output.
	struct AnimationReplicationAcceptance {
		AnimatorInstanceHandle animator{};
		AnimationDeterministicFingerprint fingerprint{};
		AnimationGeneration publicationGeneration = 0U;
		std::uint64_t minimumSequence = 0U;
		bool allowPredictedInput = false;
		bool allowLateJoinSnapshot = false;
		auto operator<=>(const AnimationReplicationAcceptance&) const = default;
	};

	class TERRAENGINE_API AnimationReplicationCodec final {
	public:
		[[nodiscard]] AnimationStatus Encode(const AnimationReplicationPacket& packet,
			std::vector<std::uint8_t>& bytes) const noexcept;
		[[nodiscard]] AnimationStatus Decode(std::span<const std::uint8_t> bytes,
			AnimationReplicationPacket& packet) const noexcept;
		[[nodiscard]] AnimationStatus Decode(std::span<const std::uint8_t> bytes,
			const AnimationReplicationAcceptance& acceptance,
			AnimationReplicationPacket& packet) const noexcept;
	};
}
