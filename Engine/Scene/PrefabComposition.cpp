// PrefabComposition.cpp - deterministic prefab source and instance resolution.

#include "PrefabComposition.h"

#include <algorithm>
#include <functional>
#include <unordered_set>

namespace TerraEngine::Scene {
	namespace {
		SceneDiagnostic MakePrefabDiagnostic(SceneDiagnosticSeverity severity, TerraEngine::UUID objectId, std::string message, std::string action)
		{
			return SceneDiagnostic{
				severity,
				SceneDiagnosticCategory::Composition,
				objectId,
				{},
				std::move(message),
				std::move(action)
			};
		}
	}

	bool ValidatePrefabSource(PrefabSource& source)
	{
		if (source.sourceId == TerraEngine::EMPTY_UUID)
		{
			source.sourceId = TerraEngine::GenerateUUID();
		}

		if (source.deleted)
		{
			source.sourceGraph.diagnostics.push_back(MakePrefabDiagnostic(SceneDiagnosticSeverity::Error, source.sourceId, "Prefab source is marked deleted.", "Restore the source or convert dependent instances to ordinary objects."));
			return false;
		}

		return ValidateSceneGraph(source.sourceGraph);
	}

	PrefabCompositionResult ResolvePrefabInstance(const PrefabSource& source, const PrefabInstance& instance)
	{
		PrefabCompositionResult result;
		result.instance = instance;
		result.resolvedGraph = source.sourceGraph;

		if (source.deleted)
		{
			result.success = false;
			result.diagnostics.push_back(MakePrefabDiagnostic(SceneDiagnosticSeverity::Error, instance.rootObjectId, "Prefab instance references a deleted source.", "Convert the instance or restore the prefab source."));
			return result;
		}

		MarkStaleOverrides(result.instance, source);
		for (const PrefabOverride& overrideValue : result.instance.overrides)
		{
			if (overrideValue.stale)
			{
				result.diagnostics.push_back(MakePrefabDiagnostic(SceneDiagnosticSeverity::Warning, overrideValue.targetObjectId, "Prefab override targets a missing source object.", "Reset the stale override or restore the source object."));
			}
		}

		result.success = ValidateSceneGraph(result.resolvedGraph);
		result.diagnostics.insert(result.diagnostics.end(), result.resolvedGraph.diagnostics.begin(), result.resolvedGraph.diagnostics.end());
		return result;
	}

	std::vector<SceneDiagnostic> DetectPrefabDependencyCycles(const std::unordered_map<TerraEngine::UUID, PrefabSource>& sources)
	{
		std::vector<SceneDiagnostic> diagnostics;
		std::unordered_set<TerraEngine::UUID> visiting;
		std::unordered_set<TerraEngine::UUID> visited;

		std::function<void(TerraEngine::UUID)> visit = [&](TerraEngine::UUID sourceId) {
			if (visited.contains(sourceId))
			{
				return;
			}
			if (!visiting.insert(sourceId).second)
			{
				diagnostics.push_back(MakePrefabDiagnostic(SceneDiagnosticSeverity::Fatal, sourceId, "Nested prefab dependency cycle detected.", "Remove one nested prefab reference from the cycle."));
				return;
			}

			if (auto source = sources.find(sourceId); source != sources.end())
			{
				for (const TerraEngine::UUID& nestedId : source->second.nestedSourceIds)
				{
					visit(nestedId);
				}
			}

			visiting.erase(sourceId);
			visited.insert(sourceId);
		};

		for (const auto& [sourceId, source] : sources)
		{
			(void)source;
			visit(sourceId);
		}
		return diagnostics;
	}

	void MarkStaleOverrides(PrefabInstance& instance, const PrefabSource& source)
	{
		for (PrefabOverride& overrideValue : instance.overrides)
		{
			const bool existsInSource = source.sourceGraph.objects.find(overrideValue.targetObjectId) != source.sourceGraph.objects.end();
			overrideValue.stale = !existsInSource;
		}
	}

	void ApplyOverridesToDefaults(PrefabSource& source, const PrefabInstance& instance, const std::vector<PrefabOverride>& selectedOverrides)
	{
		(void)instance;
		for (const PrefabOverride& overrideValue : selectedOverrides)
		{
			if (overrideValue.kind != PrefabOverrideKind::Property || overrideValue.stale)
			{
				continue;
			}
			if (auto object = source.sourceGraph.objects.find(overrideValue.targetObjectId); object != source.sourceGraph.objects.end())
			{
				object->second.name = overrideValue.propertyPath == "name" ? overrideValue.authoredValue : object->second.name;
			}
		}
		++source.sourceGraph.revision;
	}

	void ResetOverridesToDefaults(PrefabInstance& instance, const std::optional<TerraEngine::UUID>& targetObjectId)
	{
		instance.overrides.erase(
			std::remove_if(instance.overrides.begin(), instance.overrides.end(), [&](const PrefabOverride& overrideValue) {
				return !targetObjectId.has_value() || overrideValue.targetObjectId == *targetObjectId;
			}),
			instance.overrides.end());
	}

	std::vector<SceneDiagnostic> ValidatePrefabDelete(const PrefabSource& source, const std::vector<PrefabInstance>& instances, bool convertInstances)
	{
		std::vector<SceneDiagnostic> diagnostics;
		const bool hasLiveInstances = std::any_of(instances.begin(), instances.end(), [&](const PrefabInstance& instance) {
			return instance.sourceId == source.sourceId && !instance.convertedToObjects;
		});

		if (hasLiveInstances && !convertInstances)
		{
			diagnostics.push_back(MakePrefabDiagnostic(SceneDiagnosticSeverity::Error, source.sourceId, "Prefab delete cancelled because live instances still reference the source.", "Confirm conversion if these instances should become ordinary scene objects."));
		}
		return diagnostics;
	}
}
