// SceneGraphRuntime.cpp - deterministic hierarchy resolution for live and authored scenes.

#include "SceneGraphRuntime.h"

#include "Components/Transform.h"
#include "Core/Scheduling/JobGraph.h"
#include "Core/Object/Object.h"
#include "Core/Object/ObjectManager.h"
#include "Scene/SceneChangePacket.h"
#include "Scene/HierarchySnapshot.h"
#include "ThreadPool/ThreadPool.h"

#include <algorithm>
#include <charconv>
#include <functional>
#include <memory>
#include <sstream>
#include <tuple>
#include <unordered_set>

namespace TerraEngine::Scene {
	namespace {
		glm::vec3 ReadVec3(const nlohmann::json& value, const glm::vec3& fallback)
		{
			if (!value.is_array() || value.size() < 3)
			{
				return fallback;
			}

			return glm::vec3(
				value.at(0).get<float>(),
				value.at(1).get<float>(),
				value.at(2).get<float>());
		}

		glm::dvec3 ReadDVec3(const nlohmann::json& value, const glm::dvec3& fallback)
		{
			if (!value.is_array() || value.size() < 3)
			{
				return fallback;
			}
			return glm::dvec3(value.at(0).get<double>(), value.at(1).get<double>(), value.at(2).get<double>());
		}

		glm::quat ReadQuat(const nlohmann::json& value, const glm::quat& fallback)
		{
			if (!value.is_array() || value.size() < 4)
			{
				return fallback;
			}

			return glm::quat(
				value.at(0).get<float>(),
				value.at(1).get<float>(),
				value.at(2).get<float>(),
				value.at(3).get<float>());
		}

		TerraEngine::UUID ReadUuid(const nlohmann::json& source, std::string_view key)
		{
			if (!source.contains(std::string(key)) || !source.at(std::string(key)).is_string())
			{
				return TerraEngine::EMPTY_UUID;
			}

			const auto parsed = TerraEngine::ParseUUID(source.at(std::string(key)).get<std::string>());
			return parsed.value_or(TerraEngine::EMPTY_UUID);
		}

		bool ReadInt64Axis(const nlohmann::json& value, std::int64_t& axis)
		{
			if (!value.is_string())
			{
				return false;
			}
			const std::string encoded = value.get<std::string>();
			const auto [end, error] = std::from_chars(encoded.data(), encoded.data() + encoded.size(), axis);
			return error == std::errc{} && end == encoded.data() + encoded.size();
		}

		bool ReadCanonicalPosition(const nlohmann::json& transform, WorldPosition& position)
		{
			const auto positionIt = transform.find("worldPosition");
			if (positionIt == transform.end() || !positionIt->is_object())
			{
				return false;
			}
			const auto cellIt = positionIt->find("cell");
			if (cellIt == positionIt->end() || !cellIt->is_object()
				|| !cellIt->contains("x") || !cellIt->contains("y") || !cellIt->contains("z")
				|| !ReadInt64Axis(cellIt->at("x"), position.cell.x)
				|| !ReadInt64Axis(cellIt->at("y"), position.cell.y)
				|| !ReadInt64Axis(cellIt->at("z"), position.cell.z))
			{
				return false;
			}
			position.offsetMetres = ReadDVec3(positionIt->value("offsetMetres", nlohmann::json::array()), position.offsetMetres);
			const WorldCoordinateResult normalized = NormalizeWorldPosition(position);
			if (!normalized.Succeeded())
			{
				return false;
			}
			position = normalized.position;
			return true;
		}

		void AppendDiagnostic(SceneGraph& graph, SceneDiagnosticSeverity severity, SceneDiagnosticCategory category, const SceneObjectId& objectId, std::string message, std::string action)
		{
			graph.diagnostics.push_back(SceneDiagnostic{
				severity,
				category,
				objectId,
				{},
				std::move(message),
				std::move(action)
			});
		}

		bool HasBlockingDiagnostics(const SceneGraph& graph)
		{
			return std::any_of(graph.diagnostics.begin(), graph.diagnostics.end(), [](const SceneDiagnostic& diagnostic) {
				return diagnostic.severity == SceneDiagnosticSeverity::Error || diagnostic.severity == SceneDiagnosticSeverity::Fatal;
			});
		}

