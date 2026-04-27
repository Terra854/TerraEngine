// ScriptComponent.h — Component managing managed script bindings and their serialized state.
//
// This file defines the structures for script field descriptors, snapshots, and
// diagnostic metadata used to bridge C++ and C# script state. It handles the
// persistence of script variables within scenes and facilitates data migration
// during script reloads.

#pragma once

#include "Core/Object/Component.h"
#include "Utilities/UUID.h"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

// Static reflection metadata coming from the managed bridge. This describes
// what the script type looks like, independent of any one live instance.
struct ScriptFieldDescriptor
{
	std::string fieldPath;
	std::string serializedName;
	std::string displayName;
	std::string valueKind = "Unsupported";
	std::string containerKind = "Scalar";
	bool editable = false;
	bool hiddenInInspector = false;
	std::string readOnlyReason;
	int order = 0;
	std::string tooltip;
	double rangeMin = 0.0;
	double rangeMax = 0.0;
	bool hasRange = false;
	std::string typeFingerprint;
	nlohmann::json defaultValue;
	std::vector<std::string> enumOptions;
	std::vector<ScriptFieldDescriptor> children;
};

// to_json is an implementation entry point; keep its inputs, side effects, and ownership expectations aligned with callers.
inline void to_json(nlohmann::json& jsonValue, const ScriptFieldDescriptor& descriptor)
{
	jsonValue = nlohmann::json{
		{ "fieldPath", descriptor.fieldPath },
		{ "serializedName", descriptor.serializedName },
		{ "displayName", descriptor.displayName },
		{ "valueKind", descriptor.valueKind },
		{ "containerKind", descriptor.containerKind },
		{ "editable", descriptor.editable },
		{ "hiddenInInspector", descriptor.hiddenInInspector },
		{ "readOnlyReason", descriptor.readOnlyReason },
		{ "order", descriptor.order },
		{ "tooltip", descriptor.tooltip },
		{ "typeFingerprint", descriptor.typeFingerprint },
		{ "defaultValue", descriptor.defaultValue },
		{ "enumOptions", descriptor.enumOptions },
		{ "children", descriptor.children }
	};

	// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
	if (descriptor.hasRange)
	{
		jsonValue["rangeMin"] = descriptor.rangeMin;
		jsonValue["rangeMax"] = descriptor.rangeMax;
	}
}

// from_json is an implementation entry point; keep its inputs, side effects, and ownership expectations aligned with callers.
inline void from_json(const nlohmann::json& jsonValue, ScriptFieldDescriptor& descriptor)
{
	descriptor.fieldPath = jsonValue.value("fieldPath", std::string{});
	descriptor.serializedName = jsonValue.value("serializedName", descriptor.fieldPath);
	descriptor.displayName = jsonValue.value("displayName", descriptor.serializedName);
	descriptor.valueKind = jsonValue.value("valueKind", std::string{ "Unsupported" });
	descriptor.containerKind = jsonValue.value("containerKind", std::string{ "Scalar" });
	descriptor.editable = jsonValue.value("editable", false);
	descriptor.hiddenInInspector = jsonValue.value("hiddenInInspector", false);
	descriptor.readOnlyReason = jsonValue.value("readOnlyReason", std::string{});
	descriptor.order = jsonValue.value("order", 0);
	descriptor.tooltip = jsonValue.value("tooltip", std::string{});
	descriptor.typeFingerprint = jsonValue.value("typeFingerprint", std::string{});
	descriptor.defaultValue = jsonValue.contains("defaultValue") ? jsonValue["defaultValue"] : nlohmann::json{};
	descriptor.enumOptions = jsonValue.value("enumOptions", std::vector<std::string>{});
	descriptor.children = jsonValue.value("children", std::vector<ScriptFieldDescriptor>{});
	descriptor.hasRange = jsonValue.contains("rangeMin") && jsonValue.contains("rangeMax");
	descriptor.rangeMin = jsonValue.value("rangeMin", 0.0);
	descriptor.rangeMax = jsonValue.value("rangeMax", 0.0);
}

