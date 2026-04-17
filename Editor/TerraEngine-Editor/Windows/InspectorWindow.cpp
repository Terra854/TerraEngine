#include "InspectorWindow.h"
#include "../Subsystems/MainEditorSubsystem.h"
#include "../Subsystems/ScriptCompilationSubsystem.h"
#include "../Subsystems/UndoRedoSystem.h"
#include "Engine/Core/Object/ObjectManager.h"
#include "Engine/Core/Object/Component.h"
#include "Engine/Resource/ResourceManager.h"
#include "Engine/Renderer/RenderSystem.h"
#include "Engine/Components/LightComponent.h"
#include "Engine/Components/UIComponent.h"
#include "Engine/Components/ScriptComponent.h"
#include "Engine/Scripting/ScriptingSystem.h"
#include "AssetDatabase.h"
#include "Editor/AssetCompilers/MeshCompiler.h"
#include "Editor/AssetCompilers/TextureCompiler.h"
#include "Engine/File Formats/TerraEngineMaterialFile.h"
#include "Engine/File Formats/TerraEngineMeshFile.h"
#include "Engine/File Formats/TerraEngineTextureFile.h"
#include <imgui.h>
#include <imgui_internal.h>
#include <glm/geometric.hpp>
#include <glm/vec3.hpp>
#include <glm/gtc/quaternion.hpp>
#include <glm/trigonometric.hpp>
#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdint>
#include <fstream>
#include <filesystem>
#include <limits>

using ReflectionProperty = TerraEngine::Reflection::Property;
using ReflectionType = TerraEngine::Reflection::Type;
using ReflectionVariant = TerraEngine::Reflection::Variant;

namespace
{
template <typename ValueType>
bool IsMetaValueType(const ReflectionVariant& value)
{
	return TerraEngine::Reflection::IsType<ValueType>(value);
}

template <typename ValueType>
ValueType MetaValue(const ReflectionVariant& value)
{
	return TerraEngine::Reflection::GetValue<ValueType>(value);
}

template <typename ValueType>
bool IsMetaType(const ReflectionType& type)
{
	return TerraEngine::Reflection::IsType<ValueType>(type);
}

inline bool IsValid(const ReflectionVariant& value)
{
	return TerraEngine::Reflection::IsValid(value);
}

inline bool IsValid(const ReflectionProperty& value)
{
	return TerraEngine::Reflection::IsValid(value);
}

inline bool IsValid(const ReflectionType& value)
{
	return TerraEngine::Reflection::IsValid(value);
}

inline std::string MetaPropertyName(const ReflectionProperty& property)
{
	return TerraEngine::Reflection::GetPropertyName(property);
}

inline ReflectionType MetaPropertyType(const ReflectionProperty& property)
{
	return TerraEngine::Reflection::GetPropertyType(property);
}

inline ReflectionProperty FindMetaProperty(const ReflectionType& type, std::string_view name)
{
	return TerraEngine::Reflection::FindProperty(type, name);
}

// Convert internal registry keys like "LightComponent" to user-facing names.
// This keeps serialization/component lookup stable while matching editor UX labels.
std::string BuildDisplayComponentName(const std::string& componentName)
{
	if (componentName == "LightComponent")
	{
		return "Light";
	}

	if (componentName == "Script")
	{
		return "Script";
	}

	const std::string componentSuffix = "Component";
	if (componentName.size() > componentSuffix.size()
		&& componentName.compare(componentName.size() - componentSuffix.size(), componentSuffix.size(), componentSuffix) == 0)
	{
		return componentName.substr(0, componentName.size() - componentSuffix.size());
	}

	return componentName;
}

const char* VkFormatToString(uint32_t vkFormat)
{
	switch (vkFormat)
	{
	case 37: return "R8G8B8A8_UNORM";
	case 43: return "R8G8B8A8_SRGB";
	case 50: return "B8G8R8A8_UNORM";
	case 56: return "B8G8R8A8_SRGB";
	case 131: return "BC1_RGB_UNORM_BLOCK";
	case 133: return "BC1_RGB_SRGB_BLOCK";
	case 137: return "BC3_UNORM_BLOCK";
	case 139: return "BC3_SRGB_BLOCK";
	case 141: return "BC4_UNORM_BLOCK";
	case 142: return "BC4_SNORM_BLOCK";
	case 143: return "BC5_UNORM_BLOCK";
	case 144: return "BC5_SNORM_BLOCK";
	case 145: return "BC7_UNORM_BLOCK";
	case 146: return "BC7_SRGB_BLOCK";
	case 157: return "ASTC_4x4_UNORM_BLOCK";
	case 158: return "ASTC_4x4_SRGB_BLOCK";
	default: return "Other/Unknown";
	}
}

std::string BuildScriptBindingStatusMessage(const ScriptBinding& binding)
{
	if (binding.status == "Attached")
	{
		return {};
	}

	if (binding.status == "BrokenType")
	{
		return "The active Assembly-CSharp build does not currently expose this class as a public non-abstract TerraEngine.Script type. This usually means the script failed to compile, was renamed, changed namespace, or no longer inherits from TerraEngine.Script.";
	}

	if (binding.status == "LiveReady")
	{
		return {};
	}

	if (binding.status == "InstanceUnavailable")
	{
		return "TerraEngine could not find a live managed instance for this binding. The object may have been destroyed, disabled, or is still waiting for the next script update.";
	}

	if (binding.status == "ReloadPending")
	{
		return "Assembly-CSharp is recompiling or reloading, so live values are temporarily unavailable.";
	}

	if (binding.status.empty())
	{
		return {};
	}

	return "Script status: " + binding.status + ".";
}

ImVec4 BuildScriptBindingStatusColor(const ScriptBinding& binding)
{
	if (binding.status == "BrokenType")
	{
		return ImVec4(1.0f, 0.45f, 0.45f, 1.0f);
	}

	if (binding.status == "InstanceUnavailable")
	{
		return ImVec4(1.0f, 0.7f, 0.4f, 1.0f);
	}

	return ImVec4(0.7f, 0.7f, 0.7f, 1.0f);
}

std::string ReadScriptMetadataString(const AssetMetadata* assetMetadata, const char* key)
{
	if (assetMetadata == nullptr)
	{
		return {};
	}

	const auto settingIt = assetMetadata->settings.find(key);
	if (settingIt == assetMetadata->settings.end() || !settingIt->is_string())
	{
		return {};
	}

	return settingIt->get<std::string>();
}

// Inspector headers use the script class name without namespaces because that
// is the same source Unity uses for "My Custom Script (Script)" cards.
std::string ExtractScriptLeafName(const std::string& fullName)
{
	if (fullName.empty())
	{
		return {};
	}

	const std::size_t lastNamespaceSeparator = fullName.find_last_of('.');
	if (lastNamespaceSeparator == std::string::npos)
	{
		return fullName;
	}

	return fullName.substr(lastNamespaceSeparator + 1);
}

// Unity nicifies PascalCase class names into inspector titles like
// "New Mono Behaviour Script" while leaving the backing identifier untouched.
std::string NicifyScriptIdentifier(const std::string& identifier)
{
	if (identifier.empty())
	{
		return {};
	}

	std::string nicified;
	nicified.reserve(identifier.size() * 2);

	for (std::size_t index = 0; index < identifier.size(); ++index)
	{
		const unsigned char current = static_cast<unsigned char>(identifier[index]);
		if (current == '_' || current == '-')
		{
			if (!nicified.empty() && nicified.back() != ' ')
			{
				nicified.push_back(' ');
			}
			continue;
		}

		bool shouldInsertSpace = false;
		if (index > 0 && !nicified.empty() && nicified.back() != ' ')
		{
			const unsigned char previous = static_cast<unsigned char>(identifier[index - 1]);
			const unsigned char next = index + 1 < identifier.size()
				? static_cast<unsigned char>(identifier[index + 1])
				: 0;
			shouldInsertSpace =
				(std::isupper(current) != 0 && (std::islower(previous) != 0 || std::isdigit(previous) != 0))
				|| (std::isupper(current) != 0 && std::isupper(previous) != 0 && next != 0 && std::islower(next) != 0)
				|| (std::isdigit(current) != 0 && std::isalpha(previous) != 0)
				|| (std::isalpha(current) != 0 && std::isdigit(previous) != 0);
		}

		if (shouldInsertSpace)
		{
			nicified.push_back(' ');
		}

		nicified.push_back(static_cast<char>(current));
	}

	return nicified;
}

std::string BuildScriptBindingDisplayName(const ScriptBinding& binding, const AssetMetadata* assetMetadata)
{
	const std::string currentFullName = ReadScriptMetadataString(assetMetadata, "scriptFullName");
	const std::string scriptLeafName = ExtractScriptLeafName(currentFullName.empty() ? binding.className : currentFullName);
	if (scriptLeafName.empty())
	{
		return "Missing Script";
	}

	return NicifyScriptIdentifier(scriptLeafName);
}

std::string BuildScriptAssetDisplayName(const ScriptBinding& binding, const AssetMetadata* assetMetadata)
{
	if (assetMetadata != nullptr && !assetMetadata->sourcePath.empty())
	{
		return assetMetadata->sourcePath.stem().string();
	}

	const std::string currentFullName = ReadScriptMetadataString(assetMetadata, "scriptFullName");
	const std::string scriptLeafName = ExtractScriptLeafName(currentFullName.empty() ? binding.className : currentFullName);
	return scriptLeafName.empty() ? "MissingScript" : scriptLeafName;
}

std::string FormatScriptFieldValue(const ScriptFieldValue& fieldValue)
{
	if (fieldValue.value.is_string())
	{
		return fieldValue.value.get<std::string>();
	}

	if (fieldValue.value.is_boolean())
	{
		return fieldValue.value.get<bool>() ? "True" : "False";
	}

	if (fieldValue.value.is_number_integer())
	{
		return std::to_string(fieldValue.value.get<long long>());
	}

	if (fieldValue.value.is_number_unsigned())
	{
		return std::to_string(fieldValue.value.get<unsigned long long>());
	}

	if (fieldValue.value.is_number_float())
	{
		return std::to_string(fieldValue.value.get<double>());
	}

	if (fieldValue.value.is_null())
	{
		return "None";
	}

	return fieldValue.value.dump();
}

std::string ExtractScriptFieldRootPath(std::string_view fieldPath)
{
	const std::size_t dotIndex = fieldPath.find('.');
	const std::size_t bracketIndex = fieldPath.find('[');
	const std::size_t cutIndex = dotIndex == std::string::npos
		? bracketIndex
		: bracketIndex == std::string::npos ? dotIndex : std::min(dotIndex, bracketIndex);
	return cutIndex == std::string::npos ? std::string(fieldPath) : std::string(fieldPath.substr(0, cutIndex));
}

std::string ExtractScriptFieldRelativePath(std::string_view rootPath, std::string_view fullPath)
{
	if (fullPath.size() <= rootPath.size())
	{
		return {};
	}

	std::string relativePath = std::string(fullPath.substr(rootPath.size()));
	if (!relativePath.empty() && relativePath.front() == '.')
	{
		relativePath.erase(relativePath.begin());
	}

	return relativePath;
}

std::string ReplaceFirstCollectionPlaceholder(const std::string& fieldPath, std::size_t index)
{
	const std::size_t placeholderIndex = fieldPath.find("[]");
	if (placeholderIndex == std::string::npos)
	{
		return fieldPath;
	}

	return fieldPath.substr(0, placeholderIndex) + "[" + std::to_string(index) + "]" + fieldPath.substr(placeholderIndex + 2);
}

ScriptFieldDescriptor MaterializeCollectionDescriptor(const ScriptFieldDescriptor& templateDescriptor, std::size_t index)
{
	ScriptFieldDescriptor descriptor = templateDescriptor;
	const auto materializeChildren = [&](const auto& self, ScriptFieldDescriptor& currentDescriptor) -> void {
		currentDescriptor.fieldPath = ReplaceFirstCollectionPlaceholder(currentDescriptor.fieldPath, index);
		for (ScriptFieldDescriptor& child : currentDescriptor.children)
		{
			self(self, child);
		}
	};

	materializeChildren(materializeChildren, descriptor);
	if (descriptor.displayName == "Element")
	{
		descriptor.displayName = "Element " + std::to_string(index);
	}
	return descriptor;
}

nlohmann::json BuildDefaultScriptFieldJson(const ScriptFieldDescriptor& descriptor)
{
	// Prefer the managed field initializer captured from the C# type. When the
	// bridge cannot provide one, fall back to a type-based editor default.
	if (!descriptor.defaultValue.is_null())
	{
		return descriptor.defaultValue;
	}

	if (descriptor.containerKind == "Collection")
	{
		return nlohmann::json::array();
	}

	if (descriptor.containerKind == "NestedObject")
	{
		nlohmann::json objectValue = nlohmann::json::object();
		for (const ScriptFieldDescriptor& child : descriptor.children)
		{
			objectValue[child.serializedName] = BuildDefaultScriptFieldJson(child);
		}
		return objectValue;
	}

	if (descriptor.valueKind == "Bool")
	{
		return false;
	}

	if (descriptor.valueKind == "Int")
	{
		return 0;
	}

	if (descriptor.valueKind == "Float")
	{
		return 0.0f;
	}

	if (descriptor.valueKind == "String")
	{
		return "";
	}

	if (descriptor.valueKind == "Enum")
	{
		return descriptor.enumOptions.empty() ? "" : descriptor.enumOptions.front();
	}

	if (descriptor.valueKind == "Vector2")
	{
		return nlohmann::json::array({ 0.0f, 0.0f });
	}

	if (descriptor.valueKind == "Vector3")
	{
		return nlohmann::json::array({ 0.0f, 0.0f, 0.0f });
	}

	if (descriptor.valueKind == "Vector4")
	{
		return nlohmann::json::array({ 0.0f, 0.0f, 0.0f, 0.0f });
	}

	if (descriptor.valueKind == "Quaternion")
	{
		return nlohmann::json::array({ 0.0f, 0.0f, 0.0f, 1.0f });
	}

	if (descriptor.valueKind == "EntityReference")
	{
		return 0;
	}

	return nullptr;
}

nlohmann::json* ResolveScriptJsonPath(nlohmann::json& rootValue, std::string_view rootPath, std::string_view fullPath, bool createMissing)
{
	std::string relativePath = ExtractScriptFieldRelativePath(rootPath, fullPath);
	if (relativePath.empty())
	{
		return &rootValue;
	}

	nlohmann::json* current = &rootValue;
	for (std::size_t index = 0; index < relativePath.size();)
	{
		if (relativePath[index] == '.')
		{
			++index;
			continue;
		}

		if (relativePath[index] == '[')
		{
			const std::size_t closingBracket = relativePath.find(']', index);
			if (closingBracket == std::string::npos)
			{
				return nullptr;
			}

			const std::size_t arrayIndex = static_cast<std::size_t>(std::stoul(relativePath.substr(index + 1, closingBracket - index - 1)));
			if (!current->is_array())
			{
				if (!createMissing)
				{
					return nullptr;
				}

				*current = nlohmann::json::array();
			}

			while (current->size() <= arrayIndex)
			{
				current->push_back(nullptr);
			}

			current = &(*current)[arrayIndex];
			index = closingBracket + 1;
			continue;
		}

		const std::size_t tokenEnd = relativePath.find_first_of(".[", index);
		const std::string token = relativePath.substr(index, tokenEnd == std::string::npos ? std::string::npos : tokenEnd - index);
		if (!current->is_object())
		{
			if (!createMissing)
			{
				return nullptr;
			}

			*current = nlohmann::json::object();
		}

		current = &(*current)[token];
		if (tokenEnd == std::string::npos)
		{
			break;
		}

		index = tokenEnd;
	}

	return current;
}

const nlohmann::json* ResolveScriptJsonPath(const nlohmann::json& rootValue, std::string_view rootPath, std::string_view fullPath)
{
	std::string relativePath = ExtractScriptFieldRelativePath(rootPath, fullPath);
	if (relativePath.empty())
	{
		return &rootValue;
	}

	const nlohmann::json* current = &rootValue;
	for (std::size_t index = 0; index < relativePath.size();)
	{
		if (relativePath[index] == '.')
		{
			++index;
			continue;
		}

		if (relativePath[index] == '[')
		{
			const std::size_t closingBracket = relativePath.find(']', index);
			if (closingBracket == std::string::npos || !current->is_array())
			{
				return nullptr;
			}

			const std::size_t arrayIndex = static_cast<std::size_t>(std::stoul(relativePath.substr(index + 1, closingBracket - index - 1)));
			if (arrayIndex >= current->size())
			{
				return nullptr;
			}

			current = &(*current)[arrayIndex];
			index = closingBracket + 1;
			continue;
		}

		const std::size_t tokenEnd = relativePath.find_first_of(".[", index);
		const std::string token = relativePath.substr(index, tokenEnd == std::string::npos ? std::string::npos : tokenEnd - index);
		if (!current->is_object())
		{
			return nullptr;
		}

		auto tokenIt = current->find(token);
		if (tokenIt == current->end())
		{
			return nullptr;
		}

		current = &(*tokenIt);
		if (tokenEnd == std::string::npos)
		{
			break;
		}

		index = tokenEnd;
	}

	return current;
}
}

