// TerraEngineModelPackageCompiler.cpp - Process-isolated glTF/FBX package compiler.
//
// The editor launches this tool for large model package imports so cancellation
// can terminate a child process instead of trying to kill an unsafe in-process
// C++ worker thread.

#include <algorithm>
#include <filesystem>
#include <iostream>
#include <optional>
#include <string>
#include <string_view>

#include <nlohmann/json.hpp>

#include "Editor/AssetCompilers/ModelPackageImporter.h"
#include "Editor/TerraEngine-Editor/Assets/AssetImportResult.h"

namespace
{
	struct CommandLine
	{
		std::filesystem::path sourcePath;
		std::filesystem::path assetRoot;
		std::filesystem::path buildRoot;
		std::filesystem::path sourceRelativePath;
		TerraEngine::AssetPipeline::ModelPackagePreparationRoots preparationRoots;
		TerraEngine::UUID packageGuid = TerraEngine::EMPTY_UUID;
		TerraEngine::UUID shaderGuid = TerraEngine::EMPTY_UUID;
		bool showHelp = false;
	};

	void PrintUsage()
	{
		std::cerr << "Usage: TerraEngineModelPackageCompiler --source <model.gltf|model.glb|model.fbx> --asset-root <Assets> --build-root <Build/Assets> --source-relative <relative path> --package-guid <uuid> --shader-guid <uuid> [--prepare-asset-root <private Assets> --prepare-build-root <private Build>]\n";
	}

	bool ParseUuidArgument(std::string_view value, TerraEngine::UUID& outUuid)
	{
		const std::optional<TerraEngine::UUID> parsed = TerraEngine::ParseUUID(value);
		if (!parsed.has_value())
		{
			return false;
		}
		outUuid = *parsed;
		return true;
	}

	bool ParseCommandLine(int argc, char** argv, CommandLine& args)
	{
		for (int index = 1; index < argc; ++index)
		{
			const std::string_view flag{ argv[index] };
			if (flag == "-h" || flag == "--help")
			{
				args.showHelp = true;
				return true;
			}
			if (index + 1 >= argc)
			{
				std::cerr << "Missing value after " << flag << ".\n";
				return false;
			}

			if (flag == "--source" || flag == "-i")
			{
				args.sourcePath = argv[++index];
			}
			else if (flag == "--asset-root")
			{
				args.assetRoot = argv[++index];
			}
			else if (flag == "--build-root")
			{
				args.buildRoot = argv[++index];
			}
			else if (flag == "--source-relative")
			{
				args.sourceRelativePath = argv[++index];
			}
			else if (flag == "--prepare-asset-root")
			{
				args.preparationRoots.assetRoot = argv[++index];
			}
			else if (flag == "--prepare-build-root")
			{
				args.preparationRoots.buildRoot = argv[++index];
			}
			else if (flag == "--package-guid")
			{
				if (!ParseUuidArgument(argv[++index], args.packageGuid))
				{
					std::cerr << "Invalid package GUID.\n";
					return false;
				}
			}
			else if (flag == "--shader-guid")
			{
				if (!ParseUuidArgument(argv[++index], args.shaderGuid))
				{
					std::cerr << "Invalid shader GUID.\n";
					return false;
				}
			}
			else
			{
				std::cerr << "Unrecognized argument: " << flag << "\n";
				return false;
			}
		}

		if (args.showHelp)
		{
			return true;
		}

		if (args.sourcePath.empty()
			|| args.assetRoot.empty()
			|| args.buildRoot.empty()
			|| args.sourceRelativePath.empty()
			|| args.packageGuid == TerraEngine::EMPTY_UUID
			|| args.preparationRoots.assetRoot.empty() != args.preparationRoots.buildRoot.empty())
		{
			std::cerr << "Required model package compile arguments are missing.\n";
			return false;
		}
		return true;
	}

	void EmitProgress(float progress, std::string_view stage, std::string_view message)
	{
		nlohmann::json progressDocument;
		progressDocument["type"] = "progress";
		progressDocument["progress"] = progress;
		progressDocument["stage"] = std::string(stage);
		progressDocument["message"] = std::string(message);
		std::cout << progressDocument.dump() << std::endl;
	}

