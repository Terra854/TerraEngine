#include "Scene/SceneManager.h"

#include <algorithm>
#include <cctype>
#include <fstream>
#include <sstream>
#include <utility>

#include "Core/Logger/Logger.h"

SceneManager& SceneManager::Instance()
{
	static SceneManager instance;
	return instance;
}

SceneManager::~SceneManager() = default;

SceneManager::SceneManager()
	: threadPool(ThreadPool::instance())
{
	// Default to the process working directory so authored scene paths can be
	// relative to the project root until a caller overrides the location.
	sceneRoot = std::filesystem::current_path();
}

void SceneManager::Initialize()
{
	// Clear any stale tracking in case the manager is restarted during a hot
	// reload. At this stage nothing is loaded yet so the reset is cheap and
	// keeps stale cache entries from leaking across sessions.
	UnloadAll();
}

void SceneManager::Update()
{
	// Scene loading is handled via explicit requests and shared futures, so the
	// update step is intentionally empty. Keeping it in place preserves the
	// SystemsBase contract for future streaming logic.
}

void SceneManager::Shutdown()
{
	// Release cached JSON blobs and let any outstanding futures drop their
	// references so the thread pool can finish without holding scene data.
	UnloadAll();
}

std::string SceneManager::SystemName()
{
	return "SceneManager";
}

void SceneManager::SetSceneRoot(const std::filesystem::path& root)
{
	// Normalize immediately so repeated calls with different relative forms map
	// to the same cache keys.
	sceneRoot = root.lexically_normal();
}

std::filesystem::path SceneManager::GetSceneRoot() const
{
	return sceneRoot;
}

TerraEngine::UUID SceneManager::LoadScene(const std::filesystem::path& path)
{
	return LoadOrGetCached<SceneDocument>(path, sceneCache, [this](const std::filesystem::path& resolvedPath, SceneFileType type) {
		return LoadSceneDocument(resolvedPath, type);
	}, SceneFileType::Scene);
}

TerraEngine::UUID SceneManager::LoadPrefab(const std::filesystem::path& path)
{
	return LoadOrGetCached<SceneDocument>(path, prefabCache, [this](const std::filesystem::path& resolvedPath, SceneFileType type) {
		return LoadSceneDocument(resolvedPath, type);
	}, SceneFileType::Prefab);
}

std::shared_future<TerraEngine::UUID> SceneManager::LoadSceneAsync(const std::filesystem::path& path)
{
	return LoadOrSchedule<SceneDocument>(path, sceneCache, [this](const std::filesystem::path& resolvedPath, SceneFileType type) {
		return LoadSceneDocument(resolvedPath, type);
	}, SceneFileType::Scene);
}

std::shared_future<TerraEngine::UUID> SceneManager::LoadPrefabAsync(const std::filesystem::path& path)
{
	return LoadOrSchedule<SceneDocument>(path, prefabCache, [this](const std::filesystem::path& resolvedPath, SceneFileType type) {
		return LoadSceneDocument(resolvedPath, type);
	}, SceneFileType::Prefab);
}

const SceneDocument* SceneManager::GetScene(const TerraEngine::UUID& id) const
{
	return GetResourceById(id, sceneCache);
}

const SceneDocument* SceneManager::GetPrefab(const TerraEngine::UUID& id) const
{
	return GetResourceById(id, prefabCache);
}

bool SceneManager::SetActiveScene(const TerraEngine::UUID& id)
{
	// The active scene must already be loaded; callers can invoke the async
	// load helpers beforehand and wait on the returned future.
	if (GetScene(id) == nullptr)
	{
		return false;
	}

	activeSceneId = id;
	return true;
}

const SceneDocument* SceneManager::GetActiveScene() const
{
	if (activeSceneId == TerraEngine::EMPTY_UUID)
	{
		return nullptr;
	}

	return GetScene(activeSceneId);
}

void SceneManager::UnloadAll()
{
	// Acquire locks in a stable order to avoid deadlocks if multiple threads
	// request clears simultaneously. Each cache is isolated so scene/prefab
	// data can be reclaimed independently in the future.
	{
		std::unique_lock lock(sceneCache.mutex);
		sceneCache.pathToId.clear();
		sceneCache.resources.clear();
		sceneCache.inFlight.clear();
	}
	{
		std::unique_lock lock(prefabCache.mutex);
		prefabCache.pathToId.clear();
		prefabCache.resources.clear();
		prefabCache.inFlight.clear();
	}

	activeSceneId = TerraEngine::EMPTY_UUID;
}

std::unique_ptr<SceneDocument> SceneManager::LoadSceneDocument(const std::filesystem::path& path, SceneFileType type)
{
	const std::string jsonText = ReadJsonFile(path, type);
	if (jsonText.empty())
	{
		return nullptr;
	}

	auto document = std::make_unique<SceneDocument>();
	document->sourcePath = path;
	document->name = path.stem().string();
	document->jsonContents = jsonText;
	document->isPrefab = type == SceneFileType::Prefab;
	return document;
}

std::string SceneManager::ReadJsonFile(const std::filesystem::path& path, SceneFileType type) const
{
	if (!ValidateExtension(path, type))
	{
		TERRAENGINE_LOG(LogLevel::Error, "Rejected load for " << path << " because the extension does not match the expected scene or prefab format.");
		return {};
	}

	std::ifstream file(path, std::ios::binary);
	if (!file)
	{
		TERRAENGINE_LOG(LogLevel::Error, "Failed to open scene file: " << path);
		return {};
	}

	std::stringstream buffer;
	buffer << file.rdbuf();
	std::string jsonText = buffer.str();

	if (jsonText.empty())
	{
		TERRAENGINE_LOG(LogLevel::Error, "Scene file is empty: " << path);
		return {};
	}

	// Lightweight validation: ensure the first non-whitespace character is a
	// JSON container token. This avoids pulling in a full parser while still
	// catching obvious authoring mistakes early.
	if (!LooksLikeJson(jsonText))
	{
		TERRAENGINE_LOG(LogLevel::Error, "Scene file does not appear to be JSON. Path: " << path);
		return {};
	}

	return jsonText;
}

bool SceneManager::LooksLikeJson(const std::string& text) const
{
	for (char ch : text)
	{
		if (std::isspace(static_cast<unsigned char>(ch)))
		{
			continue;
		}

		return ch == '{' || ch == '[';
	}

	return false;
}

bool SceneManager::ValidateExtension(const std::filesystem::path& path, SceneFileType type) const
{
	std::string extension = path.extension().string();
	std::transform(extension.begin(), extension.end(), extension.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });

	if (type == SceneFileType::Scene)
	{
		return extension == ".tescene";
	}

	return extension == ".teprefab";
}

std::filesystem::path SceneManager::ResolveScenePath(const std::filesystem::path& path) const
{
	if (path.is_absolute())
	{
		return path.lexically_normal();
	}

	return (sceneRoot / path).lexically_normal();
}

std::string SceneManager::NormalizeKey(const std::filesystem::path& path) const
{
	// Using generic_string keeps path separators consistent on Windows and POSIX
	// so cache keys are stable across different callers.
	return path.lexically_normal().generic_string();
}
