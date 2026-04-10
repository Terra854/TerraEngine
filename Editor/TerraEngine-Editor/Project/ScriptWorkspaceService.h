#pragma once

#include <filesystem>
#include <string>

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

// Owns the on-disk Assembly-CSharp workspace that the editor regenerates for
// every game project. The service keeps file generation deterministic so IDEs
// always open a stable solution and project layout.
class ScriptWorkspaceService
{
public:
	void SetProjectRoot(const std::filesystem::path& projectRoot);

	[[nodiscard]] bool HasProjectRoot() const;
	[[nodiscard]] const ScriptWorkspaceStatus& GetStatus() const;

	[[nodiscard]] std::filesystem::path GetProjectRoot() const;
	[[nodiscard]] std::filesystem::path GetSolutionPath() const;
	[[nodiscard]] std::filesystem::path GetProjectPath() const;
	[[nodiscard]] std::filesystem::path GetOutputRoot() const;
	[[nodiscard]] std::filesystem::path GetReferenceRoot() const;
	[[nodiscard]] std::filesystem::path GetAssemblyPath() const;

	bool EnsureWorkspace(std::string* outError = nullptr);
	bool RegenerateWorkspace(std::string* outError = nullptr);
	bool CreateScriptFromTemplate(const std::filesystem::path& destinationDirectory, const std::string& className, std::filesystem::path& outCreatedPath, std::string* outError = nullptr);
	bool OpenScriptInExternalEditor(const std::filesystem::path& scriptPath, const std::string& commandTemplate, std::string* outError = nullptr);
	bool SyncManagedReferences(std::string* outError = nullptr);

private:
	// These helpers are kept private so every caller goes through
	// EnsureWorkspace/RegenerateWorkspace and gets one consistent validation path.
	bool WriteWorkspaceFiles(std::string* outError);
	bool WriteTextIfChanged(const std::filesystem::path& path, const std::string& contents, std::string* outError) const;

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