	void ApplyPreviousVirtualGeometryOverrides(
		const TerraEngine::AssetPipeline::ModelPackageImportResult& previousResult,
		TerraEngine::AssetPipeline::ModelPackageImportOptions& options)
	{
		options.virtualGeometryMeshOverrides.clear();
		for (const TerraEngine::AssetPipeline::GeneratedModelMeshAsset& previousMesh : previousResult.generatedMeshes)
		{
			if (previousMesh.virtualGeometrySettings.hasOverride)
			{
				// Preserve the child GUID with its recipe. Applying only the first
				// saved override globally makes unrelated package meshes inherit it.
				options.virtualGeometryMeshOverrides.push_back({
					previousMesh.guid,
					previousMesh.virtualGeometrySettings });
			}
		}
	}

	void RestorePerMeshVirtualGeometryOverrides(
		const TerraEngine::AssetPipeline::ModelPackageImportResult& previousResult,
		TerraEngine::AssetPipeline::ModelPackageImportResult& importResult)
	{
		for (TerraEngine::AssetPipeline::GeneratedModelMeshAsset& mesh : importResult.generatedMeshes)
		{
			for (const TerraEngine::AssetPipeline::GeneratedModelMeshAsset& previousMesh : previousResult.generatedMeshes)
			{
				if (mesh.guid == previousMesh.guid && previousMesh.virtualGeometrySettings.hasOverride)
				{
					mesh.virtualGeometrySettings = previousMesh.virtualGeometrySettings;
					break;
				}
			}
		}
	}
}

int main(int argc, char** argv)
{
	CommandLine args{};
	if (!ParseCommandLine(argc, argv, args))
	{
		PrintUsage();
		return 1;
	}
	if (args.showHelp)
	{
		PrintUsage();
		return 0;
	}

	TerraEngine::AssetPipeline::ModelPackageImportOptions options{};
	options.sourcePath = args.sourcePath;
	options.assetRoot = args.assetRoot;
	options.buildRoot = args.buildRoot;
	options.sourceRelativePath = args.sourceRelativePath;
	options.packageGuid = args.packageGuid;
	options.shaderCollectionId = args.shaderGuid;
	options.forceReimport = true;
	if(!args.preparationRoots.assetRoot.empty()) options.preparationRoots = args.preparationRoots;
	options.progressCallback = [](float progress, std::string_view stage, std::string_view message) {
		EmitProgress(progress, stage, message);
	};

	TerraEngine::AssetPipeline::ModelPackageImportResult previousResult;
	const bool hasPreviousResult = TerraEngine::AssetPipeline::LoadModelPackageImportResult(args.sourcePath, previousResult);
	if (hasPreviousResult)
	{
		ApplyPreviousVirtualGeometryOverrides(previousResult, options);
	}

	EmitProgress(0.01f, "Starting", "Launching model package compiler.");
	TerraEngine::AssetPipeline::ModelPackageImportResult importResult = TerraEngine::AssetPipeline::ImportModelPackage(options);
	if (hasPreviousResult)
	{
		RestorePerMeshVirtualGeometryOverrides(previousResult, importResult);
	}

	// Preparation never writes even a failure report beside authored sources.
	// Successful candidates keep their sidecar with their private asset outputs.
	if(options.preparationRoots && importResult.status == TerraEngine::AssetPipeline::ModelImportStatus::Failed) return 1;
	const auto resultSourcePath = options.preparationRoots
		? options.preparationRoots->assetRoot / args.sourceRelativePath : args.sourcePath;
	if (!TerraEngine::AssetPipeline::SaveModelPackageImportResult(resultSourcePath, importResult))
	{
		std::cerr << "Failed to save model package import result sidecar for " << args.sourcePath << "\n";
		return 1;
	}

	const bool wroteCompiledMesh =
		!importResult.generatedMeshes.empty()
		&& std::all_of(
			importResult.generatedMeshes.begin(),
			importResult.generatedMeshes.end(),
			[&args, &options](const TerraEngine::AssetPipeline::GeneratedModelMeshAsset& mesh) {
				// Model packages now publish renderable child meshes. The compiler
				// succeeds only when every generated child that appears in the
				// sidecar has a matching compiled runtime mesh in Build/Assets.
				const auto& outputRoot = options.preparationRoots ? options.preparationRoots->buildRoot : args.buildRoot;
				return !mesh.compiledPath.empty() && std::filesystem::exists(outputRoot / mesh.compiledPath);
			});
	const bool succeeded = importResult.status != TerraEngine::AssetPipeline::ModelImportStatus::Failed && wroteCompiledMesh;
	EmitProgress(succeeded ? 1.0f : 0.99f, succeeded ? "Complete" : "Failed", succeeded ? "Model package compile finished." : "Model package compile failed.");
	return succeeded ? 0 : 1;
}