// Command that records a property change on a component. Captures the 
// previous and next values to allow bidirectional state switching.
class ModifyComponentCommand : public ICommand {
public:
    ModifyComponentCommand(entt::entity entity, const std::string& component, const std::string& property, ReflectionVariant oldVal, ReflectionVariant newVal)
        : m_Uuid(ObjectManager::instance().getUUID(entity)), m_Component(component), m_Property(property), m_OldValue(oldVal), m_NewValue(newVal) {}

    void Undo() override {
        ApplyValue(m_OldValue);
    }

    void Redo() override {
        ApplyValue(m_NewValue);
    }

    std::string GetName() const override {
        return "Modify " + m_Component + "." + m_Property;
    }

private:
    void ApplyValue(const ReflectionVariant& val) {
        if (auto obj = ObjectManager::instance().getObject(m_Uuid)) {
            auto& reg = ComponentAccessRegistry();
            if (reg.count(m_Component)) {
                // Setters handle thread-safety internally via component mutexes.
                reg[m_Component].setter(*obj, m_Property, val);
            }
        }
    }

    TerraEngine::UUID m_Uuid;
    std::string m_Component;
    std::string m_Property;
    ReflectionVariant m_OldValue;
    ReflectionVariant m_NewValue;
};

enum class ObjectMetadataField {
    Tag,
    Layer,
    Active,
    Static
};

class ModifyObjectMetadataCommand : public ICommand {
public:
    ModifyObjectMetadataCommand(entt::entity entity, ObjectMetadataField field, ReflectionVariant oldVal, ReflectionVariant newVal)
        : m_Uuid(ObjectManager::instance().getUUID(entity)), m_Field(field), m_OldValue(oldVal), m_NewValue(newVal) {}

    void Undo() override { ApplyValue(m_OldValue); }
    void Redo() override { ApplyValue(m_NewValue); }

    std::string GetName() const override {
        switch (m_Field) {
            case ObjectMetadataField::Tag: return "Modify Object Tag";
            case ObjectMetadataField::Layer: return "Modify Object Layer";
            case ObjectMetadataField::Active: return "Modify Object Active State";
            case ObjectMetadataField::Static: return "Modify Object Static State";
        }
        return "Modify Object Metadata";
    }

private:
    void ApplyValue(const ReflectionVariant& val) {
        if (auto obj = ObjectManager::instance().getObject(m_Uuid)) {
            switch (m_Field) {
                case ObjectMetadataField::Tag: obj->setTag(MetaValue<std::string>(val)); break;
                case ObjectMetadataField::Layer: obj->setLayer(MetaValue<std::string>(val)); break;
                case ObjectMetadataField::Active: obj->setActive(MetaValue<bool>(val)); break;
                case ObjectMetadataField::Static: obj->setStatic(MetaValue<bool>(val)); break;
            }
        }
    }

    TerraEngine::UUID m_Uuid;
    ObjectMetadataField m_Field;
    ReflectionVariant m_OldValue;
    ReflectionVariant m_NewValue;
};

InspectorWindow::InspectorWindow(MainEditorSubsystem* owner)
    : EditorWindow("Inspector"), m_Owner(owner) {
}

void InspectorWindow::UpdateInspectorDragState() {
	if (ImGui::IsItemActive() && ImGui::IsMouseDragging(ImGuiMouseButton_Left)) {
		m_InspectorValueDragging = true;
	}
}

void InspectorWindow::DrawProperty(const std::string& compName, ReflectionProperty prop, Object& obj, ComponentAccessFunctions& access) {
	std::string name = MetaPropertyName(prop);
	ReflectionVariant var = access.getter(obj, name);
	ReflectionType type = MetaPropertyType(prop);

    if (!IsValid(var)) return;

    // Static buffers track interaction state across multiple frames while a 
    // widget is being manipulated.
    static ReflectionVariant oldValue;
    static bool isEditing = false;
    static std::string editingProp;
    static entt::entity editingEntity = entt::null;

    // Snapshot the initial value when the user first interacts with a control.
    auto startEditing = [&]() {
        if (ImGui::IsItemActivated()) {
            oldValue = access.getter(obj, name);
            isEditing = true;
            editingProp = name;
            editingEntity = obj.EntityId();
        }
    };

    // Push the command to the UndoRedoSystem once the interaction is finalized
    // (e.g. mouse button released after a drag).
    auto endEditing = [&]() {
        if (ImGui::IsItemDeactivatedAfterEdit()) {
            ReflectionVariant newVal = access.getter(obj, name);
            UndoRedoSystem::Instance().PushCommand(std::make_unique<ModifyComponentCommand>(obj.EntityId(), compName, name, oldValue, newVal));
            isEditing = false;
        }
    };

    ImGui::TableNextRow();
    ImGui::TableSetColumnIndex(0);
    ImGui::AlignTextToFramePadding();
    ImGui::TextUnformatted(name.c_str());
    ImGui::TableSetColumnIndex(1);

	if (IsMetaType<float>(type)) {
		float val = MetaValue<float>(var);
        ImGui::SetNextItemWidth(-1.0f);
		if (ImGui::DragFloat(("##" + name).c_str(), &val, 0.1f)) {
            if (compName == "CameraComponent") {
                if (name == "nearPlane") {
                    if (val < 0.001f) val = 0.001f;
                    
                    ReflectionVariant farVar = access.getter(obj, "farPlane");
                    if (IsValid(farVar)) {
                        float farP = MetaValue<float>(farVar);
                        if (farP < val + 0.001f) {
                            access.setter(obj, "farPlane", val + 0.001f);
                        }
                    }
                }
                else if (name == "farPlane") {
                    ReflectionVariant nearVar = access.getter(obj, "nearPlane");
                    if (IsValid(nearVar)) {
                        float nearP = MetaValue<float>(nearVar);
                        if (val < nearP + 0.001f) val = nearP + 0.001f;
                    }
                }
            }
			access.setter(obj, name, val);
		}
        startEditing();
        endEditing();
		UpdateInspectorDragState();
	}
	else if (IsMetaType<int>(type)) {
		int val = MetaValue<int>(var);
        ImGui::SetNextItemWidth(-1.0f);
		if (ImGui::DragInt(("##" + name).c_str(), &val)) {
			access.setter(obj, name, val);
		}
        startEditing();
        endEditing();
		UpdateInspectorDragState();
	}
	else if (IsMetaType<std::uint32_t>(type)) {
		std::uint32_t val = MetaValue<std::uint32_t>(var);
		int signedValue = static_cast<int>(val);
        ImGui::SetNextItemWidth(-1.0f);
		if (ImGui::DragInt(("##" + name).c_str(), &signedValue, 1.0f, 0, std::numeric_limits<int>::max())) {
			const std::uint32_t clampedValue = static_cast<std::uint32_t>(std::max(0, signedValue));
			access.setter(obj, name, clampedValue);
		}
		startEditing();
		endEditing();
		UpdateInspectorDragState();
	}
	else if (IsMetaType<bool>(type)) {
		bool val = MetaValue<bool>(var);
		if (ImGui::Checkbox(("##" + name).c_str(), &val)) {
            // Checkboxes are instantaneous interactions, so we push the 
            // command immediately.
            ReflectionVariant oldB = access.getter(obj, name);
			access.setter(obj, name, val);
            ReflectionVariant newB = access.getter(obj, name);
            UndoRedoSystem::Instance().PushCommand(std::make_unique<ModifyComponentCommand>(obj.EntityId(), compName, name, oldB, newB));
		}
	}
	else if (IsMetaType<std::string>(type)) {
		std::string val = MetaValue<std::string>(var);
		char buffer[256];
		strncpy(buffer, val.c_str(), sizeof(buffer));
        ImGui::SetNextItemWidth(-1.0f);
		if (ImGui::InputText(("##" + name).c_str(), buffer, sizeof(buffer))) {
			access.setter(obj, name, std::string(buffer));
		}
        startEditing();
        endEditing();

		if (ImGui::BeginDragDropTarget()) {
			const char* expected = nullptr;
			if (name == "sourceMarkup") expected = "ASSET_TUIML";

			if (expected) {
				if (const ImGuiPayload* payload = ImGui::AcceptDragDropPayload(expected)) {
					IM_ASSERT(payload->DataSize == sizeof(TerraEngine::UUID));
					TerraEngine::UUID newId = *(const TerraEngine::UUID*)payload->Data;
					if (const AssetMetadata* meta = AssetDatabase::Instance().FindAssetByGuid(newId)) {
						std::string path = meta->sourcePath.generic_string();
						ReflectionVariant oldV = access.getter(obj, name);
						access.setter(obj, name, path);
						ReflectionVariant newV = access.getter(obj, name);
						UndoRedoSystem::Instance().PushCommand(std::make_unique<ModifyComponentCommand>(obj.EntityId(), compName, name, oldV, newV));
					}
				}
			}
			ImGui::EndDragDropTarget();
		}
	}
	else if (IsMetaType<glm::vec3>(type)) {
		glm::vec3 val = MetaValue<glm::vec3>(var);
        ImGui::SetNextItemWidth(-1.0f);
		if (ImGui::DragFloat3(("##" + name).c_str(), &val.x, 0.1f)) {
			access.setter(obj, name, val);
		}
        startEditing();
        endEditing();
		UpdateInspectorDragState();
	}
	else if (IsMetaType<glm::quat>(type)) {
		glm::quat q = MetaValue<glm::quat>(var);
		glm::vec3 euler = glm::degrees(glm::eulerAngles(q));
        ImGui::SetNextItemWidth(-1.0f);
		if (ImGui::DragFloat3(("##" + name).c_str(), &euler.x, 0.1f)) {
			glm::quat newQ = glm::quat(glm::radians(euler));
			access.setter(obj, name, newQ);
		}
        startEditing();
        endEditing();
		UpdateInspectorDragState();
	}
	else if (IsMetaType<TerraEngine::UUID>(type)) {
		TerraEngine::UUID val = MetaValue<TerraEngine::UUID>(var);
		std::string label = "None";
		bool found = false;
		if (val != TerraEngine::EMPTY_UUID) {
			const auto& assets = AssetDatabase::Instance().GetAssets();
			for (const auto& [path, meta] : assets) {
				if (meta.guid == val) {
					label = path.stem().string();
					found = true;
					break;
				}
			}
			if (!found) label = "(Missing)";
		}

		char buf[128];
		strncpy(buf, label.c_str(), sizeof(buf));
		
		ImGui::SetNextItemWidth(-1.0f);
		if (val != TerraEngine::EMPTY_UUID && !found) ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1, 0, 0, 1));
		ImGui::InputText(("##" + name).c_str(), buf, sizeof(buf), ImGuiInputTextFlags_ReadOnly);
		if (val != TerraEngine::EMPTY_UUID && !found) ImGui::PopStyleColor();

		if (ImGui::BeginPopupContextItem()) {
			if (ImGui::MenuItem("Clear")) {
				ReflectionVariant oldId = access.getter(obj, name);
				access.setter(obj, name, TerraEngine::EMPTY_UUID);
				ReflectionVariant nextId = access.getter(obj, name);
				UndoRedoSystem::Instance().PushCommand(std::make_unique<ModifyComponentCommand>(obj.EntityId(), compName, name, oldId, nextId));
			}
			ImGui::EndPopup();
		}

		if (ImGui::BeginDragDropTarget()) {
			const char* payloads[] = { "ASSET_MESH", "ASSET_MATERIAL", "ASSET_TEXTURE", "ASSET_SHADER", "ASSET_GUID" };
			const char* expected = nullptr;

			if (name == "meshId") expected = "ASSET_MESH";
			else if (name == "materialId") expected = "ASSET_MATERIAL";
			else if (name.find("Texture") != std::string::npos || name.find("texture") != std::string::npos) expected = "ASSET_TEXTURE";

			if (expected) {
				if (const ImGuiPayload* payload = ImGui::AcceptDragDropPayload(expected)) {
					IM_ASSERT(payload->DataSize == sizeof(TerraEngine::UUID));
					TerraEngine::UUID newId = *(const TerraEngine::UUID*)payload->Data;
                    ReflectionVariant oldId = access.getter(obj, name);
					access.setter(obj, name, newId);
                    ReflectionVariant nextId = access.getter(obj, name);
                    UndoRedoSystem::Instance().PushCommand(std::make_unique<ModifyComponentCommand>(obj.EntityId(), compName, name, oldId, nextId));
				}
			} else {
				// Generic UUID property, accept any asset payload
				for (const char* p : payloads) {
					if (const ImGuiPayload* payload = ImGui::AcceptDragDropPayload(p)) {
						IM_ASSERT(payload->DataSize == sizeof(TerraEngine::UUID));
						TerraEngine::UUID newId = *(const TerraEngine::UUID*)payload->Data;
                        ReflectionVariant oldId = access.getter(obj, name);
						access.setter(obj, name, newId);
                        ReflectionVariant nextId = access.getter(obj, name);
                        UndoRedoSystem::Instance().PushCommand(std::make_unique<ModifyComponentCommand>(obj.EntityId(), compName, name, oldId, nextId));
						break;
					}
				}
			}
			ImGui::EndDragDropTarget();
		}
	}
}