		void AddJsonObjectRecursive(SceneGraph& graph, const nlohmann::json& objectJson, const SceneObjectId& parentId)
		{
			SceneGraphObject object;
			object.objectId = ReadUuid(objectJson, "uuid");
			if (object.objectId == TerraEngine::EMPTY_UUID)
			{
				object.objectId = ReadUuid(objectJson, "id");
			}
			if (object.objectId == TerraEngine::EMPTY_UUID)
			{
				object.objectId = TerraEngine::GenerateUUID();
				AppendDiagnostic(graph, SceneDiagnosticSeverity::Warning, SceneDiagnosticCategory::Serialization, object.objectId, "Authored object was missing a UUID.", "Assign a persistent UUID before saving the scene.");
			}

			object.parentId = parentId;
			object.name = objectJson.value("name", "Object");
			object.active = objectJson.value("active", true);

			if (const auto transformIt = objectJson.find("transform"); transformIt != objectJson.end())
			{
				if (!transformIt->is_object() || !ReadCanonicalPosition(*transformIt, object.worldPose.position))
				{
					AppendDiagnostic(graph, SceneDiagnosticSeverity::Error, SceneDiagnosticCategory::Serialization, object.objectId, "Transform did not provide a strict canonical worldPosition.", "Write TerraSceneUnboundedV1 canonical cell strings and normalized offsets.");
				}
				object.worldPose.rotation = ReadQuat(transformIt->value("rotation", nlohmann::json::array()), object.worldPose.rotation);
				object.worldPose.scale = ReadVec3(transformIt->value("scale", nlohmann::json::array()), object.worldPose.scale);
				if (const auto relativeIt = transformIt->find("parentRelativePose"); relativeIt != transformIt->end() && relativeIt->is_object())
				{
					object.parentRelativePose.translationMetres = ReadDVec3(relativeIt->value("translationMetres", nlohmann::json::array()), object.parentRelativePose.translationMetres);
					object.parentRelativePose.rotation = ReadQuat(relativeIt->value("rotation", nlohmann::json::array()), object.parentRelativePose.rotation);
					object.parentRelativePose.scale = ReadVec3(relativeIt->value("scale", nlohmann::json::array()), object.parentRelativePose.scale);
					object.parentRelativePose.transformSpaceId = relativeIt->value("transformSpaceId", 0ULL);
				}
			}

			if (parentId == TerraEngine::EMPTY_UUID)
			{
				graph.rootObjects.push_back(object.objectId);
			}
			else if (auto parent = graph.objects.find(parentId); parent != graph.objects.end())
			{
				parent->second.children.push_back(object.objectId);
			}

			const SceneObjectId objectId = object.objectId;
			if (!graph.objects.emplace(objectId, std::move(object)).second)
			{
				AppendDiagnostic(graph, SceneDiagnosticSeverity::Error, SceneDiagnosticCategory::Hierarchy, objectId, "Duplicate object UUID found in authored scene data.", "Regenerate one of the duplicated object identifiers.");
				return;
			}

			if (const auto childrenIt = objectJson.find("children"); childrenIt != objectJson.end() && childrenIt->is_array())
			{
				for (const auto& childJson : *childrenIt)
				{
					AddJsonObjectRecursive(graph, childJson, objectId);
				}
			}
		}

		std::uint32_t DepthForObject(const SceneGraph& graph, SceneObjectId objectId)
		{
			std::uint32_t depth = 0;
			auto iter = graph.objects.find(objectId);
			while (iter != graph.objects.end() && iter->second.parentId != TerraEngine::EMPTY_UUID)
			{
				++depth;
				iter = graph.objects.find(iter->second.parentId);
			}
			return depth;
		}

		SceneWorldTransformRecord ResolveWorldTransformRecord(SceneGraphObject& object, const SceneGraphObject* parent, std::uint32_t hierarchyLevel, std::uint64_t graphRevision)
		{
			// Parent records are resolved by an earlier hierarchy level. Workers
			// therefore only read immutable parent data and write the one child
			// object assigned to their snapshot item.
			if (parent != nullptr)
			{
				const glm::vec3 boundedRelative = parent->worldPose.rotation
					* (parent->worldPose.scale * glm::vec3(object.parentRelativePose.translationMetres));
				const WorldCoordinateResult translated = ApplyWorldDelta(parent->worldPose.position, { {}, glm::dvec3(boundedRelative) });
				if (translated.Succeeded())
				{
					object.worldPose.position = translated.position;
				}
				object.worldPose.rotation = parent->worldPose.rotation * object.parentRelativePose.rotation;
				object.worldPose.scale = parent->worldPose.scale * object.parentRelativePose.scale;
			}

			// The graph revision is captured per worker chunk; the exact record uses
			// the narrower local transform revision for stale-work validation.
			(void)graphRevision;
			return SceneWorldTransformRecord{
				object.objectId,
				object.entity,
				object.worldPose,
				hierarchyLevel,
				object.transformRevision
			};
		}

		void ExpandDirtyBranch(const SceneGraph& graph, const SceneObjectId& objectId, std::unordered_set<SceneObjectId>& dirtySet)
		{
			if (!dirtySet.insert(objectId).second)
			{
				return;
			}

			const auto object = graph.objects.find(objectId);
			if (object == graph.objects.end())
			{
				return;
			}

			std::vector<SceneObjectId> children = object->second.children;
			std::sort(children.begin(), children.end(), [](const SceneObjectId& left, const SceneObjectId& right) {
				return TerraEngine::ToHexString(left) < TerraEngine::ToHexString(right);
			});
			for (const SceneObjectId& childId : children)
			{
				ExpandDirtyBranch(graph, childId, dirtySet);
			}
		}
	}

