// ProjectBrowserWindow.cpp — Project Browser Window Implementation
//
// Implements the asset management UI, including folder navigation, file searching, and thumbnail rendering.
// It interacts with the operating system's filesystem and the engine's resource management systems to handle asset lifecycle.

#include "ProjectBrowserWindow.h"
#include "../Subsystems/MainEditorSubsystem.h"
#include "../Subsystems/ScriptCompilationSubsystem.h"
#include "../Terrain/TerrainCreationService.h"
#include "UIBuilderWindow.h"
#include "ProjectManager.h"
#include "AssetDatabase.h"
#include "ModelPackagePrivatePaths.h"
#include "AssetImportResult.h"
#include "ModelPackageImporter.h"
#include "Engine/Resource/ResourceManager.h"
#include "Engine/ThreadPool/ThreadPool.h"
#include <imgui.h>
#include <ImGuiFileDialog.h>
#include <algorithm>
#include <chrono>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <optional>
#include <vector>
#include "../../../Engine/File Formats/TerraEngineMaterialFile.h"

namespace
{
const char* kDefaultUIMarkup = R"(<UIRoot>
	<VisualElement id="main-container" class="container">
		<Label id="title" text="New TerraEngine UI" />
		<Button id="primary-button" text="Click Me" />
	</VisualElement>
</UIRoot>
)";

// MakeUniqueAssetPath is an implementation entry point; keep its inputs, side effects, and ownership expectations aligned with callers.
std::filesystem::path MakeUniqueAssetPath(const std::filesystem::path& directory, const std::string& baseName, const std::string& extension)
{
	std::filesystem::path candidatePath = directory / (baseName + extension);
	int duplicateIndex = 1;
	// Iterate over the current data snapshot; avoid changing ownership rules inside the loop body.
	while (std::filesystem::exists(candidatePath))
	{
		candidatePath = directory / (baseName + " (" + std::to_string(duplicateIndex++) + ")" + extension);
	}

	return candidatePath;
}

// OpenMarkupAssetInBuilder is an implementation entry point; keep its inputs, side effects, and ownership expectations aligned with callers.
void OpenMarkupAssetInBuilder(MainEditorSubsystem* owner, const std::filesystem::path& markupPath)
{
	if (owner == nullptr)
	{
		return;
	}

	// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
	if (std::shared_ptr<UIBuilderWindow> uiBuilder = owner->GetWindow<UIBuilderWindow>())
	{
		uiBuilder->OpenMarkupDocument(std::filesystem::absolute(markupPath));
	}
}

bool DrawImportProgressOverlay(const AssetImportProgress& job, const ImVec2& tileMin, const ImVec2& tileMax)
{
	ImDrawList* drawList = ImGui::GetWindowDrawList();
	const float clampedProgress = std::clamp(job.progress, 0.0f, 1.0f);
	const ImU32 backdropColor = IM_COL32(18, 21, 25, 220);
	const ImU32 fillColor = job.failed ? IM_COL32(210, 70, 70, 235) : IM_COL32(68, 156, 210, 235);
	const ImU32 borderColor = IM_COL32(235, 240, 245, 150);
	const float padding = 6.0f;
	const float barHeight = 16.0f;
	const ImVec2 barMin(tileMin.x + padding, tileMax.y - padding - barHeight);
	const ImVec2 barMax(tileMax.x - padding, tileMax.y - padding);
	const ImVec2 fillMax(barMin.x + (barMax.x - barMin.x) * clampedProgress, barMax.y);

	drawList->AddRectFilled(tileMin, tileMax, IM_COL32(5, 8, 12, 90), 4.0f);
	drawList->AddRectFilled(barMin, barMax, backdropColor, 3.0f);
	drawList->AddRectFilled(barMin, fillMax, fillColor, 3.0f);
	drawList->AddRect(barMin, barMax, borderColor, 3.0f);

	char percentText[32]{};
	std::snprintf(percentText, sizeof(percentText), "%u%%", static_cast<unsigned int>(std::round(clampedProgress * 100.0f)));
	const ImVec2 textSize = ImGui::CalcTextSize(percentText);
	const ImVec2 textPos(
		barMin.x + std::max(2.0f, ((barMax.x - barMin.x) - textSize.x) * 0.5f),
		barMin.y + std::max(0.0f, ((barMax.y - barMin.y) - textSize.y) * 0.5f));
	drawList->AddText(textPos, IM_COL32(255, 255, 255, 245), percentText);

	const float closeSize = 18.0f;
	const ImVec2 closeMin(tileMax.x - closeSize - 4.0f, tileMin.y + 4.0f);
	const ImVec2 closeMax(tileMax.x - 4.0f, tileMin.y + 4.0f + closeSize);
	const ImVec2 previousCursor = ImGui::GetCursorScreenPos();
	ImGui::SetCursorScreenPos(closeMin);
	ImGui::InvisibleButton("CancelImport", ImVec2(closeSize, closeSize));
	const bool closeHovered = ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenOverlappedByItem);
	const bool clicked = ImGui::IsItemClicked(ImGuiMouseButton_Left);
	ImGui::SetCursorScreenPos(previousCursor);
	drawList->AddRectFilled(closeMin, closeMax, closeHovered ? IM_COL32(60, 68, 80, 245) : IM_COL32(24, 28, 34, 235), 3.0f);
	drawList->AddRect(closeMin, closeMax, IM_COL32(255, 255, 255, 170), 3.0f);
	const ImVec2 xTextSize = ImGui::CalcTextSize("X");
	drawList->AddText(
		ImVec2(closeMin.x + (closeSize - xTextSize.x) * 0.5f, closeMin.y + (closeSize - xTextSize.y) * 0.5f),
		IM_COL32(255, 255, 255, 245),
		"X");
	if (closeHovered) {
		ImGui::SetTooltip("Cancel import and remove this asset");
	}
	return clicked;
}

bool IsModelPackagePath(const std::filesystem::path& path)
{
	return TerraEngine::AssetPipeline::IsModelPackageSourceExtension(path);
}

bool IsImportResultSidecarPath(const std::filesystem::path& path)
{
	const std::string filename = path.filename().string();
	const std::string suffix = ".import.json";
	return filename.size() >= suffix.size()
		&& filename.compare(filename.size() - suffix.size(), suffix.size(), suffix) == 0;
}

bool IsGeneratedFolderForPackage(const std::filesystem::path& directory)
{
	if (!std::filesystem::is_directory(directory))
	{
		return false;
	}

	const std::string folderName = directory.filename().string();
	const std::string generatedSuffix = "_Generated";
	if (folderName.size() <= generatedSuffix.size()
		|| folderName.compare(folderName.size() - generatedSuffix.size(), generatedSuffix.size(), generatedSuffix) != 0)
	{
		return false;
	}

	const std::filesystem::path parent = directory.parent_path();
	const std::string packageStem = folderName.substr(0, folderName.size() - generatedSuffix.size());
	return std::filesystem::exists(parent / (packageStem + ".gltf"))
		|| std::filesystem::exists(parent / (packageStem + ".glb"))
		|| std::filesystem::exists(parent / (packageStem + ".fbx"));
}

nlohmann::json LoadPackageImportResultJson(const std::filesystem::path& packagePath)
{
	const std::filesystem::path sidecarPath = TerraEngine::AssetPipeline::GetImportResultPath(packagePath);
	std::ifstream input(sidecarPath);
	if (!input)
	{
		return nlohmann::json::object();
	}

	nlohmann::json document = nlohmann::json::object();
	try
	{
		input >> document;
	}
	catch (const nlohmann::json::exception&)
	{
		return nlohmann::json::object();
	}
	return document;
}

enum class PackageChildKind
{
	Mesh,
	Material,
	Texture,
	Prefab,
	// Skeletons are package-owned runtime payloads. They remain read-only until
	// the user explicitly makes a checked editable copy in the selected folder.
	Skeleton,
	// Animation clips are immutable package children. Keep them distinct so a
	// click reaches the clip Inspector instead of masquerading as a loose asset.
	AnimationClip
};

struct PackageChildTile
{
	PackageChildKind kind = PackageChildKind::Mesh;
	std::string name;
	std::string sourcePath;
	std::string guidHex;
};

