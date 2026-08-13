// PhysicsColliderCompiler.h - Worker-safe orchestration for collider cooking.
//
// Recipe parsing, envelope construction, and provider cooking remain in their
// typed physics modules. This editor layer owns cancellation, diagnostics, and
// the protected filesystem publication boundary shared by UI and headless jobs.

#pragma once

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <string>
#include <string_view>
#include <vector>

namespace TerraEngine::AssetPipeline
{
	inline constexpr std::string_view kPhysicsColliderRecipeExtension = ".tecollider";
	inline constexpr std::string_view kPhysicsCookedColliderExtension = ".tepcoll";
	inline constexpr std::string_view kPhysicsColliderProviderRevision = "2e28006e4a367624bb512bf2c37ca99a93d9d1ea";

	enum class PhysicsColliderCompileStage : std::uint8_t
	{
		Requested,
		Validating,
		Cooking,
		Staged,
		Published,
		Cancelled,
		Failed
	};

	struct PhysicsColliderCompileDiagnostic
	{
		PhysicsColliderCompileStage stage = PhysicsColliderCompileStage::Requested;
		std::string code;
		std::string message;
	};

	struct PhysicsColliderCompileReport
	{
		PhysicsColliderCompileStage stage = PhysicsColliderCompileStage::Requested;
		bool published = false;
		bool reusedCachedOutput = false;
		bool preservedLastKnownGood = false;
		std::uint64_t sourceFingerprint = 0U;
		std::uint64_t settingsFingerprint = 0U;
		std::vector<PhysicsColliderCompileDiagnostic> diagnostics;
	};

	// Callbacks run outside the protected publication section. They operate only
	// on immutable input bytes and a private staged path, so editor worker jobs
	// never mutate live asset-database state or an existing cooked collider.
	struct PhysicsColliderCookCallbacks
	{
		// A typed caller may validate an existing immutable envelope against the
		// just-read recipe/settings fingerprints and skip provider work. Returning
		// true promises that outputPath is already complete and compatible.
		std::function<bool(
			const std::filesystem::path& outputPath,
			const std::vector<std::byte>& recipeBytes,
			std::uint64_t sourceFingerprint,
			std::uint64_t settingsFingerprint,
			PhysicsColliderCompileReport& report)> canReusePublishedOutput;
		std::function<bool(
			const std::filesystem::path& recipePath,
			const std::vector<std::byte>& recipeBytes,
			std::vector<std::byte>& preparedRecipe,
			PhysicsColliderCompileReport& report)> validate;
		std::function<bool(
			const std::vector<std::byte>& preparedRecipe,
			const std::filesystem::path& stagedOutput,
			const std::atomic_bool& cancellationRequested,
			PhysicsColliderCompileReport& report)> cook;
		std::function<bool(
			const std::filesystem::path& stagedOutput,
			PhysicsColliderCompileReport& report)> validateStagedOutput;
		// Re-read source/settings fingerprints immediately before rename. This
		// rejects a stale worker result rather than publishing over newer authoring.
		std::function<bool(
			std::uint64_t expectedSourceFingerprint,
			std::uint64_t expectedSettingsFingerprint,
			PhysicsColliderCompileReport& report)> verifyPublicationInputs;
	};

	struct PhysicsColliderCompileRequest
	{
		std::filesystem::path recipePath;
		std::filesystem::path outputPath;
		std::vector<std::byte> settingsBytes;
		const std::atomic_bool* cancellationRequested = nullptr;
		PhysicsColliderCookCallbacks callbacks;
	};

	[[nodiscard]] const char* ToString(PhysicsColliderCompileStage stage) noexcept;

	// Runs all expensive work on the caller's worker task. Only the final
	// stage-then-rename operation takes the process-local publication lock.
	// A failure or cancellation always removes staging and preserves any prior
	// .tepcoll file as the last-known-good output.
	[[nodiscard]] PhysicsColliderCompileReport CompilePhysicsCollider(
		const PhysicsColliderCompileRequest& request);
}