	std::string_view ToString(SceneDiagnosticSeverity severity)
	{
		switch (severity)
		{
		case SceneDiagnosticSeverity::Info: return "Info";
		case SceneDiagnosticSeverity::Warning: return "Warning";
		case SceneDiagnosticSeverity::Error: return "Error";
		case SceneDiagnosticSeverity::Fatal: return "Fatal";
		}
		return "Unknown";
	}

	std::string_view ToString(SceneDiagnosticCategory category)
	{
		switch (category)
		{
		case SceneDiagnosticCategory::Hierarchy: return "Hierarchy";
		case SceneDiagnosticCategory::Composition: return "Composition";
		case SceneDiagnosticCategory::Mutation: return "Mutation";
		case SceneDiagnosticCategory::Parity: return "Parity";
		case SceneDiagnosticCategory::Serialization: return "Serialization";
		case SceneDiagnosticCategory::ExportStartup: return "ExportStartup";
		}
		return "Unknown";
	}

	std::string FormatSceneDiagnostic(const SceneDiagnostic& diagnostic)
	{
		std::ostringstream stream;
		stream << '[' << ToString(diagnostic.severity) << "][" << ToString(diagnostic.category) << ']';
		if (diagnostic.objectId != TerraEngine::EMPTY_UUID)
		{
			stream << '[' << TerraEngine::ToHexString(diagnostic.objectId) << ']';
		}
		if (!diagnostic.requestId.empty())
		{
			stream << '[' << diagnostic.requestId << ']';
		}
		stream << ' ' << diagnostic.message;
		if (!diagnostic.recommendedAction.empty())
		{
			stream << " Action: " << diagnostic.recommendedAction;
		}
		return stream.str();
	}

	SceneGraph BuildSceneGraphFromObjectManager(ObjectManager& objects, bool computeWorldTransforms)
	{
		SceneGraph graph;
		graph.sceneGeneration = objects.GetSceneGeneration();
		graph.hierarchyRevision = objects.GetHierarchyRevision();
		graph.revision = objects.GetSpatialRevision();

		std::function<void(Object, SceneObjectId)> visitObject = [&](Object object, SceneObjectId parentId) {
			SceneGraphObject graphObject;
			graphObject.objectId = objects.getUUID(object);
			graphObject.entity = object.EntityId();
			graphObject.parentId = parentId;
			graphObject.name = objects.getObjectName(object);
			graphObject.active = objects.isObjectActive(object);
			graphObject.transformRevision = objects.GetObjectTransformRevision(object);
			graphObject.spatialRevision = objects.GetObjectSpatialRevision(object);
			if (Transform* transform = objects.GetComponent<Transform>(object))
			{
				// Copy protected-side authored state into the graph snapshot. Worker
				// propagation mutates only this detached copy, never live components.
				graphObject.worldPose = transform->worldPose;
				graphObject.parentRelativePose = transform->parentRelativePose;
			}

			const SceneObjectId objectId = graphObject.objectId;
			if (parentId == TerraEngine::EMPTY_UUID)
			{
				graph.rootObjects.push_back(objectId);
			}
			else if (auto parent = graph.objects.find(parentId); parent != graph.objects.end())
			{
				parent->second.children.push_back(objectId);
			}

			graph.objects.emplace(objectId, std::move(graphObject));
			for (Object child : objects.getChildren(object))
			{
				visitObject(child, objectId);
			}
		};

		std::vector<Object> roots = objects.getRootObjects();
		objects.SortObjectsByStableId(roots);
		for (Object root : roots)
		{
			visitObject(root, TerraEngine::EMPTY_UUID);
		}

		ValidateSceneGraph(graph);
		if (computeWorldTransforms)
		{
			ComputeWorldTransforms(graph);
		}
		return graph;
	}

	SceneGraph BuildSceneGraphFromJson(const nlohmann::json& sceneJson)
	{
		SceneGraph graph;
		if (sceneJson.contains("uuid") && sceneJson["uuid"].is_string())
		{
			graph.sceneId = TerraEngine::ParseUUID(sceneJson["uuid"].get<std::string>()).value_or(TerraEngine::EMPTY_UUID);
		}

		const nlohmann::json* rootArray = nullptr;
		if (auto objectsIt = sceneJson.find("objects"); objectsIt != sceneJson.end() && objectsIt->is_array())
		{
			rootArray = &(*objectsIt);
		}
		else if (auto entitiesIt = sceneJson.find("entities"); entitiesIt != sceneJson.end() && entitiesIt->is_array())
		{
			rootArray = &(*entitiesIt);
		}

		if (rootArray)
		{
			for (const auto& objectJson : *rootArray)
			{
				AddJsonObjectRecursive(graph, objectJson, TerraEngine::EMPTY_UUID);
			}
		}

		ValidateSceneGraph(graph);
		ComputeWorldTransforms(graph);
		return graph;
	}