// Live runtime sample captured for the actively watched inspector selection.
// struct ScriptFieldSnapshot groups related state and operations so callers have one documented contract.
struct ScriptFieldSnapshot
{
	std::string fieldPath;
	nlohmann::json value;
	std::string source = "Authored";
	std::string refreshState = "Clean";
	bool differsFromAuthored = false;
	std::uint64_t lastObservedVersion = 0;
};

// to_json is an implementation entry point; keep its inputs, side effects, and ownership expectations aligned with callers.
inline void to_json(nlohmann::json& jsonValue, const ScriptFieldSnapshot& snapshot)
{
	jsonValue = nlohmann::json{
		{ "fieldPath", snapshot.fieldPath },
		{ "value", snapshot.value },
		{ "source", snapshot.source },
		{ "refreshState", snapshot.refreshState },
		{ "differsFromAuthored", snapshot.differsFromAuthored },
		{ "lastObservedVersion", snapshot.lastObservedVersion }
	};
}

// from_json is an implementation entry point; keep its inputs, side effects, and ownership expectations aligned with callers.
inline void from_json(const nlohmann::json& jsonValue, ScriptFieldSnapshot& snapshot)
{
	snapshot.fieldPath = jsonValue.value("fieldPath", std::string{});
	snapshot.value = jsonValue.contains("value") ? jsonValue["value"] : nlohmann::json{};
	snapshot.source = jsonValue.value("source", std::string{ "Authored" });
	snapshot.refreshState = jsonValue.value("refreshState", std::string{ "Clean" });
	snapshot.differsFromAuthored = jsonValue.value("differsFromAuthored", false);
	snapshot.lastObservedVersion = jsonValue.value("lastObservedVersion", static_cast<std::uint64_t>(0));
}

// Diagnostics can come from type reflection, migration, or live runtime reads.
// struct ScriptFieldDiagnostic groups related state and operations so callers have one documented contract.
struct ScriptFieldDiagnostic
{
	std::string diagnosticId;
	std::string scope = "Field";
	std::string fieldPath;
	std::string severity = "Info";
	std::string code;
	std::string message;
	std::vector<std::string> repairActionIds;
};

// to_json is an implementation entry point; keep its inputs, side effects, and ownership expectations aligned with callers.
inline void to_json(nlohmann::json& jsonValue, const ScriptFieldDiagnostic& diagnostic)
{
	jsonValue = nlohmann::json{
		{ "diagnosticId", diagnostic.diagnosticId },
		{ "scope", diagnostic.scope },
		{ "fieldPath", diagnostic.fieldPath },
		{ "severity", diagnostic.severity },
		{ "code", diagnostic.code },
		{ "message", diagnostic.message },
		{ "repairActionIds", diagnostic.repairActionIds }
	};
}

// from_json is an implementation entry point; keep its inputs, side effects, and ownership expectations aligned with callers.
inline void from_json(const nlohmann::json& jsonValue, ScriptFieldDiagnostic& diagnostic)
{
	diagnostic.diagnosticId = jsonValue.value("diagnosticId", std::string{});
	diagnostic.scope = jsonValue.value("scope", std::string{ "Field" });
	diagnostic.fieldPath = jsonValue.value("fieldPath", std::string{});
	diagnostic.severity = jsonValue.value("severity", std::string{ "Info" });
	diagnostic.code = jsonValue.value("code", std::string{});
	diagnostic.message = jsonValue.value("message", std::string{});
	diagnostic.repairActionIds = jsonValue.value("repairActionIds", std::vector<std::string>{});
}

// Repair actions are UI-facing affordances derived from diagnostics and
// migration results, such as clearing stale data after a reload.
struct ScriptRepairAction
{
	std::string actionId;
	std::string kind;
	std::string label;
	std::string targetPath;
	bool requiresUserSelection = false;
};

// to_json is an implementation entry point; keep its inputs, side effects, and ownership expectations aligned with callers.
inline void to_json(nlohmann::json& jsonValue, const ScriptRepairAction& action)
{
	jsonValue = nlohmann::json{
		{ "actionId", action.actionId },
		{ "kind", action.kind },
		{ "label", action.label },
		{ "targetPath", action.targetPath },
		{ "requiresUserSelection", action.requiresUserSelection }
	};
}

