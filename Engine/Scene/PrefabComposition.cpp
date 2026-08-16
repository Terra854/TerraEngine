// PrefabComposition.cpp - deterministic prefab source and instance resolution.

#include "PrefabComposition.h"

#include <algorithm>
#include <functional>
#include <unordered_set>

namespace TerraEngine::Scene {
	bool ValidateUnboundedPrefabDocument(const nlohmann::json& document, std::string* outError)
	{
		auto fail = [outError](std::string reason) { if (outError) *outError = std::move(reason); return false; };
		if (!document.is_object() || document.value("format", std::string{}) != "TerraPrefabUnboundedV1") return fail("prefab format must be TerraPrefabUnboundedV1");
		// Source identifiers are kept as strings in JSON, since UUID precision must
		// not depend on a JavaScript or JSON number implementation.
		if (!document.contains("sourceId") || !document["sourceId"].is_string()) return fail("prefab requires a string sourceId");
		if (!document.contains("objects") || !document["objects"].is_array()) return fail("prefab requires an objects array");
		if (const auto overrides = document.find("overrides"); overrides != document.end() && !overrides->is_array()) return fail("prefab overrides must be an array");
		return true;
	}

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

		nlohmann::json ParseAuthoredValue(const std::string& authoredValue)
		{
			try
			{
				return nlohmann::json::parse(authoredValue);
			}
			catch (const nlohmann::json::parse_error&)
			{
				// Plain strings are valid property values; JSON literals keep numeric,
				// Boolean, array, and object types without a second schema language.
				return authoredValue;
			}
		}

		bool SetJsonPropertyPath(nlohmann::json& root, std::string_view path, nlohmann::json value)
		{
			if (path.empty()) return false;
			nlohmann::json* current = &root;
			std::size_t begin = 0U;
			while (begin < path.size())
			{
				const std::size_t separator = path.find('.', begin);
				const std::string key(path.substr(begin, separator == std::string_view::npos ? path.size() - begin : separator - begin));
				if (key.empty()) return false;
				if (separator == std::string_view::npos)
				{
					if (!current->contains(key)) return false;
					(*current)[key] = std::move(value);
					return true;
				}
				if (!current->contains(key) || !(*current)[key].is_object()) return false;
				current = &(*current)[key];
				begin = separator + 1U;
			}
			return false;
		}

		bool ApplyComponentDefaultOverride(PrefabSource& source, const PrefabOverride& overrideValue, std::vector<SceneDiagnostic>& diagnostics)
		{
			auto objectDefaults = source.componentDefaults.find(overrideValue.targetObjectId);
			if (objectDefaults == source.componentDefaults.end())
			{
				diagnostics.push_back(MakePrefabDiagnostic(SceneDiagnosticSeverity::Error, overrideValue.targetObjectId,
					"Prefab override targets an object without component defaults.", "Restore the component default before applying this override."));
				return false;
			}
			auto component = objectDefaults->second.find(overrideValue.componentName);
			if (component == objectDefaults->second.end() || !component->second.is_object())
			{
				diagnostics.push_back(MakePrefabDiagnostic(SceneDiagnosticSeverity::Error, overrideValue.targetObjectId,
					"Prefab override targets a missing component default.", "Reset the stale component override or restore the component."));
				return false;
			}

			nlohmann::json* propertyRoot = &component->second;
			std::string propertyPath = overrideValue.propertyPath;
			if (overrideValue.recordId != TerraEngine::EMPTY_UUID)
			{
				const char* collectionName = overrideValue.componentName == "ColliderSetComponent" ? "colliders"
					: overrideValue.componentName == "ConstraintSetComponent" ? "constraints" : nullptr;
				if (collectionName == nullptr || !component->second.contains(collectionName) || !component->second[collectionName].is_array())
				{
					diagnostics.push_back(MakePrefabDiagnostic(SceneDiagnosticSeverity::Error, overrideValue.targetObjectId,
						"Stable record override does not target a physics record collection.", "Choose a collider or constraint record property."));
					return false;
				}
				const std::string recordHex = TerraEngine::ToHexString(overrideValue.recordId);
				auto record = std::find_if(component->second[collectionName].begin(), component->second[collectionName].end(), [&](const nlohmann::json& value) {
					return value.is_object() && value.value("recordId", std::string{}) == recordHex;
				});
				if (record == component->second[collectionName].end())
				{
					diagnostics.push_back(MakePrefabDiagnostic(SceneDiagnosticSeverity::Error, overrideValue.targetObjectId,
						"Prefab override targets a missing stable physics record.", "Reset the stale record override or restore that record."));
					return false;
				}
				propertyRoot = &*record;
				const std::string prefix = std::string(collectionName) + ".";
				if (propertyPath.starts_with(prefix)) propertyPath.erase(0U, prefix.size());
			}

			nlohmann::json authored = overrideValue.connectedObjectSourceId != TerraEngine::EMPTY_UUID
				? nlohmann::json(TerraEngine::ToHexString(overrideValue.connectedObjectSourceId))
				: ParseAuthoredValue(overrideValue.authoredValue);
			if (!SetJsonPropertyPath(*propertyRoot, propertyPath, std::move(authored)))
			{
				diagnostics.push_back(MakePrefabDiagnostic(SceneDiagnosticSeverity::Error, overrideValue.targetObjectId,
					"Prefab override property path is missing from the component default.", "Choose an existing nested property path."));
				return false;
			}
			return true;
		}