	bool ValidateSceneGraph(SceneGraph& graph)
	{
		std::unordered_set<SceneObjectId> referencedRoots;
		for (const SceneObjectId& rootId : graph.rootObjects)
		{
			if (!referencedRoots.insert(rootId).second)
			{
				AppendDiagnostic(graph, SceneDiagnosticSeverity::Error, SceneDiagnosticCategory::Hierarchy, rootId, "Root object appears more than once.", "Keep each root object listed once.");
			}
			if (graph.objects.find(rootId) == graph.objects.end())
			{
				AppendDiagnostic(graph, SceneDiagnosticSeverity::Error, SceneDiagnosticCategory::Hierarchy, rootId, "Root object does not exist in the graph.", "Remove the stale root reference or restore the object.");
			}
		}

		for (const auto& [objectId, object] : graph.objects)
		{
			if (objectId == TerraEngine::EMPTY_UUID)
			{
				AppendDiagnostic(graph, SceneDiagnosticSeverity::Error, SceneDiagnosticCategory::Hierarchy, objectId, "Object has an empty UUID.", "Assign a generated UUID before runtime use.");
			}

			if (object.parentId != TerraEngine::EMPTY_UUID && graph.objects.find(object.parentId) == graph.objects.end())
			{
				AppendDiagnostic(graph, SceneDiagnosticSeverity::Error, SceneDiagnosticCategory::Hierarchy, objectId, "Object references a missing parent.", "Restore the parent or detach the object to the scene root.");
			}
			const WorldCoordinateResult normalizedPosition = NormalizeWorldPosition(object.worldPose.position);
			if (!normalizedPosition.Succeeded() || !IsWorldPoseFinite(object.worldPose))
			{
				AppendDiagnostic(graph, SceneDiagnosticSeverity::Error, SceneDiagnosticCategory::Composition, objectId, "Object has an invalid canonical world pose.", "Store finite rotation and scale with a normalized WorldCell plus offset.");
			}
			if (object.parentId != TerraEngine::EMPTY_UUID && !IsParentRelativePoseInRange(object.parentRelativePose))
			{
				// A graph can originate from untrusted scene JSON, not just from the
				// protected ObjectManager path. Validate the bounded local contract
				// before worker propagation rotates or scales this relationship.
				AppendDiagnostic(graph, SceneDiagnosticSeverity::Error, SceneDiagnosticCategory::Hierarchy, objectId, "Child parent-relative pose exceeds the bounded transform space.", "Reparent within the supported local radius or use an intermediate anchor.");
			}

			for (const SceneObjectId& childId : object.children)
			{
				if (graph.objects.find(childId) == graph.objects.end())
				{
					AppendDiagnostic(graph, SceneDiagnosticSeverity::Error, SceneDiagnosticCategory::Hierarchy, objectId, "Object references a missing child.", "Remove the stale child reference or restore the child object.");
				}
			}
		}

		std::unordered_set<SceneObjectId> visiting;
		std::unordered_set<SceneObjectId> visited;
		std::function<void(SceneObjectId)> visit = [&](SceneObjectId objectId) {
			if (visited.contains(objectId))
			{
				return;
			}
			if (!visiting.insert(objectId).second)
			{
				AppendDiagnostic(graph, SceneDiagnosticSeverity::Fatal, SceneDiagnosticCategory::Hierarchy, objectId, "Cycle detected in scene hierarchy.", "Break the parent-child loop before entering runtime.");
				return;
			}
			if (auto object = graph.objects.find(objectId); object != graph.objects.end())
			{
				for (const SceneObjectId& childId : object->second.children)
				{
					visit(childId);
				}
			}
			visiting.erase(objectId);
			visited.insert(objectId);
		};

		for (const SceneObjectId& rootId : graph.rootObjects)
		{
			visit(rootId);
		}
		for (const auto& [objectId, object] : graph.objects)
		{
			(void)object;
			if (!visited.contains(objectId))
			{
				visit(objectId);
				AppendDiagnostic(graph, SceneDiagnosticSeverity::Warning, SceneDiagnosticCategory::Hierarchy, objectId, "Object is not reachable from any root.", "Attach the object to a root if it should be active.");
			}
		}

		return !HasBlockingDiagnostics(graph);
	}

