#include "InspectorWindow.h"
#include "../Subsystems/MainEditorSubsystem.h"
#include "Engine/Core/Object/ObjectManager.h"
#include "Engine/Core/Object/Component.h"
#include "AssetDatabase.h"
#include <imgui.h>
#include <glm/vec3.hpp>
#include <glm/gtc/quaternion.hpp>
#include <algorithm>

InspectorWindow::InspectorWindow(MainEditorSubsystem* owner)
    : EditorWindow("Inspector"), m_Owner(owner) {
}

void InspectorWindow::UpdateInspectorDragState() {
	if (ImGui::IsItemActive() && ImGui::IsMouseDragging(ImGuiMouseButton_Left)) {
		m_InspectorValueDragging = true;
	}
}

void InspectorWindow::DrawProperty(rttr::property prop, Object& obj, ComponentAccessFunctions& access) {
	std::string name = prop.get_name().to_string();
	rttr::variant var = access.getter(obj, name);
	rttr::type type = prop.get_type();

	if (type == rttr::type::get<float>()) {
		float val = var.get_value<float>();
		if (ImGui::DragFloat(name.c_str(), &val, 0.1f)) {
			access.setter(obj, name, val);
		}
		UpdateInspectorDragState();
	}
	else if (type == rttr::type::get<int>()) {
		int val = var.get_value<int>();
		if (ImGui::DragInt(name.c_str(), &val)) {
			access.setter(obj, name, val);
		}
		UpdateInspectorDragState();
	}
	else if (type == rttr::type::get<bool>()) {
		bool val = var.get_value<bool>();
		if (ImGui::Checkbox(name.c_str(), &val)) {
			access.setter(obj, name, val);
		}
	}
	else if (type == rttr::type::get<std::string>()) {
		std::string val = var.get_value<std::string>();
		char buffer[256];
		strncpy(buffer, val.c_str(), sizeof(buffer));
		if (ImGui::InputText(name.c_str(), buffer, sizeof(buffer))) {
			access.setter(obj, name, std::string(buffer));
		}
	}
	else if (type == rttr::type::get<glm::vec3>()) {
		glm::vec3 val = var.get_value<glm::vec3>();
		if (ImGui::DragFloat3(name.c_str(), &val.x, 0.1f)) {
			access.setter(obj, name, val);
		}
		UpdateInspectorDragState();
	}
	else if (type == rttr::type::get<glm::quat>()) {
		glm::quat q = var.get_value<glm::quat>();
		glm::vec3 euler = glm::degrees(glm::eulerAngles(q));
		if (ImGui::DragFloat3(name.c_str(), &euler.x, 0.1f)) {
			glm::quat newQ = glm::quat(glm::radians(euler));
			access.setter(obj, name, newQ);
		}
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
					access.setter(obj, name, newId);
				}
			} else {
				// Generic UUID property, accept any asset payload
				for (const char* p : payloads) {
					if (const ImGuiPayload* payload = ImGui::AcceptDragDropPayload(p)) {
						IM_ASSERT(payload->DataSize == sizeof(TerraEngine::UUID));
						TerraEngine::UUID newId = *(const TerraEngine::UUID*)payload->Data;
						access.setter(obj, name, newId);
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
                        transAccess.setter(obj, "active", isActive);
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
                // objManager.setObjectName(obj, nameBuf); // Missing API
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
                        if (activeProp.is_valid()) access.setter(obj, "active", compActive);
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
                        DrawProperty(prop, obj, access);
                    }
                    ImGui::Unindent();
                }
                ImGui::Separator();
                ImGui::PopID();
            }

            ImGui::Spacing();
            ImGui::Separator();
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
