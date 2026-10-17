// ProjectBrowserWindow.h — Project Browser Window Definition
//
// Defines the ProjectBrowserWindow class, providing a filesystem-like interface for managing project assets.
// It supports file navigation, asset importing, and organization within the project's directory structure.

#pragma once
#include "../Assets/AnimationAssetActions.h"
#include "../Terrain/TerrainCreationService.h"
#include "../Framework/EditorWindow.h"
#include <array>
#include <atomic>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <filesystem>
#include <future>
#include <unordered_set>

class MainEditorSubsystem;


class ProjectBrowserWindow : public EditorWindow {
public:
	ProjectBrowserWindow(MainEditorSubsystem* owner);
	void Reset();

protected:
	virtual void OnGui() override;

private:
	void DrawDirectoryTree(const std::filesystem::path& path);
	void DrawDirectoryContent(const std::filesystem::path& path);
	void OpenCreateAssetModal(const std::string& title, const std::string& defaultName, const std::string& extension);
	void OpenCreateAnimationAssetModal(TerraEngine::Editor::Assets::AnimationAssetKind kind);
	void DrawCreateAssetModal();
	void CreateNamedAsset();
	void OpenAnimationAsset(const std::filesystem::path& path);
	void DrawAnimationAssetContextActions(const std::filesystem::path& path);
	// Package skeletons and clips are reimport-owned. This browser action creates
	// a separate checked loose copy in the selected Assets folder instead of
	// allowing an edit to mutate the package's generated output.
	void ExtractGeneratedAnimationPackageChild(const std::string& guidHex,
		bool isSkeleton, const std::string& displayName);
	void DrawAnimationLifecycleModal();
	void PollAnimationValidation();

	MainEditorSubsystem* m_Owner;
	std::filesystem::path m_CurrentDirectory;
	std::string m_SearchFilter;
	std::unordered_set<std::string> m_CollapsedPackageGroups;
	std::unordered_set<std::string> m_InitializedPackageGroups;
	std::string m_SelectedPackageChildGuid;
	float m_ThumbnailSize = 64.0f;
	float m_Padding = 16.0f;

	std::filesystem::path m_AssetToDelete;
	bool m_ShowDeleteModal = false;
	bool m_ShowCreateAssetModal = false;
	bool m_ShowCreateScriptModal = false;
	std::string m_CreateAssetTitle;
	std::string m_CreateAssetExtension;
	std::array<char, 128> m_NewAssetName{};
	std::uint32_t m_TerrainWidthSamples = 256u;
	std::uint32_t m_TerrainDepthSamples = 256u;
	float m_TerrainSampleSpacingMetres = 1.0F;
	struct TerrainCreationWorkerState {
		std::mutex mutex;
		float progress = 0.0F;
		std::string stage;
		std::string message;
	};
	std::shared_ptr<TerrainCreationWorkerState> m_TerrainCreationState = std::make_shared<TerrainCreationWorkerState>();
	std::shared_ptr<std::atomic_bool> m_TerrainCreationCancellation;
	std::future<TerraEngine::Editor::Terrain::TerrainCreationResult> m_TerrainCreationFuture;
	std::filesystem::path m_TerrainCreationTarget;
	std::array<char, 128> m_NewScriptName{};
	std::array<char, 128> m_AnimationLifecycleValue{};
	std::string m_CreateAssetError;
	std::string m_CreateScriptError;
	TerraEngine::Editor::Assets::AnimationAssetActions m_AnimationAssetActions;
	std::optional<TerraEngine::Editor::Assets::AnimationAssetKind> m_CreateAnimationAssetKind;
	std::uint64_t m_AnimationLifecycleAssetId = 0U;
	enum class AnimationLifecycleModal : std::uint8_t { None, Rename, Move };
	AnimationLifecycleModal m_AnimationLifecycleModal = AnimationLifecycleModal::None;
	struct AnimationValidationWorkerState {
		std::mutex mutex;
		std::optional<TerraEngine::Editor::Assets::AnimationAssetValidationResult> completed;
		std::atomic_bool running{false};
	};
	std::shared_ptr<AnimationValidationWorkerState> m_AnimationValidation =
		std::make_shared<AnimationValidationWorkerState>();
	std::optional<TerraEngine::Editor::Assets::AnimationAssetValidationRequest> m_AnimationValidationRequest;
	std::string m_AnimationLifecycleMessage;
};