	std::vector<SceneObjectId> BuildTraversalOrder(const SceneGraph& graph)
	{
		std::vector<SceneObjectId> order;
		std::unordered_set<SceneObjectId> visited;
		std::function<void(SceneObjectId)> visit = [&](SceneObjectId objectId) {
			if (!visited.insert(objectId).second)
			{
				return;
			}

			if (graph.objects.find(objectId) == graph.objects.end())
			{
				return;
			}

			order.push_back(objectId);
			std::vector<SceneObjectId> children = graph.objects.at(objectId).children;
			std::sort(children.begin(), children.end(), [](const SceneObjectId& lhs, const SceneObjectId& rhs) {
				return TerraEngine::ToHexString(lhs) < TerraEngine::ToHexString(rhs);
			});
			for (const SceneObjectId& childId : children)
			{
				visit(childId);
			}
		};

		std::vector<SceneObjectId> roots = graph.rootObjects;
		std::sort(roots.begin(), roots.end(), [](const SceneObjectId& lhs, const SceneObjectId& rhs) {
			return TerraEngine::ToHexString(lhs) < TerraEngine::ToHexString(rhs);
		});
		for (const SceneObjectId& rootId : roots)
		{
			visit(rootId);
		}
		return order;
	}

	void ComputeWorldTransforms(SceneGraph& graph)
	{
		(void)PropagateWorldTransforms(graph, BuildDirtyTransformState(graph));
	}

	SceneDirtyTransformState BuildDirtyTransformState(const SceneGraph& graph, const std::vector<SceneObjectId>& dirtyRoots, SceneTransformDirtyReason reason)
	{
		SceneDirtyTransformState state;
		state.fullRefreshRequested = dirtyRoots.empty() || reason == SceneTransformDirtyReason::FullRefresh || reason == SceneTransformDirtyReason::SceneLoad;
		state.reason = reason;
		state.sourceRevision = graph.revision;

		std::unordered_set<SceneObjectId> rootSet;
		if (state.fullRefreshRequested)
		{
			state.dirtyRoots = graph.rootObjects;
			rootSet.insert(state.dirtyRoots.begin(), state.dirtyRoots.end());
		}
		else
		{
			for (const SceneObjectId& rootId : dirtyRoots)
			{
				if (rootId != TerraEngine::EMPTY_UUID && graph.objects.find(rootId) != graph.objects.end() && rootSet.insert(rootId).second)
				{
					state.dirtyRoots.push_back(rootId);
				}
			}
			// Exact packets can list both a direct edit and the descendants whose
			// world transforms changed. Collapse them to minimal roots before branch
			// expansion so each descendant is scheduled exactly once.
			state.dirtyRoots.erase(std::remove_if(state.dirtyRoots.begin(), state.dirtyRoots.end(), [&graph, &rootSet](const SceneObjectId& objectId) {
				auto object = graph.objects.find(objectId);
				while (object != graph.objects.end() && object->second.parentId != TerraEngine::EMPTY_UUID)
				{
					if (rootSet.find(object->second.parentId) != rootSet.end())
					{
						return true;
					}
					object = graph.objects.find(object->second.parentId);
				}
				return false;
			}), state.dirtyRoots.end());
			rootSet.clear();
			rootSet.insert(state.dirtyRoots.begin(), state.dirtyRoots.end());
		}
		std::sort(state.dirtyRoots.begin(), state.dirtyRoots.end(), [](const SceneObjectId& left, const SceneObjectId& right) {
			return TerraEngine::ToHexString(left) < TerraEngine::ToHexString(right);
		});

		std::unordered_set<SceneObjectId> expandedSet;
		for (const SceneObjectId& rootId : state.dirtyRoots)
		{
			ExpandDirtyBranch(graph, rootId, expandedSet);
		}

		state.expandedDirtyObjects.assign(expandedSet.begin(), expandedSet.end());
		std::sort(state.expandedDirtyObjects.begin(), state.expandedDirtyObjects.end(), [](const SceneObjectId& left, const SceneObjectId& right) {
			return TerraEngine::ToHexString(left) < TerraEngine::ToHexString(right);
		});
		for (const SceneObjectId& objectId : state.expandedDirtyObjects)
		{
			if (rootSet.find(objectId) == rootSet.end())
			{
				state.affectedDescendants.push_back(objectId);
			}
		}
		return state;
	}

	SceneDirtyTransformState BuildDirtyTransformState(const SceneGraph& graph, const SceneChangePacket& changes)
	{
		const SceneChangePacketValidationResult validation = changes.Validate(graph.sceneGeneration);
		if (!validation.valid)
		{
			// Stale work must not silently expand to a full scene scan: doing so
			// would expose data outside the packet's protected publication point.
			SceneDirtyTransformState rejected;
			rejected.valid = false;
			rejected.fullRefreshRequested = false;
			rejected.sourceRevision = graph.revision;
			return rejected;
		}

		std::vector<SceneObjectId> roots;
		roots.reserve(changes.TransformChanged().size() + changes.HierarchyChanged().size());
		for (const SceneObjectRevisionSnapshot& snapshot : changes.TransformChanged())
		{
			roots.push_back(snapshot.objectId);
		}
		for (const SceneObjectRevisionSnapshot& snapshot : changes.HierarchyChanged())
		{
			roots.push_back(snapshot.objectId);
		}
		if (roots.empty())
		{
			// A valid empty packet is an intentional no-op. It must not become a
			// whole-scene refresh merely because no dirty root was supplied.
			SceneDirtyTransformState empty;
			empty.fullRefreshRequested = false;
			empty.reason = SceneTransformDirtyReason::LocalTransform;
			empty.sourceRevision = graph.revision;
			empty.sourceSceneGeneration = changes.SceneGeneration();
			return empty;
		}
		const SceneTransformDirtyReason reason = changes.HierarchyChanged().empty()
			? SceneTransformDirtyReason::LocalTransform
			: SceneTransformDirtyReason::Hierarchy;
		SceneDirtyTransformState state = BuildDirtyTransformState(graph, roots, reason);
		state.sourceSceneGeneration = changes.SceneGeneration();
		return state;
	}

