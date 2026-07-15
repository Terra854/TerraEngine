// SceneChangePacket.cpp - validation and read-only access for scene changes.

#include "SceneChangePacket.h"

#include <algorithm>

namespace TerraEngine::Scene {

	struct SceneChangePacket::Storage {
		std::uint64_t sceneGeneration = 0;
		std::uint64_t publicationRevision = 0;
		std::vector<SceneObjectRevisionSnapshot> added;
		std::vector<SceneObjectRevisionSnapshot> removed;
		std::vector<SceneObjectRevisionSnapshot> metadataChanged;
		std::vector<SceneObjectRevisionSnapshot> componentChanged;
		std::vector<SceneObjectRevisionSnapshot> transformChanged;
		std::vector<SceneObjectRevisionSnapshot> hierarchyChanged;
		std::vector<SceneObjectRevisionSnapshot> boundsChanged;
	};

	namespace {
		const std::vector<SceneObjectRevisionSnapshot>& EmptySnapshots()
		{
			static const std::vector<SceneObjectRevisionSnapshot> empty;
			return empty;
		}

		bool IsStrictlyOrdered(const std::vector<SceneObjectRevisionSnapshot>& snapshots, bool hierarchyOrder)
		{
			for (std::size_t index = 1; index < snapshots.size(); ++index)
			{
				const SceneObjectRevisionSnapshot& previous = snapshots[index - 1];
				const SceneObjectRevisionSnapshot& current = snapshots[index];
				if (hierarchyOrder && previous.hierarchyDepth > current.hierarchyDepth)
				{
					return false;
				}
				if ((!hierarchyOrder || previous.hierarchyDepth == current.hierarchyDepth)
					&& TerraEngine::ToHexString(previous.objectId) >= TerraEngine::ToHexString(current.objectId))
				{
					return false;
				}
			}
			return true;
		}

		bool HasValidIdentity(const SceneObjectRevisionSnapshot& snapshot)
		{
			return snapshot.objectId != TerraEngine::EMPTY_UUID
				&& snapshot.lifetimeGeneration != 0
				&& !snapshot.object.IsEmpty();
		}
	}

	SceneChangePacket::SceneChangePacket(std::shared_ptr<const Storage> value)
		: storage(std::move(value))
	{
	}

	SceneChangePacket SceneChangePacket::Create(
		std::uint64_t sceneGeneration,
		std::uint64_t publicationRevision,
		std::vector<SceneObjectRevisionSnapshot> added,
		std::vector<SceneObjectRevisionSnapshot> removed,
		std::vector<SceneObjectRevisionSnapshot> metadataChanged,
		std::vector<SceneObjectRevisionSnapshot> componentChanged,
		std::vector<SceneObjectRevisionSnapshot> transformChanged,
		std::vector<SceneObjectRevisionSnapshot> hierarchyChanged,
		std::vector<SceneObjectRevisionSnapshot> boundsChanged)
	{
		auto value = std::make_shared<Storage>();
		value->sceneGeneration = sceneGeneration;
		value->publicationRevision = publicationRevision;
		value->added = std::move(added);
		value->removed = std::move(removed);
		value->metadataChanged = std::move(metadataChanged);
		value->componentChanged = std::move(componentChanged);
		value->transformChanged = std::move(transformChanged);
		value->hierarchyChanged = std::move(hierarchyChanged);
		value->boundsChanged = std::move(boundsChanged);
		return SceneChangePacket(std::move(value));
	}

	std::uint64_t SceneChangePacket::SceneGeneration() const { return storage ? storage->sceneGeneration : 0; }
	std::uint64_t SceneChangePacket::PublicationRevision() const { return storage ? storage->publicationRevision : 0; }
	bool SceneChangePacket::Empty() const
	{
		return !storage || (storage->added.empty() && storage->removed.empty() && storage->metadataChanged.empty()
			&& storage->componentChanged.empty() && storage->transformChanged.empty() && storage->hierarchyChanged.empty()
			&& storage->boundsChanged.empty());
	}

	const std::vector<SceneObjectRevisionSnapshot>& SceneChangePacket::Added() const { return storage ? storage->added : EmptySnapshots(); }
	const std::vector<SceneObjectRevisionSnapshot>& SceneChangePacket::Removed() const { return storage ? storage->removed : EmptySnapshots(); }
	const std::vector<SceneObjectRevisionSnapshot>& SceneChangePacket::MetadataChanged() const { return storage ? storage->metadataChanged : EmptySnapshots(); }
	const std::vector<SceneObjectRevisionSnapshot>& SceneChangePacket::ComponentChanged() const { return storage ? storage->componentChanged : EmptySnapshots(); }
	const std::vector<SceneObjectRevisionSnapshot>& SceneChangePacket::TransformChanged() const { return storage ? storage->transformChanged : EmptySnapshots(); }
	const std::vector<SceneObjectRevisionSnapshot>& SceneChangePacket::HierarchyChanged() const { return storage ? storage->hierarchyChanged : EmptySnapshots(); }
	const std::vector<SceneObjectRevisionSnapshot>& SceneChangePacket::BoundsChanged() const { return storage ? storage->boundsChanged : EmptySnapshots(); }

	SceneChangePacketValidationResult SceneChangePacket::Validate(std::uint64_t expectedSceneGeneration) const
	{
		if (!storage || storage->sceneGeneration == 0 || storage->publicationRevision == 0)
		{
			return { false, "Scene change packet has no published generation." };
		}
		if (expectedSceneGeneration != 0 && storage->sceneGeneration != expectedSceneGeneration)
		{
			return { false, "Scene change packet belongs to a stale scene generation." };
		}

		auto validateSet = [](const std::vector<SceneObjectRevisionSnapshot>& snapshots, bool hierarchyOrder) -> bool {
			return IsStrictlyOrdered(snapshots, hierarchyOrder)
				&& std::all_of(snapshots.begin(), snapshots.end(), HasValidIdentity);
		};
		if (!validateSet(storage->added, false) || !validateSet(storage->removed, false)
			|| !validateSet(storage->metadataChanged, false) || !validateSet(storage->componentChanged, false)
			|| !validateSet(storage->transformChanged, false) || !validateSet(storage->boundsChanged, false)
			|| !validateSet(storage->hierarchyChanged, true))
		{
			return { false, "Scene change packet has invalid identities, duplicate entries, or nondeterministic ordering." };
		}
		return { true, "Scene change packet is valid." };
	}
}
