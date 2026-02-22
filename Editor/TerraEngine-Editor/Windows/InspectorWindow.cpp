#include "InspectorWindow.h"
#include "../Subsystems/MainEditorSubsystem.h"
#include "../Subsystems/UndoRedoSystem.h"
#include "Engine/Core/Object/ObjectManager.h"
#include "Engine/Core/Object/Component.h"
#include "Engine/Resource/ResourceManager.h"
#include "AssetDatabase.h"
#include "Engine/File Formats/TerraEngineMaterialFile.h"
#include "Engine/File Formats/TerraEngineMeshFile.h"
#include "Engine/File Formats/TerraEngineTextureFile.h"
#include <imgui.h>
#include <glm/vec3.hpp>
#include <glm/gtc/quaternion.hpp>
#include <algorithm>
#include <fstream>
#include <filesystem>

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
                    if (ImGui::BeginDragDropSource()) {
                        int sourceIdx = (int)i;
                        ImGui::SetDragDropPayload("COMPONENT_ORDER", &sourceIdx, sizeof(int));
                        ImGui::Text("Reorder %s", compName.c_str());
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
                ImGui::TextUnformatted(compName.c_str());

                ImGui::SetCursorScreenPos(ImVec2(p.x, p.y + lineHeight));
                ImGui::Dummy(ImVec2(0,0));

                if (isOpen) {
                    ImGui::Indent();
                    for (auto prop : type.get_properties()) {
                        if (prop.get_name() == "active") continue;
                        DrawProperty(compName, prop, obj, access);
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
                std::sort(availableComponents.begin(), availableComponents.end());

                for (const auto& compName : availableComponents) {
                    if (searchBuf[0] != '\0') {
                        std::string lowerName = compName;
                        std::string lowerFilter = searchBuf;
                        std::transform(lowerName.begin(), lowerName.end(), lowerName.begin(), ::tolower);
                        std::transform(lowerFilter.begin(), lowerFilter.end(), lowerFilter.begin(), ::tolower);
                        if (lowerName.find(lowerFilter) == std::string::npos) continue;
                    }

                    if (ImGui::Selectable(compName.c_str())) {
                        accessReg[compName].addComponent(obj);
                        ImGui::CloseCurrentPopup();
                    }
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

    drawTextureSlot("Base Color", TextureSemantic::BaseColor);
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

	// Explicitly call out legacy assets so users understand why runtime now rejects them.
	if (header.version < kMeshCurrentVersion) {
		ImGui::TextColored(ImVec4(1, 0.7f, 0, 1), "Legacy/non-meshlet TEMF detected (version %u).", (uint32_t)header.version);
		ImGui::TextWrapped("Runtime now requires meshlet-only TEMF version %u or newer. Recompile this asset to remove legacy data.", (uint32_t)kMeshCurrentVersion);
	}
	else if (header.version > kMeshCurrentVersion) {
		ImGui::TextColored(ImVec4(1, 1, 0, 1), "TEMF version %u is newer than this editor/runtime (%u).", (uint32_t)header.version, (uint32_t)kMeshCurrentVersion);
	}

	ImGui::Text("Compiled Info (.temf - meshlet only)");
	ImGui::Indent();
	ImGui::Text("Version: %u", (uint32_t)header.version);
	ImGui::Text("Meshlet Vertices: %u", header.meshletVertexCount);
	ImGui::Text("Meshlet Primitive Indices: %u", header.meshletPrimitiveIndexCount);
	ImGui::Text("Meshlets: %u", header.meshletCount);
	ImGui::Text("Meshlet Sections: %u", header.meshletSectionCount);
	ImGui::Text("Meshlet Bounds: %u", header.meshletBoundsCount);
	ImGui::Unindent();

	ImGui::Spacing();
	if (ImGui::CollapsingHeader("Meshlet Payload", ImGuiTreeNodeFlags_DefaultOpen)) {
		ImGui::Indent();
		ImGui::Text("Header Size: %u", header.headerSize);
		ImGui::Text("Vertex Payload Offset: %u", header.meshletVertexDataOffset);
		ImGui::Text("Primitive Index Payload Offset: %u", header.meshletPrimitiveIndexDataOffset);
		ImGui::Text("Meshlet Table Offset: %u", header.meshletTableOffset);
		ImGui::Text("Meshlet Section Offset: %u", header.meshletSectionOffset);
		ImGui::Text("Meshlet Bounds Offset: %u", header.meshletBoundsOffset);
		ImGui::Unindent();
	}

	if (ImGui::CollapsingHeader("Vertex Layout", ImGuiTreeNodeFlags_DefaultOpen)) {
		ImGui::Indent();
		ImGui::Text("Stride: %u bytes", header.layout.strideBytes);
		ImGui::Text("Attributes: %u", header.layout.attributeCount);

		if (ImGui::BeginTable("VertexAttribTable", 3, ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg)) {
			ImGui::TableSetupColumn("Semantic");
			ImGui::TableSetupColumn("Format");
			ImGui::TableSetupColumn("Offset");
			ImGui::TableHeadersRow();

			for (uint32_t i = 0; i < header.layout.attributeCount; ++i) {
				const auto& attr = header.layout.attributes[i];
				ImGui::TableNextRow();
				ImGui::TableSetColumnIndex(0);

				const char* semantics[] = { "Position", "Normal", "Tangent", "Bitangent", "TexCoord0", "Color0" };
				if (attr.semantic < 6) ImGui::TextUnformatted(semantics[attr.semantic]);
				else ImGui::Text("Other (%u)", attr.semantic);

				ImGui::TableSetColumnIndex(1);
				const char* formats[] = { "None", "R10G10B10A2Snorm", "R10G10B10A2Unorm", "R16G16Unorm" };
				if (attr.format < 4) ImGui::TextUnformatted(formats[attr.format]);
				else ImGui::Text("Other (%u)", attr.format);

				ImGui::TableSetColumnIndex(2);
				ImGui::Text("%u", attr.offsetBytes);
			}
			ImGui::EndTable();
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
