// InspectorWindow.h — Object Inspector Window Definition
//
// Defines the InspectorWindow class, which displays and allows editing of properties for the currently selected entity or asset.
// It uses the engine's reflection system to dynamically generate UI controls for various component types and data fields.

#pragma once
#include "../Framework/EditorWindow.h"
#include "Engine/Core/Object/Reflection.h"
#include "Engine/Utilities/UUID.h"
#include <entt/entity/entity.hpp>
#include <map>
#include <vector>
#include <string>
#include <filesystem>

class MainEditorSubsystem;
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
	bool IsPrefabOverride(entt::entity obj, const std::string& compName, const std::string& propertyName) const;
	void ResetPrefabOverride(entt::entity obj, const std::string& compName, const std::string& propertyName);
	void DrawPrefabLinkInspector(entt::entity obj);
	void DrawProperty(const std::string& compName, TerraEngine::Reflection::Property prop, entt::entity obj, ComponentAccessFunctions& access);
	void DrawScriptComponentInspector(entt::entity obj);
	void DrawScriptDropTarget(entt::entity obj);
	void DrawLightComponentInspector(const std::string& compName, entt::entity obj, ComponentAccessFunctions& access);
	void DrawUIComponentInspector(const std::string& compName, entt::entity obj, ComponentAccessFunctions& access);
	void DrawModelPackageInspector(const std::filesystem::path& path);
	void DrawModelPackageChildInspector(const std::filesystem::path& packagePath, TerraEngine::UUID guid, const std::string& kind, const std::string& displayName);
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
