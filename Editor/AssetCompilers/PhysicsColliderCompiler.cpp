// PhysicsColliderCompiler.cpp - Transactional collider cook publication.

#include "PhysicsColliderCompiler.h"

#include <array>
#include <fstream>
#include <mutex>
#include <system_error>

namespace TerraEngine::AssetPipeline
{
	namespace
	{
		std::mutex g_PhysicsColliderPublicationMutex;

		bool IsCancelled(const PhysicsColliderCompileRequest& request)
		{
			return request.cancellationRequested != nullptr
				&& request.cancellationRequested->load(std::memory_order_acquire);
		}

		const std::atomic_bool& CancellationFlag(const PhysicsColliderCompileRequest& request)
		{
			// Synchronous headless callers may omit a token. A shared immutable false
			// flag keeps the cook callback contract uniform without nullable logic.
			static const std::atomic_bool kNeverCancelled = false;
			return request.cancellationRequested != nullptr ? *request.cancellationRequested : kNeverCancelled;
		}

		void AddDiagnostic(PhysicsColliderCompileReport& report, PhysicsColliderCompileStage stage, std::string code, std::string message)
		{
			// Bound reports so one malformed mesh cannot create an unbounded editor
			// job record. The detailed typed cooker may keep its own bounded details.
			if(report.diagnostics.size() < 32U)
				report.diagnostics.push_back({stage, std::move(code), std::move(message)});
		}

		std::uint64_t Fingerprint(const std::vector<std::byte>& bytes)
		{
			std::uint64_t value = 14695981039346656037ULL;
			for(const std::byte byte : bytes)
			{
				value ^= static_cast<std::uint8_t>(byte);
				value *= 1099511628211ULL;
			}
			return value;
		}

		bool ReadBytes(const std::filesystem::path& path, std::vector<std::byte>& destination)
		{
			std::ifstream stream(path, std::ios::binary | std::ios::ate);
			if(!stream)
				return false;
			const std::streamsize size = stream.tellg();
			if(size < 0)
				return false;
			destination.resize(static_cast<std::size_t>(size));
			stream.seekg(0, std::ios::beg);
			return size == 0 || static_cast<bool>(stream.read(reinterpret_cast<char*>(destination.data()), size));
		}

		void RemoveStagingFile(const std::filesystem::path& path)
		{
			std::error_code ignored;
			std::filesystem::remove(path, ignored);
		}

		bool PublishStagedFile(const std::filesystem::path& stagedPath, const std::filesystem::path& outputPath)
		{
			std::lock_guard lock(g_PhysicsColliderPublicationMutex);
			std::error_code error;
			if(!outputPath.parent_path().empty())
				std::filesystem::create_directories(outputPath.parent_path(), error);
			if(error)
				return false;

			// The temporary backup exists only during the protected replacement. If
			// rename fails, it restores the old usable envelope without partial state.
			const std::filesystem::path backupPath = outputPath.string() + ".previous";
			std::filesystem::remove(backupPath, error);
			error.clear();
			const bool hadPrevious = std::filesystem::exists(outputPath, error) && !error;
			if(hadPrevious)
			{
				std::filesystem::rename(outputPath, backupPath, error);
				if(error)
					return false;
			}

			std::filesystem::rename(stagedPath, outputPath, error);
			if(error)
			{
				if(hadPrevious)
				{
					std::error_code restoreError;
					std::filesystem::rename(backupPath, outputPath, restoreError);
				}
				return false;
			}
			if(hadPrevious)
				std::filesystem::remove(backupPath, error);
			return true;
		}
	}

	const char* ToString(PhysicsColliderCompileStage stage) noexcept
	{
		switch(stage)
		{
		case PhysicsColliderCompileStage::Requested: return "requested";
		case PhysicsColliderCompileStage::Validating: return "validating";
		case PhysicsColliderCompileStage::Cooking: return "cooking";
		case PhysicsColliderCompileStage::Staged: return "staged";
		case PhysicsColliderCompileStage::Published: return "published";
		case PhysicsColliderCompileStage::Cancelled: return "cancelled";
		case PhysicsColliderCompileStage::Failed: return "failed";
		}
		return "unknown";
	}

