// PhysicsSettings.cpp - defaults and bounded validation shared by every surface.

#include "PhysicsSettings.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <iomanip>
#include <sstream>
#include <unordered_set>

namespace TerraEngine::Physics {
	namespace {
		PhysicsSettingsValidationResult Reject(
			PhysicsSettingsValidationCode code,
			std::string message,
			std::size_t layerIndex = kPhysicsCollisionLayerCount) {
			return PhysicsSettingsValidationResult{false, code, std::move(message), layerIndex};
		}

		std::string NormalizedLayerName(std::string_view name) {
			// Layer names are compared case-insensitively so "Player" and "player"
			// cannot serialize as two visually ambiguous stable layer identities.
			std::string normalized(name);
			std::transform(normalized.begin(), normalized.end(), normalized.begin(), [](unsigned char character) {
				return static_cast<char>(std::tolower(character));
			});
			return normalized;
		}
	}

	PhysicsSettings PhysicsSettings::Defaults() {
		PhysicsSettings settings;
		for(std::size_t index = 0; index < settings.layers.size(); ++index) {
			CollisionLayer& layer = settings.layers[index];
			layer.id = static_cast<std::uint8_t>(index);
			layer.collisionMask = 0xFFFFFFFFU;
			layer.broadPhaseGroup = 0U;
			layer.enabled = true;

			if(index == 0U) {
				layer.name = "Default";
			} else {
				std::ostringstream name;
				name << "Layer " << std::setw(2) << std::setfill('0') << index;
				layer.name = name.str();
			}
		}
		return settings;
	}

	PhysicsSettingsValidationResult PhysicsSettings::Validate() const {
		if(schemaVersion != kPhysicsSettingsSchemaVersion) {
			return Reject(PhysicsSettingsValidationCode::UnsupportedSchema,
				"PhysicsSettings schema version is unsupported and requires an explicit migration.");
		}
		if(!gravity.IsFinite()) {
			return Reject(PhysicsSettingsValidationCode::NonfiniteGravity,
				"World gravity must contain finite metres-per-second-squared values.");
		}
		if(!std::isfinite(fixedTimeStep) || fixedTimeStep <= 0.0F || fixedTimeStep > 0.1F) {
			return Reject(PhysicsSettingsValidationCode::InvalidFixedTimeStep,
				"Fixed timestep must be finite, positive, and no greater than 0.1 seconds.");
		}
		if(maxSubsteps == 0U || maxSubsteps > 16U) {
			return Reject(PhysicsSettingsValidationCode::InvalidSubstepLimit,
				"Maximum substeps must be between 1 and 16.");
		}
		if(!std::isfinite(maxAccumulatedTime)
			|| maxAccumulatedTime < fixedTimeStep
			|| maxAccumulatedTime > 1.0F) {
			return Reject(PhysicsSettingsValidationCode::InvalidAccumulatorLimit,
				"Accumulator clamp must hold at least one fixed tick and remain at or below one second.");
		}
		// Jolt requires at least two velocity iterations for friction and stores
		// per-constraint iteration overrides in an eight-bit value.
		if(velocityIterations < 2U || velocityIterations > 255U) {
			return Reject(PhysicsSettingsValidationCode::InvalidVelocityIterations,
				"Velocity iterations must remain between 2 and 255.");
		}
		if(positionIterations < 1U || positionIterations > 255U) {
			return Reject(PhysicsSettingsValidationCode::InvalidPositionIterations,
				"Position iterations must remain between 1 and 255.");
		}

		std::array<bool, kPhysicsCollisionLayerCount> seenIds{};
		std::unordered_set<std::string> seenNames;
		for(std::size_t index = 0; index < layers.size(); ++index) {
			const CollisionLayer& layer = layers[index];
			if(layer.id >= kPhysicsCollisionLayerCount) {
				return Reject(PhysicsSettingsValidationCode::InvalidLayerId,
					"Collision layer ID must be between 0 and 31.", index);
			}
			if(seenIds[layer.id]) {
				return Reject(PhysicsSettingsValidationCode::DuplicateLayerId,
					"Collision layer IDs must be unique and stable.", index);
			}
			seenIds[layer.id] = true;
			if(layer.name.empty()) {
				return Reject(PhysicsSettingsValidationCode::EmptyLayerName,
					"Collision layer names must not be empty.", index);
			}
			if(!seenNames.insert(NormalizedLayerName(layer.name)).second) {
				return Reject(PhysicsSettingsValidationCode::DuplicateLayerName,
					"Collision layer names must be unique without case ambiguity.", index);
			}
			if(layer.broadPhaseGroup >= kPhysicsCollisionLayerCount) {
				return Reject(PhysicsSettingsValidationCode::InvalidBroadPhaseGroup,
					"Broad-phase group must use a stable value between 0 and 31.", index);
			}
		}

		if(!std::isfinite(maxCastDistance)
			|| maxCastDistance <= 0.0F
			|| maxCastDistance > kPhysicsMaximumCastDistance
			|| !std::isfinite(maxShapeExtent)
			|| maxShapeExtent <= 0.0F
			|| maxShapeExtent > kPhysicsMaximumShapeExtent
			|| maxHitsPerQuery == 0U
			|| maxHitsPerQuery > kPhysicsMaximumHitsPerQuery
			|| maxRequestsPerBatch == 0U
			|| maxRequestsPerBatch > kPhysicsMaximumRequestsPerBatch) {
			return Reject(PhysicsSettingsValidationCode::InvalidQueryLimit,
				"Query limits must be positive and no greater than the calibrated provider bounds.");
		}
		if(retainedResultTicks == 0U || retainedResultTicks > kPhysicsRetainedResultTicks) {
			return Reject(PhysicsSettingsValidationCode::InvalidHistoryLimit,
				"Query result retention must stay within the calibrated two-tick history.");
		}
		if(retainedDebugTicks == 0U || retainedDebugTicks > kPhysicsMaximumDebugHistoryTicks
			|| maxDebugPrimitives == 0U || maxDebugPrimitives > kPhysicsMaximumDebugPrimitives) {
			return Reject(PhysicsSettingsValidationCode::InvalidDebugLimit,
				"Debug history and primitive limits must stay within their calibrated bounds.");
		}
		if(transformPublicationChunkSize < kPhysicsMinimumPublicationBatchSize
			|| transformPublicationChunkSize > kPhysicsMaximumPublicationBatchSize
			|| providerExtractionBatchSize < kPhysicsMinimumPublicationBatchSize
			|| providerExtractionBatchSize > kPhysicsMaximumPublicationBatchSize) {
			return Reject(PhysicsSettingsValidationCode::InvalidPublicationBatchSize,
				"Physics publication and extraction batch sizes must remain between 64 and 8,192 records.");
		}
		if(parallelPreparationThreshold == 0U
			|| parallelPreparationThreshold > kPhysicsMaximumParallelThreshold
			|| parallelExtractionThreshold == 0U
			|| parallelExtractionThreshold > kPhysicsMaximumParallelThreshold) {
			return Reject(PhysicsSettingsValidationCode::InvalidParallelThreshold,
				"Physics parallel thresholds must remain between 1 and 1,000,000 work items.");
		}
		if(aggregateDiagnosticsIntervalFrames == 0U
			|| aggregateDiagnosticsIntervalFrames > kPhysicsMaximumDiagnosticsIntervalFrames) {
			return Reject(PhysicsSettingsValidationCode::InvalidDiagnosticsInterval,
				"Aggregate physics diagnostics must publish every 1 to 600 frames.");
		}

		return PhysicsSettingsValidationResult{true, PhysicsSettingsValidationCode::None, {}, kPhysicsCollisionLayerCount};
	}

