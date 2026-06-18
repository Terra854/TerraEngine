#pragma once

#include "Scene/SceneGraphRuntime.h"

#include <string>

namespace TerraEngine::Tests::SceneFixtures {

	inline TerraEngine::UUID MakeStableUuid(std::uint8_t value)
	{
		TerraEngine::UUID id{};
		id[15] = value;
		return id;
	}

	inline TerraEngine::Scene::SceneGraphObject MakeObject(std::uint8_t idValue, std::string name, TerraEngine::UUID parentId = TerraEngine::EMPTY_UUID)
	{
		TerraEngine::Scene::SceneGraphObject object;
		object.objectId = MakeStableUuid(idValue);
		object.parentId = parentId;
		object.name = std::move(name);
		return object;
	}

	inline TerraEngine::Scene::SceneGraph MakeThreeLevelGraph()
	{
		TerraEngine::Scene::SceneGraph graph;
		auto root = MakeObject(1, "Root");
		auto child = MakeObject(2, "Child", root.objectId);
		auto grandchild = MakeObject(3, "Grandchild", child.objectId);
		root.children.push_back(child.objectId);
		child.children.push_back(grandchild.objectId);
		graph.rootObjects.push_back(root.objectId);
		graph.objects.emplace(root.objectId, root);
		graph.objects.emplace(child.objectId, child);
		graph.objects.emplace(grandchild.objectId, grandchild);
		return graph;
	}

	inline TerraEngine::Scene::SceneGraph MakeLargeFiveLevelGraph(std::size_t objectCount)
	{
		TerraEngine::Scene::SceneGraph graph;
		for (std::size_t index = 0; index < objectCount; ++index)
		{
			const std::uint8_t idValue = static_cast<std::uint8_t>((index % 250u) + 1u);
			auto object = MakeObject(idValue, "Object" + std::to_string(index));
			object.objectId[0] = static_cast<std::uint8_t>(index / 250u);
			if (index == 0)
			{
				graph.rootObjects.push_back(object.objectId);
			}
			else
			{
				const std::size_t parentIndex = (index - 1u) / 4u;
				TerraEngine::UUID parentId = MakeStableUuid(static_cast<std::uint8_t>((parentIndex % 250u) + 1u));
				parentId[0] = static_cast<std::uint8_t>(parentIndex / 250u);
				object.parentId = parentId;
				graph.objects[parentId].children.push_back(object.objectId);
			}
			graph.objects.emplace(object.objectId, object);
		}
		return graph;
	}

}
