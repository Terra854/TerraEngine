// InspectorWindow.h — Object Inspector Window Definition
//
// Defines the InspectorWindow class, which displays and allows editing of properties for the currently selected entity or asset.
// It uses the engine's reflection system to dynamically generate UI controls for various component types and data fields.

#pragma once
#include "../Framework/EditorWindow.h"
#include "Engine/Core/Object/Reflection.h"
#include <entt/entity/entity.hpp>
#include <map>
#include <vector>
#include <string>
#include <filesystem>

class MainEditorSubsystem;
class Object;
// struct ComponentAccessFunctions groups related state and operations so callers have one documented contract.
struct ComponentAccessFunctions;

class InspectorWindow : public EditorWindow {
public:
	InspectorWindow(MainEditorSubsystem* owner);
	
	// Reports whether a drag control in the inspector is active this frame for cursor wrapping.
	// IsInspectorValueDragging is an implementation entry point; keep its inputs, side effects, and ownership expectations aligned with callers.
	bool IsInspectorValueDragging() const { return m_InspectorValueDragging; }

protected:
	virtual void OnGui() override;

private:
	void DrawProperty(const std::string& compName, TerraEngine::Reflection::Property prop, Object& obj, ComponentAccessFunctions& access);
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
	
	TerraEngine::Reflection::Variant m_PreEditValue;
	std::string m_PreEditName;
	std::string m_PreEditScriptJson;

	std::filesystem::path m_LastInspectedAsset;
	std::vector<uint8_t> m_MaterialDataBuffer;
	std::string m_LastScriptMessage;
	bool m_LastScriptMessageIsError = false;

	char m_TagInputBuffer[256] = "";
	char m_LayerInputBuffer[256] = "";
};
