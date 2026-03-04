#include "InspectorWindow.h"
#include "../Subsystems/MainEditorSubsystem.h"
#include "../Subsystems/UndoRedoSystem.h"
#include "Engine/Core/Object/ObjectManager.h"
#include "Engine/Core/Object/Component.h"
#include "Engine/Resource/ResourceManager.h"
#include "Engine/Scene/LightComponent.h"
#include "AssetDatabase.h"
#include "Engine/File Formats/TerraEngineMaterialFile.h"
#include "Engine/File Formats/TerraEngineMeshFile.h"
#include "Engine/File Formats/TerraEngineTextureFile.h"
#include <imgui.h>
#include <glm/geometric.hpp>
#include <glm/vec3.hpp>
#include <glm/gtc/quaternion.hpp>
#include <glm/trigonometric.hpp>
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <fstream>
#include <filesystem>
#include <limits>

namespace
{
// Convert internal registry keys like "LightComponent" to user-facing names.
// This keeps serialization/component lookup stable while matching editor UX labels.
std::string BuildDisplayComponentName(const std::string& componentName)
{
	if (componentName == "LightComponent")
	{
		return "Light";
	}

	const std::string componentSuffix = "Component";
	if (componentName.size() > componentSuffix.size()
		&& componentName.compare(componentName.size() - componentSuffix.size(), componentSuffix.size(), componentSuffix) == 0)
	{
		return componentName.substr(0, componentName.size() - componentSuffix.size());
	}

	return componentName;
}
}

// Command that records a property change on a component. Captures the 
// previous and next values to allow bidirectional state switching.
class ModifyComponentCommand : public ICommand {
public:
    ModifyComponentCommand(entt::entity entity, const std::string& component, const std::string& property, rttr::variant oldVal, rttr::variant newVal)
        : m_Entity(entity), m_Component(component), m_Property(property), m_OldValue(oldVal), m_NewValue(newVal) {}

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
    void ApplyValue(const rttr::variant& val) {
        if (auto obj = ObjectManager::instance().getObject(m_Entity)) {
            auto& reg = ComponentAccessRegistry();
            if (reg.count(m_Component)) {
                // Setters handle thread-safety internally via component mutexes.
                reg[m_Component].setter(*obj, m_Property, val);
            }
        }
    }

    entt::entity m_Entity;
    std::string m_Component;
    std::string m_Property;
    rttr::variant m_OldValue;
    rttr::variant m_NewValue;
};

InspectorWindow::InspectorWindow(MainEditorSubsystem* owner)
    : EditorWindow("Inspector"), m_Owner(owner) {
}

void InspectorWindow::UpdateInspectorDragState() {
	if (ImGui::IsItemActive() && ImGui::IsMouseDragging(ImGuiMouseButton_Left)) {
		m_InspectorValueDragging = true;
	}
}

