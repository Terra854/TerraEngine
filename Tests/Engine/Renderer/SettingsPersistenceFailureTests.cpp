// Actual public settings owners and their real error-log contract.
#include "TestHarness.h"
#include <stdexcept>
#include <string_view>
#include "Renderer/RenderSystem.h"
#include "Resource/ResourceManager.h"
#include "Core/Logger/Logger.h"
#include "Utilities/OwnedProcess.h"
#include "Tests/Support/ProcessCommandLine.h"
#include "Utilities/PlatformUtils.h"
#include "Tests/Support/ReviewRemediation/RenderSettingsFailureHooks.h"
#include <atomic>
#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <nlohmann/json.hpp>

namespace TerraEngine::Tests
{
	namespace
	{
		// Retain each invocation separately. Exclusive directory creation plus a
		// monotonic time/counter suffix prevents concurrent/repeated evidence writes
		// from replacing an earlier native receipt; collisions retry without deletion.
		std::filesystem::path FreshEvidenceDirectory(const std::filesystem::path& parent, std::string_view label) {
			static std::atomic_uint64_t next{0};
			std::filesystem::create_directories(parent);
			for (unsigned attempt = 0; attempt < 100; ++attempt) {
				const auto path = parent / (std::string(label) + "-"
					+ std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()) + "-"
					+ std::to_string(next.fetch_add(1u, std::memory_order_relaxed)));
				if (path.parent_path() != parent) throw std::runtime_error("Evidence label must remain within its owned parent.");
				if (std::filesystem::create_directory(path)) return path;
			}
			throw std::runtime_error("Could not reserve a fresh evidence directory.");
		}

