// AnimationAssetOperations.cpp - checked asset publication and read adapters.

#include "AnimationAssetOperations.h"
#include "AnimationAutomationSchemas.h"

#include "../Assets/AssetDatabase.h"
#include "Engine/Animation/Assets/AnimationAssetTypes.h"
#include "Engine/File Formats/TerraEngineAnimationFile.h"
#include "Engine/Resource/ResourceManager.h"

#include <atomic>
#include <filesystem>
#include <fstream>
#include <limits>
#include <optional>
#include <string>
#include <system_error>

#ifdef _WIN32
	#ifndef NOMINMAX
		#define NOMINMAX
	#endif
	#define WIN32_LEAN_AND_MEAN
	#include <Windows.h>
#endif

namespace TerraEngine::Editor::Automation
{
	namespace
	{
		std::atomic_uint64_t g_AnimationAutomationStageSequence{1U};

		AutomationDispatchResult Fail(ErrorCategory category, std::string message)
		{
			AutomationDispatchResult result;
			result.error = category;
			result.message = std::move(message);
			return result;
		}

		bool ReadBoundedFile(
			const std::filesystem::path& path,
			std::size_t maximumBytes,
			std::vector<std::uint8_t>& output,
			std::string& error)
		{
			output.clear();
			std::ifstream stream(path, std::ios::binary | std::ios::ate);
			if(!stream)
			{
				error = "The registered animation asset could not be opened.";
				return false;
			}
			const std::streamoff length = stream.tellg();
			if(length < 0 || static_cast<std::uint64_t>(length) > maximumBytes)
			{
				error = "The registered animation asset exceeds its bounded edit size.";
				return false;
			}
			output.resize(static_cast<std::size_t>(length));
			stream.seekg(0, std::ios::beg);
			if(!output.empty()
				&& !stream.read(reinterpret_cast<char*>(output.data()),
					static_cast<std::streamsize>(output.size())))
			{
				output.clear();
				error = "The registered animation asset ended during the bounded read.";
				return false;
			}
			return true;
		}

		std::filesystem::path MakeStagedPath(const std::filesystem::path& target)
		{
			const std::uint64_t sequence = g_AnimationAutomationStageSequence.fetch_add(
				1U, std::memory_order_relaxed);
			return target.parent_path()
				/ (target.filename().string() + ".automation-" + std::to_string(sequence) + ".staged");
		}

		bool WriteCompleteFile(
			const std::filesystem::path& path,
			std::span<const std::uint8_t> bytes,
			std::string& error)
		{
			std::ofstream stream(path, std::ios::binary | std::ios::trunc);
			if(!stream)
			{
				error = "Could not create the staged animation document.";
				return false;
			}
			if(!bytes.empty())
			{
				stream.write(reinterpret_cast<const char*>(bytes.data()),
					static_cast<std::streamsize>(bytes.size()));
			}
			stream.flush();
			if(!stream)
			{
				error = "Could not flush the complete staged animation document.";
				return false;
			}
			stream.close();
			if(!stream)
			{
				error = "Could not close the complete staged animation document.";
				return false;
			}
			return true;
		}

		bool ReplaceFileAtomically(
			const std::filesystem::path& staged,
			const std::filesystem::path& target,
			std::string& error)
		{
		#ifdef _WIN32
			if(MoveFileExW(
				staged.c_str(), target.c_str(),
				MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH) == FALSE)
			{
				error = "Could not atomically replace the animation document (Windows error "
					+ std::to_string(GetLastError()) + ").";
				return false;
			}
		#else
			std::error_code replaceError;
			std::filesystem::rename(staged, target, replaceError);
			if(replaceError)
			{
				error = "Could not atomically replace the animation document: "
					+ replaceError.message();
				return false;
			}
		#endif
			return true;
		}

		bool PublishSourceBytes(
			const std::filesystem::path& target,
			std::span<const std::uint8_t> bytes,
			std::string& error)
		{
			const std::filesystem::path staged = MakeStagedPath(target);
			std::error_code cleanupError;
			if(!WriteCompleteFile(staged, bytes, error))
			{
				std::filesystem::remove(staged, cleanupError);
				return false;
			}
			if(!ReplaceFileAtomically(staged, target, error))
			{
				std::filesystem::remove(staged, cleanupError);
				return false;
			}
			return true;
		}

		std::optional<AssetType> RuntimeTypeForImporter(std::string_view importer)
		{
			if(importer == "SkeletonAssetImporter") return AssetType::Skeleton;
			if(importer == "AnimationClipAssetImporter") return AssetType::AnimationClip;
			if(importer == "AnimationControllerAssetImporter") return AssetType::AnimationController;
			if(importer == "AnimationMaskAssetImporter") return AssetType::AnimationMask;
			if(importer == "RetargetProfileAssetImporter") return AssetType::RetargetProfile;
			if(importer == "MotionDatabaseAssetImporter") return AssetType::MotionDatabase;
			if(importer == "RagdollProfileAssetImporter") return AssetType::RagdollProfile;
			return std::nullopt;
		}
	}