// from_json is an implementation entry point; keep its inputs, side effects, and ownership expectations aligned with callers.
inline void from_json(const nlohmann::json& jsonValue, ScriptRepairAction& action)
{
	action.actionId = jsonValue.value("actionId", std::string{});
	action.kind = jsonValue.value("kind", std::string{});
	action.label = jsonValue.value("label", std::string{});
	action.targetPath = jsonValue.value("targetPath", std::string{});
	action.requiresUserSelection = jsonValue.value("requiresUserSelection", false);
}

// Summarizes how authored values survived a script reload so the inspector can
// explain whether data was preserved, converted, or left broken.
struct ScriptBindingMigrationResult
{
	std::string bindingId;
	std::string fromAssemblyId;
	std::string toAssemblyId;
	std::vector<std::string> preservedFieldPaths;
	std::vector<std::string> convertedFieldPaths;
	std::vector<std::string> brokenFieldPaths;
	std::vector<std::string> hiddenStoredFieldPaths;
};

// to_json is an implementation entry point; keep its inputs, side effects, and ownership expectations aligned with callers.
inline void to_json(nlohmann::json& jsonValue, const ScriptBindingMigrationResult& migrationResult)
{
	jsonValue = nlohmann::json{
		{ "bindingId", migrationResult.bindingId },
		{ "fromAssemblyId", migrationResult.fromAssemblyId },
		{ "toAssemblyId", migrationResult.toAssemblyId },
		{ "preservedFieldPaths", migrationResult.preservedFieldPaths },
		{ "convertedFieldPaths", migrationResult.convertedFieldPaths },
		{ "brokenFieldPaths", migrationResult.brokenFieldPaths },
		{ "hiddenStoredFieldPaths", migrationResult.hiddenStoredFieldPaths }
	};
}

// from_json is an implementation entry point; keep its inputs, side effects, and ownership expectations aligned with callers.
inline void from_json(const nlohmann::json& jsonValue, ScriptBindingMigrationResult& migrationResult)
{
	migrationResult.bindingId = jsonValue.value("bindingId", std::string{});
	migrationResult.fromAssemblyId = jsonValue.value("fromAssemblyId", std::string{});
	migrationResult.toAssemblyId = jsonValue.value("toAssemblyId", std::string{});
	migrationResult.preservedFieldPaths = jsonValue.value("preservedFieldPaths", std::vector<std::string>{});
	migrationResult.convertedFieldPaths = jsonValue.value("convertedFieldPaths", std::vector<std::string>{});
	migrationResult.brokenFieldPaths = jsonValue.value("brokenFieldPaths", std::vector<std::string>{});
	migrationResult.hiddenStoredFieldPaths = jsonValue.value("hiddenStoredFieldPaths", std::vector<std::string>{});
}

// ComputeScriptBindingTypeId is an implementation entry point; keep its inputs, side effects, and ownership expectations aligned with callers.
inline std::uint64_t ComputeScriptBindingTypeId(std::string_view className)
{
	// Export staging rewrites bindings from class names to deterministic FNV-64
	// identifiers, so runtime lookup helpers need the same hash to match a
	// managed type request against a staged binding that no longer stores text.
	constexpr std::uint64_t kFnvOffsetBasis = 14695981039346656037ull;
	constexpr std::uint64_t kFnvPrime = 1099511628211ull;
	std::uint64_t hash = kFnvOffsetBasis;
	// Iterate over the current data snapshot; avoid changing ownership rules inside the loop body.
	for (unsigned char value : className)
	{
		hash ^= static_cast<std::uint64_t>(value);
		hash *= kFnvPrime;
	}

	return hash;
}

// Authored value persisted into the scene for one reflected script field.
// struct ScriptFieldValue groups related state and operations so callers have one documented contract.
struct ScriptFieldValue
{
	std::string fieldName;
	std::string fieldPath;
	std::string displayName;
	std::string valueKind;
	nlohmann::json value;
	std::string compatibility = "Current";
	std::string sourceTypeFingerprint;
	std::string persistenceState = "Current";
	std::string diagnosticMessage;
};