void InspectorWindow::DrawScriptComponentInspector(Object& obj) {
	ScriptComponent* scriptComponent = obj.getComponent<ScriptComponent>();
	if (scriptComponent == nullptr) {
		return;
	}

	auto& assetDatabase = AssetDatabase::Instance();
	EditorContext* context = m_Owner != nullptr ? m_Owner->GetContext() : nullptr;
	const bool playMode = ScriptingSystem::Instance().IsRunning();
	const bool reloadPending = context != nullptr
		&& (context->scripting.coordinatorState == "Queued"
			|| context->scripting.coordinatorState == "Building"
			|| context->scripting.coordinatorState == "Reloading");
	auto guard = scriptComponent->acquireWriteLock();
	std::vector<ScriptBinding>& bindings = scriptComponent->GetBindings();

	if (bindings.empty()) {
		return;
	}

	std::map<std::string, int> shortNameCounts;
	for (const ScriptBinding& binding : bindings)
	{
		shortNameCounts[ExtractScriptLeafName(binding.className)]++;
	}

	// Mirror Unity's inspector model: each binding gets its own component
	// card even though Terra stores all bindings inside one ScriptComponent.
	for (std::size_t bindingIndex = 0; bindingIndex < bindings.size(); ++bindingIndex) {
		ScriptBinding& binding = bindings[bindingIndex];
		const AssetMetadata* assetMetadata = assetDatabase.FindAssetByGuid(binding.assetGuid);
		if (assetMetadata == nullptr && !binding.className.empty()) {
			assetMetadata = assetDatabase.FindScriptAssetByFullName(binding.className);
		}
		std::string displayName = BuildScriptBindingDisplayName(binding, assetMetadata);
		const bool needsDisambiguation = shortNameCounts[ExtractScriptLeafName(binding.className)] > 1;
		if (needsDisambiguation)
		{
			const std::string scriptNamespace = ReadScriptMetadataString(assetMetadata, "scriptNamespace");
			if (!scriptNamespace.empty())
			{
				displayName += " [" + scriptNamespace + "]";
			}
			else if (assetMetadata != nullptr)
			{
				displayName += " [" + assetMetadata->sourcePath.parent_path().string() + "]";
			}
			else if (!binding.className.empty())
			{
				displayName += " [" + binding.className + "]";
			}
		}
		const std::string scriptAssetName = BuildScriptAssetDisplayName(binding, assetMetadata);
		const std::string componentTitle = displayName + " (Script)";
		std::string statusMessage = reloadPending ? (context != nullptr ? context->scripting.lastSummary : std::string{}) : BuildScriptBindingStatusMessage(binding);

		ImGui::PushID(binding.bindingId.c_str());
		bool bindingEnabled = binding.enabled;
		bool isOpen = ImGui::GetStateStorage()->GetInt(ImGui::GetID("IsOpen"), 1) != 0;
		ImVec2 headerPosition = ImGui::GetCursorScreenPos();
		float headerWidth = ImGui::GetContentRegionAvail().x;
		float headerHeight = ImGui::GetFrameHeight();

		ImGui::GetWindowDrawList()->AddRectFilled(
			headerPosition,
			ImVec2(headerPosition.x + headerWidth, headerPosition.y + headerHeight),
			ImGui::GetColorU32(ImGuiCol_Header));

		ImGui::SetNextItemAllowOverlap();
		if (ImGui::InvisibleButton("##header_bg", ImVec2(headerWidth, headerHeight))) {
			isOpen = !isOpen;
			ImGui::GetStateStorage()->SetInt(ImGui::GetID("IsOpen"), isOpen);
		}

		bool removeBinding = false;
		if (ImGui::BeginPopupContextItem("ComponentSettings")) {
			if (ImGui::MenuItem("Remove Component")) {
				removeBinding = true;
			}
			ImGui::EndPopup();
		}

		ImGui::SetCursorScreenPos(headerPosition);
		ImGui::Dummy(ImVec2(2.0f, 0.0f));
		ImGui::SameLine();
		if (ImGui::ArrowButton("##arrow", isOpen ? ImGuiDir_Down : ImGuiDir_Right)) {
			isOpen = !isOpen;
			ImGui::GetStateStorage()->SetInt(ImGui::GetID("IsOpen"), isOpen);
		}

		ImGui::SameLine();
		ImGui::PushStyleColor(ImGuiCol_FrameBg, ImVec4(0.2f, 0.2f, 0.2f, 1.0f));
		if (ImGui::Checkbox("##compActive", &bindingEnabled)) {
			const std::string oldJson = scriptComponent->GetBindingsJson();
			binding.enabled = bindingEnabled;
			scriptComponent->SynchronizeStorageFromBindings();
			const std::string newJson = scriptComponent->GetBindingsJson();
			UndoRedoSystem::Instance().PushCommand(std::make_unique<ModifyComponentCommand>(obj.EntityId(), "Script", "BindingsJson", oldJson, newJson));
		}
		ImGui::PopStyleColor();

		ImGui::SameLine();
		ImGui::AlignTextToFramePadding();
		ImGui::TextUnformatted(componentTitle.c_str());

		ImGui::SetCursorScreenPos(ImVec2(headerPosition.x, headerPosition.y + headerHeight));
		ImGui::Dummy(ImVec2(0.0f, 0.0f));

		if (removeBinding) {
			const std::string oldJson = scriptComponent->GetBindingsJson();
			scriptComponent->RemoveBinding(binding.bindingId);
			const std::string newJson = scriptComponent->GetBindingsJson();
			UndoRedoSystem::Instance().PushCommand(std::make_unique<ModifyComponentCommand>(obj.EntityId(), "Script", "BindingsJson", oldJson, newJson));
			ImGui::Separator();
			ImGui::PopID();
			break;
		}

		if (isOpen) {
			ImGui::Indent();
			char scriptBuffer[256];
			strncpy(scriptBuffer, scriptAssetName.c_str(), sizeof(scriptBuffer));
			scriptBuffer[sizeof(scriptBuffer) - 1] = '\0';
			ImGui::BeginDisabled();
			ImGui::SetNextItemWidth(-1.0f);
			ImGui::InputText("##ScriptAsset", scriptBuffer, sizeof(scriptBuffer), ImGuiInputTextFlags_ReadOnly);
			const bool hoveredScriptField = ImGui::IsItemHovered();
			ImGui::EndDisabled();
			if (assetMetadata != nullptr && hoveredScriptField) {
				ImGui::SetTooltip("%s", assetMetadata->sourcePath.string().c_str());
			}

			auto getCurrentRootValue = [&](const ScriptFieldDescriptor& rootDescriptor) -> nlohmann::json {
				if (playMode) {
					if (const ScriptFieldSnapshot* liveSnapshot = binding.FindLiveSnapshot(rootDescriptor.fieldPath)) {
						return liveSnapshot->value;
					}

					if (const ScriptFieldValue* runtimeOverride = binding.FindRuntimeOverride(rootDescriptor.fieldPath)) {
						return runtimeOverride->value;
					}
				}

				if (const ScriptFieldValue* authoredValue = binding.FindSerializedValue(rootDescriptor.fieldPath)) {
					return authoredValue->value;
				}

				return BuildDefaultScriptFieldJson(rootDescriptor);
			};

			auto commitRootValue = [&](const ScriptFieldDescriptor& rootDescriptor, const nlohmann::json& nextRootValue) -> bool {
				if (playMode) {
					std::string liveWriteError;
					if (!ScriptingSystem::Instance().WriteLiveBindingValue(static_cast<uint32_t>(obj.EntityId()), binding, rootDescriptor.fieldPath, nextRootValue, &liveWriteError)) {
						m_LastScriptMessage = liveWriteError.empty() ? "TerraEngine could not apply the live script edit." : liveWriteError;
						m_LastScriptMessageIsError = true;
						return false;
					}

					m_LastScriptMessage.clear();
					m_LastScriptMessageIsError = false;
					return true;
				}

				const std::string oldJson = scriptComponent->GetBindingsJson();
				ScriptFieldValue& storedValue = binding.UpsertSerializedValue(rootDescriptor.fieldPath, rootDescriptor.valueKind);
				storedValue.fieldPath = rootDescriptor.fieldPath;
				storedValue.fieldName = rootDescriptor.fieldPath;
				storedValue.displayName = rootDescriptor.displayName;
				storedValue.valueKind = rootDescriptor.valueKind;
				storedValue.value = nextRootValue;
				storedValue.compatibility = "Current";
				storedValue.persistenceState = "Current";
				storedValue.sourceTypeFingerprint = rootDescriptor.typeFingerprint;
				storedValue.diagnosticMessage.clear();
				scriptComponent->SynchronizeStorageFromBindings();
				const std::string newJson = scriptComponent->GetBindingsJson();
				if (oldJson != newJson) {
					UndoRedoSystem::Instance().PushCommand(std::make_unique<ModifyComponentCommand>(obj.EntityId(), "Script", "BindingsJson", oldJson, newJson));
				}

				return true;
			};

			auto removeStoredField = [&](std::string_view fieldPath) {
				const std::string oldJson = scriptComponent->GetBindingsJson();
				if (binding.RemoveSerializedValue(fieldPath)) {
					scriptComponent->SynchronizeStorageFromBindings();
					const std::string newJson = scriptComponent->GetBindingsJson();
					UndoRedoSystem::Instance().PushCommand(std::make_unique<ModifyComponentCommand>(obj.EntityId(), "Script", "BindingsJson", oldJson, newJson));
				}
			};

			auto renderField = [&](const auto& self, const ScriptFieldDescriptor& rootDescriptor, const ScriptFieldDescriptor& descriptor) -> void {
				if (descriptor.hiddenInInspector) {
					return;
				}

				nlohmann::json currentRootValue = getCurrentRootValue(rootDescriptor);
				const nlohmann::json* resolvedValue = ResolveScriptJsonPath(currentRootValue, rootDescriptor.fieldPath, descriptor.fieldPath);
				const nlohmann::json currentValue = resolvedValue != nullptr ? *resolvedValue : BuildDefaultScriptFieldJson(descriptor);
				const bool editable = descriptor.editable && descriptor.valueKind != "Unsupported" && !reloadPending && binding.status != "BrokenType";

				ImGui::PushID(descriptor.fieldPath.c_str());
				if (descriptor.containerKind == "NestedObject") {
					if (ImGui::TreeNodeEx(descriptor.displayName.c_str(), ImGuiTreeNodeFlags_DefaultOpen)) {
						for (const ScriptFieldDescriptor& child : descriptor.children) {
							self(self, rootDescriptor, child);
						}
						ImGui::TreePop();
					}
					if (!descriptor.tooltip.empty() && ImGui::IsItemHovered()) {
						ImGui::SetTooltip("%s", descriptor.tooltip.c_str());
					}
					ImGui::PopID();
					return;
				}

				if (descriptor.containerKind == "Collection") {
					nlohmann::json collectionValue = currentValue.is_array() ? currentValue : nlohmann::json::array();
					const std::string treeLabel = descriptor.displayName + " [" + std::to_string(collectionValue.size()) + "]";
					const bool open = ImGui::TreeNodeEx(treeLabel.c_str(), ImGuiTreeNodeFlags_DefaultOpen);
					if (!descriptor.tooltip.empty() && ImGui::IsItemHovered()) {
						ImGui::SetTooltip("%s", descriptor.tooltip.c_str());
					}
					if (editable) {
						ImGui::SameLine();
						if (ImGui::SmallButton("+")) {
							nlohmann::json nextRootValue = currentRootValue;
							nlohmann::json* collectionTarget = ResolveScriptJsonPath(nextRootValue, rootDescriptor.fieldPath, descriptor.fieldPath, true);
							if (collectionTarget != nullptr) {
								if (!collectionTarget->is_array()) {
									*collectionTarget = nlohmann::json::array();
								}

								collectionTarget->push_back(descriptor.children.empty() ? nullptr : BuildDefaultScriptFieldJson(descriptor.children.front()));
								commitRootValue(rootDescriptor, nextRootValue);
							}
						}
					}

					if (open) {
						for (std::size_t elementIndex = 0; elementIndex < collectionValue.size(); ++elementIndex) {
							if (!descriptor.children.empty()) {
								ScriptFieldDescriptor elementDescriptor = MaterializeCollectionDescriptor(descriptor.children.front(), elementIndex);
								ImGui::PushID(static_cast<int>(elementIndex));
								if (editable) {
									if (ImGui::SmallButton("x")) {
										nlohmann::json nextRootValue = currentRootValue;
										nlohmann::json* collectionTarget = ResolveScriptJsonPath(nextRootValue, rootDescriptor.fieldPath, descriptor.fieldPath, true);
										if (collectionTarget != nullptr && collectionTarget->is_array() && elementIndex < collectionTarget->size()) {
											collectionTarget->erase(collectionTarget->begin() + static_cast<nlohmann::json::difference_type>(elementIndex));
											commitRootValue(rootDescriptor, nextRootValue);
										}
									}
									ImGui::SameLine();
									if (ImGui::SmallButton("^") && elementIndex > 0) {
										nlohmann::json nextRootValue = currentRootValue;
										nlohmann::json* collectionTarget = ResolveScriptJsonPath(nextRootValue, rootDescriptor.fieldPath, descriptor.fieldPath, true);
										if (collectionTarget != nullptr && collectionTarget->is_array()) {
											std::swap((*collectionTarget)[elementIndex], (*collectionTarget)[elementIndex - 1]);
											commitRootValue(rootDescriptor, nextRootValue);
										}
									}
									ImGui::SameLine();
									if (ImGui::SmallButton("v") && elementIndex + 1 < collectionValue.size()) {
										nlohmann::json nextRootValue = currentRootValue;
										nlohmann::json* collectionTarget = ResolveScriptJsonPath(nextRootValue, rootDescriptor.fieldPath, descriptor.fieldPath, true);
										if (collectionTarget != nullptr && collectionTarget->is_array()) {
											std::swap((*collectionTarget)[elementIndex], (*collectionTarget)[elementIndex + 1]);
											commitRootValue(rootDescriptor, nextRootValue);
										}
									}
									ImGui::SameLine();
								}

								self(self, rootDescriptor, elementDescriptor);
								ImGui::PopID();
							}
						}

						ImGui::TreePop();
					}

					ImGui::PopID();
					return;
				}

				if (!descriptor.readOnlyReason.empty() && descriptor.valueKind == "Unsupported") {
					ImGui::TextDisabled("%s", descriptor.displayName.c_str());
					ImGui::TextWrapped("%s", descriptor.readOnlyReason.c_str());
					ImGui::PopID();
					return;
				}

				if (descriptor.valueKind == "Bool") {
					bool editedValue = currentValue.is_boolean() ? currentValue.get<bool>() : false;
					if (ImGui::Checkbox(descriptor.displayName.c_str(), &editedValue) && editable) {
						nlohmann::json nextRootValue = currentRootValue;
						if (nlohmann::json* targetValue = ResolveScriptJsonPath(nextRootValue, rootDescriptor.fieldPath, descriptor.fieldPath, true)) {
							*targetValue = editedValue;
							commitRootValue(rootDescriptor, nextRootValue);
						}
					}
				} else if (descriptor.valueKind == "Int") {
					int editedValue = currentValue.is_number_integer() ? currentValue.get<int>() : 0;
					const bool changed = descriptor.hasRange
						? ImGui::SliderInt(descriptor.displayName.c_str(), &editedValue, static_cast<int>(descriptor.rangeMin), static_cast<int>(descriptor.rangeMax))
						: ImGui::DragInt(descriptor.displayName.c_str(), &editedValue, 1.0f);
					if (changed && editable) {
						nlohmann::json nextRootValue = currentRootValue;
						if (nlohmann::json* targetValue = ResolveScriptJsonPath(nextRootValue, rootDescriptor.fieldPath, descriptor.fieldPath, true)) {
							*targetValue = editedValue;
							commitRootValue(rootDescriptor, nextRootValue);
						}
					}
					UpdateInspectorDragState();
				} else if (descriptor.valueKind == "Float") {
					float editedValue = currentValue.is_number() ? currentValue.get<float>() : 0.0f;
					const bool changed = descriptor.hasRange
						? ImGui::SliderFloat(descriptor.displayName.c_str(), &editedValue, static_cast<float>(descriptor.rangeMin), static_cast<float>(descriptor.rangeMax))
						: ImGui::DragFloat(descriptor.displayName.c_str(), &editedValue, 0.1f);
					if (changed && editable) {
						nlohmann::json nextRootValue = currentRootValue;
						if (nlohmann::json* targetValue = ResolveScriptJsonPath(nextRootValue, rootDescriptor.fieldPath, descriptor.fieldPath, true)) {
							*targetValue = editedValue;
							commitRootValue(rootDescriptor, nextRootValue);
						}
					}
					UpdateInspectorDragState();
				} else if (descriptor.valueKind == "String") {
					std::string editedValue = currentValue.is_string() ? currentValue.get<std::string>() : std::string{};
					char textBuffer[256];
					strncpy(textBuffer, editedValue.c_str(), sizeof(textBuffer));
					textBuffer[sizeof(textBuffer) - 1] = '\0';
					if (ImGui::InputText(descriptor.displayName.c_str(), textBuffer, sizeof(textBuffer)) && editable) {
						nlohmann::json nextRootValue = currentRootValue;
						if (nlohmann::json* targetValue = ResolveScriptJsonPath(nextRootValue, rootDescriptor.fieldPath, descriptor.fieldPath, true)) {
							*targetValue = std::string(textBuffer);
							commitRootValue(rootDescriptor, nextRootValue);
						}
					}
				} else if (descriptor.valueKind == "Enum") {
					int selectedIndex = 0;
					if (currentValue.is_string()) {
						auto optionIt = std::find(descriptor.enumOptions.begin(), descriptor.enumOptions.end(), currentValue.get<std::string>());
						if (optionIt != descriptor.enumOptions.end()) {
							selectedIndex = static_cast<int>(std::distance(descriptor.enumOptions.begin(), optionIt));
						}
					}

					std::vector<const char*> options;
					options.reserve(descriptor.enumOptions.size());
					for (const std::string& option : descriptor.enumOptions) {
						options.push_back(option.c_str());
					}
					if (!options.empty() && ImGui::Combo(descriptor.displayName.c_str(), &selectedIndex, options.data(), static_cast<int>(options.size())) && editable) {
						nlohmann::json nextRootValue = currentRootValue;
						if (nlohmann::json* targetValue = ResolveScriptJsonPath(nextRootValue, rootDescriptor.fieldPath, descriptor.fieldPath, true)) {
							*targetValue = descriptor.enumOptions[static_cast<std::size_t>(selectedIndex)];
							commitRootValue(rootDescriptor, nextRootValue);
						}
					}
				} else if (descriptor.valueKind == "Vector2" || descriptor.valueKind == "Vector3" || descriptor.valueKind == "Vector4" || descriptor.valueKind == "Quaternion") {
					const int componentCount = descriptor.valueKind == "Vector2" ? 2 : descriptor.valueKind == "Vector3" ? 3 : 4;
					float components[4] = { 0.0f, 0.0f, 0.0f, descriptor.valueKind == "Quaternion" ? 1.0f : 0.0f };
					if (currentValue.is_array()) {
						for (int componentIndex = 0; componentIndex < componentCount && componentIndex < static_cast<int>(currentValue.size()); ++componentIndex) {
							components[componentIndex] = currentValue[static_cast<std::size_t>(componentIndex)].get<float>();
						}
					}

					const bool changed = componentCount == 2
						? ImGui::DragFloat2(descriptor.displayName.c_str(), components, 0.1f)
						: componentCount == 3
							? ImGui::DragFloat3(descriptor.displayName.c_str(), components, 0.1f)
							: ImGui::DragFloat4(descriptor.displayName.c_str(), components, 0.1f);
					if (changed && editable) {
						nlohmann::json nextRootValue = currentRootValue;
						if (nlohmann::json* targetValue = ResolveScriptJsonPath(nextRootValue, rootDescriptor.fieldPath, descriptor.fieldPath, true)) {
							nlohmann::json nextVector = nlohmann::json::array();
							for (int componentIndex = 0; componentIndex < componentCount; ++componentIndex) {
								nextVector.push_back(components[componentIndex]);
							}
							*targetValue = std::move(nextVector);
							commitRootValue(rootDescriptor, nextRootValue);
						}
					}
					UpdateInspectorDragState();
				} else if (descriptor.valueKind == "EntityReference") {
					int entityId = currentValue.is_number_integer() ? currentValue.get<int>() : 0;
					if (ImGui::DragInt(descriptor.displayName.c_str(), &entityId, 1.0f, 0) && editable) {
						nlohmann::json nextRootValue = currentRootValue;
						if (nlohmann::json* targetValue = ResolveScriptJsonPath(nextRootValue, rootDescriptor.fieldPath, descriptor.fieldPath, true)) {
							*targetValue = entityId;
							commitRootValue(rootDescriptor, nextRootValue);
						}
					}
					UpdateInspectorDragState();
				} else {
					std::string readOnlyValue = currentValue.is_discarded() ? "None" : currentValue.dump();
					ImGui::TextDisabled("%s: %s", descriptor.displayName.c_str(), readOnlyValue.c_str());
				}

				if (!descriptor.tooltip.empty() && ImGui::IsItemHovered()) {
					ImGui::SetTooltip("%s", descriptor.tooltip.c_str());
				}

				if (context != nullptr) {
					if (ImGui::IsItemActive()) {
						context->scripting.activeEditedBindingId = binding.bindingId;
						context->scripting.activeEditedFieldPath = descriptor.fieldPath;
					} else if (context->scripting.activeEditedBindingId == binding.bindingId && context->scripting.activeEditedFieldPath == descriptor.fieldPath) {
						context->scripting.activeEditedBindingId.clear();
						context->scripting.activeEditedFieldPath.clear();
					}
				}

				ImGui::PopID();
			};

			if (!binding.fieldDescriptors.empty()) {
				for (const ScriptFieldDescriptor& rootDescriptor : binding.fieldDescriptors) {
					renderField(renderField, rootDescriptor, rootDescriptor);
				}
			} else {
				for (const ScriptFieldValue& fieldValue : binding.serializedValues) {
					ImGui::TextDisabled("%s: %s", fieldValue.fieldName.c_str(), FormatScriptFieldValue(fieldValue).c_str());
				}
			}

			std::string brokenFieldToRemove;
			for (const ScriptFieldValue& fieldValue : binding.serializedValues) {
				if (fieldValue.compatibility != "Broken") {
					continue;
				}

				ImGui::PushID((fieldValue.fieldPath + ":broken").c_str());
				ImGui::TextColored(ImVec4(1.0f, 0.6f, 0.35f, 1.0f), "%s is stale after reload.", fieldValue.displayName.empty() ? fieldValue.fieldName.c_str() : fieldValue.displayName.c_str());
				if (!fieldValue.diagnosticMessage.empty()) {
					ImGui::TextWrapped("%s", fieldValue.diagnosticMessage.c_str());
				}
				if (ImGui::SmallButton("Clear Value")) {
					brokenFieldToRemove = fieldValue.fieldPath.empty() ? fieldValue.fieldName : fieldValue.fieldPath;
				}
				ImGui::SameLine();
				if (ImGui::SmallButton("Remove Stale Data")) {
					brokenFieldToRemove = fieldValue.fieldPath.empty() ? fieldValue.fieldName : fieldValue.fieldPath;
				}
				ImGui::PopID();

				if (!brokenFieldToRemove.empty()) {
					break;
				}
			}

			if (!brokenFieldToRemove.empty()) {
				removeStoredField(brokenFieldToRemove);
			}

			for (const ScriptFieldDiagnostic& diagnostic : binding.diagnostics) {
				if (diagnostic.message.empty()) {
					continue;
				}

				// Unsupported fields already render their read-only explanation
				// inline, so repeating the same warning below every frame only adds
				// noisy duplicate yellow rows to the inspector.
				if (diagnostic.code == "UnsupportedType") {
					continue;
				}

				const ImVec4 diagnosticColor = diagnostic.severity == "Error"
					? ImVec4(1.0f, 0.45f, 0.45f, 1.0f)
					: diagnostic.severity == "Warning"
						? ImVec4(1.0f, 0.8f, 0.35f, 1.0f)
						: ImVec4(0.7f, 0.7f, 0.7f, 1.0f);
				ImGui::TextColored(diagnosticColor, "%s", diagnostic.message.c_str());
			}

			if (!statusMessage.empty()) {
				ImGui::TextColored(reloadPending ? ImVec4(1.0f, 0.8f, 0.35f, 1.0f) : BuildScriptBindingStatusColor(binding), "%s", statusMessage.c_str());
			}
			ImGui::Unindent();
		}

		ImGui::Separator();
		ImGui::PopID();
	}
}