void InspectorWindow::DrawProperty(const std::string& compName, rttr::property prop, Object& obj, ComponentAccessFunctions& access) {
	std::string name = prop.get_name().to_string();
	rttr::variant var = access.getter(obj, name);
	rttr::type type = prop.get_type();

    // Static buffers track interaction state across multiple frames while a 
    // widget is being manipulated.
    static rttr::variant oldValue;
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
            rttr::variant newVal = access.getter(obj, name);
            UndoRedoSystem::Instance().PushCommand(std::make_unique<ModifyComponentCommand>(obj.EntityId(), compName, name, oldValue, newVal));
            isEditing = false;
        }
    };

	if (type == rttr::type::get<float>()) {
		float val = var.get_value<float>();
		if (ImGui::DragFloat(name.c_str(), &val, 0.1f)) {
            if (compName == "CameraComponent") {
                if (name == "nearPlane") {
                    if (val < 0.001f) val = 0.001f;
                    
                    float farP = access.getter(obj, "farPlane").get_value<float>();
                    if (farP < val + 0.001f) {
                        access.setter(obj, "farPlane", val + 0.001f);
                    }
                }
                else if (name == "farPlane") {
                    float nearP = access.getter(obj, "nearPlane").get_value<float>();
                    if (val < nearP + 0.001f) val = nearP + 0.001f;
                }
            }
			access.setter(obj, name, val);
		}
        startEditing();
        endEditing();
		UpdateInspectorDragState();
	}
	else if (type == rttr::type::get<int>()) {
		int val = var.get_value<int>();
		if (ImGui::DragInt(name.c_str(), &val)) {
			access.setter(obj, name, val);
		}
        startEditing();
        endEditing();
		UpdateInspectorDragState();
	}
	else if (type == rttr::type::get<std::uint32_t>()) {
		std::uint32_t val = var.get_value<std::uint32_t>();
		int signedValue = static_cast<int>(val);
		if (ImGui::DragInt(name.c_str(), &signedValue, 1.0f, 0, std::numeric_limits<int>::max())) {
			const std::uint32_t clampedValue = static_cast<std::uint32_t>(std::max(0, signedValue));
			access.setter(obj, name, clampedValue);
		}
		startEditing();
		endEditing();
		UpdateInspectorDragState();
	}
	else if (type == rttr::type::get<bool>()) {
		bool val = var.get_value<bool>();
		if (ImGui::Checkbox(name.c_str(), &val)) {
            // Checkboxes are instantaneous interactions, so we push the 
            // command immediately.
            rttr::variant oldB = access.getter(obj, name);
			access.setter(obj, name, val);
            rttr::variant newB = access.getter(obj, name);
            UndoRedoSystem::Instance().PushCommand(std::make_unique<ModifyComponentCommand>(obj.EntityId(), compName, name, oldB, newB));
		}
	}
	else if (type == rttr::type::get<std::string>()) {
		std::string val = var.get_value<std::string>();
		char buffer[256];
		strncpy(buffer, val.c_str(), sizeof(buffer));
		if (ImGui::InputText(name.c_str(), buffer, sizeof(buffer))) {
			access.setter(obj, name, std::string(buffer));
		}
        startEditing();
        endEditing();
	}
	else if (type == rttr::type::get<glm::vec3>()) {
		glm::vec3 val = var.get_value<glm::vec3>();
		if (ImGui::DragFloat3(name.c_str(), &val.x, 0.1f)) {
			access.setter(obj, name, val);
		}
        startEditing();
        endEditing();
		UpdateInspectorDragState();
	}
	else if (type == rttr::type::get<glm::quat>()) {
		glm::quat q = var.get_value<glm::quat>();
		glm::vec3 euler = glm::degrees(glm::eulerAngles(q));
		if (ImGui::DragFloat3(name.c_str(), &euler.x, 0.1f)) {
			glm::quat newQ = glm::quat(glm::radians(euler));
			access.setter(obj, name, newQ);
		}
        startEditing();
        endEditing();
		UpdateInspectorDragState();
	}
	else if (type == rttr::type::get<TerraEngine::UUID>()) {
		TerraEngine::UUID val = var.get_value<TerraEngine::UUID>();
		std::string label = "None";
		if (val != TerraEngine::EMPTY_UUID) {
			const auto& assets = AssetDatabase::Instance().GetAssets();
			bool found = false;
			for (const auto& [path, meta] : assets) {
				if (meta.guid == val) {
					label = path.stem().string();
					found = true;
					break;
				}
			}
			if (!found) label = "Unknown Asset";
		}

		char buf[128];
		strncpy(buf, label.c_str(), sizeof(buf));
		ImGui::SetNextItemWidth(ImGui::GetContentRegionAvail().x - 30);
		ImGui::InputText(name.c_str(), buf, sizeof(buf), ImGuiInputTextFlags_ReadOnly);

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
                    rttr::variant oldId = access.getter(obj, name);
					access.setter(obj, name, newId);
                    rttr::variant nextId = access.getter(obj, name);
                    UndoRedoSystem::Instance().PushCommand(std::make_unique<ModifyComponentCommand>(obj.EntityId(), compName, name, oldId, nextId));
				}
			} else {
				// Generic UUID property, accept any asset payload
				for (const char* p : payloads) {
					if (const ImGuiPayload* payload = ImGui::AcceptDragDropPayload(p)) {
						IM_ASSERT(payload->DataSize == sizeof(TerraEngine::UUID));
						TerraEngine::UUID newId = *(const TerraEngine::UUID*)payload->Data;
                        rttr::variant oldId = access.getter(obj, name);
						access.setter(obj, name, newId);
                        rttr::variant nextId = access.getter(obj, name);
                        UndoRedoSystem::Instance().PushCommand(std::make_unique<ModifyComponentCommand>(obj.EntityId(), compName, name, oldId, nextId));
						break;
					}
				}
			}
			ImGui::EndDragDropTarget();
		}

		ImGui::SameLine();
		if (ImGui::Button("X", ImVec2(24, 0))) {
			rttr::variant oldId = access.getter(obj, name);
			access.setter(obj, name, TerraEngine::EMPTY_UUID);
			rttr::variant nextId = access.getter(obj, name);
			UndoRedoSystem::Instance().PushCommand(std::make_unique<ModifyComponentCommand>(obj.EntityId(), compName, name, oldId, nextId));
		}
	}
}

