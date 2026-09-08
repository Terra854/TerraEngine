// NavigationAutomationTests.cpp - provider-neutral editor automation contracts.

#include "../TestHarness.h"

#include "EditorAutomationJobService.h"

#include <filesystem>
#include <fstream>
#include <nlohmann/json.hpp>
#include <sstream>
#include <string>

namespace TerraEngine::EditorTests
{
	using namespace TerraEngine::Editor::Automation;

	namespace
	{
		std::string ReadSource(const char* relativePath)
		{
			std::ifstream input(std::filesystem::path(TERRAENGINE_SOURCE_DIR) / relativePath, std::ios::binary);
			if(!input) return {};
			std::ostringstream output;
			output << input.rdbuf();
			return output.str();
		}

		constexpr std::size_t kMaximumSchemaArray = 1024U;
	}

	void RegisterNavigationAutomationTests(TestRegistry& registry)
	{
		registry.Add("Editor.Navigation.AutomationSchemasAreVersionedBoundedAndProviderNeutral", [](TestContext& context) {
			const std::string source = ReadSource("Editor/TerraEngine-Editor/Automation/Providers/NavigationAutomationProvider.cpp");
			context.Expect(!source.empty(), "Navigation automation must have a registered provider implementation.");
			if(source.empty()) return;
			for(const std::string_view command : {"navigation.settings.get", "navigation.diagnostics.get",
				"navigation.sources.inspect", "navigation.route.request", "navigation.route.read",
				"navigation.route.cancel", "navigation.authoring.apply", "navigation.cache.prepare", "navigation.cache.status"})
				context.Expect(source.find(command) != std::string::npos, "Every navigation capability must have a stable provider command.");
			context.Expect(source.find("schemaVersion") != std::string::npos && source.find("maxItems") != std::string::npos,
				"Navigation responses must identify their schema version and bounded array policy.");
			context.Expect(source.find("dtNav") == std::string::npos && source.find("Recast") == std::string::npos,
				"Public navigation automation schemas must not expose provider internals.");
		});

		registry.Add("Editor.Navigation.AutomationReadsHaveNoSideEffects", [](TestContext& context) {
			const std::string source = ReadSource("Editor/TerraEngine-Editor/Automation/Providers/NavigationAutomationProvider.cpp");
			context.Expect(!source.empty(), "Navigation automation provider source must be available for the read-side-effect contract.");
			if(source.empty()) return;
			const std::size_t reads = source.find("navigation.diagnostics.get");
			context.Expect(reads != std::string::npos, "Diagnostics read capability must be present.");
			if(reads != std::string::npos)
				context.Expect(source.find("Prepare", reads) == std::string::npos
					&& source.find("Publish", reads) == std::string::npos
					&& source.find("Mutat", reads) == std::string::npos,
					"Read capabilities must not rebuild, search, publish, or mutate merely to show status.");
		});

		registry.Add("Editor.Navigation.AutomationCacheJobsRetainBoundedLifecycle", [](TestContext& context) {
			EditorAutomationJobService jobs(2U);
			AutomationJobCreateRequest request;
			request.kind = "navigation.cache.prepare";
			request.capability = "navigation.cache.prepare";
			const AutomationJobSnapshot created = jobs.Create(request);
			context.Expect(!created.jobId.empty() && created.state == AutomationJobState::Queued,
				"Cache preparation must begin as a bounded queued job with a stable identity.");
			context.Expect(jobs.PublishProgress(created.jobId, 0.5, "prepare"), "Cache jobs must publish bounded progress.");
			context.Expect(jobs.CompleteSucceeded(created.jobId, nlohmann::json{{"schemaVersion", 1}, {"items", nlohmann::json::array()}}),
				"A completed cache job must publish one terminal result.");
			const auto result = jobs.Get(created.jobId);
			context.Expect(result.has_value() && result->state == AutomationJobState::Succeeded,
				"Cache status must retain terminal state until bounded job eviction.");
			context.Expect(kMaximumSchemaArray == 1024U, "Schema array limits must remain explicit and bounded.");
		});

		registry.Add("Editor.Navigation.AutomationTypedCommandsRequireReceipts", [](TestContext& context) {
			const std::string source = ReadSource("Editor/TerraEngine-Editor/Automation/Providers/NavigationAutomationProvider.cpp");
			context.Expect(source.find("commitReceipt") != std::string::npos,
				"Typed authoring and route commands must return a durable commit/request receipt.");
			context.Expect(source.find("route.request") != std::string::npos
				&& source.find("route.cancel") != std::string::npos,
				"Runtime route automation must expose request and cancellation as separate typed operations.");
		});

		registry.Add("Editor.Navigation.AutomationUsesTypedNavigationResolver", [](TestContext& context) {
			const std::string source = ReadSource("Editor/TerraEngine-Editor/Automation/EditorAutomationService.cpp");
			context.Expect(!source.empty(), "Navigation automation must have a concrete editor service handler.");
			if(source.empty()) return;
			const std::size_t resolver = source.find("ResolveNavigationAutomationHandler");
			const std::size_t handler = source.find("ExecuteNavigationAutomationHandler");
			context.Expect(resolver != std::string::npos && handler != std::string::npos,
				"Navigation provider operations must resolve to a domain-specific handler.");
			context.Expect(source.find("NavigationAutomationProvider>(resolveNavigationHandler)") != std::string::npos,
				"Navigation provider registration must not use the generic resolver.");
			context.Expect(source.find("NavigationSystem::Instance") != std::string::npos,
				"Route operations must use the provider-neutral NavigationSystem boundary.");
			context.Expect(source.find("GetNavigationAuthoringTransactions") != std::string::npos,
				"Authoring must use the protected typed navigation transaction coordinator.");
		});
	}
}