	std::string_view ToString(PhysicsSettingsValidationCode code) {
		switch(code) {
		case PhysicsSettingsValidationCode::None: return "None";
		case PhysicsSettingsValidationCode::UnsupportedSchema: return "UnsupportedSchema";
		case PhysicsSettingsValidationCode::NonfiniteGravity: return "NonfiniteGravity";
		case PhysicsSettingsValidationCode::InvalidFixedTimeStep: return "InvalidFixedTimeStep";
		case PhysicsSettingsValidationCode::InvalidSubstepLimit: return "InvalidSubstepLimit";
		case PhysicsSettingsValidationCode::InvalidAccumulatorLimit: return "InvalidAccumulatorLimit";
		case PhysicsSettingsValidationCode::InvalidVelocityIterations: return "InvalidVelocityIterations";
		case PhysicsSettingsValidationCode::InvalidPositionIterations: return "InvalidPositionIterations";
		case PhysicsSettingsValidationCode::InvalidLayerId: return "InvalidLayerId";
		case PhysicsSettingsValidationCode::EmptyLayerName: return "EmptyLayerName";
		case PhysicsSettingsValidationCode::DuplicateLayerName: return "DuplicateLayerName";
		case PhysicsSettingsValidationCode::DuplicateLayerId: return "DuplicateLayerId";
		case PhysicsSettingsValidationCode::InvalidBroadPhaseGroup: return "InvalidBroadPhaseGroup";
		case PhysicsSettingsValidationCode::InvalidQueryLimit: return "InvalidQueryLimit";
		case PhysicsSettingsValidationCode::InvalidHistoryLimit: return "InvalidHistoryLimit";
		case PhysicsSettingsValidationCode::InvalidDebugLimit: return "InvalidDebugLimit";
		case PhysicsSettingsValidationCode::InvalidPublicationBatchSize: return "InvalidPublicationBatchSize";
		case PhysicsSettingsValidationCode::InvalidParallelThreshold: return "InvalidParallelThreshold";
		case PhysicsSettingsValidationCode::InvalidDiagnosticsInterval: return "InvalidDiagnosticsInterval";
		}
		return "Unknown";
	}
}