	SceneTransformWorkPlan BuildTransformWorkPlan(const SceneGraph& graph)
	{
		HierarchySnapshot snapshot;
		snapshot.revision = graph.revision;
		for (const SceneObjectId& objectId : BuildTraversalOrder(graph))
		{
			const auto object = graph.objects.find(objectId);
			if (object == graph.objects.end())
			{
				continue;
			}
			HierarchySnapshotEntry entry;
			entry.objectId = objectId;
			entry.parentId = object->second.parentId;
			entry.depth = DepthForObject(graph, objectId);
			snapshot.orderedObjects.push_back(entry);
		}
		for (std::size_t index = 0; index < snapshot.orderedObjects.size(); ++index)
		{
			const std::uint32_t depth = snapshot.orderedObjects[index].depth;
			if (!snapshot.depthRanges.empty() && snapshot.depthRanges.back().depth == depth)
			{
				snapshot.depthRanges.back().end = index + 1;
			}
			else
			{
				snapshot.depthRanges.push_back({ depth, index, index + 1 });
			}
		}
		snapshot.valid = true;
		snapshot.formatted = FormatHierarchySnapshot(snapshot);
		return BuildTransformWorkPlan(graph, snapshot);
	}

	SceneTransformWorkPlan BuildTransformWorkPlan(const SceneGraph& graph, const HierarchySnapshot& snapshot)
	{
		SceneTransformWorkPlan plan;
		plan.diagnostics = graph.diagnostics;
		plan.valid = !std::any_of(plan.diagnostics.begin(), plan.diagnostics.end(), [](const SceneDiagnostic& diagnostic) {
			return diagnostic.severity == SceneDiagnosticSeverity::Error || diagnostic.severity == SceneDiagnosticSeverity::Fatal;
		});
		if (!snapshot.valid)
		{
			plan.valid = false;
			plan.diagnostics.push_back(SceneDiagnostic{
				SceneDiagnosticSeverity::Error,
				SceneDiagnosticCategory::Hierarchy,
				TerraEngine::EMPTY_UUID,
				{},
				"Hierarchy snapshot is invalid; transform work planning is blocked.",
				"Rebuild hierarchy links before preparing transform worker ranges."
			});
		}
		if (!plan.valid)
		{
			return plan;
		}

		std::unordered_map<std::uint32_t, SceneTransformWorkUnit> unitsByDepth;
		for (const HierarchyDepthRange& range : snapshot.depthRanges)
		{
			SceneTransformWorkUnit& unit = unitsByDepth[range.depth];
			unit.hierarchyLevel = range.depth;
			unit.workUnitId = "level-" + std::to_string(range.depth);
			if (range.depth > 0)
			{
				unit.parentDependencies.push_back("level-" + std::to_string(range.depth - 1));
			}
			const std::size_t end = std::min(range.end, snapshot.orderedObjects.size());
			for (std::size_t index = range.begin; index < end; ++index)
			{
				const SceneObjectId objectId = snapshot.orderedObjects[index].objectId;
				if (graph.objects.find(objectId) != graph.objects.end())
				{
					unit.objects.push_back(objectId);
				}
			}
		}
		for (auto& [depth, unit] : unitsByDepth)
		{
			(void)depth;
			std::sort(unit.objects.begin(), unit.objects.end(), [](const SceneObjectId& left, const SceneObjectId& right) {
				return TerraEngine::ToHexString(left) < TerraEngine::ToHexString(right);
			});
			std::sort(unit.parentDependencies.begin(), unit.parentDependencies.end());
			unit.parentDependencies.erase(std::unique(unit.parentDependencies.begin(), unit.parentDependencies.end()), unit.parentDependencies.end());
			plan.units.push_back(std::move(unit));
		}
		std::sort(plan.units.begin(), plan.units.end(), [](const SceneTransformWorkUnit& left, const SceneTransformWorkUnit& right) {
			return std::tie(left.hierarchyLevel, left.workUnitId) < std::tie(right.hierarchyLevel, right.workUnitId);
		});
		return plan;
	}

