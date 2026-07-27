// InspectorWindow.h — Object Inspector Window Definition
//
// Defines the InspectorWindow class, which displays and allows editing of properties for the currently selected entity or asset.
// It uses the engine's reflection system to dynamically generate UI controls for various component types and data fields.

#pragma once
#include "../Framework/EditorWindow.h"
#include "AssetDatabase.h"
#include "Engine/Core/Object/Reflection.h"
#include "Engine/Utilities/UUID.h"
#include "../Animation/AnimationPreviewController.h"
#include "../Subsystems/PhysicsEditorAuthoringState.h"
#include <entt/entity/entity.hpp>
#include <map>
#include <vector>
#include <string>
#include <filesystem>
#include <cstdint>
#include <memory>
#include <optional>

class MainEditorSubsystem;
namespace TerraEngine::Animation { struct AnimationClipAsset; }
// struct ComponentAccessFunctions groups related state and operations so callers have one documented contract.
struct ComponentAccessFunctions;

class InspectorWindow : public EditorWindow {
public:
	// Editor-only DTO consumed by MainEditorSubsystem at its protected authoring
	// boundary; it deliberately has no provider or live-world pointer.
	using PhysicsRebuildIntent = TerraEngine::Editor::PhysicsRebuildIntent;

	InspectorWindow(MainEditorSubsystem* owner);
	~InspectorWindow() override;
	
	// Reports whether a drag control in the inspector is active this frame for cursor wrapping.
	// IsInspectorValueDragging is an implementation entry point; keep its inputs, side effects, and ownership expectations aligned with callers.
	bool IsInspectorValueDragging() const { return m_InspectorValueDragging; }
	std::vector<PhysicsRebuildIntent> DrainPhysicsRebuildIntentsForProtectedBoundary();
	void QueuePhysicsRebuildIntentsForChangedObjects(const std::vector<TerraEngine::UUID>& objectIds, std::string reason);

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
	// Physics components own nested authored records that reflection intentionally
	// does not flatten.  These editors keep record identity and units visible.
	void DrawRigidBodyInspector(const std::vector<entt::entity>& objects);
	void DrawColliderSetInspector(const std::vector<entt::entity>& objects);
	void DrawConstraintSetInspector(const std::vector<entt::entity>& objects);
	void DrawCharacterControllerInspector(const std::vector<entt::entity>& objects);
	void DrawPhysicsRuntimeState(entt::entity obj);
	void DrawAnimatorInspector(entt::entity obj, const std::vector<entt::entity>& selectedObjects);
	TerraEngine::UUID ResolveAnimatorStartClipAssetId(
		TerraEngine::UUID controllerAssetId,
		TerraEngine::UUID defaultClipAssetId);
	std::optional<std::uint32_t> ResolveAnimatorPlaybackMaximumFrame(
		TerraEngine::UUID clipAssetId);
	void DrawAnimationPreviewControls(
		const TerraEngine::Editor::Animation::AnimationPreviewSessionDescriptor& descriptor);
	void DrawModelPackageInspector(const std::filesystem::path& path);
	void DrawModelPackageChildInspector(const std::filesystem::path& packagePath, TerraEngine::UUID guid, const std::string& kind, const std::string& displayName);
	void DrawVirtualGeometryOverrideControls(const AssetMetadata& metadata);
	void DrawGeneratedMeshVirtualGeometryOverrideControls(const std::filesystem::path& packagePath, TerraEngine::UUID meshGuid);
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
	// The editor owns intent, not a provider/world pointer.  The editor lifecycle
	// coordinator drains these at its declared protected authoring boundary.
	TerraEngine::Editor::PhysicsRebuildIntentQueue m_PhysicsRebuildIntents;
	VirtualGeometryAdaptiveSettingsMetadata m_VirtualGeometryEditSettings;
	std::string m_VirtualGeometryEditKey;
	std::string m_VirtualGeometrySettingsMessage;
	bool m_VirtualGeometrySettingsMessageIsError = false;
	// Clip bytes and playback cursors are editor-only caches. Neither record is
	// reachable from reflection or scene serialization, so closing/changing the
	// Inspector cannot modify authored AnimatorComponent data.
	TerraEngine::Editor::Animation::AnimationPreviewController m_AnimationPreview;
	std::unique_ptr<TerraEngine::Animation::AnimationClipAsset> m_InspectedAnimationClip;
	std::optional<TerraEngine::Editor::Animation::AnimationPreviewSessionDescriptor> m_LastAnimationClipDescriptor;
	std::string m_InspectedAnimationClipKey;
	TerraEngine::Animation::AnimationStatus m_InspectedAnimationClipStatus =
		TerraEngine::Animation::AnimationStatus::NotReady;
	// The controller's entry leaf changes only when its publication generation
	// changes. Cache the resolved clip identity so the Inspector does not decode
	// the controller document every frame.
	TerraEngine::UUID m_AnimatorFrameRangeControllerId = TerraEngine::EMPTY_UUID;
	std::uint64_t m_AnimatorFrameRangeControllerGeneration = 0U;
	TerraEngine::UUID m_AnimatorFrameRangeControllerEntryClipId = TerraEngine::EMPTY_UUID;
	// Playback duration changes only when the resolved clip's ResourceManager
	// publication generation changes. Cache the small editor range instead of
	// copying and decoding a potentially large clip every Inspector frame.
	TerraEngine::UUID m_AnimatorFrameRangeClipId = TerraEngine::EMPTY_UUID;
	std::uint64_t m_AnimatorFrameRangeClipGeneration = 0U;
	std::optional<std::uint32_t> m_AnimatorFrameRangeMaximum;

	char m_TagInputBuffer[256] = "";
	char m_LayerInputBuffer[256] = "";
};
