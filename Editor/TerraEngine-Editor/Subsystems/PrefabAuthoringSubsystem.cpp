// PrefabAuthoringSubsystem.cpp - editor prefab create/open/edit/delete workflows.

#include "PrefabAuthoringSubsystem.h"

void PrefabAuthoringSubsystem::Initialize(EditorContext& contextRef)
{
	context = &contextRef;
}

void PrefabAuthoringSubsystem::Update(float deltaTime)
{
	(void)deltaTime;
}

void PrefabAuthoringSubsystem::DrawUI()
{
}

void PrefabAuthoringSubsystem::Shutdown()
{
	context = nullptr;
	sources.clear();
	instances.clear();
	diagnostics.clear();
}

std::string PrefabAuthoringSubsystem::GetName() const
{
	return "Prefab Authoring";
}

TerraEngine::Scene::PrefabAuthoringSession PrefabAuthoringSubsystem::CreatePrefabFromSelection(TerraEngine::Scene::PrefabSource source)
{
	if (source.sourceId == TerraEngine::EMPTY_UUID)
	{
		source.sourceId = TerraEngine::GenerateUUID();
	}
	TerraEngine::Scene::ValidatePrefabSource(source);
	sources[source.sourceId] = source;

	return TerraEngine::Scene::PrefabAuthoringSession{
		TerraEngine::GenerateUUID(),
		source.sourceId,
		source.sourcePath,
		true,
		source.sourceGraph.diagnostics
	};
}

std::optional<TerraEngine::Scene::PrefabAuthoringSession> PrefabAuthoringSubsystem::OpenPrefab(TerraEngine::UUID sourceId)
{
	auto source = sources.find(sourceId);
	if (source == sources.end() || source->second.deleted)
	{
		return std::nullopt;
	}

	return TerraEngine::Scene::PrefabAuthoringSession{
		TerraEngine::GenerateUUID(),
		sourceId,
		source->second.sourcePath,
		false,
		source->second.sourceGraph.diagnostics
	};
}

bool PrefabAuthoringSubsystem::SavePrefab(TerraEngine::UUID sourceId)
{
	auto source = sources.find(sourceId);
	if (source == sources.end())
	{
		return false;
	}

	source->second.sourceGraph.diagnostics.clear();
	return TerraEngine::Scene::ValidatePrefabSource(source->second);
}

std::vector<TerraEngine::Scene::SceneDiagnostic> PrefabAuthoringSubsystem::DeletePrefab(TerraEngine::UUID sourceId, bool convertInstances)
{
	auto source = sources.find(sourceId);
	if (source == sources.end())
	{
		return {};
	}

	diagnostics = TerraEngine::Scene::ValidatePrefabDelete(source->second, instances, convertInstances);
	if (!diagnostics.empty())
	{
		return diagnostics;
	}

	if (convertInstances)
	{
		for (auto& instance : instances)
		{
			if (instance.sourceId == sourceId)
			{
				instance.convertedToObjects = true;
			}
		}
	}

	source->second.deleted = true;
	sources.erase(source);
	return diagnostics;
}

void PrefabAuthoringSubsystem::ApplyInstanceOverridesToDefaults(TerraEngine::UUID sourceId, const TerraEngine::Scene::PrefabInstance& instance, const std::vector<TerraEngine::Scene::PrefabOverride>& selectedOverrides)
{
	if (auto source = sources.find(sourceId); source != sources.end())
	{
		TerraEngine::Scene::ApplyOverridesToDefaults(source->second, instance, selectedOverrides);
	}
}

void PrefabAuthoringSubsystem::ResetInstanceOverrides(TerraEngine::Scene::PrefabInstance& instance, const std::optional<TerraEngine::UUID>& targetObjectId)
{
	TerraEngine::Scene::ResetOverridesToDefaults(instance, targetObjectId);
}

const std::unordered_map<TerraEngine::UUID, TerraEngine::Scene::PrefabSource>& PrefabAuthoringSubsystem::GetSources() const
{
	return sources;
}

std::vector<TerraEngine::Scene::SceneDiagnostic> PrefabAuthoringSubsystem::GetDiagnostics() const
{
	return diagnostics;
}
