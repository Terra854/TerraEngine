// PhysicsMaterialAssetWriter.cpp - Validated staged persistence for .tephysmat.

#include "PhysicsMaterialAssetWriter.h"

#include <fstream>
#include <system_error>
#include <utility>

namespace TerraEngine::AssetPipeline
{
	namespace
	{
		bool PublishStagedFile(const std::filesystem::path& stagedPath, const std::filesystem::path& outputPath)
		{
			std::error_code error;
			if(!outputPath.parent_path().empty())
				std::filesystem::create_directories(outputPath.parent_path(), error);
			if(error)
				return false;

			// Rename within one directory is the protected publication boundary.
			// Windows does not replace an existing target, so retain a short-lived
			// backup and restore it if publishing the staged replacement fails.
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

	PhysicsMaterialAssetWriteResult WritePhysicsMaterialAsset(
		const std::filesystem::path& outputPath,
		const Physics::PhysicsMaterial& material)
	{
		PhysicsMaterialAssetWriteResult result;
		if(outputPath.extension() != Physics::kPhysicsMaterialExtension)
		{
			result.message = "Physics material paths must use the .tephysmat extension.";
			result.diagnostics.Add(Physics::PhysicsMaterialDiagnosticSeverity::Error, "invalid_extension", result.message);
			return result;
		}

		const Physics::PhysicsMaterialValidationResult validation = material.Validate();
		if(!validation.accepted)
		{
			result.message = validation.message;
			result.diagnostics.Add(Physics::PhysicsMaterialDiagnosticSeverity::Error, std::string(Physics::ToString(validation.code)), validation.message);
			return result;
		}

		std::error_code error;
		if(!outputPath.parent_path().empty())
			std::filesystem::create_directories(outputPath.parent_path(), error);
		if(error)
		{
			result.message = "Could not create the physics material output directory: " + error.message();
			result.diagnostics.Add(Physics::PhysicsMaterialDiagnosticSeverity::Error, "create_directory_failed", result.message);
			return result;
		}

		const std::filesystem::path stagedPath = outputPath.string() + ".staged";
		std::filesystem::remove(stagedPath, error);
		{
			std::ofstream stream(stagedPath, std::ios::binary | std::ios::trunc);
			if(!stream)
			{
				result.message = "Could not open the physics material staging file.";
				result.diagnostics.Add(Physics::PhysicsMaterialDiagnosticSeverity::Error, "open_stage_failed", result.message);
				return result;
			}
			stream << material.ToJson().dump(2) << '\n';
			if(!stream)
			{
				result.message = "Could not write the physics material staging file.";
				result.diagnostics.Add(Physics::PhysicsMaterialDiagnosticSeverity::Error, "write_stage_failed", result.message);
				stream.close();
				std::filesystem::remove(stagedPath, error);
				return result;
			}
		}

		Physics::PhysicsMaterial stagedMaterial;
		if(!LoadPhysicsMaterialAsset(stagedPath, stagedMaterial, result.diagnostics)
			|| stagedMaterial.assetId != material.assetId)
		{
			result.message = "The staged physics material did not pass read-back validation.";
			result.diagnostics.Add(Physics::PhysicsMaterialDiagnosticSeverity::Error, "stage_validation_failed", result.message);
			std::filesystem::remove(stagedPath, error);
			return result;
		}

		if(!PublishStagedFile(stagedPath, outputPath))
		{
			result.message = "Could not publish the staged physics material; the previous asset was preserved.";
			result.diagnostics.Add(Physics::PhysicsMaterialDiagnosticSeverity::Error, "publish_failed", result.message);
			std::filesystem::remove(stagedPath, error);
			return result;
		}

		result.published = true;
		result.message = "Physics material published.";
		return result;
	}

	bool LoadPhysicsMaterialAsset(
		const std::filesystem::path& inputPath,
		Physics::PhysicsMaterial& destination,
		Physics::PhysicsMaterialDiagnostics& diagnostics)
	{
		if(inputPath.extension() != Physics::kPhysicsMaterialExtension && inputPath.extension() != ".staged")
		{
			diagnostics.Add(Physics::PhysicsMaterialDiagnosticSeverity::Error, "invalid_extension", "Physics material paths must use the .tephysmat extension.");
			return false;
		}

		std::ifstream stream(inputPath, std::ios::binary);
		if(!stream)
		{
			diagnostics.Add(Physics::PhysicsMaterialDiagnosticSeverity::Error, "open_failed", "Could not open the physics material asset.");
			return false;
		}

		try
		{
			const nlohmann::json document = nlohmann::json::parse(stream);
			Physics::PhysicsMaterial parsed;
			if(!Physics::PhysicsMaterial::LoadJson(document, parsed, diagnostics))
				return false;
			destination = std::move(parsed);
			return true;
		}
		catch(const nlohmann::json::exception& exception)
		{
			diagnostics.Add(Physics::PhysicsMaterialDiagnosticSeverity::Error, "invalid_json", exception.what());
			return false;
		}
	}
}