		bool EnterSettingsChild(TestContext& context, const std::string& name)
		{
			const char* child = std::getenv("TERRAENGINE_SETTINGS_REVIEW_CHILD");
			if (child && name == child) return true;
			const auto executable = PlatformUtils::GetExecutablePath();
			Utilities::OwnedProcessRequest request;
			request.commandLine = QuoteProcessPath(executable);
			request.workingDirectory = executable.parent_path(); request.timeout = std::chrono::seconds(30);
			request.environment = {{"TERRAENGINE_SETTINGS_REVIEW_CHILD", name}, {"TERRAENGINE_TEST_FILTER", name},
				{"TERRAENGINE_TEST_EXCLUDE_FILTER", ""}, {"TERRAENGINE_VULKAN_REVIEW_ENABLE", "0"}, {"TERRAENGINE_VULKAN_REVIEW_CHILD", ""}};
			const std::atomic_bool cancelled{false};
			const auto result = Utilities::OwnedProcess::Run(request, cancelled);
			context.Expect(result.started && !result.timedOut && result.exitCode == 0,
				"Actual settings failure child must settle normally: " + result.output);
			return false;
		}
		void ExerciseFailure(TestContext& context, const std::string& name, bool save)
		{
			if (!EnterSettingsChild(context, name)) return;
			const auto parent = FreshEvidenceDirectory(std::filesystem::path(TERRAENGINE_SOURCE_DIR) / "out/validation/review-remediation/settings-persistence", "failure-child");
			std::filesystem::create_directories(parent);
			const auto root = parent / (name + "-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
			context.Expect(root.parent_path() == parent && !std::filesystem::exists(root), "Settings fixture must own a fresh bounded root.");
			std::filesystem::create_directories(root / "Assets");
			struct Cleanup
			{
				std::filesystem::path root;
				~Cleanup() { Review::SetRenderSettingsFailureHook(nullptr); std::error_code ignored; std::filesystem::remove_all(root, ignored); }
			} cleanup{root};
			auto& resources = ResourceManager::Instance(); auto& renderer = RenderSystem::Instance();
			// The exact-case child owns all singleton startup/root configuration.
			// No developer root is modified and no GPU/context is initialized.
			resources.SetAssetRoot(root / "Assets");
			resources.ConfigureEditorStartupProfile(renderer.GetRenderSettings(), false, false, root, "SettingsPersistenceFailureTests");
			context.Expect(resources.GetProjectRoot() == root, "The actual owner must resolve the isolated project root.");
			std::atomic_uint32_t errors{0};
			const auto sink = Logger::Instance().AddSink([&](const LogEntry& entry) {
				if (entry.level == LogLevel::Error && entry.file.find("RenderSystemSettings.cpp") != std::string::npos
					&& entry.message.find(save ? "Failed to save RenderSettings" : "Failed to load RenderSettings") != std::string::npos)
					errors.fetch_add(1, std::memory_order_relaxed);
			});
			struct RemoveSink { Logger::SinkId id; ~RemoveSink() { Logger::Instance().RemoveSink(id); } } remove{sink};
			const auto authored = root / "ProjectSettings/RenderSettings.json";
			const std::string authoredBytes = "{\"preserveOnReadFailure\":true}";
			if(!save) { std::filesystem::create_directories(authored.parent_path()); std::ofstream(authored, std::ios::binary) << authoredBytes; }
			Review::RenderSettingsFailureHook hook;
			hook.operation = save ? Review::SettingsFilesystemOperation::CreateDirectories : Review::SettingsFilesystemOperation::Exists;
			context.Expect(Review::SetRenderSettingsFailureHook(&hook), "Settings filesystem fault hook must be compiled and admitted.");
			bool escaped = false; std::string exception;
			try { if (save) renderer.SaveSettings(); else renderer.LoadSettings(); }
			catch (const std::exception& failure) { escaped = true; exception = failure.what(); }
			catch (...) { escaped = true; exception = "non-standard exception"; }
			Review::SetRenderSettingsFailureHook(nullptr);
			if(!save) { std::ifstream input(authored, std::ios::binary); const std::string actual{std::istreambuf_iterator<char>(input), {}}; context.Expect(actual == authoredBytes, "Transient load failure must preserve the existing settings bytes."); }
			// Gather the subsequent-operation proof even on the known pre-fix red;
			// the assertion never hides a failed prerequisite behind missing data.
			renderer.SaveSettings(); const auto settings = root / "ProjectSettings/RenderSettings.json";
			std::ifstream input(settings); nlohmann::json persisted;
			if (input) persisted = nlohmann::json::parse(input, nullptr, false);
			const bool healthy = persisted.is_object() && persisted.contains("settings")
				&& persisted.at("settings").is_object() && persisted.at("settings").contains("maxDynamicLights")
				&& persisted.at("settings").at("maxDynamicLights").get<std::uint32_t>() == renderer.GetRenderSettings().maxDynamicLights;
			std::ofstream(parent / (name + ".json")) << nlohmann::json{{"scope", "ActualPublicSettingsOwner_ControlledFilesystemError"},
				{"operation", save ? "Save.CreateDirectories" : "Load.Exists"}, {"faultReached", hook.reached}, {"exceptionEscaped", escaped},
				{"exception", exception}, {"errorLogs", errors.load()}, {"healthyRetryPersisted", healthy}, {"gpuInitialized", false}}.dump(2);
			context.Expect(hook.reached == 1, "The one-shot failure must reach the actual filesystem call boundary exactly once.");
			context.Expect(!escaped && errors.load() >= 1, "Filesystem failure must stay inside the owner and produce its explicit error report.");
			context.Expect(healthy, "A healthy subsequent owner operation must persist actual settings.");
		}
	}
	void RegisterSettingsPersistenceFailureTests(TestRegistry& registry)
	{
		registry.Add("Renderer.Review.SettingsPersistence.SaveCreateFailureIsContained", [](TestContext& context) {
			ExerciseFailure(context, "Renderer.Review.SettingsPersistence.SaveCreateFailureIsContained", true);
		});
		registry.Add("Renderer.Review.SettingsPersistence.LoadExistsFailureIsContained", [](TestContext& context) {
			ExerciseFailure(context, "Renderer.Review.SettingsPersistence.LoadExistsFailureIsContained", false);
		});
	}
}
