// EditorAutomationTransaction.cpp - Immutable transaction envelope checks.

#include "EditorAutomationTransaction.h"

#include "Core/Object/Component.h"
#include "Core/Object/ObjectManager.h"

#include <glm/gtc/quaternion.hpp>
#include <glm/vec3.hpp>

#include <charconv>
#include <cmath>
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

		bool ParseExactAxis(const nlohmann::json& value, std::int64_t& outAxis)
		{
			// JSON numbers cannot preserve every int64 value. Require a decimal
			// string so the cell identity remains exact across transports.
			if (!value.is_string() || value.get_ref<const std::string&>().empty())
			{
				return false;
			}
			const std::string& text = value.get_ref<const std::string&>();
			const char* begin = text.data();
			const char* end = begin + text.size();
			const auto parsed = std::from_chars(begin, end, outAxis);
			return parsed.ec == std::errc{} && parsed.ptr == end;
		}

		bool ParseDoubleOffset(const nlohmann::json& value, glm::dvec3& outOffset)
		{
			if (!value.is_array() || value.size() != 3u)
			{
				return false;
			}
			for (std::size_t index = 0; index < 3u; ++index)
			{
				if (!value[index].is_number())
				{
					return false;
				}
				outOffset[index] = value[index].get<double>();
				if (!std::isfinite(outOffset[index]))
				{
					return false;
				}
			}
			return true;
		}

		bool ParseFiniteFloatArray(const nlohmann::json& value, std::size_t expectedCount)
		{
			if (!value.is_array() || value.size() != expectedCount)
			{
				return false;
			}
			for (const nlohmann::json& member : value)
			{
				if (!member.is_number() || !std::isfinite(member.get<float>()))
				{
					return false;
				}
			}
			return true;
		}
	}

	bool ParseCanonicalWorldPosition(
		const nlohmann::json& value,
		TerraEngine::Scene::WorldPosition& outPosition,
		std::string& outError)
	{
		if (!value.is_object() || !value.contains("cell") || !value.contains("offsetMetres")
			|| !value["cell"].is_object())
		{
			outError = "WorldPosition must contain cell decimal strings and offsetMetres[3].";
			return false;
		}
		const nlohmann::json& cell = value["cell"];
		if (!cell.contains("x") || !cell.contains("y") || !cell.contains("z")
			|| !ParseExactAxis(cell["x"], outPosition.cell.x)
			|| !ParseExactAxis(cell["y"], outPosition.cell.y)
			|| !ParseExactAxis(cell["z"], outPosition.cell.z)
			|| !ParseDoubleOffset(value["offsetMetres"], outPosition.offsetMetres))
		{
			outError = "WorldPosition requires exact decimal int64 cell axes and three finite double offsets.";
			return false;
		}
		const TerraEngine::Scene::WorldCoordinateResult normalized =
			TerraEngine::Scene::NormalizeWorldPosition(outPosition);
		if (!normalized.Succeeded() || normalized.position != outPosition)
		{
			outError = "WorldPosition offsets must already be normalized to the canonical cell range.";
			return false;
		}
		return true;
	}

	bool ParseCanonicalWorldDelta(
		const nlohmann::json& value,
		TerraEngine::Scene::WorldDelta& outDelta,
		std::string& outError)
	{
		if (!value.is_object() || !value.contains("cellDelta") || !value.contains("offsetMetres")
			|| !value["cellDelta"].is_object())
		{
			outError = "WorldDelta must contain cellDelta decimal strings and offsetMetres[3].";
			return false;
		}
		const nlohmann::json& cell = value["cellDelta"];
		if (!cell.contains("x") || !cell.contains("y") || !cell.contains("z")
			|| !ParseExactAxis(cell["x"], outDelta.cellDelta.x)
			|| !ParseExactAxis(cell["y"], outDelta.cellDelta.y)
			|| !ParseExactAxis(cell["z"], outDelta.cellDelta.z)
			|| !ParseDoubleOffset(value["offsetMetres"], outDelta.offsetMetres))
		{
			outError = "WorldDelta requires exact decimal int64 cell axes and three finite double offsets.";
			return false;
		}
		const TerraEngine::Scene::WorldDeltaResult normalized = TerraEngine::Scene::NormalizeWorldDelta(outDelta);
		if (!normalized.Succeeded() || normalized.delta != outDelta)
		{
			outError = "WorldDelta offsets must already be normalized to the canonical cell range.";
			return false;
		}
		return true;
	}

	nlohmann::json SerializeCanonicalWorldPosition(const TerraEngine::Scene::WorldPosition& position)
	{
		return {{"cell", {{"x", TerraEngine::Scene::EncodeWorldCellAxis(position.cell.x)},
			{"y", TerraEngine::Scene::EncodeWorldCellAxis(position.cell.y)},
			{"z", TerraEngine::Scene::EncodeWorldCellAxis(position.cell.z)}}},
			{"offsetMetres", {position.offsetMetres.x, position.offsetMetres.y, position.offsetMetres.z}}};
	}

	AutomationTransactionPreflight PreflightAutomationTransaction(const nlohmann::json& arguments, const AutomationTransactionLimits& limits)
	{
		AutomationTransactionPreflight result;
		if (!arguments.is_object() || arguments.value("format", std::string{}) != "TerraSpatialTransactionV1")
		{
			result.message = "Automation transactions must declare format TerraSpatialTransactionV1.";
			return result;
		}
		if (!arguments.contains("operations") || !arguments["operations"].is_array() || arguments["operations"].empty())
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

			if (kind == "world.set_transform" || kind == "world.translate")
			{
				if (!operation.contains("observedTransformRevision")
					|| !operation["observedTransformRevision"].is_number_unsigned())
				{
					prepared.error = ErrorCategory::Validation;
					prepared.message = "Canonical world operations require observedTransformRevision.";
					return prepared;
				}
				if (target->UsesLocalObject())
				{
					prepared.error = ErrorCategory::Validation;
					prepared.message = "Canonical world operations cannot target an object created in the same transaction.";
					return prepared;
				}
				if (objects.GetObjectTransformRevision(*objects.getObject(target->objectId))
					!= operation["observedTransformRevision"].get<std::uint64_t>())
				{
					prepared.error = ErrorCategory::Conflict;
					prepared.message = "Canonical world operation observed a stale transform revision.";
					return prepared;
				}
			}

			if (kind == "world.set_transform")
			{
				if (!operation.contains("pose"))
				{
					prepared.message = "world.set_transform requires a canonical pose.";
					return prepared;
				}
				TerraEngine::Scene::WorldPose pose = *objects.GetCanonicalWorldPose(*objects.getObject(target->objectId));
				std::string coordinateError;
				if (!ParseCanonicalWorldPosition(operation["pose"], pose.position, coordinateError))
				{
					prepared.message = coordinateError;
					return prepared;
				}
				if (operation["pose"].contains("rotation"))
				{
					if (!ParseFiniteFloatArray(operation["pose"]["rotation"], 4u))
					{
						prepared.error = ErrorCategory::Validation;
						prepared.message = "world.set_transform rotation must be four finite local components.";
						return prepared;
					}
					pose.rotation = glm::quat(operation["pose"]["rotation"][3].get<float>(), operation["pose"]["rotation"][0].get<float>(), operation["pose"]["rotation"][1].get<float>(), operation["pose"]["rotation"][2].get<float>());
				}
				if (operation["pose"].contains("scale"))
				{
					if (!ParseFiniteFloatArray(operation["pose"]["scale"], 3u))
					{
						prepared.error = ErrorCategory::Validation;
						prepared.message = "world.set_transform scale must be three finite local components.";
						return prepared;
					}
					pose.scale = {operation["pose"]["scale"][0].get<float>(), operation["pose"]["scale"][1].get<float>(), operation["pose"]["scale"][2].get<float>()};
				}
				builder.SetWorldTransform(objects, target->objectId, pose);
			}
			else if (kind == "world.translate")
			{
				TerraEngine::Scene::WorldDelta delta;
				std::string coordinateError;
				if (!operation.contains("delta") || !ParseCanonicalWorldDelta(operation["delta"], delta, coordinateError))
				{
					prepared.message = coordinateError.empty() ? "world.translate requires a canonical delta." : coordinateError;
					return prepared;
				}
				builder.TranslateWorld(objects, target->objectId, delta);
			}
			else if (kind == "object.delete")
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
				if (componentName == "Transform")
				{
					prepared.message = "Transform reflection writes are unsupported; use world.set_transform or world.translate.";
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