		void RemapConstraintPayload(nlohmann::json& payload, const PrefabInstance& instance)
		{
			if (!payload.contains("constraints") || !payload["constraints"].is_array()) return;
			for (nlohmann::json& record : payload["constraints"])
			{
				for (const char* field : {"firstObjectId", "secondObjectId"})
				{
					if (!record.contains(field) || !record[field].is_string()) continue;
					const std::optional<TerraEngine::UUID> sourceId = TerraEngine::ParseUUID(record[field].get<std::string>());
					if (!sourceId || *sourceId == TerraEngine::EMPTY_UUID) continue;
					if (const auto remapped = RemapConnectedObjectUuid(instance, *sourceId)) record[field] = TerraEngine::ToHexString(*remapped);
				}
			}
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
		PrefabSource resolvedDefaults = source;

		if (source.deleted)
		{
			result.success = false;
			result.diagnostics.push_back(MakePrefabDiagnostic(SceneDiagnosticSeverity::Error, instance.rootObjectId, "Prefab instance references a deleted source.", "Convert the instance or restore the prefab source."));
			return result;
		}

		MarkStaleOverrides(result.instance, source);
		if (!ValidatePrefabOverrideBatch(source, result.instance, result.instance.overrides, result.diagnostics))
		{
			// Do not publish a partially remapped instance.  The caller receives the
			// original source graph and actionable diagnostics instead.
			result.success = false;
			return result;
		}
		for (PrefabOverride& overrideValue : result.instance.overrides)
		{
			if (overrideValue.connectedObjectSourceId != TerraEngine::EMPTY_UUID)
			{
				overrideValue.connectedObjectInstanceId = *RemapConnectedObjectUuid(result.instance, overrideValue.connectedObjectSourceId);
			}
		}
		for (const PrefabOverride& overrideValue : result.instance.overrides)
		{
			if (!overrideValue.stale && overrideValue.kind == PrefabOverrideKind::Property && !overrideValue.componentName.empty())
			{
				if (!ApplyComponentDefaultOverride(resolvedDefaults, overrideValue, result.diagnostics))
				{
					result.success = false;
					return result;
				}
			}
		}
		for (auto& [sourceObjectId, components] : resolvedDefaults.componentDefaults)
		{
			const auto mapped = instance.sourceToInstanceObjectIds.find(sourceObjectId);
			const TerraEngine::UUID resolvedObjectId = mapped == instance.sourceToInstanceObjectIds.end() ? sourceObjectId : mapped->second;
			for (auto& [componentName, payload] : components)
			{
				if (componentName == "ConstraintSetComponent") RemapConstraintPayload(payload, instance);
			}
			result.resolvedComponentDefaults[resolvedObjectId] = std::move(components);
		}
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
		std::vector<SceneDiagnostic> diagnostics;
		(void)TryApplyOverridesToDefaults(source, instance, selectedOverrides, diagnostics);
	}