void InspectorWindow::DrawScriptDropTarget(Object& obj) {
	// Use the visible inspector body as the drop zone so scripts can be
	// attached by drag-and-drop without rendering a dedicated placeholder box.
	ImGuiWindow* inspectorWindow = ImGui::GetCurrentWindow();
	const ImRect dropBounds(inspectorWindow->InnerRect.Min, inspectorWindow->InnerRect.Max);
	if (!ImGui::BeginDragDropTargetCustom(dropBounds, inspectorWindow->GetID("InspectorScriptDropTarget"))) {
		return;
	}

	if (const ImGuiPayload* payload = ImGui::AcceptDragDropPayload("ASSET_SCRIPT")) {
		IM_ASSERT(payload->DataSize == sizeof(TerraEngine::UUID));
		const TerraEngine::UUID scriptGuid = *static_cast<const TerraEngine::UUID*>(payload->Data);
		const AssetMetadata* scriptMetadata = AssetDatabase::Instance().FindAssetByGuid(scriptGuid);
		if (scriptMetadata == nullptr) {
			m_LastScriptMessage = "The dropped script asset no longer exists in the asset database.";
			m_LastScriptMessageIsError = true;
		} else {
			const std::string className = AssetDatabase::Instance().GetScriptFullName(scriptGuid);
			const bool sourceLooksAttachable = scriptMetadata->settings.contains("isAttachable")
				&& scriptMetadata->settings["isAttachable"].get<bool>();
			if (className.empty()) {
				m_LastScriptMessage = "The dropped script file does not currently declare a usable gameplay class.";
				m_LastScriptMessageIsError = true;
			} else if (!sourceLooksAttachable) {
				m_LastScriptMessage = "The dropped script source does not currently inherit from TerraEngine.Script.";
				m_LastScriptMessageIsError = true;
			} else {
				ScriptComponent* scriptComponent = obj.getComponent<ScriptComponent>();
				if (scriptComponent == nullptr) {
					scriptComponent = &obj.addComponent<ScriptComponent>();
				}

				auto guard = scriptComponent->acquireWriteLock();
				const std::string oldJson = scriptComponent->GetBindingsJson();
				// Terra mirrors Unity here: one object can hold multiple script
				// bindings, including multiple instances of the exact same class.
				ScriptBinding& binding = scriptComponent->AddBinding(scriptGuid, className);
				ScriptingSystem& scriptingSystem = ScriptingSystem::Instance();
				scriptingSystem.InitializeBindingMetadata(binding);
				const bool assemblyCanAttachImmediately = binding.status == "Attached";
				scriptComponent->SynchronizeStorageFromBindings();
				const std::string newJson = scriptComponent->GetBindingsJson();
				UndoRedoSystem::Instance().PushCommand(std::make_unique<ModifyComponentCommand>(obj.EntityId(), "Script", "BindingsJson", oldJson, newJson));

				if (assemblyCanAttachImmediately) {
					m_LastScriptMessage = "Attached " + className + " to the selected object.";
				} else {
					// Persist the binding immediately, but do not force a rebuild just
					// because the user attached an existing script asset in the editor.
					// If the gameplay assembly is stale or missing, the binding will
					// activate on the next normal successful compile or reload.
					m_LastScriptMessage = "Added " + className + " to the selected object. The binding will activate when the gameplay assembly is next ready.";
				}
				m_LastScriptMessageIsError = false;
			}
		}
	}

	ImGui::EndDragDropTarget();

	if (!m_LastScriptMessage.empty()) {
		const ImVec4 messageColor = m_LastScriptMessageIsError ? ImVec4(1.0f, 0.4f, 0.4f, 1.0f) : ImVec4(0.5f, 0.85f, 0.5f, 1.0f);
		ImGui::TextColored(messageColor, "%s", m_LastScriptMessage.c_str());
	}
}

