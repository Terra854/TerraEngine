#include "InspectorWindow.h"
#include "../Subsystems/MainEditorSubsystem.h"
#include "../Subsystems/UndoRedoSystem.h"
#include "Engine/Core/Object/ObjectManager.h"
#include "Engine/Core/Object/Component.h"
#include "Engine/Resource/ResourceManager.h"
#include "Engine/Renderer/RenderSystem.h"
#include "Engine/Scene/LightComponent.h"
#include "AssetDatabase.h"
#include "Editor/AssetCompilers/MeshCompiler.h"
#include "Editor/AssetCompilers/TextureCompiler.h"
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

    if (!var.is_valid()) return;

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

    ImGui::TableNextRow();
    ImGui::TableSetColumnIndex(0);
    ImGui::AlignTextToFramePadding();
    ImGui::TextUnformatted(name.c_str());
    ImGui::TableSetColumnIndex(1);

	if (type == rttr::type::get<float>()) {
		float val = var.get_value<float>();
        ImGui::SetNextItemWidth(-1.0f);
		if (ImGui::DragFloat(("##" + name).c_str(), &val, 0.1f)) {
            if (compName == "CameraComponent") {
                if (name == "nearPlane") {
                    if (val < 0.001f) val = 0.001f;
                    
                    rttr::variant farVar = access.getter(obj, "farPlane");
                    if (farVar.is_valid()) {
                        float farP = farVar.get_value<float>();
                        if (farP < val + 0.001f) {
                            access.setter(obj, "farPlane", val + 0.001f);
                        }
                    }
                }
                else if (name == "farPlane") {
                    rttr::variant nearVar = access.getter(obj, "nearPlane");
                    if (nearVar.is_valid()) {
                        float nearP = nearVar.get_value<float>();
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
	else if (type == rttr::type::get<int>()) {
		int val = var.get_value<int>();
        ImGui::SetNextItemWidth(-1.0f);
		if (ImGui::DragInt(("##" + name).c_str(), &val)) {
			access.setter(obj, name, val);
		}
        startEditing();
        endEditing();
		UpdateInspectorDragState();
	}
	else if (type == rttr::type::get<std::uint32_t>()) {
		std::uint32_t val = var.get_value<std::uint32_t>();
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
	else if (type == rttr::type::get<bool>()) {
		bool val = var.get_value<bool>();
		if (ImGui::Checkbox(("##" + name).c_str(), &val)) {
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
        ImGui::SetNextItemWidth(-1.0f);
		if (ImGui::InputText(("##" + name).c_str(), buffer, sizeof(buffer))) {
			access.setter(obj, name, std::string(buffer));
		}
        startEditing();
        endEditing();
	}
	else if (type == rttr::type::get<glm::vec3>()) {
		glm::vec3 val = var.get_value<glm::vec3>();
        ImGui::SetNextItemWidth(-1.0f);
		if (ImGui::DragFloat3(("##" + name).c_str(), &val.x, 0.1f)) {
			access.setter(obj, name, val);
		}
        startEditing();
        endEditing();
		UpdateInspectorDragState();
	}
	else if (type == rttr::type::get<glm::quat>()) {
		glm::quat q = var.get_value<glm::quat>();
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
	else if (type == rttr::type::get<TerraEngine::UUID>()) {
		TerraEngine::UUID val = var.get_value<TerraEngine::UUID>();
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
				rttr::variant oldId = access.getter(obj, name);
				access.setter(obj, name, TerraEngine::EMPTY_UUID);
				rttr::variant nextId = access.getter(obj, name);
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
            if (rttr::variant appVariant = access.getter(obj, "appearance"); appVariant.is_valid() && appVariant.is_type<std::uint32_t>())
            {
                currentAppearance = appVariant.get_value<std::uint32_t>();
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
            if (rttr::variant colorVariant = access.getter(obj, "color"); colorVariant.is_valid() && colorVariant.is_type<glm::vec3>())
            {
                lightColor = colorVariant.get_value<glm::vec3>();
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
                if (rttr::variant tempVariant = access.getter(obj, "temperature"); tempVariant.is_valid() && tempVariant.is_type<float>())
                {
                    temperature = tempVariant.get_value<float>();
                }
                drawLabeledWidget("Temperature", [&]() {
                    if (ImGui::SliderFloat("##Temperature", &temperature, 1000.0f, 20000.0f, "%.0f K"))
                    {
                        setFloatPropertyWithUndo("temperature", temperature);
                    }
                });
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
            if (rttr::variant indVariant = access.getter(obj, "indirectMultiplier"); indVariant.is_valid() && indVariant.is_type<float>())
            {
                indirectMultiplier = indVariant.get_value<float>();
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
                if (rttr::variant rangeVariant = access.getter(obj, "range"); rangeVariant.is_valid() && rangeVariant.is_type<float>())
                {
                    range = rangeVariant.get_value<float>();
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
                if (rttr::variant innerVariant = access.getter(obj, "innerConeAngle"); innerVariant.is_valid() && innerVariant.is_type<float>())
                {
                    innerDegrees = glm::degrees(innerVariant.get_value<float>());
                }
                if (rttr::variant outerVariant = access.getter(obj, "outerConeAngle"); outerVariant.is_valid() && outerVariant.is_type<float>())
                {
                    outerDegrees = glm::degrees(outerVariant.get_value<float>());
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
                if (rttr::variant wVariant = access.getter(obj, "width"); wVariant.is_valid() && wVariant.is_type<float>())
                {
                    width = wVariant.get_value<float>();
                }
                if (rttr::variant hVariant = access.getter(obj, "height"); hVariant.is_valid() && hVariant.is_type<float>())
                {
                    height = hVariant.get_value<float>();
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
            if (rttr::variant castVariant = access.getter(obj, "castsShadows"); castVariant.is_valid() && castVariant.is_type<bool>())
            {
                castsShadows = castVariant.get_value<bool>();
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
                if (rttr::variant strVariant = access.getter(obj, "shadowStrength"); strVariant.is_valid() && strVariant.is_type<float>())
                {
                    shadowStrength = strVariant.get_value<float>();
                }
                drawLabeledWidget("Strength", [&]() {
                    if (ImGui::SliderFloat("##Strength", &shadowStrength, 0.0f, 1.0f))
                    {
                        setFloatPropertyWithUndo("shadowStrength", std::clamp(shadowStrength, 0.0f, 1.0f));
                    }
                });

                float shadowBias = 0.005f;
                if (rttr::variant biasVariant = access.getter(obj, "shadowBias"); biasVariant.is_valid() && biasVariant.is_type<float>())
                {
                    shadowBias = biasVariant.get_value<float>();
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
                        if (ImGui::BeginTable("PropertyTable", 2, ImGuiTableFlags_SizingStretchProp)) {
                            ImGui::TableSetupColumn("Label", ImGuiTableColumnFlags_WidthFixed, 100.0f);
                            ImGui::TableSetupColumn("Widget", ImGuiTableColumnFlags_WidthStretch);

                            for (auto prop : type.get_properties()) {
                                if (prop.get_name() == "active") continue;
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

		ImGui::TableNextRow();
		ImGui::TableSetColumnIndex(0);
		ImGui::TextUnformatted("UASTC Quality");
		ImGui::TableSetColumnIndex(1);
		ImGui::SetNextItemWidth(-1.0f);

		// Mutable reference to update metadata directly in the database
		AssetMetadata& metadata = const_cast<AssetMetadata&>(it->second);
		int currentQuality = 1; // Balanced default
		if (metadata.settings.contains("quality")) {
			currentQuality = metadata.settings["quality"].get<int>();
		}

		const char* qualityPresets[] = { "Low (Fast)", "Balanced", "High (Slower)", "Ultra (Very Slow)" };
		if (ImGui::Combo("##UastcQuality", &currentQuality, qualityPresets, IM_ARRAYSIZE(qualityPresets))) {
			metadata.settings["quality"] = currentQuality;
			// Trigger re-import which will call CompileTexture with the new quality
			AssetDatabase::Instance().ImportAsset(std::filesystem::absolute(path), true);
			
			// Notify the renderer to reload the updated asset from disk
			RenderSystem::Instance().ReloadTexture(metadata.guid);
		}

		ImGui::EndTable();
	}
}