std::string ToLowerText(std::string text)
{
	std::transform(text.begin(), text.end(), text.begin(), [](unsigned char character) {
		return static_cast<char>(std::tolower(character));
	});
	return text;
}

bool IsTerrainPrivateDataDirectory(const std::filesystem::path& directory)
{
	// A .teterrain.data directory is implementation-owned immutable storage for
	// its sibling parent asset. It must not appear as a separately editable
	// folder in either half of the Project Browser.
	return std::filesystem::is_directory(directory)
		&& ToLowerText(directory.filename().string()).ends_with(".teterrain.data");
}

bool MatchesSearchText(const std::string& text, const std::string& searchFilter)
{
	if (searchFilter.empty())
	{
		return true;
	}
	return ToLowerText(text).find(ToLowerText(searchFilter)) != std::string::npos;
}

void AppendPackageChildren(std::vector<PackageChildTile>& outChildren, const nlohmann::json& items, PackageChildKind kind)
{
	if (!items.is_array())
	{
		return;
	}

	for (const nlohmann::json& item : items)
	{
		PackageChildTile child{};
		child.kind = kind;
		child.name = item.value("name", std::string("Generated"));
		child.sourcePath = item.value("sourcePath", std::string());
		child.guidHex = item.value("guid", std::string());
		outChildren.push_back(std::move(child));
	}
}

void AppendPublishedAnimationChildren(
	std::vector<PackageChildTile>& outChildren,
	const nlohmann::json& document, const std::string& manifestKind,
	PackageChildKind tileKind, const char* fallbackName)
{
	const nlohmann::json manifest = document.value("animationManifest", nlohmann::json::object());
	const std::uint64_t candidateGeneration = manifest.value("candidateGeneration", std::uint64_t{0U});
	const std::uint64_t publishedGeneration = manifest.value("publishedGeneration", std::uint64_t{0U});
	if (!manifest.is_object()
		|| manifest.value("cancelled", false)
		|| !manifest.value("publishedAtomically", false)
		|| publishedGeneration == 0U
		|| candidateGeneration != publishedGeneration)
	{
		return;
	}

	for (const nlohmann::json& asset : manifest.value("subAssets", nlohmann::json::array()))
	{
		if (!asset.is_object() || asset.value("kind", std::string()) != manifestKind)
		{
			continue;
		}
		const std::string guid = asset.value("guid", std::string());
		if (!TerraEngine::ParseUUID(guid)) continue;

		PackageChildTile child;
		child.kind = tileKind;
		const std::string displayName = asset.value("name", std::string());
		const std::string semanticKey = asset.value("semanticKey", std::string());
		const std::filesystem::path compiledPath = asset.value("compiledPath", std::string());
		child.name = !displayName.empty()
			? displayName
			: !semanticKey.empty() ? semanticKey
				: !compiledPath.stem().empty() ? compiledPath.stem().string()
				: fallbackName;
		child.sourcePath = asset.value("sourcePath", std::string());
		if (child.sourcePath.empty()) child.sourcePath = compiledPath.generic_string();
		child.guidHex = guid;
		outChildren.push_back(std::move(child));
	}
}

std::vector<PackageChildTile> BuildPackageChildTiles(const std::filesystem::path& packagePath)
{
	const nlohmann::json document = LoadPackageImportResultJson(packagePath);
	std::vector<PackageChildTile> children;
	if (!document.is_object() || !document.contains("package"))
	{
		return children;
	}

	// Generated prefabs are the primary thing users drag into scenes, so show
	// them before the implementation-detail mesh/material/texture children.
	AppendPackageChildren(children, document.value("generatedPrefabs", nlohmann::json::array()), PackageChildKind::Prefab);
	// This lightweight tile pass trusts only the publication marker already read
	// for the browser. Inspector/AssetDatabase repeats full dependency and hash
	// validation before it opens the compiled payload.
	AppendPublishedAnimationChildren(children, document, "Skeleton",
		PackageChildKind::Skeleton, "Skeleton");
	AppendPublishedAnimationChildren(children, document, "AnimationClip",
		PackageChildKind::AnimationClip, "Animation Clip");
	AppendPackageChildren(children, document.value("generatedMeshes", nlohmann::json::array()), PackageChildKind::Mesh);
	AppendPackageChildren(children, document.value("generatedMaterials", nlohmann::json::array()), PackageChildKind::Material);
	AppendPackageChildren(children, document.value("generatedTextures", nlohmann::json::array()), PackageChildKind::Texture);
	return children;
}

bool PackageChildrenMatchSearch(const std::vector<PackageChildTile>& children, const std::string& searchFilter)
{
	for (const PackageChildTile& child : children)
	{
		if (MatchesSearchText(child.name, searchFilter) || MatchesSearchText(child.sourcePath, searchFilter))
		{
			return true;
		}
	}
	return false;
}

const char* PackageChildIcon(PackageChildKind kind)
{
	switch (kind)
	{
	case PackageChildKind::Mesh:
		return "[MSH]";
	case PackageChildKind::Material:
		return "[MAT]";
	case PackageChildKind::Texture:
		return "[TEX]";
	case PackageChildKind::Prefab:
		return "[PFB]";
	case PackageChildKind::Skeleton:
		return "[SKL]";
	case PackageChildKind::AnimationClip:
		return "[ANM]";
	default:
		return "[SUB]";
	}
}

const char* PackageChildPayload(PackageChildKind kind)
{
	switch (kind)
	{
	case PackageChildKind::Mesh:
		return "ASSET_MESH";
	case PackageChildKind::Material:
		return "ASSET_MATERIAL";
	case PackageChildKind::Texture:
		return "ASSET_TEXTURE";
	case PackageChildKind::Prefab:
		return "ASSET_PREFAB";
	case PackageChildKind::Skeleton:
		return "ASSET_GUID";
	case PackageChildKind::AnimationClip:
		// ImplementationCheck preview is read-only; later authoring work may add a typed clip
		// drag payload without changing this stable selection identity.
		return "ASSET_GUID";
	default:
		return "ASSET_GUID";
	}
}

const char* PackageChildKindName(PackageChildKind kind)
{
	switch (kind)
	{
	case PackageChildKind::Mesh:
		return "Mesh";
	case PackageChildKind::Material:
		return "Material";
	case PackageChildKind::Texture:
		return "Texture";
	case PackageChildKind::Prefab:
		return "Prefab";
	case PackageChildKind::Skeleton:
		return "Skeleton";
	case PackageChildKind::AnimationClip:
		return "AnimationClip";
	default:
		return "SubAsset";
	}
}

void DrawTileLabel(const std::string& label, float width, const std::string& tooltip = {})
{
	const ImGuiStyle& style = ImGui::GetStyle();
	const float labelHeight = (ImGui::GetTextLineHeight() * 2.0f) + (style.ItemSpacing.y * 0.25f);
	const ImVec2 labelMin = ImGui::GetCursorScreenPos();
	const ImVec2 labelMax(labelMin.x + width, labelMin.y + labelHeight);

	// Project Browser rows should be stable even when names wrap differently.
	// Draw the text into a fixed two-line box, then reserve that exact box with
	// an invisible item so every tile contributes the same row height.
	ImDrawList* drawList = ImGui::GetWindowDrawList();
	ImGui::PushClipRect(labelMin, labelMax, true);
	drawList->AddText(
		ImGui::GetFont(),
		ImGui::GetFontSize(),
		labelMin,
		ImGui::GetColorU32(ImGuiCol_Text),
		label.c_str(),
		nullptr,
		width);
	ImGui::PopClipRect();

	ImGui::InvisibleButton("##TileLabel", ImVec2(width, labelHeight));
	if (!tooltip.empty() && ImGui::IsItemHovered())
	{
		ImGui::SetTooltip("%s", tooltip.c_str());
	}
}