void InspectorWindow::DrawLightComponentInspector(const std::string& compName, Object& obj, ComponentAccessFunctions& access)
{
    auto drawLabeledWidget = [&](const char* label, auto widgetFunc) {
        ImGui::TableNextRow();
        ImGui::TableSetColumnIndex(0);
        ImGui::AlignTextToFramePadding();
        ImGui::TextUnformatted(label);
        ImGui::TableSetColumnIndex(1);
        ImGui::SetNextItemWidth(-1.0f);
        widgetFunc();
    };

	// Keep undo/redo granular by committing only fields that changed through
	// this custom inspector layout.
	auto setFloatPropertyWithUndo = [&](const char* propertyName, float newValue) {
		ReflectionVariant oldVariant = access.getter(obj, propertyName);
		if (!IsValid(oldVariant) || !IsMetaValueType<float>(oldVariant))
		{
			return;
		}

		const float oldValue = MetaValue<float>(oldVariant);
		if (std::abs(oldValue - newValue) <= 0.0001f)
		{
			return;
		}

		access.setter(obj, propertyName, newValue);
		ReflectionVariant nextVariant = access.getter(obj, propertyName);
		UndoRedoSystem::Instance().PushCommand(std::make_unique<ModifyComponentCommand>(obj.EntityId(), compName, propertyName, oldVariant, nextVariant));
	};

	auto setBoolPropertyWithUndo = [&](const char* propertyName, bool newValue) {
		ReflectionVariant oldVariant = access.getter(obj, propertyName);
		if (!IsValid(oldVariant) || !IsMetaValueType<bool>(oldVariant))
		{
			return;
		}

		const bool oldValue = MetaValue<bool>(oldVariant);
		if (oldValue == newValue)
		{
			return;
		}

		access.setter(obj, propertyName, newValue);
		ReflectionVariant nextVariant = access.getter(obj, propertyName);
		UndoRedoSystem::Instance().PushCommand(std::make_unique<ModifyComponentCommand>(obj.EntityId(), compName, propertyName, oldVariant, nextVariant));
	};

	auto setUintPropertyWithUndo = [&](const char* propertyName, std::uint32_t newValue) {
		ReflectionVariant oldVariant = access.getter(obj, propertyName);
		if (!IsValid(oldVariant) || !IsMetaValueType<std::uint32_t>(oldVariant))
		{
			return;
		}

		const std::uint32_t oldValue = MetaValue<std::uint32_t>(oldVariant);
		if (oldValue == newValue)
		{
			return;
		}

		access.setter(obj, propertyName, newValue);
		ReflectionVariant nextVariant = access.getter(obj, propertyName);
		UndoRedoSystem::Instance().PushCommand(std::make_unique<ModifyComponentCommand>(obj.EntityId(), compName, propertyName, oldVariant, nextVariant));
	};

	auto setVec3PropertyWithUndo = [&](const char* propertyName, const glm::vec3& newValue) {
		ReflectionVariant oldVariant = access.getter(obj, propertyName);
		if (!IsValid(oldVariant) || !IsMetaValueType<glm::vec3>(oldVariant))
		{
			return;
		}

		const glm::vec3 oldValue = MetaValue<glm::vec3>(oldVariant);
		if (glm::length(oldValue - newValue) <= 0.0001f)
		{
			return;
		}

		access.setter(obj, propertyName, newValue);
		ReflectionVariant nextVariant = access.getter(obj, propertyName);
		UndoRedoSystem::Instance().PushCommand(std::make_unique<ModifyComponentCommand>(obj.EntityId(), compName, propertyName, oldVariant, nextVariant));
	};

	constexpr std::uint32_t kMaxAuthorableType = static_cast<std::uint32_t>(LightType::Area);
	std::uint32_t encodedType = static_cast<std::uint32_t>(LightType::Point);
	if (ReflectionVariant typeVariant = access.getter(obj, "type"); IsValid(typeVariant) && IsMetaValueType<std::uint32_t>(typeVariant))
	{
		encodedType = MetaValue<std::uint32_t>(typeVariant);
	}
	const bool hasUnsupportedType = encodedType > kMaxAuthorableType;
	std::uint32_t effectiveType = hasUnsupportedType ? static_cast<std::uint32_t>(LightType::Point) : encodedType;

	const bool isDirectional = effectiveType == static_cast<std::uint32_t>(LightType::Directional);
	const bool isSpot = effectiveType == static_cast<std::uint32_t>(LightType::Spot);
	const bool isArea = effectiveType == static_cast<std::uint32_t>(LightType::Area);

	const ImGuiTreeNodeFlags sectionFlags = ImGuiTreeNodeFlags_DefaultOpen;
	if (ImGui::CollapsingHeader("General", sectionFlags))
	{
        if (ImGui::BeginTable("LightGeneral", 2, ImGuiTableFlags_SizingStretchProp)) {
            ImGui::TableSetupColumn("Label", ImGuiTableColumnFlags_WidthFixed, 100.0f);
            ImGui::TableSetupColumn("Widget", ImGuiTableColumnFlags_WidthStretch);

            const char* lightTypes[] = { "Directional", "Point", "Spot", "Area" };
            if (hasUnsupportedType)
            {
                ImGui::TableNextRow();
                ImGui::TableSetColumnIndex(0);
                ImGui::TableSetColumnIndex(1);
                ImGui::TextColored(ImVec4(1.0f, 0.72f, 0.24f, 1.0f), "Unsupported legacy light type value: %u", encodedType);
                ImGui::TextUnformatted("Select a supported type to replace it.");
            }

            int typeIndex = hasUnsupportedType
                ? static_cast<int>(LightType::Point)
                : static_cast<int>(effectiveType);
            drawLabeledWidget("Type", [&]() {
                if (ImGui::Combo("##Type", &typeIndex, lightTypes, IM_ARRAYSIZE(lightTypes)))
                {
                    typeIndex = std::clamp(typeIndex, 0, static_cast<int>(kMaxAuthorableType));
                    effectiveType = static_cast<std::uint32_t>(typeIndex);
                    setUintPropertyWithUndo("type", effectiveType);
                }
            });

            std::uint32_t currentAppearance = 0u;
            if (ReflectionVariant appVariant = access.getter(obj, "appearance"); IsValid(appVariant) && IsMetaValueType<std::uint32_t>(appVariant))
            {
                currentAppearance = MetaValue<std::uint32_t>(appVariant);
            }

            const char* appearanceTypes[] = { "Color", "Filter and Temperature" };
            int appIndex = static_cast<int>(currentAppearance);
            drawLabeledWidget("Appearance", [&]() {
                if (ImGui::Combo("##Appearance", &appIndex, appearanceTypes, IM_ARRAYSIZE(appearanceTypes)))
                {
                    setUintPropertyWithUndo("appearance", static_cast<std::uint32_t>(appIndex));
                }
            });

            glm::vec3 lightColor = glm::vec3(1.0f);
            if (ReflectionVariant colorVariant = access.getter(obj, "color"); IsValid(colorVariant) && IsMetaValueType<glm::vec3>(colorVariant))
            {
                lightColor = MetaValue<glm::vec3>(colorVariant);
            }
            drawLabeledWidget(currentAppearance == 0u ? "Color" : "Filter", [&]() {
                if (ImGui::ColorEdit3("##Color", &lightColor.x))
                {
                    setVec3PropertyWithUndo("color", lightColor);
                }
            });

            if (currentAppearance == 1u)
            {
                float temperature = 6500.0f;
                if (ReflectionVariant tempVariant = access.getter(obj, "temperature"); IsValid(tempVariant) && IsMetaValueType<float>(tempVariant))
                {
                    temperature = MetaValue<float>(tempVariant);
                }
                drawLabeledWidget("Temperature", [&]() {
                    if (ImGui::SliderFloat("##Temperature", &temperature, 1000.0f, 20000.0f, "%.0f K"))
                    {
                        setFloatPropertyWithUndo("temperature", temperature);
                    }
                });
            }

            float intensity = 1.0f;
            if (ReflectionVariant intensityVariant = access.getter(obj, "intensity"); IsValid(intensityVariant) && IsMetaValueType<float>(intensityVariant))
            {
                intensity = MetaValue<float>(intensityVariant);
            }

            std::uint32_t currentUnit = 0u;
            if (ReflectionVariant unitVariant = access.getter(obj, "unit"); IsValid(unitVariant) && IsMetaValueType<std::uint32_t>(unitVariant))
            {
                currentUnit = MetaValue<std::uint32_t>(unitVariant);
            }

            const char* intensityFormat = "%.2f";
            if (isDirectional)
            {
                intensityFormat = "%.2f lux";
            }
            else
            {
                intensityFormat = currentUnit == 0u ? "%.2f lm" : "%.2f lux";
            }

            drawLabeledWidget("Intensity", [&]() {
                if (ImGui::DragFloat("##Intensity", &intensity, 0.05f, 0.0f, 1000000.0f, intensityFormat))
                {
                    setFloatPropertyWithUndo("intensity", std::max(0.0f, intensity));
                }
            });
            UpdateInspectorDragState();

            if (!isDirectional)
            {
                const char* lightUnits[] = { "Lumens", "Lux" };
                int unitIndex = std::clamp(static_cast<int>(currentUnit), 0, 1);
                drawLabeledWidget("Unit", [&]() {
                    if (ImGui::Combo("##Unit", &unitIndex, lightUnits, IM_ARRAYSIZE(lightUnits)))
                    {
                        setUintPropertyWithUndo("unit", static_cast<std::uint32_t>(unitIndex));

                        // Convert intensity value to maintain perceived brightness
                        if (currentUnit == 0u && unitIndex == 1) // Lumens -> Lux
                        {
                            setFloatPropertyWithUndo("intensity", std::max(0.0f, intensity / (4.0f * glm::pi<float>())));
                        }
                        else if (currentUnit == 1u && unitIndex == 0) // Lux -> Lumens
                        {
                            setFloatPropertyWithUndo("intensity", std::max(0.0f, intensity * (4.0f * glm::pi<float>())));
                        }
                    }
                });
            }

            float indirectMultiplier = 1.0f;
            if (ReflectionVariant indVariant = access.getter(obj, "indirectMultiplier"); IsValid(indVariant) && IsMetaValueType<float>(indVariant))
            {
                indirectMultiplier = MetaValue<float>(indVariant);
            }
            drawLabeledWidget("Indirect", [&]() {
                if (ImGui::DragFloat("##Indirect", &indirectMultiplier, 0.05f, 0.0f, 10.0f))
                {
                    setFloatPropertyWithUndo("indirectMultiplier", std::max(0.0f, indirectMultiplier));
                }
            });
            UpdateInspectorDragState();

            ImGui::EndTable();
        }
	}

	if (ImGui::CollapsingHeader("Shape", sectionFlags))
	{
        if (ImGui::BeginTable("LightShape", 2, ImGuiTableFlags_SizingStretchProp)) {
            ImGui::TableSetupColumn("Label", ImGuiTableColumnFlags_WidthFixed, 100.0f);
            ImGui::TableSetupColumn("Widget", ImGuiTableColumnFlags_WidthStretch);

            if (!isDirectional)
            {
                float range = 10.0f;
                if (ReflectionVariant rangeVariant = access.getter(obj, "range"); IsValid(rangeVariant) && IsMetaValueType<float>(rangeVariant))
                {
                    range = MetaValue<float>(rangeVariant);
                }
                drawLabeledWidget("Range", [&]() {
                    if (ImGui::DragFloat("##Range", &range, 0.1f, 0.1f, 10000.0f, "%.2f m"))
                    {
                        setFloatPropertyWithUndo("range", std::max(0.1f, range));
                    }
                });
                UpdateInspectorDragState();
            }

            if (isSpot)
            {
                float innerDegrees = 20.0f;
                float outerDegrees = 30.0f;
                if (ReflectionVariant innerVariant = access.getter(obj, "innerConeAngle"); IsValid(innerVariant) && IsMetaValueType<float>(innerVariant))
                {
                    innerDegrees = glm::degrees(MetaValue<float>(innerVariant));
                }
                if (ReflectionVariant outerVariant = access.getter(obj, "outerConeAngle"); IsValid(outerVariant) && IsMetaValueType<float>(outerVariant))
                {
                    outerDegrees = glm::degrees(MetaValue<float>(outerVariant));
                }

                drawLabeledWidget("Inner Angle", [&]() {
                    if (ImGui::DragFloat("##InnerAngle", &innerDegrees, 0.1f, 0.0f, 89.0f, "%.1f deg")) {
                        innerDegrees = std::clamp(innerDegrees, 0.0f, 89.0f);
                        outerDegrees = std::clamp(outerDegrees, innerDegrees + 0.1f, 89.9f);
                        setFloatPropertyWithUndo("innerConeAngle", glm::radians(innerDegrees));
                        setFloatPropertyWithUndo("outerConeAngle", glm::radians(outerDegrees));
                    }
                });
                UpdateInspectorDragState();

                drawLabeledWidget("Outer Angle", [&]() {
                    if (ImGui::DragFloat("##OuterAngle", &outerDegrees, 0.1f, 0.1f, 89.9f, "%.1f deg")) {
                        innerDegrees = std::clamp(innerDegrees, 0.0f, 89.0f);
                        outerDegrees = std::clamp(outerDegrees, innerDegrees + 0.1f, 89.9f);
                        setFloatPropertyWithUndo("innerConeAngle", glm::radians(innerDegrees));
                        setFloatPropertyWithUndo("outerConeAngle", glm::radians(outerDegrees));
                    }
                });
                UpdateInspectorDragState();
            }

            if (isArea)
            {
                float width = 1.0f;
                float height = 1.0f;
                if (ReflectionVariant wVariant = access.getter(obj, "width"); IsValid(wVariant) && IsMetaValueType<float>(wVariant))
                {
                    width = MetaValue<float>(wVariant);
                }
                if (ReflectionVariant hVariant = access.getter(obj, "height"); IsValid(hVariant) && IsMetaValueType<float>(hVariant))
                {
                    height = MetaValue<float>(hVariant);
                }

                drawLabeledWidget("Width", [&]() {
                    if (ImGui::DragFloat("##Width", &width, 0.1f, 0.01f, 1000.0f, "%.2f m")) setFloatPropertyWithUndo("width", std::max(0.01f, width));
                });
                UpdateInspectorDragState();

                drawLabeledWidget("Height", [&]() {
                    if (ImGui::DragFloat("##Height", &height, 0.1f, 0.01f, 1000.0f, "%.2f m")) setFloatPropertyWithUndo("height", std::max(0.01f, height));
                });
                UpdateInspectorDragState();
            }

            ImGui::EndTable();
        }
	}

	if (ImGui::CollapsingHeader("Shadows", sectionFlags))
	{
        if (ImGui::BeginTable("LightShadows", 2, ImGuiTableFlags_SizingStretchProp)) {
            ImGui::TableSetupColumn("Label", ImGuiTableColumnFlags_WidthFixed, 100.0f);
            ImGui::TableSetupColumn("Widget", ImGuiTableColumnFlags_WidthStretch);

            bool castsShadows = false;
            if (ReflectionVariant castVariant = access.getter(obj, "castsShadows"); IsValid(castVariant) && IsMetaValueType<bool>(castVariant))
            {
                castsShadows = MetaValue<bool>(castVariant);
            }
            drawLabeledWidget("Realtime Shadows", [&]() {
                if (ImGui::Checkbox("##RealtimeShadows", &castsShadows))
                {
                    setBoolPropertyWithUndo("castsShadows", castsShadows);
                }
            });

            if (castsShadows)
            {
                float shadowStrength = 1.0f;
                if (ReflectionVariant strVariant = access.getter(obj, "shadowStrength"); IsValid(strVariant) && IsMetaValueType<float>(strVariant))
                {
                    shadowStrength = MetaValue<float>(strVariant);
                }
                drawLabeledWidget("Strength", [&]() {
                    if (ImGui::SliderFloat("##Strength", &shadowStrength, 0.0f, 1.0f))
                    {
                        setFloatPropertyWithUndo("shadowStrength", std::clamp(shadowStrength, 0.0f, 1.0f));
                    }
                });

                float shadowBias = 0.005f;
                if (ReflectionVariant biasVariant = access.getter(obj, "shadowBias"); IsValid(biasVariant) && IsMetaValueType<float>(biasVariant))
                {
                    shadowBias = MetaValue<float>(biasVariant);
                }
                drawLabeledWidget("Bias", [&]() {
                    if (ImGui::DragFloat("##Bias", &shadowBias, 0.001f, 0.0f, 0.1f, "%.4f"))
                    {
                        setFloatPropertyWithUndo("shadowBias", std::clamp(shadowBias, 0.0f, 0.1f));
                    }
                });
                UpdateInspectorDragState();
            }

            ImGui::EndTable();
        }
	}
}

void InspectorWindow::DrawUIComponentInspector(const std::string& compName, Object& obj, ComponentAccessFunctions& access)
{
	UIComponent* uiComponent = obj.getComponent<UIComponent>();
	if (uiComponent == nullptr)
	{
		return;
	}

	auto drawLabeledWidget = [&](const char* label, auto widgetFunc) {
		ImGui::TableNextRow();
		ImGui::TableSetColumnIndex(0);
		ImGui::AlignTextToFramePadding();
		ImGui::TextUnformatted(label);
		ImGui::TableSetColumnIndex(1);
		ImGui::SetNextItemWidth(-1.0f);
		widgetFunc();
	};

	auto setStringPropertyWithUndo = [&](const char* propertyName, const std::string& newValue) {
		ReflectionVariant oldVariant = access.getter(obj, propertyName);
		if (!IsValid(oldVariant) || !IsMetaValueType<std::string>(oldVariant))
		{
			return;
		}

		const std::string oldValue = MetaValue<std::string>(oldVariant);
		if (oldValue == newValue)
		{
			return;
		}

		access.setter(obj, propertyName, newValue);
		ReflectionVariant nextVariant = access.getter(obj, propertyName);
		UndoRedoSystem::Instance().PushCommand(std::make_unique<ModifyComponentCommand>(obj.EntityId(), compName, propertyName, oldVariant, nextVariant));
	};

	// The custom UIComponent inspector owns its own property table because the
	// generic component path is bypassed for this specialized multi-asset UI.
	if (ImGui::BeginTable("UIComponentPropertyTable", 2, ImGuiTableFlags_SizingStretchProp))
	{
		ImGui::TableSetupColumn("Label", ImGuiTableColumnFlags_WidthFixed, 100.0f);
		ImGui::TableSetupColumn("Widget", ImGuiTableColumnFlags_WidthStretch);

		drawLabeledWidget("Source Asset", [&]() {
			std::string markupPath = uiComponent->GetSourceMarkup();
			char markupBuffer[256];
			strncpy(markupBuffer, markupPath.c_str(), sizeof(markupBuffer));
			markupBuffer[sizeof(markupBuffer) - 1] = '\0';
			if (ImGui::InputText("##UIComponentSourceMarkup", markupBuffer, sizeof(markupBuffer)))
			{
				setStringPropertyWithUndo("sourceMarkup", markupBuffer);
			}

			if (ImGui::BeginPopupContextItem())
			{
				if (ImGui::MenuItem("Clear"))
				{
					setStringPropertyWithUndo("sourceMarkup", "");
				}
				ImGui::EndPopup();
			}

			if (ImGui::BeginDragDropTarget())
			{
				if (const ImGuiPayload* payload = ImGui::AcceptDragDropPayload("ASSET_TUIML"))
				{
					IM_ASSERT(payload->DataSize == sizeof(TerraEngine::UUID));
					const TerraEngine::UUID assetGuid = *static_cast<const TerraEngine::UUID*>(payload->Data);
					if (const AssetMetadata* meta = AssetDatabase::Instance().FindAssetByGuid(assetGuid))
					{
						setStringPropertyWithUndo("sourceMarkup", meta->sourcePath.generic_string());
					}
				}
				ImGui::EndDragDropTarget();
			}
		});

		ImGui::EndTable();
	}
}

