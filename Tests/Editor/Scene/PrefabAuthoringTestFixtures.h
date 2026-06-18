#pragma once

#include "Scene/PrefabComposition.h"
#include "Tests/Engine/Scene/SceneGraphTestFixtures.h"

namespace TerraEngine::EditorTests::PrefabFixtures {

	inline TerraEngine::Scene::PrefabSource MakePrefabSource()
	{
		TerraEngine::Scene::PrefabSource source;
		source.sourceId = TerraEngine::Tests::SceneFixtures::MakeStableUuid(42);
		source.sourcePath = "Fixtures/Prefab.teprefab";
		source.sourceGraph = TerraEngine::Tests::SceneFixtures::MakeThreeLevelGraph();
		return source;
	}

	inline TerraEngine::Scene::PrefabInstance MakePrefabInstance(const TerraEngine::Scene::PrefabSource& source)
	{
		TerraEngine::Scene::PrefabInstance instance;
		instance.instanceId = TerraEngine::Tests::SceneFixtures::MakeStableUuid(43);
		instance.sourceId = source.sourceId;
		instance.rootObjectId = source.sourceGraph.rootObjects.front();
		return instance;
	}

}