	SceneTransformPropagationResult PropagateWorldTransforms(SceneGraph& graph, const SceneDirtyTransformState& dirtyState)
	{
		SceneTransformPropagationResult result;
		result.graphRevision = graph.revision;

		SceneTransformWorkPlan workPlan = BuildTransformWorkPlan(graph);
		result.valid = workPlan.valid;
		result.hierarchyLevelCount = static_cast<std::uint32_t>(workPlan.units.size());
		result.dirtyRootCount = static_cast<std::uint32_t>(dirtyState.dirtyRoots.size());
		result.expandedDirtyObjectCount = static_cast<std::uint32_t>(dirtyState.expandedDirtyObjects.size());
		result.diagnostics = workPlan.diagnostics;
		if (!dirtyState.valid)
		{
			result.valid = false;
			result.diagnostics.push_back(SceneDiagnostic{
				SceneDiagnosticSeverity::Error,
				SceneDiagnosticCategory::Mutation,
				TerraEngine::EMPTY_UUID,
				{},
				"Rejected scene change packet cannot drive transform propagation.",
				"Publish a generation-valid packet at the protected boundary before preparing transform work."
			});
			return result;
		}
		if (!workPlan.valid)
		{
			return result;
		}

		std::unordered_set<SceneObjectId> dirtyObjects(dirtyState.expandedDirtyObjects.begin(), dirtyState.expandedDirtyObjects.end());
		const bool processAll = dirtyState.fullRefreshRequested;

		struct TransformWorkItem {
			SceneGraphObject* object = nullptr;
			const SceneGraphObject* parent = nullptr;
			std::uint32_t hierarchyLevel = 0;
			bool process = false;
		};

		struct TransformChunkOutput {
			std::vector<SceneWorldTransformRecord> records;
			std::uint32_t processed = 0;
			std::uint32_t skipped = 0;
		};

		std::vector<std::vector<TransformWorkItem>> levelItems;
		levelItems.reserve(workPlan.units.size());
		std::size_t totalItems = 0;
		for (const SceneTransformWorkUnit& unit : workPlan.units)
		{
			std::vector<TransformWorkItem> items;
			items.reserve(unit.objects.size());
			for (const SceneObjectId& objectId : unit.objects)
			{
				auto objectIt = graph.objects.find(objectId);
				if (objectIt == graph.objects.end())
				{
					continue;
				}

				SceneGraphObject& object = objectIt->second;
				const SceneGraphObject* parent = nullptr;
				if (object.parentId != TerraEngine::EMPTY_UUID)
				{
					auto parentIt = graph.objects.find(object.parentId);
					if (parentIt != graph.objects.end())
					{
						parent = &parentIt->second;
					}
				}

				items.push_back(TransformWorkItem{
					&object,
					parent,
					unit.hierarchyLevel,
					processAll || dirtyObjects.find(objectId) != dirtyObjects.end()
				});
			}
			totalItems += items.size();
			levelItems.push_back(std::move(items));
		}

		auto mergeOutput = [&result](const TransformChunkOutput& output) {
			result.processedObjectCount += output.processed;
			result.skippedCleanObjectCount += output.skipped;
			for (const SceneWorldTransformRecord& record : output.records)
			{
				result.worldTransformsByObject[record.objectId] = record.worldPose;
				if (record.entity != entt::null)
				{
					result.worldTransformsByEntity[record.entity] = record.worldPose;
				}
				result.records.push_back(record);
			}
		};

		constexpr std::size_t kWorkerThreshold = 512;
		constexpr std::size_t kPreferredChunkSize = 256;
		const bool canUseWorkers = ThreadPool::instance().getThreadCount() > 0;
		if (!canUseWorkers || totalItems < kWorkerThreshold)
		{
			// Small scenes are faster without graph allocation and worker wake-up.
			// The same per-level work items are still used, so the serial fallback
			// has identical ordering and diagnostics to the threaded path.
			result.inlineJobCount = totalItems > 0u ? 1u : 0u;
			for (const std::vector<TransformWorkItem>& items : levelItems)
			{
				TransformChunkOutput output;
				output.records.reserve(items.size());
				for (const TransformWorkItem& item : items)
				{
					if (!item.process)
					{
						++output.skipped;
						continue;
					}
					output.records.push_back(ResolveWorldTransformRecord(*item.object, item.parent, item.hierarchyLevel, graph.revision));
					++output.processed;
				}
				mergeOutput(output);
			}
		}
		else
		{
			TerraEngine::Scheduling::JobGraph jobGraph("scene-transform-propagation", "Scene Transform Propagation");
			std::vector<TerraEngine::Scheduling::DependencyHandle> previousLevelJobs;
			std::vector<std::shared_ptr<TransformChunkOutput>> outputs;

			for (std::size_t levelIndex = 0; levelIndex < levelItems.size(); ++levelIndex)
			{
				const std::vector<TransformWorkItem>& items = levelItems[levelIndex];
				std::vector<TerraEngine::Scheduling::DependencyHandle> currentLevelJobs;
				for (std::size_t begin = 0; begin < items.size(); begin += kPreferredChunkSize)
				{
					const std::size_t end = std::min(begin + kPreferredChunkSize, items.size());
					auto output = std::make_shared<TransformChunkOutput>();
					output->records.reserve(end - begin);

					TerraEngine::Scheduling::JobGraphJobDeclaration job;
					job.name = "Transform.Level" + std::to_string(levelIndex) + ".Chunk" + std::to_string(begin / kPreferredChunkSize);
					job.executionPolicy = TerraEngine::Scheduling::JobGraphExecutionPolicy::WorkerEligible;
					job.cancellable = true;
					job.requiresSuccessfulDependencies = true;
					job.mayMutateSceneStructure = false;
					job.profilingLabel = "SceneGraphRuntime: Propagate World Transforms";
					job.dependencies = previousLevelJobs;
					job.work = [&items, begin, end, output, revision = graph.revision](TerraEngine::Scheduling::JobGraphContext& context) {
						for (std::size_t index = begin; index < end; ++index)
						{
							if (context.IsCancellationRequested())
							{
								return;
							}
							const TransformWorkItem& item = items[index];
							if (!item.process)
							{
								++output->skipped;
								continue;
							}
							output->records.push_back(ResolveWorldTransformRecord(*item.object, item.parent, item.hierarchyLevel, revision));
							++output->processed;
						}
					};
					currentLevelJobs.push_back(jobGraph.AddJob(std::move(job)));
					outputs.push_back(std::move(output));
				}
				previousLevelJobs = std::move(currentLevelJobs);
			}

			const TerraEngine::Scheduling::JobGraphSummary& summary = jobGraph.Execute();
			result.workerJobCount = summary.workerJobCount;
			result.protectedJobCount = summary.protectedJobCount;
			result.inlineJobCount = summary.inlineJobCount;
			result.coordinatorWaitTimeNs = summary.coordinatorWaitTimeNs;
			result.workerTimeNs = summary.workerTimeNs;
			result.workerUtilization = summary.workerUtilization;
			if (!summary.valid || !summary.completed)
			{
				result.valid = false;
				result.diagnostics.push_back(SceneDiagnostic{
					SceneDiagnosticSeverity::Error,
					SceneDiagnosticCategory::Hierarchy,
					TerraEngine::EMPTY_UUID,
					{},
					"Scene transform propagation job graph did not complete.",
					"Inspect JobGraph diagnostics before publishing world transforms."
				});
				return result;
			}

			for (const std::shared_ptr<TransformChunkOutput>& output : outputs)
			{
				mergeOutput(*output);
			}
		}

		std::sort(result.records.begin(), result.records.end(), [](const SceneWorldTransformRecord& left, const SceneWorldTransformRecord& right) {
			return std::make_tuple(left.hierarchyLevel, TerraEngine::ToHexString(left.objectId))
				< std::make_tuple(right.hierarchyLevel, TerraEngine::ToHexString(right.objectId));
		});
		return result;
	}