bool DrawPackageChildTile(const PackageChildTile& child, float thumbnailSize, float labelWidth, bool isSelected)
{
	// Generated package children are package-owned. Mesh/material/texture
	// children drag by GUID for assignment; prefab children point at a real
	// .teprefab file that can be opened through the normal prefab editor.
	if (isSelected)
	{
		ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.2f, 0.4f, 0.6f, 1.0f));
		ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.25f, 0.48f, 0.72f, 1.0f));
	}
	else
	{
		ImGui::PushStyleColor(ImGuiCol_Button, ImGui::GetStyleColorVec4(ImGuiCol_FrameBg));
		ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImGui::GetStyleColorVec4(ImGuiCol_FrameBgHovered));
	}
	const bool clicked = ImGui::Button(PackageChildIcon(child.kind), ImVec2(thumbnailSize, thumbnailSize));
	ImGui::PopStyleColor(2);

	if (ImGui::BeginDragDropSource(ImGuiDragDropFlags_SourceAllowNullID))
	{
		if (const std::optional<TerraEngine::UUID> guid = TerraEngine::ParseUUID(child.guidHex))
		{
			ImGui::SetDragDropPayload(PackageChildPayload(child.kind), &(*guid), sizeof(TerraEngine::UUID));
			ImGui::Text("%s", child.name.c_str());
		}
		ImGui::EndDragDropSource();
	}

	std::string tooltip = child.sourcePath.empty()
		? child.name
		: child.name + "\n" + child.sourcePath;
	if(child.kind == PackageChildKind::Skeleton || child.kind == PackageChildKind::AnimationClip)
	{
		tooltip += "\nRead-only model-package child. Right-click to make an editable loose copy.";
	}
	DrawTileLabel(child.name, labelWidth, tooltip);
	return clicked;
}

void DrawPackageFoldoutControl(bool packageExpanded, const std::string& packageGroupKey, std::unordered_set<std::string>& collapsedPackageGroups)
{
	// Draw the foldout as an overlay on the package tile's upper-right corner.
	// The cursor is restored afterward so the label and generated child tiles keep their normal grid layout.
	const char* foldoutLabel = packageExpanded ? "<" : ">";
	const ImVec2 tileMin = ImGui::GetItemRectMin();
	const ImVec2 tileMax = ImGui::GetItemRectMax();
	const ImVec2 restoreCursor = ImGui::GetCursorScreenPos();
	const ImVec2 buttonSize(18.0f, 24.0f);
	ImGui::SetCursorScreenPos(ImVec2(tileMax.x - buttonSize.x, tileMin.y + ((tileMax.y - tileMin.y - buttonSize.y) * 0.5f)));
	ImGui::PushID("PackageFoldout");
	if (ImGui::Button(foldoutLabel, buttonSize))
	{
		if (packageExpanded)
		{
			collapsedPackageGroups.insert(packageGroupKey);
		}
		else
		{
			collapsedPackageGroups.erase(packageGroupKey);
		}
	}
	ImGui::PopID();
	ImGui::SetCursorScreenPos(restoreCursor);
}

}

ProjectBrowserWindow::ProjectBrowserWindow(MainEditorSubsystem* owner)
	// EditorWindow is an implementation entry point; keep its inputs, side effects, and ownership expectations aligned with callers.
	: EditorWindow("Project Browser", true, "General"), m_Owner(owner) {
	std::fill(m_NewAssetName.begin(), m_NewAssetName.end(), '\0');
	std::fill(m_NewScriptName.begin(), m_NewScriptName.end(), '\0');
}

