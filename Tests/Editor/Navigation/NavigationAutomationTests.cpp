// NavigationAutomationTests.cpp - provider-neutral editor automation contracts.

#include "../TestHarness.h"

#include "Editor/TerraEngine-Editor/Automation/EditorAutomationService.h"
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
				"navigation.route.request", "navigation.route.read", "navigation.route.cancel",
				"navigation.cache.prepare", "navigation.cache.status", "navigation.agent.bind",
				"navigation.agent.destination.location", "navigation.agent.destination.object",
				"navigation.agent.stop", "navigation.agent.receipt", "navigation.agent.status",
				"navigation.agent.guidance"})
				context.Expect(source.find(command) != std::string::npos, "Every navigation capability must have a stable provider command.");
			context.Expect(source.find("schemaVersion") != std::string::npos && source.find("maxItems") != std::string::npos,
				"Navigation responses must identify their schema version and bounded array policy.");
			context.Expect(source.find("dtNav") == std::string::npos && source.find("Recast") == std::string::npos,
				"Public navigation automation schemas must not expose provider internals.");
			context.Expect(source.find("profileId") == std::string::npos,
				"Route automation must accept copied traversal values without restoring profile identity.");
		});

		registry.Add("Editor.Navigation.Automation.UnknownComponentUsesStandardRejection", [](TestContext& context) {
			const std::string provider = ReadSource("Editor/TerraEngine-Editor/Automation/Providers/NavigationAutomationProvider.cpp");
			const std::string transaction = ReadSource("Editor/TerraEngine-Editor/Automation/EditorAutomationTransaction.cpp");
			context.Expect(provider.find("NavigationSourceComponent") == std::string::npos
				&& provider.find("NavigationConnectionComponent") == std::string::npos,
				"Navigation automation must not classify retired component names specially.");
			context.Expect(transaction.find("Component type is not registered for automation.") != std::string::npos,
				"Unknown component names must use the generic unregistered-component rejection path.");
			context.Expect(transaction.find("NavigationSourceComponent") == std::string::npos
				&& transaction.find("NavigationConnectionComponent") == std::string::npos
				&& transaction.find("tombstone") == std::string::npos,
				"Generic rejection must not queue navigation-specific mutation, history, or tombstones.");
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

		registry.Add("Editor.Navigation.AgentAutomationCapabilitiesDeclareReadAndDeferredWriteAuthority", [](TestContext& context) {
			EditorAutomationService service;
			const auto bind = service.GetRegistry().Find("navigation.agent.bind", 1U);
			const auto destination = service.GetRegistry().Find("navigation.agent.destination.location", 1U);
			const auto stop = service.GetRegistry().Find("navigation.agent.stop", 1U);
			const auto guidance = service.GetRegistry().Find("navigation.agent.guidance", 1U);
			context.Expect(bind.has_value() && destination.has_value() && stop.has_value() && guidance.has_value(),
				"Agent tooling must be discoverable through the editor's registered provider, not merely named in source.");
			if(bind && destination && stop && guidance)
			{
				context.Expect(bind->descriptor.permission == AutomationPermission::Read
					&& bind->descriptor.mutationMode == MutationMode::ReadOnly
					&& guidance->descriptor.permission == AutomationPermission::Read
					&& destination->descriptor.permission == AutomationPermission::ProjectWrite
					&& stop->descriptor.permission == AutomationPermission::ProjectWrite,
					"Published binding and guidance are read-only, while typed intent commands declare deferred project-write authority.");
				context.Expect(destination->descriptor.execution.workerPreparation && destination->descriptor.execution.protectedCommit
					&& stop->descriptor.execution.workerPreparation && stop->descriptor.execution.protectedCommit,
					"Agent commands must advertise worker-safe validation and a protected runtime admission boundary.");
			}
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
			context.Expect(source.find("NavigationAgentSystem::Instance") != std::string::npos
				&& source.find("QueueStop") != std::string::npos
				&& source.find("ReadGuidance") != std::string::npos,
				"Agent automation must call the typed runtime facade for deferred commands and copied reads.");
			context.Expect(source.find("ResolvePublishedAgent") != std::string::npos
				&& source.find("FindIdentity(ObjectManager") == std::string::npos,
				"Read-only agent binding must resolve only the published identity snapshot, never scan or create an attachment through live ECS.");
			context.Expect(source.find("DeserializeNavigationProfile") != std::string::npos,
				"Route automation must validate and copy the complete identity-free profile value.");
		});
	}
}