void InspectorWindow::DrawLightComponentInspector(const std::string& compName, Object& obj, ComponentAccessFunctions& access)
{
	// Keep undo/redo granular by committing only fields that changed through
	// this custom inspector layout.
	auto setFloatPropertyWithUndo = [&](const char* propertyName, float newValue) {
		rttr::variant oldVariant = access.getter(obj, propertyName);
		if (!oldVariant.is_valid() || !oldVariant.is_type<float>())
		{
			return;
		}

		const float oldValue = oldVariant.get_value<float>();
		if (std::abs(oldValue - newValue) <= 0.0001f)
		{
			return;
		}

		access.setter(obj, propertyName, newValue);
		rttr::variant nextVariant = access.getter(obj, propertyName);
		UndoRedoSystem::Instance().PushCommand(std::make_unique<ModifyComponentCommand>(obj.EntityId(), compName, propertyName, oldVariant, nextVariant));
	};

	auto setBoolPropertyWithUndo = [&](const char* propertyName, bool newValue) {
		rttr::variant oldVariant = access.getter(obj, propertyName);
		if (!oldVariant.is_valid() || !oldVariant.is_type<bool>())
		{
			return;
		}

		const bool oldValue = oldVariant.get_value<bool>();
		if (oldValue == newValue)
		{
			return;
		}

		access.setter(obj, propertyName, newValue);
		rttr::variant nextVariant = access.getter(obj, propertyName);
		UndoRedoSystem::Instance().PushCommand(std::make_unique<ModifyComponentCommand>(obj.EntityId(), compName, propertyName, oldVariant, nextVariant));
	};

	auto setUintPropertyWithUndo = [&](const char* propertyName, std::uint32_t newValue) {
		rttr::variant oldVariant = access.getter(obj, propertyName);
		if (!oldVariant.is_valid() || !oldVariant.is_type<std::uint32_t>())
		{
			return;
		}

		const std::uint32_t oldValue = oldVariant.get_value<std::uint32_t>();
		if (oldValue == newValue)
		{
			return;
		}

		access.setter(obj, propertyName, newValue);
		rttr::variant nextVariant = access.getter(obj, propertyName);
		UndoRedoSystem::Instance().PushCommand(std::make_unique<ModifyComponentCommand>(obj.EntityId(), compName, propertyName, oldVariant, nextVariant));
	};

	auto setVec3PropertyWithUndo = [&](const char* propertyName, const glm::vec3& newValue) {
		rttr::variant oldVariant = access.getter(obj, propertyName);
		if (!oldVariant.is_valid() || !oldVariant.is_type<glm::vec3>())
		{
			return;
		}

		const glm::vec3 oldValue = oldVariant.get_value<glm::vec3>();
		if (glm::length(oldValue - newValue) <= 0.0001f)
		{
			return;
		}

		access.setter(obj, propertyName, newValue);
		rttr::variant nextVariant = access.getter(obj, propertyName);
		UndoRedoSystem::Instance().PushCommand(std::make_unique<ModifyComponentCommand>(obj.EntityId(), compName, propertyName, oldVariant, nextVariant));
	};

	constexpr std::uint32_t kMaxAuthorableType = static_cast<std::uint32_t>(LightType::Area);
	std::uint32_t encodedType = static_cast<std::uint32_t>(LightType::Point);
	if (rttr::variant typeVariant = access.getter(obj, "type"); typeVariant.is_valid() && typeVariant.is_type<std::uint32_t>())
	{
		encodedType = typeVariant.get_value<std::uint32_t>();
	}
	const bool hasUnsupportedType = encodedType > kMaxAuthorableType;
	std::uint32_t effectiveType = hasUnsupportedType ? static_cast<std::uint32_t>(LightType::Point) : encodedType;

	const bool isDirectional = effectiveType == static_cast<std::uint32_t>(LightType::Directional);
	const bool isSpot = effectiveType == static_cast<std::uint32_t>(LightType::Spot);
	const bool isArea = effectiveType == static_cast<std::uint32_t>(LightType::Area);

	const ImGuiTreeNodeFlags sectionFlags = ImGuiTreeNodeFlags_DefaultOpen;
	if (ImGui::CollapsingHeader("General", sectionFlags))
	{
		const char* lightTypes[] = { "Directional", "Point", "Spot", "Area" };
		if (hasUnsupportedType)
		{
			ImGui::TextColored(ImVec4(1.0f, 0.72f, 0.24f, 1.0f), "Unsupported legacy light type value: %u", encodedType);
			ImGui::TextUnformatted("Select a supported type to replace it.");
		}

		int typeIndex = hasUnsupportedType
			? static_cast<int>(LightType::Point)
			: static_cast<int>(effectiveType);
		if (ImGui::Combo("Type", &typeIndex, lightTypes, IM_ARRAYSIZE(lightTypes)))
		{
			typeIndex = std::clamp(typeIndex, 0, static_cast<int>(kMaxAuthorableType));
			effectiveType = static_cast<std::uint32_t>(typeIndex);
			setUintPropertyWithUndo("type", effectiveType);
		}

		std::uint32_t currentAppearance = 0u;
		if (rttr::variant appVariant = access.getter(obj, "appearance"); appVariant.is_valid() && appVariant.is_type<std::uint32_t>())
		{
			currentAppearance = appVariant.get_value<std::uint32_t>();
		}

		const char* appearanceTypes[] = { "Color", "Filter and Temperature" };
		int appIndex = static_cast<int>(currentAppearance);
		if (ImGui::Combo("Appearance", &appIndex, appearanceTypes, IM_ARRAYSIZE(appearanceTypes)))
		{
			setUintPropertyWithUndo("appearance", static_cast<std::uint32_t>(appIndex));
		}

		glm::vec3 lightColor = glm::vec3(1.0f);
		if (rttr::variant colorVariant = access.getter(obj, "color"); colorVariant.is_valid() && colorVariant.is_type<glm::vec3>())
		{
			lightColor = colorVariant.get_value<glm::vec3>();
		}
		if (ImGui::ColorEdit3(currentAppearance == 0u ? "Color" : "Filter", &lightColor.x))
		{
			setVec3PropertyWithUndo("color", lightColor);
		}

		if (currentAppearance == 1u)
		{
			float temperature = 6500.0f;
			if (rttr::variant tempVariant = access.getter(obj, "temperature"); tempVariant.is_valid() && tempVariant.is_type<float>())
			{
				temperature = tempVariant.get_value<float>();
			}
			if (ImGui::SliderFloat("Temperature", &temperature, 1000.0f, 20000.0f, "%.0f K"))
			{
				setFloatPropertyWithUndo("temperature", temperature);
			}
		}

		float intensity = 1.0f;
		if (rttr::variant intensityVariant = access.getter(obj, "intensity"); intensityVariant.is_valid() && intensityVariant.is_type<float>())
		{
			intensity = intensityVariant.get_value<float>();
		}

		std::uint32_t currentUnit = 0u;
		if (rttr::variant unitVariant = access.getter(obj, "unit"); unitVariant.is_valid() && unitVariant.is_type<std::uint32_t>())
		{
			currentUnit = unitVariant.get_value<std::uint32_t>();
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

		if (ImGui::DragFloat("Intensity", &intensity, 0.05f, 0.0f, 1000000.0f, intensityFormat))
		{
			setFloatPropertyWithUndo("intensity", std::max(0.0f, intensity));
		}
		UpdateInspectorDragState();

		if (!isDirectional)
		{
			const char* lightUnits[] = { "Lumens", "Lux" };
			int unitIndex = std::clamp(static_cast<int>(currentUnit), 0, 1);
			if (ImGui::Combo("Unit", &unitIndex, lightUnits, IM_ARRAYSIZE(lightUnits)))
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
		}

		float indirectMultiplier = 1.0f;
		if (rttr::variant indVariant = access.getter(obj, "indirectMultiplier"); indVariant.is_valid() && indVariant.is_type<float>())
		{
			indirectMultiplier = indVariant.get_value<float>();
		}
		if (ImGui::DragFloat("Indirect Multiplier", &indirectMultiplier, 0.05f, 0.0f, 10.0f))
		{
			setFloatPropertyWithUndo("indirectMultiplier", std::max(0.0f, indirectMultiplier));
		}
		UpdateInspectorDragState();
	}

	if (ImGui::CollapsingHeader("Shape", sectionFlags))
	{
		if (!isDirectional)
		{
			float range = 10.0f;
			if (rttr::variant rangeVariant = access.getter(obj, "range"); rangeVariant.is_valid() && rangeVariant.is_type<float>())
			{
				range = rangeVariant.get_value<float>();
			}
			if (ImGui::DragFloat("Range", &range, 0.1f, 0.1f, 10000.0f, "%.2f m"))
			{
				setFloatPropertyWithUndo("range", std::max(0.1f, range));
			}
			UpdateInspectorDragState();
		}

		if (isSpot)
		{
			float innerDegrees = 20.0f;
			float outerDegrees = 30.0f;
			if (rttr::variant innerVariant = access.getter(obj, "innerConeAngle"); innerVariant.is_valid() && innerVariant.is_type<float>())
			{
				innerDegrees = glm::degrees(innerVariant.get_value<float>());
			}
			if (rttr::variant outerVariant = access.getter(obj, "outerConeAngle"); outerVariant.is_valid() && outerVariant.is_type<float>())
			{
				outerDegrees = glm::degrees(outerVariant.get_value<float>());
			}

			const bool innerEdited = ImGui::DragFloat("Inner Angle", &innerDegrees, 0.1f, 0.0f, 89.0f, "%.1f deg");
			UpdateInspectorDragState();
			const bool outerEdited = ImGui::DragFloat("Outer Angle", &outerDegrees, 0.1f, 0.1f, 89.9f, "%.1f deg");
			UpdateInspectorDragState();
			if (innerEdited || outerEdited)
			{
				innerDegrees = std::clamp(innerDegrees, 0.0f, 89.0f);
				outerDegrees = std::clamp(outerDegrees, innerDegrees + 0.1f, 89.9f);
				setFloatPropertyWithUndo("innerConeAngle", glm::radians(innerDegrees));
				setFloatPropertyWithUndo("outerConeAngle", glm::radians(outerDegrees));
			}
		}

		if (isArea)
		{
			float width = 1.0f;
			float height = 1.0f;
			if (rttr::variant wVariant = access.getter(obj, "width"); wVariant.is_valid() && wVariant.is_type<float>())
			{
				width = wVariant.get_value<float>();
			}
			if (rttr::variant hVariant = access.getter(obj, "height"); hVariant.is_valid() && hVariant.is_type<float>())
			{
				height = hVariant.get_value<float>();
			}

			const bool wEdited = ImGui::DragFloat("Width", &width, 0.1f, 0.01f, 1000.0f, "%.2f m");
			UpdateInspectorDragState();
			const bool hEdited = ImGui::DragFloat("Height", &height, 0.1f, 0.01f, 1000.0f, "%.2f m");
			UpdateInspectorDragState();

			if (wEdited) setFloatPropertyWithUndo("width", std::max(0.01f, width));
			if (hEdited) setFloatPropertyWithUndo("height", std::max(0.01f, height));
		}
	}

	if (ImGui::CollapsingHeader("Shadows", sectionFlags))
	{
		bool castsShadows = false;
		if (rttr::variant castVariant = access.getter(obj, "castsShadows"); castVariant.is_valid() && castVariant.is_type<bool>())
		{
			castsShadows = castVariant.get_value<bool>();
		}
		if (ImGui::Checkbox("Realtime Shadows", &castsShadows))
		{
			setBoolPropertyWithUndo("castsShadows", castsShadows);
		}

		if (castsShadows)
		{
			float shadowStrength = 1.0f;
			if (rttr::variant strVariant = access.getter(obj, "shadowStrength"); strVariant.is_valid() && strVariant.is_type<float>())
			{
				shadowStrength = strVariant.get_value<float>();
			}
			if (ImGui::SliderFloat("Strength", &shadowStrength, 0.0f, 1.0f))
			{
				setFloatPropertyWithUndo("shadowStrength", std::clamp(shadowStrength, 0.0f, 1.0f));
			}

			float shadowBias = 0.005f;
			if (rttr::variant biasVariant = access.getter(obj, "shadowBias"); biasVariant.is_valid() && biasVariant.is_type<float>())
			{
				shadowBias = biasVariant.get_value<float>();
			}
			if (ImGui::DragFloat("Bias", &shadowBias, 0.001f, 0.0f, 0.1f, "%.4f"))
			{
				setFloatPropertyWithUndo("shadowBias", std::clamp(shadowBias, 0.0f, 0.1f));
			}
			UpdateInspectorDragState();
		}
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
        } else if (ext == ".obj" || ext == ".fbx") {
            DrawMeshInspector(*selectedAsset);
        } else if (ext == ".png" || ext == ".jpg" || ext == ".tga" || ext == ".bmp") {
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
            
            bool isActive = true;
            auto& accessReg = ComponentAccessRegistry();
            if (accessReg.find("Transform") != accessReg.end()) {
                ComponentAccessFunctions& transAccess = accessReg["Transform"];
                if (transAccess.hasComponent(obj)) {
                    rttr::variant var = transAccess.getter(obj, "active");
                    if (var.is_valid()) isActive = var.get_value<bool>();
                }
            }

            ImGui::PushID("InspectorHeader");
            if (ImGui::Checkbox("##ActiveGlobal", &isActive)) {
                if (accessReg.find("Transform") != accessReg.end()) {
                    ComponentAccessFunctions& transAccess = accessReg["Transform"];
                    if (transAccess.hasComponent(obj)) {
                        rttr::variant oldA = transAccess.getter(obj, "active");
                        transAccess.setter(obj, "active", isActive);
                        rttr::variant newA = transAccess.getter(obj, "active");
                        UndoRedoSystem::Instance().PushCommand(std::make_unique<ModifyComponentCommand>(obj.EntityId(), "Transform", "active", oldA, newA));
                    }
                }
            }
            ImGui::PopID();
            
            ImGui::SameLine();
            
            std::string name = objManager.getObjectName(obj);
            char nameBuf[256];
            strncpy(nameBuf, name.c_str(), sizeof(nameBuf));
            ImGui::SetNextItemWidth(ImGui::GetContentRegionAvail().x);
            if (ImGui::InputText("##ObjectName", nameBuf, sizeof(nameBuf))) {
                if (name != nameBuf) {
                    UndoRedoSystem::Instance().PushCommand(std::make_unique<RenameObjectCommand>(obj.EntityId(), name, nameBuf));
                    objManager.setObjectName(obj, nameBuf);
                }
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
				const std::string displayName = BuildDisplayComponentName(compName);

                if (!access.hasComponent(obj)) continue; 

                ImGui::PushID(compName.c_str());
                
                bool compActive = true;
                rttr::type type = rttr::type::get_by_name(compName);
                rttr::property activeProp = type.get_property("active");
                if (activeProp.is_valid()) {
                    rttr::variant var = access.getter(obj, "active");
                    if (var.is_valid()) compActive = var.get_value<bool>();
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
                        if (activeProp.is_valid()) {
                            rttr::variant oldA = access.getter(obj, "active");
                            access.setter(obj, "active", compActive);
                            rttr::variant newA = access.getter(obj, "active");
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
					else
					{
						for (auto prop : type.get_properties()) {
							if (prop.get_name() == "active") continue;
							DrawProperty(compName, prop, obj, access);
						}
					}
                    ImGui::Unindent();
                }
                ImGui::Separator();
                ImGui::PopID();
            }
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
                    if (!access.hasComponent(obj)) {
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

    // Shading Model
    const char* shadingModels[] = { "Unlit", "MetallicRoughness", "SpecularGlossiness" };
    int shadingModelIdx = (int)header->shadingModel;
    if (ImGui::Combo("Shading Model", &shadingModelIdx, shadingModels, IM_ARRAYSIZE(shadingModels))) {
        header->shadingModel = (uint32_t)shadingModelIdx;
        changed = true;
    }

    // Alpha Mode
    const char* alphaModes[] = { "Opaque", "Mask", "Blend" };
    int alphaModeIdx = (int)header->alphaMode;
    if (ImGui::Combo("Alpha Mode", &alphaModeIdx, alphaModes, IM_ARRAYSIZE(alphaModes))) {
        header->alphaMode = (uint32_t)alphaModeIdx;
        changed = true;
    }

    if (header->alphaMode == (uint32_t)AlphaMode::Mask) {
        if (ImGui::DragFloat("Alpha Cutoff", &header->alphaCutoff, 0.01f, 0.0f, 1.0f)) changed = true;
    }

    ImGui::Separator();
    
    // Shader Collection
    {
        TerraEngine::UUID currentShaderId = header->shaderCollectionId;
        std::string shaderName = "None";
        if (currentShaderId != TerraEngine::EMPTY_UUID) {
            for (const auto& [path, meta] : AssetDatabase::Instance().GetAssets()) {
                if (meta.guid == currentShaderId) {
                    shaderName = path.filename().string();
                    break;
                }
            }
            if (shaderName == "None" && currentShaderId == ResourceManager::Instance().GetDefaultShaderCollectionID()) {
                shaderName = "Default Shader";
            }
        }

        ImGui::TextUnformatted("Shader");
        ImGui::SameLine(ImGui::GetContentRegionAvail().x - 150);
        char buf[128];
        strncpy(buf, shaderName.c_str(), sizeof(buf));
        ImGui::SetNextItemWidth(150);
        ImGui::InputText("##shader", buf, sizeof(buf), ImGuiInputTextFlags_ReadOnly);

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
    }

    ImGui::Separator();

    if (ImGui::ColorEdit4("Base Color", header->baseColorFactor)) changed = true;
    if (ImGui::DragFloat("Metallic", &header->metallicFactor, 0.01f, 0.0f, 1.0f)) changed = true;
    if (ImGui::DragFloat("Roughness", &header->roughnessFactor, 0.01f, 0.0f, 1.0f)) changed = true;
    
    if (ImGui::ColorEdit3("Emissive", header->emissiveFactor)) changed = true;
    
    if (ImGui::DragFloat("Normal Scale", &header->normalScale, 0.01f)) changed = true;
    if (ImGui::DragFloat("Occlusion", &header->occlusionStrength, 0.01f, 0.0f, 1.0f)) changed = true;

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
        if (currentId != TerraEngine::EMPTY_UUID) {
            for (const auto& [path, meta] : AssetDatabase::Instance().GetAssets()) {
                if (meta.guid == currentId) {
                    assetName = path.filename().string();
                    break;
                }
            }
        }

        ImGui::PushID(label);
        ImGui::TextUnformatted(label);
        ImGui::SameLine(ImGui::GetContentRegionAvail().x - 150);
        
        char buf[128];
        strncpy(buf, assetName.c_str(), sizeof(buf));
        ImGui::SetNextItemWidth(150);
        ImGui::InputText("##tex", buf, sizeof(buf), ImGuiInputTextFlags_ReadOnly);

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

        if (targetBinding) {
            ImGui::Indent();
            if (ImGui::DragFloat2("UV Scale", targetBinding->uvScale, 0.01f)) changed = true;
            if (ImGui::DragFloat2("UV Offset", targetBinding->uvOffset, 0.01f)) changed = true;
            
            const char* filters[] = { "Nearest", "Bilinear", "Trilinear", "Anisotropic" };
            int filterIdx = (int)targetBinding->samplerFilter;
            if (ImGui::Combo("Filter", &filterIdx, filters, IM_ARRAYSIZE(filters))) {
                targetBinding->samplerFilter = (SamplerFilter)filterIdx;
                changed = true;
            }
            ImGui::Unindent();
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
    bool doubleSided = (header->flags & DoubleSided) != 0;
    if (ImGui::Checkbox("Double Sided", &doubleSided)) {
        if (doubleSided) header->flags |= DoubleSided;
        else header->flags &= ~DoubleSided;
        changed = true;
    }

    bool useVertexColor = (header->flags & UseVertexColor) != 0;
    if (ImGui::Checkbox("Use Vertex Color", &useVertexColor)) {
        if (useVertexColor) header->flags |= UseVertexColor;
        else header->flags &= ~UseVertexColor;
        changed = true;
    }

    bool receivesShadows = (header->flags & ReceivesShadows) != 0;
    if (ImGui::Checkbox("Receives Shadows", &receivesShadows)) {
        if (receivesShadows) header->flags |= ReceivesShadows;
        else header->flags &= ~ReceivesShadows;
        changed = true;
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

	const MeshletRemapEncoding remapEncoding = static_cast<MeshletRemapEncoding>(header.meshletVertexRemapEncoding);
	const bool remapEncodingValid = remapEncoding == MeshletRemapEncoding::Uint16 || remapEncoding == MeshletRemapEncoding::Uint32;
	const uint32_t remapElementBytes = remapEncodingValid ? GetRemapElementSizeBytes(remapEncoding) : 0u;
	const bool boundsFlagSet = (header.meshletFlags & kMeshletHeaderFlagHasBounds) != 0u;
	const bool boundsMetadataConsistent = boundsFlagSet
		? (header.meshletBoundsCount == header.meshletCount && header.meshletBoundsOffset != 0u)
		: (header.meshletBoundsCount == 0u && header.meshletBoundsOffset == 0u);

	ImGui::Text("Compiled Info (.temf - meshlet only)");
	ImGui::Indent();
	ImGui::Text("Version: %u", (uint32_t)header.version);
	ImGui::Text("Meshlet Vertices: %u", header.meshletVertexCount);
	ImGui::Text("Meshlet Vertex Remap Entries: %u", header.meshletVertexRemapCount);
	const char* remapEncodingLabel = "invalid";
	if (remapEncoding == MeshletRemapEncoding::Uint16) {
		remapEncodingLabel = "u16";
	}
	else if (remapEncoding == MeshletRemapEncoding::Uint32) {
		remapEncodingLabel = "u32";
	}
	ImGui::Text("Remap Encoding: %s", remapEncodingLabel);
	ImGui::Text("Remap Encoded Bytes: %llu", (unsigned long long)header.meshletVertexRemapCount * (unsigned long long)remapElementBytes);
	ImGui::Text("Remap Decoded Bytes (runtime): %llu", (unsigned long long)header.meshletVertexRemapCount * (unsigned long long)sizeof(uint32_t));
	ImGui::Text("Meshlet Primitive Indices: %u", header.meshletPrimitiveIndexCount);
	ImGui::Text("Meshlets: %u", header.meshletCount);
	ImGui::Text("Meshlet Sections: %u", header.meshletSectionCount);
	ImGui::Text("Meshlet Bounds: %u", header.meshletBoundsCount);
	ImGui::Text("Meshlet Flags: 0x%08X", header.meshletFlags);
	ImGui::Unindent();

	ImGui::Spacing();
	if (ImGui::CollapsingHeader("Meshlet Payload", ImGuiTreeNodeFlags_DefaultOpen)) {
		ImGui::Indent();
		ImGui::Text("Header Size: %u", header.headerSize);
		ImGui::Text("Vertex Payload Offset: %u", header.meshletVertexDataOffset);
		ImGui::Text("Vertex Remap Payload Offset: %u", header.meshletVertexRemapDataOffset);
		ImGui::Text("Primitive Index Payload Offset: %u", header.meshletPrimitiveIndexDataOffset);
		ImGui::Text("Meshlet Table Offset: %u", header.meshletTableOffset);
		ImGui::Text("Meshlet Section Offset: %u", header.meshletSectionOffset);
		ImGui::Text("Meshlet Bounds Offset: %u", header.meshletBoundsOffset);
		if (!remapEncodingValid) {
			ImGui::TextColored(ImVec4(1, 0, 0, 1), "Remap encoding metadata is invalid.");
		}
		if (!boundsMetadataConsistent) {
			ImGui::TextColored(ImVec4(1, 0.7f, 0, 1), "Bounds flag/count/offset metadata is inconsistent.");
		}
		ImGui::Unindent();
	}

	if (ImGui::CollapsingHeader("Bounds & Quantization", ImGuiTreeNodeFlags_DefaultOpen)) {
		ImGui::Indent();
		ImGui::Text("Center: %.3f, %.3f, %.3f", header.quantization.positionOffset[0], header.quantization.positionOffset[1], header.quantization.positionOffset[2]);
		ImGui::Text("Extent: %.3f, %.3f, %.3f", header.quantization.positionScale[0], header.quantization.positionScale[1], header.quantization.positionScale[2]);
		ImGui::Text("UV Min: %.3f, %.3f", header.quantization.uvOffset[0], header.quantization.uvOffset[1]);
		ImGui::Text("UV Scale: %.3f, %.3f", header.quantization.uvScale[0], header.quantization.uvScale[1]);
		if (header.meshletBoundsCount == 0) {
			ImGui::TextColored(ImVec4(1, 1, 0, 1), "No meshlet bounds payload is present.");
		}
		ImGui::Unindent();
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
            // Read TETFDesc from TOC
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
    ImGui::Indent();
    ImGui::Text("Dimensions: %u x %u", (uint32_t)desc.width, (uint32_t)desc.height);
    if (desc.depth > 1) ImGui::Text("Depth: %u", (uint32_t)desc.depth);
    if (desc.layers > 1) ImGui::Text("Layers: %u", (uint32_t)desc.layers);
    ImGui::Text("Mips: %u", (uint32_t)desc.mipCount);
    
    const char* formats[] = { "RGBA8", "BGRA8" }; // Simplified, should match TETFFormat
    std::string formatStr = "Unknown";
    if (desc.format == (uint8_t)TETFFormat::RGBA8) formatStr = "RGBA8";
    else if (desc.format == (uint8_t)TETFFormat::BGRA8) formatStr = "BGRA8";
    else if (desc.format == (uint8_t)TETFFormat::BC1) formatStr = "BC1 (DXT1)";
    else if (desc.format == (uint8_t)TETFFormat::BC3) formatStr = "BC3 (DXT5)";
    else if (desc.format == (uint8_t)TETFFormat::BC4) formatStr = "BC4";
    else if (desc.format == (uint8_t)TETFFormat::BC5) formatStr = "BC5";
    else if (desc.format == (uint8_t)TETFFormat::BC6H) formatStr = "BC6H";
    else if (desc.format == (uint8_t)TETFFormat::BC7) formatStr = "BC7";
    
    ImGui::Text("Format: %s", formatStr.c_str());
    
    const char* channelTypes[] = { "UNORM", "SNORM", "UINT", "SINT", "FLOAT" };
    if (desc.channelType < 5) ImGui::Text("Type: %s", channelTypes[desc.channelType]);

    const char* semantics[] = { "Unknown", "Color", "Data" };
    if (desc.semantic < 3) ImGui::Text("Semantic: %s", semantics[desc.semantic]);

    ImGui::Unindent();
}
