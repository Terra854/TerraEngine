// ArtifactValidation.h - synchronous, worker-safe inspection of owned artifact bytes.
#pragma once

#include "Core/Export.h"
#include <cstddef>
#include <span>
#include <string>
#include <string_view>

namespace TerraEngine::Runtime {
	// No producer-defined stub identity currently exists. ExplicitStub is reserved
	// for such a documented identity, never inferred from ordinary words.
	enum class ArtifactDisposition { Valid, Invalid, ExplicitStub, Unsupported };
	enum class ArtifactKind {
		AuthoredData, JsonDocument, Scene, PhysicsSettings, PhysicsMaterial,
		CookedCollider, Mesh, Texture, Material, NativeGameplay
	};
	enum class NativeImageTarget { WindowsX64, LinuxX64, Unsupported };

	struct ArtifactValidationRequest {
		// The caller owns immutable storage throughout this synchronous call.
		// Existing manifest size/hash/GUID checks remain the caller's authority.
		std::span<const std::byte> bytes;
		ArtifactKind kind = ArtifactKind::AuthoredData;
		NativeImageTarget target = NativeImageTarget::Unsupported;
		std::string_view logicalPath;
	};
	struct ArtifactValidationResult {
		ArtifactDisposition disposition = ArtifactDisposition::Invalid;
		std::string code;
		std::string diagnostic;
		std::string affectedPath;
		std::string bootstrap;
		bool IsValid() const noexcept { return disposition == ArtifactDisposition::Valid; }
		bool IsExplicitStub() const noexcept { return disposition == ArtifactDisposition::ExplicitStub; }
	};

	// These inspectors never execute candidate code or acquire live engine state.
	TERRAENGINE_API NativeImageTarget HostNativeImageTarget() noexcept;
	TERRAENGINE_API ArtifactKind InferArtifactKind(std::string_view logicalPath);
	TERRAENGINE_API ArtifactValidationResult ValidateArtifact(const ArtifactValidationRequest& request);
}