void InspectorWindow::OnGui() {
	m_InspectorValueDragging = false;
    
    auto selectedAsset = m_Owner->GetSelectedAsset();
    if (selectedAsset) {
        std::string ext = selectedAsset->extension().string();
        std::transform(ext.begin(), ext.end(), ext.begin(), ::tolower);

        if (ext == ".tmat") {
            DrawMaterialInspector(*selectedAsset);
        } else if (IsSupportedMeshSourceExtension(*selectedAsset)) {
            DrawMeshInspector(*selectedAsset);
        } else if (IsSupportedTextureSourceExtension(*selectedAsset)) {
            DrawTextureInspector(*selectedAsset);
        } else {
             ImGui::Text("Selected Asset: %s", selectedAsset->filename().string().c_str());
        }
        return;
    }

    auto selectedEntity = m_Owner->GetSelectedEntity();
    if (selectedEntity) {
        auto& objManager = ObjectManager::instance();
        if (auto objOpt = objManager.getObject(*selectedEntity)) {
            Object obj = *objOpt;
            
            auto& accessReg = ComponentAccessRegistry();

            // Header: Active Checkbox, Name Input, Static Checkbox
            ImGui::PushID("InspectorHeaderTop");
            
            bool isActive = obj.isActive();
            if (ImGui::Checkbox("##ActiveGlobal", &isActive)) {
                ReflectionVariant oldVal = obj.isActive();
                obj.setActive(isActive);
                ReflectionVariant newVal = obj.isActive();
                UndoRedoSystem::Instance().PushCommand(std::make_unique<ModifyObjectMetadataCommand>(obj.EntityId(), ObjectMetadataField::Active, oldVal, newVal));
            }
            ImGui::PopID();
            
            ImGui::SameLine();
            
            std::string name = obj.getName();
            char nameBuf[256];
            strncpy(nameBuf, name.c_str(), sizeof(nameBuf));
            
            // Calculate width for name input: available width minus "Static" checkbox width
            float staticWidth = ImGui::CalcTextSize("Static").x + ImGui::GetStyle().ItemSpacing.x + ImGui::GetFrameHeight();
            ImGui::SetNextItemWidth(ImGui::GetContentRegionAvail().x - staticWidth - ImGui::GetStyle().ItemSpacing.x);
            
            if (ImGui::InputText("##ObjectName", nameBuf, sizeof(nameBuf))) {
                if (name != nameBuf) {
                    UndoRedoSystem::Instance().PushCommand(std::make_unique<RenameObjectCommand>(obj.EntityId(), name, nameBuf));
                    obj.setName(nameBuf);
                }
            }
            
            ImGui::SameLine();
            
            bool isStatic = obj.isStatic();
            if (ImGui::Checkbox("Static", &isStatic)) {
                ReflectionVariant oldVal = obj.isStatic();
                obj.setStatic(isStatic);
                ReflectionVariant newVal = obj.isStatic();
                UndoRedoSystem::Instance().PushCommand(std::make_unique<ModifyObjectMetadataCommand>(obj.EntityId(), ObjectMetadataField::Static, oldVal, newVal));
            }

            // Tag and Layer
            ImGui::Spacing();
            
            // Use a table for alignment
            if (ImGui::BeginTable("TagLayerTable", 4, ImGuiTableFlags_SizingStretchProp)) {
                ImGui::TableSetupColumn("TagLabel", ImGuiTableColumnFlags_WidthFixed, 30.0f);
                ImGui::TableSetupColumn("TagValue", ImGuiTableColumnFlags_WidthStretch);
                ImGui::TableSetupColumn("LayerLabel", ImGuiTableColumnFlags_WidthFixed, 40.0f);
                ImGui::TableSetupColumn("LayerValue", ImGuiTableColumnFlags_WidthStretch);

                ImGui::TableNextRow();
                
                ImGui::TableSetColumnIndex(0);
                ImGui::AlignTextToFramePadding();
                ImGui::TextDisabled("Tag");
                
                ImGui::TableSetColumnIndex(1);
                std::string tag = obj.getTag();
                ImGui::SetNextItemWidth(-1.0f);
                if (ImGui::BeginCombo("##TagCombo", tag.c_str())) {
                    const auto& tags = objManager.getAvailableTags();
                    for (const auto& t : tags) {
                        bool is_selected = (tag == t);
                        if (ImGui::Selectable(t.c_str(), is_selected)) {
                            if (tag != t) {
                                ReflectionVariant oldV = tag;
                                obj.setTag(t);
                                ReflectionVariant newV = t;
                                UndoRedoSystem::Instance().PushCommand(std::make_unique<ModifyObjectMetadataCommand>(obj.EntityId(), ObjectMetadataField::Tag, oldV, newV));
                            }
                        }
                        if (is_selected) ImGui::SetItemDefaultFocus();
                    }
                    
                    ImGui::Separator();
                    if (ImGui::Selectable("Add Tag...")) {
                        ImGui::OpenPopup("AddTagPopup");
                        memset(m_TagInputBuffer, 0, sizeof(m_TagInputBuffer));
                    }
                    
                    ImGui::EndCombo();
                }

                if (ImGui::BeginPopup("AddTagPopup")) {
                    ImGui::Text("New Tag Name:");
                    ImGui::InputText("##NewTagName", m_TagInputBuffer, sizeof(m_TagInputBuffer));
                    if (ImGui::Button("Add") || (ImGui::IsKeyPressed(ImGuiKey_Enter))) {
                        if (strlen(m_TagInputBuffer) > 0) {
                            objManager.registerTag(m_TagInputBuffer);
                        }
                        ImGui::CloseCurrentPopup();
                    }
                    ImGui::SameLine();
                    if (ImGui::Button("Cancel")) {
                        ImGui::CloseCurrentPopup();
                    }
                    ImGui::EndPopup();
                }

                ImGui::TableSetColumnIndex(2);
                ImGui::AlignTextToFramePadding();
                ImGui::TextDisabled("Layer");
                
                ImGui::TableSetColumnIndex(3);
                std::string layer = obj.getLayer();
                ImGui::SetNextItemWidth(-1.0f);
                if (ImGui::BeginCombo("##LayerCombo", layer.c_str())) {
                    const auto& layers = objManager.getAvailableLayers();
                    for (const auto& l : layers) {
                        bool is_selected = (layer == l);
                        if (ImGui::Selectable(l.c_str(), is_selected)) {
                            if (layer != l) {
                                ReflectionVariant oldV = layer;
                                obj.setLayer(l);
                                ReflectionVariant newV = l;
                                UndoRedoSystem::Instance().PushCommand(std::make_unique<ModifyObjectMetadataCommand>(obj.EntityId(), ObjectMetadataField::Layer, oldV, newV));
                            }
                        }
                        if (is_selected) ImGui::SetItemDefaultFocus();
                    }

                    ImGui::Separator();
                    if (ImGui::Selectable("Add Layer...")) {
                        ImGui::OpenPopup("AddLayerPopup");
                        memset(m_LayerInputBuffer, 0, sizeof(m_LayerInputBuffer));
                    }

                    ImGui::EndCombo();
                }

                if (ImGui::BeginPopup("AddLayerPopup")) {
                    ImGui::Text("New Layer Name:");
                    ImGui::InputText("##NewLayerName", m_LayerInputBuffer, sizeof(m_LayerInputBuffer));
                    if (ImGui::Button("Add") || (ImGui::IsKeyPressed(ImGuiKey_Enter))) {
                        if (strlen(m_LayerInputBuffer) > 0) {
                            objManager.registerLayer(m_LayerInputBuffer);
                        }
                        ImGui::CloseCurrentPopup();
                    }
                    ImGui::SameLine();
                    if (ImGui::Button("Cancel")) {
                        ImGui::CloseCurrentPopup();
                    }
                    ImGui::EndPopup();
                }

                ImGui::EndTable();
            }
            
            ImGui::Separator();
            
            std::vector<std::string>& inspectorComponentOrder = m_PerObjectComponentOrders[obj.EntityId()];
            
            std::unordered_set<std::string> activeComponents;
            bool hasTransform = false;
            for (auto& [compName, access] : accessReg) {
                if (access.hasComponent(obj)) {
                    activeComponents.insert(compName);
                    if (compName == "Transform") hasTransform = true;
                }
            }

            for (auto it = inspectorComponentOrder.begin(); it != inspectorComponentOrder.end(); ) {
                if (activeComponents.find(*it) == activeComponents.end()) it = inspectorComponentOrder.erase(it);
                else ++it;
            }
            for (const auto& compName : activeComponents) {
                bool found = false;
                for (const auto& existing : inspectorComponentOrder) if (existing == compName) found = true;
                if (!found) inspectorComponentOrder.push_back(compName);
            }

            if (hasTransform) {
                auto it = std::find(inspectorComponentOrder.begin(), inspectorComponentOrder.end(), "Transform");
                if (it != inspectorComponentOrder.end() && it != inspectorComponentOrder.begin()) {
                    inspectorComponentOrder.erase(it);
                    inspectorComponentOrder.insert(inspectorComponentOrder.begin(), "Transform");
                } else if (it == inspectorComponentOrder.end()) {
                    inspectorComponentOrder.insert(inspectorComponentOrder.begin(), "Transform");
                }
            }

            for (size_t i = 0; i < inspectorComponentOrder.size(); ++i) {
                std::string compName = inspectorComponentOrder[i];
                if (accessReg.find(compName) == accessReg.end()) continue;
                ComponentAccessFunctions& access = accessReg[compName];
				if (compName == "Script") {
					// ScriptComponent is just the serialized backing store. Draw each
					// script binding as its own Unity-style component block instead.
					DrawScriptComponentInspector(obj);
					continue;
				}
				const std::string displayName = BuildDisplayComponentName(compName);

                if (!access.hasComponent(obj)) continue; 

                ImGui::PushID(compName.c_str());
                
                bool compActive = true;
                // Use the reflection type captured by the engine registry so
                // the editor queries the same EnTT meta context that owns the
                // component registrations.
                ReflectionType type = access.reflectionType;
                ReflectionProperty activeProp = FindMetaProperty(type, "active");
                if (IsValid(activeProp)) {
                    ReflectionVariant var = access.getter(obj, "active");
                    if (IsValid(var)) compActive = MetaValue<bool>(var);
                }

                bool isOpen = ImGui::GetStateStorage()->GetInt(ImGui::GetID("IsOpen"), 1) != 0;
                ImVec2 p = ImGui::GetCursorScreenPos();
                float availWidth = ImGui::GetContentRegionAvail().x;
                float lineHeight = ImGui::GetFrameHeight();

                ImGui::GetWindowDrawList()->AddRectFilled(p, ImVec2(p.x + availWidth, p.y + lineHeight), ImGui::GetColorU32(ImGuiCol_Header));

                ImGui::SetNextItemAllowOverlap();
                if (ImGui::InvisibleButton("##header_bg", ImVec2(availWidth, lineHeight))) {
                    isOpen = !isOpen;
                    ImGui::GetStateStorage()->SetInt(ImGui::GetID("IsOpen"), isOpen);
                }

                if (compName != "Transform") {
                    if (ImGui::BeginPopupContextItem("ComponentSettings")) {
                        if (ImGui::MenuItem("Remove Component")) {
                            UndoRedoSystem::Instance().PushCommand(std::make_unique<RemoveComponentCommand>(obj.EntityId(), compName));
                            access.removeComponent(obj);
                        }
                        ImGui::EndPopup();
                    }
                }

                if (!access.hasComponent(obj)) {
                    ImGui::PopID();
                    continue;
                }

                if (compName != "Transform") {
                    if (ImGui::BeginDragDropSource()) {
                        int sourceIdx = (int)i;
                        ImGui::SetDragDropPayload("COMPONENT_ORDER", &sourceIdx, sizeof(int));
                        ImGui::Text("Reorder %s", displayName.c_str());
                        ImGui::EndDragDropSource();
                    }
                    if (ImGui::BeginDragDropTarget()) {
                        if (const ImGuiPayload* payload = ImGui::AcceptDragDropPayload("COMPONENT_ORDER")) {
                            int sourceIdx = *(const int*)payload->Data;
                            int targetIdx = (int)i;
                            if (sourceIdx >= 0 && sourceIdx < inspectorComponentOrder.size() && targetIdx < inspectorComponentOrder.size()) {
                                std::string sourceName = inspectorComponentOrder[sourceIdx];
                                if (sourceName != "Transform") {
                                    if (sourceIdx < targetIdx) {
                                        inspectorComponentOrder.insert(inspectorComponentOrder.begin() + targetIdx + 1, sourceName);
                                        inspectorComponentOrder.erase(inspectorComponentOrder.begin() + sourceIdx);
                                    } else if (sourceIdx > targetIdx) {
                                        inspectorComponentOrder.insert(inspectorComponentOrder.begin() + targetIdx, sourceName);
                                        inspectorComponentOrder.erase(inspectorComponentOrder.begin() + sourceIdx + 1);
                                    }
                                }
                            }
                        }
                        ImGui::EndDragDropTarget();
                    }
                }

                ImGui::SetCursorScreenPos(p);
                ImGui::Dummy(ImVec2(2, 0)); ImGui::SameLine(); 
                if (ImGui::ArrowButton("##arrow", isOpen ? ImGuiDir_Down : ImGuiDir_Right)) {
                    isOpen = !isOpen;
                    ImGui::GetStateStorage()->SetInt(ImGui::GetID("IsOpen"), isOpen);
                }

                ImGui::SameLine();
                
                if (compName != "Transform") {
                    ImGui::PushStyleColor(ImGuiCol_FrameBg, ImVec4(0.2f, 0.2f, 0.2f, 1.0f));
                    if (ImGui::Checkbox("##compActive", &compActive)) {
                        if (IsValid(activeProp)) {
                            ReflectionVariant oldA = access.getter(obj, "active");
                            access.setter(obj, "active", compActive);
                            ReflectionVariant newA = access.getter(obj, "active");
                            UndoRedoSystem::Instance().PushCommand(std::make_unique<ModifyComponentCommand>(obj.EntityId(), compName, "active", oldA, newA));
                        }
                    }
                    ImGui::PopStyleColor();
                } else {
                    ImGui::Dummy(ImVec2(ImGui::GetFrameHeight(), 0)); 
                }

                ImGui::SameLine();
                ImGui::AlignTextToFramePadding();
                ImGui::TextUnformatted(displayName.c_str());

                ImGui::SetCursorScreenPos(ImVec2(p.x, p.y + lineHeight));
                ImGui::Dummy(ImVec2(0,0));

                if (isOpen) {
                    ImGui::Indent();
					if (compName == "LightComponent")
					{
						DrawLightComponentInspector(compName, obj, access);
					}
					else if (compName == "UIComponent")
					{
						DrawUIComponentInspector(compName, obj, access);
					}
					else if (compName == "Script")
					{
						DrawScriptComponentInspector(obj);
					}
					else
					{
                        if (ImGui::BeginTable("PropertyTable", 2, ImGuiTableFlags_SizingStretchProp)) {
                            ImGui::TableSetupColumn("Label", ImGuiTableColumnFlags_WidthFixed, 100.0f);
                            ImGui::TableSetupColumn("Widget", ImGuiTableColumnFlags_WidthStretch);

                            for (const ReflectionProperty& prop : TerraEngine::Reflection::GetProperties(type)) {
                                if (MetaPropertyName(prop) == "active") continue;
                                DrawProperty(compName, prop, obj, access);
                            }
                            ImGui::EndTable();
                        }
					}
                    ImGui::Unindent();
                }
                ImGui::Separator();
                ImGui::PopID();
            }
            ImGui::Spacing();
            DrawScriptDropTarget(obj);
            ImGui::Spacing();

            float availWidth = ImGui::GetContentRegionAvail().x;
            if (ImGui::Button("Add Component", ImVec2(availWidth, 0))) {
                ImGui::OpenPopup("AddComponentPopup");
            }

            if (ImGui::BeginPopup("AddComponentPopup")) {
                static char searchBuf[64] = "";
                ImGui::InputTextWithHint("##CompSearch", "Search...", searchBuf, IM_ARRAYSIZE(searchBuf));
                
                ImGui::Separator();

                std::vector<std::string> availableComponents;
                for (auto& [name, access] : accessReg) {
                    if (name != "Script" && !access.hasComponent(obj)) {
                        availableComponents.push_back(name);
                    }
                }
				std::sort(availableComponents.begin(), availableComponents.end(), [](const std::string& lhs, const std::string& rhs) {
					return BuildDisplayComponentName(lhs) < BuildDisplayComponentName(rhs);
				});

                for (const auto& compName : availableComponents) {
					const std::string displayName = BuildDisplayComponentName(compName);
                    if (searchBuf[0] != '\0') {
                        std::string lowerName = compName;
						std::string lowerDisplayName = displayName;
                        std::string lowerFilter = searchBuf;
                        std::transform(lowerName.begin(), lowerName.end(), lowerName.begin(), ::tolower);
						std::transform(lowerDisplayName.begin(), lowerDisplayName.end(), lowerDisplayName.begin(), ::tolower);
                        std::transform(lowerFilter.begin(), lowerFilter.end(), lowerFilter.begin(), ::tolower);
						if (lowerName.find(lowerFilter) == std::string::npos
							&& lowerDisplayName.find(lowerFilter) == std::string::npos) continue;
                    }

					ImGui::PushID(compName.c_str());
                    if (ImGui::Selectable(displayName.c_str())) {
                        UndoRedoSystem::Instance().PushCommand(std::make_unique<AddComponentCommand>(obj.EntityId(), compName));
                        accessReg[compName].addComponent(obj);
                        ImGui::CloseCurrentPopup();
                    }
					ImGui::PopID();
                }
                ImGui::EndPopup();
            }

        } else {
            m_Owner->SetSelectedEntity(std::nullopt);
        }
    }
}

