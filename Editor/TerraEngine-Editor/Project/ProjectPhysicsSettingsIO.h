// ProjectPhysicsSettingsIO.h - Validated project physics settings loading.
//
// The editor window and Play Mode lifecycle both consume this helper so the
// values shown to authors are exactly the values used to create a physics world.

#pragma once

#include "ProjectSettings.h"
#include "Engine/Physics/PhysicsSettings.h"

#include <filesystem>
#include <fstream>
#include <nlohmann/json.hpp>
#include <string>
#include <utility>

namespace TerraEngine::Editor::ProjectSettings
{
	// ReadPhysicsSettings loads one complete provider-neutral settings document.
	// A missing file is valid and resolves to engine defaults, while malformed or
	// unsupported documents are rejected before they can replace a live world.
	inline bool ReadPhysicsSettings(
		const std::filesystem::path& path,
		TerraEngine::Physics::PhysicsSettings& output,
		std::string& error)
	{
		TerraEngine::Physics::PhysicsSettings candidate =
			TerraEngine::Physics::PhysicsSettings::Defaults();
		if (!std::filesystem::exists(path))
		{
			output = candidate;
			return true;
		}

		std::ifstream input(path);
		if (!input)
		{
			error = "Physics Settings could not be opened.";
			return false;
		}

		try
		{
			nlohmann::json document;
			input >> document;
			candidate.schemaVersion = document.value("schemaVersion", candidate.schemaVersion);
			if (document.contains("gravity") && document["gravity"].is_array()
				&& document["gravity"].size() == 3U)
			{
				candidate.gravity = {
					document["gravity"][0].get<float>(),
					document["gravity"][1].get<float>(),
					document["gravity"][2].get<float>()
				};
			}
			candidate.fixedTimeStep = document.value("fixedTimeStep", candidate.fixedTimeStep);
			candidate.maxSubsteps = document.value("maxSubsteps", candidate.maxSubsteps);
			candidate.maxAccumulatedTime = document.value("maxAccumulatedTime", candidate.maxAccumulatedTime);
			candidate.velocityIterations = document.value("velocityIterations", candidate.velocityIterations);
			candidate.positionIterations = document.value("positionIterations", candidate.positionIterations);
			candidate.maxCastDistance = document.value("maxCastDistance", candidate.maxCastDistance);
			candidate.maxShapeExtent = document.value("maxShapeExtent", candidate.maxShapeExtent);
			candidate.maxHitsPerQuery = document.value("maxHitsPerQuery", candidate.maxHitsPerQuery);
			candidate.maxRequestsPerBatch = document.value("maxRequestsPerBatch", candidate.maxRequestsPerBatch);
			candidate.retainedResultTicks = document.value("retainedResultTicks", candidate.retainedResultTicks);
			candidate.debugVisualizationEnabled = document.value("debugVisualizationEnabled", candidate.debugVisualizationEnabled);
			candidate.retainedDebugTicks = document.value("retainedDebugTicks", candidate.retainedDebugTicks);
			candidate.maxDebugPrimitives = document.value("maxDebugPrimitives", candidate.maxDebugPrimitives);
			candidate.transformPublicationChunkSize = document.value("transformPublicationChunkSize", candidate.transformPublicationChunkSize);
			candidate.providerExtractionBatchSize = document.value("providerExtractionBatchSize", candidate.providerExtractionBatchSize);
			candidate.parallelPreparationThreshold = document.value("parallelPreparationThreshold", candidate.parallelPreparationThreshold);
			candidate.parallelExtractionThreshold = document.value("parallelExtractionThreshold", candidate.parallelExtractionThreshold);
			candidate.aggregateDiagnosticsIntervalFrames = document.value("aggregateDiagnosticsIntervalFrames", candidate.aggregateDiagnosticsIntervalFrames);
			candidate.continuousCollisionEnabled = document.value("continuousCollisionEnabled", candidate.continuousCollisionEnabled);

			if (document.contains("layers") && document["layers"].is_array()
				&& document["layers"].size() == candidate.layers.size())
			{
				for (std::size_t index = 0U; index < candidate.layers.size(); ++index)
				{
					const nlohmann::json& source = document["layers"][index];
					auto& destination = candidate.layers[index];
					destination.id = source.value("id", destination.id);
					destination.name = source.value("name", destination.name);
					destination.collisionMask = source.value("collisionMask", destination.collisionMask);
					destination.broadPhaseGroup = source.value("broadPhaseGroup", destination.broadPhaseGroup);
					destination.enabled = source.value("enabled", destination.enabled);
				}
			}

			const TerraEngine::Physics::PhysicsSettingsValidationResult validation = candidate.Validate();
			if (!validation.accepted)
			{
				error = validation.message;
				return false;
			}
			output = std::move(candidate);
			return true;
		}
		catch (const std::exception& exception)
		{
			error = std::string("Physics Settings JSON is invalid: ") + exception.what();
			return false;
		}
	}

	// ReadProjectPhysicsSettings resolves the canonical project-owned path used by
	// editor Play Mode, the settings UI, exports, and installed validation tools.
	inline bool ReadProjectPhysicsSettings(
		const std::filesystem::path& projectRoot,
		TerraEngine::Physics::PhysicsSettings& output,
		std::string& error)
	{
		return ReadPhysicsSettings(projectRoot / kPhysicsSettingsRelativePath, output, error);
	}
}