	AutomationDispatchResult CommitCookedAnimationAssetEdit(
		AssetDatabase& assetDatabase,
		const TerraEngine::UUID& assetId,
		std::string_view requiredImporter,
		std::size_t maximumBytes,
		bool dryRun,
		const AnimationCookedAssetEdit& edit)
	{
		if(assetId == TerraEngine::EMPTY_UUID || requiredImporter.empty() || maximumBytes == 0U || !edit)
		{
			return Fail(ErrorCategory::Validation, "The cooked animation edit context is incomplete.");
		}
		const AssetMetadata* metadata = assetDatabase.FindAssetByGuid(assetId);
		if(metadata == nullptr || metadata->importerName != requiredImporter)
		{
			return Fail(ErrorCategory::MissingTarget,
				"The requested asset is not registered with the required animation importer.");
		}

		const std::filesystem::path sourcePath = std::filesystem::absolute(
			assetDatabase.GetAssetRoot() / metadata->sourcePath).lexically_normal();
		const std::filesystem::path compiledPath = std::filesystem::absolute(
			assetDatabase.GetBuildRoot() / metadata->compiledPath).lexically_normal();
		std::vector<std::uint8_t> originalBytes;
		std::string ioError;
		if(!ReadBoundedFile(sourcePath, maximumBytes, originalBytes, ioError))
		{
			return Fail(ErrorCategory::Unavailable, std::move(ioError));
		}

		TerraEngine::Animation::AnimationAssetReader envelope;
		if(envelope.Parse(originalBytes) != TerraEngine::Animation::AnimationStatus::Ok
			|| envelope.Envelope().assetId.value != assetId)
		{
			return Fail(ErrorCategory::Validation,
				"The registered source is not a checked cooked animation envelope for this asset ID.");
		}

		std::vector<std::uint8_t> candidateBytes;
		AutomationDispatchResult prepared = edit(originalBytes, candidateBytes);
		if(!prepared.ok)
		{
			return prepared;
		}
		if(candidateBytes.empty() || candidateBytes.size() > maximumBytes)
		{
			return Fail(ErrorCategory::OverBudget,
				"The authored candidate is empty or exceeds the bounded cooked asset size.");
		}
		if(candidateBytes == originalBytes)
		{
			return Fail(ErrorCategory::Conflict,
				"The requested authored edit is a no-op against the current animation document generation.");
		}
		TerraEngine::Animation::AnimationAssetReader candidateEnvelope;
		if(candidateEnvelope.Parse(candidateBytes) != TerraEngine::Animation::AnimationStatus::Ok
			|| candidateEnvelope.Envelope().assetId.value != assetId)
		{
			return Fail(ErrorCategory::Validation,
				"The authored candidate did not serialize as the same checked animation asset.");
		}

		if(!prepared.data.is_object())
		{
			prepared.data = nlohmann::json::object();
		}
		prepared.data["assetId"] = TerraEngine::ToHexString(assetId);
		prepared.data["sourcePath"] = metadata->sourcePath.generic_string();
		prepared.data["compiledPath"] = metadata->compiledPath.generic_string();
		prepared.data["candidateBytes"] = candidateBytes.size();
		if(dryRun)
		{
			prepared.message = "Validated the authored animation asset candidate without publishing it.";
			prepared.data["publication"] = "none_dry_run";
			return prepared;
		}

		// A watcher notification from the preceding authored save may still own a
		// worker compile for this same source. Finish only that matching job before
		// publishing the next candidate. This protected-boundary wait prevents the
		// synchronous read-after-write import below from being coalesced behind an
		// older worker and then comparing the candidate against stale build bytes.
		assetDatabase.FinalizeActiveImportForExplicitReimport(sourcePath);
		if(!PublishSourceBytes(sourcePath, candidateBytes, ioError))
		{
			return Fail(ErrorCategory::Internal, std::move(ioError));
		}
		assetDatabase.ImportAsset(sourcePath, true);

		std::vector<std::uint8_t> publishedBytes;
		const bool published = ReadBoundedFile(compiledPath, maximumBytes, publishedBytes, ioError)
			&& publishedBytes == candidateBytes;
		if(!published)
		{
			std::string rollbackError;
			const bool sourceRestored = PublishSourceBytes(sourcePath, originalBytes, rollbackError);
			if(sourceRestored)
			{
				assetDatabase.ImportAsset(sourcePath, true);
			}
			AutomationDispatchResult failed = Fail(ErrorCategory::Internal,
				"AssetDatabase did not publish the complete authored animation candidate; the prior source was restored.");
			failed.data = {
				{"sourceRestored", sourceRestored},
				{"publishError", ioError},
				{"rollbackError", rollbackError}
			};
			return failed;
		}

		bool activeResourceReloaded = false;
		if(const std::optional<AssetType> type = RuntimeTypeForImporter(requiredImporter))
		{
			// A false result normally means the asset was not resident. The new path and
			// bytes are still registered for its first load, so that is not a failure.
			activeResourceReloaded = ResourceManager::Instance().ReloadAnimationAsset(assetId, *type);
		}
		prepared.message = "Published the validated animation asset through AssetDatabase.";
		prepared.data["publication"] = "asset_database_atomic_copy";
		prepared.data["activeResourceReloaded"] = activeResourceReloaded;
		return prepared;
	}

	AutomationDispatchResult ExecuteAnimationAssetOperation(
		std::string_view operation,
		const nlohmann::json& arguments,
		AnimationAutomationTransactions& transactions,
		std::uint64_t generation)
	{
		AutomationDispatchResult validated = ValidateAnimationAutomationRequest(operation, arguments);
		if(!validated.ok)
		{
			return validated;
		}
		if(operation == "animation.asset.inspect")
		{
			validated.data["assetId"] = arguments.at("assetId");
			validated.data["immutable"] = true;
			return validated;
		}
		AnimationAutomationMutation mutation{
			std::string(operation),
			arguments.value("idempotencyKey", ""),
			arguments.value("correlationId", ""),
			arguments.value("expectedGeneration", 0ULL),
			arguments.value("dryRun", false),
			arguments};
		return transactions.Execute(mutation, generation);
	}
}