void InspectorWindow::DrawMaterialInspector(const std::filesystem::path& path) {
    if (m_LastInspectedAsset != path) {
        m_LastInspectedAsset = path;
        std::ifstream in(path, std::ios::binary | std::ios::ate);
        if (in.is_open()) {
            std::streamsize size = in.tellg();
            in.seekg(0, std::ios::beg);
            m_MaterialDataBuffer.resize(size);
            if (!in.read((char*)m_MaterialDataBuffer.data(), size)) {
                m_MaterialDataBuffer.clear();
            }
        }
    }

    if (m_MaterialDataBuffer.size() < sizeof(MaterialHeader)) {
        ImGui::TextColored(ImVec4(1, 0, 0, 1), "Invalid material file!");
        return;
    }

    MaterialHeader* header = (MaterialHeader*)m_MaterialDataBuffer.data();
    bool changed = false;

    ImGui::Text("Material: %s", path.filename().string().c_str());
    ImGui::Separator();

    if (ImGui::BeginTable("MaterialSettings", 2, ImGuiTableFlags_SizingStretchProp)) {
        ImGui::TableSetupColumn("Label", ImGuiTableColumnFlags_WidthFixed, 120.0f);
        ImGui::TableSetupColumn("Widget", ImGuiTableColumnFlags_WidthStretch);

        auto drawLabeledWidget = [&](const char* label, auto widgetFunc) {
            ImGui::TableNextRow();
            ImGui::TableSetColumnIndex(0);
            ImGui::AlignTextToFramePadding();
            ImGui::TextUnformatted(label);
            ImGui::TableSetColumnIndex(1);
            ImGui::SetNextItemWidth(-1.0f);
            widgetFunc();
        };

        // Shading Model
        const char* shadingModels[] = { "Unlit", "MetallicRoughness", "SpecularGlossiness" };
        int shadingModelIdx = (int)header->shadingModel;
        drawLabeledWidget("Shading Model", [&]() {
            if (ImGui::Combo("##ShadingModel", &shadingModelIdx, shadingModels, IM_ARRAYSIZE(shadingModels))) {
                header->shadingModel = (uint32_t)shadingModelIdx;
                changed = true;
            }
        });

        // Alpha Mode
        const char* alphaModes[] = { "Opaque", "Mask", "Blend" };
        int alphaModeIdx = (int)header->alphaMode;
        drawLabeledWidget("Alpha Mode", [&]() {
            if (ImGui::Combo("##AlphaMode", &alphaModeIdx, alphaModes, IM_ARRAYSIZE(alphaModes))) {
                header->alphaMode = (uint32_t)alphaModeIdx;
                changed = true;
            }
        });

        if (header->alphaMode == (uint32_t)AlphaMode::Mask) {
            drawLabeledWidget("Alpha Cutoff", [&]() {
                if (ImGui::DragFloat("##AlphaCutoff", &header->alphaCutoff, 0.01f, 0.0f, 1.0f)) changed = true;
            });
        }

        // Shader Collection
        {
            TerraEngine::UUID currentShaderId = header->shaderCollectionId;
            std::string shaderName = "None";
            bool shaderExists = false;
            if (currentShaderId != TerraEngine::EMPTY_UUID) {
                for (const auto& [path, meta] : AssetDatabase::Instance().GetAssets()) {
                    if (meta.guid == currentShaderId) {
                        shaderName = path.filename().string();
                        shaderExists = true;
                        break;
                    }
                }
                if (shaderName == "None" && currentShaderId == ResourceManager::Instance().GetDefaultShaderCollectionID()) {
                    shaderName = "Default Shader";
                    shaderExists = true;
                }

                if (!shaderExists) {
                    shaderName = "(Missing)";
                }
            }

            drawLabeledWidget("Shader", [&]() {
                char buf[128];
                strncpy(buf, shaderName.c_str(), sizeof(buf));
                if (currentShaderId != TerraEngine::EMPTY_UUID && !shaderExists) ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1, 0, 0, 1));
                ImGui::InputText("##shader", buf, sizeof(buf), ImGuiInputTextFlags_ReadOnly);
                if (currentShaderId != TerraEngine::EMPTY_UUID && !shaderExists) ImGui::PopStyleColor();

                if (ImGui::BeginDragDropTarget()) {
                    if (const ImGuiPayload* payload = ImGui::AcceptDragDropPayload("ASSET_SHADER")) {
                        header->shaderCollectionId = *(const TerraEngine::UUID*)payload->Data;
                        changed = true;
                    }
                    ImGui::EndDragDropTarget();
                }
                if (ImGui::BeginPopupContextItem("shader_context")) {
                    if (ImGui::MenuItem("Set Default")) {
                        header->shaderCollectionId = ResourceManager::Instance().GetDefaultShaderCollectionID();
                        changed = true;
                    }
                    if (ImGui::MenuItem("Clear")) {
                        header->shaderCollectionId = TerraEngine::EMPTY_UUID;
                        changed = true;
                    }
                    ImGui::EndPopup();
                }
            });
        }

        drawLabeledWidget("Base Color", [&]() {
            if (ImGui::ColorEdit4("##BaseColor", header->baseColorFactor)) changed = true;
        });

        drawLabeledWidget("Metallic", [&]() {
            if (ImGui::DragFloat("##Metallic", &header->metallicFactor, 0.01f, 0.0f, 1.0f)) changed = true;
        });

        drawLabeledWidget("Roughness", [&]() {
            if (ImGui::DragFloat("##Roughness", &header->roughnessFactor, 0.01f, 0.0f, 1.0f)) changed = true;
        });
        
        drawLabeledWidget("Emissive", [&]() {
            if (ImGui::ColorEdit3("##Emissive", header->emissiveFactor)) changed = true;
        });
        
        drawLabeledWidget("Normal Scale", [&]() {
            if (ImGui::DragFloat("##NormalScale", &header->normalScale, 0.01f)) changed = true;
        });

        drawLabeledWidget("Occlusion", [&]() {
            if (ImGui::DragFloat("##Occlusion", &header->occlusionStrength, 0.01f, 0.0f, 1.0f)) changed = true;
        });

        ImGui::EndTable();
    }

    ImGui::Separator();
    ImGui::Text("Textures");

    auto drawTextureSlot = [&](const char* label, TextureSemantic semantic) {
        TextureBinding* targetBinding = nullptr;
        TextureBinding* allBindings = (TextureBinding*)(m_MaterialDataBuffer.data() + header->textureArrayOffset);
        
        for (uint32_t i = 0; i < header->textureCount; ++i) {
            if (allBindings[i].semantic == semantic) {
                targetBinding = &allBindings[i];
                break;
            }
        }

        TerraEngine::UUID currentId = targetBinding ? targetBinding->textureUuid : TerraEngine::EMPTY_UUID;
        std::string assetName = "None";
        bool assetExists = false;
        if (currentId != TerraEngine::EMPTY_UUID) {
            for (const auto& [path, meta] : AssetDatabase::Instance().GetAssets()) {
                if (meta.guid == currentId) {
                    assetName = path.filename().string();
                    assetExists = true;
                    break;
                }
            }
            if (!assetExists) {
                assetName = "(Missing)";
            }
        }

        ImGui::PushID(label);
        
        if (ImGui::BeginTable("TextureSlotTable", 2, ImGuiTableFlags_SizingStretchProp)) {
            ImGui::TableSetupColumn("Label", ImGuiTableColumnFlags_WidthFixed, 120.0f);
            ImGui::TableSetupColumn("Widget", ImGuiTableColumnFlags_WidthStretch);

            ImGui::TableNextRow();
            ImGui::TableSetColumnIndex(0);
            ImGui::AlignTextToFramePadding();
            ImGui::TextUnformatted(label);
            ImGui::TableSetColumnIndex(1);
            ImGui::SetNextItemWidth(-1.0f);

            char buf[128];
            strncpy(buf, assetName.c_str(), sizeof(buf));
            if (currentId != TerraEngine::EMPTY_UUID && !assetExists) ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1, 0, 0, 1));
            ImGui::InputText("##tex", buf, sizeof(buf), ImGuiInputTextFlags_ReadOnly);
            if (currentId != TerraEngine::EMPTY_UUID && !assetExists) ImGui::PopStyleColor();

            if (ImGui::BeginDragDropTarget()) {
                if (const ImGuiPayload* payload = ImGui::AcceptDragDropPayload("ASSET_TEXTURE")) {
                    TerraEngine::UUID newId = *(const TerraEngine::UUID*)payload->Data;
                    
                    if (!targetBinding) {
                        // Add new binding
                        size_t insertPos = header->textureArrayOffset + (header->textureCount * sizeof(TextureBinding));
                        m_MaterialDataBuffer.insert(m_MaterialDataBuffer.begin() + insertPos, sizeof(TextureBinding), 0);
                        
                        // CRITICAL: Re-acquire ALL pointers after resize as buffer may have moved
                        header = (MaterialHeader*)m_MaterialDataBuffer.data();
                        targetBinding = (TextureBinding*)(m_MaterialDataBuffer.data() + insertPos);
                        
                        targetBinding->semantic = semantic;
                        targetBinding->samplerFilter = SamplerFilter::Bilinear;
                        targetBinding->addressU = SamplerAddressMode::Repeat;
                        targetBinding->addressV = SamplerAddressMode::Repeat;
                        targetBinding->addressW = SamplerAddressMode::Repeat;
                        targetBinding->uvScale[0] = 1.0f; targetBinding->uvScale[1] = 1.0f;
                        
                        header->textureCount++;
                        header->floatParameterOffset += sizeof(TextureBinding);
                    }
                    
                    targetBinding->textureUuid = newId;
                    changed = true;
                }
                ImGui::EndDragDropTarget();
            }

            if (targetBinding && ImGui::BeginPopupContextItem()) {
                if (ImGui::MenuItem("Clear")) {
                    TextureBinding* allBindings = (TextureBinding*)(m_MaterialDataBuffer.data() + header->textureArrayOffset);
                    int index = -1;
                    for (uint32_t i = 0; i < header->textureCount; ++i) {
                        if (&allBindings[i] == targetBinding) {
                            index = i;
                            break;
                        }
                    }

                    if (index != -1) {
                        size_t bindingOffset = header->textureArrayOffset + (index * sizeof(TextureBinding));
                        m_MaterialDataBuffer.erase(m_MaterialDataBuffer.begin() + bindingOffset, m_MaterialDataBuffer.begin() + bindingOffset + sizeof(TextureBinding));

                        // Re-acquire header after modification
                        header = (MaterialHeader*)m_MaterialDataBuffer.data();
                        header->textureCount--;
                        header->floatParameterOffset -= sizeof(TextureBinding);
                        changed = true;

                        targetBinding = nullptr;
                    }
                }
                ImGui::EndPopup();
            }

            if (targetBinding && assetExists) {
                ImGui::TableNextRow();
                ImGui::TableSetColumnIndex(1); // Indent within the second column
                
                ImGui::SetNextItemWidth(-1.0f);
                if (ImGui::DragFloat2("##UVScale", targetBinding->uvScale, 0.01f)) changed = true;
                ImGui::SameLine(); ImGui::Text("UV Scale");

                ImGui::SetNextItemWidth(-1.0f);
                if (ImGui::DragFloat2("##UVOffset", targetBinding->uvOffset, 0.01f)) changed = true;
                ImGui::SameLine(); ImGui::Text("UV Offset");
                
                const char* filters[] = { "Nearest", "Bilinear", "Trilinear", "Anisotropic" };
                int filterIdx = (int)targetBinding->samplerFilter;
                ImGui::SetNextItemWidth(-1.0f);
                if (ImGui::Combo("##Filter", &filterIdx, filters, IM_ARRAYSIZE(filters))) {
                    targetBinding->samplerFilter = (SamplerFilter)filterIdx;
                    changed = true;
                }
            }
            ImGui::EndTable();
        }
        ImGui::PopID();
    };

    drawTextureSlot("Base Map", TextureSemantic::Base);
    drawTextureSlot("Normal Map", TextureSemantic::Normal);
    drawTextureSlot("Metallic Roughness", TextureSemantic::MetallicRoughness);
    drawTextureSlot("Occlusion", TextureSemantic::Occlusion);
    drawTextureSlot("Emissive", TextureSemantic::Emissive);

    ImGui::Separator();
    
    // Material Flags
    if (ImGui::BeginTable("MaterialFlags", 2, ImGuiTableFlags_SizingStretchProp)) {
        ImGui::TableSetupColumn("Label", ImGuiTableColumnFlags_WidthFixed, 120.0f);
        ImGui::TableSetupColumn("Widget", ImGuiTableColumnFlags_WidthStretch);

        auto drawLabeledCheckbox = [&](const char* label, bool& value, auto onValueChange) {
            ImGui::TableNextRow();
            ImGui::TableSetColumnIndex(0);
            ImGui::AlignTextToFramePadding();
            ImGui::TextUnformatted(label);
            ImGui::TableSetColumnIndex(1);
            if (ImGui::Checkbox(("##" + std::string(label)).c_str(), &value)) {
                onValueChange(value);
                changed = true;
            }
        };

        bool doubleSided = (header->flags & DoubleSided) != 0;
        drawLabeledCheckbox("Double Sided", doubleSided, [&](bool val) {
            if (val) header->flags |= DoubleSided;
            else header->flags &= ~DoubleSided;
        });

        bool useVertexColor = (header->flags & UseVertexColor) != 0;
        drawLabeledCheckbox("Use Vertex Color", useVertexColor, [&](bool val) {
            if (val) header->flags |= UseVertexColor;
            else header->flags &= ~UseVertexColor;
        });

        bool receivesShadows = (header->flags & ReceivesShadows) != 0;
        drawLabeledCheckbox("Receives Shadows", receivesShadows, [&](bool val) {
            if (val) header->flags |= ReceivesShadows;
            else header->flags &= ~ReceivesShadows;
        });

        ImGui::EndTable();
    }

    if (changed) {
        std::ofstream outFile(path, std::ios::binary);
        if (outFile.is_open()) {
            outFile.write(reinterpret_cast<const char*>(m_MaterialDataBuffer.data()), static_cast<std::streamsize>(m_MaterialDataBuffer.size()));
            outFile.close();

            // Notify ResourceManager to reload this material from disk
            auto& assets = AssetDatabase::Instance().GetAssets();
            auto it = assets.find(std::filesystem::absolute(path));
            if (it != assets.end()) {
                ResourceManager::Instance().ReloadMaterial(it->second.guid);
            }
        }
    }
}

