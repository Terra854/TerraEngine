#include "Tests/Support/RendererEfficiency/Editor/RendererEfficiencyScenePublication.h"
#include "Core/Object/ObjectManager.h"
#include "Core/Object/Component.h"
#include <algorithm>
#include <map>

namespace TerraEngine::Editor::Automation
{
	bool CaptureRendererEfficiencyRollbackPublication(ObjectManager& objects,
		const Scene::TransactionReadSet& observed, RendererEfficiencyRollbackPublication& output, std::string& reason)
	{
		// Called only while the receipt pump owns protected live-scene access.
		// Packet publication and renderer spatial revision are different clocks:
		// retain the real owner and copy the renderer clock without relabeling it
		// as a new transaction publication. Failed capture leaves output untouched.
		RendererEfficiencyRollbackPublication captured;
		captured.packet = objects.LoadPublishedSceneChangePacketShared();
		captured.sceneGeneration = objects.GetSceneGeneration();
		if(!observed.sceneGeneration || observed.sceneGeneration != captured.sceneGeneration
			|| !captured.packet || !captured.packet->PublicationRevision()
			|| !captured.packet->Validate(captured.sceneGeneration).valid
			|| observed.objects.size() != 1)
		{ reason = "Rollback requires a retained valid same-scene scheduler publication and one exact target."; return false; }
		const auto& target = observed.objects.front();
		const auto object = objects.getObject(target.objectId);
		if(!object || !target.lifetimeGeneration
			|| objects.getObjectLifetimeGeneration(*object) != target.lifetimeGeneration)
		{ reason = "Rollback publication target identity or lifetime changed before queuing."; return false; }
		captured.targetId = target.objectId; captured.targetLifetime = target.lifetimeGeneration;
		captured.targetSpatialRevision = objects.GetObjectSpatialRevision(*object);
		captured.rendererSpatialRevision = objects.GetSpatialRevision();
		if(!captured.targetSpatialRevision || !captured.rendererSpatialRevision)
		{ reason = "Rollback target has no actual protected renderer spatial revision."; return false; }
		output = std::move(captured); reason.clear(); return true;
	}
	namespace
	{
		using namespace Scene;
		bool AuthoredValuesEqual(const Reflection::Variant& actual, const Reflection::Variant& expected)
		{
			if(!Reflection::IsValid(actual) || !Reflection::IsValid(expected) || actual.type().info() != expected.type().info()) return false;
			// A getter in the engine DLL and a copied editor transaction can have
			// different EnTT context owners. Compare their exact native values, not
			// meta_any's context identity. Unsupported types remain fail-closed.
			const auto equal = [&]<typename T>() {
				const auto* left = actual.try_cast<T>();
				const auto* right = expected.try_cast<T>();
				return left && right && *left == *right;
			};
			return equal.template operator()<UUID>() || equal.template operator()<std::vector<UUID>>()
				|| equal.template operator()<bool>() || equal.template operator()<std::uint32_t>()
				|| equal.template operator()<float>() || equal.template operator()<double>()
				|| equal.template operator()<glm::vec3>() || equal.template operator()<glm::dvec3>();
		}
		bool CaptureTargets(ObjectManager& objects, const Transaction& transaction,
			std::vector<RendererEfficiencyPublishedTarget>& targets, std::string& reason)
		{
			std::map<std::int32_t, UUID> locals;
			std::map<UUID, std::vector<const TransactionWriteOperation*>> writes;
			for(const auto& write : transaction.writeSet.operations)
				if(write.kind == TransactionWriteKind::CreateObject) locals[write.createdLocalObjectId] = write.createdStableObjectId;
			for(const auto& write : transaction.writeSet.operations)
			{
				const auto id = write.kind == TransactionWriteKind::CreateObject ? write.createdStableObjectId
					: write.target.UsesLocalObject() ? locals[write.target.localObjectId] : write.target.objectId;
				if(id == EMPTY_UUID) { reason = "Fixture publication requires stable typed target identities."; return false; }
				writes[id].push_back(&write);
			}
			for(const auto& [id, operations] : writes)
			{
				const auto previous = std::find_if(transaction.readSet.objects.begin(), transaction.readSet.objects.end(), [&](const auto& row) { return row.objectId == id; });
				const auto object = objects.getObject(id); RendererEfficiencyPublishedTarget target; target.revision.objectId = id;
				const bool recreated = std::any_of(operations.begin(), operations.end(), [](const auto* write) { return write->kind == TransactionWriteKind::CreateObject; });
				if(!object)
				{
					if(operations.back()->kind != TransactionWriteKind::DeleteObject || previous == transaction.readSet.objects.end()) { reason = "Committed fixture target is absent unexpectedly."; return false; }
					target.removed = true; target.revision.lifetimeGeneration = previous->lifetimeGeneration; targets.push_back(target); continue;
				}
				target.revision.lifetimeGeneration = objects.getObjectLifetimeGeneration(*object);
				target.revision.transformRevision = objects.GetObjectTransformRevision(*object);
				target.revision.spatialRevision = objects.GetObjectSpatialRevision(*object);
				target.revision.componentGeneration = objects.getObjectComponentGeneration(*object);
				target.revision.metadataGeneration = objects.getObjectMetadataGeneration(*object);
				target.added = recreated;
				if(recreated && previous != transaction.readSet.objects.end() && previous->lifetimeGeneration == target.revision.lifetimeGeneration)
				{ reason = "Recreated fixture identity did not receive a new native lifetime."; return false; }
				if(!target.revision.lifetimeGeneration || (!recreated && previous != transaction.readSet.objects.end() && previous->lifetimeGeneration != target.revision.lifetimeGeneration)) { reason = "Committed fixture target lifetime changed."; return false; }
				for(const auto* write : operations)
				{
					switch(write->kind)
					{
					case TransactionWriteKind::CreateObject: break;
					case TransactionWriteKind::DeleteObject:
						if(!recreated) { reason = "Deleted fixture target remains live."; return false; } break;
					case TransactionWriteKind::SetWorldTransform:
					{
						const auto pose = objects.GetCanonicalWorldPose(*object);
						if(!pose || pose->position != write->worldPose.position || pose->rotation != write->worldPose.rotation || pose->scale != write->worldPose.scale)
						{ reason = "Committed fixture authored world pose is not visible."; return false; }
						target.transform = previous == transaction.readSet.objects.end() || target.revision.transformRevision != previous->transformRevision; break;
					}
					case TransactionWriteKind::SetActive:
						if(objects.isObjectActive(*object) != write->boolValue) { reason = "Committed fixture active value is not visible."; return false; }
						target.metadata = previous == transaction.readSet.objects.end() || target.revision.metadataGeneration != previous->metadataGeneration; break;
					case TransactionWriteKind::SetStatic:
						if(objects.isObjectStatic(*object) != write->boolValue) { reason = "Committed fixture static category is not visible."; return false; }
						target.staticCategory = true; target.isStatic = write->boolValue;
						target.metadata = previous == transaction.readSet.objects.end() || target.revision.metadataGeneration != previous->metadataGeneration; break;
					case TransactionWriteKind::AddComponent:
					case TransactionWriteKind::UpdateComponentProperty:
					{
						const auto component = ComponentAccessRegistry().find(write->componentName);
						if(component == ComponentAccessRegistry().end() || !component->second.entityHasComponent(objects.getRegistry(), object->EntityId())) { reason = "Committed fixture component is not visible."; return false; }
						if(write->kind == TransactionWriteKind::UpdateComponentProperty)
						{
							const auto actual = component->second.entityGetter(objects.getRegistry(), object->EntityId(), write->propertyName);
							const auto property = Reflection::FindProperty(component->second.reflectionType, write->propertyName);
							auto expected = write->propertyValue.allow_cast(Reflection::GetPropertyType(property));
							if(!Reflection::IsValid(actual) || !Reflection::IsValid(expected) || !AuthoredValuesEqual(actual, expected))
							{
								reason = "Committed fixture authored component value is not visible: target=" + ToHexString(id)
									+ " component=" + write->componentName + " property=" + write->propertyName
									+ " actualType=" + Reflection::GetTypeName(actual.type())
									+ " expectedType=" + Reflection::GetTypeName(expected.type());
								return false;
							}
						}
						target.component = previous == transaction.readSet.objects.end() || target.revision.componentGeneration != previous->componentGeneration; break;
					}
					default: reason = "Fixture scene publication does not support this typed write kind."; return false;
					}
				}
				targets.push_back(std::move(target));
			}
			return !targets.empty();
		}
	}
	bool CaptureRendererEfficiencyCommittedScenePublication(ObjectManager& objects,
		RendererEfficiencyScenePublication& proof, const Scene::AutomationTransactionReceipt& receipt, std::string& reason)
	{
		if(receipt.transactionId != proof.transaction.id || receipt.status != Scene::TransactionStatus::Committed || receipt.rejectedWriteCount)
		{ reason = "Fixture publication requires its exact actual Committed receipt."; return false; }
		std::vector<RendererEfficiencyPublishedTarget> captured;
		if(proof.transaction.readSet.sceneGeneration && objects.GetSceneGeneration() != proof.transaction.readSet.sceneGeneration) { reason = "Committed fixture scene generation changed."; return false; }
		if(!CaptureTargets(objects, proof.transaction, captured, reason)) return false;
		proof.sceneGeneration = objects.GetSceneGeneration(); proof.targets = std::move(captured); proof.committedReceipt = receipt; proof.committedCaptured = true; return true;
	}
	bool ResolveRendererEfficiencyScenePublication(ObjectManager& objects,
		RendererEfficiencyScenePublication& proof, std::uint64_t& actualSpatialRevision, std::string& reason)
	{
		if(!proof.committedCaptured) { reason = "Awaiting actual committed fixture receipt."; return false; }
		if(objects.GetSceneGeneration() != proof.sceneGeneration) { proof.invalid = true; reason = "Owned scene changed before the committed fixture publication."; return false; }
		// Retain the exact packet observed at this protected boundary. Never drain
		// or publish the scheduler journal here, and never substitute a world clock.
		const auto packet = objects.LoadPublishedSceneChangePacketShared();
		if(!packet || packet->SceneGeneration() != proof.sceneGeneration || objects.GetSceneGeneration() != proof.sceneGeneration
			|| !packet->Validate(proof.sceneGeneration).valid || packet->PublicationRevision() <= (proof.priorPacket ? proof.priorPacket->PublicationRevision() : 0)) { reason = "Awaiting newer same-scene scheduler publication for the committed fixture."; return false; }
		std::vector<RendererEfficiencyPublishedTarget> live;
		if(!CaptureTargets(objects, proof.transaction, live, reason)) { proof.invalid = true; return false; }
		for(const auto& target : proof.targets)
		{
			const auto current = std::find_if(live.begin(), live.end(), [&](const auto& row) { return row.revision.objectId == target.revision.objectId; });
			if(current == live.end() || current->removed != target.removed || current->revision.lifetimeGeneration != target.revision.lifetimeGeneration
				|| (target.staticCategory && current->isStatic != target.isStatic)
				|| (target.transform && current->revision.transformRevision != target.revision.transformRevision)) { proof.invalid = true; reason = "Committed fixture target identity or authored transform changed before publication."; return false; }
			const auto matches = [&](const auto& rows, auto revision) { return std::any_of(rows.begin(), rows.end(), [&](const auto& row) {
				return row.objectId == target.revision.objectId && row.lifetimeGeneration == target.revision.lifetimeGeneration && revision(row); }); };
			if((target.added && !matches(packet->Added(), [](const auto&) { return true; }))
				|| (target.removed && !matches(packet->Removed(), [](const auto&) { return true; }))
				|| (target.transform && !matches(target.added ? packet->Added() : packet->TransformChanged(), [&](const auto& row) { return row.transformRevision == target.revision.transformRevision; }))
				|| (target.component && !matches(target.added ? packet->Added() : packet->ComponentChanged(), [&](const auto& row) { return row.componentGeneration >= target.revision.componentGeneration; }))
				|| (target.metadata && !matches(target.added ? packet->Added() : packet->MetadataChanged(), [&](const auto& row) { return row.metadataGeneration >= target.revision.metadataGeneration && (!target.staticCategory || row.isStatic == target.isStatic); }))) { reason = "Awaiting exact committed target revisions in the scheduler-owned scene packet."; return false; }
		}
		const auto spatial = objects.GetSpatialRevision();
		if(!spatial) { reason = "Published fixture has no actual native renderer spatial revision."; return false; }
		proof.acceptedPacket = packet; actualSpatialRevision = spatial; reason.clear(); return true;
	}
}