// to_json is an implementation entry point; keep its inputs, side effects, and ownership expectations aligned with callers.
inline void to_json(nlohmann::json& jsonValue, const ScriptFieldValue& fieldValue)
{
	jsonValue = nlohmann::json{
		{ "fieldName", fieldValue.fieldName.empty() ? fieldValue.fieldPath : fieldValue.fieldName },
		{ "fieldPath", fieldValue.fieldPath.empty() ? fieldValue.fieldName : fieldValue.fieldPath },
		{ "displayName", fieldValue.displayName },
		{ "valueKind", fieldValue.valueKind },
		{ "value", fieldValue.value },
		{ "compatibility", fieldValue.compatibility },
		{ "sourceTypeFingerprint", fieldValue.sourceTypeFingerprint },
		{ "persistenceState", fieldValue.persistenceState },
		{ "diagnosticMessage", fieldValue.diagnosticMessage }
	};
}

// from_json is an implementation entry point; keep its inputs, side effects, and ownership expectations aligned with callers.
inline void from_json(const nlohmann::json& jsonValue, ScriptFieldValue& fieldValue)
{
	fieldValue.fieldName = jsonValue.value("fieldName", std::string{});
	fieldValue.fieldPath = jsonValue.value("fieldPath", fieldValue.fieldName);
	// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
	if (fieldValue.fieldName.empty())
	{
		fieldValue.fieldName = fieldValue.fieldPath;
	}

	fieldValue.displayName = jsonValue.value("displayName", fieldValue.fieldName);
	fieldValue.valueKind = jsonValue.value("valueKind", std::string{});
	fieldValue.value = jsonValue.contains("value") ? jsonValue["value"] : nlohmann::json{};
	fieldValue.compatibility = jsonValue.value("compatibility", std::string{ "Current" });
	fieldValue.sourceTypeFingerprint = jsonValue.value("sourceTypeFingerprint", std::string{});
	fieldValue.persistenceState = jsonValue.value("persistenceState", fieldValue.compatibility);
	fieldValue.diagnosticMessage = jsonValue.value("diagnosticMessage", std::string{});
}

// struct ScriptBinding groups related state and operations so callers have one documented contract.
struct ScriptBinding
{
	std::string bindingId;
	TerraEngine::UUID assetGuid = TerraEngine::EMPTY_UUID;
	std::string className;
	std::uint64_t scriptId = 0;
	std::string status;
	bool enabled = true;
	int orderIndex = 0;
	// Only authored values are serialized here. Runtime state is kept in the
	// transient members below so stopping play can discard live overrides cleanly.
	std::vector<ScriptFieldValue> serializedValues;
	std::vector<std::string> diagnosticIds;

	// Runtime-only descriptor, snapshot, and repair metadata stay outside the
	// persisted BindingsJson payload so TerraEngine can keep authored values
	// stable while still showing live state for the active inspector selection.
	std::string fullTypeName;
	std::string displayName;
	bool runtimeAvailable = false;
	int runtimeOverrideCount = 0;
	std::vector<ScriptFieldValue> runtimeOverrides;
	std::vector<ScriptFieldDescriptor> fieldDescriptors;
	std::vector<ScriptFieldSnapshot> liveSnapshots;
	std::vector<ScriptFieldDiagnostic> diagnostics;
	std::vector<ScriptRepairAction> repairActions;
	ScriptBindingMigrationResult migrationResult;
	std::string lastBridgeMessage;
	std::uint64_t snapshotVersion = 0;

	[[nodiscard]] ScriptFieldValue* FindSerializedValue(std::string_view fieldPathText)
	{
		const auto matchByPath = [fieldPathText](const ScriptFieldValue& fieldValue) {
			return fieldValue.fieldPath == fieldPathText || fieldValue.fieldName == fieldPathText;
		};

		auto it = std::find_if(serializedValues.begin(), serializedValues.end(), matchByPath);
		return it == serializedValues.end() ? nullptr : &(*it);
	}

	[[nodiscard]] const ScriptFieldValue* FindSerializedValue(std::string_view fieldPathText) const
	{
		const auto matchByPath = [fieldPathText](const ScriptFieldValue& fieldValue) {
			return fieldValue.fieldPath == fieldPathText || fieldValue.fieldName == fieldPathText;
		};

		auto it = std::find_if(serializedValues.begin(), serializedValues.end(), matchByPath);
		return it == serializedValues.end() ? nullptr : &(*it);
	}

