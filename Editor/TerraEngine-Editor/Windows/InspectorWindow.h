#pragma once
#include "../Framework/EditorWindow.h"
#include <entt/entity/entity.hpp>
#include <map>
#include <vector>
#include <string>
#include <filesystem>
#include <rttr/type>

class MainEditorSubsystem;
class Object;
struct ComponentAccessFunctions;

class InspectorWindow : public EditorWindow {
public:
    InspectorWindow(MainEditorSubsystem* owner);
    
    // Reports whether a drag control in the inspector is active this frame for cursor wrapping.
	bool IsInspectorValueDragging() const { return m_InspectorValueDragging; }

protected:
    virtual void OnGui() override;

private:
	void DrawProperty(const std::string& compName, rttr::property prop, Object& obj, ComponentAccessFunctions& access);
	void DrawScriptComponentInspector(Object& obj);
	void DrawScriptDropTarget(Object& obj);
	void DrawLightComponentInspector(const std::string& compName, Object& obj, ComponentAccessFunctions& access);
	void DrawUIComponentInspector(const std::string& compName, Object& obj, ComponentAccessFunctions& access);
	void DrawMaterialInspector(const std::filesystem::path& path);
	void DrawMeshInspector(const std::filesystem::path& path);
	void DrawTextureInspector(const std::filesystem::path& path);
	void UpdateInspectorDragState();

    MainEditorSubsystem* m_Owner;
    std::map<entt::entity, std::vector<std::string>> m_PerObjectComponentOrders;
    bool m_InspectorValueDragging = false;

    std::filesystem::path m_LastInspectedAsset;
    std::vector<uint8_t> m_MaterialDataBuffer;
    std::string m_LastScriptMessage;
    bool m_LastScriptMessageIsError = false;

    char m_TagInputBuffer[256] = "";
    char m_LayerInputBuffer[256] = "";
};