// ProjectBrowserWindow::OnGui is an implementation entry point; keep its inputs, side effects, and ownership expectations aligned with callers.
void ProjectBrowserWindow::OnGui() {
	// File-dialog completion is processed before the main browser body, so bind
	// the active Assets root here as well as in the ordinary drawing path.
	if(m_Owner && m_Owner->GetContext()
		&& m_Owner->GetContext()->projectManager->HasActiveProject()
		&& m_AnimationAssetActions.AssetRoot().empty()) {
		(void)m_AnimationAssetActions.SetAssetRoot(AssetDatabase::Instance().GetAssetRoot());
	}
	if (ImGuiFileDialog::Instance()->Display("ImportAssetDlg")) {
		if (ImGuiFileDialog::Instance()->IsOk()) {
			std::filesystem::path filePath = ImGuiFileDialog::Instance()->GetFilePathName();
			// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
			if (std::filesystem::exists(filePath)) {
				if(TerraEngine::Editor::Assets::AnimationAssetActions::KindForPath(filePath)) {
					TerraEngine::Editor::Assets::AnimationAssetEntry imported;
					std::error_code relativeError;
					const std::filesystem::path relativeFolder = std::filesystem::relative(
						m_CurrentDirectory, AssetDatabase::Instance().GetAssetRoot(), relativeError);
					const auto status = relativeError
						? TerraEngine::Editor::Animation::AnimationAuthoringStatus::InvalidArgument
						: m_AnimationAssetActions.Import(filePath, relativeFolder.generic_string(), imported);
					m_AnimationLifecycleMessage = status == TerraEngine::Editor::Animation::AnimationAuthoringStatus::Ok
						? "Validated animation document imported atomically."
						: m_AnimationAssetActions.LastDiagnostic();
				} else {
					std::filesystem::path dest = AssetDatabase::Instance().GetAssetRoot() / filePath.filename();
					try {
						std::filesystem::copy(filePath, dest, std::filesystem::copy_options::overwrite_existing);
					} catch (const std::exception&) {
						m_AnimationLifecycleMessage = "The selected asset could not be imported.";
					}
				}
			}
		}
		ImGuiFileDialog::Instance()->Close();
	}

	// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
	if (m_Owner->GetContext() && m_Owner->GetContext()->projectManager->HasActiveProject()) {
		std::filesystem::path root = AssetDatabase::Instance().GetAssetRoot();
		// AssetDatabase remains the project-wide registration/import owner. The
		// browser lifecycle service only receives its root so it can stage source
		// documents; the watcher/database observes those files normally.
		if(m_AnimationAssetActions.AssetRoot().empty()) {
			(void)m_AnimationAssetActions.SetAssetRoot(root);
		}
		PollAnimationValidation();
		// A previously selected compiler candidate must return to the public root
		// before any existence probe, directory iteration or editable selection.
		if (m_CurrentDirectory.empty()
			|| TerraEngine::Editor::Assets::IsModelPackageTransactionPath(m_CurrentDirectory)
			|| !std::filesystem::exists(m_CurrentDirectory)) {
			m_CurrentDirectory = root;
		}

		// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
		if (ImGui::Button("Add")) {
			ImGui::OpenPopup("AddAssetPopup");
		}

		// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
		if (ImGui::BeginPopup("AddAssetPopup")) {
			if (ImGui::MenuItem("Folder")) {
				OpenCreateAssetModal("Create Folder", "New Folder", "");
			}
			// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
			if (ImGui::MenuItem("Material (.tmat)")) {
				OpenCreateAssetModal("Create Material", "New Material", ".tmat");
			}
			if (ImGui::MenuItem("Prefab (.teprefab)")) {
				OpenCreateAssetModal("Create Prefab", "New Prefab", ".teprefab");
			}
			if (ImGui::MenuItem("Terrain (.teterrain)")) {
				OpenCreateAssetModal("Create Terrain", "New Terrain", ".teterrain");
			}
			// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
			if (ImGui::MenuItem("UI Markup (.tuiml)")) {
				OpenCreateAssetModal("Create UI Markup", "New UI", ".tuiml");
			}
			if(ImGui::BeginMenu("Animation")) {
				for(const TerraEngine::Editor::Assets::AnimationAssetKind kind
					: TerraEngine::Editor::Assets::AnimationAssetActions::SupportedKinds()) {
					const std::string label = std::string(
						TerraEngine::Editor::Assets::AnimationAssetActions::DisplayName(kind))
						+ " (" + TerraEngine::Editor::Assets::AnimationAssetActions::Extension(kind) + ")";
					if(ImGui::MenuItem(label.c_str())) {
						OpenCreateAnimationAssetModal(kind);
					}
				}
				ImGui::Separator();
				if(ImGui::MenuItem("Undo Animation Asset Action", nullptr, false,
					m_AnimationAssetActions.UndoDepth() != 0U)) {
					(void)m_AnimationAssetActions.Undo();
				}
				if(ImGui::MenuItem("Redo Animation Asset Action", nullptr, false,
					m_AnimationAssetActions.RedoDepth() != 0U)) {
					(void)m_AnimationAssetActions.Redo();
				}
				ImGui::EndMenu();
			}
			// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
			if (ImGui::MenuItem("C# Script (.cs)")) {
				std::fill(m_NewScriptName.begin(), m_NewScriptName.end(), '\0');
				const char* defaultName = "NewScript";
				std::copy(defaultName, defaultName + std::strlen(defaultName), m_NewScriptName.begin());
				m_CreateScriptError.clear();
				m_ShowCreateScriptModal = true;
			}
			ImGui::EndPopup();
		}

		ImGui::SameLine();
		char searchBuf[128];
		strncpy(searchBuf, m_SearchFilter.c_str(), sizeof(searchBuf));
		ImGui::SetNextItemWidth(200);
		// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
		if (ImGui::InputTextWithHint("##Search", "Search...", searchBuf, IM_ARRAYSIZE(searchBuf))) {
			m_SearchFilter = searchBuf;
		}

		ImGui::Separator();

		// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
		if (ImGui::BeginTable("BrowserSplit", 2, ImGuiTableFlags_Resizable | ImGuiTableFlags_BordersInnerV)) {
			ImGui::TableSetupColumn("Tree", ImGuiTableColumnFlags_WidthFixed, 200.0f);
			ImGui::TableSetupColumn("Content", ImGuiTableColumnFlags_WidthStretch);
			
			ImGui::TableNextRow();
			ImGui::TableSetColumnIndex(0);
			
			// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
			if (std::filesystem::exists(root)) {
				ImGuiTreeNodeFlags rootFlags = ImGuiTreeNodeFlags_OpenOnArrow | ImGuiTreeNodeFlags_OpenOnDoubleClick | ImGuiTreeNodeFlags_DefaultOpen | ImGuiTreeNodeFlags_SpanAvailWidth;
				if (root == m_CurrentDirectory) rootFlags |= ImGuiTreeNodeFlags_Selected;
				
				bool open = ImGui::TreeNodeEx("Assets", rootFlags);
				// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
				if (ImGui::IsItemClicked()) m_CurrentDirectory = root;
				
				if (open) {
					DrawDirectoryTree(root);
					ImGui::TreePop();
				}
			}

			ImGui::TableSetColumnIndex(1);
			
			// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
			if (std::filesystem::exists(m_CurrentDirectory)) {
				DrawDirectoryContent(m_CurrentDirectory);
			}

			ImGui::Separator();

			ImGui::SetNextItemWidth(100);
			ImGui::SliderFloat("Zoom", &m_ThumbnailSize, 32.0f, 128.0f);
			
			ImGui::EndTable();
		}

		// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
		if (m_ShowDeleteModal) {
			ImGui::OpenPopup("Delete Asset?");
			m_ShowDeleteModal = false;
		}

		// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
		if (ImGui::BeginPopupModal("Delete Asset?", NULL, ImGuiWindowFlags_AlwaysAutoResize)) {
			ImGui::Text("Are you sure you want to delete '%s'?", m_AssetToDelete.filename().string().c_str());
			const bool animationAsset = TerraEngine::Editor::Assets::AnimationAssetActions::KindForPath(m_AssetToDelete).has_value();
			if(animationAsset) ImGui::TextWrapped("Animation deletion can be restored with Undo Animation Asset Action.");
			else ImGui::TextColored(ImVec4(1.0f, 0.4f, 0.4f, 1.0f), "This action cannot be undone!");
			ImGui::Separator();

			// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
			if (ImGui::Button("Delete", ImVec2(120, 0))) {
				if(animationAsset) {
					TerraEngine::Editor::Assets::AnimationAssetEntry tracked;
					if(m_AnimationAssetActions.EnsureTracked(m_AssetToDelete, tracked)
						== TerraEngine::Editor::Animation::AnimationAuthoringStatus::Ok) {
						(void)m_AnimationAssetActions.Delete(tracked.id);
					}
				} else {
					AssetDatabase::Instance().DeleteAsset(m_AssetToDelete);
				}
				ImGui::CloseCurrentPopup();
			}
			ImGui::SetItemDefaultFocus();
			ImGui::SameLine();
			// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
			if (ImGui::Button("Cancel", ImVec2(120, 0))) {
				ImGui::CloseCurrentPopup();
			}
			ImGui::EndPopup();
		}

		DrawCreateAssetModal();
		DrawAnimationLifecycleModal();
		if(!m_AnimationLifecycleMessage.empty()) {
			ImGui::TextWrapped("%s", m_AnimationLifecycleMessage.c_str());
		}

		// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
		if (m_ShowCreateScriptModal) {
			ImGui::OpenPopup("Create C# Script");
			m_ShowCreateScriptModal = false;
		}

		// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
		if (ImGui::BeginPopupModal("Create C# Script", nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
			ImGui::Text("New scripts are created in the currently selected asset folder.");
			ImGui::InputText("Class Name", m_NewScriptName.data(), m_NewScriptName.size());
			// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
			if (!m_CreateScriptError.empty()) {
				ImGui::TextColored(ImVec4(1.0f, 0.4f, 0.4f, 1.0f), "%s", m_CreateScriptError.c_str());
			}

			// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
			if (ImGui::Button("Create", ImVec2(120, 0))) {
				std::filesystem::path createdPath;
				std::string error;
				// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
				if (ScriptCompilationSubsystem::Instance().CreateScriptAsset(m_CurrentDirectory, m_NewScriptName.data(), createdPath, &error)) {
					m_Owner->SetSelectedAsset(std::filesystem::absolute(createdPath));
					ImGui::CloseCurrentPopup();
				} else {
					m_CreateScriptError = error;
				}
			}
			ImGui::SameLine();
			// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
			if (ImGui::Button("Cancel", ImVec2(120, 0))) {
				ImGui::CloseCurrentPopup();
			}
			ImGui::EndPopup();
		}
	}
}

void ProjectBrowserWindow::OpenCreateAssetModal(const std::string& title, const std::string& defaultName, const std::string& extension)
{
	m_CreateAnimationAssetKind.reset();
	m_CreateAssetTitle = title;
	m_CreateAssetExtension = extension;
	m_CreateAssetError.clear();
	std::fill(m_NewAssetName.begin(), m_NewAssetName.end(), '\0');
	std::copy(defaultName.begin(), defaultName.end(), m_NewAssetName.begin());
	m_ShowCreateAssetModal = true;
}

void ProjectBrowserWindow::OpenCreateAnimationAssetModal(
	TerraEngine::Editor::Assets::AnimationAssetKind kind)
{
	m_CreateAnimationAssetKind = kind;
	OpenCreateAssetModal(
		std::string("Create ") + TerraEngine::Editor::Assets::AnimationAssetActions::DisplayName(kind),
		std::string("New ") + TerraEngine::Editor::Assets::AnimationAssetActions::DisplayName(kind),
		TerraEngine::Editor::Assets::AnimationAssetActions::Extension(kind));
	// OpenCreateAssetModal resets ordinary mode; restore the selected typed action.
	m_CreateAnimationAssetKind = kind;
}

void ProjectBrowserWindow::DrawCreateAssetModal()
{
	if (m_ShowCreateAssetModal) {
		ImGui::OpenPopup(m_CreateAssetTitle.c_str());
		m_ShowCreateAssetModal = false;
	}

	if (ImGui::BeginPopupModal(m_CreateAssetTitle.c_str(), nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
		if(m_TerrainCreationFuture.valid()
			&& m_TerrainCreationFuture.wait_for(std::chrono::seconds(0)) == std::future_status::ready)
		{
			const auto result = m_TerrainCreationFuture.get();
			m_TerrainCreationCancellation.reset();
			if(result.succeeded)
			{
				m_Owner->SetSelectedAsset(std::filesystem::absolute(result.assetPath));
				ImGui::CloseCurrentPopup();
			}
			else m_CreateAssetError = result.message.empty() ? "The terrain asset could not be created." : result.message;
		}
		const bool terrainCreationRunning = m_TerrainCreationFuture.valid();
		if(terrainCreationRunning) ImGui::BeginDisabled();
		ImGui::InputText("Name", m_NewAssetName.data(), m_NewAssetName.size());
		if (m_CreateAssetExtension == ".teterrain") {
			ImGui::InputScalar("Width (samples)", ImGuiDataType_U32, &m_TerrainWidthSamples);
			ImGui::InputScalar("Depth (samples)", ImGuiDataType_U32, &m_TerrainDepthSamples);
			ImGui::InputFloat("Sample spacing (metres)", &m_TerrainSampleSpacingMetres, 0.05F, 0.5F, "%.3f");
			const std::uint64_t areaCountX = (static_cast<std::uint64_t>(m_TerrainWidthSamples) + 255u) / 256u;
			const std::uint64_t areaCountZ = (static_cast<std::uint64_t>(m_TerrainDepthSamples) + 255u) / 256u;
			const double physicalWidth = static_cast<double>(m_TerrainWidthSamples) * m_TerrainSampleSpacingMetres;
			const double physicalDepth = static_cast<double>(m_TerrainDepthSamples) * m_TerrainSampleSpacingMetres;
			ImGui::Text("Footprint: %.2f m x %.2f m", physicalWidth, physicalDepth);
			ImGui::Text("Coverage: %llu x %llu areas (%llu total)",
				static_cast<unsigned long long>(areaCountX), static_cast<unsigned long long>(areaCountZ),
				static_cast<unsigned long long>(areaCountX * areaCountZ));
			ImGui::TextDisabled("Horizontal sample spacing is fixed after creation.");
		}
		if (!m_CreateAssetError.empty()) {
			ImGui::TextColored(ImVec4(1.0f, 0.4f, 0.4f, 1.0f), "%s", m_CreateAssetError.c_str());
		}
		if(terrainCreationRunning)
		{
			ImGui::EndDisabled();
			std::scoped_lock progressLock(m_TerrainCreationState->mutex);
			ImGui::ProgressBar(m_TerrainCreationState->progress, ImVec2(320.0F, 0.0F), m_TerrainCreationState->stage.c_str());
			if(!m_TerrainCreationState->message.empty()) ImGui::TextWrapped("%s", m_TerrainCreationState->message.c_str());
		}

		if(!terrainCreationRunning && (ImGui::Button("Create", ImVec2(120, 0)) || ImGui::IsKeyPressed(ImGuiKey_Enter))) {
			CreateNamedAsset();
		}
		if(!terrainCreationRunning) ImGui::SameLine();
		if (ImGui::Button(terrainCreationRunning ? "Cancel creation" : "Cancel", ImVec2(120, 0))) {
			if(terrainCreationRunning && m_TerrainCreationCancellation)
				m_TerrainCreationCancellation->store(true, std::memory_order_release);
			else
			ImGui::CloseCurrentPopup();
		}
		ImGui::EndPopup();
	}
}

void ProjectBrowserWindow::CreateNamedAsset()
{
	std::string assetName = m_NewAssetName.data();
	if (assetName.empty()) {
		m_CreateAssetError = "Name cannot be empty.";
		return;
	}

	if(m_CreateAnimationAssetKind) {
		std::error_code relativeError;
		const std::filesystem::path root = AssetDatabase::Instance().GetAssetRoot();
		const std::filesystem::path relativeFolder = std::filesystem::relative(
			m_CurrentDirectory, root, relativeError);
		TerraEngine::Editor::Assets::AnimationAssetEntry created;
		const auto status = !relativeError
			? m_AnimationAssetActions.Create(*m_CreateAnimationAssetKind, assetName,
				relativeFolder.generic_string(), created)
			: TerraEngine::Editor::Animation::AnimationAuthoringStatus::InvalidArgument;
		if(status != TerraEngine::Editor::Animation::AnimationAuthoringStatus::Ok) {
			m_CreateAssetError = "The animation asset could not be staged in the selected folder.";
			return;
		}
		m_Owner->SetSelectedAsset(std::filesystem::absolute(created.sourcePath));
		m_CreateAnimationAssetKind.reset();
		ImGui::CloseCurrentPopup();
		return;
	}

	const std::filesystem::path targetPath = m_CreateAssetExtension.empty()
		? MakeUniqueAssetPath(m_CurrentDirectory, assetName, "")
		: MakeUniqueAssetPath(m_CurrentDirectory, assetName, m_CreateAssetExtension);

	if (m_CreateAssetExtension.empty()) {
		std::filesystem::create_directory(targetPath);
		m_CurrentDirectory = targetPath;
		ImGui::CloseCurrentPopup();
		return;
	}

	if (m_CreateAssetExtension == ".teterrain") {
		TerraEngine::AssetPipeline::TerrainImportRequest request;
		request.sourceFormat = TerraEngine::AssetPipeline::TerrainSourceFormat::Flat;
		request.assetId = TerraEngine::GenerateUUID();
		request.flatWidth = m_TerrainWidthSamples;
		request.flatDepth = m_TerrainDepthSamples;
		request.sampleSpacingMetres = m_TerrainSampleSpacingMetres;
		// Validation occurs before any descriptor or private generation is staged,
		// so an invalid preview can never register a partial terrain asset.
		TerraEngine::Editor::Terrain::TerrainCreationService service =
			TerraEngine::Editor::Terrain::CreateProjectTerrainCreationService();
		m_TerrainCreationCancellation = std::make_shared<std::atomic_bool>(false);
		m_TerrainCreationTarget = targetPath;
		const auto state = m_TerrainCreationState;
		{
			std::scoped_lock lock(state->mutex);
			state->progress = 0.0F;
			state->stage = "Queued";
			state->message = "Terrain creation is waiting for a worker.";
		}
		m_TerrainCreationFuture = service.CreateOrImportAsync(request, targetPath,
			m_TerrainCreationCancellation,
			[state](float progress, std::string_view stage, std::string_view message) {
				std::scoped_lock lock(state->mutex);
				state->progress = progress;
				state->stage = std::string(stage);
				state->message = std::string(message);
				return true;
			});
		return;
	}

	if (m_CreateAssetExtension == ".tmat") {
		MaterialHeader header{};
		std::memcpy(header.magic, "TMAT", 4);
		header.version = kMaterialCurrentVersion;
		header.headerSize = sizeof(MaterialHeader);
		header.flags = 0;
		header.shadingModel = (uint32_t)ShadingModel::Unlit;
		header.alphaMode = (uint32_t)AlphaMode::Opaque;
		header.shaderCollectionId = ResourceManager::Instance().GetDefaultShaderCollectionID();
		header.baseColorFactor[0] = 1.0f;
		header.baseColorFactor[1] = 1.0f;
		header.baseColorFactor[2] = 1.0f;
		header.baseColorFactor[3] = 1.0f;
		header.metallicFactor = 0.0f;
		header.roughnessFactor = 0.5f;
		header.normalScale = 1.0f;
		header.occlusionStrength = 1.0f;
		header.textureCount = 0;
		header.floatParameterCount = 0;
		header.textureArrayOffset = sizeof(MaterialHeader);
		header.floatParameterOffset = sizeof(MaterialHeader);

		std::ofstream out(targetPath, std::ios::binary);
		out.write(reinterpret_cast<const char*>(&header), sizeof(MaterialHeader));
	} else if (m_CreateAssetExtension == ".tuiml") {
		std::ofstream markupOutput(targetPath);
		markupOutput << kDefaultUIMarkup;
	} else if (m_CreateAssetExtension == ".teprefab") {
		nlohmann::json prefabJson;
		prefabJson["name"] = targetPath.stem().string();
		prefabJson["objects"] = nlohmann::json::array();
		std::ofstream prefabOutput(targetPath);
		prefabOutput << prefabJson.dump(4);
	}

	m_Owner->SetSelectedAsset(std::filesystem::absolute(targetPath));
	ImGui::CloseCurrentPopup();
}

// ProjectBrowserWindow::DrawDirectoryTree is an implementation entry point; keep its inputs, side effects, and ownership expectations aligned with callers.
void ProjectBrowserWindow::DrawDirectoryTree(const std::filesystem::path& path) {
	// Also reject a stale direct request before touching private disk storage.
	if (TerraEngine::Editor::Assets::IsModelPackageTransactionPath(path)) return;
	if (!std::filesystem::exists(path)) return;

	// Iterate over the current data snapshot; avoid changing ownership rules inside the loop body.
	for (const auto& entry : std::filesystem::directory_iterator(path)) {
		if (entry.path().extension() == ".meta") continue;
		const auto directory = TerraEngine::Editor::Assets::InspectProjectBrowserEntry(entry.path(),
			[&entry](const std::filesystem::path&) { return entry.is_directory(); });
		if (!directory || !*directory) continue;
		if (IsTerrainPrivateDataDirectory(entry.path())) continue;

		std::string filename = entry.path().filename().string();
		
		ImGuiTreeNodeFlags flags = ImGuiTreeNodeFlags_OpenOnArrow | ImGuiTreeNodeFlags_OpenOnDoubleClick | ImGuiTreeNodeFlags_SpanAvailWidth;
		// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
		if (entry.path() == m_CurrentDirectory) {
			flags |= ImGuiTreeNodeFlags_Selected;
		}

		bool open = ImGui::TreeNodeEx(filename.c_str(), flags);
		// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
		if (ImGui::IsItemClicked()) {
			m_CurrentDirectory = entry.path();
		}

		// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
		if (open) {
			DrawDirectoryTree(entry.path());
			ImGui::TreePop();
		}
	}
}

// ProjectBrowserWindow::DrawDirectoryContent is an implementation entry point; keep its inputs, side effects, and ownership expectations aligned with callers.
void ProjectBrowserWindow::DrawDirectoryContent(const std::filesystem::path& path) {
	// No private transaction can enter the sortable/editable entry collection.
	if (TerraEngine::Editor::Assets::IsModelPackageTransactionPath(path)) return;
	float cellSize = m_ThumbnailSize + m_Padding;
	float panelWidth = ImGui::GetContentRegionAvail().x;
	int columnCount = (int)(panelWidth / cellSize);
	// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
	if (columnCount < 1) columnCount = 1;

	std::vector<std::filesystem::directory_entry> entries;
	// Iterate over the current data snapshot; avoid changing ownership rules inside the loop body.
	for (const auto& entry : std::filesystem::directory_iterator(path)) {
		// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
		if (entry.path().extension() == ".meta") continue;
		const auto directory = TerraEngine::Editor::Assets::InspectProjectBrowserEntry(entry.path(),
			[&entry](const std::filesystem::path&) { return entry.is_directory(); });
		if (!directory) continue;
		if (!*directory && IsImportResultSidecarPath(entry.path())) continue;
		if (entry.is_directory() && IsGeneratedFolderForPackage(entry.path())) continue;
		if (entry.is_directory() && IsTerrainPrivateDataDirectory(entry.path())) continue;
		entries.push_back(entry);
	}

	std::sort(entries.begin(), entries.end(), [](const auto& a, const auto& b) {
		// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
		if (a.is_directory() != b.is_directory()) {
			return a.is_directory() > b.is_directory(); // Directories first
		}
		return a.path().filename().string() < b.path().filename().string();
	});

	// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
	if (ImGui::BeginTable("ContentTable", columnCount, ImGuiTableFlags_SizingFixedFit)) {
		for (int columnIndex = 0; columnIndex < columnCount; ++columnIndex)
		{
			ImGui::TableSetupColumn(nullptr, ImGuiTableColumnFlags_WidthFixed, cellSize);
		}

		for (const auto& entry : entries) {
			std::string filename = entry.path().filename().string();
			const bool isPackage = !entry.is_directory() && IsModelPackagePath(entry.path());
			const std::vector<PackageChildTile> packageChildren = isPackage ? BuildPackageChildTiles(entry.path()) : std::vector<PackageChildTile>{};
			// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
			if (!m_SearchFilter.empty()) {
				// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
				if (!MatchesSearchText(filename, m_SearchFilter) && !PackageChildrenMatchSearch(packageChildren, m_SearchFilter)) continue;
			}

			ImGui::TableNextColumn();
			
			ImGui::PushID(filename.c_str());
			const std::filesystem::path absoluteEntryPath = std::filesystem::absolute(entry.path());
			const std::string packageGroupKey = absoluteEntryPath.string();
			if (isPackage && !m_InitializedPackageGroups.contains(packageGroupKey))
			{
				// Model packages can expose many virtual children. Start each
				// package collapsed once per browser session so the asset grid
				// opens cleanly, then preserve the user's manual foldout choice.
				m_InitializedPackageGroups.insert(packageGroupKey);
				m_CollapsedPackageGroups.insert(packageGroupKey);
			}
			const bool packageExpanded = !isPackage || !m_CollapsedPackageGroups.contains(packageGroupKey);
			
			std::string icon = entry.is_directory() ? "[DIR]" : "[FILE]";
			// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
			if (!entry.is_directory() && entry.path().extension() == ".cs") {
				icon = "[C#]";
			} else if (!entry.is_directory() && entry.path().extension() == ".tuiml") {
				icon = "[UI]";
			} else if (!entry.is_directory() && entry.path().extension() == ".teprefab") {
				icon = "[PFB]";
			} else if (!entry.is_directory() && entry.path().extension() == ".teterrain") {
				icon = "[TER]";
			} else if (!entry.is_directory() && IsModelPackagePath(entry.path())) {
				icon = "[PKG]";
			}
			
			bool isSelected = false;
			auto currentSelected = m_Owner->GetSelectedAsset();
			// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
			if (currentSelected && *currentSelected == absoluteEntryPath) {
				isSelected = true;
			}

			// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
			if (isSelected) {
				ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.2f, 0.4f, 0.6f, 1.0f));
			}

			const float tileLabelWidth = cellSize;
			const float packageChildThumbnailSize = m_ThumbnailSize;
			bool assetIconHovered = false;
			const std::optional<AssetImportProgress> importProgress = !entry.is_directory()
				? AssetDatabase::Instance().GetImportProgress(absoluteEntryPath)
				: std::nullopt;
			if ((isPackage && !packageChildren.empty()) || importProgress.has_value())
			{
				ImGui::SetNextItemAllowOverlap();
			}
			if (ImGui::Button(icon.c_str(), ImVec2(m_ThumbnailSize, m_ThumbnailSize))) {
				if (!entry.is_directory()) {
					m_SelectedPackageChildGuid.clear();
					m_Owner->SetSelectedAsset(absoluteEntryPath);
				}
			}
			const ImVec2 tileMin = ImGui::GetItemRectMin();
			const ImVec2 tileMax = ImGui::GetItemRectMax();
			assetIconHovered = ImGui::IsItemHovered();
			if (importProgress.has_value()) {
				if (DrawImportProgressOverlay(*importProgress, tileMin, tileMax)) {
					AssetDatabase::Instance().CancelImport(absoluteEntryPath, true);
				}
			}

			if (!entry.is_directory() && ImGui::BeginDragDropSource()) {
				auto& assets = AssetDatabase::Instance().GetAssets();
				auto it = assets.find(std::filesystem::absolute(entry.path()));
				// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
				if (it != assets.end()) {
					TerraEngine::UUID guid = it->second.guid;
					const std::string& importer = it->second.importerName;
					const char* payloadType = "ASSET_GUID";

					// Only standalone mesh assets can fill a single MeshRenderComponent
					// slot. GLB/FBX package parents are containers for generated
					// mesh children, so dragging the parent as ASSET_MESH would assign
					// a GUID that no renderable mesh resource owns.
					if (importer == "MeshImporter") payloadType = "ASSET_MESH";
					else if (importer == "TextureImporter") payloadType = "ASSET_TEXTURE";
					else if (importer == "ShaderImporter") payloadType = "ASSET_SHADER";
					else if (importer == "MaterialImporter") payloadType = "ASSET_MATERIAL";
					else if (importer == "ScriptImporter") payloadType = "ASSET_SCRIPT";
					else if (importer == "UIImporter") payloadType = "ASSET_TUIML";
					else if (importer == "StyleImporter") payloadType = "ASSET_TUSS";
					else if (importer == "PrefabImporter") payloadType = "ASSET_PREFAB";
					else if (importer == "AudioClipImporter" || importer == "CookedAudioClipImporter") payloadType = "ASSET_AUDIO";
					// Terrain fields accept only durable terrain parents. A dedicated
					// payload prevents an arbitrary asset GUID from being assigned by drag.
					else if (importer == "TerrainImporter") payloadType = "ASSET_TERRAIN";

					ImGui::SetDragDropPayload(payloadType, &guid, sizeof(TerraEngine::UUID));
					ImGui::Text("%s", filename.c_str());
				}
				ImGui::EndDragDropSource();
			}

			if (ImGui::BeginPopupContextItem("AssetIconContextMenu")) {
				DrawAnimationAssetContextActions(absoluteEntryPath);
				if (ImGui::MenuItem("Delete")) {
					m_AssetToDelete = absoluteEntryPath;
					m_ShowDeleteModal = true;
				}
				ImGui::EndPopup();
			}
			if (isPackage && !packageChildren.empty())
			{
				DrawPackageFoldoutControl(packageExpanded, packageGroupKey, m_CollapsedPackageGroups);
			}
			// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
			if (isSelected) {
				ImGui::PopStyleColor();
			}

			// Double-click performs the "open" action while single-click stays a
			// pure selection gesture, which matches the Unity-style asset flow.
			if (assetIconHovered && ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left)) {
				if (entry.is_directory()) {
					m_CurrentDirectory = entry.path();
				} else {
					// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
					if (entry.path().extension() == ".tescene") {
						auto projectRoot = m_Owner->GetContext()->projectManager->GetProjectRoot();
						auto assetRoot = projectRoot / "Assets";
						std::error_code ec;
						auto relPath = std::filesystem::relative(entry.path(), assetRoot, ec);
						// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
						if (!ec) {
							m_Owner->RequestOpenScene(relPath);
						} else {
							m_Owner->RequestOpenScene(entry.path());
						}
					} else if (entry.path().extension() == ".cs") {
						ScriptCompilationSubsystem::Instance().OpenScript(std::filesystem::absolute(entry.path()));
					} else if (entry.path().extension() == ".tuiml") {
						OpenMarkupAssetInBuilder(m_Owner, entry.path());
					} else if (entry.path().extension() == ".teprefab") {
						m_Owner->OpenPrefabAsset(std::filesystem::absolute(entry.path()));
					} else if(TerraEngine::Editor::Assets::AnimationAssetActions::KindForPath(entry.path())) {
						OpenAnimationAsset(entry.path());
					}
				}
			}

			DrawTileLabel(filename, tileLabelWidth, filename);
			if (ImGui::BeginPopupContextItem("AssetNameContextMenu")) {
				DrawAnimationAssetContextActions(absoluteEntryPath);
				if (ImGui::MenuItem("Delete")) {
					m_AssetToDelete = absoluteEntryPath;
					m_ShowDeleteModal = true;
				}
				ImGui::EndPopup();
			}
			if (isPackage && packageExpanded) {
				for (const PackageChildTile& child : packageChildren) {
					ImGui::TableNextColumn();
					ImGui::PushID(child.guidHex.c_str());
					if (DrawPackageChildTile(child, packageChildThumbnailSize, tileLabelWidth, child.guidHex == m_SelectedPackageChildGuid))
					{
						m_SelectedPackageChildGuid = child.guidHex;
						const std::filesystem::path childSourcePath = AssetDatabase::Instance().GetAssetRoot() / child.sourcePath;
						if (child.kind == PackageChildKind::Material && !child.sourcePath.empty() && std::filesystem::exists(childSourcePath))
						{
							m_Owner->SetSelectedAsset(std::filesystem::absolute(childSourcePath));
						}
						else if (child.kind == PackageChildKind::Prefab && !child.sourcePath.empty() && std::filesystem::exists(childSourcePath))
						{
							m_Owner->SetSelectedAsset(std::filesystem::absolute(childSourcePath));
							if (ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left))
							{
								m_Owner->OpenPrefabAsset(std::filesystem::absolute(childSourcePath));
							}
						}
						else if (const std::optional<TerraEngine::UUID> childGuid = TerraEngine::ParseUUID(child.guidHex))
						{
							MainEditorSubsystem::SelectedPackageChildAsset selection{};
							selection.packagePath = absoluteEntryPath;
							selection.guid = *childGuid;
							selection.kind = PackageChildKindName(child.kind);
							selection.displayName = child.name;
							m_Owner->SetSelectedPackageChildAsset(std::move(selection));
						}
						else
						{
						m_Owner->SetSelectedAsset(absoluteEntryPath);
						}
					}
					if((child.kind == PackageChildKind::Skeleton
						|| child.kind == PackageChildKind::AnimationClip)
						&& ImGui::BeginPopupContextItem("GeneratedAnimationPackageChildContext"))
					{
						// Package reimport owns this child. The context action is deliberately
						// named for its result so artists do not mistake it for an in-place edit.
						ImGui::TextDisabled("Read-only model-package child");
						if(ImGui::MenuItem("Make Editable Copy in Current Folder"))
						{
							ExtractGeneratedAnimationPackageChild(child.guidHex,
								child.kind == PackageChildKind::Skeleton, child.name);
						}
						if(ImGui::IsItemHovered())
						{
							ImGui::SetTooltip("Creates a separate checked loose asset with a new identity. The package child stays read-only.");
						}
						ImGui::EndPopup();
					}
					ImGui::PopID();
				}
			}
			ImGui::PopID();
		}
		ImGui::EndTable();
	}
}

