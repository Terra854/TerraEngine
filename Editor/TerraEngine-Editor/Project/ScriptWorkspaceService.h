// ScriptWorkspaceService.h — Managed code workspace management
//
// Defines the service responsible for maintaining the on-disk C# solution and project files for a game project. it ensures that the Assembly-CSharp workspace is correctly configured with required engine references and provides mechanisms for launching external IDEs.

#pragma once

#include "ManagedReadinessManifest.h"

#include <filesystem>
#include <string>
#include <vector>

enum class ScriptWorkspaceHealth
{
	Missing,
	Healthy,
	Broken
};


struct ScriptWorkspaceStatus
{
	// "Missing" means no project is open yet, "Healthy" means the generated
	// files and copied references line up with the active project root, and
	// "Broken" means TerraEngine found a recoverable workspace problem.
	ScriptWorkspaceHealth health = ScriptWorkspaceHealth::Missing;
	std::filesystem::path solutionPath;
	std::filesystem::path projectPath;
	std::filesystem::path outputRoot;
	std::filesystem::path referenceRoot;
	std::string lastMessage;
};

// ScriptTemplateDescriptor is the complete allowlist exposed to automation.
// Callers receive semantic IDs only; template source paths and raw contents
// remain editor-owned so this API cannot become an arbitrary file writer.
struct ScriptTemplateDescriptor
{
	std::string id;
	std::string displayName;
	std::string description;
};

// ScriptCreationRequest describes the constrained inputs accepted by the
// editor-owned script publisher. destination is always project-relative and
// must resolve beneath Assets, including after symlink resolution.
struct ScriptCreationRequest
{
	std::string templateId;
	std::string typeName;
	std::string namespaceName;
	std::filesystem::path destination;
};

struct ScriptCreationResult
{
	std::filesystem::path projectRelativePath;
	std::filesystem::path absolutePath;
	std::string qualifiedTypeName;
};

// Owns the on-disk Assembly-CSharp workspace that the editor regenerates for
// every game project. The service keeps file generation deterministic so IDEs
// always open a stable solution and project layout.
class ScriptWorkspaceService
{
public:
	struct EditorCommandContext
	{
		// Paths used by command templates. Tests and runtime code share this
		// shape so placeholder expansion stays consistent across platforms.
		std::filesystem::path solutionPath;
		std::filesystem::path projectPath;
		std::filesystem::path scriptPath;
	};

	void SetProjectRoot(const std::filesystem::path& projectRoot);

	[[nodiscard]] bool HasProjectRoot() const;
	[[nodiscard]] const ScriptWorkspaceStatus& GetStatus() const;

	[[nodiscard]] std::filesystem::path GetProjectRoot() const;
	[[nodiscard]] std::filesystem::path GetSolutionPath() const;
	[[nodiscard]] std::filesystem::path GetProjectPath() const;
	[[nodiscard]] std::filesystem::path GetOutputRoot() const;
	[[nodiscard]] std::filesystem::path GetReferenceRoot() const;
	[[nodiscard]] std::filesystem::path GetAssemblyPath() const;
	[[nodiscard]] std::filesystem::path GetManagedReadinessManifestPath() const;

	// Computes the deterministic cache key for one requested build profile.
	// Callers validate this key before direct-loading a prior Assembly-CSharp DLL.
	[[nodiscard]] ManagedReadinessInputs BuildManagedReadinessInputs(const std::string& buildProfile) const;
	[[nodiscard]] ManagedReadinessValidation ValidateManagedReadiness(
		const std::string& buildProfile,
		ManagedReadinessManifest* outManifest = nullptr,
		std::string* outError = nullptr) const;
	bool PublishManagedReadiness(const std::string& buildProfile, std::string* outError = nullptr) const;

	bool EnsureWorkspace(std::string* outError = nullptr);
	bool RegenerateWorkspace(std::string* outError = nullptr);
	[[nodiscard]] std::vector<ScriptTemplateDescriptor> ListApprovedTemplates() const;
	bool CreateApprovedScript(const ScriptCreationRequest& request, ScriptCreationResult& outResult, std::string* outError = nullptr);
	bool SynchronizeWorkspace(ScriptWorkspaceStatus& outStatus, std::string* outError = nullptr);
	bool CreateScriptFromTemplate(const std::filesystem::path& destinationDirectory, const std::string& className, std::filesystem::path& outCreatedPath, std::string* outError = nullptr);
	bool OpenScriptInExternalEditor(const std::filesystem::path& scriptPath, const std::string& commandTemplate, std::string* outError = nullptr);
	bool SyncManagedReferences(std::string* outError = nullptr);

	[[nodiscard]] static std::string QuoteCommandArgument(const std::filesystem::path& path);
	[[nodiscard]] static std::string ExpandEditorCommandTemplate(std::string commandTemplate, const EditorCommandContext& context);
	[[nodiscard]] static std::string BuildDefaultEditorCommand(const std::filesystem::path& targetPath);
	// Returns true only when a readable source DLL/PDB/analyzer differs from
	// the project copy. This lets validation prove repeated warm opens avoid
	// redundant reference writes without trusting file timestamps.
	[[nodiscard]] static bool RequiresManagedReferenceCopy(const std::filesystem::path& source, const std::filesystem::path& target);

private:
	// These helpers are kept private so every caller goes through
	// EnsureWorkspace/RegenerateWorkspace and gets one consistent validation path.
	bool WriteWorkspaceFiles(std::string* outError);
	bool WriteTextIfChanged(const std::filesystem::path& path, const std::string& contents, std::string* outError) const;
	bool CopyManagedReferenceIfChanged(const std::filesystem::path& source, const std::filesystem::path& target, std::string* outError) const;

	[[nodiscard]] std::filesystem::path ResolveTemplateRoot() const;
	[[nodiscard]] std::filesystem::path ResolveManagedBridgePath() const;
	[[nodiscard]] std::filesystem::path ResolveManagedAnalyzerPath() const;
#if defined(_WIN32)
	[[nodiscard]] std::filesystem::path ResolveVisualStudioPath() const;
	[[nodiscard]] bool LaunchVisualStudioWorkspace(const std::filesystem::path& scriptPath, std::string* outError) const;
#endif
	[[nodiscard]] std::string GetProjectName() const;
	[[nodiscard]] std::string LoadTemplate(const std::filesystem::path& path) const;
	[[nodiscard]] bool LaunchCommand(const std::string& commandTemplate, const std::filesystem::path& scriptPath, std::string* outError) const;
	[[nodiscard]] bool LaunchDefaultEditor(const std::filesystem::path& targetPath, std::string* outError) const;

	// Absolute normalized root of the currently open game project.
	std::filesystem::path m_ProjectRoot;
	ScriptWorkspaceStatus m_Status;
};
