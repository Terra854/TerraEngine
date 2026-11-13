// Thin reuse of RuntimeMemorySafetyRendererFixture's installed Engine bootstrap.
// One watched child owns one actual renderer/window; this never borrows the
// offscreen VulkanReviewFixture device or replaces another context's dispatch.
#pragma once
#include "Core/MainEngine.h"
#include "Tests/Support/ReviewRemediation/VulkanReviewFixture.h"
#include <fstream>
#include <nlohmann/json.hpp>
#include "Core/Logger/Logger.h"
#include "Renderer/RenderSystem.h"
#include "Renderer/RHI/Vulkan/VulkanContext.h"
#include "Resource/ResourceManager.h"
#include "Window/WindowSystem.h"
#include <GLFW/glfw3.h>
#include <chrono>
#include <atomic>
#include <cstdlib>
#include <filesystem>
#include <stdexcept>

namespace TerraEngine::Tests::Review
{
	// Retain each invocation separately. Exclusive directory creation plus a
	// monotonic time/counter suffix prevents concurrent/repeated evidence writes
	// from replacing an earlier native receipt; collisions retry without deletion.
	inline std::filesystem::path FreshNativeReviewEvidenceDirectory(const std::filesystem::path& parent, std::string_view label) {
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
	// Persist exact subprocess status before the parent assertion. Missing child
	// harness output must not hide an access violation, timeout or launch failure.
	inline void RecordNativeReviewChild(std::string_view name, const VulkanReviewChild& child)
	{
		const auto root = FreshNativeReviewEvidenceDirectory(std::filesystem::path(TERRAENGINE_SOURCE_DIR) / "out/validation/review-remediation/native-child-status", name);
		const auto stem = root / "child";
		std::ofstream(stem.string() + ".stdout.log") << child.output;
		std::ofstream(stem.string() + ".json") << nlohmann::json{{"case",name},{"started",child.started},{"timedOut",child.timedOut},
			{"exitCodeSigned",child.exitCode},{"exitCodeBits",static_cast<std::uint32_t>(child.exitCode)}}.dump(2);
	}
	class ExistingRendererFixtureSession final
	{
	public:
		explicit ExistingRendererFixtureSession(std::string_view caseName)
		{
			const char* configured = std::getenv("TERRAENGINE_REVIEW_INSTALL_ROOT");
			if (!configured) throw std::runtime_error("Native prerequisite missing: explicit installed Build root.");
			install = std::filesystem::path(configured);
#ifdef _WIN32
			const auto hostLibrary = "hostfxr.dll";
#else
			const auto hostLibrary = "libhostfxr.so";
#endif
			if (!install.is_absolute() || !std::filesystem::is_regular_file(install / "Editor/Managed" / hostLibrary))
				throw std::runtime_error("Native prerequisite missing: absolute installed bundled Engine/Managed closure.");
			root = FreshNativeReviewEvidenceDirectory(std::filesystem::path(TERRAENGINE_SOURCE_DIR) / "out/validation/review-remediation/native-renderer-sessions", caseName);
			std::filesystem::create_directories(root / "Build/Assets"); std::filesystem::create_directories(root / "Assets/Scenes");
			workingDirectory = std::filesystem::current_path(); std::filesystem::current_path(install);
			validationSink = Logger::Instance().AddSink([this](const LogEntry& entry) {
				if (entry.level == LogLevel::Error && entry.message.find("[Vulkan] ") != std::string::npos) validationErrors.fetch_add(1);
			});
			try
			{
				auto& resources = ResourceManager::Instance(); resources.SetEngineRoot(install / "Editor"); resources.SetAssetRoot(root / "Build/Assets");
				resources.ConfigureEditorStartupProfile(RenderSettings{}, true, false, root, "Review native lifetime fixture");
				engine.Initialize();
				context = dynamic_cast<VulkanContext*>(RenderSystem::Instance().GetContext());
				if (!context) throw std::runtime_error("Native prerequisite missing: actual Vulkan context.");
				const auto receipt = context->GetEmptyFrameValidationReceipt();
				if (!receipt.initialized || !receipt.validationActuallyEnabled) throw std::runtime_error("Native prerequisite missing: initialized context with actual validation.");
				if (auto* window = static_cast<GLFWwindow*>(WindowSystem::Instance().GetNativeHandle())) glfwHideWindow(window);
				else throw std::runtime_error("Native prerequisite missing: actual presentation window.");
			}
			catch (...) { try { engine.Shutdown(); } catch (...) {} Logger::Instance().RemoveSink(validationSink); std::error_code ignored; std::filesystem::current_path(workingDirectory, ignored); throw; }
		}
		~ExistingRendererFixtureSession() noexcept
		{
			try { engine.Shutdown(); } catch (...) {}
			Logger::Instance().RemoveSink(validationSink);
			std::error_code ignored; std::filesystem::current_path(workingDirectory, ignored);
			// The root is retained as evidence. It never aliases a developer project.
		}
		VulkanContext& Native() const { return *context; }
		bool Update(float deltaTime) { return engine.Update(deltaTime); }
		void CloseRenderer() { RenderSystem::Instance().Shutdown(); }
		std::uint32_t ValidationErrors() const { return validationErrors.load(); }
		const std::filesystem::path& Artifacts() const { return root; }
		const std::filesystem::path& Install() const { return install; }
	private:
		Engine engine;
		VulkanContext* context = nullptr;
		std::filesystem::path install, root, workingDirectory;
		Logger::SinkId validationSink = 0;
		std::atomic_uint32_t validationErrors{0};
	};
}
