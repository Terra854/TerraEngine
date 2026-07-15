// BulkScenePublication.cpp - protected atomic-like scene publication helper.

#include "Scene/BulkScenePublication.h"

#include "Core/Object/ObjectManager.h"
#include "Core/Scheduling/WorkerSafety.h"

#include <limits>
#include <stdexcept>

namespace TerraEngine::Scene {
	BulkScenePublicationPacket BuildBulkScenePublicationPacket(
		const PreparedSceneDecode& prepared,
		std::uint64_t expectedSceneGeneration,
		std::uint64_t preparedGeneration)
	{
		BulkScenePublicationPacket packet;
		packet.expectedSceneGeneration = expectedSceneGeneration;
		packet.preparedGeneration = preparedGeneration;
		packet.reservedObjectCount = prepared.objectCount;
		packet.reservedComponentCount = prepared.componentCount;
		packet.reservedHierarchyCount = prepared.hierarchyCount;
		packet.cancelled = prepared.cancelled || !prepared.accepted;
		packet.objects = prepared.objects;
		return packet;
	}

	BulkScenePublicationPreflight PreflightBulkScenePublication(const BulkScenePublicationPacket& packet, const ObjectManager& objects)
	{
		if (packet.cancelled) return {false, "prepared scene packet was cancelled or rejected"};
		if (packet.expectedSceneGeneration == 0U || packet.expectedSceneGeneration != objects.GetSceneGeneration()) {
			return {false, "prepared scene packet belongs to a stale scene generation"};
		}
		if (packet.reservedObjectCount != packet.objects.size()) return {false, "prepared scene object count does not match its exact reserve"};
		if (packet.reservedObjectCount > static_cast<std::uint64_t>(std::numeric_limits<std::size_t>::max())) return {false, "prepared scene reserve exceeds addressable storage"};
		for (std::size_t index = 0U; index < packet.objects.size(); ++index) {
			const PreparedSceneObjectRecord& record = packet.objects[index];
			if (!record.object.is_object() || (record.parentRecordIndex.has_value() && *record.parentRecordIndex >= index)) {
				return {false, "prepared scene has an invalid object record or non-parent-first hierarchy"};
			}
		}
		return {true, {}};
	}

	BulkScenePublicationResult PublishBulkScenePacket(
		const BulkScenePublicationPacket& packet,
		ObjectManager& objects,
		const std::function<bool()>& cancellationRequested)
	{
		if (TerraEngine::Scheduling::IsWorkerSafetyScopeActive()) {
			TerraEngine::Scheduling::ReportWorkerSafetyViolation(
				TerraEngine::Scheduling::WorkerSafetySeverity::Error,
				"PublishBulkScenePacket",
				"Worker code attempted to publish a live bulk scene packet.",
				"Return the packet to the scene manager and publish it at a declared protected boundary.");
			return {false, false, false, objects.GetSceneGeneration(), "bulk publication requires a protected scheduler boundary"};
		}
		const BulkScenePublicationPreflight preflight = PreflightBulkScenePublication(packet, objects);
		if (!preflight.accepted) return {false, false, packet.cancelled, objects.GetSceneGeneration(), preflight.reason};
		if (cancellationRequested && cancellationRequested()) return {false, false, true, objects.GetSceneGeneration(), "bulk publication was cancelled before its protected boundary"};

		// Capture the previous authoritative in-memory graph before changing it.
		// This rollback copy is used only inside the protected boundary; workers
		// never observe it and no lifecycle callback fires until Finish succeeds.
		std::vector<nlohmann::json> rollbackRoots;
		for (Object root : objects.getRootObjects()) rollbackRoots.push_back(Serializer::SerializeObject(root));

		try {
			objects.BeginBulkPublication({static_cast<std::size_t>(packet.reservedObjectCount), static_cast<std::size_t>(packet.reservedComponentCount)});
			objects.clear();
			std::vector<Object> publishedObjects;
			publishedObjects.reserve(packet.objects.size());
			for (const PreparedSceneObjectRecord& record : packet.objects) {
				if (cancellationRequested && cancellationRequested()) throw std::runtime_error("bulk publication cancelled during protected materialization");
				// Records are guaranteed to be parent-first by preflight. Attach the
				// object while it still has its default transform, then deserialize its
				// authored components. The normal setParent path preserves world space;
				// attaching after component restoration would therefore reinterpret an
				// authored local camera/child transform as a temporary root world pose.
				std::optional<entt::entity> parentEntity;
				if (record.parentRecordIndex.has_value()) {
					parentEntity = publishedObjects[*record.parentRecordIndex].EntityId();
				}
				const entt::entity entity = Serializer::DeserializeObjectEntity(record.object, parentEntity);
				auto created = objects.getObject(entity);
				if (!created.has_value()) throw std::runtime_error("bulk publication could not resolve a materialized object");
				publishedObjects.push_back(*created);
			}
			objects.FinishBulkPublication(true);
			return {true, false, false, objects.GetSceneGeneration(), {}};
		}
		catch (const std::exception& exception) {
			// The old graph is restored before releasing the protected boundary, so
			// observers see either the previous complete graph or the new complete
			// graph, never a partially populated replacement.
			objects.clear();
			for (const nlohmann::json& root : rollbackRoots) (void)Serializer::DeserializeObjectEntity(root);
			objects.FinishBulkPublication(true);
			return {false, true, cancellationRequested && cancellationRequested(), objects.GetSceneGeneration(), exception.what()};
		}
	}

}