void ProjectBrowserWindow::OpenAnimationAsset(const std::filesystem::path& path)
{
	TerraEngine::Editor::Assets::AnimationAssetEntry tracked;
	const auto status = m_AnimationAssetActions.EnsureTracked(std::filesystem::absolute(path), tracked);
	if(status != TerraEngine::Editor::Animation::AnimationAuthoringStatus::Ok) {
		m_AnimationLifecycleMessage = m_AnimationAssetActions.LastDiagnostic();
		return;
	}
	if(!m_Owner->OpenAnimationAssetEditor(path)) {
		m_AnimationLifecycleMessage = "No native editor accepts this animation document.";
	}
}

void ProjectBrowserWindow::ExtractGeneratedAnimationPackageChild(
	const std::string& guidHex, bool isSkeleton, const std::string& displayName)
{
	const std::optional<TerraEngine::UUID> guid = TerraEngine::ParseUUID(guidHex);
	if(!guid)
	{
		m_AnimationLifecycleMessage = "The generated child does not have a valid asset identity.";
		return;
	}
	const GeneratedModelPackageChildKind generatedKind = isSkeleton
		? GeneratedModelPackageChildKind::Skeleton
		: GeneratedModelPackageChildKind::AnimationClip;
	GeneratedModelPackageChildMetadata child;
	if(!AssetDatabase::Instance().TryGetGeneratedModelPackageChildMetadata(*guid,
		generatedKind, child) || !child.readOnlyPackageChild || child.compiledPath.empty())
	{
		m_AnimationLifecycleMessage = "The selected package child is not a published read-only animation payload.";
		return;
	}

	std::error_code relativeError;
	const std::filesystem::path assetRoot = AssetDatabase::Instance().GetAssetRoot();
	const std::filesystem::path destinationFolder = std::filesystem::relative(
		m_CurrentDirectory, assetRoot, relativeError);
	if(relativeError)
	{
		m_AnimationLifecycleMessage = "Select a folder inside Assets before making an editable copy.";
		return;
	}
	// Display names come from arbitrary source formats. Replace only filename-illegal
	// characters so the default remains recognizable while the typed lifecycle
	// service can keep its strict path-containment rules.
	std::string editableName = displayName;
	for(char& character : editableName)
	{
		if(std::strchr("\\/:*?\"<>|", character) != nullptr) character = '_';
	}
	if(editableName.empty() || editableName == "." || editableName == "..")
	{
		editableName = isSkeleton ? "Editable Skeleton" : "Editable Animation Clip";
	}

	const std::filesystem::path generatedSource = child.compiledPath.is_absolute()
		? child.compiledPath
		: AssetDatabase::Instance().GetBuildRoot() / child.compiledPath;
	TerraEngine::Editor::Assets::AnimationAssetEntry extracted;
	const auto status = m_AnimationAssetActions.ExtractReadOnlyPackageChild(
		isSkeleton ? TerraEngine::Editor::Assets::AnimationAssetKind::Skeleton
			: TerraEngine::Editor::Assets::AnimationAssetKind::Clip,
		generatedSource, AssetDatabase::Instance().GetBuildRoot(), std::move(editableName),
		destinationFolder.generic_string(), extracted);
	if(status != TerraEngine::Editor::Animation::AnimationAuthoringStatus::Ok)
	{
		m_AnimationLifecycleMessage = m_AnimationAssetActions.LastDiagnostic();
		return;
	}
	// The catalog creates the durable bytes. AssetDatabase owns registration and
	// the companion .meta identity, so import it only after the atomic publish.
	AssetDatabase::Instance().ImportAssetWithProgress(extracted.sourcePath, true);
	m_Owner->SetSelectedAsset(std::filesystem::absolute(extracted.sourcePath));
	m_AnimationLifecycleMessage = m_AnimationAssetActions.LastDiagnostic();
}

