#pragma once

#include "Scene/SceneChangePacket.h"
#include "Scene/TransactionalMutation.h"
#include "Scene/SceneMutationQueue.h"
#include <memory>
#include <string>
#include <vector>

class ObjectManager;

namespace TerraEngine::Editor::Automation
{
	// A rejected transaction publishes no new scene state. Retain its prior
	// protected renderer clock and immutable scene owner instead of using the
	// reserved (normally zero) transaction spatial-publication field.
	struct RendererEfficiencyRollbackPublication
	{
		std::shared_ptr<const Scene::SceneChangePacket> packet;
		UUID targetId{};
		std::uint64_t sceneGeneration = 0, targetLifetime = 0, targetSpatialRevision = 0, rendererSpatialRevision = 0;
	};
	bool CaptureRendererEfficiencyRollbackPublication(ObjectManager& objects,
		const Scene::TransactionReadSet& observed, RendererEfficiencyRollbackPublication& output, std::string& reason);
	struct RendererEfficiencyPublishedTarget
	{
		Scene::SceneObjectRevisionSnapshot revision;
		bool added = false, removed = false, transform = false, component = false, metadata = false;
		// Retain exact authored category independently of unrelated metadata.
		bool staticCategory = false, isStatic = false;
	};
	// Only typed expectations and immutable packet owners cross queue ingress.
	// Live authored values are read later by the protected receipt pump.
	struct RendererEfficiencyScenePublication
	{
		Scene::Transaction transaction;
		std::shared_ptr<const Scene::SceneChangePacket> priorPacket, acceptedPacket;
		std::vector<RendererEfficiencyPublishedTarget> targets;
		std::optional<Scene::AutomationTransactionReceipt> committedReceipt;
		std::uint64_t sceneGeneration = 0;
		bool committedCaptured = false, invalid = false;
	};
	bool CaptureRendererEfficiencyCommittedScenePublication(ObjectManager& objects,
		RendererEfficiencyScenePublication& proof, const Scene::AutomationTransactionReceipt& receipt, std::string& reason);
	bool ResolveRendererEfficiencyScenePublication(ObjectManager& objects,
		RendererEfficiencyScenePublication& proof, std::uint64_t& actualSpatialRevision, std::string& reason);
}
