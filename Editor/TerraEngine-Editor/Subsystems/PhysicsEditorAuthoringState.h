// PhysicsEditorAuthoringState.h - UI-independent editor physics state helpers.
//
// Inspector and hierarchy windows use these small containers, while editor
// tests exercise the same UUID and protected-boundary behavior without linking
// the complete ImGui workspace implementation.

#pragma once

#include "Engine/Core/Object/ObjectManager.h"
#include "Engine/Utilities/UUID.h"

#include <algorithm>
#include <cstdint>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace TerraEngine::Editor
{
	struct PhysicsRebuildIntent
	{
		TerraEngine::UUID objectId = TerraEngine::EMPTY_UUID;
		std::string reason;
		std::uint64_t sequence = 0U;
	};

	// The queue deduplicates by authored UUID. It never stores EnTT handles or
	// provider state, so a protected drain cannot target a recycled object slot.
	class PhysicsRebuildIntentQueue
	{
	public:
		void Queue(const std::vector<TerraEngine::UUID>& objectIds, std::string reason)
		{
			for(const TerraEngine::UUID objectId : objectIds)
			{
				if(objectId == TerraEngine::EMPTY_UUID)
					continue;
				const auto existing = std::find_if(intents.begin(), intents.end(), [&](const PhysicsRebuildIntent& intent) {
					return intent.objectId == objectId;
				});
				if(existing == intents.end())
					intents.push_back({objectId, reason, ++nextSequence});
				else
					existing->reason = reason;
			}
		}

		[[nodiscard]] std::vector<PhysicsRebuildIntent> Drain()
		{
			std::vector<PhysicsRebuildIntent> drained = std::move(intents);
			intents.clear();
			return drained;
		}

	private:
		std::vector<PhysicsRebuildIntent> intents;
		std::uint64_t nextSequence = 0U;
	};

	struct StableEditorSelection
	{
		std::vector<TerraEngine::UUID> objectIds;
		std::optional<entt::entity> primaryEntity;
	};

	// Resolves transient editor handles immediately and retains only unique UUIDs.
	// Invalid handles are ignored, and the final valid input remains the primary
	// selection to preserve existing Inspector focus behavior.
	inline StableEditorSelection BuildStableEditorSelection(const std::vector<entt::entity>& entities)
	{
		StableEditorSelection selection;
		for(const entt::entity entity : entities)
		{
			const std::optional<Object> object = ObjectManager::instance().getObject(entity);
			if(!object)
				continue;
			const TerraEngine::UUID objectId = ObjectManager::instance().getUUID(*object);
			if(std::find(selection.objectIds.begin(), selection.objectIds.end(), objectId) == selection.objectIds.end())
			{
				selection.objectIds.push_back(objectId);
				selection.primaryEntity = entity;
			}
		}
		return selection;
	}
}
