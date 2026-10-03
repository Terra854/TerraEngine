#include "Audio/AudioAuthoringSnapshot.h"
#include <algorithm>
#include <cmath>
#include <stdexcept>

namespace TerraEngine::Audio {
	// Validate coordinate ownership before immutable records can reach worker jobs.
	AudioAuthoringSnapshot AudioAuthoringSnapshot::Create(AudioAuthoringSnapshotData data) {
		if(data.coordinateMode != AudioCoordinateMode::CanonicalWorld && data.coordinateMode != AudioCoordinateMode::SharedLocalFrame)
			throw std::invalid_argument("Audio snapshot has an unknown coordinate mode.");
		const bool canonical = data.coordinateMode == AudioCoordinateMode::CanonicalWorld;
		const auto finite = [](AudioVector3 value) { return std::isfinite(value.x) && std::isfinite(value.y) && std::isfinite(value.z); };
		const auto validatePosition = [&](auto& record) {
			if(record.worldPosition.has_value() != canonical)
				throw std::invalid_argument("Audio snapshot mixes canonical-world and shared-local coordinates.");
			if(canonical) {
				const auto normalized = TerraEngine::Scene::NormalizeWorldPosition(*record.worldPosition);
				if(!normalized.Succeeded()) throw std::invalid_argument("Audio snapshot has an invalid canonical position.");
				record.worldPosition = normalized.position;
			} else if(!finite(record.position)) throw std::invalid_argument("Audio snapshot has a non-finite local position.");
		};
		for(auto& source : data.sources) {
			validatePosition(source);
			if(!std::isfinite(source.maximumDistance)) throw std::invalid_argument("Audio snapshot has a non-finite source range.");
		}
		for(auto& listener : data.listeners) {
			validatePosition(listener);
			if(!finite(listener.forward)) throw std::invalid_argument("Audio snapshot has a non-finite listener basis.");
		}
		// Stable identity ordering makes selection independent of registry iteration.
		std::sort(data.listeners.begin(), data.listeners.end(), [](const auto& a, const auto& b) {
			return a.objectId == b.objectId ? a.componentGeneration < b.componentGeneration : a.objectId < b.objectId;
		});
		return AudioAuthoringSnapshot(std::make_shared<const AudioAuthoringSnapshotData>(std::move(data)));
	}
}