	PhysicsColliderCompileReport CompilePhysicsCollider(const PhysicsColliderCompileRequest& request)
	{
		PhysicsColliderCompileReport report;
		const bool priorOutputExists = std::filesystem::exists(request.outputPath);
		const std::filesystem::path stagedPath = request.outputPath.string() + ".staged";
		RemoveStagingFile(stagedPath);

		if(request.recipePath.extension() != kPhysicsColliderRecipeExtension
			|| request.outputPath.extension() != kPhysicsCookedColliderExtension)
		{
			report.stage = PhysicsColliderCompileStage::Failed;
			AddDiagnostic(report, report.stage, "invalid_extension", "Collider cooking requires a .tecollider recipe and .tepcoll output.");
			return report;
		}
		if(!request.callbacks.validate || !request.callbacks.cook || !request.callbacks.validateStagedOutput)
		{
			report.stage = PhysicsColliderCompileStage::Failed;
			AddDiagnostic(report, report.stage, "missing_typed_cooker", "No provider-neutral recipe/envelope cooker was supplied.");
			return report;
		}
		if(!stagedPath.parent_path().empty())
		{
			std::error_code directoryError;
			std::filesystem::create_directories(stagedPath.parent_path(), directoryError);
			if(directoryError)
			{
				report.stage = PhysicsColliderCompileStage::Failed;
				report.preservedLastKnownGood = priorOutputExists;
				AddDiagnostic(report, report.stage, "stage_directory_failed", "The collider staging directory could not be created.");
				return report;
			}
		}
		if(IsCancelled(request))
		{
			report.stage = PhysicsColliderCompileStage::Cancelled;
			report.preservedLastKnownGood = priorOutputExists;
			AddDiagnostic(report, report.stage, "cancelled", "Collider cook was cancelled before validation.");
			return report;
		}

		std::vector<std::byte> recipeBytes;
		if(!ReadBytes(request.recipePath, recipeBytes))
		{
			report.stage = PhysicsColliderCompileStage::Failed;
			report.preservedLastKnownGood = priorOutputExists;
			AddDiagnostic(report, report.stage, "recipe_read_failed", "The collider recipe could not be read.");
			return report;
		}
		report.sourceFingerprint = Fingerprint(recipeBytes);
		report.settingsFingerprint = Fingerprint(request.settingsBytes);
		if(priorOutputExists && request.callbacks.canReusePublishedOutput
			&& request.callbacks.canReusePublishedOutput(
				request.outputPath, recipeBytes, report.sourceFingerprint,
				report.settingsFingerprint, report))
		{
			report.stage = PhysicsColliderCompileStage::Published;
			report.published = true;
			report.reusedCachedOutput = true;
			AddDiagnostic(report, report.stage, "cache_reused",
				"The existing cooked collider already matches the normalized inputs.");
			return report;
		}

		report.stage = PhysicsColliderCompileStage::Validating;
		std::vector<std::byte> preparedRecipe;
		if(!request.callbacks.validate(request.recipePath, recipeBytes, preparedRecipe, report) || IsCancelled(request))
		{
			report.stage = IsCancelled(request) ? PhysicsColliderCompileStage::Cancelled : PhysicsColliderCompileStage::Failed;
			report.preservedLastKnownGood = priorOutputExists;
			AddDiagnostic(report, report.stage, IsCancelled(request) ? "cancelled" : "recipe_validation_failed", "Collider recipe was not published.");
			return report;
		}

		report.stage = PhysicsColliderCompileStage::Cooking;
		if(!request.callbacks.cook(preparedRecipe, stagedPath, CancellationFlag(request), report) || IsCancelled(request))
		{
			RemoveStagingFile(stagedPath);
			report.stage = IsCancelled(request) ? PhysicsColliderCompileStage::Cancelled : PhysicsColliderCompileStage::Failed;
			report.preservedLastKnownGood = priorOutputExists;
			AddDiagnostic(report, report.stage, IsCancelled(request) ? "cancelled" : "cook_failed", "Collider cook failed before publication.");
			return report;
		}

		report.stage = PhysicsColliderCompileStage::Staged;
		if(!request.callbacks.validateStagedOutput(stagedPath, report) || IsCancelled(request))
		{
			RemoveStagingFile(stagedPath);
			report.stage = IsCancelled(request) ? PhysicsColliderCompileStage::Cancelled : PhysicsColliderCompileStage::Failed;
			report.preservedLastKnownGood = priorOutputExists;
			AddDiagnostic(report, report.stage, IsCancelled(request) ? "cancelled" : "stage_validation_failed", "The staged cooked collider envelope was rejected.");
			return report;
		}

		if(request.callbacks.verifyPublicationInputs
			&& !request.callbacks.verifyPublicationInputs(report.sourceFingerprint, report.settingsFingerprint, report))
		{
			RemoveStagingFile(stagedPath);
			report.stage = PhysicsColliderCompileStage::Failed;
			report.preservedLastKnownGood = priorOutputExists;
			AddDiagnostic(report, report.stage, "publication_inputs_changed", "Recipe or cook settings changed while the worker was cooking.");
			return report;
		}

		if(IsCancelled(request) || !PublishStagedFile(stagedPath, request.outputPath))
		{
			RemoveStagingFile(stagedPath);
			report.stage = IsCancelled(request) ? PhysicsColliderCompileStage::Cancelled : PhysicsColliderCompileStage::Failed;
			report.preservedLastKnownGood = priorOutputExists;
			AddDiagnostic(report, report.stage, IsCancelled(request) ? "cancelled" : "publish_failed", "The staged collider was not published; the previous output was preserved.");
			return report;
		}

		report.stage = PhysicsColliderCompileStage::Published;
		report.published = true;
		AddDiagnostic(report, report.stage, "published", "Cooked collider passed validation and was atomically published.");
		return report;
	}
}