	SceneRuntimeSummary BuildSceneRuntimeSummary(const SceneGraph& graph)
	{
		SceneRuntimeSummary summary;
		summary.graphRevision = graph.revision;
		summary.diagnostics = graph.diagnostics;

		for (const SceneObjectId& objectId : BuildTraversalOrder(graph))
		{
			const SceneGraphObject& object = graph.objects.at(objectId);
			summary.objects.push_back(SceneSummaryObject{
				object.objectId,
				object.parentId,
				object.name,
				DepthForObject(graph, objectId),
				object.worldPose.position
			});
		}
		return summary;
	}

	std::uint64_t ObserveHierarchyRevision(const ObjectManager& objects)
	{
		// Transactions only need a stable hierarchy token while preparing work.
		// ObjectManager owns the authoritative parent/child revision, so this
		// helper keeps SceneGraphRuntime callers from depending on storage details.
		return objects.GetHierarchyRevision();
	}

	std::uint64_t ObserveHierarchyRevision(const SceneGraph& graph)
	{
		// SceneGraph snapshots carry the source revision used for traversal and
		// transform work planning; returning it gives worker-prepared transactions
		// the same conflict token without touching live ObjectManager state.
		return graph.revision;
	}

	bool ReparentPreserveWorld(ObjectManager& objects, entt::entity child, std::optional<entt::entity> parent)
	{
		return ReparentWithMode(objects, child, parent, ReparentMode::KeepWorld);
	}

	bool ReparentWithMode(ObjectManager& objects, entt::entity child, std::optional<entt::entity> parent, ReparentMode mode)
	{
		std::optional<Object> childObject = objects.getObject(child);
		if (!childObject)
		{
			return false;
		}
		std::optional<Object> parentObject;
		if (parent)
		{
			parentObject = objects.getObject(*parent);
			if (!parentObject)
			{
				return false;
			}
		}
		return objects.setParent(*childObject, parentObject, mode);
	}
}
