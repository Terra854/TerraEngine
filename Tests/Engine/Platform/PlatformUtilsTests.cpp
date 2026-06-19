#include "TestHarness.h"

#include "Utilities/PlatformUtils.h"

#include <filesystem>
#include <string>

namespace TerraEngine::Tests {
	namespace {
		const char* ExpectedPlatformName() {
#if defined(_WIN32)
			return "Windows";
#elif defined(__linux__)
			return "Linux";
#else
			return "Unknown";
#endif
		}
	}

	void RegisterPlatformUtilsTests(TestRegistry& registry) {
		registry.Add("Engine.PlatformUtils.ReportsCurrentPlatform", [](TestContext& context) {
			const RuntimePlatform platform = PlatformUtils::GetRuntimePlatform();
#if defined(_WIN32)
			context.Expect(platform == RuntimePlatform::Windows, "Windows builds should report RuntimePlatform::Windows.");
#elif defined(__linux__)
			context.Expect(platform == RuntimePlatform::Linux, "Linux builds should report RuntimePlatform::Linux.");
#else
			context.Expect(platform == RuntimePlatform::Unknown || platform == RuntimePlatform::macOS, "Unknown host builds should not report Windows or Linux accidentally.");
#endif
			context.Expect(std::string(ExpectedPlatformName()).size() > 0u, "The test should name the expected platform for diagnostics.");
		});

		registry.Add("Engine.PlatformUtils.PersistentDataPathSanitizesProductName", [](TestContext& context) {
			const std::filesystem::path dataPath = PlatformUtils::GetPersistentDataPath("Terra/Engine:Linux*Smoke?");
			const std::string genericPath = dataPath.generic_string();

			context.Expect(!genericPath.empty(), "Persistent data path should resolve to a non-empty path.");
			context.Expect(genericPath.find("Terra_Engine_Linux_Smoke_") != std::string::npos, "Invalid path characters should be replaced for portable save-data folders.");
			context.Expect(std::filesystem::exists(dataPath), "Persistent data path should be created before it is returned.");
		});

		registry.Add("Engine.PlatformUtils.OpenUrlRejectsMalformedInput", [](TestContext& context) {
			context.Expect(!PlatformUtils::OpenUrl("not a url"), "OpenUrl should reject strings without a URL scheme.");
			context.Expect(!PlatformUtils::OpenUrl(""), "OpenUrl should reject empty strings without launching platform handlers.");
		});
	}
}