	[[nodiscard]] ScriptFieldValue* FindRuntimeOverride(std::string_view fieldPathText)
	{
		const auto matchByPath = [fieldPathText](const ScriptFieldValue& fieldValue) {
			return fieldValue.fieldPath == fieldPathText || fieldValue.fieldName == fieldPathText;
		};

		auto it = std::find_if(runtimeOverrides.begin(), runtimeOverrides.end(), matchByPath);
		return it == runtimeOverrides.end() ? nullptr : &(*it);
	}

	[[nodiscard]] const ScriptFieldSnapshot* FindLiveSnapshot(std::string_view fieldPathText) const
	{
		const auto matchByPath = [fieldPathText](const ScriptFieldSnapshot& snapshot) {
			return snapshot.fieldPath == fieldPathText;
		};

		auto it = std::find_if(liveSnapshots.begin(), liveSnapshots.end(), matchByPath);
		return it == liveSnapshots.end() ? nullptr : &(*it);
	}

	[[nodiscard]] const ScriptFieldDescriptor* FindFieldDescriptor(std::string_view fieldPathText) const
	{
		const auto visitDescriptor = [&](const auto& self, const std::vector<ScriptFieldDescriptor>& descriptors) -> const ScriptFieldDescriptor* {
			// Iterate over the current data snapshot; avoid changing ownership rules inside the loop body.
			for (const ScriptFieldDescriptor& descriptor : descriptors)
			{
				if (descriptor.fieldPath == fieldPathText)
				{
					return &descriptor;
				}

				// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
				if (const ScriptFieldDescriptor* nested = self(self, descriptor.children))
				{
					return nested;
				}
			}

			return nullptr;
		};

		return visitDescriptor(visitDescriptor, fieldDescriptors);
	}

	[[nodiscard]] ScriptFieldValue& UpsertSerializedValue(std::string_view fieldPathText, std::string_view valueKindText)
	{
		// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
		if (ScriptFieldValue* existingValue = FindSerializedValue(fieldPathText))
		{
			if (existingValue->fieldPath.empty())
			{
				existingValue->fieldPath = std::string(fieldPathText);
			}
			// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
			if (existingValue->fieldName.empty())
			{
				existingValue->fieldName = existingValue->fieldPath;
			}
			// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
			if (existingValue->valueKind.empty())
			{
				existingValue->valueKind = std::string(valueKindText);
			}
			return *existingValue;
		}

		ScriptFieldValue nextValue;
		nextValue.fieldPath = std::string(fieldPathText);
		nextValue.fieldName = nextValue.fieldPath;
		nextValue.displayName = nextValue.fieldPath;
		nextValue.valueKind = std::string(valueKindText);
		serializedValues.push_back(std::move(nextValue));
		return serializedValues.back();
	}

	// RemoveSerializedValue is an implementation entry point; keep its inputs, side effects, and ownership expectations aligned with callers.
	bool RemoveSerializedValue(std::string_view fieldPathText)
	{
		const auto matchByPath = [fieldPathText](const ScriptFieldValue& fieldValue) {
			return fieldValue.fieldPath == fieldPathText || fieldValue.fieldName == fieldPathText;
		};

		auto it = std::remove_if(serializedValues.begin(), serializedValues.end(), matchByPath);
		// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
		if (it == serializedValues.end())
		{
			return false;
		}

		serializedValues.erase(it, serializedValues.end());
		return true;
	}

	// ClearRuntimeState is an implementation entry point; keep its inputs, side effects, and ownership expectations aligned with callers.
	void ClearRuntimeState()
	{
		runtimeAvailable = false;
		runtimeOverrideCount = 0;
		runtimeOverrides.clear();
		liveSnapshots.clear();
		diagnostics.clear();
		repairActions.clear();
		lastBridgeMessage.clear();
		snapshotVersion = 0;
	}
};

// to_json is an implementation entry point; keep its inputs, side effects, and ownership expectations aligned with callers.
inline void to_json(nlohmann::json& jsonValue, const ScriptBinding& binding)
{
	jsonValue = nlohmann::json{
		{ "bindingId", binding.bindingId },
		{ "assetGuid", binding.assetGuid },
		{ "className", binding.className },
		{ "status", binding.status },
		{ "enabled", binding.enabled },
		{ "orderIndex", binding.orderIndex },
		{ "serializedValues", binding.serializedValues },
		{ "diagnosticIds", binding.diagnosticIds }
	};

	// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
	if (binding.scriptId != 0)
	{
		// Authored scenes keep using className, while staged export copies can
		// persist the resolved scriptId without rewriting editor source assets.
		jsonValue["scriptId"] = binding.scriptId;
	}
}

