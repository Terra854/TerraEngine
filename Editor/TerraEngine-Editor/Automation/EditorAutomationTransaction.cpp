// EditorAutomationTransaction.cpp - Immutable transaction envelope checks.

#include "EditorAutomationTransaction.h"

#include "Core/Object/Component.h"
#include "Core/Object/ObjectManager.h"

#include <glm/gtc/quaternion.hpp>
#include <glm/vec3.hpp>

#include <unordered_set>

namespace TerraEngine::Editor::Automation
{
	namespace
	{
		using TerraEngine::Scene::ExistingObject;
		using TerraEngine::Scene::LocalObject;
		using TerraEngine::Scene::TransactionObjectRef;

		std::optional<TransactionObjectRef> ResolveTransactionTarget(
			const nlohmann::json& value,
			ObjectManager& objects,
			const std::unordered_map<std::string, std::int32_t>& localObjectIds)
		{
			if (value.is_object() && value.contains("localId") && value["localId"].is_string())
			{
				const auto local = localObjectIds.find(value["localId"].get<std::string>());
				return local == localObjectIds.end() ? std::nullopt : std::optional<TransactionObjectRef>(LocalObject(local->second));
			}
			if (value.is_string())
			{
				const std::optional<TerraEngine::UUID> objectId = TerraEngine::ParseUUID(value.get<std::string>());
				return objectId && objects.getObject(*objectId)
					? std::optional<TransactionObjectRef>(ExistingObject(*objectId))
					: std::nullopt;
			}
			if (value.is_number_unsigned() || value.is_number_integer())
			{
				const entt::entity entity = static_cast<entt::entity>(value.get<std::uint32_t>());
				const std::optional<Object> object = objects.getObject(entity);
				return object
					? std::optional<TransactionObjectRef>(ExistingObject(objects.getUUID(*object)))
					: std::nullopt;
			}
			return std::nullopt;
		}

		TerraEngine::Reflection::Variant JsonToVariant(
			const nlohmann::json& value,
			const TerraEngine::Reflection::Type& expectedType)
		{
			using namespace TerraEngine::Reflection;
			if (value.is_boolean()) return Variant{value.get<bool>()};
			if (value.is_number_integer()) return Variant{value.get<int>()};
			if (value.is_number_float()) return Variant{value.get<float>()};
			if (value.is_string())
			{
				if (IsValid(expectedType) && expectedType.info().hash() == entt::type_id<TerraEngine::UUID>().hash())
				{
					const std::optional<TerraEngine::UUID> parsed = TerraEngine::ParseUUID(value.get<std::string>());
					return parsed ? Variant{*parsed} : Variant{};
				}
				return Variant{value.get<std::string>()};
			}
			if (value.is_array() && value.size() == 3u)
			{
				return Variant{glm::vec3(value[0].get<float>(), value[1].get<float>(), value[2].get<float>())};
			}
			if (value.is_array() && value.size() == 4u
				&& IsValid(expectedType) && expectedType.info().hash() == entt::type_id<glm::quat>().hash())
			{
				return Variant{glm::quat(value[0].get<float>(), value[1].get<float>(), value[2].get<float>(), value[3].get<float>())};
			}
			return {};
		}
	}

	AutomationTransactionPreflight PreflightAutomationTransaction(const nlohmann::json& arguments, const AutomationTransactionLimits& limits)
	{
		AutomationTransactionPreflight result;
		if (!arguments.is_object() || !arguments.contains("operations") || !arguments["operations"].is_array() || arguments["operations"].empty())
		{
			result.message = "A non-empty operations array is required.";
			return result;
		}
		result.operationCount = arguments["operations"].size();
		if (result.operationCount > limits.maximumOperations || arguments.dump().size() > limits.maximumPayloadBytes)
		{
			result.error = ErrorCategory::OverBudget;
			result.message = "Automation transaction exceeds its operation or payload budget.";
			return result;
		}
		result.valid = true;
		result.message = "Transaction envelope is ready for protected preflight.";
		return result;
	}

