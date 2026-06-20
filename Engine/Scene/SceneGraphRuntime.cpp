// SceneGraphRuntime.cpp - deterministic hierarchy resolution for live and authored scenes.

#include "SceneGraphRuntime.h"

#include "Components/Transform.h"
#include "Core/Object/Object.h"
#include "Core/Object/ObjectManager.h"

#include <algorithm>
#include <functional>
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
				object.localPosition = ReadVec3(transformIt->value("position", nlohmann::json::array()), object.localPosition);
				object.localRotation = ReadQuat(transformIt->value("rotation", nlohmann::json::array()), object.localRotation);
				object.localScale = ReadVec3(transformIt->value("scale", nlohmann::json::array()), object.localScale);
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

	SceneGraph BuildSceneGraphFromObjectManager(ObjectManager& objects)
	{
		SceneGraph graph;
		graph.revision = objects.GetSpatialRevision();

		std::function<void(Object, SceneObjectId)> visitObject = [&](Object object, SceneObjectId parentId) {
			SceneGraphObject graphObject;
			graphObject.objectId = objects.getUUID(object.EntityId());
			graphObject.parentId = parentId;
			graphObject.name = objects.getObjectName(object);
			graphObject.active = objects.isObjectActive(object);
			if (Transform* transform = object.getComponent<Transform>())
			{
				graphObject.localPosition = GetComponentProperty<Transform, glm::vec3>(*transform, "Position");
				graphObject.localRotation = GetComponentProperty<Transform, glm::quat>(*transform, "Rotation");
				graphObject.localScale = GetComponentProperty<Transform, glm::vec3>(*transform, "Scale");
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
		std::sort(roots.begin(), roots.end(), [&](const Object& lhs, const Object& rhs) {
			return TerraEngine::ToHexString(objects.getUUID(lhs.EntityId())) < TerraEngine::ToHexString(objects.getUUID(rhs.EntityId()));
		});
		for (Object root : roots)
		{
			visitObject(root, TerraEngine::EMPTY_UUID);
		}

		ValidateSceneGraph(graph);
		ComputeWorldTransforms(graph);
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
		std::function<void(SceneObjectId)> resolve = [&](SceneObjectId objectId) {
			auto objectIt = graph.objects.find(objectId);
			if (objectIt == graph.objects.end())
			{
				return;
			}

			SceneGraphObject& object = objectIt->second;
			if (object.parentId != TerraEngine::EMPTY_UUID)
			{
				auto parentIt = graph.objects.find(object.parentId);
				if (parentIt != graph.objects.end())
				{
					const SceneGraphObject& parent = parentIt->second;
					object.worldPosition = parent.worldPosition + parent.worldRotation * (parent.worldScale * object.localPosition);
					object.worldRotation = parent.worldRotation * object.localRotation;
					object.worldScale = parent.worldScale * object.localScale;
				}
			}
			else
			{
				object.worldPosition = object.localPosition;
				object.worldRotation = object.localRotation;
				object.worldScale = object.localScale;
			}

			for (const SceneObjectId& childId : object.children)
			{
				resolve(childId);
			}
		};

		for (const SceneObjectId& rootId : graph.rootObjects)
		{
			resolve(rootId);
		}
	}

	SceneTransformWorkPlan BuildTransformWorkPlan(const SceneGraph& graph)
	{
		SceneTransformWorkPlan plan;
		plan.diagnostics = graph.diagnostics;
		plan.valid = !std::any_of(plan.diagnostics.begin(), plan.diagnostics.end(), [](const SceneDiagnostic& diagnostic) {
			return diagnostic.severity == SceneDiagnosticSeverity::Error || diagnostic.severity == SceneDiagnosticSeverity::Fatal;
		});
		if (!plan.valid)
		{
			return plan;
		}

		std::unordered_map<std::uint32_t, SceneTransformWorkUnit> unitsByDepth;
		for (const SceneObjectId& objectId : BuildTraversalOrder(graph))
		{
			const std::uint32_t depth = DepthForObject(graph, objectId);
			SceneTransformWorkUnit& unit = unitsByDepth[depth];
			unit.hierarchyLevel = depth;
			unit.workUnitId = "level-" + std::to_string(depth);
			unit.objects.push_back(objectId);
			if (depth > 0)
			{
				unit.parentDependencies.push_back("level-" + std::to_string(depth - 1));
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
				object.worldPosition
			});
		}
		return summary;
	}

	bool ReparentPreserveWorld(ObjectManager& objects, Object child, std::optional<Object> parent)
	{
		return objects.setParent(child, parent);
	}
}
