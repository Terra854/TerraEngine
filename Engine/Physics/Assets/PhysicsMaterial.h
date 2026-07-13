// PhysicsMaterial.h - JSON schema for GUID-addressed .tephysmat assets.
//
// The asset remains provider-neutral. Runtime creation consumes the resolved
// value object below rather than serializing Jolt materials or provider enums.

#pragma once

#include "Core/Export.h"
#include "Physics/PhysicsTypes.h"
#include "Utilities/UUID.h"

#include <cstddef>
#include <cstdint>
#include <nlohmann/json.hpp>
#include <string>
#include <string_view>
#include <vector>

namespace TerraEngine::Physics
{
	inline constexpr std::uint32_t kPhysicsMaterialSchemaVersion = 1U;
	inline constexpr std::string_view kPhysicsMaterialExtension = ".tephysmat";
	inline constexpr std::size_t kPhysicsMaterialMaximumDiagnostics = 16U;
	inline constexpr std::size_t kPhysicsMaterialMaximumSurfaceTagLength = 128U;

	enum class PhysicsMaterialDiagnosticSeverity : std::uint8_t
	{
		Info,
		Warning,
		Error
	};

	struct PhysicsMaterialDiagnostic
	{
		PhysicsMaterialDiagnosticSeverity severity = PhysicsMaterialDiagnosticSeverity::Info;
		std::string code;
		std::string message;
	};

	// Diagnostics are intentionally capped. A malformed asset must not grow an
	// editor report without bound when a scene references it many times.
	struct PhysicsMaterialDiagnostics
	{
		std::vector<PhysicsMaterialDiagnostic> entries;
		std::size_t suppressedCount = 0U;

		void Add(PhysicsMaterialDiagnosticSeverity severity, std::string code, std::string message);
		[[nodiscard]] bool HasErrors() const noexcept;
	};

	enum class PhysicsMaterialValidationCode : std::uint8_t
	{
		None,
		UnsupportedSchema,
		MissingAssetId,
		InvalidStaticFriction,
		InvalidDynamicFriction,
		InvalidRestitution,
		InvalidDensity,
		InvalidFrictionCombineMode,
		InvalidRestitutionCombineMode,
		InvalidSurfaceTag
	};

	struct PhysicsMaterialValidationResult
	{
		bool accepted = false;
		PhysicsMaterialValidationCode code = PhysicsMaterialValidationCode::None;
		std::string message;
	};

	// This value is copied into worker-safe shape recipes. It has no provider
	// pointer, cache key, or mutable backend material ID, so it is safe to share.
	struct PhysicsMaterialRuntime
	{
		TerraEngine::UUID assetId = TerraEngine::EMPTY_UUID;
		float staticFriction = 0.6F;
		float dynamicFriction = 0.6F;
		float restitution = 0.0F;
		float density = 1000.0F;
		PhysicsCombineMode frictionCombineMode = PhysicsCombineMode::Average;
		PhysicsCombineMode restitutionCombineMode = PhysicsCombineMode::Average;
		std::string surfaceTag;
	};

	// Version-one .tephysmat authored payload. Asset database metadata supplies
	// the GUID in normal editor flows; keeping it in JSON makes headless and
	// export validation deterministic without resolving a filesystem path.
	struct PhysicsMaterial
	{
		std::uint32_t schemaVersion = kPhysicsMaterialSchemaVersion;
		TerraEngine::UUID assetId = TerraEngine::EMPTY_UUID;
		float staticFriction = 0.6F;
		float dynamicFriction = 0.6F;
		float restitution = 0.0F;
		float density = 1000.0F;
		PhysicsCombineMode frictionCombineMode = PhysicsCombineMode::Average;
		PhysicsCombineMode restitutionCombineMode = PhysicsCombineMode::Average;
		std::string surfaceTag;

		[[nodiscard]] static PhysicsMaterial Defaults(TerraEngine::UUID assetId = TerraEngine::EMPTY_UUID);
		[[nodiscard]] PhysicsMaterialValidationResult Validate() const;
		[[nodiscard]] PhysicsMaterialRuntime BuildRuntime() const;
		[[nodiscard]] nlohmann::json ToJson() const;

		// LoadJson accepts only known schemas. It may migrate a known older
		// payload into the current schema, but never guesses future semantics.
		[[nodiscard]] static bool LoadJson(
			const nlohmann::json& source,
			PhysicsMaterial& destination,
			PhysicsMaterialDiagnostics& diagnostics);
		[[nodiscard]] static bool MigrateJson(
			nlohmann::json& document,
			PhysicsMaterialDiagnostics& diagnostics);
	};

	// Alias keeps call sites expressive where the surrounding code distinguishes
	// authoring assets from the immutable runtime material value.
	using PhysicsMaterialAsset = PhysicsMaterial;

	TERRAENGINE_API std::string_view ToString(PhysicsMaterialDiagnosticSeverity severity);
	TERRAENGINE_API std::string_view ToString(PhysicsMaterialValidationCode code);
}