	PreparedAutomationTransaction PrepareAutomationTransaction(
		const nlohmann::json& arguments,
		ObjectManager& objects,
		std::string transactionId)
	{
		PreparedAutomationTransaction prepared;
		const AutomationTransactionPreflight envelope = PreflightAutomationTransaction(arguments);
		if (!envelope.valid)
		{
			prepared.error = envelope.error;
			prepared.message = envelope.message;
			return prepared;
		}

		TerraEngine::Scene::TransactionalMutationBuilder builder(
			std::move(transactionId),
			TerraEngine::Scene::TransactionOrigin::Automation,
			"editor.mcp");
		builder.SetUndoGroup(arguments.value("label", std::string("Automation transaction")));
		std::unordered_set<TerraEngine::UUID> observedObjects;
		std::int32_t nextLocalObjectId = 1;

		const auto observeExisting = [&](const TransactionObjectRef& target) {
			if (!target.UsesLocalObject() && target.objectId != TerraEngine::EMPTY_UUID && observedObjects.insert(target.objectId).second)
			{
				builder.ObserveObject(objects, target.objectId);
			}
		};

		for (const nlohmann::json& operation : arguments["operations"])
		{
			if (!operation.is_object() || !operation.contains("kind") || !operation["kind"].is_string())
			{
				prepared.message = "Every transaction operation requires a typed kind.";
				return prepared;
			}

			const std::string kind = operation["kind"].get<std::string>();
			if (kind == "object.create")
			{
				const std::string localId = operation.value("localId", std::string{});
				if (localId.empty() || prepared.localObjectIds.contains(localId))
				{
					prepared.error = ErrorCategory::Conflict;
					prepared.message = "Object creation local IDs must be unique and non-empty.";
					return prepared;
				}
				const std::int32_t numericLocalId = nextLocalObjectId++;
				prepared.localObjectIds.emplace(localId, numericLocalId);
				TransactionObjectRef parent;
				if (operation.contains("parent") && !operation["parent"].is_null())
				{
					const std::optional<TransactionObjectRef> resolvedParent = ResolveTransactionTarget(operation["parent"], objects, prepared.localObjectIds);
					if (!resolvedParent)
					{
						prepared.error = ErrorCategory::MissingTarget;
						prepared.message = "Object creation parent does not exist or is declared later in the transaction.";
						return prepared;
					}
					parent = *resolvedParent;
					observeExisting(parent);
				}
				builder.CreateObject(numericLocalId, operation.value("name", std::string("Object")), parent);
				if (operation.value("select", false)) prepared.selectionTarget = LocalObject(numericLocalId);
				continue;
			}

			const std::optional<TransactionObjectRef> target = ResolveTransactionTarget(operation.value("target", nlohmann::json{}), objects, prepared.localObjectIds);
			if (!target)
			{
				prepared.error = ErrorCategory::MissingTarget;
				prepared.message = "Transaction target does not exist or is declared later in the transaction.";
				return prepared;
			}
			observeExisting(*target);

			if (kind == "object.delete")
			{
				builder.DeleteObject(*target);
			}
			else if (kind == "object.set_metadata")
			{
				if (operation.contains("name") && operation["name"].is_string()) builder.SetName(*target, operation["name"].get<std::string>());
				if (operation.contains("tag") && operation["tag"].is_string()) builder.SetTag(*target, operation["tag"].get<std::string>());
				if (operation.contains("layer") && operation["layer"].is_string()) builder.SetLayer(*target, operation["layer"].get<std::string>());
				if (operation.contains("active") && operation["active"].is_boolean()) builder.SetActive(*target, operation["active"].get<bool>());
				if (operation.contains("static") && operation["static"].is_boolean()) builder.SetStatic(*target, operation["static"].get<bool>());
			}
			else if (kind == "object.set_parent")
			{
				TransactionObjectRef parent;
				if (operation.contains("parent") && !operation["parent"].is_null())
				{
					const std::optional<TransactionObjectRef> resolvedParent = ResolveTransactionTarget(operation["parent"], objects, prepared.localObjectIds);
					if (!resolvedParent)
					{
						prepared.error = ErrorCategory::MissingTarget;
						prepared.message = "Transaction parent does not exist or is declared later in the transaction.";
						return prepared;
					}
					parent = *resolvedParent;
					observeExisting(parent);
				}
				builder.ReparentObject(*target, parent);
			}
			else if (kind == "component.add" || kind == "component.remove")
			{
				const std::string component = operation.value("component", std::string{});
				if (component.empty())
				{
					prepared.message = "Component operations require a component name.";
					return prepared;
				}
				if (kind == "component.add") builder.AddComponent(*target, component);
				else builder.RemoveComponent(*target, component);
			}
			else if (kind == "component.set_property")
			{
				const std::string componentName = operation.value("component", std::string{});
				const std::string propertyName = operation.value("property", std::string{});
				if (componentName.empty() || propertyName.empty() || !operation.contains("value"))
				{
					prepared.message = "Component property operations require component, property, and value.";
					return prepared;
				}
				TerraEngine::Reflection::Variant value;
				{
					std::lock_guard registryLock(ComponentAccessRegistryMutex());
					const auto component = ComponentAccessRegistry().find(componentName);
					if (component == ComponentAccessRegistry().end())
					{
						prepared.message = "Component type is not registered for automation.";
						return prepared;
					}
					const TerraEngine::Reflection::Property property = TerraEngine::Reflection::FindProperty(component->second.reflectionType, propertyName);
					value = JsonToVariant(operation["value"], TerraEngine::Reflection::GetPropertyType(property));
				}
				if (!TerraEngine::Reflection::IsValid(value))
				{
					prepared.message = "Component property value cannot be represented by the reflected property type.";
					return prepared;
				}
				builder.UpdateComponentProperty(*target, componentName, propertyName, std::move(value));
			}
			else if (kind == "selection.set")
			{
				prepared.selectionTarget = *target;
			}
			else
			{
				prepared.error = ErrorCategory::UnsupportedCapability;
				prepared.message = "Transaction operation kind is not supported: " + kind;
				return prepared;
			}
		}

		prepared.transaction = builder.Build();
		prepared.valid = true;
		prepared.message = "Transaction is ready for protected commit.";
		return prepared;
	}
}