	std::optional<TerraEngine::UUID> RemapConnectedObjectUuid(const PrefabInstance& instance, TerraEngine::UUID sourceObjectId)
	{
		if (sourceObjectId == TerraEngine::EMPTY_UUID)
		{
			return TerraEngine::EMPTY_UUID;
		}
		const auto mapped = instance.sourceToInstanceObjectIds.find(sourceObjectId);
		if (mapped == instance.sourceToInstanceObjectIds.end() || mapped->second == TerraEngine::EMPTY_UUID)
		{
			return std::nullopt;
		}
		return mapped->second;
	}

	bool ValidatePrefabOverrideBatch(const PrefabSource& source, const PrefabInstance& instance, const std::vector<PrefabOverride>& overrides, std::vector<SceneDiagnostic>& diagnostics)
	{
		bool valid = true;
		if (std::find(source.nestedSourceIds.begin(), source.nestedSourceIds.end(), source.sourceId) != source.nestedSourceIds.end())
		{
			diagnostics.push_back(MakePrefabDiagnostic(SceneDiagnosticSeverity::Fatal, source.sourceId,
				"Prefab override batch was rejected because the source directly nests itself.", "Remove the cyclic nested prefab reference before applying overrides."));
			valid = false;
		}
		for (const PrefabOverride& overrideValue : overrides)
		{
			if (overrideValue.stale || source.sourceGraph.objects.find(overrideValue.targetObjectId) == source.sourceGraph.objects.end())
			{
				diagnostics.push_back(MakePrefabDiagnostic(SceneDiagnosticSeverity::Error, overrideValue.targetObjectId,
					"Prefab override batch targets a missing source object.", "Reset the stale override before applying the batch."));
				valid = false;
			}
			if (overrideValue.connectedObjectSourceId != TerraEngine::EMPTY_UUID
				&& !RemapConnectedObjectUuid(instance, overrideValue.connectedObjectSourceId).has_value())
			{
				diagnostics.push_back(MakePrefabDiagnostic(SceneDiagnosticSeverity::Error, overrideValue.targetObjectId,
					"Connected-object override cannot be remapped to this prefab instance.", "Restore the referenced source object or remove the invalid constraint override."));
				valid = false;
			}
			if (overrideValue.kind == PrefabOverrideKind::Property && !overrideValue.componentName.empty())
			{
				PrefabSource validationCopy = source;
				if (!ApplyComponentDefaultOverride(validationCopy, overrideValue, diagnostics)) valid = false;
			}
		}
		return valid;
	}

	bool TryApplyOverridesToDefaults(PrefabSource& source, const PrefabInstance& instance, const std::vector<PrefabOverride>& selectedOverrides, std::vector<SceneDiagnostic>& diagnostics)
	{
		// Stage on a full copy.  Any stale target, invalid remap, or invalid graph
		// leaves the caller's prefab source untouched, including its revision.
		PrefabSource candidate = source;
		if (!ValidatePrefabOverrideBatch(candidate, instance, selectedOverrides, diagnostics))
		{
			return false;
		}
		for (const PrefabOverride& overrideValue : selectedOverrides)
		{
			if (overrideValue.kind != PrefabOverrideKind::Property || overrideValue.stale)
			{
				continue;
			}
			if (auto object = candidate.sourceGraph.objects.find(overrideValue.targetObjectId); object != candidate.sourceGraph.objects.end())
			{
				if (overrideValue.componentName.empty())
				{
					object->second.name = overrideValue.propertyPath == "name" ? overrideValue.authoredValue : object->second.name;
				}
				else if (!ApplyComponentDefaultOverride(candidate, overrideValue, diagnostics))
				{
					return false;
				}
			}
		}
		if (!ValidatePrefabSource(candidate))
		{
			diagnostics.insert(diagnostics.end(), candidate.sourceGraph.diagnostics.begin(), candidate.sourceGraph.diagnostics.end());
			return false;
		}
		++candidate.sourceGraph.revision;
		source = std::move(candidate);
		return true;
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