void InspectorWindow::DrawMeshInspector(const std::filesystem::path& path) {
    ImGui::Text("Mesh Asset: %s", path.filename().string().c_str());
    ImGui::Separator();

    auto& assets = AssetDatabase::Instance().GetAssets();
    auto it = assets.find(std::filesystem::absolute(path));
    if (it == assets.end()) {
        ImGui::TextColored(ImVec4(1, 1, 0, 1), "Asset not found in database.");
        return;
    }

    const AssetMetadata& meta = it->second;
    std::filesystem::path fullCompiledPath = AssetDatabase::Instance().GetBuildRoot() / meta.compiledPath;

    if (meta.compiledPath.empty() || !std::filesystem::exists(fullCompiledPath)) {
        ImGui::TextColored(ImVec4(1, 1, 0, 1), "Compiled mesh file (.temf) not found.");
        return;
    }

	MeshHeader header{};
	std::ifstream in(fullCompiledPath, std::ios::binary);
	if (in.is_open()) {
		in.read((char*)&header, sizeof(MeshHeader));
	}

	if (memcmp(header.magic, "TEMF", 4) != 0) {
		ImGui::TextColored(ImVec4(1, 0, 0, 1), "Invalid TEMF header in compiled mesh!");
		return;
	}

	// Direct replacement format requires an exact version match and full rebuild.
	if (header.version != kMeshCurrentVersion) {
		ImGui::TextColored(ImVec4(1, 0.7f, 0, 1), "Unsupported TEMF version %u.", (uint32_t)header.version);
		ImGui::TextWrapped("This build accepts only direct-replacement TEMF version %u. Recompile all mesh assets.", (uint32_t)kMeshCurrentVersion);
	}

    auto drawInfoRow = [](const char* label, const char* value) {
        ImGui::TableNextRow();
        ImGui::TableSetColumnIndex(0);
        ImGui::TextUnformatted(label);
        ImGui::TableSetColumnIndex(1);
        ImGui::TextUnformatted(value);
    };

    auto drawInfoRowU32 = [&](const char* label, uint32_t value) {
        drawInfoRow(label, std::to_string(value).c_str());
    };

	ImGui::Text("Compiled Info (.temf - meshlet only)");
    if (ImGui::BeginTable("MeshInfoTable", 2, ImGuiTableFlags_SizingStretchProp)) {
        ImGui::TableSetupColumn("Label", ImGuiTableColumnFlags_WidthFixed, 180.0f);
        ImGui::TableSetupColumn("Value", ImGuiTableColumnFlags_WidthStretch);

        drawInfoRowU32("Version", (uint32_t)header.version);
        drawInfoRowU32("Meshlet Vertices", header.meshletVertexCount);
        drawInfoRowU32("Meshlet Vertex Remap Entries", header.meshletVertexRemapCount);
        
        const char* remapEncodingLabel = "invalid";
        if (static_cast<MeshletRemapEncoding>(header.meshletVertexRemapEncoding) == MeshletRemapEncoding::Uint16) remapEncodingLabel = "u16";
        else if (static_cast<MeshletRemapEncoding>(header.meshletVertexRemapEncoding) == MeshletRemapEncoding::Uint32) remapEncodingLabel = "u32";
        drawInfoRow("Remap Encoding", remapEncodingLabel);

        drawInfoRowU32("Meshlet Primitive Indices", header.meshletPrimitiveIndexCount);
        drawInfoRowU32("Meshlets", header.meshletCount);
        drawInfoRowU32("Meshlet Sections", header.meshletSectionCount);
        drawInfoRowU32("Meshlet Bounds", header.meshletBoundsCount);
        
        char flagsBuf[16];
        snprintf(flagsBuf, sizeof(flagsBuf), "0x%08X", header.meshletFlags);
        drawInfoRow("Meshlet Flags", flagsBuf);

        ImGui::EndTable();
    }

	ImGui::Spacing();
	if (ImGui::CollapsingHeader("Meshlet Payload", ImGuiTreeNodeFlags_DefaultOpen)) {
        if (ImGui::BeginTable("MeshPayloadTable", 2, ImGuiTableFlags_SizingStretchProp)) {
            ImGui::TableSetupColumn("Label", ImGuiTableColumnFlags_WidthFixed, 180.0f);
            ImGui::TableSetupColumn("Value", ImGuiTableColumnFlags_WidthStretch);

            drawInfoRowU32("Header Size", header.headerSize);
            drawInfoRowU32("Vertex Payload Offset", header.meshletVertexDataOffset);
            drawInfoRowU32("Vertex Remap Payload Offset", header.meshletVertexRemapDataOffset);
            drawInfoRowU32("Primitive Index Payload Offset", header.meshletPrimitiveIndexDataOffset);
            drawInfoRowU32("Meshlet Table Offset", header.meshletTableOffset);
            drawInfoRowU32("Meshlet Section Offset", header.meshletSectionOffset);
            drawInfoRowU32("Meshlet Bounds Offset", header.meshletBoundsOffset);

            ImGui::EndTable();
        }
	}

	if (ImGui::CollapsingHeader("Bounds & Quantization", ImGuiTreeNodeFlags_DefaultOpen)) {
        if (ImGui::BeginTable("MeshQuantTable", 2, ImGuiTableFlags_SizingStretchProp)) {
            ImGui::TableSetupColumn("Label", ImGuiTableColumnFlags_WidthFixed, 180.0f);
            ImGui::TableSetupColumn("Value", ImGuiTableColumnFlags_WidthStretch);

            char buf[64];
            snprintf(buf, sizeof(buf), "%.3f, %.3f, %.3f", header.quantization.positionOffset[0], header.quantization.positionOffset[1], header.quantization.positionOffset[2]);
            drawInfoRow("Center", buf);
            snprintf(buf, sizeof(buf), "%.3f, %.3f, %.3f", header.quantization.positionScale[0], header.quantization.positionScale[1], header.quantization.positionScale[2]);
            drawInfoRow("Extent", buf);
            snprintf(buf, sizeof(buf), "%.3f, %.3f", header.quantization.uvOffset[0], header.quantization.uvOffset[1]);
            drawInfoRow("UV Min", buf);
            snprintf(buf, sizeof(buf), "%.3f, %.3f", header.quantization.uvScale[0], header.quantization.uvScale[1]);
            drawInfoRow("UV Scale", buf);

            ImGui::EndTable();
        }
	}
}

void InspectorWindow::DrawTextureInspector(const std::filesystem::path& path) {
    ImGui::Text("Texture Asset: %s", path.filename().string().c_str());
    ImGui::Separator();

    auto& assets = AssetDatabase::Instance().GetAssets();
    auto it = assets.find(std::filesystem::absolute(path));
    if (it == assets.end()) {
        ImGui::TextColored(ImVec4(1, 1, 0, 1), "Asset not found in database.");
        return;
    }

    if (AssetDatabase::Instance().IsAssetCompiling(std::filesystem::absolute(path))) {
        ImGui::Spacing();
        ImGui::PushStyleColor(ImGuiCol_PlotHistogram, ImVec4(0.2f, 0.6f, 0.8f, 1.0f));
        // Use a negative fraction animated by time to show an indeterminate progress bar
        ImGui::ProgressBar(-1.0f * (float)ImGui::GetTime(), ImVec2(-1, 0), "Compiling...");
        ImGui::PopStyleColor();
        return;
    }

    const AssetMetadata& meta = it->second;
    std::filesystem::path fullCompiledPath = AssetDatabase::Instance().GetBuildRoot() / meta.compiledPath;

    if (meta.compiledPath.empty() || !std::filesystem::exists(fullCompiledPath)) {
        ImGui::TextColored(ImVec4(1, 1, 0, 1), "Compiled texture file (.tetf) not found.");
        return;
    }

    TETFHeader header{};
    TETFDesc desc{};
    std::ifstream in(fullCompiledPath, std::ios::binary);
    if (in.is_open()) {
        in.read((char*)&header, sizeof(TETFHeader));
        if (header.magic == kMagic) {
            std::vector<TETFChunkEntry> chunks(header.chunkCount);
            in.seekg(header.tocOffset, std::ios::beg);
            in.read((char*)chunks.data(), chunks.size() * sizeof(TETFChunkEntry));
            
            bool foundDesc = false;
            uint32_t descFourCC = MakeFourCC('D', 'E', 'S', 'C');
            for (const auto& chunk : chunks) {
                if (chunk.id == descFourCC) {
                    in.seekg(chunk.offset, std::ios::beg);
                    in.read((char*)&desc, sizeof(TETFDesc));
                    foundDesc = true;
                    break;
                }
            }

            if (!foundDesc) {
                ImGui::TextColored(ImVec4(1, 0, 0, 1), "DESC chunk not found in TETF!");
                return;
            }
        } else {
            ImGui::TextColored(ImVec4(1, 0, 0, 1), "Invalid TETF header!");
            return;
        }
    }

	ImGui::Text("Compiled Info (.tetf)");
    if (ImGui::BeginTable("TextureInfoTable", 2, ImGuiTableFlags_SizingStretchProp)) {
        ImGui::TableSetupColumn("Label", ImGuiTableColumnFlags_WidthFixed, 120.0f);
        ImGui::TableSetupColumn("Value", ImGuiTableColumnFlags_WidthStretch);

        auto drawRow = [](const char* label, const char* value) {
            ImGui::TableNextRow();
            ImGui::TableSetColumnIndex(0);
            ImGui::TextUnformatted(label);
            ImGui::TableSetColumnIndex(1);
            ImGui::TextUnformatted(value);
        };

        char buf[64];
        snprintf(buf, sizeof(buf), "%u x %u", (uint32_t)desc.width, (uint32_t)desc.height);
        drawRow("Dimensions", buf);

        if (desc.depth > 1) drawRow("Depth", std::to_string(desc.depth).c_str());
        if (desc.layers > 1) drawRow("Layers", std::to_string(desc.layers).c_str());
        drawRow("Mips", std::to_string(desc.mipCount).c_str());

        drawRow("Format", VkFormatToString(desc.vkFormat));

        const char* semantics[] = { "Unknown", "Color", "Data" };
        if (desc.semantic < 3) drawRow("Semantic", semantics[desc.semantic]);

        ImGui::EndTable();
    }

	ImGui::Spacing();
	ImGui::Text("Import Settings");
	if (ImGui::BeginTable("TextureImportTable", 2, ImGuiTableFlags_SizingStretchProp)) {
		ImGui::TableSetupColumn("Label", ImGuiTableColumnFlags_WidthFixed, 120.0f);
		ImGui::TableSetupColumn("Widget", ImGuiTableColumnFlags_WidthStretch);

		// Mutable reference to update metadata directly in the database
		AssetMetadata& metadata = const_cast<AssetMetadata&>(it->second);
		bool settingsChanged = false;

		// UASTC Quality
		ImGui::TableNextRow();
		ImGui::TableSetColumnIndex(0);
		ImGui::TextUnformatted("UASTC Quality");
		ImGui::TableSetColumnIndex(1);
		ImGui::SetNextItemWidth(-1.0f);

		int currentQuality = 1; // Balanced default
		if (metadata.settings.contains("quality")) {
			currentQuality = metadata.settings["quality"].get<int>();
		}

		const char* qualityPresets[] = { "Low (Fast)", "Balanced", "High (Slower)", "Ultra (Very Slow)" };
		if (ImGui::Combo("##UastcQuality", &currentQuality, qualityPresets, IM_ARRAYSIZE(qualityPresets))) {
			metadata.settings["quality"] = currentQuality;
			settingsChanged = true;
		}

		// Generate Mipmaps
		ImGui::TableNextRow();
		ImGui::TableSetColumnIndex(0);
		ImGui::TextUnformatted("Generate Mipmaps");
		ImGui::TableSetColumnIndex(1);
		
		bool genMips = true;
		if (metadata.settings.contains("generateMipmaps")) {
			genMips = metadata.settings["generateMipmaps"].get<bool>();
		}
		if (ImGui::Checkbox("##GenMips", &genMips)) {
			metadata.settings["generateMipmaps"] = genMips;
			settingsChanged = true;
		}

		// Downsampling Filter
		if (genMips) {
			ImGui::TableNextRow();
			ImGui::TableSetColumnIndex(0);
			ImGui::TextUnformatted("Filter Type");
			ImGui::TableSetColumnIndex(1);
			ImGui::SetNextItemWidth(-1.0f);

			const char* filters[] = { "Box", "Triangle", "Lanczos", "Kaiser" };
			std::string currentFilter = "Lanczos";
			if (metadata.settings.contains("filterType")) {
				currentFilter = metadata.settings["filterType"].get<std::string>();
			}

			int filterIdx = 2; // Lanczos default
			for (int i = 0; i < 4; ++i) {
				if (currentFilter == filters[i]) {
					filterIdx = i;
					break;
				}
			}

			if (ImGui::Combo("##FilterType", &filterIdx, filters, IM_ARRAYSIZE(filters))) {
				metadata.settings["filterType"] = filters[filterIdx];
				settingsChanged = true;
			}
		}

		if (settingsChanged) {
			// Trigger re-import which will call CompileTexture with new settings
			AssetDatabase::Instance().ImportAsset(std::filesystem::absolute(path), true);
			
			// Notify the renderer to reload the updated asset from disk
			RenderSystem::Instance().ReloadTexture(metadata.guid);
		}

		ImGui::EndTable();
	}

	ImGui::Spacing();
	if (ImGui::Button("Reimport", ImVec2(-1, 0))) {
		AssetDatabase::Instance().ImportAsset(std::filesystem::absolute(path), true);
		RenderSystem::Instance().ReloadTexture(it->second.guid);
	}
}
