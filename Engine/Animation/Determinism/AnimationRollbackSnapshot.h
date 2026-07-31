// AnimationRollbackSnapshot.h - Atomic capture and restore of copied animation authority.

#pragma once

#include "Animation/Determinism/AnimationNetworkPackets.h"

namespace TerraEngine::Animation {
	// A protected runtime boundary builds this requirement from its current live
	// identities. Restore validates the whole value before replacing an output.
	struct AnimationRollbackRestoreRequirements {
		AnimatorInstanceHandle animator{};
		AnimationDeterministicFingerprint fingerprint{};
		AnimationGeneration publicationGeneration = 0U;
		AnimationRollbackGenerationStamp generations{};
		auto operator<=>(const AnimationRollbackRestoreRequirements&) const = default;
	};

	// This codec never reaches into AnimationSystem. Workers and transports pass
	// owning values, while the system applies a successful restored copy only at
	// its declared protected boundary.
	class TERRAENGINE_API AnimationRollbackSnapshotCodec final {
	public:
		[[nodiscard]] AnimationStatus Capture(
			const AnimationRollbackSnapshot& source,
			AnimationRollbackSnapshot& captured) const noexcept;

		// The complete overload proves object/component/scene/asset/program,
		// physics-world, publication, and history identity in one atomic preflight.
		[[nodiscard]] AnimationStatus Restore(
			const AnimationRollbackSnapshot& snapshot,
			const AnimationRollbackRestoreRequirements& requirements,
			AnimationRollbackSnapshot& restored) const noexcept;

		// This compatibility overload supports existing ImplementationCheck+ callers that have not
		// yet assembled the complete generation stamp. It still validates snapshot
		// integrity, asset/program fingerprints, and publication generation.
		[[nodiscard]] AnimationStatus Restore(
			const AnimationRollbackSnapshot& snapshot,
			const AnimationDeterministicFingerprint& expectedFingerprint,
			AnimationGeneration expectedPublicationGeneration,
			AnimationRollbackSnapshot& restored) const noexcept;
	};
}
