// PhysicsSceneViewTests.cpp - unregistered contracts for immutable debug display.

#include "TestHarness.h"

#include "Physics/PhysicsDiagnostics.h"
#include "Physics/PhysicsSettings.h"
#include "Renderer/RenderSystem.h"
#include "Editor/TerraEngine-Editor/Windows/SceneViewOverlayTypes.h"

#include <filesystem>
#include <fstream>
#include <memory>
#include <sstream>
#include <string>
#include <vector>

namespace TerraEngine::EditorTests
{
	namespace
	{
		std::string ReadSceneViewSource()
		{
			// The deterministic editor test target does not link the interactive
			// ImGui window. Reading the maintained source still guards the exact API
			// contract that otherwise asserts only when a collider is selected live.
			const std::filesystem::path sourcePath =
				std::filesystem::path(TERRAENGINE_SOURCE_DIR) / "Editor/TerraEngine-Editor/Windows/SceneViewWindow.cpp";
			std::ifstream input(sourcePath, std::ios::binary);
			std::ostringstream buffer;
			buffer << input.rdbuf();
			return buffer.str();
		}
	}

	void RegisterPhysicsSceneViewTests(TestRegistry& registry)
	{
		registry.Add("Physics.SceneView.ColliderAuthoringUsesChildResizeFlags", [](TestContext& context) {
			const std::string source = ReadSceneViewSource();
			context.Expect(
				source.find("ImGuiChildFlags_Borders | ImGuiChildFlags_AutoResizeY") != std::string::npos,
				"Collider authoring must use the supported ImGui child border and vertical auto-size flags.");
			context.Expect(
				source.find("ImGuiWindowFlags_AlwaysAutoResize") == std::string::npos,
				"Scene View must not pass the rejected AlwaysAutoResize window flag to BeginChild.");
		});

		registry.Add("Physics.SceneView.DebugPacketsRemainValidWhilePaused", [](TestContext& context) {
			Physics::PhysicsDebugPacket packet;
			packet.world = {{1U}, 3U};
			packet.producedTick = 7U;
			packet.expiryTick = 9U;
			context.Expect(packet.IsValidFor(packet.world, 7U), "A packet must be visible at its first published paused tick.");
			context.Expect(packet.IsValidFor(packet.world, 9U), "A packet must stay visible through its configured expiry tick.");
			context.Expect(!packet.IsValidFor(packet.world, 10U), "A paused redraw must not revive expired debug packet data.");
		});

		registry.Add("Physics.SceneView.DebugPacketHistoryIsBoundedAndFiltersWorld", [](TestContext& context) {
			Physics::PhysicsDebugPacketHistory history(2U);
			const Physics::PhysicsWorldHandle worldA{{1U}, 1U};
			const Physics::PhysicsWorldHandle worldB{{2U}, 1U};
			for (Physics::PhysicsTick tick = 1U; tick <= 3U; ++tick)
			{
				auto packet = std::make_shared<Physics::PhysicsDebugPacket>();
				packet->world = worldA;
				packet->producedTick = tick;
				packet->expiryTick = 10U;
				history.Publish(packet);
			}
			context.ExpectEqual(static_cast<std::size_t>(2U), history.Size(), "History must evict the oldest packet at its hard capacity.");
			context.ExpectEqual(static_cast<std::size_t>(2U), history.Select(worldA, 3U).size(), "The active world should select retained packets in reverse publication order.");
			context.Expect(history.Select(worldB, 3U).empty(), "A recreated or other world must not display stale packets.");
		});

		registry.Add("Physics.SceneView.DebugPacketHistoryReleasesExpiredPrimitiveStorage", [](TestContext& context) {
			Physics::PhysicsDebugPacketHistory history(32U);
			const Physics::PhysicsWorldHandle worldA{{1U}, 1U};
			std::weak_ptr<const Physics::PhysicsDebugPacket> firstPacket;
			for (Physics::PhysicsTick tick = 1U; tick <= 4U; ++tick)
			{
				auto packet = std::make_shared<Physics::PhysicsDebugPacket>();
				packet->world = worldA;
				packet->producedTick = tick;
				packet->expiryTick = tick + 2U;
				packet->lines = std::make_shared<const std::vector<Physics::PhysicsDebugLine>>(1000U);
				if (tick == 1U)
				{
					firstPacket = packet;
				}
				history.Publish(std::move(packet));
			}

			context.Expect(firstPacket.expired(), "Publishing tick four must release tick one's expired shared primitive vectors immediately.");
			context.ExpectEqual(static_cast<std::size_t>(3U), history.Size(), "History ownership must follow the two-future-tick expiry window instead of growing to its emergency capacity.");

			auto replacement = std::make_shared<Physics::PhysicsDebugPacket>();
			replacement->world = {{2U}, 1U};
			replacement->producedTick = 1U;
			replacement->expiryTick = 3U;
			history.Publish(std::move(replacement));
			context.ExpectEqual(static_cast<std::size_t>(1U), history.Size(), "Publishing a replacement world must release every packet owned by the previous world.");
		});

		registry.Add("Physics.SceneView.DisplaySelectionCountsOnlyEnabledPrimitiveKinds", [](TestContext& context) {
			auto lines = std::make_shared<const std::vector<Physics::PhysicsDebugLine>>(2U);
			auto queries = std::make_shared<const std::vector<Physics::PhysicsDebugQuery>>(3U);
			auto packet = std::make_shared<Physics::PhysicsDebugPacket>();
			packet->lines = lines;
			packet->queries = queries;
			Physics::PhysicsDebugDisplaySelection selection;
			selection.showRecentQueries = false;
			context.ExpectEqual(static_cast<std::size_t>(2U), Editor::SceneViewOverlay::CountPhysicsDebugPrimitives({packet}, selection), "Display selection must suppress disabled query primitives without changing the packet.");
		});

		registry.Add("Physics.SceneView.AllDebugKindsAreImmutableAndBounded", [](TestContext& context) {
			auto packet = std::make_shared<Physics::PhysicsDebugPacket>();
			packet->lines = std::make_shared<const std::vector<Physics::PhysicsDebugLine>>(1U);
			packet->triangles = std::make_shared<const std::vector<Physics::PhysicsDebugTriangle>>(1U);
			packet->contacts = std::make_shared<const std::vector<Physics::PhysicsDebugContact>>(1U);
			packet->constraints = std::make_shared<const std::vector<Physics::PhysicsDebugConstraint>>(1U);
			packet->characters = std::make_shared<const std::vector<Physics::PhysicsDebugCharacter>>(1U);
			packet->queries = std::make_shared<const std::vector<Physics::PhysicsDebugQuery>>(1U);
			context.ExpectEqual(static_cast<std::size_t>(6U), Editor::SceneViewOverlay::CountPhysicsDebugPrimitives({packet}, {}), "All six immutable debug categories must remain selectable.");

			Physics::PhysicsSettings invalid = Physics::PhysicsSettings::Defaults();
			invalid.maxDebugPrimitives = Physics::kPhysicsMaximumDebugPrimitives + 1U;
			context.Expect(!invalid.Validate().accepted, "Project settings must reject an unbounded debug primitive budget.");
			invalid = Physics::PhysicsSettings::Defaults();
			invalid.retainedDebugTicks = Physics::kPhysicsMaximumDebugHistoryTicks + 1U;
			context.Expect(!invalid.Validate().accepted, "Project settings must reject unbounded paused-history retention.");
		});

		registry.Add("Physics.SceneView.RendererUnavailableStateKeepsDiagnosticsReadable", [](TestContext& context) {
			Editor::SceneViewOverlay::SceneViewDiagnosticsSnapshot snapshot;
			snapshot.rendererAvailable = false;
			snapshot.disabledOverlayReason = "Renderer unavailable";
			snapshot.physicsDebugPacketCount = 2U;
			const nlohmann::json json = Editor::SceneViewOverlay::SerializeDiagnosticsSnapshot(snapshot);
			context.Expect(!json.value("rendererAvailable", true), "Renderer-unavailable overlay state must serialize without an RHI access.");
			context.ExpectEqual(static_cast<std::size_t>(2U), json.value("physicsDebugPacketCount", static_cast<std::size_t>(0U)), "Diagnostic packet metadata must remain readable while rendering is unavailable.");
			context.Expect(!RenderSystem::Instance().SubmitSceneViewPhysicsDebugPackets({}, {}), "A missing renderer must reject display submission without allocating an RHI or touching physics.");
		});
	}
}