void ProjectBrowserWindow::DrawAnimationAssetContextActions(const std::filesystem::path& path)
{
	if(!TerraEngine::Editor::Assets::AnimationAssetActions::KindForPath(path)) return;
	TerraEngine::Editor::Assets::AnimationAssetEntry tracked;
	if(m_AnimationAssetActions.EnsureTracked(path, tracked)
		!= TerraEngine::Editor::Animation::AnimationAuthoringStatus::Ok) {
		ImGui::TextDisabled("Invalid animation document");
		return;
	}
	if(ImGui::MenuItem("Open / Edit")) OpenAnimationAsset(path);
	if(ImGui::MenuItem("Duplicate")) {
		TerraEngine::Editor::Assets::AnimationAssetEntry copy;
		const auto status = m_AnimationAssetActions.Duplicate(
			tracked.id, tracked.name + " Copy", copy);
		m_AnimationLifecycleMessage = status == TerraEngine::Editor::Animation::AnimationAuthoringStatus::Ok
			? "Animation asset duplicated." : "Animation asset duplication failed.";
	}
	if(ImGui::MenuItem("Rename")) {
		m_AnimationLifecycleAssetId = tracked.id;
		m_AnimationLifecycleModal = AnimationLifecycleModal::Rename;
		std::fill(m_AnimationLifecycleValue.begin(), m_AnimationLifecycleValue.end(), '\0');
		std::copy_n(tracked.name.data(), std::min(tracked.name.size(), m_AnimationLifecycleValue.size() - 1U),
			m_AnimationLifecycleValue.begin());
	}
	if(ImGui::MenuItem("Move")) {
		m_AnimationLifecycleAssetId = tracked.id;
		m_AnimationLifecycleModal = AnimationLifecycleModal::Move;
		std::fill(m_AnimationLifecycleValue.begin(), m_AnimationLifecycleValue.end(), '\0');
		std::copy_n(tracked.folder.data(), std::min(tracked.folder.size(), m_AnimationLifecycleValue.size() - 1U),
			m_AnimationLifecycleValue.begin());
	}
	const bool validatingThisAsset = m_AnimationValidationRequest
		&& m_AnimationValidationRequest->assetId == tracked.id
		&& m_AnimationValidation->running.load(std::memory_order_acquire);
	if(ImGui::MenuItem(validatingThisAsset ? "Cancel Validation" : "Validate")) {
		if(validatingThisAsset) {
			(void)m_AnimationAssetActions.CancelValidation(*m_AnimationValidationRequest);
			m_AnimationValidationRequest.reset();
			m_AnimationLifecycleMessage = "Animation validation canceled; late worker output will be stale.";
		} else if(!m_AnimationValidation->running.load(std::memory_order_acquire)) {
			TerraEngine::Editor::Assets::AnimationAssetValidationRequest request;
			if(m_AnimationAssetActions.BeginValidation(tracked.id, request)
				== TerraEngine::Editor::Animation::AnimationAuthoringStatus::Ok) {
				m_AnimationValidationRequest = request;
				m_AnimationValidation->running.store(true, std::memory_order_release);
				const std::shared_ptr<AnimationValidationWorkerState> shared = m_AnimationValidation;
				auto work = [shared, request]() {
					auto result = TerraEngine::Editor::Assets::AnimationAssetActions::ValidateCandidate(request);
					std::scoped_lock lock(shared->mutex);
					shared->completed = std::move(result);
				};
				if(ThreadPool::instance().getThreadCount() == 0U) work();
				else ThreadPool::instance().enqueue(ThreadPool::instance().createTask(std::move(work)), 1U);
			}
		}
	}
	ImGui::Separator();
}