// from_json is an implementation entry point; keep its inputs, side effects, and ownership expectations aligned with callers.
inline void from_json(const nlohmann::json& jsonValue, ScriptBinding& binding)
{
	binding.bindingId = jsonValue.value("bindingId", std::string{});
	binding.assetGuid = jsonValue.value("assetGuid", TerraEngine::EMPTY_UUID);
	binding.className = jsonValue.value("className", std::string{});
	binding.scriptId = jsonValue.value("scriptId", static_cast<std::uint64_t>(0));
	binding.status = jsonValue.value("status", std::string{});
	// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
	if (binding.status == "PendingCompile")
	{
		binding.status.clear();
	}
	binding.enabled = jsonValue.value("enabled", true);
	binding.orderIndex = jsonValue.value("orderIndex", 0);
	binding.serializedValues = jsonValue.value("serializedValues", std::vector<ScriptFieldValue>{});
	binding.diagnosticIds = jsonValue.value("diagnosticIds", std::vector<std::string>{});
	binding.fullTypeName.clear();
	binding.displayName.clear();
	binding.runtimeAvailable = false;
	binding.runtimeOverrideCount = 0;
	binding.runtimeOverrides.clear();
	binding.fieldDescriptors.clear();
	binding.liveSnapshots.clear();
	binding.diagnostics.clear();
	binding.repairActions.clear();
	binding.migrationResult = ScriptBindingMigrationResult{};
	binding.lastBridgeMessage.clear();
	binding.snapshotVersion = 0;

	// Iterate over the current data snapshot; avoid changing ownership rules inside the loop body.
	for (ScriptFieldValue& fieldValue : binding.serializedValues)
	{
		if (fieldValue.fieldPath.empty())
		{
			fieldValue.fieldPath = fieldValue.fieldName;
		}
		// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
		if (fieldValue.fieldName.empty())
		{
			fieldValue.fieldName = fieldValue.fieldPath;
		}
		// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
		if (fieldValue.displayName.empty())
		{
			fieldValue.displayName = fieldValue.fieldName;
		}
	}
}

/**
 * @class ScriptComponent
 * @brief Stores one ordered list of gameplay script bindings for an object.
 */
class ScriptComponent : public Component<ScriptComponent> {
public:
	static constexpr std::string_view kComponentName = "Script";

	ScriptComponent() = default;

	// GetBindings is an implementation entry point; keep its inputs, side effects, and ownership expectations aligned with callers.
	std::vector<ScriptBinding>& GetBindings()
	{
		return bindings;
	}

	// GetBindings is an implementation entry point; keep its inputs, side effects, and ownership expectations aligned with callers.
	const std::vector<ScriptBinding>& GetBindings() const
	{
		return bindings;
	}

	[[nodiscard]] ScriptBinding* FindBinding(std::string_view bindingIdText)
	{
		auto it = std::find_if(bindings.begin(), bindings.end(), [bindingIdText](const ScriptBinding& binding) {
			return binding.bindingId == bindingIdText;
		});

		return it == bindings.end() ? nullptr : &(*it);
	}

	[[nodiscard]] const ScriptBinding* FindBinding(std::string_view bindingIdText) const
	{
		auto it = std::find_if(bindings.begin(), bindings.end(), [bindingIdText](const ScriptBinding& binding) {
			return binding.bindingId == bindingIdText;
		});

		return it == bindings.end() ? nullptr : &(*it);
	}

	// HasBinding is an implementation entry point; keep its inputs, side effects, and ownership expectations aligned with callers.
	bool HasBinding(std::string_view className) const
	{
		const std::uint64_t requestedScriptId = className.empty()
			? static_cast<std::uint64_t>(0)
			: ComputeScriptBindingTypeId(className);

		// Iterate over the current data snapshot; avoid changing ownership rules inside the loop body.
		for (const ScriptBinding& binding : bindings)
		{
			if (binding.className == className)
			{
				return true;
			}

			// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
			if (!className.empty() && binding.scriptId != 0 && binding.scriptId == requestedScriptId)
			{
				return true;
			}
		}

		return false;
	}

