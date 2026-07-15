// PhysicsSceneViewTests.cpp - unregistered contracts for immutable debug display.

#include "TestHarness.h"

#include "Physics/PhysicsDiagnostics.h"
#include "Physics/PhysicsSettings.h"
#include "Renderer/RenderSystem.h"
#include "Editor/TerraEngine-Editor/Windows/SceneViewOverlayMarkers.h"
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
		std::string ReadEditorSource(const std::filesystem::path& relativePath)
		{
			// The deterministic editor test target does not link the interactive
			// ImGui windows. Reading their maintained sources still guards which
			// window owns the collider controls without requiring a Vulkan editor.
			const std::filesystem::path sourcePath =
				std::filesystem::path(TERRAENGINE_SOURCE_DIR) / relativePath;
			std::ifstream input(sourcePath, std::ios::binary);
			std::ostringstream buffer;
			buffer << input.rdbuf();
			return buffer.str();
		}
	}

	void RegisterPhysicsSceneViewTests(TestRegistry& registry)
	{
		registry.Add("Physics.SceneView.PhysicsBearingObjectsAreNotGenericEmpties", [](TestContext& context) {
			Editor::SceneViewOverlay::MarkerComponentClassification colliderOnly;
			colliderOnly.hasColliderSet = true;
			context.Expect(Editor::SceneViewOverlay::IsPhysicsBearing(colliderOnly), "A collider-only authoring object must be recognized as physics-bearing.");
			context.Expect(
				Editor::SceneViewOverlay::ClassifyMarkerCategory(colliderOnly) == Editor::SceneViewOverlay::MarkerCategory::Physics,
				"Physics-bearing objects without meshes must use the physics marker instead of the generic empty marker.");

			Editor::SceneViewOverlay::MarkerComponentClassification trueEmpty;
			context.Expect(Editor::SceneViewOverlay::IsGenuinelyEmpty(trueEmpty), "An object with only identity and transform state is genuinely empty.");
			context.Expect(
				Editor::SceneViewOverlay::ClassifyMarkerCategory(trueEmpty) == Editor::SceneViewOverlay::MarkerCategory::EmptyObject,
				"Only genuinely empty objects may receive the generic empty marker.");

			Editor::SceneViewOverlay::MarkerComponentClassification scripted;
			scripted.hasOtherMarkerRelevantComponent = true;
			context.Expect(
				Editor::SceneViewOverlay::ClassifyMarkerCategory(scripted) == Editor::SceneViewOverlay::MarkerCategory::Unsupported,
				"An object with another functional component must not be mislabeled as empty.");
		});

		registry.Add("Physics.SceneView.ColliderAuthoringControlsLiveInColliderInspector", [](TestContext& context) {
			const std::string sceneViewSource = ReadEditorSource("Editor/TerraEngine-Editor/Windows/SceneViewWindow.cpp");
			const std::string inspectorSource = ReadEditorSource("Editor/TerraEngine-Editor/Windows/InspectorWindow.cpp");
			context.Expect(
				sceneViewSource.find("PhysicsColliderAuthoring") == std::string::npos,
				"Scene View must not draw an automatically opened collider control panel over its toolbar.");
			context.Expect(
				sceneViewSource.find("Collider preview handles own viewport manipulation until Apply or Cancel") != std::string::npos,
				"An active collider preview must suppress the overlapping object transform gizmo until editing ends.");
			context.Expect(
				sceneViewSource.find("Empty-space and object clicks inside Scene View must not silently abandon") != std::string::npos,
				"Scene View picking must preserve an active collider preview until explicit Apply or Cancel.");
			context.Expect(
				sceneViewSource.find("MarkObjectTransformDirty(*selectedObject)") != std::string::npos,
				"Scene View transform gizmos must invalidate propagated child world transforms after moving a parent.");
			context.Expect(
				sceneViewSource.find("Collider shape refreshed from the restored undo/redo state") != std::string::npos,
				"An active collider preview must follow exact ColliderSet values restored by undo and redo without leaving edit mode.");
			context.Expect(
				inspectorSource.find("Edit Shape in Scene View") != std::string::npos
					&& inspectorSource.find("Half Extents (m)") != std::string::npos
					&& inspectorSource.find("Preview Center (m)") == std::string::npos
					&& inspectorSource.find("Scene View Shape Preview") == std::string::npos
					&& inspectorSource.find("ImGui::BeginDisabled(!editActive)") != std::string::npos
					&& inspectorSource.find("Apply Collider Edit") != std::string::npos,
				"ColliderSet Inspector must show one plainly named shape control set, enabling it only for explicit editing and Apply.");
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