void ProjectBrowserWindow::PollAnimationValidation()
{
	std::optional<TerraEngine::Editor::Assets::AnimationAssetValidationResult> completed;
	{
		std::scoped_lock lock(m_AnimationValidation->mutex);
		if(m_AnimationValidation->completed) {
			completed = std::move(m_AnimationValidation->completed);
			m_AnimationValidation->completed.reset();
		}
	}
	if(!completed) return;
	const auto status = m_AnimationAssetActions.CompleteValidation(*completed);
	if(status != TerraEngine::Editor::Animation::AnimationAuthoringStatus::Stale) {
		m_AnimationLifecycleMessage = completed->diagnostic;
	}
	m_AnimationValidationRequest.reset();
	m_AnimationValidation->running.store(false, std::memory_order_release);
}

void ProjectBrowserWindow::DrawAnimationLifecycleModal()
{
	if(m_AnimationLifecycleModal == AnimationLifecycleModal::None) return;
	const char* title = m_AnimationLifecycleModal == AnimationLifecycleModal::Rename
		? "Rename Animation Asset" : "Move Animation Asset";
	ImGui::OpenPopup(title);
	if(!ImGui::BeginPopupModal(title, nullptr, ImGuiWindowFlags_AlwaysAutoResize)) return;
	ImGui::InputText(m_AnimationLifecycleModal == AnimationLifecycleModal::Rename ? "Name" : "Assets-relative folder",
		m_AnimationLifecycleValue.data(), m_AnimationLifecycleValue.size());
	if(ImGui::Button("Apply", ImVec2(120.0F, 0.0F))) {
		const auto status = m_AnimationLifecycleModal == AnimationLifecycleModal::Rename
			? m_AnimationAssetActions.Rename(m_AnimationLifecycleAssetId, m_AnimationLifecycleValue.data())
			: m_AnimationAssetActions.Move(m_AnimationLifecycleAssetId, m_AnimationLifecycleValue.data());
		m_AnimationLifecycleMessage = status == TerraEngine::Editor::Animation::AnimationAuthoringStatus::Ok
			? "Animation lifecycle action applied." : "Animation lifecycle action failed.";
		if(status == TerraEngine::Editor::Animation::AnimationAuthoringStatus::Ok) {
			m_AnimationLifecycleModal = AnimationLifecycleModal::None;
			ImGui::CloseCurrentPopup();
		}
	}
	ImGui::SameLine();
	if(ImGui::Button("Cancel", ImVec2(120.0F, 0.0F))) {
		m_AnimationLifecycleModal = AnimationLifecycleModal::None;
		ImGui::CloseCurrentPopup();
	}
	ImGui::EndPopup();
}

// ProjectBrowserWindow::Reset is an implementation entry point; keep its inputs, side effects, and ownership expectations aligned with callers.
void ProjectBrowserWindow::Reset() {
	if(m_TerrainCreationCancellation)
		m_TerrainCreationCancellation->store(true, std::memory_order_release);
	m_CurrentDirectory.clear();
	m_CollapsedPackageGroups.clear();
	m_InitializedPackageGroups.clear();
	m_SelectedPackageChildGuid.clear();
	// A project owns its own animation catalog and inverse history. Worker packets
	// are value-only, so a late completion against this fresh catalog is stale.
	m_AnimationAssetActions = TerraEngine::Editor::Assets::AnimationAssetActions{};
	m_AnimationValidationRequest.reset();
	m_AnimationLifecycleModal = AnimationLifecycleModal::None;
	m_AnimationLifecycleMessage.clear();
}