	// AddBinding is an implementation entry point; keep its inputs, side effects, and ownership expectations aligned with callers.
	ScriptBinding& AddBinding(const TerraEngine::UUID& assetGuid, std::string_view className)
	{
		ScriptBinding binding;
		binding.bindingId = TerraEngine::ToHexString(TerraEngine::GenerateUUID());
		binding.assetGuid = assetGuid;
		binding.className = std::string(className);
		binding.fullTypeName = binding.className;
		binding.status.clear();
		bindings.push_back(std::move(binding));
		SynchronizeStorageFromBindings();
		return bindings.back();
	}

	// RemoveBinding is an implementation entry point; keep its inputs, side effects, and ownership expectations aligned with callers.
	bool RemoveBinding(std::string_view bindingId)
	{
		for (auto it = bindings.begin(); it != bindings.end(); ++it)
		{
			// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
			if (it->bindingId == bindingId)
			{
				bindings.erase(it);
				SynchronizeStorageFromBindings();
				return true;
			}
		}

		return false;
	}

	// ClearRuntimeState is an implementation entry point; keep its inputs, side effects, and ownership expectations aligned with callers.
	void ClearRuntimeState()
	{
		for (ScriptBinding& binding : bindings)
		{
			binding.ClearRuntimeState();
		}
	}

	// SynchronizeStorageFromBindings is an implementation entry point; keep its inputs, side effects, and ownership expectations aligned with callers.
	void SynchronizeStorageFromBindings()
	{
		for (std::size_t index = 0; index < bindings.size(); ++index)
		{
			bindings[index].orderIndex = static_cast<int>(index);
		}

		// Keep the reflected JSON mirror normalized so serializer and undo/redo
		// always round-trip the exact authored binding state.
		bindingsStorage = nlohmann::json(bindings).dump();
	}

	// SynchronizeBindingsFromStorage is an implementation entry point; keep its inputs, side effects, and ownership expectations aligned with callers.
	void SynchronizeBindingsFromStorage()
	{
		if (bindingsStorage.empty())
		{
			bindings.clear();
			bindingsStorage = "[]";
			return;
		}

		try
		{
			bindings = nlohmann::json::parse(bindingsStorage).get<std::vector<ScriptBinding>>();
		}
		catch (...)
		{
			bindings.clear();
			bindingsStorage = "[]";
		}

		// Iterate over the current data snapshot; avoid changing ownership rules inside the loop body.
		for (std::size_t index = 0; index < bindings.size(); ++index)
		{
			if (bindings[index].bindingId.empty())
			{
				bindings[index].bindingId = TerraEngine::ToHexString(TerraEngine::GenerateUUID());
			}

			bindings[index].orderIndex = static_cast<int>(index);
		}

		// Rewrite the cache after parsing so future comparisons and scene saves
		// use one stable JSON layout instead of preserving arbitrary whitespace.
		bindingsStorage = nlohmann::json(bindings).dump();
	}

	// GetBindingsJson is an implementation entry point; keep its inputs, side effects, and ownership expectations aligned with callers.
	const std::string& GetBindingsJson() const
	{
		return bindingsStorage;
	}

	// SetBindingsJson is an implementation entry point; keep its inputs, side effects, and ownership expectations aligned with callers.
	void SetBindingsJson(const std::string& jsonText)
	{
		bindingsStorage = jsonText.empty() ? "[]" : jsonText;
		SynchronizeBindingsFromStorage();
	}

	std::string lastResolvedAssemblyId;

	// reflect is an implementation entry point; keep its inputs, side effects, and ownership expectations aligned with callers.
	static void reflect(entt::meta_factory<ScriptComponent>& factory)
	{
		TerraEngine::Reflection::RegisterAccessorProperty<&ScriptComponent::SetBindingsJson, &ScriptComponent::GetBindingsJson>(factory, "BindingsJson");
		TerraEngine::Reflection::RegisterDataProperty<&ScriptComponent::lastResolvedAssemblyId>(factory, "LastResolvedAssemblyId");
	}

private:
	std::vector<ScriptBinding> bindings;
	std::string bindingsStorage = "[]";
};
